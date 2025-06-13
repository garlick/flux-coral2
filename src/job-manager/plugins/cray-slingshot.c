/************************************************************\
 * Copyright 2025 Lawrence Livermore National Security, LLC
 * (c.f. AUTHORS, NOTICE.LLNS, COPYING)
 *
 * This file is part of the Flux resource manager framework.
 * For details, see https://github.com/flux-framework.
 *
 * SPDX-License-Identifier: LGPL-3.0
\************************************************************/

/* cray-slingshot.c - post reserved VNIs to job eventlog */

#if HAVE_CONFIG_H
#include "config.h"
#endif
#include <jansson.h>
#include <flux/core.h>
#include <flux/jobtap.h>

#include "src/common/libutil/errprintf.h"
#include "src/common/libutil/idf58.h"
#include "ccan/str/str.h"

#include "vnipool.h"

#define PLUGIN_NAME "cray-slingshot"

struct cray_slingshot {
    struct vnipool *vnipool;
};

static const int default_vnis_per_job = 1;

static void cray_slingshot_destroy (struct cray_slingshot *ctx)
{
    if (ctx) {
        int saved_errno = errno;
        vnipool_destroy (ctx->vnipool);
        free (ctx);
        errno = saved_errno;
    }
}

static struct cray_slingshot *cray_slingshot_create (void)
{
    struct cray_slingshot *ctx;
    if (!(ctx = calloc (1, sizeof (*ctx))))
        return NULL;
    if (!(ctx->vnipool = vnipool_create ()))
        goto error;
    return ctx;
error:
    cray_slingshot_destroy (ctx);
    return NULL;
}

/* plugin.query
 */
static int plugin_query_cb (flux_plugin_t *p, const char *topic, flux_plugin_arg_t *args, void *arg)
{
    struct cray_slingshot *ctx = arg;
    flux_t *h = flux_jobtap_get_flux (p);
    json_t *o;

    if (!(o = vnipool_query (ctx->vnipool))) {
        flux_log_error (h, "%s: error creating query response", PLUGIN_NAME);
        return -1;
    }
    if (flux_plugin_arg_pack (args, FLUX_PLUGIN_ARG_OUT, "{s:O}", "vnipool", o) < 0) {
        flux_log_error (h, "%s: error packing query args", PLUGIN_NAME);
        json_decref (o);
        return -1;
    }
    json_decref (o);
    return 0;
}

/* conf.update
 */
static int conf_update_cb (flux_plugin_t *p, const char *topic, flux_plugin_arg_t *args, void *arg)
{
    struct cray_slingshot *ctx = arg;
    const char *vni_pool = NULL;
    flux_error_t error;

    if (flux_plugin_arg_unpack (args,
                                FLUX_PLUGIN_ARG_IN,
                                "{s:{s?{s?s}}}",
                                "conf",
                                "cray-slingshot",
                                "vni-pool",
                                &vni_pool)
        < 0) {
        errprintf (&error,
                   "%s: error unpacking conf.update arguments: %s",
                   PLUGIN_NAME,
                   flux_plugin_arg_strerror (args));
        goto error;
    }
    if (vnipool_configure (ctx->vnipool, vni_pool, &error) < 0)
        goto error;
    return 0;
error:
    return flux_jobtap_error (p, args, "%s", error.text);
}

/* job.state.run
 */
static int job_state_run_cb (flux_plugin_t *p,
                             const char *topic,
                             flux_plugin_arg_t *args,
                             void *arg)
{
    struct cray_slingshot *ctx = arg;
    flux_error_t error;
    flux_jobid_t id;
    json_t *options = NULL;
    json_t *R;
    int vnicount = default_vnis_per_job;
    int fakedevs = 0;
    json_t *res;

    if (flux_plugin_arg_unpack (args,
                                FLUX_PLUGIN_ARG_IN,
                                "{s:I s:{s:{s:{s?{s?{s?o}}}}} s:o}",
                                "id",
                                &id,
                                "jobspec",
                                "attributes",
                                "system",
                                "shell",
                                "options",
                                "cray-slingshot",
                                &options,
                                "R",
                                &R)
        < 0) {
        errprintf (&error, "error unpacking job info: %s", strerror (errno));
        goto error;
    }
    if (options) {
        // -o cray-slingshot=off
        if (json_is_string (options) && streq (json_string_value (options), "off"))
            return 0;
        // -o cray-slingshot.vnicount=N
        // -o cray-slingshot.fakedevs=N
        json_error_t jerror;
        if (json_unpack_ex (options,
                            &jerror,
                            0,
                            "{s?i s?i !}",
                            "vnicount",
                            &vnicount,
                            "fakedevs",
                            &fakedevs)
            < 0) {
            errprintf (&error, "error parsing cray-slingshot shell options: %s", jerror.text);
            errno = EINVAL;
            goto error;
        }
    }
    if (!(res = vnipool_reserve (ctx->vnipool, id, vnicount, &error)))
        goto error;
    if (flux_jobtap_event_post_pack (p, id, "cray-slingshot", "{s:O}", "reservation", res) < 0) {
        errprintf (&error, "error posting cray-slingshot job event");
        (void)vnipool_release (ctx->vnipool, id, NULL);
        goto error;
    }
    return 0;
error:
    flux_jobtap_raise_exception (p, id, "cray-slingshot", 0, "%s", error.text);
    return 0;
}

/* job.state.cleanup
 */
static int job_state_cleanup_cb (flux_plugin_t *p,
                                 const char *topic,
                                 flux_plugin_arg_t *args,
                                 void *arg)
{
    struct cray_slingshot *ctx = arg;
    flux_t *h = flux_jobtap_get_flux (p);
    flux_jobid_t id;
    flux_error_t error;

    if (flux_plugin_arg_unpack (args, FLUX_PLUGIN_ARG_IN, "{s:I}", "id", &id) < 0) {
        flux_log_error (h, "flux_plugin_arg_unpack");
        return -1;
    }
    if (vnipool_release (ctx->vnipool, id, &error) < 0 && errno != ENOENT) {
        flux_log (h,
                  LOG_ERR,
                  "%s: VNI release error for %s: %s",
                  PLUGIN_NAME,
                  idf58 (id),
                  error.text);
        return -1;
    }
    return 0;
}

/* jobtap plugin main entry point
 */
int flux_plugin_init (flux_plugin_t *p)
{
    struct cray_slingshot *ctx;

    if (!(ctx = cray_slingshot_create ()) || flux_plugin_set_name (p, PLUGIN_NAME) < 0
        || flux_plugin_add_handler (p, "job.state.run", job_state_run_cb, ctx) < 0
        || flux_plugin_add_handler (p, "job.state.cleanup", job_state_cleanup_cb, ctx) < 0
        || flux_plugin_add_handler (p, "conf.update", conf_update_cb, ctx) < 0
        || flux_plugin_add_handler (p, "plugin.query", plugin_query_cb, ctx) < 0
        || flux_plugin_aux_set (p, NULL, ctx, (flux_free_f)cray_slingshot_destroy)) {
        cray_slingshot_destroy (ctx);
        return -1;
    }
    return 0;
}

// vi:ts=4 sw=4 expandtab
