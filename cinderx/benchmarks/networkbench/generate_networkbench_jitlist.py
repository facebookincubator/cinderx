#!/usr/bin/env python3
import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parents[2]
RUNNER = Path("cinderx/benchmarks/networkbench/run_server_client.py")
DEFAULT_OUTPUT = Path(__file__).with_name("networkbench.jitlist.txt")
DEFAULT_REQUEST_COUNT = 10000
COMPILED_FUNC_RE = re.compile(r"^JIT: .* -- Finished compiling (\S+) in .*$")


def parse_compiled_functions(log_output: str) -> list[str]:
    funcs: set[str] = set()
    for line in log_output.splitlines():
        match = COMPILED_FUNC_RE.search(line)
        if match is not None:
            funcs.add(match.group(1))
    return sorted(funcs)


def read_jit_logs(log_dir: Path) -> str:
    chunks: list[str] = []
    for path in sorted(log_dir.glob("jit.*.log")):
        chunks.append(path.read_text(encoding="ascii", errors="replace"))
    return "".join(chunks)


def run_networkbench(python: str, request_count: int) -> str:
    env = dict(os.environ)
    command = [
        python,
        str(RUNNER),
        str(request_count),
    ]
    with tempfile.TemporaryDirectory(prefix="networkbench-jitlog-") as log_dir_str:
        log_dir = Path(log_dir_str)
        # Keep JIT logs out of the shared benchmark stdout/stderr stream.
        env.update(
            {
                "PYTHONJITDEBUG": "1",
                "CINDERX_JIT_LOG_FILE": str(log_dir / "jit.{pid}.log"),
            }
        )
        proc = subprocess.run(
            command,
            cwd=REPO_ROOT,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="ascii",
            errors="replace",
        )
        log_output = read_jit_logs(log_dir)
    if proc.returncode != 0:
        sys.stderr.write(proc.stdout)
        raise SystemExit(f"{' '.join(command)} failed with exit code {proc.returncode}")
    return log_output or proc.stdout


def write_jitlist(functions: list[str], output_path: Path) -> None:
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text("".join(f"{func}\n" for func in functions), encoding="ascii")


def positive_int(value: str) -> int:
    parsed = int(value)
    if parsed < 1:
        raise argparse.ArgumentTypeError("must be greater than 0")
    return parsed


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Regenerate the networkbench jitlist from a JIT debug run."
    )
    parser.add_argument(
        "request_count",
        nargs="?",
        default=DEFAULT_REQUEST_COUNT,
        type=positive_int,
        help=f"number of networkbench requests to run (default: {DEFAULT_REQUEST_COUNT})",
    )
    parser.add_argument(
        "--python",
        default="python",
        help="Python executable to use for the benchmark run (default: python)",
    )
    parser.add_argument(
        "-o",
        "--output",
        default=DEFAULT_OUTPUT,
        type=Path,
        help=f"jitlist file to write (default: {DEFAULT_OUTPUT})",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    log_output = run_networkbench(args.python, args.request_count)
    functions = parse_compiled_functions(log_output)
    if not functions:
        raise SystemExit("No compiled functions found in networkbench output")
    write_jitlist(functions, args.output)
    print(f"Wrote {len(functions)} functions to {args.output}")


if __name__ == "__main__":
    main()
