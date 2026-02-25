# Copyright (c) Meta Platforms, Inc. and affiliates.

"""
Spectral norm benchmark.

Computes the spectral norm of an infinite matrix A, defined by
A[i][j] = 1 / ((i + j) * (i + j + 1) / 2 + i + 1). This involves
repeated matrix-vector multiplications using nested loops and
function calls. Exercises compute-bound numerical work with heavy
function call overhead, list comprehensions, and zip/sum builtins.

Based on the classic "spectral-norm" benchmark from the Computer Language
Benchmarks Game.
"""

import sys

import cinderx.jit


def eval_A(i, j):
    ij = i + j
    return 1.0 / (ij * (ij + 1) // 2 + i + 1)


def eval_A_times_u(u, n):
    result = []
    for i in range(n):
        s = 0.0
        for j in range(n):
            s += eval_A(i, j) * u[j]
        result.append(s)
    return result


def eval_At_times_u(u, n):
    result = []
    for i in range(n):
        s = 0.0
        for j in range(n):
            s += eval_A(j, i) * u[j]
        result.append(s)
    return result


def eval_AtA_times_u(u, n):
    return eval_At_times_u(eval_A_times_u(u, n), n)


def spectral_norm(n):
    u = [1.0] * n
    v = [0.0] * n

    for _ in range(10):
        v = eval_AtA_times_u(u, n)
        u = eval_AtA_times_u(v, n)

    vBv = 0.0
    vv = 0.0
    for ui, vi in zip(u, v):
        vBv += ui * vi
        vv += vi * vi
    return (vBv / vv) ** 0.5


class SpectralNorm(object):
    def run(self, iterations):
        for _ in range(iterations):
            n = 1200
            result = spectral_norm(n)
            # The spectral norm converges to a known value.
            # Expected value was determined empirically for n=1200.
            if abs(result - 1.2742241500970801) > 1e-9:
                return False
        return True


if __name__ == "__main__":
    cinderx.jit.auto()

    num_iterations = 1
    if len(sys.argv) > 1:
        num_iterations = int(sys.argv[1])
    SpectralNorm().run(num_iterations)
