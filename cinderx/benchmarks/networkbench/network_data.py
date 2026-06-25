NODE_COUNT = 4_000
SOURCE_NODE = 0
DESTINATION_NODE = NODE_COUNT - 1
FORWARD_EDGE_COUNT = 200
LONG_EDGE_COUNT = 25
LONG_EDGE_STRIDE = 37


def build_reachability_matrix() -> list[list[int]]:
    matrix = []
    for source in range(NODE_COUNT):
        row = [0] * NODE_COUNT
        for offset in range(1, FORWARD_EDGE_COUNT + 1):
            row[(source + offset) % NODE_COUNT] = 1
        for offset in range(1, LONG_EDGE_COUNT + 1):
            row[(source + offset * LONG_EDGE_STRIDE) % NODE_COUNT] = 1
        row[source] = 0
        matrix.append(row)
    return matrix


REACHABILITY_MATRIX = build_reachability_matrix()
