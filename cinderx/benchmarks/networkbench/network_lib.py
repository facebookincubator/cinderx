from very_simple_queue import VerySimpleQueue


def are_reachable(
        graph: list[list[int]],
        source: int,
        destination: int,
) -> tuple[bool, list[int]]:
    queue = VerySimpleQueue()
    explored = [False for _ in range(len(graph))]
    reached_from = [-1 for _ in range(len(graph))]
    explored[source] = True
    queue.put(source)
    while not queue.empty():
        v = queue.get()
        if v == destination:
            return True, produce_path(reached_from, source, destination)
        for w, reachable in enumerate(graph[v]):
            if not reachable:
                continue
            if not explored[w]:
                explored[w] = True
                queue.put(w)
                assert reached_from[w] == -1
                reached_from[w] = v
    return False, []


def produce_path(
        reached_from: list[int],
        source: int,
        destination: int,
) -> list[int]:
    reverse_path = [destination]
    while (latest := reverse_path[-1]) != source:
        reverse_path.append(reached_from[latest])
    assert reverse_path[-1] == source
    return list(reversed(reverse_path))
