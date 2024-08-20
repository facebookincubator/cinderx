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
            raise RuntimeError(f"Expected '{exp}', got '{line}'")
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
            if line == "--- End ---":
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
            while True:
                line = next_line()
                new_lines.append(line)
                if not line.startswith("---"):
                    continue
                if line == expected_version:
                    break
                if not line.startswith("--- Expected"):
                    raise RuntimeError(
                        f"Test '{suite_name}.{test_case}' hit {line!r} before seeing expected HIR output for version {py_version!r}"
                    )

            # Figure out what the expected HIR output is supposed to be.
            hir_lines = []
            while not peek_line().startswith("---"):
                hir_lines.append(next_line())
            if test_case in failed_tests:
                # For text HIR tests, there should only be one element in the
                # failed test dict.
                hir_lines = next(iter(failed_tests[test_case].values()))

            new_lines += hir_lines

            # Process any other expected HIR blocks that come after.
            if peek_line().startswith("--- Expected"):
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
        f.write("\n".join(new_lines))


CPP_TEST_NAME_RE: re.Pattern[str] = re.compile(r"^TEST(_F)?\(([^,]+), ([^)]+)\) {")
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


def update_cpp_tests(  # noqa: C901
    failed_suites: SuiteOutputDict, failed_cpp_tests: set[tuple[str, str]]
) -> None:
    def expect_state(estate: State) -> None:
        nonlocal state, lineno, cpp_filename
        if state is not estate:
            sys.exit(
                f"Expected state {estate} at {cpp_filename}:{lineno}, actual {state}"
            )

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

                # This may collapse a two-line start to the variable onto one
                # line, which clang-format will clean up.
                new_lines.append(decl + ' R"(' + actual_lines[0])
                new_lines += actual_lines[1:]
                state = State.SKIP_EXPECTED
                continue

            if state is State.SKIP_EXPECTED:
                if line == CPP_EXPECTED_END:
                    new_lines.append(line)
                    state = State.PROCESS_FAILED_TEST
                continue

            new_lines.append(line)

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

        new_lines = update_text_test(suite_name, old_lines, failed_tests, py_version)
        write_if_changed(suite_file, old_lines, new_lines)

    if len(failed_cpp_tests) > 0:
        update_cpp_tests(failed_suites, failed_cpp_tests)


if __name__ == "__main__":
    sys.exit(main())
