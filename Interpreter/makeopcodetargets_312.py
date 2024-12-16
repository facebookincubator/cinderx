import cinderx_opcode
import lib_opcode as opcode
import makeopcodetargets_base

cinderx_opcode.init(
    opcode.opname,
    opcode.opmap,
    opcode.hasname,
    opcode.hasjrel,
    opcode.hasjabs,
    opcode.hasconst,
    interp_only=True,
)

makeopcodetargets_base.find_module = lambda name: opcode
makeopcodetargets_base.main()
