#!/usr/bin/env python3
# (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

"""Bootstrap per-Python-version expected output in a CinderX HIR test file.

HIR golden tests (RuntimeTests/hir_tests/*.txt) carry one or more per-version
expected-output blocks per test case, delimited by `--- Expected MAJOR.MINOR ---`
(see RuntimeTests/hir_tests/dead_code_elimination_test.txt). When CinderX gains
support for a new Python version the expected HIR is almost always identical to
the previous version's at first; this tool seeds each test's
`--- Expected <new> ---` block by copying the previous version's block, giving
TestScripts/update_hir_expected.py a baseline to diff against.

For each test case it copies the body of the largest existing expected block
whose version is strictly less than the target version.

Usage:
    python3 tools/bootstrap_hir_test.py 3.16 RuntimeTests/hir_tests/some_test.txt
"""

from __future__ import annotations

# pyre-strict

import argparse
import logging
import re
import sys

logger: logging.Logger = logging.getLogger(__name__)

# A delimiter line both starts and ends with this (e.g. `--- Test Name ---`).
DELIM_PREFIX = "---"

EXPECTED_RE: re.Pattern[str] = re.compile(r"^--- Expected (\d+)\.(\d+) ---$")

# An expected/skip block: its header delimiter line plus the body lines that
# follow it (up to but not including the next delimiter).
Block = tuple[str, list[str]]


class TestCase:
    def __init__(self) -> None:
        # Test name line(s); a `@disabled` marker occupies its own leading line.
        self.name_lines: list[str] = []
        # `--- Input ---` header plus its body lines.
        self.input_lines: list[str] = []
        # Ordered `--- Expected X.Y ---` / `--- Skip ---` blocks.
        self.blocks: list[Block] = []


def parse_version(s: str) -> tuple[int, int]:
    """Parse a `MAJOR.MINOR` string into an (int, int) tuple."""
    parts = s.split(".")
    if len(parts) != 2:
        raise ValueError(
            f"Invalid Python version format: {s!r}, expected 'MAJOR.MINOR'"
        )
    major, minor = parts
    if major != "3":
        raise ValueError(f"Only Python 3.x versions supported, got: {s!r}")
    return (int(major), int(minor))


def is_delim(line: str) -> bool:
    return line.startswith(DELIM_PREFIX)


class Parser:
    """Line-based parser for the HIR test file grammar."""

    def __init__(self, lines: list[str]) -> None:
        self._lines = lines
        self._index = 0
        # Header lines: suite name, `--- Passes ---`, and pass names.
        self.header_lines: list[str] = []
        self.tests: list[TestCase] = []
        # The trailing `--- End ---` line.
        self.end_line: str = "--- End ---"

    def _peek(self) -> str:
        if self._index >= len(self._lines):
            raise ValueError("Unexpected end of file")
        return self._lines[self._index]

    def _next(self) -> str:
        line = self._peek()
        self._index += 1
        return line

    def _read_body(self) -> list[str]:
        """Read lines up to (not including) the next delimiter line."""
        body: list[str] = []
        while self._index < len(self._lines) and not is_delim(self._peek()):
            body.append(self._next())
        return body

    def parse(self) -> None:
        self._expect("--- Test Suite Name ---")
        self.header_lines.append(self._next())  # suite name
        self._expect("--- Passes ---")
        self.header_lines += self._read_body()  # optional pass names

        while True:
            line = self._next()
            if line == "--- End ---":
                self.end_line = line
                break
            if line != "--- Test Name ---":
                raise ValueError(f"Expected '--- Test Name ---', got {line!r}")
            self.tests.append(self._parse_test_case())

    def _parse_test_case(self) -> TestCase:
        tc = TestCase()
        name = self._next()
        if name == "@disabled":
            tc.name_lines.append(name)
            name = self._next()
        tc.name_lines.append(name)

        self._expect("--- Input ---")
        tc.input_lines.append("--- Input ---")
        tc.input_lines += self._read_body()

        # Remaining blocks belong to this test until the next test or `--- End ---`.
        while self._peek() not in ("--- Test Name ---", "--- End ---"):
            header = self._next()
            tc.blocks.append((header, self._read_body()))
        return tc

    def _expect(self, expected: str) -> None:
        line = self._next()
        if line != expected:
            raise ValueError(f"Expected {expected!r}, got {line!r}")


def seed_test_case(tc: TestCase, target: tuple[int, int]) -> bool:
    """Add a target-version expected block copied from the previous version.

    Returns True if a block was added, False if the test was left untouched.
    """
    versions: dict[tuple[int, int], int] = {}
    for i, (header, _body) in enumerate(tc.blocks):
        m = EXPECTED_RE.match(header)
        if m is not None:
            versions[(int(m[1]), int(m[2]))] = i

    if target in versions:
        return False  # Already present: idempotent, leave untouched.

    candidates = [v for v in versions if v < target]
    if not candidates:
        name = tc.name_lines[-1]
        logger.warning(
            "No expected block older than %d.%d for test %r; skipping",
            target[0],
            target[1],
            name,
        )
        return False

    src = max(candidates)
    src_body = list(tc.blocks[versions[src]][1])
    new_block: Block = (f"--- Expected {target[0]}.{target[1]} ---", src_body)

    # Insert in ascending version order: right after the source block (the
    # largest version below the target).
    tc.blocks.insert(versions[src] + 1, new_block)
    return True


def render(parser: Parser) -> list[str]:
    lines: list[str] = ["--- Test Suite Name ---"]
    lines += parser.header_lines[:1]  # suite name
    lines.append("--- Passes ---")
    lines += parser.header_lines[1:]  # pass names
    for tc in parser.tests:
        lines.append("--- Test Name ---")
        lines += tc.name_lines
        lines += tc.input_lines
        for header, body in tc.blocks:
            lines.append(header)
            lines += body
    lines.append(parser.end_line)
    return lines


def main() -> int:
    logging.basicConfig(level=logging.INFO, format="%(message)s")

    arg_parser = argparse.ArgumentParser(description=__doc__)
    arg_parser.add_argument("version", help="Target Python version, e.g. '3.16'")
    arg_parser.add_argument("test_file", help="Path to an HIR test .txt file")
    args = arg_parser.parse_args()

    target = parse_version(args.version)

    with open(args.test_file, "r") as f:
        old_lines = f.read().split("\n")
    # `split("\n")` on a trailing-newline file yields a trailing "" element.
    if old_lines and old_lines[-1] == "":
        old_lines.pop()

    # A few legacy files (e.g. call_optimization_test.txt) use the old
    # version-agnostic bare-`---` format and have no `--- Expected X.Y ---`
    # blocks to seed from. Skip them rather than failing.
    if not old_lines or old_lines[0] != "--- Test Suite Name ---":
        logger.warning(
            "Skipping %s: not in the labeled '--- Expected X.Y ---' format",
            args.test_file,
        )
        return 0

    parser = Parser(old_lines)
    parser.parse()

    seeded = sum(seed_test_case(tc, target) for tc in parser.tests)
    skipped = len(parser.tests) - seeded

    new_lines = render(parser)
    if new_lines == old_lines:
        logger.info("No changes: %s already up to date", args.test_file)
        return 0

    with open(args.test_file, "w") as f:
        f.write("\n".join(new_lines) + "\n")
    logger.info(
        "Updated %s: seeded %d test(s) for %d.%d, %d left unchanged",
        args.test_file,
        seeded,
        target[0],
        target[1],
        skipped,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
