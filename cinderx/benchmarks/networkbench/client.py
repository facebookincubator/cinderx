import argparse
import asyncio
from collections.abc import Mapping, Sequence
import random
import time
from typing import TypeAlias, TypedDict

import config
import network_data
import matrix_codec


HEADER_ENCODING = "iso-8859-1"
HTTPHeaderValue: TypeAlias = str | int
HTTPHeaders: TypeAlias = Mapping[str, HTTPHeaderValue]
HTTPResponse: TypeAlias = tuple[int, bytes]


class ReachabilityPayload(TypedDict):
    source: int
    destination: int
    graph: list[list[int]]


def encode_http_request(
    method: str,
    path: str,
    headers: HTTPHeaders,
    body: bytes = b"",
) -> bytes:
    encoded_headers = [
        f"{method} {path} HTTP/1.1\r\n",
        f"Host: {config.HOST}:{config.PORT}\r\n",
        "Connection: close\r\n",
    ]
    for name, value in headers.items():
        encoded_headers.append(f"{name}: {value}\r\n")
    encoded_headers.append("\r\n")
    return "".join(encoded_headers).encode("ascii") + body


async def read_http_response(reader: asyncio.StreamReader) -> HTTPResponse:
    status_line = await reader.readline()
    if not status_line:
        raise RuntimeError("Empty response")

    parts = status_line.decode(HEADER_ENCODING).split(" ", 2)
    if len(parts) < 2:
        raise RuntimeError("Invalid response status line")
    status = int(parts[1])

    headers: dict[str, str] = {}
    while True:
        line = await reader.readline()
        if line in (b"\r\n", b"\n", b""):
            break
        name, value = line.decode(HEADER_ENCODING).split(":", 1)
        headers[name.strip().lower()] = value.strip()

    content_length = headers.get("content-length")
    if content_length is None:
        body = await reader.read()
    else:
        body = await reader.readexactly(int(content_length))
    return status, body


async def make_http_request(
    method: str,
    path: str,
    headers: HTTPHeaders | None = None,
    body: bytes = b"",
) -> HTTPResponse:
    reader, writer = await asyncio.open_connection(config.HOST, config.PORT)
    try:
        writer.write(encode_http_request(method, path, headers or {}, body))
        await writer.drain()
        return await read_http_response(reader)
    finally:
        writer.close()
        await writer.wait_closed()


async def wait_for_server() -> None:
    while True:
        try:
            status, _body = await make_http_request("GET", config.STATUS_PATH)
        except Exception:
            await asyncio.sleep(0.01)
            continue
        if status == 200:
            return


def make_reachability_payload() -> ReachabilityPayload:
    return {
        "source": network_data.SOURCE_NODE,
        "destination": network_data.DESTINATION_NODE,
        "graph": network_data.REACHABILITY_MATRIX,
    }


def encode_reachability_body(payload: ReachabilityPayload) -> bytes:
    return matrix_codec.encode_reachability_request(
        payload["graph"],
        payload["source"],
        payload["destination"],
    )


_cached_reachability_body = encode_reachability_body(make_reachability_payload())


def reachability_headers(body: bytes) -> dict[str, HTTPHeaderValue]:
    return {
        "Content-Type": config.REACHABILITY_CONTENT_TYPE,
        "Content-Length": len(body),
    }


def network_post_headers(
    network_id: int,
    matrix_size: int,
    body: bytes,
) -> dict[str, HTTPHeaderValue]:
    return {
        "Content-Type": config.NETWORK_CONTENT_TYPE,
        "Content-Length": len(body),
        "X-Network-Id": str(network_id),
        "X-Network-Size": str(matrix_size),
    }


def network_get_headers(network_id: int) -> dict[str, HTTPHeaderValue]:
    return {"X-Network-Id": str(network_id)}


async def get_reachability_response() -> HTTPResponse:
    body = _cached_reachability_body
    return await make_http_request(
        "GET",
        config.REACHABLE_PATH,
        headers=reachability_headers(body),
        body=body,
    )


async def post_network_matrix(network_id: int) -> None:
    matrix = network_data.build_reachability_matrix()
    body = matrix_codec.encode_square_matrix(matrix)
    status, _body = await make_http_request(
        "POST",
        config.NETWORK_PATH,
        headers=network_post_headers(network_id, len(matrix), body),
        body=body,
    )
    if status != 200:
        raise RuntimeError(f"POST /network failed: {status}")


async def upload_network_matrices() -> None:
    for network_id in range(config.NETWORK_MATRIX_COUNT):
        await post_network_matrix(network_id)


async def make_requests(request_count: int) -> None:
    semaphore = asyncio.Semaphore(config.CLIENT_MAX_INFLIGHT_REQUESTS)

    async def make_bounded_request(request_index: int) -> None:
        async with semaphore:
            await make_request(request_index)

    await asyncio.gather(
        *(make_bounded_request(request_index) for request_index in range(request_count))
    )


async def make_request(request_index: int) -> None:
    if is_network_get_request(request_index):
        await make_network_request(request_index)
    else:
        await make_reachability_request()


def is_network_get_request(_request_index: int) -> bool:
    return random.randrange(100) < config.NETWORK_GET_PERCENT


async def make_network_request(request_index: int) -> None:
    network_id = request_index % config.NETWORK_MATRIX_COUNT
    status, _body = await make_http_request(
        "GET",
        config.NETWORK_PATH,
        headers=network_get_headers(network_id),
    )
    if status != 200:
        raise RuntimeError(f"GET /network failed: {status}")


async def make_reachability_request() -> None:
    status, _body = await get_reachability_response()
    if status != 200:
        raise RuntimeError(f"GET /reachable failed: {status}")


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
        help="number of benchmark requests to send",
    )
    return parser.parse_args(argv)


async def run_benchmark(args: argparse.Namespace) -> None:
    await wait_for_server()
    await upload_network_matrices()
    started = time.perf_counter()
    await make_requests(args.request_count)
    elapsed = time.perf_counter() - started
    print(f"Average requests per second: {args.request_count / elapsed:.2f}")


def main(argv: Sequence[str] | None = None) -> None:
    args = parse_args(argv)
    asyncio.run(run_benchmark(args))


if __name__ == "__main__":
    main()
