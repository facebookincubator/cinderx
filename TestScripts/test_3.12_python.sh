#!/bin/bash

set -x

cd "$(dirname "$(readlink -f "$0")")"/ || exit

TMP_FILE=/tmp/tests_summary.json

function run_tests() {
    local mode="$1"
    local expected_file="$2"
    local update_mode="$3"

    buck run -c cinderx.use_3_12=true @fbcode//mode/"$mode" fbcode//cinderx:python-tests -- --json-summary-file="$TMP_FILE"

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
BUILD_MODE=opt
for v in "$@" ; do
  case "$v" in
    --update)
      UPDATE_MODE=1
      ;;
    *)
      BUILD_MODE=$v
      ;;
  esac
done

run_tests "$BUILD_MODE" "$PWD/3.12-python-expected-tests-$BUILD_MODE.json" "$UPDATE_MODE"
