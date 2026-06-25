FROM ubuntu:24.04

SHELL ["/bin/bash", "-exo", "pipefail", "-c"]

ARG DEBIAN_FRONTEND=noninteractive

WORKDIR /root

ENV CINDER_ROOT=/root/cinder
ENV CINDERX_ROOT=/root/cinderx
ENV CINDERX_VENV=/root/cinderx/.venv-mp312
ENV CINDERX_BUNDLE_ARCHIVE=/root/cinderx/scratch/cinderx-bundled/libcinderx-bundled.a
ENV NETWORKBENCH_JITLIST=/root/cinderx/cinderx/benchmarks/networkbench/networkbench.jitlist.txt
ENV LLVM_VERSION=21
ENV LLVM_ROOT=/usr/lib/llvm-$LLVM_VERSION
ENV PATH=$LLVM_ROOT/bin:$PATH
ENV PROFILE_TASK="$CINDERX_ROOT/cinderx/benchmarks/networkbench/run_server_client.py 1000"

# Install the packages that used to be baked into ubuntu/cpython-build-benchmark.
RUN apt-get update; \
    apt-get install -yq --no-install-recommends \
        build-essential \
        ca-certificates \
        cmake \
        gdb \
        git \
        gnupg \
        linux-tools-generic \
        libbz2-dev \
        libffi-dev \
        libgdbm-compat-dev \
        libgdbm-dev \
        liblzma-dev \
        libncurses5-dev \
        libreadline-dev \
        libsqlite3-dev \
        libssl-dev \
        libzstd-dev \
        lsb-release \
        pkg-config \
        python3 \
        python3-dev \
        python3-full \
        python3-venv \
        software-properties-common \
        tk-dev \
        uuid-dev \
        wget \
        xz-utils \
        zlib1g-dev; \
    wget -O /tmp/llvm.sh https://apt.llvm.org/llvm.sh; \
    chmod +x /tmp/llvm.sh; \
    /tmp/llvm.sh "$LLVM_VERSION" all; \
    apt-get install -yq --no-install-recommends \
        "bolt-$LLVM_VERSION" \
        "libbolt-$LLVM_VERSION-dev"; \
    LLVM_BOLT="$(command -v llvm-bolt || command -v "llvm-bolt-$LLVM_VERSION")"; \
    MERGE_FDATA="$(command -v merge-fdata || command -v "merge-fdata-$LLVM_VERSION")"; \
    LLVM_PROFDATA="$(command -v llvm-profdata || command -v "llvm-profdata-$LLVM_VERSION")"; \
    PERF="$(command -v perf || find /usr/lib/linux-tools-* -type f -name perf -print -quit)"; \
    test -n "$PERF"; \
    ln -sf "$LLVM_BOLT" /usr/local/bin/llvm-bolt; \
    ln -sf "$MERGE_FDATA" /usr/local/bin/merge-fdata; \
    ln -sf "$LLVM_PROFDATA" /usr/local/bin/llvm-profdata; \
    ln -sf "$PERF" /usr/local/bin/perf; \
    command -v cmake; \
    command -v perf; \
    command -v llvm-bolt; \
    command -v merge-fdata; \
    command -v llvm-profdata; \
    command -v llvm-ar; \
    command -v llvm-ranlib; \
    llvm-bolt --version; \
    llvm-profdata --version; \
    rm -rf /var/lib/apt/lists/* /tmp/llvm.sh

RUN git clone -b meta/3.12 https://github.com/facebookincubator/cinder.git

WORKDIR $CINDER_ROOT

# Build a bootstrap interpreter first. CinderX needs a working meta/3.12 Python
# to run its own PGO build before we can link those optimized objects into the
# final CPython executable.
RUN CC=clang CXX=clang++ ./configure --prefix="$CINDER_ROOT/mp312"
RUN make -j
RUN make install

WORKDIR /root

WORKDIR $CINDERX_ROOT

COPY . "$CINDERX_ROOT/"

RUN test -f "$NETWORKBENCH_JITLIST"

RUN "$CINDER_ROOT/mp312/bin/python3" -m venv "$CINDERX_VENV"

RUN "$CINDERX_VENV/bin/python" -m pip install --upgrade pip setuptools

# Install CinderX with its PGO flow enabled. The local checkout supplies the
# profile-task override, so the PGO workload is networkbench instead of the
# default CPython test-suite task.
RUN PYTHONJITLISTFILE="$NETWORKBENCH_JITLIST" \
    CINDERX_ENABLE_PGO=1 \
    CINDERX_ENABLE_LTO=1 \
    CINDERX_PGO_PROFILE_TASK="$PROFILE_TASK" \
    CC=clang \
    CXX=clang++ \
    "$CINDERX_VENV/bin/pip" -vvv install --ignore-requires-python --no-build-isolation --no-clean .

# CMake's setuptools build produces a shared _cinderx extension. For the final
# benchmark image, fold the PGO/LTO object files from that build into one static
# archive and link it as a CPython built-in module in the final optimized
# interpreter executable.
RUN CINDERX_BUILD_DIR="$(find "$CINDERX_ROOT/scratch" -maxdepth 1 -type d -name 'temp.*' -print -quit)"; \
    test -n "$CINDERX_BUILD_DIR"; \
    mkdir -p "$(dirname "$CINDERX_BUNDLE_ARCHIVE")"; \
    find "$CINDERX_BUILD_DIR" -path '*/CMakeFiles/*.dir/*' -name '*.o' -print0 \
      | sort -z \
      | xargs -0 llvm-ar rcs "$CINDERX_BUNDLE_ARCHIVE"; \
    llvm-ranlib "$CINDERX_BUNDLE_ARCHIVE"; \
    llvm-ar t "$CINDERX_BUNDLE_ARCHIVE" | grep '_cinderx.cpp.o' > /dev/null

WORKDIR $CINDER_ROOT

RUN make distclean

RUN printf '%s\n' \
      '*static*' \
      "_cinderx -Wl,--whole-archive $CINDERX_BUNDLE_ARCHIVE -Wl,--no-whole-archive -lstdc++ -lz -ldl -lpthread -lm" \
      > Modules/Setup.local; \
    cat Modules/Setup.local

# The final binary includes CinderX, so keep BOLT's core layout passes while
# avoiding LLVM 21 AArch64 apply passes that crash or exceed the VM memory limit.
RUN CC=clang \
    CXX=clang++ \
    LDFLAGS="-fuse-ld=lld -flto" \
    BOLT_APPLY_FLAGS="-skip-funcs=_PyEval_EvalFrameDefault,sre_ucs1_match/1,sre_ucs2_match/1,sre_ucs4_match/1 -reorder-blocks=ext-tsp -reorder-functions=cdsort -split-functions -reorder-functions-use-hot-size -peepholes=none -use-gnu-stack" \
    ./configure --prefix="$CINDER_ROOT/mp312" --enable-optimizations --enable-bolt
RUN PYTHONPATH="$CINDERX_ROOT/cinderx/PythonLib" \
    PYTHONJITLISTFILE="$NETWORKBENCH_JITLIST" \
    make -j
RUN make install

WORKDIR $CINDERX_ROOT

RUN find "$CINDERX_VENV" -name '_cinderx*.so' -delete; \
    "$CINDERX_VENV/bin/python" -c 'import importlib.util; spec = importlib.util.find_spec("_cinderx"); assert spec is not None and spec.origin == "built-in", spec; import cinderx; assert cinderx.is_initialized(), cinderx.get_import_error()'

ENTRYPOINT ["bash", "-c", "source $CINDERX_VENV/bin/activate && python cinderx/benchmarks/networkbench/run_bench.py -n 1"]
