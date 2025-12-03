#!/usr/bin/env python3
# (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

"""
Generic bisection tool to find minimal subset of items that cause a command to fail.

Given:
- A list of M items (from a text file)
- A shell command to test

This script finds the minimal subset N of items that causes the command to exit with
a non-zero status code.

Throughout tests the order of items, relative to the full input list, is preserved.
"""

import argparse
import logging
import os
import subprocess
import sys
import tempfile


logger = logging.getLogger("bisect")


def config_logger(verbose: bool) -> None:
    formatter = logging.Formatter(
        fmt="[bisect] %(asctime)s: %(message)s", datefmt="%Y-%m-%d %H:%M:%S"
    )
    handler = logging.StreamHandler(sys.stderr)
    handler.setFormatter(formatter)
    logger.addHandler(handler)
    logger.setLevel(logging.DEBUG if verbose else logging.INFO)


class BisectRunner:
    def __init__(
        self,
        command: str,
        items: list[str],
    ):
        """
        Initialize the bisect runner.

        Args:
            command: Shell command to execute. The temp file path is available as $BISECT_FILE.
            items: List of items to bisect
        """
        self.command = command
        self.all_items = items
        self.test_count = 0

    def test_items(self, items: list[str]) -> bool:
        """
        Test if the given items cause the command to fail.

        Args:
            items: List of items to test

        Returns:
            True if command fails (non-zero exit), False if succeeds
        """
        self.test_count += 1

        # Create temporary file with items
        with tempfile.NamedTemporaryFile(
            mode="w", suffix=".txt", prefix="bisect_", delete=False
        ) as f:
            f.write("\n".join(items) + "\n")
            items_file = f.name

        try:
            # Set up environment with BISECT_FILE
            env = os.environ.copy()
            env["BISECT_FILE"] = items_file

            logger.debug(f"Test #{self.test_count}: Testing {len(items)} items")
            logger.debug(f"  Command: {self.command}")
            logger.debug(f"  Items: {items}")

            result = subprocess.run(
                self.command,
                shell=True,
                capture_output=True,
                env=env,
                check=False,
            )

            fails = result.returncode != 0
            logger.debug(
                f"  Result: {'FAIL' if fails else 'PASS'} (exit code: {result.returncode})"
            )
            return fails
        finally:
            # Clean up temp file
            try:
                os.unlink(items_file)
            except OSError:
                pass

    def _to_ordered_list(self, items_set: set[str]) -> list[str]:
        """
        Convert a set of items to a list in their original order from all_items.

        Args:
            items_set: Set of items to convert

        Returns:
            Items in their original order
        """
        return [item for item in self.all_items if item in items_set]

    def bisect_recursive(
        self, items_set: set[str], known_minimal_set: set[str], indent: str = ""
    ) -> set[str]:
        """
        Recursively bisect to find minimal failing set.

        Args:
            items_set: Current set of items to bisect
            known_minimal_set: Items already known to be necessary
            indent: Indentation string for logging nested recursion

        Returns:
            Set of minimal items needed to cause failure
        """
        logger.info(
            f"{indent}Bisecting {len(items_set)} items with {len(known_minimal_set)} known minimal"
        )

        while len(items_set) > 1:
            logger.info(
                f"{indent}Testing with {len(known_minimal_set) + len(items_set)} candidates"
            )

            # Split items in half (preserving order within each half for consistent behavior)
            items_list = self._to_ordered_list(items_set)
            mid = len(items_list) // 2
            left_set = set(items_list[:mid])
            right_set = set(items_list[mid:])

            # Test if left half alone (with known_minimal) causes failure
            test_set = known_minimal_set | left_set
            if self.test_items(self._to_ordered_list(test_set)):
                items_set = left_set
                continue

            # Test if right half alone (with known_minimal) causes failure
            test_set = known_minimal_set | right_set
            if self.test_items(self._to_ordered_list(test_set)):
                items_set = right_set
                continue

            # We need something from both halves to trigger the failure.
            # Bisect each half independently with known_minimal, then combine.
            minimal_right_set = self.bisect_recursive(
                right_set, known_minimal_set | left_set, indent + "< "
            )
            # Extract only the items from right_set
            right_items_set = minimal_right_set & right_set

            minimal_left_set = self.bisect_recursive(
                left_set, known_minimal_set | right_items_set, indent + "> "
            )
            # Extract only the items from left_set
            left_items_set = minimal_left_set & left_set

            return known_minimal_set | left_items_set | right_items_set

        return known_minimal_set | items_set

    def find_minimal_failing_set(self) -> list[str] | None:
        """
        Find the minimal set of items that cause the command to fail.

        Returns:
            List of minimal items in original order, or None if command doesn't fail with all items
        """
        # First check if the full set fails
        logger.info(f"Testing with all {len(self.all_items)} items...")
        if not self.test_items(self.all_items):
            logger.info("Command succeeds with all items - nothing to bisect!")
            return None

        logger.info("Testing with no items...")
        if self.test_items([]):
            logger.info("Command fails with no items - nothing to bisect!")
            return None

        logger.info("Starting bisection...")
        logger.info("=" * 60)

        minimal_set = self.bisect_recursive(set(self.all_items), set())

        logger.info("=" * 60)
        logger.info(f"Bisection complete after {self.test_count} tests")

        # Convert set back to ordered list
        return self._to_ordered_list(minimal_set)


def read_items_file(file_path: str) -> list[str]:
    """Read items from a text file, one per line."""
    with open(file_path, "r") as f:
        # Strip whitespace and filter out empty lines
        items = [line.strip() for line in f if line.strip()]
    return items


def main():
    parser = argparse.ArgumentParser(
        description="Find minimal subset of items that cause a command to fail",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Basic usage - temp file path available as $BISECT_FILE
  %(prog)s -c "grep -q bad $BISECT_FILE && exit 1 || exit 0" -i items.txt

  # Testing with a Python script
  %(prog)s -c "python test.py --input $BISECT_FILE" -i functions.txt

  # With verbose output
  %(prog)s -c "make test CONFIG=$BISECT_FILE" -i configs.txt -v
        """,
    )

    parser.add_argument(
        "-c",
        "--command",
        required=True,
        help="Shell command to execute. Temp file path available as $BISECT_FILE",
    )

    parser.add_argument(
        "-i",
        "--input",
        required=True,
        help="Input file containing list of items (one per line)",
    )

    parser.add_argument(
        "-o", "--output", help="Output file for minimal failing set (default: stdout)"
    )

    parser.add_argument(
        "-v",
        "--verbose",
        action="store_true",
        help="Enable verbose output",
    )

    args = parser.parse_args()

    config_logger(args.verbose)

    # Read input items
    items = read_items_file(args.input)
    logger.info(f"Loaded {len(items)} items from {args.input}")
    if not items:
        logger.error("No items found in input file!")
        return 1

    # Run bisection
    runner = BisectRunner(
        command=args.command,
        items=items,
    )

    minimal_items = runner.find_minimal_failing_set()

    if minimal_items is None:
        return 1

    # Output results
    logger.info(f"\nMinimal failing set ({len(minimal_items)} items):")

    output_text = "\n".join(minimal_items) + "\n"

    if args.output:
        with open(args.output, "w") as f:
            f.write(output_text)
        logger.info(f"Results written to {args.output}")
    else:
        print(output_text, end="")

    return 0


if __name__ == "__main__":
    sys.exit(main())
