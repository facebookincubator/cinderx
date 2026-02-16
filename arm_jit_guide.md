# ARM64 (aarch64) JIT Bring-up Guide (CinderX)

This is a pragmatic, reproducible workflow to get CinderX JIT running on ARM, then keep it working while syncing upstream.

## 0. Pick A Repo Layout (Recommended)

Do upstream sync in a *git* clone, and treat the ARM host as the place that validates builds/benchmarks.

- Upstream clone (git): `d:\code\cinderx-upstream-20260213`
- Work branch: `arm-jit`
- ARM host build dir: `/root/work/cinderx-main` (or `/root/work/cinderx-upstream`)

If you only have a non-git snapshot (`d:\code\cinderx-main`), it is fine for local browsing, but it is not a good long-term base for syncing.

## 1. ARM Host Prereqs (Generic)

You need:

- a working CPython on ARM (e.g. 3.14)
- compiler toolchain (`clang` or `gcc/g++`)
- `cmake`
- build tools (`make` or `ninja`)
- `git` (for syncing upstream on ARM)
- enough RAM/swap to build a wheel

Low-memory ARM machines: add swap and build with `-j 1`.

## 2. Build A Wheel On ARM (Baseline)

In your ARM build directory:

```bash
cd /root/work/cinderx-main
/opt/python-3.14/bin/python3.14 -m build --wheel
```

If you want to constrain parallelism:

```bash
export CMAKE_BUILD_PARALLEL_LEVEL=1
```

## 3. Install The Wheel Into A Venv

```bash
python -m venv /root/venv-cinderx
. /root/venv-cinderx/bin/activate
python -m pip install -U pip
python -m pip install --force-reinstall /root/work/cinderx-main/dist/cinderx-*-linux_aarch64.whl
```

## 4. Smoke Tests (Before Benchmarks)

1. Import works:

```bash
PYTHONJIT=0 python -c "import cinderx; print('import-ok')"
```

2. JIT init works (should not crash):

```bash
PYTHONJIT=1 PYTHONJITAUTO=0 python -c "print('jit-init-ok')"
```

3. jitlist test (compile just one target; safest early step):

```bash
cat >/tmp/jitlist.txt <<'EOF'
__main__:*
EOF
PYTHONJIT=1 PYTHONJITLISTFILE=/tmp/jitlist.txt PYTHONJITENABLEJITLISTWILDCARDS=1 \
  python -c "def f(n):\n  s=0\n  for i in range(n): s+=i\n  return s\nprint(f(100000))"
```

## 5. Run pyperformance (Start With richards)

Install pyperformance in the *driver* venv and pass through env vars:

```bash
. /root/venv-cinderx/bin/activate
python -m pip install pyperformance
PYTHONJIT=1 PYTHONJITLISTFILE=/tmp/jitlist.txt PYTHONJITENABLEJITLISTWILDCARDS=1 \
  python -m pyperformance run --debug-single-value -b richards \
    --inherit-environ PYTHONJIT,PYTHONJITLISTFILE,PYTHONJITENABLEJITLISTWILDCARDS \
    -o /root/work/richards_jitlist_debug.json
```

Then try auto-jit:

```bash
PYTHONJIT=1 PYTHONJITAUTO=50 \
  python -m pyperformance run --debug-single-value -b richards \
    --inherit-environ PYTHONJIT,PYTHONJITAUTO \
    -o /root/work/richards_autojit50_debug.json
```

## 6. If It Segfaults: Fast Triage Loop

1. Capture JIT log:

```bash
PYTHONJITDEBUG=1 PYTHONJITLOGFILE=/tmp/jit.log ...
```

2. Run under gdb:

```bash
env PYTHONJIT=1 PYTHONJITAUTO=50 gdb -q --args python <script> <args>
```

3. Reduce to a minimal repro:

- Use `PYTHONJITLISTFILE` to compile just one function.
- Shrink the script until it crashes in <1s.

This makes ARM bring-up tractable: "fix one crash, re-run richards".

## 7. Periodic Upstream Sync (Two Options)

### Option A (Recommended): Sync On Windows (git) -> copy to ARM

- Fetch/rebase in `d:\code\cinderx-upstream-20260213`
- Create a `git archive` tarball and `scp` it to ARM
- On ARM: rsync into the non-git working tree (preserve `scratch/`), rebuild wheel, run smoke + `pyperformance richards`

One-command driver:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\push_to_arm.ps1 -RepoPath d:\code\cinderx-upstream-20260213
```

### Option B: Sync Directly On ARM (git clone on ARM)

- `git clone` upstream on ARM
- keep branch `arm-jit`
- cron/systemd timer runs: fetch -> rebase -> build -> smoke -> richards

See:

- `scripts/sync_upstream.ps1` (Windows: fetch + ff main + rebase `arm-jit`)
- `scripts/push_to_arm.ps1` (Windows: sync + archive + deploy + gate on ARM)
- `scripts/arm/remote_update_build_test.sh` (ARM: apply tar + build + smoke + gates)
