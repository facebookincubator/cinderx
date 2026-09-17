#!/bin/bash

# Regenerate cinderx/cpython_test_partition.bzl.
#
# Classification depends on what the interpreter can actually import and how
# each module exposes its tests, so it has to run under the CinderX Python for
# the version in question rather than being computed statically.
#
# Pass version(s) to regenerate only those; existing data for other versions is
# preserved from the current partition file.

set -e

VERSIONS=("${@:-3.14}")

cd "$(dirname "$(readlink -f "$0")")"

CINDERX_ROOT="$(pwd)/.."
JSON_FILES=()

for VERSION in "${VERSIONS[@]}"; do
    OUT="/tmp/cpython_partition_${VERSION}.json"
    echo "Classifying $VERSION"
    (cd ../../.. && buck2 run "fbcode//cinderx:python${VERSION}" -- \
        cinderx/TestScripts/classify_cpython_tests.py \
        --version "$VERSION" --output "$OUT")
    JSON_FILES+=("$OUT")
done

fbpython gen_cpython_test_defs.py "${JSON_FILES[@]}" --cinderx-root "$CINDERX_ROOT"
