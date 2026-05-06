#!/bin/bash

# Generate what should be the current contents of the tests.bzl file and write
# it out to $1.  Optionally pass version(s) after the output file to regenerate
# only those versions (existing data for other versions is preserved from the
# current tests.bzl).

set -e

ALL_TEST_VERSIONS=(3.12 3.14 3.15)

# Free-threaded versions for which we generate TSAN test lists.
# Maps CinderX version ID to the third-party/python subdirectory containing
# the Python source (must match PY_METADATA_THIRD_PARTY in defs.bzl).
declare -A TSAN_VERSIONS=(
    ["3.14t"]="3.14"
    # ["3.15t"]="main"
)

usage() {
    echo "Usage: $0 <output_file> [VERSION...]"
    echo ""
    echo "Generate what should be the current contents of the tests.bzl file."
    echo "If versions are specified, only regenerate those versions (preserving"
    echo "existing data for other versions from the current tests.bzl)."
    echo ""
    echo "Available versions: ${ALL_TEST_VERSIONS[*]}"
    exit 1
}

if [ -z "$1" ]; then usage; fi
if [[ "$1" == "-h" || "$1" == "--help" ]]; then usage; fi

OUTPUT_FILE="$1"
shift

REQUESTED=("$@")

version_requested() {
    local version="$1"
    if [[ ${#REQUESTED[@]} -eq 0 ]]; then
        return 0
    fi
    for req in "${REQUESTED[@]}"; do
        if [[ "$req" == "$version" ]]; then
            return 0
        fi
    done
    return 1
}

tsan_version_requested() {
    local tsan_ver="$1"
    if version_requested "$tsan_ver"; then
        return 0
    fi
    local base_ver="${tsan_ver%t}"
    if version_requested "$base_ver"; then
        return 0
    fi
    return 1
}

SCRIPT_DIR="$(dirname "$(readlink -f "$0")")"
TP_DIR="$SCRIPT_DIR/../../../third-party/python"
TESTS_BZL="$SCRIPT_DIR/../tests.bzl"

# Extract an existing version's test list from tests.bzl and write it to a file
# in the same format that buck would produce (a Python list literal).
extract_tests() {
    local version_key="$1"
    local output_file="$2"
    fbpython -c "
import ast, sys
with open('$TESTS_BZL') as f:
    tree = ast.parse(f.read())
for node in tree.body:
    if isinstance(node, ast.Assign):
        for target in node.targets:
            if isinstance(target, ast.Name) and target.id == 'TESTS':
                d = ast.literal_eval(node.value)
                tests = d.get('$version_key', [])
                print('[')
                for t in tests:
                    print('    \"{}\",'.format(t))
                print(']')
                sys.exit(0)
print('[]')
" > "$output_file"
}

# Extract an existing TSAN version's test list from tests.bzl.
extract_tsan_tests() {
    local version_key="$1"
    local output_file="$2"
    fbpython -c "
import ast, sys
with open('$TESTS_BZL') as f:
    tree = ast.parse(f.read())
for node in tree.body:
    if isinstance(node, ast.Assign):
        for target in node.targets:
            if isinstance(target, ast.Name) and target.id == 'TSAN_TESTS':
                d = ast.literal_eval(node.value)
                tests = d.get('$version_key', [])
                for t in tests:
                    print('    \"{}\",'.format(t))
                sys.exit(0)
" > "$output_file"
}

for VERSION in "${ALL_TEST_VERSIONS[@]}"; do
    if version_requested "$VERSION"; then
        echo "Generate $VERSION"
        buck run "fbcode//cinderx:ctl${VERSION}" > "/tmp/${VERSION}.txt"
    else
        echo "Preserve existing $VERSION from tests.bzl"
        extract_tests "$VERSION" "/tmp/${VERSION}.txt"
    fi
done

# Generate TSAN test lists from upstream tsan.py for each free-threaded version.
for tsan_ver in "${!TSAN_VERSIONS[@]}"; do
    if tsan_version_requested "$tsan_ver"; then
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
    else
        echo "Preserve existing TSAN $tsan_ver from tests.bzl"
        extract_tsan_tests "$tsan_ver" "/tmp/tsan_tests_${tsan_ver}.txt"
    fi
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
    for VERSION in "${ALL_TEST_VERSIONS[@]}"; do
        echo "\"$VERSION\":"
        cat "/tmp/${VERSION}.txt"
        echo ","
    done
    echo "}"
} > "$OUTPUT_FILE"
pyfmt "$OUTPUT_FILE"
