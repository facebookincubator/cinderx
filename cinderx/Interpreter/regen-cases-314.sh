#!/usr/bin/env bash
# (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

# Copied from cpython Makefile
# Point srcdir to the internal copy of cpython 3.12 so we can use the cases_generator
srcdir='../../../third-party/python/3.14/patched'

PYTHONPATH=$srcdir/Tools/cases_generator \
fbpython -- \
    $srcdir/Tools/cases_generator/tier1_generator.py \
        -o 3.14/Includes/generated_cases.c.h \
        $srcdir/Python/bytecodes.c \
        3.14/cinder-bytecodes.c

PYTHONPATH=$srcdir/Tools/cases_generator \
fbpython -- \
    $srcdir/Tools/cases_generator/target_generator.py \
        -o 3.14/cinderx_opcode_targets.h \
        $srcdir/Python/bytecodes.c \
        3.14/cinder-bytecodes.c


PYTHONPATH=$srcdir/Tools/cases_generator \
fbpython -- \
    $srcdir/Tools/cases_generator/opcode_metadata_generator.py \
        -o 3.14/cinder_opcode_metadata.h \
        $srcdir/Python/bytecodes.c \
        3.14/cinder-bytecodes.c

gen='generated'
sed -i "1i // @$gen" 3.14/Includes/generated_cases.c.h
