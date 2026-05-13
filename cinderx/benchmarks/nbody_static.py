import sys

from cinderx.compiler.strict import loader as static_python_loader

static_python_loader.install()

from nbody_static_lib import bench_nbody, DEFAULT_ITERATIONS, DEFAULT_REFERENCE

if __name__ == "__main__":
    num_iterations = 5
    if len(sys.argv) > 1:
        num_iterations = int(sys.argv[1])
    bench_nbody(num_iterations, DEFAULT_REFERENCE, DEFAULT_ITERATIONS)
