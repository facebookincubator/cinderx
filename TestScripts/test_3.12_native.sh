#!/bin/bash

# Runs native CinderX tests against 3.12. We have a script for this because
# many tests still don't work. This script will first runs all the tests
# expected to pass and stops if any fail. It then runs all the other tests, and
# if any pass reports this and adds to the list of known passing tests.

set -e

cd "$(dirname "$(readlink -f "$0")")"/

function run_tests() {
    local target="$1"
    local testids_file="$2"

    echo "Checking if existing tests pass from $target"

    buck test -c cinderx.use_3_12=true "$target" -- --test-ids "@$testids_file"

    echo "Checking if any new tests pass from $target"

    EVENT_LOG=$(mktemp)
    PASSING_TESTS=$(mktemp)
    buck test -c cinderx.use_3_12=true "$target" -- \
        --exclude-test-ids "@$testids_file" --event-log-file="$EVENT_LOG" || true

    jq 'select(.status == 1)
        | [.test_name, .result_id]
        | [(.[0] | split(" - ")[1]), (.[1] | split(".")[1])]
        | select(.[0] != "main")'  "$EVENT_LOG" > "$PASSING_TESTS"

    if [ -s "$PASSING_TESTS" ] ; then
        echo "The following tests not known to pass before are now passing:"
        jq -r '.[0]' "$PASSING_TESTS"
        jq -r '.[1]' "$PASSING_TESTS" >> "$testids_file"
        sort -u -o "$testids_file" "$testids_file"
        echo "The IDs for these new tests have been added to $testids_file"
        echo "If this looks correct please commit the changes. Otherwise revert."
    else
        echo "No new tests passing"
    fi
}

run_tests \
    fbcode//cinderx/RuntimeTests:RuntimeTests \
    "$PWD/3.12-runtime-testids-passing"

run_tests \
    fbcode//cinderx/StrictModules/Tests:Tests \
    "$PWD/3.12-strictmodules-testids-passing"
