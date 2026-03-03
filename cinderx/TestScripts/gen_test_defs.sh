#!/bin/bash

# Generate what should be the current contents of the tests.bzl file and write
# it out to $1.

set -e

if [ -z "$1" ]; then echo "Usage: $0 <output_file>"; exit 1; fi

SCRIPT_DIR="$(dirname "$(readlink -f "$0")")"
TP_DIR="$SCRIPT_DIR/../../../third-party/python"

# Free-threaded versions for which we generate TSAN test lists.
# Maps CinderX version ID to the third-party/python subdirectory containing
# the Python source (must match PY_METADATA_THIRD_PARTY in defs.bzl).
declare -A TSAN_VERSIONS=(
    ["3.14t"]="3.14"
    # ["3.15t"]="main"
)

echo Generate 3.10
buck run fbcode//cinderx:ctl3.10 > /tmp/3.10.txt
echo Generate 3.12
buck run fbcode//cinderx:ctl3.12 > /tmp/3.12.txt
echo Generate 3.14
buck run fbcode//cinderx:ctl3.14 > /tmp/3.14.txt
echo Generate 3.15
buck run fbcode//cinderx:ctl3.15 > /tmp/3.15.txt

# Generate TSAN test lists from upstream tsan.py for each free-threaded version.
for tsan_ver in "${!TSAN_VERSIONS[@]}"; do
    tp_subdir="${TSAN_VERSIONS[$tsan_ver]}"
    tsan_py="$TP_DIR/$tp_subdir/patched/Lib/test/libregrtest/tsan.py"
    echo "Generate TSAN test list for $tsan_ver from $tsan_py"
    fbpython -c "
import sys, os
sys.path.insert(0, os.path.dirname('$tsan_py'))
from tsan import TSAN_TESTS
for t in TSAN_TESTS:
    print('    \"test.{}\",'.format(t))
" > "/tmp/tsan_tests_${tsan_ver}.txt"
done

{
    echo "TSAN_TESTS = {"
    for tsan_ver in $(echo "${!TSAN_VERSIONS[@]}" | tr ' ' '\n' | sort); do
        echo "\"$tsan_ver\": ["
        cat "/tmp/tsan_tests_${tsan_ver}.txt"
        echo "],"
    done
    echo "}"
    echo ""
    echo "TESTS = {"
    echo \"3.10\":
    cat /tmp/3.10.txt
    echo ","
    echo \"3.12\":
    cat /tmp/3.12.txt
    echo ","
    echo \"3.14\":
    cat /tmp/3.14.txt
    echo ","
    echo \"3.15\":
    cat /tmp/3.15.txt
    echo "}"
} > "$1"
pyfmt "$1"
