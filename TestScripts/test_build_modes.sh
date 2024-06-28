#!/bin/bash

set -xe
set -o pipefail

cd "$(dirname "$(readlink -f "$0")")"/../

function fail() {
    set +x
    echo -en '\n\033[1;31m'
    echo "$@"
    echo -en '\033[1;0m\n'
    exit 1
}

function run_buck2() {
    local mode=$1
    shift
    buck2 run -c cinderx.use_3_12="$use_3_12" @fbcode//mode/"$mode" "$@"
}

function run_python_code() {
    local mode=$1
    local code=$2
    local expect_except=$3
    if run_buck2 "$mode" fbcode//cinderx:python -- -c "$code" ; then
        if [[ "$expect_except" == "except" ]] ; then
            fail "Expected '$1' run of '$2' to fail"
        fi
    else
        if [[ "$expect_except" == "except" ]] ; then
            return
        fi
        fail "Expected '$1' run of '$2' to pass"
    fi
}

function check_opt_flags() {
    local mode=$1
    local should_opt=$2
    local libpython=$(echo $(run_buck2 "$mode" fbcode//cinderx:dist-path)/lib/libpython3.*.so)
    if strings "$libpython" | grep -- "-O3 -fno-omit-frame-pointer" >/dev/null ; then
        if [[ "$should_opt" == "yes" ]] ; then
            return
        fi
        fail "Expected to not find opt flags with build mode $mode"
    else
        if [[ "$should_opt" == "yes" ]] ; then
            fail "Expected to find opt flags with build mode $mode"
        fi
    fi
}

for use_3_12 in true false ; do
    # # Only debug builds have sys.gettotalrefcount()
    run_python_code dbg 'import sys; sys.gettotalrefcount()'
    run_python_code dev 'import sys; sys.gettotalrefcount()'
    run_python_code opt 'import sys; sys.gettotalrefcount()' except

    # # Only ASAN builds should have the ASAN API avilable through ctypes
    run_python_code dbg 'import ctypes; ctypes.pythonapi.__asan_default_options()' except
    run_python_code dev 'import ctypes; ctypes.pythonapi.__asan_default_options()'
    run_python_code opt 'import ctypes; ctypes.pythonapi.__asan_default_options()' except

    # We embed build flags in our objects
    check_opt_flags dev no
    check_opt_flags dbg no
    check_opt_flags opt yes
done

echo -e '\n\033[1;32m**All tests passed!**\033[1;0m\n'
