import os
import tempfile


SERVER_PROCESS_COUNT = int(os.getenv("SERVER_PROCESS_COUNT", "8"))
CLIENT_MAX_INFLIGHT_REQUESTS = int(os.getenv("CLIENT_MAX_INFLIGHT_REQUESTS", "16"))
HOST, PORT = "localhost", 8080
STATUS_PATH = "/status"
REACHABLE_PATH = "/reachable"
NETWORK_PATH = "/network"
JSON_CONTENT_TYPE = "application/json"
REACHABILITY_CONTENT_TYPE = "application/vnd.networkbench.reachability"
NETWORK_CONTENT_TYPE = "application/vnd.networkbench.matrix"
MAX_REQUEST_BODY_BYTES = 400_000_000
NETWORK_MATRIX_COUNT = int(os.getenv("NETWORK_MATRIX_COUNT", "16"))
NETWORK_GET_PERCENT = int(os.getenv("NETWORK_GET_PERCENT", "90"))
NETWORK_STORAGE_DIR = os.getenv(
    "NETWORK_STORAGE_DIR",
    os.path.join(tempfile.gettempdir(), "networkbench-matrices"),
)
