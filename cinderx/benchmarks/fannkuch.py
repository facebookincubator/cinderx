# Copyright (c) Meta Platforms, Inc. and affiliates.

"""
Fannkuch-redux benchmark.

Repeatedly generates permutations and counts "pancake flips" (reversals
of a prefix of the permutation). Exercises tight integer loops with
heavy list indexing -- every iteration does multiple perm[i] and
count[i] accesses, making this an excellent stress test for integer
unboxing and IndexUnbox performance.

Based on the classic "fannkuch-redux" benchmark from the Computer Language
Benchmarks Game.
"""

import sys

import cinderx.jit


def count_flips(perm1, perm, n):
    k = perm1[0]
    if not k:
        return 0
    flips = 1
    # Copy perm1 into perm for flipping
    for i in range(n):
        perm[i] = perm1[i]
    while True:
        # Reverse perm[0:k+1]
        i = 1
        j = k - 1
        while i < j:
            perm[i], perm[j] = perm[j], perm[i]
            i += 1
            j -= 1
        # Swap endpoints
        perm[0], perm[k] = perm[k], perm[0]
        k = perm[0]
        if not k:
            break
        flips += 1
    return flips


def fannkuch(n):
    perm = [0] * n
    perm1 = list(range(n))
    count = [0] * n
    max_flips = 0
    checksum = 0
    sign = 1
    m = n - 1

    while True:
        flips = count_flips(perm1, perm, n)
        if flips > max_flips:
            max_flips = flips
        checksum += flips * sign

        # Generate next permutation (Johnson-Trotter algorithm variant)
        if sign == 1:
            perm1[0], perm1[1] = perm1[1], perm1[0]
            sign = -1
        else:
            perm1[1], perm1[2] = perm1[2], perm1[1]
            sign = 1
            r = 2
            while r < n:
                if count[r] < r:
                    break
                count[r] = 0
                r += 1
            else:
                return checksum, max_flips

            # Rotate perm1[0:r+1] right by one
            perm0 = perm1[0]
            for i in range(r):
                perm1[i] = perm1[i + 1]
            perm1[r] = perm0

            count[r] += 1


class Fannkuch(object):
    def run(self, iterations):
        for _ in range(iterations):
            n = 10
            checksum, max_flips = fannkuch(n)
            # Expected values for n=10, determined empirically.
            if checksum != 73196:
                return False
            if max_flips != 38:
                return False
        return True


if __name__ == "__main__":
    cinderx.jit.auto()

    num_iterations = 1
    if len(sys.argv) > 1:
        num_iterations = int(sys.argv[1])
    Fannkuch().run(num_iterations)
