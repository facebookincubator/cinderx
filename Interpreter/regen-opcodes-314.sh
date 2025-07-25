#!/usr/bin/env bash
# (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.


srcdir=$(sl root)/third-party/python/3.14/patched


PYTHONPATH=$srcdir/Tools/cases_generator \
fbpython -- \
    $srcdir/Tools/cases_generator/opcode_id_generator.py \
        -o 3.14/opcode.h \
        $srcdir/Python/bytecodes.c \
        3.14/cinder-bytecodes.c

PYTHONPATH=$srcdir/Tools/cases_generator \
fbpython -- \
    $srcdir/Tools/cases_generator/opcode_metadata_generator.py \
        -o 3.14/cinder_opcode_metadata.h \
        $srcdir/Python/bytecodes.c \
        3.14/cinder-bytecodes.c

PYTHONPATH=$srcdir/Tools/cases_generator \
fbpython -- \
    $srcdir/Tools/cases_generator/target_generator.py \
        -o 3.14/cinderx_opcode_targets.h \
        $srcdir/Python/bytecodes.c \
        3.14/cinder-bytecodes.c
