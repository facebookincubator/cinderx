import argparse
from collections.abc import Sequence
import subprocess
import sys
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
SERVER_SCRIPT = SCRIPT_DIR / "server.py"
CLIENT_SCRIPT = SCRIPT_DIR / "client.py"
SERVER_SHUTDOWN_TIMEOUT = 5


def start_script(script: Path, *args: str) -> subprocess.Popen[bytes]:
    return subprocess.Popen(
        [sys.executable, str(script), *args],
        cwd=SCRIPT_DIR,
    )


def stop_process(process: subprocess.Popen[bytes]) -> None:
    if process.poll() is not None:
        return

    process.terminate()
    try:
        process.wait(timeout=SERVER_SHUTDOWN_TIMEOUT)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()


def run_client_with_server(request_count: int) -> int:
    server_process = start_script(SERVER_SCRIPT)
    try:
        client_process = start_script(CLIENT_SCRIPT, str(request_count))
        return client_process.wait()
    finally:
        stop_process(server_process)


def positive_int(value: str) -> int:
    parsed = int(value)
    if parsed < 1:
        raise argparse.ArgumentTypeError("must be greater than 0")
    return parsed


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "request_count",
        nargs="?",
        type=positive_int,
        default=1,
        help="number of benchmark requests for the client to send",
    )
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    try:
        args = parse_args(argv)
        return run_client_with_server(args.request_count)
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
