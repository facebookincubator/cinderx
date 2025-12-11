#!/bin/bash

set -xe
export HG_PLAIN=1

TEST_LIST_FILE=$1
PYTHON_VERSION=$2

cd "$(dirname "$(readlink -f "$0")")"/

is_sandcastle() {
    [[ -n "${SANDCASTLE:-}" || -n "${SANDCASTLE_ID:-}" || -n "${SANDCASTLE_INSTANCE_ID:-}" ]]
}

if is_sandcastle; then
    echo "Detected Sandcastle environment, checking for tags..."
    jf sync || true
    COMMIT_MSG=$(sl log -r . -T "{desc}")
    # Extract tags line and check for no-hir-stats or no_hir_stats variants
    if echo "$COMMIT_MSG" | grep -qE "^Tags:.*no[-_]hir[-_]stats"; then
        echo "Found 'no-hir-stats' tag, skipping HIR stats test."
        exit 0
    fi
fi

function run_workload() {
    # Run through cinder_test_runner to skip tests known to break in JIT
    PYTHONJITDUMPHIRSTATS=1 PYTHONJITALL=1 PYTHONJITDEBUG=1 \
        buck run @//mode/opt "fbcode//cinderx:python$PYTHON_VERSION" -- cinder_test_runner312.py test "$@"  -- --randseed=1
}

# Cache tests to run as they may have changed in this revision
TEST_ARGS=$(sed "s/^/-t /" < "$TEST_LIST_FILE")

# Run the workload twice because the first run may generate .pyc's which will
# have slightly different code paths.
echo "Running test load on current revision (non-logging run)..."
# shellcheck disable=SC2086
run_workload $TEST_ARGS 2>/dev/null
echo "Running test load on current revision (logging run)..."
# shellcheck disable=SC2086
run_workload $TEST_ARGS 2>/tmp/head_jit_log
HEAD_REV=$(sl log -r . -T "{node}")
sl prev

echo "Running test load on base revision (non-logging run)..."
# shellcheck disable=SC2086
run_workload $TEST_ARGS 2>/dev/null
echo "Running test load on base revision (logging run)..."
# shellcheck disable=SC2086
run_workload $TEST_ARGS 2>/tmp/base_jit_log
sl up "$HEAD_REV"

set +e
./jit_log_stats_parser.py compare --error-on-diff --per-function-details \
    --name1 "Head" --name2 "Base" /tmp/head_jit_log /tmp/base_jit_log
FINAL_EXIT_STATUS=$?
set -e

if [[ $FINAL_EXIT_STATUS -ne 0 ]] && is_sandcastle; then
    echo -e "\nIf not relevant, add tag no-hir-stats to this diff to suppress."
fi

exit $FINAL_EXIT_STATUS
