MAX_STATUS_CODE = 600


class WebTrafficStats:
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
        self.status_code_counts = [0] * MAX_STATUS_CODE

    def record_request(self, header_size: int, body_size: int) -> None:
        self.request_count += 1
        self.total_request_header_bytes += header_size
        self.total_request_body_bytes += body_size

    def record_response(
        self, status_code: int, header_size: int, body_size: int
    ) -> None:
        self.total_header_bytes += header_size
        self.total_body_bytes += body_size
        self.total_response_bytes += header_size + body_size

        if 0 <= status_code < MAX_STATUS_CODE:
            self.status_code_counts[status_code] += 1

        if 100 <= status_code < 200:
            self.informational_count += 1
        elif 200 <= status_code < 300:
            self.success_count += 1
        elif 300 <= status_code < 400:
            self.redirect_count += 1
        elif 400 <= status_code < 500:
            self.client_error_count += 1
        elif 500 <= status_code < 600:
            self.server_error_count += 1
        else:
            self.other_status_count += 1

    def record_request_elapsed(self, elapsed_ns: int) -> None:
        self.timed_request_count += 1
        self.total_request_ns += elapsed_ns
        if elapsed_ns > self.max_request_ns:
            self.max_request_ns = elapsed_ns

    def snapshot(self) -> dict[str, object]:
        return {
            "requests": {
                "count": self.request_count,
                "header_bytes": self.total_request_header_bytes,
                "body_bytes": self.total_request_body_bytes,
            },
            "responses": {
                "status_classes": {
                    "1xx": self.informational_count,
                    "2xx": self.success_count,
                    "3xx": self.redirect_count,
                    "4xx": self.client_error_count,
                    "5xx": self.server_error_count,
                    "other": self.other_status_count,
                },
                "status_codes": self.status_codes_snapshot(),
                "header_bytes": self.total_header_bytes,
                "body_bytes": self.total_body_bytes,
                "total_bytes": self.total_response_bytes,
            },
            "request_timing": {
                "count": self.timed_request_count,
                "total_ns": self.total_request_ns,
                "max_ns": self.max_request_ns,
            },
        }

    def status_codes_snapshot(self) -> dict[str, int]:
        return {
            str(status_code): count
            for status_code, count in enumerate(self.status_code_counts)
            if count
        }
