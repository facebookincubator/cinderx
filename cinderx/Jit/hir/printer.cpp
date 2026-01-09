// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/hir/printer.h"

#include "cinderx/Common/util.h"
#include "cinderx/Jit/symbolizer.h"

#if PY_VERSION_HEX >= 0x030E0000
#include "pycore_ceval.h"
#include "pycore_intrinsics.h"
#endif

#include <fmt/format.h>
#include <fmt/ostream.h>

#include <algorithm>
#include <ostream>
#include <sstream>
#include <vector>

namespace jit::hir {

namespace {

const char* fvc_to_string(int conversion) {
  static const char* kFvcStrings[] = {
      "None", // FVC_NONE (0)
      "Str", // FVC_STR (1)
      "Repr", // FVC_REPR (2)
      "ASCII" // FVC_ASCII (3)
  };

  if (conversion >= 0 && conversion < 4) {
    return kFvcStrings[conversion];
  }
  return "Unknown";
}
} // namespace

void HIRPrinter::Indent() {
  indent_level_ += 1;
}

std::ostream& HIRPrinter::Indented(std::ostream& os) {
  os << line_prefix_;
  for (int i = 0; i < indent_level_; i++) {
    os << "  ";
  }
  return os;
}

void HIRPrinter::Dedent() {
  indent_level_ -= 1;
}

void HIRPrinter::Print(std::ostream& os, const Function& func) {
  func_ = &func;
  SCOPE_EXIT(func_ = nullptr);

  fmt::print(
      os, "fun {} {{\n", func.fullname.empty() ? "<unknown>" : func.fullname);
  Indent();
  Print(os, func.cfg);
  Dedent();
  os << "}\n";
}

void HIRPrinter::Print(std::ostream& os, const CFG& cfg) {
  auto start = cfg.entry_block;
  std::vector<BasicBlock*> blocks = cfg.GetRPOTraversal(start);
  auto last_block = blocks.back();
  for (auto block : blocks) {
    Print(os, *block);
    if (block != last_block) {
      os << '\n';
    }
  }
}

void HIRPrinter::Print(std::ostream& os, const BasicBlock& block) {
  Indented(os);
  fmt::print(os, "bb {}", block.id);
  auto& in_edges = block.in_edges();
  if (!in_edges.empty()) {
    std::vector<const Edge*> edges(in_edges.begin(), in_edges.end());
    std::sort(edges.begin(), edges.end(), [](auto& e1, auto& e2) {
      return e1->from()->id < e2->from()->id;
    });
    os << " (preds ";
    auto sep = "";
    for (auto edge : edges) {
      fmt::print(os, "{}{}", sep, edge->from()->id);
      sep = ", ";
    }
    os << ")";
  }
  os << " {\n";
  Indent();
  for (auto& instr : block) {
    Print(os, instr);
    os << '\n';
  }
  Dedent();
  Indented(os) << "}\n";
}

static void print_reg_states(
    std::ostream& os,
    const std::vector<RegState>& reg_states) {
  auto rss = reg_states;
  std::sort(rss.begin(), rss.end(), [](RegState& a, RegState& b) {
    return a.reg->id() < b.reg->id();
  });
  os << fmt::format("<{}>", rss.size());
  if (!rss.empty()) {
    os << " ";
  }
  auto sep = "";
  for (auto& reg_state : rss) {
    const char* prefix = "?";
    switch (reg_state.value_kind) {
      case ValueKind::kSigned: {
        prefix = "s";
        break;
      }
      case ValueKind::kUnsigned: {
        prefix = "uns";
        break;
      }
      case ValueKind::kBool: {
        prefix = "bool";
        break;
      }
      case ValueKind::kDouble:
        prefix = "double";
        break;
      case ValueKind::kObject: {
        switch (reg_state.ref_kind) {
          case RefKind::kUncounted: {
            prefix = "unc";
            break;
          }
          case RefKind::kBorrowed: {
            prefix = "b";
            break;
          }
          case RefKind::kOwned: {
            prefix = "o";
            break;
          }
        }
        break;
      }
    }
    os << fmt::format("{}{}:{}", sep, prefix, reg_state.reg->name());
    sep = " ";
  }
}

static const int kMaxASCII = 127;

static std::string escape_unicode(const char* data, Py_ssize_t size) {
  std::string ret = "\"";
  for (Py_ssize_t i = 0; i < size; ++i) {
    char c = data[i];
    switch (c) {
      case '"':
      case '\\':
        ret += '\\';
        ret += c;
        break;
      case '\n':
        ret += "\\n";
        break;
      default:
        if (static_cast<unsigned char>(c) > kMaxASCII) {
          ret += '\\';
          ret += std::to_string(static_cast<unsigned char>(c));
        } else {
          ret += c;
        }
        break;
    }
  }
  ret += '"';
  return ret;
}

static std::string escape_unicode(PyObject* str) {
  Py_ssize_t size;
  const char* data = PyUnicode_AsUTF8AndSize(str, &size);
  if (data == nullptr) {
    PyErr_Clear();
    return "";
  }
  return escape_unicode(data, size);
}

static std::string format_name_impl(int idx, PyObject* names) {
  return fmt::format(
      "{}; {}", idx, escape_unicode(PyTuple_GET_ITEM(names, idx)));
}

static std::string
format_name(const Function* func, const Instr& instr, int idx) {
  auto code = func != nullptr ? func->codeFor(instr) : nullptr;
  if (idx < 0 || code == nullptr) {
    return fmt::format("{}", idx);
  }

  return format_name_impl(idx, code->co_names);
}

static std::string format_load_super(
    const Function* func,
    const LoadSuperBase& load) {
  auto code = func != nullptr ? func->codeFor(load) : nullptr;
  if (code == nullptr) {
    return fmt::format("{} {}", load.name_idx(), load.no_args_in_super_call());
  }
  return fmt::format(
      "{}, {}",
      format_name_impl(load.name_idx(), code->co_names),
      load.no_args_in_super_call());
}

static std::string
format_varname(const Function* func, const Instr& instr, int idx) {
  auto code = func != nullptr ? func->codeFor(instr) : nullptr;
  if (idx < 0 || code == nullptr) {
    return fmt::format("{}", idx);
  }

  auto names = getVarnameTuple(code, &idx);
  return format_name_impl(idx, names);
}

static std::string format_immediates(const Function* func, const Instr& instr) {
  switch (instr.opcode()) {
    case Opcode::kAssign:
    case Opcode::kBatchDecref:
    case Opcode::kBitCast:
    case Opcode::kBuildString:
    case Opcode::kBuildTemplate:
    case Opcode::kCheckErrOccurred:
    case Opcode::kCheckExc:
    case Opcode::kCheckNeg:
    case Opcode::kCheckSequenceBounds:
    case Opcode::kCIntToCBool:
    case Opcode::kCopyDictWithoutKeys:
    case Opcode::kDecref:
    case Opcode::kDeleteSubscr:
    case Opcode::kDeopt:
    case Opcode::kDictMerge:
    case Opcode::kDictSubscr:
    case Opcode::kDictUpdate:
    case Opcode::kEndInlinedFunction:
    case Opcode::kFormatWithSpec:
    case Opcode::kGetAIter:
    case Opcode::kGetANext:
    case Opcode::kGetIter:
    case Opcode::kGetLength:
    case Opcode::kGetTuple:
    case Opcode::kGuard:
    case Opcode::kIncref:
    case Opcode::kInitialYield:
    case Opcode::kInvokeIterNext:
    case Opcode::kIsInstance:
    case Opcode::kIsNegativeAndErrOccurred:
    case Opcode::kIsTruthy:
    case Opcode::kListAppend:
    case Opcode::kListExtend:
    case Opcode::kLoadCellItem:
    case Opcode::kLoadCurrentFunc:
    case Opcode::kLoadEvalBreaker:
    case Opcode::kLoadFieldAddress:
    case Opcode::kLoadVarObjectSize:
    case Opcode::kMakeCell:
    case Opcode::kMakeFunction:
    case Opcode::kMakeSet:
    case Opcode::kMakeTupleFromList:
    case Opcode::kMatchClass:
    case Opcode::kMatchKeys:
    case Opcode::kMergeSetUnpack:
    case Opcode::kPrimitiveBoxBool:
    case Opcode::kRaise:
    case Opcode::kRunPeriodicTasks:
    case Opcode::kSend:
    case Opcode::kSetCurrentAwaiter:
    case Opcode::kSetCellItem:
    case Opcode::kSetDictItem:
    case Opcode::kSetSetItem:
    case Opcode::kSetUpdate:
    case Opcode::kSnapshot:
    case Opcode::kStealCellItem:
    case Opcode::kStoreArrayItem:
    case Opcode::kStoreSubscr:
    case Opcode::kWaitHandleLoadCoroOrResult:
    case Opcode::kWaitHandleLoadWaiter:
    case Opcode::kWaitHandleRelease:
    case Opcode::kXDecref:
    case Opcode::kXIncref:
    case Opcode::kYieldAndYieldFrom:
    case Opcode::kYieldFrom:
    case Opcode::kYieldFromHandleStopAsyncIteration:
    case Opcode::kUnicodeConcat:
    case Opcode::kUnicodeRepeat:
    case Opcode::kUnicodeSubscr:
    case Opcode::kUnreachable:
    case Opcode::kYieldValue: {
      return "";
    }
    case Opcode::kBeginInlinedFunction:
      return static_cast<const BeginInlinedFunction&>(instr).fullname();
    case Opcode::kLoadArrayItem: {
      const auto& load = static_cast<const LoadArrayItem&>(instr);
      return load.offset() == 0 ? "" : fmt::format("Offset[{}]", load.offset());
    }
    case Opcode::kLoadSplitDictItem: {
      const auto& load = static_cast<const LoadSplitDictItem&>(instr);
      return fmt::format("{}", load.itemIdx());
    }
    case Opcode::kReturn: {
      const auto& ret = static_cast<const Return&>(instr);
      return ret.type() != TObject ? ret.type().toString() : "";
    }
    case Opcode::kCallEx: {
      const auto& call = static_cast<const CallEx&>(instr);
      return fmt::format(
          "{}{}",
          (call.flags() & CallFlags::Awaited) ? ", awaited" : "",
          (call.flags() & CallFlags::KwArgs) ? ", kwargs" : "");
    }
    case Opcode::kCallInd: {
      const auto& call = static_cast<const CallInd&>(instr);
      return fmt::format("{}", call.name());
    }
    case Opcode::kBinaryOp: {
      const auto& bin_op = static_cast<const BinaryOp&>(instr);
      return std::string{GetBinaryOpName(bin_op.op())};
    }
    case Opcode::kUnaryOp: {
      const auto& unary_op = static_cast<const UnaryOp&>(instr);
      return std::string{GetUnaryOpName(unary_op.op())};
    }
    case Opcode::kBranch: {
      const auto& branch = static_cast<const Branch&>(instr);
      return fmt::format("{}", branch.target()->id);
    }
    case Opcode::kVectorCall: {
      const auto& call = static_cast<const VectorCall&>(instr);
      return fmt::format(
          "{}{}{}{}",
          call.numArgs(),
          (call.flags() & CallFlags::Awaited) ? ", awaited" : "",
          (call.flags() & CallFlags::KwArgs) ? ", kwnames" : "",
          (call.flags() & CallFlags::Static) ? ", static" : "");
    }
    case Opcode::kCallCFunc: {
      const auto& call = static_cast<const CallCFunc&>(instr);
      return std::string{call.funcName()};
    }
    case Opcode::kCallIntrinsic: {
      const auto& call = static_cast<const CallIntrinsic&>(instr);
#if PY_VERSION_HEX >= 0x030E0000
      switch (call.NumOperands()) {
        case 1:
          return _PyIntrinsics_UnaryFunctions[call.index()].name;
        case 2:
          return _PyIntrinsics_BinaryFunctions[call.index()].name;
        default:
          JIT_ABORT("Invalid number of intrinsic args: {}", call.NumOperands());
      }
#else
      return fmt::format("{}", call.index());
#endif
    }
    case Opcode::kCallMethod: {
      const auto& call = static_cast<const CallMethod&>(instr);
      return fmt::format(
          "{}{}",
          call.NumOperands(),
          (call.flags() & CallFlags::Awaited) ? ", awaited" : "");
    }
    case Opcode::kCallStatic: {
      const auto& call = static_cast<const CallStatic&>(instr);
      std::optional<std::string> func_name = symbolize(call.addr());
      if (func_name.has_value()) {
        return fmt::format(
            "{}@{}, {}",
            *func_name,
            getStablePointer(call.addr()),
            call.NumOperands());
      }
      return fmt::format(
          "{}, {}", getStablePointer(call.addr()), call.NumOperands());
    }
    case Opcode::kCallStaticRetVoid: {
      const auto& call = static_cast<const CallStaticRetVoid&>(instr);
      std::optional<std::string> func_name = symbolize(call.addr());
      if (func_name.has_value()) {
        return fmt::format(
            "{}@{}, {}",
            *func_name,
            getStablePointer(call.addr()),
            call.NumOperands());
      }
      return fmt::format(
          "{}, {}", getStablePointer(call.addr()), call.NumOperands());
    }
    case Opcode::kInvokeStaticFunction: {
      const auto& call = static_cast<const InvokeStaticFunction&>(instr);
      return fmt::format(
          "{}.{}, {}, {}",
          PyUnicode_AsUTF8(call.func()->func_module),
          PyUnicode_AsUTF8(call.func()->func_qualname),
          call.NumOperands(),
          call.ret_type());
    }
    case Opcode::kInitFrameCellVars: {
      const auto& init = static_cast<const InitFrameCellVars&>(instr);
      return fmt::format("{}", init.num_cell_vars());
    }
    case Opcode::kLoadField: {
      const auto& lf = static_cast<const LoadField&>(instr);
      std::size_t offset = lf.offset();
#ifdef Py_TRACE_REFS
      // Keep these stable from the offset of ob_refcnt, in trace refs
      // we have 2 extra next/prev pointers linking all objects together
      offset -= (sizeof(PyObject*) * 2);
#endif

      return fmt::format(
          "{}@{}, {}, {}",
          lf.name(),
          offset,
          lf.type(),
          lf.borrowed() ? "borrowed" : "owned");
    }
    case Opcode::kStoreField: {
      const auto& sf = static_cast<const StoreField&>(instr);
      return fmt::format("{}@{}", sf.name(), sf.offset());
    }
    case Opcode::kCast: {
      const auto& cast = static_cast<const Cast&>(instr);
      std::string result = cast.pytype()->tp_name;
      if (cast.exact()) {
        result = fmt::format("Exact[{}]", result);
      }
      if (cast.optional()) {
        result = fmt::format("Optional[{}]", result);
      }
      return result;
    }
    case Opcode::kTpAlloc: {
      const auto& tp_alloc = static_cast<const TpAlloc&>(instr);
      return fmt::format("{}", tp_alloc.pytype()->tp_name);
    }
    case Opcode::kCompare: {
      const auto& cmp = static_cast<const Compare&>(instr);
      return std::string{GetCompareOpName(cmp.op())};
    }
    case Opcode::kFloatCompare: {
      const auto& cmp = static_cast<const FloatCompare&>(instr);
      return std::string{GetCompareOpName(cmp.op())};
    }
    case Opcode::kLongCompare: {
      const auto& cmp = static_cast<const LongCompare&>(instr);
      return std::string{GetCompareOpName(cmp.op())};
    }
    case Opcode::kUnicodeCompare: {
      const auto& cmp = static_cast<const UnicodeCompare&>(instr);
      return std::string{GetCompareOpName(cmp.op())};
    }
    case Opcode::kLongBinaryOp: {
      const auto& bin = static_cast<const LongBinaryOp&>(instr);
      return std::string{GetBinaryOpName(bin.op())};
    }
    case Opcode::kLongInPlaceOp: {
      const auto& inplace = static_cast<const LongInPlaceOp&>(instr);
      return std::string{GetInPlaceOpName(inplace.op())};
    }
    case Opcode::kFloatBinaryOp: {
      const auto& bin = static_cast<const FloatBinaryOp&>(instr);
      return std::string{GetBinaryOpName(bin.op())};
    }
    case Opcode::kCompareBool: {
      const auto& cmp = static_cast<const Compare&>(instr);
      return std::string{GetCompareOpName(cmp.op())};
    }
    case Opcode::kIntConvert: {
      const auto& conv = static_cast<const IntConvert&>(instr);
      return conv.type().toString();
    }
    case Opcode::kPrimitiveUnaryOp: {
      const auto& unary = static_cast<const PrimitiveUnaryOp&>(instr);
      return std::string{GetPrimitiveUnaryOpName(unary.op())};
    }
    case Opcode::kCondBranch:
    case Opcode::kCondBranchIterNotDone:
    case Opcode::kCondBranchCheckType: {
      const auto& cond = static_cast<const CondBranchBase&>(instr);
      auto targets =
          fmt::format("{}, {}", cond.true_bb()->id, cond.false_bb()->id);
      if (cond.IsCondBranchCheckType()) {
        Type type = static_cast<const CondBranchCheckType&>(cond).type();
        return fmt::format("{}, {}", targets, type);
      }
      return targets;
    }
    case Opcode::kDoubleBinaryOp: {
      const auto& bin_op = static_cast<const DoubleBinaryOp&>(instr);
      return std::string{GetBinaryOpName(bin_op.op())};
    }
    case Opcode::kLoadArg: {
      const auto& load = static_cast<const LoadArg&>(instr);
      auto varname = format_varname(func, load, load.arg_idx());
      if (load.type() == TObject) {
        return varname;
      }
      return fmt::format("{}, {}", varname, load.type());
    }
    case Opcode::kLoadAttrSpecial: {
      const auto& load = static_cast<const LoadAttrSpecial&>(instr);
#if PY_VERSION_HEX < 0x030C0000
      _Py_Identifier* id = load.id();
      return fmt::format("\"{}\"", id->string);
#else
      return fmt::format("\"{}\"", repr(load.id()));
#endif
    }
    case Opcode::kLoadMethod:
    case Opcode::kLoadMethodCached:
    case Opcode::kLoadModuleMethodCached: {
      const auto& load = static_cast<const LoadMethodBase&>(instr);
      return format_name(func, load, load.name_idx());
    }
    case Opcode::kLoadMethodSuper: {
      return format_load_super(func, static_cast<const LoadSuperBase&>(instr));
    }
    case Opcode::kLoadAttrSuper: {
      return format_load_super(func, static_cast<const LoadSuperBase&>(instr));
    }
    case Opcode::kLoadConst: {
      const auto& load = static_cast<const LoadConst&>(instr);
      return fmt::format("{}", load.type());
    }
    case Opcode::kLoadFunctionIndirect: {
      const auto& load = static_cast<const LoadFunctionIndirect&>(instr);
      PyObject* py_func = *load.funcptr();
      const char* name;
      if (PyFunction_Check(py_func)) {
        name = PyUnicode_AsUTF8(((PyFunctionObject*)py_func)->func_name);
      } else {
        name = Py_TYPE(py_func)->tp_name;
      }
      return fmt::format("{}", name);
    }
    case Opcode::kIntBinaryOp: {
      const auto& bin_op = static_cast<const IntBinaryOp&>(instr);
      return std::string{GetBinaryOpName(bin_op.op())};
    }
    case Opcode::kPrimitiveCompare: {
      const auto& cmp = static_cast<const PrimitiveCompare&>(instr);
      return std::string{GetPrimitiveCompareOpName(cmp.op())};
    }
    case Opcode::kPrimitiveBox: {
      const auto& box = static_cast<const PrimitiveBox&>(instr);
      return fmt::format("{}", box.type());
    }
    case Opcode::kPrimitiveUnbox: {
      const auto& unbox = static_cast<const PrimitiveUnbox&>(instr);
      return fmt::format("{}", unbox.type());
    }
    case Opcode::kIndexUnbox: {
      const auto& unbox = static_cast<const IndexUnbox&>(instr);
      return fmt::format(
          "{}", reinterpret_cast<PyTypeObject*>(unbox.exception())->tp_name);
    }
    case Opcode::kLoadGlobalCached: {
      const auto& load = static_cast<const LoadGlobalCached&>(instr);
      return format_name(func, load, load.name_idx());
    }
    case Opcode::kLoadGlobal: {
      const auto& load = static_cast<const LoadGlobal&>(instr);
      return format_name(func, load, load.name_idx());
    }
    case Opcode::kMakeList: {
      const auto& make = static_cast<const MakeList&>(instr);
      return fmt::format("{}", make.nvalues());
    }
    case Opcode::kMakeTuple: {
      const auto& make = static_cast<const MakeTuple&>(instr);
      return fmt::format("{}", make.nvalues());
    }
    case Opcode::kGetSecondOutput: {
      return fmt::format(
          "{}", static_cast<const GetSecondOutput&>(instr).type());
    }
    case Opcode::kLoadTupleItem: {
      const auto& loaditem = static_cast<const LoadTupleItem&>(instr);
      return fmt::format("{}", loaditem.idx());
    }
    case Opcode::kMakeCheckedDict: {
      const auto& makedict = static_cast<const MakeCheckedDict&>(instr);
      return fmt::format("{} {}", makedict.type(), makedict.GetCapacity());
    }
    case Opcode::kMakeCheckedList: {
      const auto& makelist = static_cast<const MakeCheckedList&>(instr);
      return fmt::format("{} {}", makelist.type(), makelist.nvalues());
    }
    case Opcode::kMakeDict: {
      const auto& makedict = static_cast<const MakeDict&>(instr);
      return fmt::format("{}", makedict.GetCapacity());
    }
    case Opcode::kPhi: {
      const auto& phi = static_cast<const Phi&>(instr);
      std::stringstream ss;
      bool first = true;
      for (auto& bb : phi.basic_blocks()) {
        if (first) {
          first = false;
        } else {
          ss << ", ";
        }
        ss << bb->id;
      }

      return ss.str();
    }
    case Opcode::kDeleteAttr:
    case Opcode::kLoadAttr:
    case Opcode::kLoadAttrCached:
    case Opcode::kLoadModuleAttrCached:
    case Opcode::kStoreAttr:
    case Opcode::kStoreAttrCached: {
      const auto& named = static_cast<const DeoptBaseWithNameIdx&>(instr);
      return format_name(func, named, named.name_idx());
    }
    case Opcode::kInPlaceOp: {
      const auto& inplace_op = static_cast<const InPlaceOp&>(instr);
      return std::string{GetInPlaceOpName(inplace_op.op())};
    }
    case Opcode::kBuildSlice: {
      const auto& build_slice = static_cast<const BuildSlice&>(instr);
      return fmt::format("{}", build_slice.NumOperands());
    }
    case Opcode::kLoadTypeAttrCacheEntryType: {
      const auto& i = static_cast<const LoadTypeAttrCacheEntryType&>(instr);
      return fmt::format("{}", i.cache_id());
    }
    case Opcode::kLoadTypeAttrCacheEntryValue: {
      const auto& i = static_cast<const LoadTypeAttrCacheEntryValue&>(instr);
      return fmt::format("{}", i.cache_id());
    }
    case Opcode::kFillTypeAttrCache: {
      const auto& ftac = static_cast<const FillTypeAttrCache&>(instr);
      return fmt::format("{}, {}", ftac.cache_id(), ftac.name_idx());
    }
    case Opcode::kLoadTypeMethodCacheEntryValue: {
      const auto& i = static_cast<const LoadTypeMethodCacheEntryValue&>(instr);
      return fmt::format("{}", i.cache_id());
    }
    case Opcode::kLoadTypeMethodCacheEntryType: {
      const auto& i = static_cast<const LoadTypeMethodCacheEntryType&>(instr);
      return fmt::format("{}", i.cache_id());
    }
    case Opcode::kFillTypeMethodCache: {
      const auto& ftmc = static_cast<const FillTypeMethodCache&>(instr);
      return fmt::format("{}, {}", ftmc.cache_id(), ftmc.name_idx());
    }
    case Opcode::kSetFunctionAttr: {
      const auto& set_fn_attr = static_cast<const SetFunctionAttr&>(instr);
      return fmt::format("{}", functionFieldName(set_fn_attr.field()));
    }
    case Opcode::kCheckField:
    case Opcode::kCheckFreevar:
    case Opcode::kCheckVar: {
      const auto& check = static_cast<const CheckBaseWithName&>(instr);
      return escape_unicode(check.name());
    }
    case Opcode::kGuardIs: {
      const auto& gs = static_cast<const GuardIs&>(instr);
      return fmt::format("{}", getStablePointer(gs.target()));
    }
    case Opcode::kGuardType: {
      const auto& gs = static_cast<const GuardType&>(instr);
      return fmt::format("{}", gs.target().toString());
    }
    case Opcode::kHintType: {
      std::ostringstream os;
      auto profile_sep = "";
      const auto& hint = static_cast<const HintType&>(instr);
      os << fmt::format("{}, ", hint.NumOperands());
      for (auto types_seen : hint.seenTypes()) {
        os << fmt::format("{}<", profile_sep);
        auto type_sep = "";
        for (auto type : types_seen) {
          os << fmt::format("{}{}", type_sep, type.toString());
          type_sep = ", ";
        }
        os << ">";
        profile_sep = ", ";
      }
      return os.str();
    }
    case Opcode::kUseType: {
      const auto& gs = static_cast<const UseType&>(instr);
      return fmt::format("{}", gs.type().toString());
    }
    case Opcode::kRaiseAwaitableError: {
      const auto& ra = static_cast<const RaiseAwaitableError&>(instr);
      return ra.isAEnter() ? "__aenter__" : "__aexit__";
    }
    case Opcode::kRaiseStatic: {
      const auto& pyerr = static_cast<const RaiseStatic&>(instr);
      std::ostringstream os;
      print_reg_states(os, pyerr.live_regs());
      return fmt::format(
          "{}, \"{}\", <{}>",
          PyExceptionClass_Name(pyerr.excType()),
          pyerr.fmt(),
          os.str());
    }
    case Opcode::kImportFrom: {
      const auto& import_from = static_cast<const ImportFrom&>(instr);
      return format_name(func, import_from, import_from.name_idx());
    }
    case Opcode::kImportName: {
      const auto& import_name = static_cast<const ImportName&>(instr);
      return format_name(func, import_name, import_name.name_idx());
    }
    case Opcode::kEagerImportName: {
      const auto& eager_import_name =
          static_cast<const EagerImportName&>(instr);
      return format_name(func, eager_import_name, eager_import_name.name_idx());
    }
    case Opcode::kRefineType: {
      const auto& rt = static_cast<const RefineType&>(instr);
      return rt.type().toString();
    }
    case Opcode::kFormatValue: {
      int conversion = static_cast<const FormatValue&>(instr).conversion();
      return fvc_to_string(conversion);
    }
    case Opcode::kUnpackExToTuple: {
      const auto& i = static_cast<const UnpackExToTuple&>(instr);
      return fmt::format("{}, {}", i.before(), i.after());
    }
    case Opcode::kDeoptPatchpoint: {
      const auto& dp = static_cast<const DeoptPatchpoint&>(instr);
      auto patcher = dp.patcher();
      if (patcher->isLinked()) {
        return fmt::format(
            "{} -> {}",
            getStablePointer(patcher->patchpoint()),
            getStablePointer(patcher->jumpTarget()));
      }
      return fmt::format("Patcher {}", getStablePointer(dp.patcher()));
    }
    case Opcode::kUpdatePrevInstr: {
      const auto& upi = static_cast<const UpdatePrevInstr&>(instr);
      return fmt::format(
          "idx:{} line_no:{}: {}",
          upi.bytecodeOffset().asIndex(),
          upi.lineNo(),
          upi.parent() != nullptr ? "has parent" : "no parent");
    }
    case Opcode::kBuildInterpolation: {
      const auto& bi = static_cast<const BuildInterpolation&>(instr);
      return fvc_to_string(bi.conversion());
    }
    case Opcode::kLoadSpecial: {
#if PY_VERSION_HEX >= 0x030E0000
      const auto& ls = static_cast<const LoadSpecial&>(instr);
      switch (ls.specialIdx()) {
        case SPECIAL___ENTER__:
          return "__enter__";
        case SPECIAL___EXIT__:
          return "__exit__";
        case SPECIAL___AENTER__:
          return "__aenter__";
        case SPECIAL___AEXIT__:
          return "__aexit__";
        default:
          JIT_ABORT("Unknown special index: {}", ls.specialIdx());
      }
#else
      JIT_ABORT("LoadSpecial not supported before 3.14");
#endif
    }
    case Opcode::kConvertValue: {
      const auto& cv = static_cast<const ConvertValue&>(instr);
      return fvc_to_string(cv.converterIdx());
    }
  }
  JIT_ABORT("Invalid opcode {}", static_cast<int>(instr.opcode()));
}

void HIRPrinter::Print(std::ostream& os, const Instr& instr) {
  Indented(os);
  if (Register* dst = instr.output()) {
    os << dst->name();
    if (dst->type() != TTop) {
      os << ":" << dst->type();
    }
    os << " = ";
  }
  os << instr.opname();

  auto immed = format_immediates(func_, instr);
  if (!immed.empty()) {
    os << "<" << immed << ">";
  }
  for (size_t i = 0, n = instr.NumOperands(); i < n; ++i) {
    auto op = instr.GetOperand(i);
    if (op != nullptr) {
      os << " " << op->name();
    } else {
      os << " nullptr";
    }
  }

  if (instr.IsSnapshot() && !full_snapshots_) {
    return;
  }
  auto fs = get_frame_state(instr);
  auto db = instr.asDeoptBase();
  if (db != nullptr) {
    os << " {\n";
    Indent();
    if (!db->descr().empty()) {
      Indented(os) << fmt::format("Descr '{}'\n", db->descr());
    }
    if (Register* guilty_reg = db->guiltyReg()) {
      Indented(os) << fmt::format("GuiltyReg {}\n", *guilty_reg);
    }
    if (db->live_regs().size() > 0) {
      Indented(os) << "LiveValues";
      print_reg_states(os, db->live_regs());
      os << '\n';
    }
    if (fs != nullptr) {
      Indented(os) << "FrameState {\n";
      Indent();
      Print(os, *fs);
      Dedent();
      Indented(os) << "}\n";
    }
    Dedent();
    Indented(os) << "}";
  } else if (fs != nullptr) {
    os << " {\n";
    Indent();
    Print(os, *fs);
    Dedent();
    Indented(os) << "}";
  }
}

void HIRPrinter::Print(std::ostream& os, const FrameState& state) {
  Indented(os) << "CurInstrOffset " << state.cur_instr_offs << '\n';

  auto nlocals = state.nlocals;
  if (nlocals > 0) {
    Indented(os) << "Locals<" << nlocals << ">";
    for (int i = 0; i < nlocals; ++i) {
      auto reg = state.localsplus[i];
      if (reg == nullptr) {
        os << " <null>";
      } else {
        os << " " << reg->name();
      }
    }
    os << '\n';
  }

  auto nlocalsplus = state.localsplus.size();
  auto ncells = nlocalsplus - state.nlocals;
  if (ncells > 0) {
    Indented(os) << "Cells<" << ncells << ">";
    for (int i = nlocals; i < nlocalsplus; ++i) {
      auto reg = state.localsplus[i];
      if (reg == nullptr) {
        os << " <null>";
      } else {
        os << " " << reg->name();
      }
    }
    os << '\n';
  }

  auto opstack_size = state.stack.size();
  if (opstack_size > 0) {
    Indented(os) << "Stack<" << opstack_size << ">";
    for (std::size_t i = 0; i < opstack_size; i++) {
      os << " " << state.stack.at(i)->name();
    }
    os << '\n';
  }

  auto& bs = state.block_stack;
  if (!bs.isEmpty()) {
    Indented(os) << "BlockStack {\n";
    Indent();
    for (const auto& entry : bs) {
      Indented(os) << fmt::format(
          "Opcode {} HandlerOff {} StackLevel {}\n",
          entry.opcode,
          entry.handler_off,
          entry.stack_level);
    }
    Dedent();
    Indented(os) << "}" << '\n';
  }
}

HIRPrinter& HIRPrinter::setFullSnapshots(bool full) {
  full_snapshots_ = full;
  return *this;
}

HIRPrinter& HIRPrinter::setLinePrefix(std::string_view prefix) {
  line_prefix_ = std::string{prefix};
  return *this;
}

std::ostream& operator<<(std::ostream& os, const Function& func) {
  HIRPrinter{}.Print(os, func);
  return os;
}

std::ostream& operator<<(std::ostream& os, const CFG& cfg) {
  HIRPrinter{}.Print(os, cfg);
  return os;
}

std::ostream& operator<<(std::ostream& os, const BasicBlock& block) {
  HIRPrinter{}.Print(os, block);
  return os;
}

std::ostream& operator<<(std::ostream& os, const Instr& instr) {
  HIRPrinter{}.Print(os, instr);
  return os;
}

std::ostream& operator<<(std::ostream& os, const FrameState& state) {
  HIRPrinter{}.Print(os, state);
  return os;
}

} // namespace jit::hir
