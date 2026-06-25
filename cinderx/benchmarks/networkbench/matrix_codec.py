from typing import TypedDict


Matrix = list[list[int]]


class ReachabilityRequest(TypedDict):
    source: int
    destination: int
    graph: Matrix


MAGIC = b"NBM1"
UINT32_BYTES = 4
HEADER_BYTES = len(MAGIC) + UINT32_BYTES * 3
MAX_UINT32 = (1 << 32) - 1


def _append_uint32(encoded: bytearray, value: int) -> None:
    if not isinstance(value, int):
        raise ValueError("integer field is not an int")
    if value < 0 or value > MAX_UINT32:
        raise ValueError("integer field is out of range")

    encoded.append((value >> 24) & 0xFF)
    encoded.append((value >> 16) & 0xFF)
    encoded.append((value >> 8) & 0xFF)
    encoded.append(value & 0xFF)


def _read_uint32(encoded: bytes, offset: int) -> int:
    return (
        (encoded[offset] << 24)
        | (encoded[offset + 1] << 16)
        | (encoded[offset + 2] << 8)
        | encoded[offset + 3]
    )


def _validate_square_matrix(matrix: Matrix) -> int:
    size = len(matrix)
    for row in matrix:
        if len(row) != size:
            raise ValueError("matrix is not square")
    return size


def encode_square_matrix(matrix: Matrix) -> bytes:
    size = _validate_square_matrix(matrix)
    encoded = bytearray(size * size)
    offset = 0
    for row in matrix:
        for value in row:
            if value != 0 and value != 1:
                raise ValueError("matrix values must be 0 or 1")
            encoded[offset] = value
            offset += 1
    return bytes(encoded)


def decode_square_matrix(encoded: bytes, size: int) -> Matrix:
    expected_length = size * size
    if len(encoded) != expected_length:
        raise ValueError("encoded matrix has the wrong length")

    matrix = []
    offset = 0
    for _ in range(size):
        next_offset = offset + size
        matrix.append(list(encoded[offset:next_offset]))
        offset = next_offset
    return matrix


def encode_reachability_request(
    matrix: Matrix,
    source: int,
    destination: int,
) -> bytes:
    size = _validate_square_matrix(matrix)
    encoded = bytearray()
    encoded.extend(MAGIC)
    _append_uint32(encoded, size)
    _append_uint32(encoded, source)
    _append_uint32(encoded, destination)
    encoded.extend(encode_square_matrix(matrix))
    return bytes(encoded)


def decode_reachability_request(encoded: bytes) -> ReachabilityRequest:
    if len(encoded) < HEADER_BYTES:
        raise ValueError("request body is too short")
    if encoded[: len(MAGIC)] != MAGIC:
        raise ValueError("invalid request body magic")

    size = _read_uint32(encoded, len(MAGIC))
    source = _read_uint32(encoded, len(MAGIC) + UINT32_BYTES)
    destination = _read_uint32(encoded, len(MAGIC) + UINT32_BYTES * 2)
    matrix = decode_square_matrix(encoded[HEADER_BYTES:], size)
    return {
        "source": source,
        "destination": destination,
        "graph": matrix,
    }
