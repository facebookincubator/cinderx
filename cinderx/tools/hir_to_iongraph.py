#!/usr/bin/env python3
# (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

"""Convert CinderX HIR text dumps to iongraph JSON format.

Usage:
    # Convert a saved HIR dump file
    python hir_to_iongraph.py /tmp/jit.log -o output.json

    # Pipe from a JIT run
    CINDERX_JIT_DUMP_HIR_PASSES=1 python -X cinderx-jit-enable script.py 2>&1 \
        | python hir_to_iongraph.py - -o output.json

Then open output.json in iongraph (https://mozilla-spidermonkey.github.io/iongraph/
or a local checkout).
"""

from __future__ import annotations

import argparse
import json
import re
import sys


# Matches the log line preceding each HIR dump.
_LOG_LINE_RE = re.compile(
    r"(?:"
    r"HIR for (.+?) (before|after) pass (.+?):"
    r"|Initial HIR for (.+?):"
    r"|Optimized HIR for (.+?):"
    r")\s*$"
)

_FUN_START_RE = re.compile(r"^fun\s+(.+?)\s*\{")

_BB_HEADER_RE = re.compile(r"^\s*bb\s+(\d+)" r"(?:\s+\(preds\s+([\d,\s]+)\))?" r"\s*\{")

# "    v0:Type = Opcode<imm> op1 op2 {"
_INSTR_WITH_OUTPUT_RE = re.compile(
    r"^\s+(v\d+)(?::(\S+))?\s*=\s*(\S+?)"
    r"(?:<(.+?)>)?"
    r"((?:\s+(?![\{])\S+)*)"
    r"\s*(\{)?\s*$"
)

# "    Opcode<imm> op1 op2 {"
_INSTR_NO_OUTPUT_RE = re.compile(
    r"^\s+([A-Z]\w+)"
    r"(?:<(.+?)>)?"
    r"((?:\s+(?![\{])\S+)*)"
    r"\s*(\{)?\s*$"
)

_REG_RE = re.compile(r"^v\d+$")

_BRANCH_RE = re.compile(r"^Branch$")
_COND_BRANCH_RE = re.compile(r"^CondBranch(?:IterNotDone|CheckType)?$")


def _parse_predecessors(preds_str: str | None) -> list[int]:
    if not preds_str:
        return []
    return [int(p.strip()) for p in preds_str.split(",") if p.strip()]


def _extract_successors(opcode: str, immediates: str | None) -> list[int]:
    if _BRANCH_RE.match(opcode) and immediates:
        return [int(immediates.strip())]
    if _COND_BRANCH_RE.match(opcode) and immediates:
        parts = [p.strip() for p in immediates.split(",")]
        if len(parts) >= 2:
            true_bb = int(parts[0])
            false_bb = int(parts[1])
            return [false_bb, true_bb]
    return []


def _skip_brace_block(lines: list[str], start: int) -> int:
    """Skip over a { ... } block starting after the opening brace."""
    brace_depth = 1
    i = start
    while i < len(lines) and brace_depth > 0:
        for ch in lines[i]:
            if ch == "{":
                brace_depth += 1
            elif ch == "}":
                brace_depth -= 1
        i += 1
    return i


def _parse_one_instruction(
    line: str,
) -> tuple[str | None, str, str, str | None, str, bool] | None:
    """Try to parse one instruction line.

    Returns (output_reg, output_type, opcode, immediates, operands_str, has_brace)
    or None if the line isn't an instruction.
    """
    m = _INSTR_WITH_OUTPUT_RE.match(line)
    if m:
        return (
            m.group(1),
            m.group(2) or "",
            m.group(3),
            m.group(4),
            m.group(5).strip(),
            m.group(6) is not None,
        )
    m = _INSTR_NO_OUTPUT_RE.match(line)
    if m:
        return (
            None,
            "",
            m.group(1),
            m.group(2),
            m.group(3).strip(),
            m.group(4) is not None,
        )
    return None


def _annotate_loops(blocks: list[dict]) -> None:
    """Detect loops via DFS and annotate blocks for iongraph.

    iongraph requires:
    - "loopheader" attribute on loop header blocks
    - "backedge" attribute on blocks that jump back to a loop header
    - Exactly one backedge predecessor per loop header
    - Backedge blocks must have the loop header as their only successor
    - Correct loopDepth on all blocks

    When a back-edge source has multiple successors (e.g. a conditional
    branch where one arm loops back), we insert a synthetic block to
    isolate the back-edge.
    """
    blocks_by_id: dict[int, dict] = {b["id"]: b for b in blocks}
    if not blocks:
        return

    # Iterative DFS to find back-edges.
    visited: set[int] = set()
    on_stack: set[int] = set()
    back_edges: set[tuple[int, int]] = set()
    loop_headers: set[int] = set()

    def dfs(start: int) -> None:
        stack: list[tuple[int, bool]] = [(start, True)]
        while stack:
            bid, is_enter = stack.pop()
            if not is_enter:
                on_stack.discard(bid)
                continue
            if bid in visited:
                continue
            visited.add(bid)
            on_stack.add(bid)
            stack.append((bid, False))
            block = blocks_by_id.get(bid)
            if block is None:
                continue
            for succ_id in reversed(block["successors"]):
                if succ_id in on_stack:
                    back_edges.add((bid, succ_id))
                    loop_headers.add(succ_id)
                elif succ_id not in visited:
                    stack.append((succ_id, True))

    entry_id = blocks[0]["id"]
    dfs(entry_id)
    for b in blocks:
        if b["id"] not in visited:
            dfs(b["id"])

    # For each loop header, collect all back-edge sources and pick one
    # canonical backedge block. If needed, insert synthetic blocks.
    next_block_id = max(b["id"] for b in blocks) + 1
    synthetic_blocks: list[dict] = []
    # Map: header_id -> chosen backedge block id
    header_to_backedge: dict[int, int] = {}

    for header_id in loop_headers:
        sources = [src for src, dst in back_edges if dst == header_id]
        if not sources:
            continue

        # Pick the first source. If it has multiple successors, we need
        # a synthetic block. If there are multiple sources to the same
        # header, we also need synthetics for extra ones.
        chosen_src = sources[0]
        chosen_block = blocks_by_id[chosen_src]

        if len(chosen_block["successors"]) == 1 and len(sources) == 1:
            # Simple case: dedicated backedge block
            header_to_backedge[header_id] = chosen_src
        else:
            # Insert a synthetic backedge block for this header.
            # All back-edge sources get their successor rewritten to
            # point to the synthetic block.
            syn_id = next_block_id
            next_block_id += 1
            syn_block = {
                "ptr": syn_id + 1,
                "id": syn_id,
                "loopDepth": 0,
                "attributes": [],
                "predecessors": sources,
                "successors": [header_id],
                "instructions": [],
            }
            synthetic_blocks.append(syn_block)
            blocks_by_id[syn_id] = syn_block

            # Rewrite each source's successor list
            for src_id in sources:
                src_block = blocks_by_id[src_id]
                src_block["successors"] = [
                    syn_id if s == header_id else s for s in src_block["successors"]
                ]

            # Update header's predecessors, replacing all back-edge
            # sources with a single syn_id entry.
            header_block = blocks_by_id[header_id]
            seen_syn = False
            new_preds = []
            for p in header_block["predecessors"]:
                if p in sources:
                    if not seen_syn:
                        new_preds.append(syn_id)
                        seen_syn = True
                else:
                    new_preds.append(p)
            header_block["predecessors"] = new_preds

            header_to_backedge[header_id] = syn_id

    blocks.extend(synthetic_blocks)

    # Recompute loop depth after structural changes.
    # Rebuild blocks_by_id since we added synthetic blocks.
    blocks_by_id = {b["id"]: b for b in blocks}

    # Compute the natural loop body for each header by intersecting
    # forward reachability from the header with backward reachability
    # from the backedge source.  Plain backward walking can pull in
    # blocks that enter the latch from outside the loop (e.g. when
    # a conditional latch has predecessors on the non-loop path).
    loop_bodies: dict[int, set[int]] = {}
    for header_id in loop_headers:
        backedge_id = header_to_backedge.get(header_id)

        forward_reachable: set[int] = {header_id}
        fw_worklist: list[int] = [header_id]
        while fw_worklist:
            bid = fw_worklist.pop()
            block = blocks_by_id.get(bid)
            if block is None:
                continue
            for succ_id in block["successors"]:
                if bid == backedge_id and succ_id == header_id:
                    continue
                if succ_id not in forward_reachable:
                    forward_reachable.add(succ_id)
                    fw_worklist.append(succ_id)

        body: set[int] = {header_id}
        worklist: list[int] = []
        if backedge_id is not None and backedge_id not in body:
            body.add(backedge_id)
            worklist.append(backedge_id)
        while worklist:
            bid = worklist.pop()
            block = blocks_by_id.get(bid)
            if block is None:
                continue
            for pred_id in block["predecessors"]:
                if pred_id not in body and pred_id in forward_reachable:
                    body.add(pred_id)
                    worklist.append(pred_id)
        loop_bodies[header_id] = body

    # Detect loop nesting: if a block in loop H1's body has a successor
    # that is loop header H2, then H2 is nested inside H1. Merge H2's
    # body into H1's body. Repeat until stable.
    changed = True
    while changed:
        changed = False
        for h1 in loop_headers:
            for h2 in loop_headers:
                if h1 == h2:
                    continue
                if h2 in loop_bodies[h1]:
                    if not loop_bodies[h2].issubset(loop_bodies[h1]):
                        loop_bodies[h1] |= loop_bodies[h2]
                        changed = True
                    continue
                # Check if any block in h1's body has h2 as a successor
                for bid in loop_bodies[h1]:
                    block = blocks_by_id.get(bid)
                    if block and h2 in block["successors"]:
                        loop_bodies[h1] |= loop_bodies[h2]
                        changed = True
                        break

    block_loop_depth: dict[int, int] = {b["id"]: 0 for b in blocks}
    for header_id in loop_headers:
        for bid in loop_bodies[header_id]:
            block_loop_depth[bid] += 1

    backedge_ids = set(header_to_backedge.values())
    for block in blocks:
        bid = block["id"]
        block["loopDepth"] = block_loop_depth[bid]
        attrs = []
        if bid in loop_headers:
            attrs.append("loopheader")
        if bid in backedge_ids:
            attrs.append("backedge")
        block["attributes"] = attrs


def _parse_function(lines: list[str]) -> list[dict]:
    """Parse basic blocks from a function body.

    Instruction IDs and ptr values are globally unique across all blocks.
    The register-to-ID map spans the whole function (SSA property).
    """
    # Collect block boundaries
    block_infos: list[tuple[int, list[int], list[str]]] = []
    current_block_id: int | None = None
    current_preds: list[int] = []
    current_lines: list[str] = []

    def flush() -> None:
        nonlocal current_lines
        if current_block_id is not None:
            block_infos.append((current_block_id, current_preds, current_lines))
            current_lines = []

    for line in lines:
        m = _BB_HEADER_RE.match(line)
        if m:
            flush()
            current_block_id = int(m.group(1))
            current_preds = _parse_predecessors(m.group(2))
            current_lines = []
        elif current_block_id is not None:
            current_lines.append(line)
    flush()

    # Parse instructions with global IDs
    reg_to_id: dict[str, int] = {}
    next_id = 0
    blocks = []

    for block_id, preds, blines in block_infos:
        instructions = []
        successors: list[int] = []
        i = 0
        while i < len(blines):
            line = blines[i]
            stripped = line.strip()
            if not stripped or stripped == "}":
                i += 1
                continue

            parsed = _parse_one_instruction(line)
            if parsed is None:
                i += 1
                continue

            output_reg, output_type, opcode, immediates, operands_str, has_brace = (
                parsed
            )

            if has_brace:
                i = _skip_brace_block(blines, i + 1)
            else:
                i += 1

            operand_names = operands_str.split() if operands_str else []
            inputs = []
            for op in operand_names:
                if _REG_RE.match(op) and op in reg_to_id:
                    inputs.append(reg_to_id[op])

            # Build opcode display string with clickable Name#ID refs
            display_parts = []
            if output_reg:
                display_parts.append(output_reg)
                if output_type:
                    display_parts.append(f":{output_type}")
                display_parts.append(" = ")

            display_parts.append(opcode)
            if immediates is not None:
                display_parts.append(f"<{immediates}>")

            for op in operand_names:
                if op in reg_to_id:
                    display_parts.append(f" {op}#{reg_to_id[op]}")
                else:
                    display_parts.append(f" {op}")

            cur_id = next_id
            next_id += 1

            if output_reg:
                reg_to_id[output_reg] = cur_id

            succs = _extract_successors(opcode, immediates)
            if succs:
                successors = succs

            instructions.append(
                {
                    "ptr": cur_id + 1,
                    "id": cur_id,
                    "opcode": "".join(display_parts),
                    "attributes": [],
                    "inputs": inputs,
                    "uses": [],
                    "memInputs": [],
                    "type": output_type if output_type else "None",
                }
            )

        blocks.append(
            {
                "ptr": block_id + 1,
                "id": block_id,
                "loopDepth": 0,
                "attributes": [],
                "predecessors": preds,
                "successors": successors,
                "instructions": instructions,
            }
        )

    _annotate_loops(blocks)
    return blocks


def _collect_fun_body(lines: list[str], start: int) -> tuple[list[str], int]:
    """Collect lines of a fun block starting after 'fun name {'.

    Returns (body_lines, next_index).
    """
    brace_depth = 1
    fun_lines = []
    i = start
    while i < len(lines) and brace_depth > 0:
        for ch in lines[i]:
            if ch == "{":
                brace_depth += 1
            elif ch == "}":
                brace_depth -= 1
        if brace_depth > 0:
            fun_lines.append(lines[i])
        i += 1
    return fun_lines, i


def _add_pass(
    functions: dict[str, list[dict]],
    func_name: str,
    pass_name: str,
    fun_lines: list[str],
) -> None:
    blocks = _parse_function(fun_lines)
    if func_name not in functions:
        functions[func_name] = []
    functions[func_name].append(
        {
            "name": pass_name,
            "mir": {"blocks": blocks},
            "lir": {"blocks": []},
        }
    )


def parse_hir_dump(text: str) -> dict:
    """Parse full HIR dump output into iongraph JSON."""
    functions: dict[str, list[dict]] = {}
    seen_before: set[str] = set()
    lines = text.split("\n")
    i = 0

    while i < len(lines):
        line = lines[i]

        m = _LOG_LINE_RE.search(line)
        if m:
            skip = False
            if m.group(1):
                func_name = m.group(1)
                phase = m.group(2)
                pass_name = m.group(3)
                if phase == "before":
                    if func_name not in seen_before:
                        seen_before.add(func_name)
                        pass_display = "Initial HIR"
                    else:
                        skip = True
                        pass_display = ""
                else:
                    pass_display = pass_name
            elif m.group(4):
                func_name = m.group(4)
                pass_display = "Initial HIR"
            else:
                func_name = m.group(5)
                pass_display = "Optimized (final)"

            i += 1
            while i < len(lines):
                if _FUN_START_RE.match(lines[i]):
                    break
                i += 1
            else:
                continue

            i += 1
            fun_lines, i = _collect_fun_body(lines, i)
            if not skip:
                _add_pass(functions, func_name, pass_display, fun_lines)
        else:
            fm = _FUN_START_RE.match(line)
            if fm:
                func_name = fm.group(1)
                i += 1
                fun_lines, i = _collect_fun_body(lines, i)
                _add_pass(functions, func_name, "HIR", fun_lines)
            else:
                i += 1

    return {
        "version": 1,
        "functions": [
            {"name": name, "passes": passes} for name, passes in functions.items()
        ],
    }


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Convert CinderX HIR text dumps to iongraph JSON."
    )
    parser.add_argument(
        "input",
        help="path to HIR dump file, or '-' to read from stdin",
    )
    parser.add_argument(
        "-o",
        "--output",
        help="output JSON file path (default: stdout)",
    )
    args = parser.parse_args()

    if args.input == "-":
        text = sys.stdin.read()
    else:
        with open(args.input) as f:
            text = f.read()

    result = parse_hir_dump(text)

    if args.output:
        with open(args.output, "w") as f:
            json.dump(result, f, indent=2)
            f.write("\n")
    else:
        json.dump(result, sys.stdout, indent=2)
        sys.stdout.write("\n")


if __name__ == "__main__":
    main()
