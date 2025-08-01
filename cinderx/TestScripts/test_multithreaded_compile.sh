#!/bin/bash

set -xe

WORKERS=10

# Running the test module straight from PythonLib/
cd "$(dirname "$(readlink -f "$0")")"/../PythonLib

function run() {
    local mode=$1
    buck2 run @fbcode//mode/"${mode}" fbcode//cinderx:python3.10 -- -X jit -X jit-multithreaded-compile-test -X jit-batch-compile-workers="${WORKERS}" -m test_cinderx.multithreaded_compile_test
}

# TASK(T186691817): There should be at least one run with TSAN but we don't have
# that configured properly in buck yet.
run dbg
run dev
run opt
