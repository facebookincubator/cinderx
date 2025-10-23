#!/usr/bin/env bash
# (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

set -e

root=$(sl root)
buck2 run fbcode//cinderx/PythonLib/opcodes:assign_opcode_numbers314 $root/fbcode/cinderx/PythonLib/opcodes/3.14/opcode.py

buck2 run :gen-opcodes-314 $root/fbcode/cinderx/Interpreter/3.14/cinder_opcode_ids.h
