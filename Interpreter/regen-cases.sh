#!/usr/bin/env bash
# (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

# Copied from cpython Makefile
# Point srcdir to the internal copy of cpython 3.12 so we can use the cases_generator
srcdir='../../../third-party/python/3.12'

PYTHONPATH=$srcdir/Tools/cases_generator \
buck run fbcode//cinderx:python3.10 -- \
    $srcdir/Tools/cases_generator/generate_cases.py \
        --emit-line-directives \
        -o Includes/generated_cases.c.h \
        -m Includes/opcode_metadata.h \
        Includes/bytecodes.c \
        cinder-bytecodes.c

gen='generated'
sed -i "1i // @$gen" Includes/generated_cases.c.h
sed -i "1i // @$gen" Includes/opcode_metadata.h
