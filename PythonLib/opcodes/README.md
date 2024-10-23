Code to deal with the new opcodes added by cinderx.

Upstream cpython has a script, `generate_opcode_h.py`, which reads opcode
definitions from `Lib/opcode.py` and generates the public header file
`Include/opcode.h`. In cinder 3.10 these opcodes were defined in
`cinderx/PythonLib/cinderx/opcode.py` and read in by `Lib/opcode.py` in our
fork of the cpython source tree, so that when `generate_opcode_h.py` called
`Lib/opcode.py` these definitions were picked up as well.

For cinderx, we build an extension module rather than forking and modifying the
cpython source code, so make a few changes to the setup:

- We cannot modify `Lib/opcode.py`, so we instead copy `generate_opcode_h.py`
  into `cinderx/Interpreter` and modify our copy of that. We now have several sets
  of patches to `generate_opcode_h.py`:
  - The changes from 3.10 -> 3.10.cinder
  - The patch to read cinderx opcodes as well as `Lib/opcode.py`
  - Separating regular opcodes (emitted as an enum rather than #defines for
     cinderx) and pseudo-opcodes (still emitted as #defines because we don't
     want them in the enum)
- Python's opcode numbering is not stable across versions, so we need to keep
  changing the numbers we assign to our additional opcodes. Rather than
  manually assign numbers every time, we split this into two parts:
  - Move the opcode definitions to
     `cinderx/PythonLib/opcodes/cinderx_opcodes.py` and define the new opcodes
     *without* opcode numbers.
  - Add a new script, `assign_opcode_numbers.py` which reads
     `cinderx_opcodes.py` and generates `opcode_312.py`. `opcode_312.py` is
     checked in, and is added as an input to our fork of
     `generate_opcode_h.py`, which then adds the new opcodes while generating
     `opcode.h`.

NOTE: `opcode_312.py` is generated manually and checked in, rather than being
generated as part of running `generate_opcode_h.py`, since it is not expected
to change once generated, and since it is useful to be able to read the code
for the opcode numbering.
