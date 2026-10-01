#!/bin/bash

# Run the tests Tier 1 skips, through cinder_test_runner.py.
#
# These tests fail in python_unittest's PAR environment but can run under
# regrtest. Shared runner exclusions are intentionally absent from this list.
#
#   INSTALL_DIR       a CinderX Python install (cinderx/PythonBin:python_<ver>[install])
#   PYTHON_ABI        the on-disk interpreter version, e.g. 3.14 or 3.14t
#   TEST_RUNNER       path to cinder_test_runner.py; must be the checked-in copy,
#                     since the runner locates its skip lists relative to its own
#                     directory and PythonLib relative to that
#   SKIPPED_TESTS_PY  path to CPythonTests/skipped_tests.py
#   RESOLVE_SCRIPT    executable that resolves skipped tests into runner args
#   TIER1_MODULES     space-separated module names owned by Tier 1

set -eu

PYTHON="${INSTALL_DIR}/bin/python${PYTHON_ABI}"

# Let the copied interpreter derive its own prefix and path from argv[0].

workdir="$(mktemp -d)"
trap 'rm -rf "${workdir}"' EXIT
cd "${workdir}"

# Resolve each entry to the module that owns it, so regrtest can be told which
# files to open. Via a file rather than a process substitution: `mapfile < <(...)`
# discards the child's exit status, and `set -e` does not see it, so a resolver
# that failed would leave an empty plan and pass this target with nothing run.
plan_file="${workdir}/plan"
declare -a tier1_args=()
for module in ${TIER1_MODULES}; do
    tier1_args+=("--tier1-module" "${module}")
done
"${RESOLVE_SCRIPT}" "${tier1_args[@]}" "${SKIPPED_TESTS_PY}" > "${plan_file}"
mapfile -t plan < "${plan_file}"

declare -a module_args=()
declare -a pattern_args=()
for line in "${plan[@]+"${plan[@]}"}"; do
    case "${line}" in
        "M "*) module_args+=("-t" "${line#M }") ;;
        "P "*) pattern_args+=("-m" "${line#P }") ;;
    esac
done

if [[ ${#module_args[@]} -eq 0 ]]; then
    echo "Resolver produced no modules from ${SKIPPED_TESTS_PY}." >&2
    exit 1
fi

"${PYTHON}" "${TEST_RUNNER}" test "${module_args[@]}" -- "${pattern_args[@]}"
