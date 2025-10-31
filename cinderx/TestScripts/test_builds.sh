#!/bin/bash

set -xe

cd "$(dirname "$(readlink -f "$0")")"/ || exit

function test_for_version() {
  local v="$1"
  buck build @fbcode//mode/dev fbcode//cinderx:python$v
  buck build @fbcode//mode/opt fbcode//cinderx/RuntimeTests:RuntimeTests_$v
}

test_for_version 3.10
test_for_version 3.12
test_for_version 3.14
