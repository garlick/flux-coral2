#!/bin/sh

test_description='Test dws-jobtap plugin with fake coral2_dws.py'

. $(dirname $0)/sharness.sh

test_under_flux 2 job

flux setattr log-stderr-level 1

PLUGINPATH=${FLUX_BUILD_DIR}/src/job-manager/plugins/.libs
DWS_SCRIPT=${SHARNESS_TEST_SRCDIR}/dws-dependencies/coral2_dws.py
DEPENDENCY_NAME="dws-create"
PROLOG_NAME="dws-setup"
EPILOG_NAME="dws-epilog"

test_expect_success 'job-manager: load dws-jobtap plugin' '
	flux jobtap load ${PLUGINPATH}/dws-jobtap.so
'

test_expect_success 'job-manager: dws jobtap plugin works when coral2_dws is absent' '
	jobid=$(flux submit --setattr=system.dw="foo" hostname) &&
	flux job wait-event -vt 5 -m description=${DEPENDENCY_NAME} \
		${jobid} dependency-add &&
	flux job wait-event -vt 1 ${jobid} exception -m note="dws.create RPC could not be sent"
	flux job wait-event -vt 1 ${jobid} clean
'

test_expect_success 'job-manager: dws jobtap plugin works when RPCs succeed' '
	create_jobid=$(flux submit -t 8 --output=dws1.out --error=dws1.out \
		flux python ${DWS_SCRIPT}) &&
	flux job wait-event -vt 15 -p guest.exec.eventlog ${create_jobid} shell.start &&
	jobid=$(flux submit --setattr=system.dw="foo" hostname) &&
	flux job wait-event -vt 5 -m description=${DEPENDENCY_NAME} \
		${jobid} dependency-add &&
	flux job wait-event -t 5 -m description=${DEPENDENCY_NAME} \
		${jobid} dependency-remove &&
	flux job wait-event -vt 5 -m description=${PROLOG_NAME} \
		${jobid} prolog-start &&
	flux job wait-event -vt 5 -m description=${PROLOG_NAME} \
		${jobid} prolog-finish &&
	flux job wait-event -vt 5 -m description=${EPILOG_NAME} \
		${jobid} epilog-start &&
	flux job wait-event -vt 5 -m description=${EPILOG_NAME} \
		${jobid} epilog-finish &&
	flux job wait-event -vt 5 ${jobid} clean &&
	flux job wait-event -vt 5 ${create_jobid} clean
'

test_expect_success 'job-manager: dws jobtap plugin works when creation RPC fails' '
	create_jobid=$(flux submit -t 8 --output=dws2.out --error=dws2.out \
		flux python ${DWS_SCRIPT} --create-fail) &&
	flux job wait-event -vt 15 -p guest.exec.eventlog ${create_jobid} shell.start &&
	jobid=$(flux submit --setattr=system.dw="foo" hostname) &&
	flux job wait-event -vt 5 -m description=${DEPENDENCY_NAME} \
		${jobid} dependency-add &&
	flux job wait-event -vt 1 ${jobid} exception &&
	flux job wait-event -vt 5 ${jobid} clean &&
	flux job wait-event -vt 5 ${create_jobid} clean
'

test_expect_success 'job-manager: dws jobtap plugin works when setup RPC fails' '
	create_jobid=$(flux submit -t 8 --output=dws3.out --error=dws3.out \
		flux python ${DWS_SCRIPT} --setup-fail) &&
	flux job wait-event -vt 15 -p guest.exec.eventlog ${create_jobid} shell.start &&
	jobid=$(flux submit --setattr=system.dw="foo" hostname) &&
	flux job wait-event -vt 5 -m description=${PROLOG_NAME} \
		${jobid} prolog-start &&
	flux job wait-event -vt 5 -m description=${PROLOG_NAME} -m status=1 \
		${jobid} prolog-finish &&
	flux job wait-event -vt 1 ${jobid} exception &&
	flux job wait-event -vt 5 ${jobid} clean &&
	flux job wait-event -vt 5 ${create_jobid} clean
'

test_expect_success 'job-manager: dws jobtap plugin works when post_run RPC fails' '
	create_jobid=$(flux submit -t 8 --output=dws3.out --error=dws3.out \
		flux python ${DWS_SCRIPT} --post-run-fail) &&
	flux job wait-event -vt 15 -p guest.exec.eventlog ${create_jobid} shell.start &&
	jobid=$(flux submit --setattr=system.dw="foo" hostname) &&
	flux job wait-event -vt 5 -m description=${EPILOG_NAME} \
		${jobid} epilog-start &&
	flux job wait-event -vt 5 -m description=${EPILOG_NAME} -m status=1 \
		${jobid} epilog-finish &&
	flux job wait-event -vt 1 ${jobid} exception &&
	flux job wait-event -vt 5 ${jobid} clean &&
	flux job wait-event -vt 5 ${create_jobid} clean
'

test_expect_success 'job-manager: dws jobtap plugin works when job hits exception during prolog' '
	create_jobid=$(flux submit -t 8 --output=dws4.out --error=dws4.out \
		flux python ${DWS_SCRIPT} --setup-hang) &&
	flux job wait-event -vt 15 -p guest.exec.eventlog ${create_jobid} shell.start &&
	jobid=$(flux submit --setattr=system.dw="foo" hostname) &&
	flux job wait-event -vt 5 -m description=${PROLOG_NAME} \
		${jobid} prolog-start &&
	flux job cancel $jobid
	flux job wait-event -vt 1 ${jobid} exception &&
	flux job wait-event -vt 5 -m description=${PROLOG_NAME} -m status=1 \
		${jobid} prolog-finish &&
	flux job wait-event -vt 5 -m description=${EPILOG_NAME} \
		${jobid} epilog-start &&
	flux job wait-event -vt 5 -m description=${EPILOG_NAME} \
		${jobid} epilog-finish &&
	flux job wait-event -vt 5 ${jobid} clean &&
	flux job wait-event -vt 5 ${create_jobid} clean
'

test_done
