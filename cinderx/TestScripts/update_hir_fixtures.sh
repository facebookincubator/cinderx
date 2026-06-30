#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Regenerate the JIT HIR test fixtures for one or more supported Python versions.
#
# Usage:
#   ./update_hir_fixtures.sh            # update all supported versions
#   ./update_hir_fixtures.sh 3.14 3.16  # update only the given versions
#
# With no arguments the set of versions is discovered from buck: every enabled
# RuntimeTests_<ver> target (i.e. those PY_VERSION_MAP entries with the C++
# unittest feature). This means a newly supported Python version is picked up
# automatically with no change to this script.

set -xeuo pipefail

script_dir="$(dirname "$(readlink -f "$0")")"

# buck resolves the project root from the working directory, so its invocations
# run in a subshell that cd's into the repo. This keeps the script's own working
# directory unchanged.
function discover_versions() {
  ( cd "$script_dir" && buck2 uquery \
    "fbcode//cinderx/RuntimeTests: except attrfilter(labels, disabled, fbcode//cinderx/RuntimeTests:)" \
    2>/dev/null ) \
    | sed -n 's#^fbcode//cinderx/RuntimeTests:RuntimeTests_\([0-9]\+\.[0-9]\+t\?\)$#\1#p'
}

function update_for_version() {
  local v="$1"
  local tmp
  tmp="$(mktemp)"
  # A non-zero exit just means some tests failed, which is how the updater
  # discovers what to rewrite. buck's own logging goes to stderr.
  ( cd "$script_dir" && buck2 run @fbcode//mode/opt \
    "fbcode//cinderx/RuntimeTests:RuntimeTests_$v" -- --gtest_color=no \
    > "$tmp" 2>/dev/null ) || true
  fbpython "$script_dir/update_hir_expected.py" -t "$tmp"
  rm -f "$tmp"
}

versions=("$@")
if [ "${#versions[@]}" -eq 0 ]; then
  mapfile -t versions < <(discover_versions)
fi

for v in "${versions[@]}"; do
  update_for_version "$v"
done
