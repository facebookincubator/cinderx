#!/usr/bin/env python3

import logging
import unittest

from cinderx.TestScripts.generic_bisect import BisectRunner, logger


# Configure logger for tests - silence output during tests
logging.basicConfig(level=logging.CRITICAL)
logger.setLevel(logging.CRITICAL)


class TestBisectAlgorithm(unittest.TestCase):
    """Test the bisection algorithm with various minimal set sizes."""

    def test_zero_items_needed_command_always_succeeds(self):
        """Test when command never fails - should return None."""
        items = ["item1", "item2", "item3", "item4"]
        # Command that always succeeds
        runner = BisectRunner(command="true", items=items)
        result = runner.find_minimal_failing_set()
        self.assertIsNone(result)

    def test_zero_items_needed_command_always_fails(self):
        """Test when command fails even with empty set - should return None."""
        items = ["item1", "item2", "item3", "item4"]
        # Command that always fails
        runner = BisectRunner(command="false", items=items)
        result = runner.find_minimal_failing_set()
        self.assertIsNone(result)

    def test_one_item_needed_at_start(self):
        """Test minimal set of 1 item when it's at the beginning."""
        items = ["bad", "item2", "item3", "item4"]
        # Fails (returns 1) only if "bad" is present
        command = "bash -c 'grep -q \"^bad$\" $BISECT_FILE && exit 1 || exit 0'"
        runner = BisectRunner(command=command, items=items)
        result = runner.find_minimal_failing_set()
        self.assertEqual(result, ["bad"])

    def test_one_item_needed_at_end(self):
        """Test minimal set of 1 item when it's at the end."""
        items = ["item1", "item2", "item3", "bad"]
        # Fails (returns 1) only if "bad" is present
        command = "bash -c 'grep -q \"^bad$\" $BISECT_FILE && exit 1 || exit 0'"
        runner = BisectRunner(command=command, items=items)
        result = runner.find_minimal_failing_set()
        self.assertEqual(result, ["bad"])

    def test_one_item_needed_in_middle(self):
        """Test minimal set of 1 item when it's in the middle."""
        items = ["item1", "item2", "bad", "item4", "item5"]
        # Fails (returns 1) only if "bad" is present
        command = "bash -c 'grep -q \"^bad$\" $BISECT_FILE && exit 1 || exit 0'"
        runner = BisectRunner(command=command, items=items)
        result = runner.find_minimal_failing_set()
        self.assertEqual(result, ["bad"])

    def test_two_items_needed_both_at_start(self):
        """Test minimal set of 2 items when both are at the start."""
        items = ["bad1", "bad2", "item3", "item4"]
        # Fails (returns 1) only if both "bad1" AND "bad2" are present
        command = 'bash -c \'grep -q "^bad1$" $BISECT_FILE && grep -q "^bad2$" $BISECT_FILE && exit 1 || exit 0\''
        runner = BisectRunner(command=command, items=items)
        result = runner.find_minimal_failing_set()
        self.assertEqual(result, ["bad1", "bad2"])

    def test_two_items_needed_both_at_end(self):
        """Test minimal set of 2 items when both are at the end."""
        items = ["item1", "item2", "bad1", "bad2"]
        # Fails (returns 1) only if both "bad1" AND "bad2" are present
        command = 'bash -c \'grep -q "^bad1$" $BISECT_FILE && grep -q "^bad2$" $BISECT_FILE && exit 1 || exit 0\''
        runner = BisectRunner(command=command, items=items)
        result = runner.find_minimal_failing_set()
        self.assertEqual(result, ["bad1", "bad2"])

    def test_two_items_needed_at_opposite_ends(self):
        """Test minimal set of 2 items when they're at opposite ends."""
        items = ["bad1", "item2", "item3", "bad2"]
        # Fails (returns 1) only if both "bad1" AND "bad2" are present
        command = 'bash -c \'grep -q "^bad1$" $BISECT_FILE && grep -q "^bad2$" $BISECT_FILE && exit 1 || exit 0\''
        runner = BisectRunner(command=command, items=items)
        result = runner.find_minimal_failing_set()
        self.assertEqual(result, ["bad1", "bad2"])

    def test_two_items_needed_separated_in_middle(self):
        """Test minimal set of 2 items when they're separated in the middle."""
        items = ["item1", "bad1", "item3", "bad2", "item5"]
        # Fails (returns 1) only if both "bad1" AND "bad2" are present
        command = 'bash -c \'grep -q "^bad1$" $BISECT_FILE && grep -q "^bad2$" $BISECT_FILE && exit 1 || exit 0\''
        runner = BisectRunner(command=command, items=items)
        result = runner.find_minimal_failing_set()
        self.assertEqual(result, ["bad1", "bad2"])

    def test_two_items_needed_adjacent_in_middle(self):
        """Test minimal set of 2 items when they're adjacent in the middle."""
        items = ["item1", "bad1", "bad2", "item4"]
        # Fails (returns 1) only if both "bad1" AND "bad2" are present
        command = 'bash -c \'grep -q "^bad1$" $BISECT_FILE && grep -q "^bad2$" $BISECT_FILE && exit 1 || exit 0\''
        runner = BisectRunner(command=command, items=items)
        result = runner.find_minimal_failing_set()
        self.assertEqual(result, ["bad1", "bad2"])

    def test_two_items_needed_with_larger_list(self):
        """Test minimal set of 2 items in a larger list with 8 items."""
        items = ["item1", "item2", "bad1", "item4", "item5", "bad2", "item7", "item8"]
        # Fails (returns 1) only if both "bad1" AND "bad2" are present
        command = 'bash -c \'grep -q "^bad1$" $BISECT_FILE && grep -q "^bad2$" $BISECT_FILE && exit 1 || exit 0\''
        runner = BisectRunner(command=command, items=items)
        result = runner.find_minimal_failing_set()
        self.assertEqual(result, ["bad1", "bad2"])


class TestBisectRunnerWithTempFile(unittest.TestCase):
    """Test that the bisect runner works with temp files."""

    def test_one_item_with_temp_file(self):
        """Test minimal set of 1 item using temp file."""
        items = ["item1", "bad", "item3"]
        # Fails (returns 1) only if "bad" is present
        command = "bash -c 'grep -q \"^bad$\" $BISECT_FILE && exit 1 || exit 0'"
        runner = BisectRunner(command=command, items=items)
        result = runner.find_minimal_failing_set()
        self.assertEqual(result, ["bad"])

    def test_two_items_with_temp_file(self):
        """Test minimal set of 2 items using temp file."""
        items = ["bad1", "item2", "bad2", "item4"]
        # Fails (returns 1) only if both "bad1" AND "bad2" are present
        command = 'bash -c \'grep -q "^bad1$" $BISECT_FILE && grep -q "^bad2$" $BISECT_FILE && exit 1 || exit 0\''
        runner = BisectRunner(command=command, items=items)
        result = runner.find_minimal_failing_set()
        self.assertEqual(result, ["bad1", "bad2"])


if __name__ == "__main__":
    unittest.main()
