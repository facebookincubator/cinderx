#!/usr/bin/env bash
set -euo pipefail

# This script runs on the ARM host. It expects:
# - ${INCOMING_DIR}/cinderx-update.tar uploaded from Windows (git archive output)
# - It will rsync the extracted sources into $WORKDIR (preserving scratch/, dist/)
# - It will build a wheel, install it into the driver venv, set up pyperformance venv
# - It will run smoke tests + pyperformance gates to catch crashes early

INCOMING_DIR="${INCOMING_DIR:-/root/work/incoming}"
WORKDIR="${WORKDIR:-/root/work/cinderx-main}"
PY="${PYTHON:-/opt/python-3.14/bin/python3.14}"
DRIVER_VENV="${DRIVER_VENV:-/root/venv-cinderx314}"
BENCH="${BENCH:-richards}"
AUTOJIT="${AUTOJIT:-50}"
PARALLEL="${PARALLEL:-1}"
SKIP_PYPERF="${SKIP_PYPERF:-0}"
RECREATE_PYPERF_VENV="${RECREATE_PYPERF_VENV:-0}"

mkdir -p "$WORKDIR" "$INCOMING_DIR" /root/work/arm-sync

RUN_ID="$(date +%Y%m%d_%H%M%S)"

echo ">> staging extract"
stage="$(mktemp -d /root/work/cinderx-stage.XXXXXX)"
tar -xf "$INCOMING_DIR/cinderx-update.tar" -C "$stage"

echo ">> rsync sources into $WORKDIR (preserve scratch/, dist/)"
rsync -a --delete --exclude scratch --exclude dist "$stage/cinderx-src/" "$WORKDIR/"
rm -rf "$stage" "$INCOMING_DIR/cinderx-update.tar"

cd "$WORKDIR"
export CMAKE_BUILD_PARALLEL_LEVEL="$PARALLEL"

echo ">> build wheel (CMAKE_BUILD_PARALLEL_LEVEL=$CMAKE_BUILD_PARALLEL_LEVEL)"
"$PY" -m build --wheel
WHEEL="$(ls -1t dist/cinderx-*.whl | head -n 1)"
echo "wheel=$WHEEL"

if [[ ! -d "$DRIVER_VENV" ]]; then
  echo ">> create driver venv $DRIVER_VENV"
  "$PY" -m venv "$DRIVER_VENV"
fi

echo ">> install wheel + pyperformance into driver venv"
. "$DRIVER_VENV/bin/activate"
PYTHONJIT=0 python -m pip install -q -U pip
PYTHONJIT=0 python -m pip install -q --force-reinstall "$WHEEL"
PYTHONJIT=0 python -m pip install -q -U pyperformance

echo ">> smoke: JIT is effective (compiled code executes, not just 'enabled')"
# We verify effectiveness by:
# 1) Run a function in interpreted mode and observe interpreted call count increases.
# 2) Force-compile it and observe the interpreted call count stops increasing while the function still runs.
env PYTHONJITAUTO=1000000 python - <<'PY'
import cinderx
import cinderx.jit as jit

assert cinderx.is_initialized()
jit.enable()

# Ensure our first calls are interpreted (avoid auto-jit during the interpreted phase).
jit.compile_after_n_calls(1000000)


def f(n: int) -> int:
    s = 0
    for i in range(n):
        s += i
    return s


_ = jit.force_uncompile(f)
assert not jit.is_jit_compiled(f), "expected f() to start interpreted"

before = jit.count_interpreted_calls(f)
for _ in range(10):
    assert f(10) == 45
after = jit.count_interpreted_calls(f)
assert after > before, (before, after)

assert jit.force_compile(f), "force_compile failed"
assert jit.is_jit_compiled(f), "expected f() to be JIT compiled"
code_size = jit.get_compiled_size(f)
assert code_size > 0, code_size

interp0 = jit.count_interpreted_calls(f)
for _ in range(2000):
    assert f(10) == 45
interp1 = jit.count_interpreted_calls(f)
assert interp1 == interp0, (interp0, interp1)

print("jit-effective-ok", "compiled_size", code_size, "interp_calls", interp1)
PY
deactivate

echo ">> ensure pyperformance venv exists"
. "$DRIVER_VENV/bin/activate"
if [[ "$RECREATE_PYPERF_VENV" == "1" ]]; then
  PYTHONJIT=0 python -m pyperformance venv recreate
else
  PYTHONJIT=0 python -m pyperformance venv create
fi
PYVENV_PATH="$(python -m pyperformance venv show | sed -n 's/^Virtual environment path: \([^ ]*\).*$/\1/p')"
echo "pyperf_venv=$PYVENV_PATH"
if [[ -z "$PYVENV_PATH" || ! -d "$PYVENV_PATH" ]]; then
  echo "ERROR: failed to determine pyperformance venv path"
  python -m pyperformance venv show || true
  exit 1
fi
deactivate

echo ">> install wheel into pyperformance venv"
. "$PYVENV_PATH/bin/activate"
PYTHONJIT=0 python -m pip install -q --force-reinstall "$WHEEL"
SITEPKG="$(python -c 'import site; print(site.getsitepackages()[0])')"

cat >"$SITEPKG/sitecustomize.py" <<'PY'
# Auto-load CinderX for pyperformance benchmark subprocesses.
#
# This file lives inside the pyperformance benchmark venv. It runs at
# interpreter startup, so keep it defensive and side-effect free.
#
# We intentionally skip loading CinderX/JIT for packaging/bootstrap commands
# (ensurepip, get-pip, pip) to keep environment setup stable.

import os
import sys


def _argv_tokens():
    toks = []
    orig = getattr(sys, "orig_argv", None)
    if orig:
        toks.extend([str(x) for x in orig])
    toks.extend([str(x) for x in getattr(sys, "argv", [])])
    return toks


tokens = _argv_tokens()
argv = getattr(sys, "argv", [])
argv0 = argv[0] if argv else ""
orig_argv = getattr(sys, "orig_argv", None)


def _has_token(name: str) -> bool:
    for t in tokens:
        if t == name:
            return True
    return False


def _has_suffix(suffix: str) -> bool:
    for t in tokens:
        if t.endswith(suffix):
            return True
    return False


def _contains(substr: str) -> bool:
    for t in tokens:
        if substr in t:
            return True
    return False


skip = (
    _has_token("ensurepip")
    or _has_token("pip")
    or _has_suffix("get-pip.py")
    or argv0.endswith("get-pip.py")
    # ensurepip bootstraps pip via: python -c '... runpy.run_module("pip", ...)'
    or _contains('run_module("pip"')
    or _contains("run_module('pip'")
)

try:
    with open("/tmp/cinderx_sitecustomize.log", "a", encoding="utf-8") as f:
        f.write(
            "argv=%r orig_argv=%r tokens=%r skip=%s disable=%r auto=%r\n"
            % (
                argv,
                orig_argv,
                tokens,
                skip,
                os.environ.get("PYTHONJITDISABLE"),
                os.environ.get("PYTHONJITAUTO"),
            )
        )
except Exception:
    pass

if not skip and os.environ.get("CINDERX_DISABLE") in (None, "", "0"):
    try:
        import cinderx.jit as jit

        if os.environ.get("PYTHONJITDISABLE") in (None, "", "0"):
            jit.enable()
    except Exception:
        # Don't make interpreter startup depend on the JIT.
        pass
PY

deactivate

echo ">> smoke: JIT init + generator + regex compile"
env PYTHONJITAUTO=0 "$PYVENV_PATH/bin/python" -c 'g=(i for i in [1]); next(g, None); import re; re.compile("a+"); print("smoke-ok")'

if [[ "$SKIP_PYPERF" == "1" ]]; then
  echo "SKIP_PYPERF=1 set; done after smoke."
  exit 0
fi

echo ">> pyperformance gate (jitlist, debug-single-value)"
cat >/tmp/jitlist_gate.txt <<'EOF'
__main__:*
EOF
. "$DRIVER_VENV/bin/activate"
env PYTHONJITLISTFILE=/tmp/jitlist_gate.txt PYTHONJITENABLEJITLISTWILDCARDS=1 \
  python -m pyperformance run --debug-single-value -b "$BENCH" \
    --inherit-environ PYTHONJITLISTFILE,PYTHONJITENABLEJITLISTWILDCARDS \
    -o "/root/work/arm-sync/${BENCH}_jitlist_${RUN_ID}.json"
deactivate

echo ">> pyperformance gate (auto-jit, debug-single-value)"
. "$DRIVER_VENV/bin/activate"
LOG="/tmp/jit_${BENCH}_autojit_${RUN_ID}.log"
env PYTHONJITAUTO="$AUTOJIT" PYTHONJITDEBUG=1 PYTHONJITLOGFILE="$LOG" \
  python -m pyperformance run --debug-single-value -b "$BENCH" \
    --inherit-environ PYTHONJITAUTO,PYTHONJITDEBUG,PYTHONJITLOGFILE \
    -o "/root/work/arm-sync/${BENCH}_autojit${AUTOJIT}_${RUN_ID}.json"
deactivate

if [[ ! -s "$LOG" ]]; then
  echo "ERROR: missing/empty JIT log: $LOG"
  exit 1
fi
# Ensure the benchmark actually hit JIT compilation of benchmark code (not just stdlib imports).
if ! grep -q "Finished compiling __main__:" "$LOG"; then
  echo "ERROR: JIT did not compile any __main__ functions during '$BENCH' (JIT may not be active in benchmark workers)"
  echo "--- jit log tail ---"
  tail -n 120 "$LOG" || true
  exit 1
fi

echo "--- jit log tail ---"
tail -n 50 "$LOG" || true
