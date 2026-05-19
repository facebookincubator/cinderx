import asyncio
from collections.abc import Sequence
import json
import multiprocessing
import os
import signal
import socket
import sys
import time

try:
    import cinderx.jit
    HAS_CINDERX = cinderx.jit.is_enabled()
except ImportError:
    HAS_CINDERX = False

import config
import network_lib
from simple_web_framework import (
    BaseRoute,
    BaseViewMiddleware,
    Connection,
    HTTPServer,
)
from traffic_stats import WebTrafficStats
from views import (
    NetworkGetRoute,
    NetworkPostRoute,
    ReachabilityRoute,
    StatusRoute,
    TimingMiddleware,
)


WEB_TRAFFIC_STATS: WebTrafficStats | None = None


def log_ignored_exception(exc: BaseException) -> None:
    print(f"pid={os.getpid()} ignoring {exc.__class__.__qualname__}: {exc}")


def print_worker_stats() -> None:
    stats = WEB_TRAFFIC_STATS
    if stats is None:
        return
    snapshot = json.dumps(stats.snapshot(), sort_keys=True)
    print(f"pid={os.getpid()} stats: {snapshot}", flush=True)


def force_compile_networkbench_helpers() -> None:
    assert HAS_CINDERX
    if not cinderx.jit.is_enabled() or cinderx.jit.get_jit_list():
        return

    for func in (
        TimingMiddleware.after_response.__func__,
        BaseRoute.prepare.__func__,
        BaseViewMiddleware.after_response.__func__,
        StatusRoute.handle.__func__,
        NetworkPostRoute.prepare.__func__,
        NetworkPostRoute.handle.__func__,
        NetworkGetRoute.prepare.__func__,
        NetworkGetRoute.handle.__func__,
        Connection.send_error,
        WebTrafficStats.record_request,
        WebTrafficStats.snapshot,
        WebTrafficStats.status_codes_snapshot,
        network_lib.are_reachable,
        network_lib.produce_path,
    ):
        cinderx.jit.force_compile(func)


def install_worker_shutdown_handlers() -> None:
    def shutdown_signal(_signum, _frame):
        try:
            loop = asyncio.get_event_loop()
        except RuntimeError as exc:
            log_ignored_exception(exc)
            return
        loop.stop()
        raise SystemExit(0)

    signal.signal(signal.SIGTERM, shutdown_signal)
    signal.signal(signal.SIGINT, shutdown_signal)


def create_server_socket() -> socket.socket:
    server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server_socket.bind((config.HOST, config.PORT))
    server_socket.listen()
    return server_socket


def serve_worker(server_socket: socket.socket) -> None:
    global WEB_TRAFFIC_STATS
    WEB_TRAFFIC_STATS = WebTrafficStats()
    server = HTTPServer(
        server_socket,
        (StatusRoute, NetworkPostRoute, NetworkGetRoute, ReachabilityRoute),
        WEB_TRAFFIC_STATS,
    )
    install_worker_shutdown_handlers()
    try:
        asyncio.run(server.serve_forever())
    except RuntimeError as exc:
        log_ignored_exception(exc)
    finally:
        print_worker_stats()
        server_socket.close()


def start_server_processes(
    server_socket: socket.socket,
) -> list[multiprocessing.Process]:
    processes = [
        multiprocessing.Process(target=serve_worker, args=(server_socket,))
        for _ in range(config.SERVER_PROCESS_COUNT)
    ]
    for process in processes:
        process.start()
    return processes


def stop_server_processes(processes: Sequence[multiprocessing.Process]) -> None:
    for process in processes:
        if process.is_alive():
            process.terminate()
    for process in processes:
        process.join()


def install_parent_shutdown_handlers(
    processes: Sequence[multiprocessing.Process],
) -> None:
    def shutdown_signal(_signum, _frame):
        stop_server_processes(processes)
        print("Bye!")
        sys.exit(0)

    signal.signal(signal.SIGTERM, shutdown_signal)
    signal.signal(signal.SIGINT, shutdown_signal)


def wait_for_server_processes(processes: Sequence[multiprocessing.Process]) -> None:
    while any(process.is_alive() for process in processes):
        time.sleep(0.5)


def main() -> None:
    if HAS_CINDERX:
        if cinderx.jit.get_jit_list():
            cinderx.jit.precompile_all()
            cinderx.jit.disable()
            try:
                cinderx.enable_parallel_gc()
            except RuntimeError:
                print("Could not enable parallel gc")
                pass
            print(f"{cinderx.get_parallel_gc_settings()=}")
        else:
            cinderx.jit.auto()
            force_compile_networkbench_helpers()
    multiprocessing.set_start_method("fork")
    server_socket = create_server_socket()
    processes = start_server_processes(server_socket)
    server_socket.close()
    install_parent_shutdown_handlers(processes)
    try:
        wait_for_server_processes(processes)
    finally:
        stop_server_processes(processes)
    print("Bye!")


if __name__ == "__main__":
    main()
