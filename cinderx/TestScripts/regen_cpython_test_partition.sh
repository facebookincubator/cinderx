#!/bin/bash

# Regenerate cinderx/cpython_test_partition.bzl.
#
# Classification depends on what the interpreter can actually import and how
# each module exposes its tests, so it has to run under the CinderX Python for
# the version in question rather than being computed statically.
#
# Usage: regen_cpython_test_partition.sh [--output-root DIR] [VERSION...]
#
# With no versions, regenerates the ones already present in the partition file.
# Naming a version adds or refreshes just that one; data for the others is
# preserved. --output-root writes elsewhere than the CinderX tree, which is how
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

if [[ ${#VERSIONS[@]} -eq 0 ]]; then
    # Default to whatever is already covered, so the staleness check does not
    # fail merely because some version has not been onboarded yet.
    if ! VERSIONS_RAW="$(fbpython -c '
import ast, sys
with open(sys.argv[1]) as partition_file:
    tree = ast.parse(partition_file.read())
for node in tree.body:
    if isinstance(node, ast.Assign) and any(
        isinstance(target, ast.Name) and target.id == "TIER1_TESTS"
        for target in node.targets
    ):
        print("\n".join(sorted(ast.literal_eval(node.value))))
        sys.exit(0)
' "$PARTITION_FILE")"; then
        echo "Failed to read versions from $PARTITION_FILE" >&2
        exit 1
    fi
    mapfile -t VERSIONS < <(printf '%s' "$VERSIONS_RAW")
fi

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
    (cd "$CINDERX_ROOT/.." && buck2 run "fbcode//cinderx:python${VERSION}" -- \
        cinderx/TestScripts/classify_cpython_tests.py \
        --version "$VERSION" --output "$OUT")
    JSON_FILES+=("$OUT")
done

fbpython gen_cpython_test_defs.py "${JSON_FILES[@]}" --cinderx-root "$OUTPUT_ROOT"
