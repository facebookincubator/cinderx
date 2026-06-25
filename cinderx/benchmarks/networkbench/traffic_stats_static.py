import __static__
from __static__ import Array, box, int64


MAX_STATUS_CODE = 600


class WebTrafficStats:
    request_count: int64
    informational_count: int64
    success_count: int64
    redirect_count: int64
    client_error_count: int64
    server_error_count: int64
    other_status_count: int64
    total_request_header_bytes: int64
    total_request_body_bytes: int64
    total_header_bytes: int64
    total_body_bytes: int64
    total_response_bytes: int64
    timed_request_count: int64
    total_request_ns: int64
    max_request_ns: int64
    status_code_counts: Array[int64]

    def __init__(self) -> None:
        self.request_count = 0
        self.informational_count = 0
        self.success_count = 0
        self.redirect_count = 0
        self.client_error_count = 0
        self.server_error_count = 0
        self.other_status_count = 0
        self.total_request_header_bytes = 0
        self.total_request_body_bytes = 0
        self.total_header_bytes = 0
        self.total_body_bytes = 0
        self.total_response_bytes = 0
        self.timed_request_count = 0
        self.total_request_ns = 0
        self.max_request_ns = 0
        self.status_code_counts = Array[int64](MAX_STATUS_CODE)

    def record_request(self, header_size: int, body_size: int) -> None:
        headers: int64 = int64(header_size)
        body: int64 = int64(body_size)

        self.request_count += 1
        self.total_request_header_bytes += headers
        self.total_request_body_bytes += body

    def record_response(
        self, status_code: int, header_size: int, body_size: int
    ) -> None:
        code: int64 = int64(status_code)
        headers: int64 = int64(header_size)
        body: int64 = int64(body_size)

        self.total_header_bytes += headers
        self.total_body_bytes += body
        self.total_response_bytes += headers + body

        if code >= 0:
            if code < int64(MAX_STATUS_CODE):
                self.status_code_counts[code] += 1

        if code >= 100:
            if code < 200:
                self.informational_count += 1
                return
            if code < 300:
                self.success_count += 1
                return
            if code < 400:
                self.redirect_count += 1
                return
            if code < 500:
                self.client_error_count += 1
                return
            if code < 600:
                self.server_error_count += 1
                return
        self.other_status_count += 1

    def record_request_elapsed(self, elapsed_ns: int) -> None:
        elapsed: int64 = int64(elapsed_ns)
        self.timed_request_count += 1
        self.total_request_ns += elapsed
        if elapsed > self.max_request_ns:
            self.max_request_ns = elapsed

    def snapshot(self) -> dict[str, object]:
        return {
            "requests": {
                "count": box(self.request_count),
                "header_bytes": box(self.total_request_header_bytes),
                "body_bytes": box(self.total_request_body_bytes),
            },
            "responses": {
                "status_classes": {
                    "1xx": box(self.informational_count),
                    "2xx": box(self.success_count),
                    "3xx": box(self.redirect_count),
                    "4xx": box(self.client_error_count),
                    "5xx": box(self.server_error_count),
                    "other": box(self.other_status_count),
                },
                "status_codes": self.status_codes_snapshot(),
                "header_bytes": box(self.total_header_bytes),
                "body_bytes": box(self.total_body_bytes),
                "total_bytes": box(self.total_response_bytes),
            },
            "request_timing": {
                "count": box(self.timed_request_count),
                "total_ns": box(self.total_request_ns),
                "max_ns": box(self.max_request_ns),
            },
        }

    def status_codes_snapshot(self) -> dict[str, int]:
        status_codes = {}
        code: int64 = 0
        while code < int64(MAX_STATUS_CODE):
            count: int64 = self.status_code_counts[code]
            if count:
                status_codes[str(box(code))] = box(count)
            code += 1
        return status_codes
