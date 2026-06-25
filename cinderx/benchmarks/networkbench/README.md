# networkbench

`networkbench` is a small benchmark meant to reproduce the shape of a Python
webserver. It exercises request parsing, route dispatch, middleware, async I/O,
filesystem-backed state, response encoding, and CPU work inside request handlers.
(File I/O through async thread executors substitutes for database query I/O.)

The benchmark runs a local HTTP server on `localhost:8080` and drives it with an
async client. The client uploads network matrices, then sends concurrent requests
against two main endpoints:

- `GET /network` reads a stored matrix and returns it.
- `GET /reachable` decodes a graph payload and computes whether two nodes are
  reachable.


Performance is measured in the client as number of requests/second, after the
upload phase.

## Running

Run one server/client benchmark pass:

```bash
cd cinderx/benchmarks/networkbench
python run_server_client.py 10000
```

The numeric argument is the number of client requests. The client prints
`Average requests per second` after the run.

Run the comparison harness:

```bash
python run_bench.py 10000 -n 5
```

`run_bench.py` compares:

- `cinderx_jitlist`: runs with `networkbench.jitlist.txt`.
- `cinderx_disable`: runs with `CINDERX_DISABLE=1`.

## Useful Knobs

These environment variables tune the workload:

- `SERVER_PROCESS_COUNT`: number of worker processes, default `8`.
- `CLIENT_MAX_INFLIGHT_REQUESTS`: client concurrency limit, default `16`.
- `NETWORK_MATRIX_COUNT`: matrices uploaded before the timed run, default `16`.
- `NETWORK_GET_PERCENT`: percentage of timed requests sent to `/network`,
  default `90`.
- `NETWORK_STORAGE_DIR`: directory used for stored matrix files.

## JIT List

Regenerate the JIT list from a debug run with:

```bash
python generate_networkbench_jitlist.py 10000
```

This rewrites `networkbench.jitlist.txt` by default.
