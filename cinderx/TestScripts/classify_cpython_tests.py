#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.

"""Partitions the upstream CPython test suite into Buck-runnable tiers.

TPX lists a target's tests and then re-invokes each one by its dotted id via
``loadTestsFromName``.  That works only for tests reachable as module
attributes, and it bypasses a module's ``load_tests`` hook.  Modules that fall
foul of either have to keep using the regrtest-based runner
(``cinder_test_runner.py``) instead.

Run this under the CinderX interpreter for the version being classified, e.g.

    buck run fbcode//cinderx:python3.14 -- \\
        cinderx/TestScripts/classify_cpython_tests.py --version 3.14

See ``Internal/docs/buckified-cpython-tests.md`` for the full rationale.
"""

from __future__ import annotations

import argparse
import ast
import importlib
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.loader import _FailedTest

from skip_list_support import iter_tests

# Tests whose subject matter is incompatible with running inside a
# python_unittest, independent of how they are discovered.
#
# The bytecode-introspection ones fail because importing `cinderx.compiler`
# registers CinderX's opcode numbers in the stdlib `opcode.hasarg`; in 3.14's
# renumbering those collide with CPython's argument-less opcodes, so `dis`
# renders a spurious oparg column.  That registration is load-bearing for
# Static Python (it is what lets `dis` decode CinderX bytecode), so it cannot
# simply be removed.
CINDERX_INCOMPATIBLE = {
    "test.test_dis": "dis output differs under CinderX opcode registration",
    "test.test_peepholer": "dis output differs under CinderX opcode registration",
    "test.test_dtrace": "dis output differs under CinderX opcode registration",
}

# Tests of the test framework itself, which assume they own the runner.
FRAMEWORK_TESTS = {
    "test.test_regrtest": "exercises regrtest, which Tier 1 does not use",
    "test.test_support": "exercises the regrtest support library",
    "test.test_unittest": "exercises unittest itself; conflicts with the TPX adapter",
}


def _addressable(test_id: str) -> bool:
    """Can TPX re-run this test by dotted name?"""
    try:
        suite = unittest.TestLoader().loadTestsFromName(test_id)
    except Exception:
        return False
    return not any(isinstance(t, _FailedTest) for t in iter_tests(suite))


def _curated_reason(modname: str) -> str | None:
    return CINDERX_INCOMPATIBLE.get(modname) or FRAMEWORK_TESTS.get(modname)


def _unaddressable_reason(tests: list[unittest.TestCase]) -> str | None:
    for test in tests:
        try:
            test_id = test.id()
        except Exception:
            return "test without a usable id()"
        if not _addressable(test_id):
            return f"unaddressable test: {test_id}"
    return None


def classify(modname: str) -> dict[str, object]:
    reason = _curated_reason(modname)
    if reason:
        return {"module": modname, "tier": 2, "reason": reason}

    try:
        module = importlib.import_module(modname)
    except unittest.SkipTest as e:
        # Platform-gated modules; the regrtest runner skips these too.
        return {"module": modname, "tier": 0, "reason": f"SkipTest at import: {e}"}
    except Exception as e:
        return {
            "module": modname,
            "tier": 2,
            "reason": f"{type(e).__name__} at import: {e}",
        }

    if hasattr(module, "load_tests"):
        # Per-test re-invocation never calls load_tests, so any state it sets up
        # (test_io injects its C/Python class attributes there) would be missing.
        return {"module": modname, "tier": 2, "reason": "defines load_tests"}

    try:
        tests = list(iter_tests(unittest.TestLoader().loadTestsFromModule(module)))
    except Exception as e:
        return {
            "module": modname,
            "tier": 2,
            "reason": f"{type(e).__name__} while loading: {e}",
        }

    reason = _unaddressable_reason(tests)
    if reason:
        return {"module": modname, "tier": 2, "reason": reason}

    return {"module": modname, "tier": 1, "total": len(tests)}


def read_test_modules(tests_bzl: Path, version: str) -> list[str]:
    """Pull the `test.*` entries for `version` out of tests.bzl."""
    tree = ast.parse(tests_bzl.read_text())
    for node in tree.body:
        if not isinstance(node, ast.Assign):
            continue
        for target in node.targets:
            if isinstance(target, ast.Name) and target.id == "TESTS":
                tests = ast.literal_eval(node.value)
                if version not in tests:
                    raise SystemExit(f"no TESTS entry for version {version}")
                return [t for t in tests[version] if t.startswith("test.")]
    raise SystemExit(f"no TESTS assignment found in {tests_bzl}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--version",
        required=True,
        help="CinderX version id, e.g. 3.14 or 3.14t. Results are keyed by this, "
        "because a free-threaded build can classify differently from its GIL twin.",
    )
    parser.add_argument(
        "--tests-bzl",
        type=Path,
        default=Path(__file__).resolve().parent.parent / "tests.bzl",
    )
    parser.add_argument("--output", type=Path, help="Write JSON here instead of stdout")
    args = parser.parse_args()

    # Resolve before chdir, so relative paths mean what the caller expects.
    output = args.output.resolve() if args.output else None

    # tests.bzl keys TESTS by interpreter version, which the "t" builds share.
    modules = read_test_modules(args.tests_bzl.resolve(), args.version.rstrip("t"))

    results = []
    # Importing test modules drops scratch files (@test_<pid>_tmp...) into the
    # working directory, so classify from a throwaway one rather than the repo.
    with tempfile.TemporaryDirectory(prefix="classify-cpython-tests-") as tmp:
        os.chdir(tmp)
        for i, modname in enumerate(modules, 1):
            record = classify(modname)
            results.append(record)
            print(
                f"[{i}/{len(modules)}] tier{record['tier']} {modname}", file=sys.stderr
            )

    payload = {"version": args.version, "modules": results}
    text = json.dumps(payload, indent=2, sort_keys=True)
    if output:
        output.write_text(text + "\n")
    else:
        print(text)


if __name__ == "__main__":
    main()
