// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/hir/printer.h"

#include <Python.h>

#include "cinderx/Common/code.h"
#include "cinderx/Common/util.h"
#include "cinderx/Jit/symbolizer.h"
#include "cinderx/Upgrade/upgrade_stubs.h" // @donotremove

#include <fmt/format.h>
#include <fmt/ostream.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <vector>

namespace jit::hir {

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
  fmt::print(
      os, "fun {} {{\n", func.fullname.empty() ? "<unknown>" : func.fullname);
  Indent();
  Print(os, func.cfg);
  Dedent();
  os << "}" << std::endl;
}

void HIRPrinter::Print(std::ostream& os, const CFG& cfg) {
  return Print(os, cfg, cfg.entry_block);
}

void HIRPrinter::Print(std::ostream& os, const CFG& cfg, BasicBlock* start) {
  std::vector<BasicBlock*> blocks = cfg.GetRPOTraversal(start);
  auto last_block = blocks.back();
  for (auto block : blocks) {
    Print(os, *block);
    if (block != last_block) {
      os << std::endl;
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
    Print(os, instr, full_snapshots_);
    os << std::endl;
  }
  Dedent();
  Indented(os) << "}" << std::endl;
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

static std::string escape_non_ascii(const std::string& str) {
  std::string result;
  for (size_t i = 0; i < str.size(); ++i) {
    unsigned char c = str[i];
    if (c > kMaxASCII) {
      result += '\\';
      result += std::to_string(c);
    } else {
      result += static_cast<char>(c);
    }
  }
  return result;
}

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

static std::string format_name(const Instr& instr, int idx) {
  auto code = instr.code();
  if (idx < 0 || code == nullptr) {
    return fmt::format("{}", idx);
  }

  return format_name_impl(idx, code->co_names);
}

static std::string format_load_super(const LoadSuperBase& load) {
  auto code = load.code();
  if (code == nullptr) {
    return fmt::format("{} {}", load.name_idx(), load.no_args_in_super_call());
  }
  return fmt::format(
      "{}, {}",
      format_name_impl(load.name_idx(), code->co_names),
      load.no_args_in_super_call());
}

static std::string format_varname(const Instr& instr, int idx) {
  auto code = instr.code();
  if (idx < 0 || code == nullptr) {
    return fmt::format("{}", idx);
  }

  auto names = getVarnameTuple(code, &idx);
  return format_name_impl(idx, names);
}

static std::string format_immediates(const Instr& instr) {
  switch (instr.opcode()) {
    case Opcode::kAssign:
    case Opcode::kBatchDecref:
    case Opcode::kBitCast:
    case Opcode::kBuildString:
    case Opcode::kCheckErrOccurred:
    case Opcode::kCheckExc:
    case Opcode::kCheckNeg:
    case Opcode::kCheckSequenceBounds:
    case Opcode::kCopyDictWithoutKeys:
    case Opcode::kDecref:
    case Opcode::kDeleteSubscr:
    case Opcode::kDeopt:
    case Opcode::kDictMerge:
    case Opcode::kDictSubscr:
    case Opcode::kDictUpdate:
    case Opcode::kEndInlinedFunction:
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
      return call.funcName();
    }
    case Opcode::kCallIntrinsic: {
      const auto& call = static_cast<const CallIntrinsic&>(instr);
      return fmt::format("{}", call.index());
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
      auto varname = format_varname(load, load.arg_idx());
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
      return format_name(load, load.name_idx());
    }
    case Opcode::kLoadMethodSuper: {
      return format_load_super(static_cast<const LoadSuperBase&>(instr));
    }
    case Opcode::kLoadAttrSuper: {
      return format_load_super(static_cast<const LoadSuperBase&>(instr));
    }
    case Opcode::kLoadConst: {
      const auto& load = static_cast<const LoadConst&>(instr);
      return fmt::format("{}", load.type());
    }
    case Opcode::kLoadFunctionIndirect: {
      const auto& load = static_cast<const LoadFunctionIndirect&>(instr);
      PyObject* func = *load.funcptr();
      const char* name;
      if (PyFunction_Check(func)) {
        name = PyUnicode_AsUTF8(((PyFunctionObject*)func)->func_name);
      } else {
        name = Py_TYPE(func)->tp_name;
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
      return format_name(load, load.name_idx());
    }
    case Opcode::kLoadGlobal: {
      const auto& load = static_cast<const LoadGlobal&>(instr);
      return format_name(load, load.name_idx());
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
      return format_name(named, named.name_idx());
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
      return format_name(import_from, import_from.name_idx());
    }
    case Opcode::kImportName: {
      const auto& import_name = static_cast<const ImportName&>(instr);
      return format_name(import_name, import_name.name_idx());
    }
    case Opcode::kEagerImportName: {
      const auto& eager_import_name =
          static_cast<const EagerImportName&>(instr);
      return format_name(eager_import_name, eager_import_name.name_idx());
    }
    case Opcode::kRefineType: {
      const auto& rt = static_cast<const RefineType&>(instr);
      return rt.type().toString();
    }
    case Opcode::kFormatValue: {
      int conversion = static_cast<const FormatValue&>(instr).conversion();
      switch (conversion) {
        case FVC_NONE:
          return "None";
        case FVC_STR:
          return "Str";
        case FVC_REPR:
          return "Repr";
        case FVC_ASCII:
          return "ASCII";
      }
      JIT_ABORT("Unknown conversion type.");
    }
    case Opcode::kUnpackExToTuple: {
      const auto& i = static_cast<const UnpackExToTuple&>(instr);
      return fmt::format("{}, {}", i.before(), i.after());
    }
    case Opcode::kDeoptPatchpoint: {
      const auto& dp = static_cast<const DeoptPatchpoint&>(instr);
      return fmt::format("{}", getStablePointer(dp.patcher()));
    }
    case Opcode::kUpdatePrevInstr: {
      const auto& upi = static_cast<const UpdatePrevInstr&>(instr);
      return fmt::format(
          "idx:{} line_no:{}", upi.bytecodeOffset().asIndex(), upi.lineNo());
    }
  }
  JIT_ABORT("Invalid opcode {}", static_cast<int>(instr.opcode()));
}

void HIRPrinter::Print(
    std::ostream& os,
    const Instr& instr,
    bool full_snapshots) {
  Indented(os);
  if (Register* dst = instr.output()) {
    os << dst->name();
    if (dst->type() != TTop) {
      os << ":" << dst->type();
    }
    os << " = ";
  }
  os << instr.opname();

  auto immed = format_immediates(instr);
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

  if (instr.IsSnapshot() && !full_snapshots) {
    return;
  }
  auto fs = get_frame_state(instr);
  auto db = instr.asDeoptBase();
  if (db != nullptr) {
    os << " {" << std::endl;
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
      os << std::endl;
    }
    if (fs != nullptr) {
      Indented(os) << "FrameState {" << std::endl;
      Indent();
      Print(os, *fs);
      Dedent();
      Indented(os) << "}" << std::endl;
    }
    Dedent();
    Indented(os) << "}";
  } else if (fs != nullptr) {
    os << " {" << std::endl;
    Indent();
    Print(os, *fs);
    Dedent();
    Indented(os) << "}";
  }
}

void HIRPrinter::Print(std::ostream& os, const FrameState& state) {
  Indented(os) << "CurInstrOffset " << state.cur_instr_offs << std::endl;

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
    os << std::endl;
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
    os << std::endl;
  }

  auto opstack_size = state.stack.size();
  if (opstack_size > 0) {
    Indented(os) << "Stack<" << opstack_size << ">";
    for (std::size_t i = 0; i < opstack_size; i++) {
      os << " " << state.stack.at(i)->name();
    }
    os << std::endl;
  }

  auto& bs = state.block_stack;
  if (bs.size() > 0) {
    Indented(os) << "BlockStack {" << std::endl;
    Indent();
    for (std::size_t i = 0; i < bs.size(); i++) {
      auto& entry = bs.at(i);
      Indented(os) << fmt::format(
          "Opcode {} HandlerOff {} StackLevel {}\n",
          entry.opcode,
          entry.handler_off,
          entry.stack_level);
    }
    Dedent();
    Indented(os) << "}" << std::endl;
  }
}

static int lastLineNumber(PyCodeObject* code) {
  int last_line = -1;
  BytecodeInstructionBlock bc_block{code};
  for (const BytecodeInstruction& bc_instr : bc_block) {
    BCOffset bc_off = bc_instr.offset();
    last_line = std::max(last_line, PyCode_Addr2Line(code, bc_off.value()));
  }
  return last_line;
}

nlohmann::json JSONPrinter::PrintSource(const Function& func) {
  PyCodeObject* code = func.code;
  if (code == nullptr) {
    // No code; must be from a test;
    return nlohmann::json();
  }
  PyObject* co_filename = code->co_filename;
  JIT_CHECK(co_filename != nullptr, "filename must not be null");
  const char* filename = PyUnicode_AsUTF8(co_filename);
  if (filename == nullptr) {
    PyErr_Clear();
    return nlohmann::json();
  }
  std::ifstream infile(filename);
  if (!infile) {
    // TODO(emacs): Does this handle nonexistent files?
    return nlohmann::json();
  }
  nlohmann::json result;
  result["name"] = "Source";
  result["type"] = "text";
  result["filename"] = filename;
  int start = code->co_firstlineno;
  result["first_line_number"] = start;
  nlohmann::json lines;
  int end = lastLineNumber(code);
  std::string line;
  int count = 0;
  while (std::getline(infile, line)) {
    count++;
    if (count > end) {
      break;
    } // done
    if (count < start) {
      continue;
    } // too early
    lines.emplace_back(line);
  }
  infile.close();
  result["lines"] = lines;
  return result;
}

// TODO(emacs): Fake line numbers using instruction addresses to better
// associate how instructions change over time
// TODO(emacs): Make JSON utils so we stop copying so much stuff around

constexpr std::string_view opname(unsigned opcode) {
  // HAVE_ARGUMENT is a delimiter, it's not a real opcode.  It aliases
  // STORE_NAME.
  if (opcode == HAVE_ARGUMENT) {
    static_assert(HAVE_ARGUMENT == STORE_NAME);
    return "STORE_NAME";
  }

  // Can't use switch statement because the compiler will complain about
  // duplicate values between HAVE_ARGUMENT/STORE_NAME.
#define CASE(NAME, CODE)  \
  if (opcode == (CODE)) { \
    return #NAME;         \
  }
#undef CASE

  return "<Unknown opcode>";
}

static std::string
reprArg(PyCodeObject* code, unsigned char opcode, unsigned char oparg) {
  ThreadedCompileSerialize guard;
  switch (opcode) {
    case BUILD_CHECKED_LIST:
    case BUILD_CHECKED_MAP:
    case CAST:
    case INVOKE_FUNCTION:
    case INVOKE_METHOD:
    case LOAD_ATTR_SUPER:
    case LOAD_CLASS:
    case LOAD_CONST:
    case LOAD_FIELD:
    case LOAD_LOCAL:
    case LOAD_METHOD_SUPER:
    case LOAD_TYPE:
    case PRIMITIVE_LOAD_CONST:
    case REFINE_TYPE:
    case STORE_FIELD:
    case STORE_LOCAL:
    case TP_ALLOC: {
      PyObject* const_obj = PyTuple_GetItem(code->co_consts, oparg);
      JIT_DCHECK(const_obj != nullptr, "bad constant");
      PyObject* repr = PyObject_Repr(const_obj);
      if (repr == nullptr) {
        PyErr_Clear();
        return fmt::format("{}: (error printing constant)", oparg);
      }
      const char* str = PyUnicode_AsUTF8(repr);
      if (str == nullptr) {
        PyErr_Clear();
        return fmt::format("{}: (error printing constant)", oparg);
      }
      return fmt::format("{}: {}", oparg, str);
    }
    case LOAD_FAST:
    case STORE_FAST:
    case DELETE_FAST: {
      PyObject* name_obj = jit::getVarname(code, oparg);
      JIT_DCHECK(name_obj != nullptr, "bad name");
      const char* name = PyUnicode_AsUTF8(name_obj);
      if (name == nullptr) {
        PyErr_Clear();
        return fmt::format("{}: (error printing varname)", oparg);
      }
      return fmt::format("{}: {}", oparg, name);
    }
    case LOAD_DEREF:
    case STORE_DEREF:
    case DELETE_DEREF: {
      PyObject* name_obj;
      if (oparg < PyTuple_GET_SIZE(PyCode_GetCellvars(code))) {
        name_obj = PyTuple_GetItem(PyCode_GetCellvars(code), oparg);
      } else {
        name_obj = PyTuple_GetItem(
            PyCode_GetFreevars(code),
            oparg - PyTuple_GET_SIZE(PyCode_GetCellvars(code)));
      }
      JIT_DCHECK(name_obj != nullptr, "bad name");
      const char* name = PyUnicode_AsUTF8(name_obj);
      if (name == nullptr) {
        PyErr_Clear();
        return fmt::format("{}: (error printing freevar)", oparg);
      }
      return fmt::format("{}: {}", oparg, name);
    }
    case LOAD_ATTR:
    case STORE_ATTR:
    case DELETE_ATTR:
    case LOAD_METHOD:
    case LOAD_GLOBAL:
    case STORE_GLOBAL:
    case DELETE_GLOBAL: {
      int name_idx = (opcode == LOAD_ATTR)
          ? loadAttrIndex(oparg)
          : (opcode == LOAD_GLOBAL ? loadGlobalIndex(oparg) : oparg);
      PyObject* name_obj = PyTuple_GetItem(code->co_names, name_idx);
      JIT_DCHECK(name_obj != nullptr, "bad name");
      const char* name = PyUnicode_AsUTF8(name_obj);
      if (name == nullptr) {
        PyErr_Clear();
        return fmt::format("{}: (error printing name)", name_idx);
      }
      return fmt::format("{}: {}", name_idx, name);
    }
    default:
      return std::to_string(oparg);
  }
  JIT_ABORT("unreachable");
}

// TODO(emacs): Write basic blocks for bytecode (using BytecodeInstructionBlock
// and BlockMap?). Need to figure out how to allocate block IDs without
// actually modifying the HIR function in place.
nlohmann::json JSONPrinter::PrintBytecode(const Function& func) {
  nlohmann::json result;
  result["name"] = "Bytecode";
  result["type"] = "asm";
  nlohmann::json block;
  block["name"] = "bb0";
  nlohmann::json instrs_json = nlohmann::json::array();
  PyCodeObject* code = func.code;
  _Py_CODEUNIT* instrs = (_Py_CODEUNIT*)PyBytes_AS_STRING(PyCode_GetCode(code));
  Py_ssize_t num_instrs =
      PyBytes_Size(PyCode_GetCode(code)) / sizeof(_Py_CODEUNIT);
  for (Py_ssize_t i = 0, off = 0; i < num_instrs;
       i++, off += sizeof(_Py_CODEUNIT)) {
    unsigned char opcode = _Py_OPCODE(instrs[i]);
    unsigned char oparg = _Py_OPARG(instrs[i]);
    nlohmann::json instr;
    instr["address"] = off;
    instr["line"] = PyCode_Addr2Line(code, off);
    instr["opcode"] =
        fmt::format("{} {}", opname(opcode), reprArg(code, opcode, oparg));
    instrs_json.emplace_back(instr);
  }
  block["instrs"] = instrs_json;
  result["blocks"] = nlohmann::json::array({block});
  return result;
}

nlohmann::json JSONPrinter::Print(const Instr& instr) {
  nlohmann::json result;
  result["line"] = instr.lineNumber();
  Register* output = instr.output();
  if (output != nullptr) {
    result["output"] = output->name();
    if (output->type() != TTop) {
      // Output must be escaped since literal Python values such as \222 can be
      // in the type.
      result["type"] = escape_non_ascii(output->type().toString());
    }
  }
  auto opcode = std::string{instr.opname()};
  auto immed = format_immediates(instr);
  if (!immed.empty()) {
    // Output must be escaped since literal Python values such as \222 can be
    // in the type.
    opcode += "<" + escape_non_ascii(immed) + ">";
  }
  result["opcode"] = opcode;
  nlohmann::json operands = nlohmann::json::array();
  for (size_t i = 0, n = instr.NumOperands(); i < n; i++) {
    auto op = instr.GetOperand(i);
    if (op != nullptr) {
      operands.emplace_back(op->name());
    } else {
      operands.emplace_back(nlohmann::json());
    }
  }
  if (instr.bytecodeOffset() != -1) {
    result["bytecode_offset"] = instr.bytecodeOffset().value();
  }
  result["operands"] = operands;
  return result;
}

nlohmann::json JSONPrinter::Print(const BasicBlock& block) {
  nlohmann::json result;
  result["name"] = fmt::format("bb{}", block.id);
  auto& in_edges = block.in_edges();
  nlohmann::json preds = nlohmann::json::array();
  if (!in_edges.empty()) {
    std::vector<const Edge*> edges(in_edges.begin(), in_edges.end());
    std::sort(edges.begin(), edges.end(), [](auto& e1, auto& e2) {
      return e1->from()->id < e2->from()->id;
    });
    for (auto edge : edges) {
      preds.emplace_back(fmt::format("bb{}", edge->from()->id));
    }
  }
  result["preds"] = preds;
  nlohmann::json instrs = nlohmann::json::array();
  for (auto& instr : block) {
    if (instr.IsSnapshot()) {
      continue;
    }
    if (instr.IsTerminator()) {
      // Handle specially below
      break;
    }
    instrs.emplace_back(Print(instr));
  }
  result["instrs"] = instrs;
  const Instr* instr = block.GetTerminator();
  JIT_CHECK(instr != nullptr, "expected terminator");
  result["terminator"] = Print(*instr);
  nlohmann::json succs = nlohmann::json::array();
  for (std::size_t i = 0; i < instr->numEdges(); i++) {
    BasicBlock* succ = instr->successor(i);
    succs.emplace_back(fmt::format("bb{}", succ->id));
  }
  result["succs"] = succs;
  return result;
}

nlohmann::json JSONPrinter::Print(const CFG& cfg) {
  std::vector<BasicBlock*> blocks = cfg.GetRPOTraversal();
  nlohmann::json result;
  for (auto block : blocks) {
    result.emplace_back(Print(*block));
  }
  return result;
}

void DebugPrint(const Function& func) {
  HIRPrinter(true).Print(std::cout, func);
}

void DebugPrint(const CFG& cfg) {
  HIRPrinter(true).Print(std::cout, cfg);
}

void DebugPrint(const BasicBlock& block) {
  HIRPrinter(true).Print(std::cout, block);
}

void DebugPrint(const Instr& instr) {
  HIRPrinter(true).Print(std::cout, instr);
}

} // namespace jit::hir
