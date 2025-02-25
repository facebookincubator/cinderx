#!/bin/bash

# Runs native CinderX tests against 3.12. We have a script for this because
# many tests still don't work. This script will first runs all the tests
# expected to pass and stops if any fail. It then runs all the other tests, and
# if any pass reports this and adds to the list of known passing tests.

set -xe

cd "$(dirname "$(readlink -f "$0")")"/

function remove_ids_from_original_file {
    local testids_file="$1"
    local failing_ids_file="$2"

    # remove all lines from testsids_file which exist in failing_ids_file
    grep -v -f "$failing_ids_file" "$testids_file" > "${testids_file}.tmp" && mv "${testids_file}.tmp" "$testids_file"
}

function run_tests() {
    local target="$1"
    local testids_file="$2"
    local should_update="$3"

    echo "Checking if existing tests pass from $target"

    EXISTING_EVENT_LOG=$(mktemp)
    FAILING_TESTS=$(mktemp)
    buck test "$target" -- --test-ids "@$testids_file" --event-log-file="$EXISTING_EVENT_LOG" || true;
    jq 'select(.status != 1 and .test_name != null)
        | [.test_name, .result_id]
        | [(.[0] | split(" - ")[1]), (.[1] | split(".")[1])]
        | select(.[0] != "main")' "$EXISTING_EVENT_LOG" > "$FAILING_TESTS"

    if [ -s "$FAILING_TESTS" ] ; then
        echo "The following tests are new failures:"
        jq -r '.[0]' "$FAILING_TESTS";
        if [ "$should_update" = 1 ]; then
            FAIL_IDS=$(mktemp);
            jq -r '.[1]' "$FAILING_TESTS" >> "$FAIL_IDS"
            remove_ids_from_original_file "$testids_file" "$FAIL_IDS"
        else
            exit 1;
        fi

    fi

    echo "Checking if any new tests pass from $target"

    EVENT_LOG=$(mktemp)
    PASSING_TESTS=$(mktemp)
    buck test "$target" -- \
        --exclude-test-ids "@$testids_file" --event-log-file="$EVENT_LOG" \
        --tags exclude-from-signalbox || true

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
        exit 1
    else
        echo "No new tests passing"
    fi
}

UPDATE_MODE=0
for v in "$@" ; do
  case "$v" in
    --update)
      UPDATE_MODE=1
      ;;
    *)
      ;;
  esac
done


run_tests \
    fbcode//cinderx/RuntimeTests:RuntimeTests_3.12 \
    "$PWD/3.12-runtime-testids-passing" "$UPDATE_MODE"
