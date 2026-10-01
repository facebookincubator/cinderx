#!/bin/bash

# Regenerate cinderx/cpython_test_partition.bzl.
#
# Classification depends on what the interpreter can actually import and how
# each module exposes its tests, so it has to run under the CinderX Python for
# the version in question rather than being computed statically.
#
# Usage: regen_cpython_test_partition.sh [--output-root DIR] [VERSION...]
#
# Always regenerates every version already present in the partition file; naming
# a version adds it to that set. The generator writes the file from its inputs
# alone, so classifying a subset would silently drop the rest.
# --output-root writes elsewhere than the CinderX tree, which is how
# test_cpython_test_partition.sh checks for staleness.

set -e

cd "$(dirname "$(readlink -f "$0")")"

CINDERX_ROOT="$(cd .. && pwd)"
PARTITION_FILE="$CINDERX_ROOT/cpython_test_partition.bzl"
OUTPUT_ROOT="$CINDERX_ROOT"

VERSIONS=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --output-root)
            if [[ $# -lt 2 || -z "$2" ]]; then
                echo "Missing directory for --output-root" >&2
                exit 1
            fi
            OUTPUT_ROOT="$2"
            shift 2
            ;;
        --)
            shift
            VERSIONS+=("$@")
            break
            ;;
        -*)
            echo "Unknown option: $1" >&2
            exit 1
            ;;
        *)
            VERSIONS+=("$1")
            shift
            ;;
    esac
done

# Whatever is already covered, so the staleness check does not fail merely
# because some version has not been onboarded yet.
if ! VERSIONS_RAW="$(fbpython -c '
import ast, sys
with open(sys.argv[1]) as partition_file:
    tree = ast.parse(partition_file.read())
covered = set()
for node in tree.body:
    if isinstance(node, ast.Assign) and any(
        isinstance(target, ast.Name) and target.id == "TIER1_TESTS"
        for target in node.targets
    ):
        covered = set(ast.literal_eval(node.value))
        break
print("\n".join(sorted(covered | set(sys.argv[2:]))))
' "$PARTITION_FILE" "${VERSIONS[@]}")"; then
    echo "Failed to read versions from $PARTITION_FILE" >&2
    exit 1
fi
mapfile -t VERSIONS < <(printf '%s' "$VERSIONS_RAW")

if [[ ${#VERSIONS[@]} -eq 0 ]]; then
    echo "No versions to generate; pass one explicitly, e.g. $0 3.14" >&2
    exit 1
fi

TEMP_ROOT="$(mktemp --tmpdir -d cpython_partition_XXXXXX)"
trap 'rm -rf "${TEMP_ROOT}"' EXIT

JSON_FILES=()
for VERSION in "${VERSIONS[@]}"; do
    OUT="${TEMP_ROOT}/${VERSION}.json"
    echo "Classifying $VERSION"
    # Run from fbcode: the interpreter resolves the script path relative to cwd.
    # opt rather than the default dev, which is ASAN: CPython gates a dozen
    # modules off under a sanitizer, and classifying there would drop them from
    # the opt and dev-nosan targets as well.
    (cd "$CINDERX_ROOT/.." && buck2 run @fbcode//mode/opt "fbcode//cinderx:python${VERSION}" -- \
        cinderx/TestScripts/classify_cpython_tests.py \
        --version "$VERSION" --output "$OUT")
    JSON_FILES+=("$OUT")
done

fbpython gen_cpython_test_defs.py "${JSON_FILES[@]}" --cinderx-root "$OUTPUT_ROOT"
