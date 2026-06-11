#!/usr/bin/env bash
# (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

set -e

# Copied from cpython Makefile
# Point srcdir to the internal copy of cpython main (3.16) so we can use the cases_generator.
# 3.16 tracks the upstream Python "main" branch, so the source lives under main/, not 3.16/.
srcdir='../../../third-party/python/main/patched'

PYTHONPATH=$srcdir/Tools/cases_generator \
fbpython -- \
    $srcdir/Tools/cases_generator/tier1_generator.py \
        -o 3.16/Includes/generated_cases.c.h \
        $srcdir/Python/bytecodes.c \
        3.16/cinder-bytecodes.c

PYTHONPATH=$srcdir/Tools/cases_generator \
fbpython -- \
    $srcdir/Tools/cases_generator/target_generator.py \
        -o 3.16/cinderx_opcode_targets.h \
        $srcdir/Python/bytecodes.c \
        3.16/cinder-bytecodes.c

gen='generated'
sed -i "1i // @$gen" 3.16/Includes/generated_cases.c.h
