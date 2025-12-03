#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.

import argparse
import logging
import os
import re
import shlex
import subprocess
import sys

from generic_bisect import BisectRunner, config_logger, logger

JITLIST_FILENAME = "jitlist.txt"


def write_jitlist(jitlist):
    with open(JITLIST_FILENAME, "w") as file:
        for func in jitlist:
            print(func, file=file)


def read_jitlist(jit_list_file):
    with open(jit_list_file) as file:
        return [line.strip() for line in file.readlines()]


COMPILED_FUNC_RE = re.compile(r" -- (Compiling|Inlining function) ([^ ]+)($| into)")


def get_compiled_funcs(command):
    environ = dict(os.environ)
    environ.update({"PYTHONJITDEBUG": "1"})

    logger.info("Generating initial jit-list")
    proc = subprocess.run(
        command,
        env=environ,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        encoding=sys.stderr.encoding,
    )
    if proc.returncode == 0:
        sys.exit("Command succeeded during jit-list generation")

    funcs = set()
    for line in proc.stderr.splitlines():
        m = COMPILED_FUNC_RE.search(line)
        if m is None:
            continue
        funcs.add(m[2])
    if len(funcs) == 0:
        sys.exit("No compiled functions found")
    # We want a deterministic jitlist, unaffected by the order functions happen
    # to be compiled in.
    return sorted(funcs)


def run_bisect(command, jit_list_file):
    if len(command) == 0:
        sys.exit("No command specified")

    prev_arg = ""
    for arg in command:
        if arg.startswith("-Xjit-log-file") or (
            prev_arg == "-X" and arg.startswith("jit-log-file")
        ):
            sys.exit(
                "Your command includes -X jit-log-file, which is incompatible "
                "with this script. Please remove it and try again."
            )
        prev_arg = arg

    if jit_list_file is None:
        jitlist = get_compiled_funcs(command)
    else:
        jitlist = read_jitlist(jit_list_file)

    # Build shell command that sets environment variables and runs the original command
    # The $BISECT_FILE will be used as PYTHONJITLISTFILE
    escaped_command = " ".join(shlex.quote(arg) for arg in command)
    shell_command = (
        f'PYTHONJITLISTFILE="$BISECT_FILE" IS_BISECTING_JITLIST=1 {escaped_command}'
    )

    logger.info("Verifying jit-list")

    # Create BisectRunner
    runner = BisectRunner(command=shell_command, items=jitlist)

    # find_minimal_failing_set does the verification and bisection
    minimal_jitlist = runner.find_minimal_failing_set()

    if minimal_jitlist is None:
        sys.exit("Bisection failed - see error messages above")

    write_jitlist(minimal_jitlist)
    logger.info(
        f"Bisect finished with {len(minimal_jitlist)} functions in {JITLIST_FILENAME}"
    )


def parse_args():
    parser = argparse.ArgumentParser(
        description="When given a command that fails with the jit enabled (including -X jit as appropriate), bisects to find a minimal jit-list that preserves the failure"
    )
    parser.add_argument(
        "--verbose", "-v", action="store_true", help="Enable verbose logging"
    )
    parser.add_argument(
        "--initial-jit-list-file",
        help="initial jitlist file (default: auto-detect the initial jit list)",
        default=None,
    )
    parser.add_argument("command", nargs=argparse.REMAINDER)

    return parser.parse_args()


def main():
    args = parse_args()
    config_logger(args.verbose)
    run_bisect(args.command, args.initial_jit_list_file)


if __name__ == "__main__":
    sys.exit(main())
