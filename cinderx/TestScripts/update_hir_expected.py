#!/usr/bin/env fbpython
# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

import argparse
import collections
import os
import re
import subprocess
import sys
from enum import Enum
from typing import Generator, Iterator, Sequence

# Maps HIR variable to its HIR output.
VarOutputDict = dict[str, Sequence[str]]

# Maps all test cases to their variables.
TestOutputDict = dict[str, VarOutputDict]

# Maps all test suites to their tests..
SuiteOutputDict = dict[str, TestOutputDict]


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "command",
        help="Path to runtime_tests binary, optionally with extra arguments to filter which tests run",
        nargs=argparse.REMAINDER,
    )
    parser.add_argument(
        "--text-input",
        "-t",
        help="File containing output from a previous run of runtime_tests",
    )

    return parser.parse_args()


BUCK_TIMESTAMP_RE: re.Pattern[str] = re.compile(
    r"^\[\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}-\d{2}:\d{2}\] "
)
TEST_RUN_RE: re.Pattern[str] = re.compile(r"^\[ RUN +\] ([^.]+)\.(.+)$")
ACTUAL_TEXT_RE: re.Pattern[str] = re.compile(r'^    Which is: "(.+)\\n"$')
EXPECTED_VAR_RE: re.Pattern[str] = re.compile(r"^  ([^ ]+)$")
VERSION_RE: re.Pattern[str] = re.compile(r"^Python Version: (.+)$")

# special-case common abbrieviations like HIR and CFG when converting
# camel-cased suite name to its snake-cased file name
SUITE_NAME_RE: re.Pattern[str] = re.compile(r"(HIR|CFG|[A-Z][a-z0-9]+)")


def unescape_gtest_string(s: str) -> str:
    result = []
    s_iter = iter(s)
    try:
        while True:
            c = next(s_iter)
            if c != "\\":
                result.append(c)
                continue
            c = next(s_iter)
            if c == "n":
                result.append("\n")
                continue
            result.append(c)
    except StopIteration:
        pass
    return "".join(result)


def get_test_stdout(args: argparse.Namespace) -> str:
    if args.text_input:
        with open(args.text_input, "r") as f:
            return f.read()

    if args.command:
        proc = subprocess.run(
            args.command + ["--gtest_color=no"],
            stdout=subprocess.PIPE,
            encoding=sys.stdout.encoding,
        )
        if proc.returncode == 0:
            raise RuntimeError("No tests failed!")
        elif proc.returncode != 1:
            raise RuntimeError(
                f"Command exited with {proc.returncode}, suggesting tests did not run to completion"
            )
        return proc.stdout

    raise RuntimeError("Must give either --text-input or a command to run")


# Optionally strip Buck formatted timestamp to allow input from Buck.
def timestamp_stripped_lineiter(lineiter: Iterator[str]) -> Generator[str, None, None]:
    for line in lineiter:
        yield BUCK_TIMESTAMP_RE.sub("", line)


def parse_stdout(stdout: str) -> tuple[str, SuiteOutputDict]:
    """
    Parse out the Python version and failed test information from stdout.
    """

    unknown_version = "<UNKNOWN>"
    py_version = unknown_version

    failed_tests = collections.defaultdict(lambda: {})
    line_iter = timestamp_stripped_lineiter(iter(stdout.split("\n")))
    test_dict: dict[str, list[str]] = {}
    test_name: tuple[str, str] = ("??", "??")
    for line in line_iter:
        if m := VERSION_RE.match(line):
            py_version = m[1]
            continue

        if m := TEST_RUN_RE.match(line):
            test_name = (m[1], m[2])
            test_dict = {}
            continue

        m = ACTUAL_TEXT_RE.match(line)
        if not m:
            continue

        actual_text = unescape_gtest_string(m[1]).split("\n")
        line = next(line_iter)
        m = EXPECTED_VAR_RE.match(line)
        if not m:
            raise RuntimeError(f"Unexpected line '{line}' after actual text")
        varname = m[1]
        if varname in test_dict:
            raise RuntimeError(
                f"Duplicate expect variable name '{varname}' in {test_name[0]}.{test_name[1]}"
            )
        test_dict[varname] = actual_text
        failed_tests[test_name[0]][test_name[1]] = test_dict

        # Skip the "Which is: ..." line after the expect variable name.
        next(line_iter)

    if py_version == unknown_version:
        raise RuntimeError("Couldn't figure out Python version from test output")

    return py_version, failed_tests


TESTS_DIR: str = os.path.normpath(
    os.path.join(os.path.dirname(__file__), "..", "RuntimeTests")
)


def map_suite_to_file_basename(suite_name: str) -> str:
    return "_".join(map(str.lower, SUITE_NAME_RE.findall(suite_name)))


assert map_suite_to_file_basename("CleanCFGTest") == "clean_cfg_test"
assert map_suite_to_file_basename("HIRBuilderTest") == "hir_builder_test"
assert (
    map_suite_to_file_basename("ProfileDataStaticHIRTest")
    == "profile_data_static_hir_test"
)
assert (
    map_suite_to_file_basename("SomethingEndingWithHIR") == "something_ending_with_hir"
)


def map_suite_to_file(suite_name: str) -> str:
    snake_name = map_suite_to_file_basename(suite_name)
    return os.path.join(TESTS_DIR, "hir_tests", snake_name + ".txt")


def update_text_test(  # noqa: C901
    suite_name: str,
    old_lines: list[str],
    failed_tests: TestOutputDict,
    py_version: str,
) -> list[str]:
    line_index: int = 0
    new_lines: list[str] = []

    def peek_line() -> str:
        nonlocal line_index
        if line_index >= len(old_lines):
            raise StopIteration
        return old_lines[line_index]

    def next_line() -> str:
        nonlocal line_index
        line = peek_line()
        line_index += 1
        return line

    def expect(exp: str, actual: str | None = None) -> None:
        line = actual or next_line()
        if line != exp:
            raise RuntimeError(
                f"Expected '{exp}', got '{line}' on line {line_index + 1}"
            )
        new_lines.append(line)

    expect("--- Test Suite Name ---")
    expect(suite_name)
    expect("--- Passes ---")

    # Optional pass names.
    while not peek_line().startswith("---"):
        new_lines.append(next_line())

    try:
        while True:
            line = next_line()
            if line in "--- End ---":
                new_lines.append(line)
                break
            expect("--- Test Name ---", line)

            test_case = next_line()
            if test_case == "@disabled":
                test_case += "\n" + next_line()
            new_lines.append(test_case)

            expect("--- Input ---")
            while not peek_line().startswith("---"):
                new_lines.append(next_line())

            # Find the right expected block for the given Python version.
            expected_version = f"--- Expected {py_version} ---"
            no_existing_expected_output = False
            while True:
                line = peek_line()
                if line in ("--- Test Name ---", "--- End ---"):
                    no_existing_expected_output = True
                    break

                line = next_line()
                new_lines.append(line)
                if line == "--- Skip ---":
                    no_existing_expected_output = True
                    break
                if not line.startswith("---"):
                    continue
                if line == expected_version:
                    break
                if not line.startswith("--- Expected"):
                    raise RuntimeError(
                        f"Test '{suite_name}.{test_case}' hit {line!r} before seeing expected HIR output for version {py_version!r}"
                    )

            if no_existing_expected_output and test_case not in failed_tests:
                continue

            # Add new expected output
            hir_lines = []
            if test_case in failed_tests:
                if no_existing_expected_output:
                    new_lines.append(expected_version)
                # For text HIR tests, there should only be one element in the
                # failed test dict.
                hir_lines = next(iter(failed_tests[test_case].values()))
                new_lines += hir_lines
                while not peek_line().startswith("---"):
                    next_line()
            else:
                while not peek_line().startswith("---"):
                    new_lines.append(next_line())

            # Process any other expected HIR blocks that come after.
            while peek_line().startswith("--- Expected") or peek_line().startswith(
                "--- Skip"
            ):
                new_lines.append(next_line())
                while not peek_line().startswith("---"):
                    new_lines.append(next_line())

    except StopIteration:
        pass

    return new_lines


def write_if_changed(filename: str, old_lines: list[str], new_lines: list[str]) -> None:
    if new_lines == old_lines:
        return
    with open(filename, "w") as f:
        print(f"Rewriting {filename}")
        f.write("\n".join(new_lines) + "\n")


CPP_TEST_NAME_RE: re.Pattern[str] = re.compile(r"^TEST(_F)?\(([^,]+), ([^)]+)\) {")
# Dynamic regex patterns - will be generated based on versions found
CPP_MACRO_ELSE_RE: re.Pattern[str] = re.compile(r"^#else")
CPP_MACRO_ENDIF_RE: re.Pattern[str] = re.compile(r"^#endif")


def python_version_to_hex(version: str) -> str:
    """Convert Python version like '3.12' to hex like '0x030C0000'."""
    parts = version.split(".")
    if len(parts) != 2:
        raise ValueError(f"Invalid Python version format: {version}")
    major, minor = parts
    if major != "3":
        raise ValueError(f"Only Python 3.x versions supported, got: {version}")
    return f"0x{int(major):02X}{int(minor):02X}0000"


def parse_version_from_line(line: str) -> str | None:
    """Extract Python version from #if or #elif preprocessor line."""
    # Match #if PY_VERSION_HEX >= 0xABCD0000 or #elif PY_VERSION_HEX >= 0xABCD0000
    match = re.match(
        r"^#(?:el)?if PY_VERSION_HEX >= 0x([0-9A-F]{2})([0-9A-F]{2})0000", line
    )
    if not match:
        return None
    major_hex, minor_hex = match.groups()
    major = int(major_hex, 16)
    minor = int(minor_hex, 16)
    return f"{major}.{minor}"


def version_compare(v1: str, v2: str) -> int:
    """Compare two Python versions. Returns -1 if v1 < v2, 0 if equal, 1 if v1 > v2."""
    v1_parts = [int(x) for x in v1.split(".")]
    v2_parts = [int(x) for x in v2.split(".")]
    if v1_parts < v2_parts:
        return -1
    elif v1_parts > v2_parts:
        return 1
    else:
        return 0


CPP_EXPECTED_START_RE: re.Pattern[str] = re.compile(r"^(  const char\* ([^ ]+) =)")
CPP_EXPECTED_END = ')";'
CPP_TEST_END = "}"


def find_cpp_files(root: str) -> Generator[str, None, None]:
    for dirpath, _dirnames, filenames in os.walk(root):
        for filename in filenames:
            if filename.endswith(".cpp"):
                yield os.path.join(dirpath, filename)


class State(Enum):
    # Active before the first test is found, or during a passing test.
    WAIT_FOR_TEST = 1

    # Active while reading a test that has failed, either waiting for an
    # expected variable or a new test.
    PROCESS_FAILED_TEST = 2

    # Active while skipping lines of an expected variable.
    SKIP_EXPECTED = 3


# pyre-ignore[30]
def update_cpp_tests(  # noqa: C901
    failed_suites: SuiteOutputDict,
    failed_cpp_tests: set[tuple[str, str]],
    py_version: str,
) -> None:
    # Validate Python version format
    try:
        parts = py_version.split(".")
        if len(parts) != 2 or parts[0] != "3":
            raise ValueError()
        int(parts[1])  # Ensure minor version is a number
    except (ValueError, IndexError):
        raise AssertionError(
            f"Invalid Python version format: {py_version}. Expected format: '3.X'"
        )

    # pyre-fixme[53]: Captured variable `cpp_filename` is not annotated.
    # pyre-fixme[53]: Captured variable `lineno` is not annotated.
    # pyre-fixme[53]: Captured variable `state` is not annotated.
    def expect_state(estate: State) -> None:
        nonlocal state, lineno, cpp_filename
        if state is not estate:
            sys.exit(
                f"Expected state {estate} at {cpp_filename}:{lineno}, actual {state}"
            )

    # pyre-fixme[53]: Captured variable `suite_name` is not annotated.
    # pyre-fixme[53]: Captured variable `test_dict` is not annotated.
    # pyre-fixme[53]: Captured variable `test_name` is not annotated.
    def expect_empty_test_dict() -> None:
        if test_dict is not None and len(test_dict) > 0:
            print(
                f"Couldn't find {len(test_dict)} expected variables in {suite_name}.{test_name}:"
            )
            print(list(test_dict.keys()))

    test_dict = {}
    for cpp_filename in find_cpp_files(TESTS_DIR):
        print(f"Considering updating tests in {cpp_filename}")
        with open(cpp_filename, "r") as f:
            old_lines = f.read().split("\n")

        state = State.WAIT_FOR_TEST
        in_version_block = None
        needs_to_close_upgraded_block = False
        new_lines = []
        for lineno, line in enumerate(old_lines, 1):  # noqa: B007
            m = CPP_TEST_NAME_RE.match(line)
            if m is not None:
                new_lines.append(line)

                expect_empty_test_dict()
                test_dict = {}

                suite_name = m[2]
                test_name = m[3]
                try:
                    failed_cpp_tests.remove((suite_name, test_name))
                except KeyError:
                    state = State.WAIT_FOR_TEST
                    continue

                test_dict = failed_suites[suite_name][test_name]
                state = State.PROCESS_FAILED_TEST
                continue

            if state is State.WAIT_FOR_TEST:
                new_lines.append(line)
                continue

            # Dynamically detect version-specific preprocessor directives
            detected_version = parse_version_from_line(line)
            if detected_version is not None:
                if line.startswith("#if"):
                    assert in_version_block is None, (
                        f"Nested version blocks @ line {lineno}"
                    )
                    in_version_block = detected_version
                elif line.startswith("#elif"):
                    assert in_version_block is not None, (
                        f"Unexpected elif at line {lineno}, not in version block"
                    )
                    in_version_block = detected_version
                new_lines.append(line)
                continue

            if in_version_block is not None and CPP_MACRO_ELSE_RE.match(line):
                # #else represents the fallback version (lowest supported version)
                in_version_block = "3.10"  # Assume 3.10 as the lowest supported version
                new_lines.append(line)
                continue

            if in_version_block is not None and CPP_MACRO_ENDIF_RE.match(line):
                in_version_block = None
                new_lines.append(line)
                continue

            m = CPP_EXPECTED_START_RE.match(line)
            if m is not None:
                expect_state(State.PROCESS_FAILED_TEST)
                decl = m[1]
                varname = m[2]

                actual_lines = test_dict.pop(varname, None)
                if actual_lines is None:
                    # This test has multiple expected variables, and this one is OK.
                    new_lines.append(line)
                    continue

                # Handle upgrading existing version block to accommodate new version
                if (
                    in_version_block is not None
                    and version_compare(py_version, in_version_block) > 0
                ):
                    # We're updating an existing version block to add newer version support
                    # Find and update the most recent #if line to use the new version
                    old_version_hex = python_version_to_hex(in_version_block)
                    new_version_hex = python_version_to_hex(py_version)

                    for i in range(len(new_lines) - 1, -1, -1):
                        if f"#if PY_VERSION_HEX >= {old_version_hex}" in new_lines[i]:
                            new_lines[i] = f"#if PY_VERSION_HEX >= {new_version_hex}"
                            break

                    # Add the new version content
                    new_lines.append(decl + ' R"(' + actual_lines[0])
                    new_lines += actual_lines[1:]
                    new_lines.append(CPP_EXPECTED_END)
                    new_lines.append(f"#elif PY_VERSION_HEX >= {old_version_hex}")

                    # Now the existing version content will be added
                    new_lines.append(line)
                    continue

                if in_version_block is not None and in_version_block != py_version:
                    new_lines.append(line)
                    continue

                # Upgrade unversioned code to version-switched block
                if in_version_block is None:
                    new_version_hex = python_version_to_hex(py_version)
                    new_lines.append(f"#if PY_VERSION_HEX >= {new_version_hex}")
                    new_lines.append(decl + ' R"(' + actual_lines[0])
                    new_lines += actual_lines[1:]
                    new_lines.append(CPP_EXPECTED_END)

                    # Determine what the fallback block should be
                    # If py_version > 3.10, use #elif for 3.10/older, otherwise use #else
                    if version_compare(py_version, "3.10") > 0:
                        fallback_hex = python_version_to_hex("3.10")
                        new_lines.append(f"#elif PY_VERSION_HEX >= {fallback_hex}")
                    else:
                        new_lines.append("#else")

                    needs_to_close_upgraded_block = True
                    new_lines.append(line)
                    continue

                # Regular case: replace content in current version block
                new_lines.append(decl + ' R"(' + actual_lines[0])
                new_lines += actual_lines[1:]
                state = State.SKIP_EXPECTED
                continue

            if state is State.SKIP_EXPECTED:
                if line == CPP_EXPECTED_END:
                    new_lines.append(line)
                    if needs_to_close_upgraded_block:
                        new_lines.append("#endif")
                        needs_to_close_upgraded_block = False
                    state = State.PROCESS_FAILED_TEST
                continue

            new_lines.append(line)

            if line == CPP_EXPECTED_END and needs_to_close_upgraded_block:
                new_lines.append("#endif")
                needs_to_close_upgraded_block = False

        expect_empty_test_dict()
        write_if_changed(cpp_filename, old_lines, new_lines)

    if len(failed_cpp_tests) > 0:
        print(f"\nCouldn't find {len(failed_cpp_tests)} failed test(s):")
        for test in failed_cpp_tests:
            print(f"{test[0]}.{test[1]}")


def main() -> None:
    args = parse_arguments()
    failed_cpp_tests = set()
    stdout = get_test_stdout(args)
    py_version, failed_suites = parse_stdout(stdout)

    for suite_name, failed_tests in failed_suites.items():
        suite_file = map_suite_to_file(suite_name)
        try:
            with open(suite_file, "r") as f:
                old_lines = f.read().split("\n")
        except FileNotFoundError:
            for test_name in failed_tests:
                failed_cpp_tests.add((suite_name, test_name))
            continue

        print(f"Scanning {suite_file}")
        new_lines = update_text_test(suite_name, old_lines, failed_tests, py_version)
        write_if_changed(suite_file, old_lines, new_lines)

    if len(failed_cpp_tests) > 0:
        update_cpp_tests(failed_suites, failed_cpp_tests, py_version)


if __name__ == "__main__":
    sys.exit(main())
