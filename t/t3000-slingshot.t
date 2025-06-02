#!/bin/sh

test_description='Test cray-slingshot support'

SHELL_PLUGINPATH=${FLUX_BUILD_DIR}/src/shell/plugins/.libs
JOBTAP_PLUGINPATH=${FLUX_BUILD_DIR}/src/job-manager/plugins/.libs
SSUTIL=${FLUX_BUILD_DIR}/src/cmd/flux-slingshot

# make sure the test environment is clean starting out
unset SLINGSHOT_VNIS
unset SLINGSHOT_DEVICES
unset SLINGSHOT_SVC_IDS
unset SLINGSHOT_TCS

. $(dirname $0)/sharness.sh

test_under_flux 1

test_expect_success 'load jobtap plugin' '
	flux jobtap load $JOBTAP_PLUGINPATH/cray-slingshot.so
'
test_expect_success 'a job cannot run because there is no vni pool' '
	test_must_fail flux run true 2>nopool.err
'
test_expect_success 'a fatal exception was raised with useful explanation' '
	grep "severity=0 failed to reserve 1 VNI (0 available)" nopool.err
'
test_expect_success 'a job can run with -o cray-slingshot=off' '
	flux run -o cray-slingshot=off true
'
test_expect_success 'create shell initrc that loads shell plugin' "
	cat >userrc.lua <<-EOT
	plugin.load { file = \"$SHELL_PLUGINPATH/cray-slingshot.so\", conf = { } }
	EOT
"
test_expect_success 'a job can still run with -o cray-slingshot=off' '
	flux run -o verbose=2 --env=-* -o pmi=none -o userrc=$(pwd)/userrc.lua \
	    -o cray-slingshot=off printenv >off.out 2>off.err
'
test_expect_success 'the shell plugin reported disabled' '
	grep "cray-slingshot: disabled" off.err
'
test_expect_success 'no SLINGSHOT_ environment variables were set' '
	test_must_fail grep SLINGSHOT_ off.out
'
test_expect_success 'SLINGSHOT_VNI can be set manually with -o cray-slingshot=off' '
	flux run --env=-* -o pmi=none -o userrc=$(pwd)/userrc.lua \
	    -o cray-slingshot=off --env=SLINGSHOT_VNI=999 printenv SLINGSHOT_VNI
'
test_expect_success 'a VNI pool of 1024-65535 can be live-configured' '
	flux config load <<-EOT
	[cray-slingshot]
	vni-pool = "1024-65535"
	EOT
'
test_expect_success 'a job can run without shell plugin and get a reservation' '
	flux run $SSUTIL jobinfo | jq -e ".vnis[0]"
'
test_expect_success 'a job can run with fake devices' '
	flux run --env=-* -o pmi=none -o userrc=$(pwd)/userrc.lua \
	    -o cray-slingshot.fakedevs=4  printenv >pool.out
'
test_expect_success 'SLINGSHOT_VNIS is set to match the reservation' '
	$SSUTIL jobinfo --jobid=$(flux job last) | jq -e ".vnis[0]" >res.out &&
	grep SLINGSHOT_VNIS=$(<res.out) pool.out
'
test_expect_success 'SLINGSHOT_DEVICES is set to expected (fake) value' '
	grep SLINGSHOT_DEVICES=cxi0,cxi1,cxi2,cxi3 pool.out
'
test_expect_success 'SLINGSHOT_SVC_IDS is set to expected (fake) value' '
	grep SLINGSHOT_SVC_IDS=10,11,12,13 pool.out
'
test_expect_success 'a job can run -o cray-slingshot.vnicount=4' '
	flux run --env=-* -o pmi=none -o userrc=$(pwd)/userrc.lua \
	    -o cray-slingshot.vnicount=4 -o cray-slingshot.fakedevs=1 printenv >pool4.out
'
test_expect_success 'SLINGSHOT_VNIS has multiple values' '
	grep SLINGSHOT_VNIS= pool4.out | grep ","
'
test_expect_success 'flux-slingshot prolog --dry-run works' '
	flux run --env=-* -o pmi=none -o userrc=$(pwd)/userrc.lua \
	    -o cray-slingshot.vnicount=3 -o cray-slingshot.fakedevs=4 \
	    $SSUTIL prolog --dry-run --userid=$(id -u)
'
test_expect_success 'flux-slingshot epilog --dry-run works' '
	flux run --env=-* -o pmi=none -o userrc=$(pwd)/userrc.lua \
	    -o cray-slingshot.vnicount=2 -o cray-slingshot.fakedevs=4 \
	    $SSUTIL epilog --dry-run --userid=$(id -u)
'
test_expect_success 'reconfigure the VNI pool with one VNI' '
	flux config load <<-EOT
	[cray-slingshot]
	vni-pool = "20"
	EOT
'
test_expect_success 'a job requesting one VNI can run' '
	flux run --env=-* -o pmi=none -o userrc=$(pwd)/userrc.lua \
	    -o cray-slingshot.vnicount=1 -o cray-slingshot.fakedevs=1 printenv SLINGSHOT_VNIS
'
test_expect_success 'another job requesting one VNI can run' '
	flux run --env=-* -o pmi=none -o userrc=$(pwd)/userrc.lua \
	    -o cray-slingshot.vnicount=1 -o cray-slingshot.fakedevs=1 printenv SLINGSHOT_VNIS
'
test_expect_success 'a job requesting two VNIs fails' '
	test_must_fail flux run --env=-* -o pmi=none -o userrc=$(pwd)/userrc.lua \
	    -o cray-slingshot.vnicount=2 -o cray-slingshot.fakedevs=1 printenv SLINGSHOT_VNIS 2>novni.err
'
test_expect_success 'a fatal exception was raised with useful explanation' '
	grep "severity=0 failed to reserve 2 VNIs (1 available)" novni.err
'
test_expect_success 'configuring a vni-pool that is an invalid idset fails' '
	test_must_fail flux config load <<-EOT
	[cray-slingshot]
	vni-pool = "-1"
	EOT
'
test_expect_success 'configuring a vni-pool that is out of range fails' '
	test_must_fail flux config load <<-EOT
	[cray-slingshot]
	vni-pool = "1024-65536"
	EOT
'
test_expect_success 'flux jobtap query cray-slingshot.so works' '
	flux jobtap query cray-slingshot.so | jq
'
test_expect_success 'unload jobtap plugin' '
	flux jobtap remove cray-slingshot.so
'
test_expect_success 'a job can run' '
	flux run -o verbose=2 --env=-* -o pmi=none -o userrc=$(pwd)/userrc.lua printenv >notap.out 2>notap.err
'
test_expect_success 'shell plugin says it is using the default CXI service' '
	grep "cray-slingshot: using default CXI service" notap.err
'
test_expect_success 'no SLINGSHOT_ environment variables were set' '
	test_must_fail grep SLINGSHOT_ notap.out
'
test_expect_success 'flux-slingshot prolog does nothing with no reservation' '
	flux run --env=-* -o pmi=none -o userrc=$(pwd)/userrc.lua \
	    $SSUTIL prolog --dry-run --userid=$(id -u) 2>prolog2.err &&
	grep "no cray-slingshot reservation" prolog2.err
'
test_expect_success 'flux-slingshot epilog does nothing with no reservation' '
	flux run --env=-* -o pmi=none -o userrc=$(pwd)/userrc.lua \
	    $SSUTIL epilog --dry-run --userid=$(id -u) 2>epilog2.err &&
	grep "no cray-slingshot reservation" epilog2.err
'
test_expect_success 'create batch script for inherited tests' "
	cat >batch.sh <<-EOT &&
	#!/bin/sh -v
	flux run -o verbose=2 --env=-* -o pmi=none -o userrc=$(pwd)/userrc.lua \
	    \\\$EXTRA_RUN_ARGS printenv >\\\$TEST_NAME.out 2>\\\$TEST_NAME.err
	EOT
	chmod +x batch.sh
"
test_expect_success 'inherited mode is selected when broker has environment' '
	flux batch --flags=waitable -N1 --env=TEST_NAME=inherit \
	    --env=SLINGSHOT_VNIS=42 \
	    --env=SLINGSHOT_DEVICES=cxi0,cxi1 \
	    --env=SLINGSHOT_SVC_IDS=10,11 \
	    --env=SLINGSHOT_TCS=0x0a \
	    ./batch.sh &&
	flux job wait --all --verbose
'
test_expect_success 'shell plugin says it used inherited environment' '
	grep "cray-slingshot: using inherited CXI service" inherit.err
'
test_expect_success 'all the SLINGSHOT_ vars were inherited' '
	grep SLINGSHOT_VNIS=42 inherit.out &&
	grep SLINGSHOT_DEVICES=cxi0,cxi1 inherit.out &&
	grep SLINGSHOT_SVC_IDS=10,11 inherit.out &&
	grep SLINGSHOT_TCS=0x0a inherit.out
'
test_expect_success 'flux-slingshot fails with bad option' '
	test_must_fail $SSUTIL --bad-option
'
test_expect_success 'flux-slingshot fails with bad subcommand' '
	test_must_fail $SSUTIL bad-subcommand
'
test_expect_success 'flux-slingshot prolog fails with bad option' '
	test_must_fail $SSUTIL prolog --bad-option
'
test_expect_success 'flux-slingshot epilog fails with bad option' '
	test_must_fail $SSUTIL epilog --bad-option
'
test_expect_success 'flux-slingshot prolog fails with no jobid' '
	test_must_fail sh -c "unset FLUX_JOB_ID; $SSUTIL prolog --userid=$(id -u)" 2>pro_nojob.err &&
	grep "FLUX_JOB_ID is not set" pro_nojob.err
'
test_expect_success 'flux-slingshot epilog fails with no jobid' '
	test_must_fail sh -c "unset FLUX_JOB_ID; $SSUTIL epilog --userid=$(id -u)" 2>epi_nojob.err &&
	grep "FLUX_JOB_ID is not set" epi_nojob.err
'
test_expect_success 'flux-slingshot prolog fails with no userid' '
	test_must_fail sh -c "unset FLUX_JOB_USERID; $SSUTIL prolog --jobid=fuzzybunny" 2>pro_nojob.err &&
	grep "FLUX_JOB_USERID is not set" pro_nojob.err
'
test_expect_success 'flux-slingshot prolog fails with no eventlog outside of job' '
	test_must_fail $SSUTIL prolog --jobid=fuzzybunny --userid=$(id -u) --dry-run 2>pro_noevent.err &&
	grep "error reading job eventlog" pro_noevent.err
'
test_expect_success 'flux-slingshot epilog fails with no eventlog outside of job' '
	test_must_fail $SSUTIL epilog --jobid=fuzzybunny --userid=$(id -u) --dry-run 2>epi_noevent.err &&
	grep "error reading job eventlog" epi_noevent.err
'
test_expect_success 'flux-slingshot list works' '
	$SSUTIL list
'
test_expect_success 'flux-slingshot list --no-header works' '
	$SSUTIL list --no-header >list.out &&
	test_must_fail grep Name list.out
'

test_done
