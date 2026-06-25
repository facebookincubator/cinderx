from __future__ import annotations

import __static__

import asyncio
from http import HTTPStatus
import json
import os
import socket
import sys
import time
import traceback
from collections.abc import Callable, Mapping
from typing import Any, ClassVar, TypeVar
import cinderx.jit


_F = TypeVar("_F", bound=Callable[..., Any])

HEADER_ENCODING = "iso-8859-1"


class Request:
    __weakref__: Any

    def __init__(
        self,
        method: str,
        path: str,
        version: str,
        headers: dict[str, str],
        raw_request_line: str,
        header_size: int,
        body_size: int,
    ) -> None:
        self.method = method
        self.path = path
        self.version = version
        self.headers = headers
        self.raw_request_line = raw_request_line
        self.header_size = header_size
        self.body_size = body_size


class HeaderNames:
    BENCHMARK = "X-Networkbench"
    CACHE_CONTROL = "Cache-Control"
    CONTENT_LENGTH = "Content-Length"
    CONTENT_TYPE = "Content-Type"
    CONNECTION = "Connection"
    PAYLOAD_KEYS = "X-Payload-Keys"
    REQUEST_BYTES = "X-Request-Bytes"
    RESPONSE_KEYS = "X-Response-Keys"
    ROUTE = "X-Route"
    VIEW_STACK = "X-View-Stack"
    REQUEST_CONTENT_LENGTH = "content-length"
    REQUEST_CONTENT_TYPE = "content-type"


class RequestContext:
    def __init__(
        self,
        connection: Connection,
        route_type: type[BaseRoute],
        traffic_stats: Any | None = None,
    ) -> None:
        self.connection = connection
        self.route_type = route_type
        self.traffic_stats = traffic_stats
        self.payload: object = route_type.no_payload
        self.response: object | None = None
        self.response_headers: dict[str, str] = {}
        self.metadata: dict[str, object] = {}
        self.started_ns: int = 0


class BaseViewMiddleware:
    enabled: ClassVar[bool] = True
    metadata_key: ClassVar[str] = ""
    response_header: ClassVar[str] = ""

    @classmethod
    async def before_request(cls, context: RequestContext) -> bool:
        return cls.enabled

    @classmethod
    async def after_response(cls, context: RequestContext) -> object | None:
        return context.response


class ViewStack:
    @classmethod
    async def before_view(cls, context: RequestContext) -> bool:
        for middleware_type in context.route_type.view_stack:
            if not await middleware_type.before_request(context):
                return False
        return True

    @classmethod
    async def after_view(cls, context: RequestContext) -> object | None:
        for middleware_type in reversed(context.route_type.view_stack):
            response = await middleware_type.after_response(context)
            if response is None:
                return None
            context.response = response
        return context.response


class BaseRoute:
    method: ClassVar[str] = "GET"
    route_name: ClassVar[str] = "base"
    view_stack_name: ClassVar[str] = "base"
    benchmark_name: ClassVar[str] = "networkbench"
    cache_control: ClassVar[str] = "no-store"
    extra_response_headers: ClassVar[tuple[tuple[str, str], ...]] = ()
    view_stack: ClassVar[tuple[type[BaseViewMiddleware], ...]] = ()
    status_code: ClassVar[int] = 200
    response_content_type: ClassVar[str] = "application/json"
    error_content_type: ClassVar[str] = "text/plain"
    no_payload: ClassVar[object] = object()

    @classmethod
    async def prepare(cls, connection: Connection) -> object:
        return cls.no_payload

    @classmethod
    async def handle(cls, connection: Connection, payload: object) -> object:
        return {}

    @classmethod
    async def finalize_response(cls, context: RequestContext) -> None:
        encoded = await cls.encode_response(context.connection, context.response)
        headers = await cls.response_headers(context.connection, encoded, context)
        context.connection.send_response(cls.status_code, encoded, headers)

    @classmethod
    async def encode_response(cls, connection: Connection, response: object) -> bytes:
        return json.dumps(response).encode()

    @classmethod
    async def response_headers(
        cls,
        connection: Connection,
        encoded: bytes,
        context: RequestContext,
    ) -> dict[str, str]:
        headers = {
            HeaderNames.BENCHMARK: cls.benchmark_name,
            HeaderNames.CACHE_CONTROL: cls.cache_control,
            HeaderNames.CONTENT_TYPE: cls.response_content_type,
            HeaderNames.ROUTE: cls.route_name,
        }
        headers.update(context.response_headers)
        for name, value in cls.extra_response_headers:
            headers[name] = value
        return headers


class Connection(asyncio.Protocol):
    NEED_MORE_DATA = object()

    def __init__(
        self,
        routes: tuple[type[BaseRoute], ...] = (),
        traffic_stats: Any | None = None,
    ) -> None:
        self.routes: tuple[type[BaseRoute], ...] = routes
        self.transport: asyncio.Transport | None = None
        self.peername: Any | None = None
        self.buffer: bytearray = bytearray()
        self.request: Request | None = None
        self.handle_task: asyncio.Task[None] | None = None
        self.response_sent: bool = False
        self.traffic_stats = traffic_stats

    def connection_made(self, transport: asyncio.Transport) -> None:
        self.transport = transport
        self.peername = transport.get_extra_info("peername")

    def data_received(self, data: bytes) -> None:
        if self.response_sent:
            return
        self.buffer.extend(data)
        handle_task = self.handle_task
        if handle_task is None or handle_task.done():
            self.handle_task = asyncio.create_task(self.handle())

    async def handle(self) -> None:
        try:
            if self.request is None:
                request, error = await self.read_request()
                self.request = request
                if self.request is None:
                    if error is not None:
                        self.send_error(400, error)
                    return

            await self.dispatch_request()
        except Exception:
            traceback.print_exc()
            if not self.response_sent:
                self.send_error(500)

    async def dispatch_request(self) -> None:
        route_type = self.find_route()
        if route_type is None:
            self.send_error(404)
            return

        payload = await route_type.prepare(self)
        if payload is self.NEED_MORE_DATA:
            return
        if payload is None:
            return

        context = RequestContext(self, route_type, self.traffic_stats)
        context.payload = payload
        if not await ViewStack.before_view(context):
            return

        response = await route_type.handle(self, payload)
        if response is None:
            return
        context.response = response
        response = await ViewStack.after_view(context)
        if response is None:
            return
        await route_type.finalize_response(context)

    def find_route(self) -> type[BaseRoute] | None:
        request = self.request
        if request is None:
            return None
        for route_type in self.routes:
            if request.method == route_type.method and request.path == route_type.path:
                return route_type
        return None

    async def read_request(self) -> tuple[Request | None, str | None]:
        header_end = self.buffer.find(b"\r\n\r\n")
        separator_length = 4
        if header_end < 0:
            header_end = self.buffer.find(b"\n\n")
            separator_length = 2
        if header_end < 0:
            return None, None

        request_header_size = header_end + separator_length
        encoded_headers = bytes(self.buffer[:header_end])
        del self.buffer[: header_end + separator_length]
        try:
            decoded_headers = encoded_headers.decode(HEADER_ENCODING)
        except UnicodeDecodeError as exc:
            return None, str(exc)

        lines = decoded_headers.splitlines()
        if not lines:
            return None, "Bad request syntax"

        decoded_request_line = lines[0]
        parts = decoded_request_line.split()
        if len(parts) != 3:
            return None, "Bad request syntax"

        headers: dict[str, str] = {}
        for line in lines[1:]:
            if not line:
                continue
            if ":" not in line:
                return None, "Bad header syntax"
            name, value = line.split(":", 1)
            headers[name.strip().lower()] = value.strip()

        request_body_size = 0
        content_length = headers.get(HeaderNames.REQUEST_CONTENT_LENGTH)
        if content_length is not None:
            try:
                request_body_size = int(content_length)
            except ValueError:
                request_body_size = 0
            if request_body_size < 0:
                request_body_size = 0

        return (
            Request(
                parts[0],
                parts[1],
                parts[2],
                headers,
                decoded_request_line,
                request_header_size,
                request_body_size,
            ),
            None,
        )

    def send_response(
        self,
        status_code: int | HTTPStatus,
        body: bytes = b"",
        headers: Mapping[str, str] | None = None,
    ) -> None:
        status = HTTPStatus(status_code)
        response_headers = {
            HeaderNames.CONTENT_LENGTH: str(len(body)),
            HeaderNames.CONNECTION: "close",
        }
        if headers is not None:
            response_headers.update(headers)

        header_chunks = [
            f"HTTP/1.1 {status.value} {status.phrase}\r\n".encode("ascii")
        ]
        for name, value in response_headers.items():
            header_chunks.append(f"{name}: {value}\r\n".encode("ascii"))
        header_chunks.append(b"\r\n")

        encoded_headers = b"".join(header_chunks)
        transport = self.transport
        assert transport is not None
        transport.write(encoded_headers)
        transport.write(body)
        stats = self.traffic_stats
        if stats is not None:
            request_header_size = 0
            request_body_size = 0
            if self.request is not None:
                request: Request = self.request
                request_header_size = request.header_size
                request_body_size = request.body_size
            stats.record_request(request_header_size, request_body_size)
            stats.record_response(status.value, len(encoded_headers), len(body))
        self.log_request(status.value, len(body))
        self.response_sent = True
        transport.close()

    def send_error(
        self,
        status_code: int,
        message: str | None = None,
        route_type: type[BaseRoute] = BaseRoute,
    ) -> None:
        status = HTTPStatus(status_code)
        _message: str = ""
        if message is None:
            _message = status.phrase
        else:
            _message = message
        self.send_response(
            status,
            _message.encode(),
            {HeaderNames.CONTENT_TYPE: route_type.error_content_type},
        )

    def send_json_response(
        self,
        response: object,
        route_type: type[BaseRoute] = BaseRoute,
    ) -> None:
        encoded = json.dumps(response).encode()
        self.send_response(
            route_type.status_code,
            encoded,
            {HeaderNames.CONTENT_TYPE: route_type.response_content_type},
        )

    @cinderx.jit.jit_suppress
    def address_string(self) -> str:
        if not self.peername:
            return "-"
        return self.peername[0]

    @cinderx.jit.jit_suppress
    def log_date_time_string(self) -> str:
        return time.strftime("%d/%b/%Y %H:%M:%S", time.localtime())

    @cinderx.jit.jit_suppress
    def log_request(self, status_code: int, response_size: int | str = "-") -> None:
        if self.request is None:
            request_line = ""
        else:
            request_line = self.request.raw_request_line
        sys.stderr.write(
            "%s - pid=%s - [%s] \"%s\" %s %s\n"
            % (
                self.address_string(),
                os.getpid(),
                self.log_date_time_string(),
                request_line,
                status_code,
                response_size,
            )
        )


class HTTPServer:
    def __init__(
        self,
        server_socket: socket.socket,
        routes: tuple[type[BaseRoute], ...] = (),
        traffic_stats: Any | None = None,
    ) -> None:
        self.server_socket = server_socket
        self.routes = routes
        self.traffic_stats = traffic_stats
        self.server: asyncio.Server | None = None

    def create_connection(self) -> Connection:
        return Connection(self.routes, self.traffic_stats)

    async def serve_forever(self) -> None:
        self.server_socket.setblocking(False)
        loop = asyncio.get_running_loop()
        self.server = await loop.create_server(
            self.create_connection,
            sock=self.server_socket,
        )
        print(f"pid={os.getpid()} Server listening at {self.server.sockets[0].getsockname()}")
        async with self.server:
            await self.server.serve_forever()
