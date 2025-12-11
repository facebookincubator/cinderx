#!/usr/bin/env python3
"""
JIT Log Stats Parser

This script processes JIT compilation log output and accumulates and formats
data output when using -X jit-dump-hir-stats.
"""

import json
import re
import sys
from collections import defaultdict
from dataclasses import dataclass
from typing import Any, Dict, Optional


@dataclass
class FunctionData:
    """Data structure to store compilation information for a function."""

    code_size: Optional[int] = None
    instructions: Dict[str, int] = None
    types: Dict[str, int] = None

    def __post_init__(self):
        if self.instructions is None:
            self.instructions = {}
        if self.types is None:
            self.types = {}


def format_type_name(type_name: str, max_prefix: int = 32, max_suffix: int = 4) -> str:
    """
    Format a type name for display by escaping newlines and truncating if too long.

    Args:
        type_name: The type name to format
        max_prefix: Maximum number of characters to show at the start (default 32)
        max_suffix: Maximum number of characters to show at the end (default 4)

    Returns:
        Formatted type name with newlines escaped and length limited
    """
    # Escape newlines for display
    escaped = type_name.replace("\n", "\\n").replace("\r", "\\r")

    # If short enough, return as-is
    max_length = max_prefix + max_suffix + 20  # 20 chars for "... X chars ..."
    if len(escaped) <= max_length:
        return escaped

    # Truncate with indicator showing total length
    prefix = escaped[:max_prefix]
    suffix = escaped[-max_suffix:]
    return f"{prefix}... {len(type_name)} chars ...{suffix}"


def parse_jit_log(
    log_file_path: str, include_code_size: bool
) -> Dict[str, FunctionData]:
    """
    Parse a JIT log file and extract compilation information for each function.

    Args:
        log_file_path: Path to the log file

    Returns:
        Dictionary mapping function names to their compilation data
    """
    # Dictionary to store function data
    function_data: Dict[str, FunctionData] = defaultdict(FunctionData)

    # Regular expression patterns
    compiling_pattern = re.compile(r"JIT: .* -- Compiling (\S+)")
    json_data_with_func_pattern = re.compile(r"JIT: .* -- Stats for (\S+): (\{.*\})")
    finished_pattern = re.compile(
        r"JIT: .* -- Finished compiling (\S+) .*code size: (\d+) bytes"
    )

    with open(log_file_path, "r", encoding="ascii", errors="replace") as file:
        current_function = None
        for line in file:
            line = line.strip()
            current_function = _process_compilation_start(
                line, compiling_pattern, function_data, current_function
            )
            if _process_json_with_function(
                line, json_data_with_func_pattern, function_data
            ):
                continue
            current_function = _process_compilation_end(
                line,
                finished_pattern,
                function_data,
                current_function,
                include_code_size,
            )

    if len(function_data) == 0:
        raise Exception(f"No compilation data found in {log_file_path}")

    return function_data


def _process_compilation_start(
    line: str,
    pattern: re.Pattern,
    function_data: Dict[str, FunctionData],
    current_function: Optional[str],
) -> Optional[str]:
    """Process a line that might indicate the start of a compilation."""
    match = pattern.search(line)
    if match:
        current_function = match.group(1)
        # Ensure the function exists in our dictionary
        if current_function not in function_data:
            function_data[current_function] = FunctionData()
    return current_function


def _process_json_with_function(
    line: str, pattern: re.Pattern, function_data: Dict[str, FunctionData]
) -> bool:
    """Process a line that might contain JSON data with a function name."""
    match = pattern.search(line)
    if match:
        func_name = match.group(1)
        try:
            json_str = match.group(2)
            data = json.loads(json_str)

            # Ensure the function exists in our dictionary
            if func_name not in function_data:
                function_data[func_name] = FunctionData()

            function_data[func_name].instructions = data.get("instructions", {})
            function_data[func_name].types = {}
            for k, v in data.get("types", {}).items():
                normalized = re.sub(r"\[.*$", "", k)
                function_data[func_name].types[normalized] = (
                    function_data[func_name].types.get(normalized, 0) + v
                )
        except json.JSONDecodeError:
            print(
                f"Warning: Failed to parse JSON data for {func_name}: {json_str}",
                file=sys.stderr,
            )
        return True
    return False


def _process_compilation_end(
    line: str,
    pattern: re.Pattern,
    function_data: Dict[str, FunctionData],
    current_function: Optional[str],
    include_code_size: bool,
) -> Optional[str]:
    """Process a line that might indicate the end of a compilation."""
    match = pattern.search(line)
    if match:
        func_name = match.group(1)
        code_size = int(match.group(2))
        function_data[func_name].code_size = code_size if include_code_size else None

        # If we've finished a function, reset current_function
        if func_name == current_function:
            return None
    return current_function


def extract_summary_data(function_data: Dict[str, FunctionData]) -> Dict[str, Any]:
    """
    Extract summary data from function data.

    Args:
        function_data: Dictionary mapping function names to their compilation data

    Returns:
        Dictionary containing summary data
    """
    # Initialize counters
    total_code_size = 0
    total_functions_called = 0
    instruction_counts = defaultdict(int)
    type_counts = defaultdict(int)
    functions_with_code_size = 0
    functions_with_calls_info = 0

    # Aggregate data
    for _, data in function_data.items():
        # Sum code sizes
        if data.code_size is not None:
            total_code_size += data.code_size
            functions_with_code_size += 1

        # Aggregate instruction counts
        for instr, count in data.instructions.items():
            instruction_counts[instr] += count

        # Aggregate type counts
        for type_name, count in data.types.items():
            type_counts[type_name] += count

    # Return summary data
    return {
        "total_functions": len(function_data),
        "total_code_size": total_code_size,
        "functions_with_code_size": functions_with_code_size,
        "total_functions_called": total_functions_called,
        "functions_with_calls_info": functions_with_calls_info,
        "instruction_counts": dict(instruction_counts),
        "type_counts": dict(type_counts),
    }


def generate_cumulative_summary(function_data: Dict[str, FunctionData]) -> None:
    """
    Generate and print a cumulative summary of all compiled functions.

    Args:
        function_data: Dictionary mapping function names to their compilation data
    """
    summary = extract_summary_data(function_data)

    # Print summary
    print("JIT Compilation Summary:")
    print(f"Total compiled functions: {summary['total_functions']}")
    print(
        f"Total code size: {summary['total_code_size']} bytes (from {summary['functions_with_code_size']} functions)"
    )

    print("\nInstruction Counts:")
    for instr, count in sorted(
        summary["instruction_counts"].items(), key=lambda x: x[1], reverse=True
    ):
        print(f"  {instr}: {count}")

    print("\nType Counts:")
    for type_name, count in sorted(
        summary["type_counts"].items(), key=lambda x: x[1], reverse=True
    ):
        print(f"  {format_type_name(type_name)}: {count}")


def compare_summaries(
    summary1: Dict[str, Any],
    summary2: Dict[str, Any],
    name1: str = "Log 1",
    name2: str = "Log 2",
) -> None:
    """
    Compare two summaries and print the differences.

    Args:
        summary1: First summary
        summary2: Second summary
        name1: Name of the first summary
        name2: Name of the second summary
    """
    print(f"Comparison: {name1} vs {name2}")

    # Compare basic metrics
    metrics = [
        ("Total compiled functions", "total_functions"),
        ("Total code size (bytes)", "total_code_size"),
    ]

    print("\nBasic Metrics:")
    print(f"{name1:<12} | {name2:<12} | {'Diff':<12} | {'Relative':<10} | {'Metric'}")
    print("-" * 85)

    for label, key in metrics:
        val1 = summary1[key]
        val2 = summary2[key]
        diff = val2 - val1
        if diff == 0:
            continue
        rel_diff = f"{(diff / val1 * 100):+.2f}%" if val1 != 0 else "N/A"
        sign = "+" if diff > 0 else ""
        print(f"{val1:<12} | {val2:<12} | {sign}{diff:<12} | {rel_diff:<10} | {label}")

    # Compare instruction counts
    print("\nInstruction Count Differences:")
    print(
        f"{name1:<12} | {name2:<12} | {'Diff':<12} | {'Relative':<10} | {'Instruction'}"
    )
    print("-" * 85)

    # Get all unique instructions
    all_instructions = set(summary1["instruction_counts"].keys()) | set(
        summary2["instruction_counts"].keys()
    )

    # Sort by absolute difference
    sorted_instructions = sorted(
        all_instructions,
        key=lambda x: abs(
            summary2["instruction_counts"].get(x, 0)
            - summary1["instruction_counts"].get(x, 0)
        ),
        reverse=True,
    )

    for instr in sorted_instructions:
        count1 = summary1["instruction_counts"].get(instr, 0)
        count2 = summary2["instruction_counts"].get(instr, 0)
        diff = count2 - count1
        if diff == 0:
            continue
        rel_diff = f"{(diff / count1 * 100):+.2f}%" if count1 != 0 else "N/A"
        sign = "+" if diff > 0 else ""
        print(
            f"{count1:<12} | {count2:<12} | {sign}{diff:<12} | {rel_diff:<10} | {instr}"
        )

    # Compare type counts
    print("\nType Count Differences:")
    print(f"{name1:<12} | {name2:<12} | {'Diff':<12} | {'Relative':<10} | {'Type'}")
    print("-" * 85)

    # Get all unique types
    all_types = set(summary1["type_counts"].keys()) | set(
        summary2["type_counts"].keys()
    )

    # Sort by absolute difference
    sorted_types = sorted(
        all_types,
        key=lambda x: abs(
            summary2["type_counts"].get(x, 0) - summary1["type_counts"].get(x, 0)
        ),
        reverse=True,
    )

    for type_name in sorted_types:
        count1 = summary1["type_counts"].get(type_name, 0)
        count2 = summary2["type_counts"].get(type_name, 0)
        diff = count2 - count1
        if diff == 0:
            continue
        rel_diff = f"{(diff / count1 * 100):+.2f}%" if count1 != 0 else "N/A"
        sign = "+" if diff > 0 else ""
        print(
            f"{count1:<12} | {count2:<12} | {sign}{diff:<12} | {rel_diff:<10} | {format_type_name(type_name)}"
        )


def compare_function_code_sizes(
    function_data1: Dict[str, FunctionData],
    function_data2: Dict[str, FunctionData],
    name1: str = "Log 1",
    name2: str = "Log 2",
) -> None:
    """
    Compare code sizes between functions and print differences.

    Args:
        function_data1: Dictionary mapping function names to their compilation data from first log
        function_data2: Dictionary mapping function names to their compilation data from second log
        name1: Name of the first log
        name2: Name of the second log
    """
    # Get all unique function names
    all_functions = set(function_data1.keys()) | set(function_data2.keys())

    # Collect functions with non-zero code size differences
    differences = []
    for func_name in all_functions:
        size1 = (
            function_data1[func_name].code_size
            if func_name in function_data1
            and function_data1[func_name].code_size is not None
            else 0
        )
        size2 = (
            function_data2[func_name].code_size
            if func_name in function_data2
            and function_data2[func_name].code_size is not None
            else 0
        )
        diff = size2 - size1
        if diff != 0:
            differences.append((func_name, size1, size2, diff))

    if not differences:
        print("\nNo per-function code size differences found.")
        return

    # Sort by absolute difference (largest first)
    differences.sort(key=lambda x: abs(x[3]), reverse=True)

    print("\nPer-Function Code Size Differences:")
    print(f"{name1:<12} | {name2:<12} | {'Diff':<12} | {'Relative':<10} | {'Function'}")
    print("-" * 85)

    for func_name, size1, size2, diff in differences:
        rel_diff = f"{(diff / size1 * 100):+.2f}%" if size1 != 0 else "N/A"
        sign = "+" if diff > 0 else ""
        print(
            f"{size1:<12} | {size2:<12} | {sign}{diff:<12} | {rel_diff:<10} | {func_name}"
        )


def compare_function_instructions_by_type(
    function_data1: Dict[str, FunctionData],
    function_data2: Dict[str, FunctionData],
    name1: str = "Log 1",
    name2: str = "Log 2",
) -> None:
    """
    For each instruction type with a global difference, list which functions
    contribute to that difference.

    Args:
        function_data1: Dictionary mapping function names to their compilation data from first log
        function_data2: Dictionary mapping function names to their compilation data from second log
        name1: Name of the first log
        name2: Name of the second log
    """
    # Get all unique function names
    all_functions = set(function_data1.keys()) | set(function_data2.keys())

    # Get all unique instruction types
    all_instructions: set[str] = set()
    for func_name in all_functions:
        if func_name in function_data1:
            all_instructions.update(function_data1[func_name].instructions.keys())
        if func_name in function_data2:
            all_instructions.update(function_data2[func_name].instructions.keys())

    # For each instruction type, find functions with differences
    instruction_to_func_diffs: Dict[str, list] = {}
    for instr in all_instructions:
        func_diffs = []
        for func_name in all_functions:
            count1 = (
                function_data1[func_name].instructions.get(instr, 0)
                if func_name in function_data1
                else 0
            )
            count2 = (
                function_data2[func_name].instructions.get(instr, 0)
                if func_name in function_data2
                else 0
            )
            diff = count2 - count1
            if diff != 0:
                func_diffs.append((func_name, count1, count2, diff))

        if func_diffs:
            # Sort by absolute difference (largest first)
            func_diffs.sort(key=lambda x: abs(x[3]), reverse=True)
            instruction_to_func_diffs[instr] = func_diffs

    if not instruction_to_func_diffs:
        print("\nNo per-instruction function differences found.")
        return

    # Sort instruction types by total absolute difference
    sorted_instructions = sorted(
        instruction_to_func_diffs.keys(),
        key=lambda i: sum(abs(d[3]) for d in instruction_to_func_diffs[i]),
        reverse=True,
    )

    print("\nPer-Instruction Function Differences:")
    for instr in sorted_instructions:
        func_diffs = instruction_to_func_diffs[instr]
        total_diff = sum(d[3] for d in func_diffs)
        sign = "+" if total_diff > 0 else ""
        print(f"\n  {instr} (total diff: {sign}{total_diff}):")
        print(f"    {name1:<10} | {name2:<10} | {'Diff':<10} | {'Function'}")
        print("    " + "-" * 70)
        for func_name, count1, count2, diff in func_diffs:
            sign = "+" if diff > 0 else ""
            print(f"    {count1:<10} | {count2:<10} | {sign}{diff:<10} | {func_name}")


def compare_function_types_by_type(
    function_data1: Dict[str, FunctionData],
    function_data2: Dict[str, FunctionData],
    name1: str = "Log 1",
    name2: str = "Log 2",
) -> None:
    """
    For each type with a global difference, list which functions
    contribute to that difference.

    Args:
        function_data1: Dictionary mapping function names to their compilation data from first log
        function_data2: Dictionary mapping function names to their compilation data from second log
        name1: Name of the first log
        name2: Name of the second log
    """
    # Get all unique function names
    all_functions = set(function_data1.keys()) | set(function_data2.keys())

    # Get all unique types
    all_types: set[str] = set()
    for func_name in all_functions:
        if func_name in function_data1:
            all_types.update(function_data1[func_name].types.keys())
        if func_name in function_data2:
            all_types.update(function_data2[func_name].types.keys())

    # For each type, find functions with differences
    type_to_func_diffs: Dict[str, list] = {}
    for type_name in all_types:
        func_diffs = []
        for func_name in all_functions:
            count1 = (
                function_data1[func_name].types.get(type_name, 0)
                if func_name in function_data1
                else 0
            )
            count2 = (
                function_data2[func_name].types.get(type_name, 0)
                if func_name in function_data2
                else 0
            )
            diff = count2 - count1
            if diff != 0:
                func_diffs.append((func_name, count1, count2, diff))

        if func_diffs:
            # Sort by absolute difference (largest first)
            func_diffs.sort(key=lambda x: abs(x[3]), reverse=True)
            type_to_func_diffs[type_name] = func_diffs

    if not type_to_func_diffs:
        print("\nNo per-type function differences found.")
        return

    # Sort types by total absolute difference
    sorted_types = sorted(
        type_to_func_diffs.keys(),
        key=lambda t: sum(abs(d[3]) for d in type_to_func_diffs[t]),
        reverse=True,
    )

    print("\nPer-Type Function Differences:")
    for type_name in sorted_types:
        func_diffs = type_to_func_diffs[type_name]
        total_diff = sum(d[3] for d in func_diffs)
        sign = "+" if total_diff > 0 else ""
        print(f"\n  {format_type_name(type_name)} (total diff: {sign}{total_diff}):")
        print(f"    {name1:<10} | {name2:<10} | {'Diff':<10} | {'Function'}")
        print("    " + "-" * 70)
        for func_name, count1, count2, diff in func_diffs:
            sign = "+" if diff > 0 else ""
            print(f"    {count1:<10} | {count2:<10} | {sign}{diff:<10} | {func_name}")


def find_functions_missing_type(
    function_data1: Dict[str, FunctionData],
    function_data2: Dict[str, FunctionData],
    type_name: str,
) -> list:
    """
    Find functions that exist in both logs but in the second log do not have the specified type.

    Args:
        function_data1: Dictionary mapping function names to their compilation data from first log
        function_data2: Dictionary mapping function names to their compilation data from second log
        type_name: The type name to check for

    Returns:
        List of function names that exist in both logs but in the second log do not have the specified type
    """
    result = []

    # Find functions that exist in both logs
    common_functions = set(function_data1.keys()) & set(function_data2.keys())

    for func_name in common_functions:
        # Check if the type exists in the first log's function data
        if type_name in function_data1[func_name].types:
            # Check if the type is missing in the second log's function data
            if type_name not in function_data2[func_name].types:
                result.append(func_name)

    return result


def print_detailed_summary(function_data: Dict[str, FunctionData]) -> None:
    """
    Print a detailed summary of the parsed function data.

    Args:
        function_data: Dictionary mapping function names to their compilation data
    """
    print(f"Found data for {len(function_data)} compiled functions:")

    for func_name, data in sorted(function_data.items()):
        print(f"\n{func_name}:")

        # Print code size (if available)
        if data.code_size is not None:
            print(f"  Code size: {data.code_size} bytes")
        else:
            print("  Code size: N/A")

        # Print instruction counts (if available)
        if data.instructions:
            print("  Instructions:")
            for instr, count in data.instructions.items():
                print(f"    {instr}: {count}")

        # Print type counts (if available)
        if data.types:
            print("  Types:")
            for type_name, count in data.types.items():
                print(f"    {format_type_name(type_name)}: {count}")


def main():
    """Main function to run the script."""
    import argparse

    parser = argparse.ArgumentParser(
        description="Parse and compare JIT compilation log files"
    )

    # Create a subparser for the different commands
    subparsers = parser.add_subparsers(dest="command", help="Command to run")

    # Single log file analysis
    analyze_parser = subparsers.add_parser("analyze", help="Analyze a single log file")
    analyze_parser.add_argument("log_file", help="Path to the log file to parse")
    analyze_parser.add_argument(
        "--detailed",
        "-d",
        action="store_true",
        help="Print detailed per-function information instead of cumulative summary",
    )

    # Compare two log files
    compare_parser = subparsers.add_parser("compare", help="Compare two log files")
    compare_parser.add_argument("log_file1", help="Path to the first log file")
    compare_parser.add_argument("log_file2", help="Path to the second log file")
    compare_parser.add_argument(
        "--name1", default="Log 1", help="Name for the first log file"
    )
    compare_parser.add_argument(
        "--name2", default="Log 2", help="Name for the second log file"
    )
    compare_parser.add_argument(
        "--per-function-details",
        action="store_true",
        help="Show per-function breakdown of instruction and type count differences",
    )
    compare_parser.add_argument(
        "--code-size",
        action="store_true",
        help="Show code-size changes (these are little non-deterministic)",
    )

    # Find functions missing a specific type
    missing_type_parser = subparsers.add_parser(
        "find_missing_type",
        help="Find functions that exist in both logs but in the second log do not have the specified type",
    )
    missing_type_parser.add_argument("log_file1", help="Path to the first log file")
    missing_type_parser.add_argument("log_file2", help="Path to the second log file")
    missing_type_parser.add_argument("type_name", help="The type name to check for")

    args = parser.parse_args()

    # Default to analyze if no command is provided
    if args.command is None:
        if len(sys.argv) > 1:
            # Assume the first argument is a log file for backward compatibility
            args.command = "analyze"
            args.log_file = sys.argv[1]
            args.detailed = "--detailed" in sys.argv or "-d" in sys.argv
        else:
            parser.print_help()
            sys.exit(1)

    try:
        if args.command == "analyze":
            function_data = parse_jit_log(args.log_file, args.code_size)
            if args.detailed:
                print_detailed_summary(function_data)
            else:
                generate_cumulative_summary(function_data)
        elif args.command == "compare":
            function_data1 = parse_jit_log(args.log_file1, args.code_size)
            function_data2 = parse_jit_log(args.log_file2, args.code_size)

            summary1 = extract_summary_data(function_data1)
            summary2 = extract_summary_data(function_data2)

            compare_summaries(summary1, summary2, args.name1, args.name2)
            if args.per_function_details:
                if args.code_size:
                    compare_function_code_sizes(
                        function_data1, function_data2, args.name1, args.name2
                    )
                compare_function_instructions_by_type(
                    function_data1, function_data2, args.name1, args.name2
                )
                compare_function_types_by_type(
                    function_data1, function_data2, args.name1, args.name2
                )
        elif args.command == "find_missing_type":
            function_data1 = parse_jit_log(args.log_file1)
            function_data2 = parse_jit_log(args.log_file2)

            missing_type_functions = find_functions_missing_type(
                function_data1, function_data2, args.type_name
            )

            if missing_type_functions:
                print(
                    f"Found {len(missing_type_functions)} functions that exist in both logs but in the second log do not have the type '{args.type_name}':"
                )
                for func_name in sorted(missing_type_functions):
                    print(f"  {func_name}")
            else:
                print(
                    f"No functions found that exist in both logs but in the second log do not have the type '{args.type_name}'."
                )
    except FileNotFoundError as e:
        print(f"Error: Log file not found: {e}", file=sys.stderr)
        sys.exit(1)
    except Exception as e:
        print(f"Error processing log file: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
