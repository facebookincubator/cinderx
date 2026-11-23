#!/bin/bash

set -xe

cd "$(dirname "$(readlink -f "$0")")"/ || exit

function test_for_version() {
  local v="$1"
  buck build @fbcode//mode/dev fbcode//cinderx:python$v
  # Building the `python$v` command alias doesn't build the interpreter it
  # runs, so name the interpreter directly.
  buck build @fbcode//mode/libcxx "fbcode//cinderx/PythonBin:python_$v"
  buck build @fbcode//mode/opt fbcode//cinderx/RuntimeTests:RuntimeTests_$v
}

test_for_version 3.12
test_for_version 3.14
test_for_version 3.14t
test_for_version 3.15
