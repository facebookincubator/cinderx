#!/usr/bin/env bash
# (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

OUT=$(buck build -m ovr_config//toolchain/python/constraints:3.12 fbcode//cinderx/Interpreter:gen_opcode_h --show-full-simple-output)
cp "$OUT" opcode_312.h

OUT=$(buck build -m ovr_config//toolchain/python/constraints:3.12 fbcode//cinderx/Interpreter:gen_opcode_h\[cinder_opcode.h\] --show-full-simple-output)
cp "$OUT" cinder_opcode_312.h

OUT=$(buck build -m ovr_config//toolchain/python/constraints:3.12 fbcode//cinderx/Interpreter:gen_cinderx_opcode_targets_h --show-full-simple-output)
cp "$OUT" cinderx_opcode_targets_312.h
