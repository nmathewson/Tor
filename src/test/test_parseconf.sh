#!/bin/sh
# Copyright 2019, The Tor Project, Inc.
# See LICENSE for licensing information

# Integration test script for verifying that Tor configurations are parsed as
# we expect.

# This script looks for its test cases as individual directories in
# src/test/conf_examples/.  Each test may have these files:
#
# torrc -- Required. This file is passed to Tor on the command line with
#      the "-f" flag.
#
# torrc.defaults -- Optional. If present, it is passed to Tor on the command
#      line with the --defaults-torrc option. If this file is absent, an empty
#      file is passed instead to prevent Tor from reading the system defaults.
#
# cmdline -- Optional. If present, it contains command-line arguments that
#      will be passed to Tor.
#
# expected -- If this file is present, then it should be the expected result of
#      "--dump-config short" for this test case.  Either "expected" or "error"
#      must be present, or the test will not run.
#
# error -- If this file is present, then it contains a regex that must be
#      matched by some line in the output of "--verify-config", which must
#      fail.  Either "expected" or "error" must be present, or the test will
#      not run.

umask 077
set -e
die() { echo "$1" >&2 ; exit 5; }

# find the tor binary
if [ $# -ge 1 ]; then
  TOR_BINARY="${1}"
  shift
else
  TOR_BINARY="${TESTING_TOR_BINARY:-./src/app/tor}"
fi

# make a safe space for temporary files
DATA_DIR=$(mktemp -d -t tor_parseconf_tests.XXXXXX)
trap 'rm -rf "$DATA_DIR"' 0
echo "" > "${DATA_DIR}/EMPTY" || die "Couldn't create empty file."

# This is where we look for examples
EXAMPLEDIR="$(dirname "$0")"/conf_examples

for dir in "${EXAMPLEDIR}"/*; do
    testname="$(basename "${dir}")"
    # We use printf since "echo -n" is not standard
    printf "%s: " "$testname"

    if test -f "${dir}/torrc.defaults"; then
        DEFAULTS="${dir}/torrc.defaults"
    else
        DEFAULTS="${DATA_DIR}/EMPTY"
    fi

    if test -f "${dir}/cmdline"; then
        CMDLINE="$(cat "${dir}"/cmdline)"
    else
        CMDLINE=""
    fi

    if test -f "${dir}/expected"; then
        # This case should succeed: run dump-config and see if it does.

        "${TOR_BINARY}" -f "${dir}"/torrc \
                        --defaults-torrc "${DEFAULTS}" \
                        --dump-config short \
                        ${CMDLINE} \
                        > "${DATA_DIR}/output.${testname}" \
                        || die "Failure: Tor exited."

        if cmp "${dir}/expected" "${DATA_DIR}/output.${testname}" ; then
            # Check round-trip.
            "${TOR_BINARY}" -f "${DATA_DIR}/output.${testname}" \
                            --defaults-torrc "${DATA_DIR}/empty" \
                            --dump-config short \
                            > "${DATA_DIR}/output_2.${testname}" \
                        || die "Failure: Tor exited on round-trip."

            if ! cmp "${DATA_DIR}/output.${testname}" \
                 "${DATA_DIR}/output_2.${testname}"; then
                echo "Failure: did not match on round-trip."
                exit 1
            fi

            echo "OK"
        else
            echo "FAIL"
            diff -u "${dir}/expected" "${DATA_DIR}/output.${testname}"
            exit 1
        fi

    elif test -f "${dir}/error"; then
        # This case should succeed: run verify-config and see if it does.

        "${TOR_BINARY}" --verify-config \
                        -f "${dir}"/torrc \
                        --defaults-torrc "${DEFAULTS}" \
                        ${CMDLINE} \
                        > "${DATA_DIR}/output.${testname}" \
                        && die "Failure: Tor did not report an error."

        expect_err="$(cat "${dir}"/error)"
        if grep "${expect_err}" "${DATA_DIR}/output.${testname}" >/dev/null; then
            echo "OK"
        else
            echo "FAIL"
            echo "Expected error: ${expect_err}"
            echo "Tor said:"
            cat "${DATA_DIR}/output.${testname}"
            exit 1
        fi

    else
        # This case is not actually configured with an expectation.

        echo "SKIP"
    fi

done
