#!/bin/bash

set -x

cd "$(dirname "$(readlink -f "$0")")"/ || exit

TMP_FILE=/tmp/tests_summary.json

function run_tests() {
    local mode="$1"
    local expected_file="$2"
    local update_mode="$3"
    local refleak_mode=$4
    local jit_all_mode=$5
    local no_retry_mode=$6

    options=("--json-summary-file=$TMP_FILE")
    if [ "$refleak_mode" == "-refleak" ] ; then
      options+=(--huntrleaks)
    fi
    if [ "$no_retry_mode" == "1" ] ; then
      options+=(--no-retry-on-test-errors)
    fi

    buck run @fbcode//mode/"$mode" "fbcode//cinderx:python-tests3.12$jit_all_mode" -- "${options[@]}";

    echo

    if cmp \
            <(jq .good "$TMP_FILE" && jq .bad "$TMP_FILE") \
            <(jq .good "$expected_file" && jq .bad "$expected_file") >/dev/null
    then
        echo "No change in expected test pass/failures"
    else
        echo "Changes in good test modules:"
        diff -u <(jq .good "$expected_file") <(jq .good "$TMP_FILE")

        echo

        echo "Changes in bad test modules:"
        diff -u <(jq .bad "$expected_file") <(jq .bad "$TMP_FILE")

        if [ "$update_mode" == 1 ] ; then
          echo "Updating expected outputs"
          jq . "$TMP_FILE" > "$expected_file"
        else
          echo "Was running with mode: $mode"
          echo
          echo "Manually update with: jq . \"$TMP_FILE\" > \"$expected_file\""
          echo "Or, re-run with --update"

          exit 1
       fi
    fi
}

UPDATE_MODE=0
REFLEAK_MODE=
BUILD_MODE=opt
JIT_ALL_MODE=
NO_RETRY_MODE=0
for v in "$@" ; do
  case "$v" in
    --update)
      UPDATE_MODE=1
      ;;
    --refleak)
      REFLEAK_MODE=-refleak
      ;;
    --jit-all)
      JIT_ALL_MODE=-jit-all
      ;;
    --no-retry)
      NO_RETRY_MODE=1
      ;;
    *)
      BUILD_MODE=$v
      ;;
  esac
done

run_tests "$BUILD_MODE" "$PWD/3.12-python-expected-tests-$BUILD_MODE$JIT_ALL_MODE$REFLEAK_MODE.json" "$UPDATE_MODE" "$REFLEAK_MODE" "$JIT_ALL_MODE" "$NO_RETRY_MODE"
