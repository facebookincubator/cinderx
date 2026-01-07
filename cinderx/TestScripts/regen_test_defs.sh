#!/bin/bash

# Verify that the tests.bzl file is up to date and accounts for all of the CPython and cinder tests.
# If the tests diff you can re-run this and cp /tmp/tests.txt to cinderx/tests.bzl

set -e

cd "$(dirname "$(readlink -f "$0")")"

./gen_test_defs.sh "$(pwd)/../tests.bzl"
