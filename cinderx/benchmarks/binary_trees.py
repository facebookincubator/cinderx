# Copyright (c) Meta Platforms, Inc. and affiliates.

"""
Binary trees benchmark.

Repeatedly builds and traverses complete binary trees of varying depths.
Exercises object allocation pressure, recursive function calls, and
tree traversal patterns. This is an allocation-heavy workload that
stresses the garbage collector and object creation/destruction paths.

Based on the classic "binary-trees" benchmark from the Computer Language
Benchmarks Game.
"""

import sys

import cinderx.jit


class TreeNode(object):
    def __init__(self, left, right):
        self.left = left
        self.right = right


def make_tree(depth):
    if depth <= 0:
        return TreeNode(None, None)
    depth -= 1
    return TreeNode(make_tree(depth), make_tree(depth))


def check_tree(node):
    if node.left is None:
        return 1
    return 1 + check_tree(node.left) + check_tree(node.right)


class BinaryTrees(object):
    def run(self, iterations):
        for _ in range(iterations):
            min_depth = 4
            max_depth = 17
            stretch_depth = max_depth + 1

            # Stretch tree
            stretch_check = check_tree(make_tree(stretch_depth))
            if stretch_check != (2 ** (stretch_depth + 1)) - 1:
                return False

            long_lived_tree = make_tree(max_depth)

            total_check = 0
            depth = min_depth
            while depth <= max_depth:
                n_iters = 2 ** (max_depth - depth + min_depth)
                check = 0
                for _ in range(n_iters):
                    check += check_tree(make_tree(depth))
                total_check += check
                depth += 2

            long_lived_check = check_tree(long_lived_tree)
            if long_lived_check != (2 ** (max_depth + 1)) - 1:
                return False

            # Verify total check count is deterministic.
            # Expected value was determined empirically for max_depth=17.
            if total_check != 29185376:
                return False

        return True


if __name__ == "__main__":
    cinderx.jit.auto()

    num_iterations = 1
    if len(sys.argv) > 1:
        num_iterations = int(sys.argv[1])
    BinaryTrees().run(num_iterations)
