#!/bin/bash

# Verify cinderx/cpython_test_partition.bzl matches what the classifier produces
# for the current Python. If this fails, run regen_cpython_test_partition.sh.

set -e

cd "$(dirname "$(readlink -f "$0")")"

CINDERX_ROOT="$(cd .. && pwd)"
GENERATED="$(mktemp -d)"
trap 'rm -rf "$GENERATED"' EXIT

./regen_cpython_test_partition.sh --output-root "$GENERATED"

diff -u "$CINDERX_ROOT/cpython_test_partition.bzl" "$GENERATED/cpython_test_partition.bzl"

echo "cpython_test_partition.bzl is up to date."
