import asyncio
import os
import time
import traceback
from typing import TypedDict

import config
import matrix_codec
import network_lib
from simple_web_framework import (
    BaseRoute,
    BaseViewMiddleware,
    Connection,
    HeaderNames,
    RequestContext,
)


class NetworkPostPayload(TypedDict):
    network_id: int
    body: bytes


class RouteMetadataMiddleware(BaseViewMiddleware):
    metadata_key = "route"

    @classmethod
    async def before_request(cls, context: RequestContext) -> bool:
        route_type = context.route_type
        context.metadata[cls.metadata_key] = route_type.route_name
        return cls.enabled

    @classmethod
    async def after_response(cls, context: RequestContext) -> object | None:
        route_type = context.route_type
        context.response_headers.update(
            {
                HeaderNames.ROUTE: str(
                    context.metadata.get(
                        cls.metadata_key,
                        route_type.route_name,
                    )
                ),
                HeaderNames.VIEW_STACK: route_type.view_stack_name,
            }
        )
        return context.response


class RequestHeadersMiddleware(BaseViewMiddleware):
    metadata_key = "request_bytes"
    default_length = "0"

    @classmethod
    async def before_request(cls, context: RequestContext) -> bool:
        request = context.connection.request
        if request is None:
            context.metadata[cls.metadata_key] = cls.default_length
            return cls.enabled

        value = request.headers.get(HeaderNames.REQUEST_CONTENT_LENGTH)
        if value is None:
            value = cls.default_length
        context.metadata[cls.metadata_key] = value
        return cls.enabled

    @classmethod
    async def after_response(cls, context: RequestContext) -> object | None:
        context.response_headers.update(
            {
                HeaderNames.REQUEST_BYTES: str(
                    context.metadata.get(
                        cls.metadata_key,
                        cls.default_length,
                    )
                )
            }
        )
        return context.response


class PayloadAuditMiddleware(BaseViewMiddleware):
    empty_size = "0"

    @classmethod
    async def after_response(cls, context: RequestContext) -> object | None:
        payload = context.payload
        if isinstance(payload, dict):
            payload_keys = str(len(payload))
        else:
            payload_keys = cls.empty_size
        context.response_headers.update({HeaderNames.PAYLOAD_KEYS: payload_keys})
        return context.response


class ResponseHeadersMiddleware(BaseViewMiddleware):
    empty_size = "0"

    @classmethod
    async def after_response(cls, context: RequestContext) -> object | None:
        route_type = context.route_type
        response = context.response
        if isinstance(response, dict):
            response_keys = str(len(response))
        else:
            response_keys = cls.empty_size
        context.response_headers.update(
            {
                HeaderNames.BENCHMARK: route_type.benchmark_name,
                HeaderNames.CACHE_CONTROL: route_type.cache_control,
                HeaderNames.CONTENT_TYPE: route_type.response_content_type,
                HeaderNames.RESPONSE_KEYS: response_keys,
            }
        )
        return response


class TimingMiddleware(BaseViewMiddleware):
    min_elapsed_ns = 0

    @classmethod
    async def before_request(cls, context: RequestContext) -> bool:
        if cls.enabled:
            context.started_ns = time.perf_counter_ns()
        return cls.enabled

    @classmethod
    async def after_response(cls, context: RequestContext) -> object | None:
        started_ns = context.started_ns
        if started_ns:
            elapsed_ns = time.perf_counter_ns() - started_ns
            if elapsed_ns < cls.min_elapsed_ns:
                elapsed_ns = cls.min_elapsed_ns
            stats = context.traffic_stats
            if stats is not None:
                stats.record_request_elapsed(elapsed_ns)
        return context.response


class NetworkbenchRoute(BaseRoute):
    route_name = "base"
    view_stack_name = "base"
    benchmark_name = "networkbench"
    cache_control = "no-store"
    extra_response_headers = ()
    view_stack = (
        TimingMiddleware,
        RouteMetadataMiddleware,
        RequestHeadersMiddleware,
        PayloadAuditMiddleware,
        ResponseHeadersMiddleware,
    )
    status_code = 200
    response_content_type = config.JSON_CONTENT_TYPE
    error_content_type = "text/plain"


class StatusRoute(NetworkbenchRoute):
    path = config.STATUS_PATH
    route_name = "status"
    view_stack_name = "status.view"

    @classmethod
    async def handle(
        cls,
        connection: Connection,
        payload: object,
    ) -> dict[str, object]:
        stats = connection.traffic_stats
        if stats is None:
            return {}
        return stats.snapshot()


class NetworkRoute(NetworkbenchRoute):
    path = config.NETWORK_PATH
    route_name = "network"
    view_stack_name = "network.view"
    request_content_type = config.NETWORK_CONTENT_TYPE
    response_content_type = config.NETWORK_CONTENT_TYPE
    max_body_bytes = config.MAX_REQUEST_BODY_BYTES
    request_network_id_header = "x-network-id"
    request_network_size_header = "x-network-size"
    response_network_id_header = "X-Network-Id"

    @classmethod
    def parse_int_header(
        cls,
        connection: Connection,
        header_name: str,
        label: str,
    ) -> int | None:
        request = connection.request
        assert request is not None
        value = request.headers.get(header_name)
        if value is None:
            connection.send_error(400, f"Missing {label}", cls)
            return None
        try:
            parsed = int(value)
        except ValueError:
            connection.send_error(400, f"Invalid {label}", cls)
            return None
        if parsed < 0:
            connection.send_error(400, f"Invalid {label}", cls)
            return None
        return parsed

    @classmethod
    def parse_network_id(cls, connection: Connection) -> int | None:
        return cls.parse_int_header(
            connection,
            cls.request_network_id_header,
            "X-Network-Id",
        )

    @classmethod
    def matrix_path(cls, network_id: int) -> str:
        return os.path.join(config.NETWORK_STORAGE_DIR, f"network-{network_id}.nbm")

    @classmethod
    def write_matrix_file(cls, network_id: int, body: bytes) -> int:
        os.makedirs(config.NETWORK_STORAGE_DIR, exist_ok=True)
        path = cls.matrix_path(network_id)
        tmp_path = f"{path}.{os.getpid()}.tmp"
        with open(tmp_path, "wb") as file:
            file.write(body)
        os.replace(tmp_path, path)
        return len(body)

    @classmethod
    def read_matrix_file(cls, network_id: int) -> bytes:
        with open(cls.matrix_path(network_id), "rb") as file:
            return file.read()

    @classmethod
    async def write_matrix(cls, network_id: int, body: bytes) -> int:
        loop = asyncio.get_running_loop()
        return await loop.run_in_executor(None, cls.write_matrix_file, network_id, body)

    @classmethod
    async def read_matrix(cls, network_id: int) -> bytes:
        loop = asyncio.get_running_loop()
        return await loop.run_in_executor(None, cls.read_matrix_file, network_id)

    @classmethod
    async def response_headers(
        cls,
        connection: Connection,
        encoded: bytes,
        context: RequestContext,
    ) -> dict[str, str]:
        headers = await super().response_headers(connection, encoded, context)
        if isinstance(context.payload, int):
            headers[cls.response_network_id_header] = str(context.payload)
        return headers


class NetworkPostRoute(NetworkRoute):
    method = "POST"
    route_name = "network-post"
    view_stack_name = "network.post.view"
    response_content_type = config.JSON_CONTENT_TYPE

    @classmethod
    async def prepare(cls, connection: Connection) -> object | None:
        network_id = cls.parse_network_id(connection)
        if network_id is None:
            return None

        request = connection.request
        assert request is not None
        try:
            length = int(
                request.headers.get(
                    HeaderNames.REQUEST_CONTENT_LENGTH,
                    "0",
                )
            )
        except ValueError:
            connection.send_error(400, "Invalid Content-Length", cls)
            return None

        matrix_size = cls.parse_int_header(
            connection,
            cls.request_network_size_header,
            "X-Network-Size",
        )
        if matrix_size is None:
            return None

        if length >= cls.max_body_bytes:
            connection.send_error(413, route_type=cls)
            return None

        if (
            request.headers.get(HeaderNames.REQUEST_CONTENT_TYPE, "")
            != cls.request_content_type
        ):
            connection.send_error(400, "Invalid Content-Type", cls)
            return None

        if len(connection.buffer) < length:
            return connection.NEED_MORE_DATA

        body = bytes(connection.buffer[:length])
        del connection.buffer[:length]
        if len(body) != matrix_size * matrix_size:
            connection.send_error(400, "Invalid matrix size", cls)
            return None
        return {"network_id": network_id, "body": body}

    @classmethod
    async def handle(
        cls,
        connection: Connection,
        payload: NetworkPostPayload,
    ) -> dict[str, int]:
        body_size = await cls.write_matrix(payload["network_id"], payload["body"])
        return {"network_id": payload["network_id"], "bytes": body_size}


class NetworkGetRoute(NetworkRoute):
    method = "GET"
    route_name = "network-get"
    view_stack_name = "network.get.view"

    @classmethod
    async def prepare(cls, connection: Connection) -> int | None:
        return cls.parse_network_id(connection)

    @classmethod
    async def handle(cls, connection: Connection, network_id: int) -> bytes | None:
        try:
            return await cls.read_matrix(network_id)
        except FileNotFoundError:
            connection.send_error(404, route_type=cls)
            return None

    @classmethod
    async def encode_response(cls, connection: Connection, response: bytes) -> bytes:
        return response


class ReachabilityRoute(NetworkbenchRoute):
    path = config.REACHABLE_PATH
    route_name = "reachability"
    view_stack_name = "reachability.view"
    request_content_type = config.REACHABILITY_CONTENT_TYPE
    max_body_bytes = config.MAX_REQUEST_BODY_BYTES

    @classmethod
    async def prepare(cls, connection: Connection) -> object | None:
        req = await cls.read_request(connection)
        if req is connection.NEED_MORE_DATA:
            return req
        if req is None:
            return None

        valid, reason = cls.validate_request(req)
        if not valid:
            connection.send_error(400, reason, cls)
            return None
        return req

    @classmethod
    async def read_request(cls, connection: Connection) -> object | None:
        request = connection.request
        assert request is not None
        try:
            length = int(
                request.headers.get(
                    HeaderNames.REQUEST_CONTENT_LENGTH,
                    "0",
                )
            )
        except ValueError:
            connection.send_error(400, "Invalid Content-Length", cls)
            return None

        if length >= cls.max_body_bytes:
            connection.send_error(413, route_type=cls)
            return None

        if (
            request.headers.get(HeaderNames.REQUEST_CONTENT_TYPE, "")
            != cls.request_content_type
        ):
            connection.send_error(400, "Invalid Content-Type", cls)
            return None

        if len(connection.buffer) < length:
            return connection.NEED_MORE_DATA

        body = bytes(connection.buffer[:length])
        del connection.buffer[:length]
        try:
            return matrix_codec.decode_reachability_request(body)
        except ValueError as exc:
            connection.send_error(400, str(exc), cls)
            return None

    @classmethod
    def validate_request(cls, req: object) -> tuple[bool, str]:
        if not isinstance(req, dict):
            return False, "not isinstance(req, dict)"
        if "source" not in req:
            return False, '"source" not in req'
        if not isinstance(req["source"], int):
            return False, "not isinstance(source, int)"
        if "destination" not in req:
            return False, '"destination" not in req'
        if not isinstance(req["destination"], int):
            return False, "not isinstance(destination, int)"
        if "graph" not in req:
            return False, '"graph" not in req'
        if not isinstance(req["graph"], list):
            return False, "not isinstance(graph, list)"
        if not req["graph"]:
            return False, "graph is empty"
        node_count = len(req["graph"])
        if req["source"] < 0 or req["source"] >= node_count:
            return False, "source is out of range"
        if req["destination"] < 0 or req["destination"] >= node_count:
            return False, "destination is out of range"
        return True, ""

    @classmethod
    async def handle(
        cls,
        connection: Connection,
        req: matrix_codec.ReachabilityRequest,
    ) -> dict[str, object] | None:
        try:
            reachable, path = network_lib.are_reachable(
                req["graph"],
                req["source"],
                req["destination"],
            )
        except Exception:
            traceback.print_exc()
            connection.send_error(500, route_type=cls)
            return None
        return {"reachable": reachable, "path": path}
