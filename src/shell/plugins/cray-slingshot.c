/************************************************************\
 * Copyright 2025 Lawrence Livermore National Security, LLC
 * (c.f. AUTHORS, NOTICE.LLNS, COPYING)
 *
 * This file is part of the Flux resource manager framework.
 * For details, see https://github.com/flux-framework.
 *
 * SPDX-License-Identifier: LGPL-3.0
\************************************************************/

/* cray-slingshot.c - shell plugin for Cassini NIC support
 *
 * There are three modes of operation:
 *
 * 1) in sub-instances, the SLINGSHOT_* environment is inherited
 * from the broker environment.  This should work for any instance
 * level, and when Flux is launched by a foreign resource manager.
 *
 * 2) in a system instance, a VNI reservation is created by a
 * jobtap plugin, and CXI services are created by a prolog script.
 * This plugin correlates the reservation with matching CXI services
 * and sets the SLINGSHOT_* environment accordingly.  Cleanup takes
 * place in eplilog/housekeeping.
 *
 * 3) if neither of the above are available, the SLINGSHOT_*
 * environment is cleared so that libfabric-enabled jobs will try
 * to use the default CXI service.
 *
 * If this plugin is getting in the way for some reason, it can
 * be completely disabled with -o cray-slingshot=off.
 */

#define FLUX_SHELL_PLUGIN_NAME "cray-slingshot"

#if HAVE_CONFIG_H
#include "config.h"
#endif
#include <string.h>
#include <math.h>
#include <jansson.h>
#include <argz.h>
#include <flux/shell.h>
#include <flux/idset.h>
#ifdef HAVE_LIBCXI_LIBCXI_H
#include <libcxi/libcxi.h>
#endif

#include "src/common/libutil/eventlog.h"
#include "ccan/str/str.h"

struct cray_slingshot_options {
    bool off;
    int vnicount;
    int fakedevs;
};

struct cray_slingshot {
    flux_jobid_t jobid;
    flux_shell_t *shell;
    flux_future_t *f_event;
    flux_future_t *f_getenv;
    struct cray_slingshot_options opt;
};

/* Timeout (seconds) waiting for job eventlog events.  This applies to each
 * event, not the total time blocked on the eventlog.
 */
static const double eventlog_timeout = 30.;

/* Convert json string array to a single string, e.g.
 * ["cxi0","cxi1"] => "cxi0,cxi1"
 */
static char *stringify_json_string_array (json_t *list)
{
    size_t index;
    json_t *entry;
    size_t argz_len;
    char *argz = NULL;

    json_array_foreach (list, index, entry) {
        if (argz_add (&argz, &argz_len, json_string_value (entry)) != 0) {
            free (argz);
            return NULL;
        }
    }
    argz_stringify (argz, argz_len, ',');
    return argz;
}

/* Set an environment variable to a list of integers derived from a json string array
 */
static int setenv_json_int_array (flux_shell_t *shell, const char *name, json_t *list)
{
    char *s;
    if (!(s = json_dumps (list, JSON_EMBED | JSON_COMPACT))  // EMBED strips enclosing braces
        || flux_shell_setenvf (shell, 1, name, "%s", s) < 0) {
        shell_log_error ("setenv %s failed", name);
        free (s);
        return -1;
    }
    free (s);
    return 0;
}

/* Set an environment variable to a list of names derived from a json string array.
 */
static int setenv_json_string_array (flux_shell_t *shell, const char *name, json_t *list)
{
    char *s;
    if (!(s = stringify_json_string_array (list))
        || flux_shell_setenvf (shell, 1, name, "%s", s) < 0) {
        shell_log_error ("setenv %s failed", name);
        free (s);
        return -1;
    }
    free (s);
    return 0;
}

static int array_append_device (json_t *array, unsigned int dev_id)
{
    char buf[64];
    json_t *o;
    snprintf (buf, sizeof (buf), "cxi%u", dev_id);
    if (!(o = json_string (buf)) || json_array_append_new (array, o) < 0) {
        json_decref (o);
        shell_log_error ("out of memory building device list");
        return -1;
    }
    return 0;
}

static int array_append_int (json_t *array, int val)
{
    json_t *o;
    if (!(o = json_integer (val)) || json_array_append_new (array, o) < 0) {
        json_decref (o);
        shell_log_error ("out of memory building int array");
        return -1;
    }
    return 0;
}

/* Add the specified number of fake device names and CXI service ids to
 * the devs and svcs arrays.
 */
static int add_fake_devices (json_t *devs, json_t *svcs, int count)
{
    for (int i = 0; i < count; i++) {
        if (array_append_device (devs, i) < 0 || array_append_int (svcs, i + 10) < 0)
            return -1;
    }
    return 0;
}

#ifdef HAVE_CXI
/* If a CXI service is enabled, not a system service, and lists
 * the same VNIs as the provided JSON array, return true.
 * Assumption: VNIs are in the same order in both arrays,
 * and we needn't bother checking uid/gid restrictions.
 */
static bool match_cxi_service (struct cxi_svc_desc *desc, json_t *vnis)
{
    if (!desc->enable || desc->is_system_svc)
        return false;
    if (desc->num_vld_vnis != json_array_size (vnis))
        return false;
    for (int i = 0; i < desc->num_vld_vnis; i++) {
        json_t *entry = json_array_get (vnis, i);
        if (desc->vnis[i] != json_integer_value (entry))
            return false;
    }
    return true;
}

/* Find the first CXI service on the specified interface that has
 * VNIs matching the vnis array.  When found, append the service
 * ID to the svcs array.
 */
static int append_cxi_service_match (json_t *svcs, uint32_t dev_id, json_t *vnis)
{
    int e;
    struct cxil_dev *dev;
    struct cxil_svc_list *svc_list = NULL;
    int match = -1;
    int rc = -1;

    if ((e = cxil_open_device (dev_id, &dev)) < 0) {
        shell_log_errn (-e, "cxil_open_device cxi%u", dev_id);
        return -1;
    }
    if ((e = cxil_get_svc_list (dev, &svc_list)) < 0) {
        shell_log_errn (-e, "cxil_get_svc_list cxi%u", dev_id);
        goto done;
    }
    for (int i = 0; i < svc_list->count; i++) {
        if (match_cxi_service (&svc_list->descs[i], vnis)) {
            match = svc_list->descs[i].svc_id;
            break;
        }
    }
    if (match == -1) {
        shell_log_error ("cxi%u: CXI service for reserved VNIs not found", dev_id);
        goto done;
    }
    if (array_append_int (svcs, match) < 0)
        goto done;
    rc = 0;
done:
    cxil_free_svc_list (svc_list);
    cxil_close_device (dev);
    return rc;
}
#endif

/* Find real Cassini devices and add their names (e.g. "cxi0")
 * to the devs array and matching CXI service IDs to the svcs array.
 */
static int add_real_devices (json_t *devs, json_t *svcs, json_t *vnis)
{
#ifdef HAVE_CXI
    struct cxil_device_list *dev_list;
    int e;
    if ((e = cxil_get_device_list (&dev_list)) < 0) {
        shell_log_errn (-e, "cxil_get_device_list");
        return -1;
    }
    for (int i = 0; i < dev_list->count; i++) {
        if (array_append_device (devs, dev_list->info[i].dev_id) < 0) {
            cxil_free_device_list (dev_list);
            return -1;
        }
        if (append_cxi_service_match (svcs, dev_list->info[i].dev_id, vnis) < 0) {
            cxil_free_device_list (dev_list);
            return -1;
        }
    }
    cxil_free_device_list (dev_list);
#endif
    return 0;
}

/* Read the cray-slingshot event from the job eventlog and return the
 * reservation object from the event context, or return NULL if not found.
 * Give up searching if 'start' or a fatal exception is posted.
 * The caller must free the returned object.
 *
 * N.B. this synchronously waits for job events to be posted, with timeouts.
 * In theory it could block for several 'eventlog_timeout' periods.
 * Note that cray-pals is doing the same thing for the 'cray-pmi-bootstrap'
 * event so KVS follower caching should be helpful.
 */
static json_t *reservation_lookup (struct cray_slingshot *ctx)
{
    bool done = false;
    json_t *res = NULL;

    while (!done) {
        const char *s;
        json_t *entry;
        const char *name;
        json_t *context;
        json_error_t jerror;

        if (flux_future_wait_for (ctx->f_event, eventlog_timeout) < 0
            || flux_job_event_watch_get (ctx->f_event, &s) < 0) {
            shell_log_error ("error reading job eventlog: %s",
                             future_strerror (ctx->f_event, errno));
            return NULL;
        }
        if (!(entry = eventlog_entry_decode (s))
            || eventlog_entry_parse (entry, NULL, &name, &context) < 0) {
            shell_log_errno ("error parsing eventlog entry");
            json_decref (entry);
            return NULL;
        }
        if (streq (name, "start")) {
            done = true;
        } else if (streq (name, "exception")) {
            int severity;
            if (json_unpack (context, "{s:i}", "severity", &severity) == 0 && severity == 0)
                done = true;
        } else if (streq (name, "cray-slingshot")) {
            if (json_unpack_ex (context, &jerror, 0, "{s:o}", "reservation", &res) < 0) {
                shell_log_error ("error parsing cray-slingshot event context: %s", jerror.text);
                return NULL;
            }
            json_incref (res);
            done = true;
        }
        json_decref (entry);
        flux_future_reset (ctx->f_event);
    }
    return res;
}

/* Read the cray-slingshot event from eventlog to find reserved VNIs,
 * then look for corresponding CXI services placed there by the prolog.
 * The eventlog watch request was set in motion by the shell.init callback.
 * Return 0 for success, -1 for fatal error, or 1 on non-fatal error.
 */
static int cray_slingshot_reserved (struct cray_slingshot *ctx)
{
    json_t *res;
    json_t *vnis;
    json_t *devices = NULL;
    json_t *cxi_svc = NULL;
    int rc = -1;

    if (!(res = reservation_lookup (ctx)))
        return 1;
    if (json_unpack (res, "{s:o}", "vnis", &vnis) < 0 || json_array_size (vnis) == 0) {
        shell_log_error ("error parsing cray-slingshot reservation");
        goto done;
    }
    if (!(devices = json_array ()) || !(cxi_svc = json_array ())) {
        shell_log_error ("out of memory building device/service lists");
        goto done;
    }
    if (ctx->opt.fakedevs > 0) {
        if (add_fake_devices (devices, cxi_svc, ctx->opt.fakedevs) < 0)
            goto done;
    } else {
        if (add_real_devices (devices, cxi_svc, vnis) < 0)
            goto done;
    }
    if (json_array_size (devices) == 0) {
        shell_log_error ("no slingshot devices were found");
        goto done;
    }
    if (setenv_json_int_array (ctx->shell, "SLINGSHOT_VNIS", vnis) < 0
        || setenv_json_string_array (ctx->shell, "SLINGSHOT_DEVICES", devices) < 0
        || setenv_json_int_array (ctx->shell, "SLINGSHOT_SVC_IDS", cxi_svc) < 0)
        goto done;
    // eh what about SLINGSHOT_TCS?
    shell_debug ("created CXI service for reserved VNIs");
    rc = 0;
done:
    json_decref (cxi_svc);
    json_decref (devices);
    json_decref (res);
    return rc;
}

/* Pass local broker's SLINGSHOT environment variables through.
 * The getenv request was set in motion by the shell.init callback.
 * Return 0 for success, -1 for fatal error, or 1 on non-fatal error.
 */
static int cray_slingshot_inherit (struct cray_slingshot *ctx)
{
    const char *vnis = NULL;
    const char *devices = NULL;
    const char *svc_ids = NULL;
    const char *tcs = NULL;

    if (flux_rpc_get_unpack (ctx->f_getenv,
                             "{s:{s?s s?s s?s s?s}}",
                             "env",
                             "SLINGSHOT_VNIS",
                             &vnis,
                             "SLINGSHOT_DEVICES",
                             &devices,
                             "SLINGSHOT_SVC_IDS",
                             &svc_ids,
                             "SLINGSHOT_TCS",
                             &tcs)
        < 0) {
        if (errno != EPERM && errno != ENOSYS) {
            shell_log_error ("broker.getenv: %s", future_strerror (ctx->f_getenv, errno));
            return -1;
        }
    }
    if (!vnis || !devices || !svc_ids)
        return 1;
    if (flux_shell_setenvf (ctx->shell, 1, "SLINGSHOT_VNIS", "%s", vnis) < 0
        || flux_shell_setenvf (ctx->shell, 1, "SLINGSHOT_DEVICES", "%s", devices) < 0
        || flux_shell_setenvf (ctx->shell, 1, "SLINGSHOT_SVC_IDS", "%s", svc_ids) < 0
        || (tcs && flux_shell_setenvf (ctx->shell, 1, "SLINGSHOT_TCS", "%s", tcs) < 0)) {
        shell_log_error ("setenv SLINGSHOT_* failed");
        return -1;
    }
    shell_debug ("using inherited CXI service");
    return 0;
}

/* shell.post-init (after init barrier, before task launch)
 *
 * Try each method to get SLINGSHOT environment set up until one works
 * Methods return 0 if successful, -1 on fatal error, or 1 on non-fatal error.
 */
static int shell_post_init_cb (flux_plugin_t *p,
                               const char *topic,
                               flux_plugin_arg_t *args,
                               void *data)
{
    struct cray_slingshot *ctx = data;
    int rc;

    if ((rc = cray_slingshot_inherit (ctx)) <= 0)
        return rc;
    if ((rc = cray_slingshot_reserved (ctx)) <= 0)
        return rc;
    shell_debug ("using default CXI service");
    return 0;
}

/* shell.init (after broker connect, before init barrier)
 *
 * Set in motion two possible options that can run in parallel
 * with the shell barrier:
 * - obtain allocated VNIs from the eventlog
 * - inherit VNIs/CXI services from the local broker
 */
static int shell_init_cb (flux_plugin_t *p, const char *topic, flux_plugin_arg_t *args, void *data)
{
    struct cray_slingshot *ctx = data;
    flux_t *h = flux_shell_get_flux (ctx->shell);

    if (!(ctx->f_event = flux_job_event_watch (h, ctx->jobid, "eventlog", 0)))
        shell_die (1, "error sending eventlog watch request");

    if (!(ctx->f_getenv = flux_rpc_pack (h,
                                         "broker.getenv",
                                         FLUX_NODEID_ANY,
                                         0,
                                         "{s:[ssss]}",
                                         "names",
                                         "SLINGSHOT_VNIS",
                                         "SLINGSHOT_DEVICES",
                                         "SLINGSHOT_SVC_IDS",
                                         "SLINGSHOT_TCS")))
        shell_die (1, "error sending broker.getenv request");
    return 0;
}

static void cray_slingshot_destroy (struct cray_slingshot *ctx)
{
    if (ctx) {
        int saved_errno = errno;
        flux_future_destroy (ctx->f_event);
        flux_future_destroy (ctx->f_getenv);
        free (ctx);
        errno = saved_errno;
    }
}

static struct cray_slingshot *cray_slingshot_create (flux_shell_t *shell)
{
    struct cray_slingshot *ctx;

    if (!(ctx = calloc (1, sizeof (*ctx))))
        return NULL;
    if (flux_shell_info_unpack (shell, "{s:I}", "jobid", &ctx->jobid) < 0) {
        shell_log_error ("Error unpacking jobid from shell info");
        goto error;
    }
    ctx->shell = shell;
    return ctx;
error:
    cray_slingshot_destroy (ctx);
    return NULL;
}

/* Parse plugin options
 *   -o cray-slingshot=off
 *   -o cray-slingshot.vnicount=N
 *   -o cray-slingshot.fakedevs=N
 */
static int cray_slingshot_parse_args (struct cray_slingshot *ctx,
                                      struct cray_slingshot_options *optp)
{
    json_t *options = NULL;
    struct cray_slingshot_options opt;

    memset (&opt, 0, sizeof (opt));
    if (flux_shell_getopt_unpack (ctx->shell, "cray-slingshot", "o", &options) < 0) {
        shell_log_error ("-o cray-slingshot: error unpacking options");
        return -1;
    }
    if (options) {
        if (json_is_string (options)) {
            if (streq (json_string_value (options), "off"))
                opt.off = true;
            else {
                shell_log_error ("-o cray-slingshot: invalid option");
                goto error;
            }
        } else {
            /* The vnicount is only consumed by the jobtap plugin,
             * so just validate it here in case it's wrong and the
             * jobtap plugin isn't loaded to catch it.
             */
            json_error_t jerror;
            if (json_unpack_ex (options,
                                &jerror,
                                0,
                                "{s?i s?i !}",
                                "vnicount",
                                &opt.vnicount,
                                "fakedevs",
                                &opt.fakedevs)
                < 0) {
                shell_log_error ("-o cray-slingshot: %s", jerror.text);
                goto error;
            }
        }
    }
    *optp = opt;
    return 0;
error:
    errno = EINVAL;
    return -1;
}

int flux_plugin_init (flux_plugin_t *p)
{
    flux_shell_t *shell;
    struct cray_slingshot *ctx;

    if (!(shell = flux_plugin_get_shell (p))
        || flux_plugin_set_name (p, FLUX_SHELL_PLUGIN_NAME) < 0)
        return -1;

    if (!(ctx = cray_slingshot_create (shell))
        || flux_plugin_aux_set (p, NULL, ctx, (flux_free_f)cray_slingshot_destroy) < 0) {
        cray_slingshot_destroy (ctx);
        return -1;
    }

    if (cray_slingshot_parse_args (ctx, &ctx->opt) < 0)
        return -1;
    if (ctx->opt.off) {
        shell_debug ("disabled");
        return 0;
    }
    shell_debug ("enabled (version %s)", PACKAGE_VERSION);

    // start with a clean slingshot environment
    flux_shell_unsetenv (ctx->shell, "SLINGSHOT_VNIS");
    flux_shell_unsetenv (ctx->shell, "SLINGSHOT_DEVICES");
    flux_shell_unsetenv (ctx->shell, "SLINGSHOT_SVC_IDS");
    flux_shell_unsetenv (ctx->shell, "SLINGSHOT_TCS");

    if (flux_plugin_add_handler (p, "shell.init", shell_init_cb, ctx) < 0
        || flux_plugin_add_handler (p, "shell.post-init", shell_post_init_cb, ctx) < 0)
        return -1;

    return 0;
}

// vi:ts=4 sw=4 expandtab
