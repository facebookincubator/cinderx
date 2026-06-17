#!/bin/bash
# (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.
#
# Bootstrap per-Python-version expected output for every HIR test file by
# running tools/bootstrap_hir_test.py over each .txt file in
# RuntimeTests/hir_tests.
#
# Usage:
#     tools/bootstrap_hir_tests.sh MAJOR.MINOR
#
# Example:
#     tools/bootstrap_hir_tests.sh 3.16

set -euo pipefail

if [ "$#" -ne 1 ]; then
  echo "Usage: $0 MAJOR.MINOR" >&2
  exit 1
fi

version="$1"

tools_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cinderx_dir="$(dirname "$tools_dir")"
hir_tests_dir="$cinderx_dir/RuntimeTests/hir_tests"

for test_file in "$hir_tests_dir"/*.txt; do
  python3 "$tools_dir/bootstrap_hir_test.py" "$version" "$test_file"
done
