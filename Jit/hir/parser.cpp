// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/hir/parser.h"

#include "cinderx/Common/log.h"
#include "cinderx/Common/ref.h"
#include "cinderx/Jit/hir/hir.h"
#include "cinderx/StaticPython/classloader.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

namespace jit::hir {

static auto nameToType() {
  std::unordered_map<std::string_view, Type> map{
#define TY(name, ...) {#name, T##name},
      HIR_TYPES(TY)
#undef TY
  };
  return map;
}

#define NEW_INSTR(type, ...)              \
  auto instr = type::create(__VA_ARGS__); \
  instruction = instr;

void HIRParser::expect(std::string_view expected) {
  std::string_view actual = GetNextToken();
  if (expected != actual) {
    JIT_LOG("Expected \"{}\", but got \"{}\"", expected, actual);
    std::abort();
  }
}

Register* HIRParser::allocateRegister(std::string_view name) {
  JIT_CHECK(
      name[0] == 'v', "invalid register name (must be v[0-9]+): {}", name);
  auto opt_id = parseInt<int>(name.substr(1));
  JIT_CHECK(
      opt_id.has_value(), "Cannot parse register '{}' into an integer", name);
  auto id = *opt_id;

  auto reg = env_->getRegister(id);
  if (reg == nullptr) {
    reg = env_->addRegister(std::make_unique<Register>(id));
  }

  max_reg_id_ = std::max(max_reg_id_, id);
  return reg;
}

Type HIRParser::parseType(std::string_view str) {
  static auto const name_types = nameToType();

  std::string_view spec_string;
  auto open_bracket = str.find('[');
  if (open_bracket != std::string::npos) {
    auto close_bracket = str.find(']');
    auto spec_len = close_bracket - (open_bracket + 1);
    if (close_bracket == std::string::npos || spec_len < 1) {
      return TBottom;
    }
    spec_string = str.substr(open_bracket + 1, spec_len);
    str = str.substr(0, open_bracket);
  }

  auto it = name_types.find(str);
  if (it == name_types.end()) {
    return TBottom;
  }

  Type base = it->second;
  if (spec_string.empty()) {
    return base;
  }

  if (base <= TCBool) {
    if (spec_string == "true") {
      return Type::fromCBool(true);
    }
    if (spec_string == "false") {
      return Type::fromCBool(false);
    }
    return TBottom;
  }

  if (base <= TBool) {
    if (spec_string == "True") {
      return Type::fromObject(Py_True);
    }
    if (spec_string == "False") {
      return Type::fromObject(Py_False);
    }
    return TBottom;
  }

  if (base <= TLong) {
    JIT_CHECK(
        Py_IsInitialized(),
        "Python runtime must be initialized for the HIR parser to parse "
        "PyObject*s (can't parse '{}')",
        str);
    JIT_CHECK(
        env_ != nullptr,
        "HIR Environment must be initialized for the HIR parser to allocate "
        "PyObject*s (can't parse '{}')",
        str);
    auto spec_value = parseInt<intptr_t>(spec_string);
    if (!spec_value.has_value()) {
      return TBottom;
    }

    auto result = Ref<>::steal(PyLong_FromLong(*spec_value));
    return Type::fromObject(env_->addReference(std::move(result)));
  }

  std::optional<intptr_t> spec_value;
  if (base <= TCInt8 || base <= TCInt16 || base <= TCInt32 || base <= TCInt64) {
    spec_value = parseInt<intptr_t>(spec_string);
  } else if (
      base <= TCUInt8 || base <= TCUInt16 || base <= TCUInt32 ||
      base <= TCUInt64) {
    spec_value = parseInt<intptr_t>(spec_string);
  } else {
    return TBottom;
  }

  if (!spec_value.has_value()) {
    return TBottom;
  }
  return Type{
      base.bits_, Type::kLifetimeBottom, Type::SpecKind::kSpecInt, *spec_value};
}

Register* HIRParser::ParseRegister() {
  std::string_view name = GetNextToken();
  return allocateRegister(name);
}

HIRParser::ListOrTuple HIRParser::parseListOrTuple() {
  std::string_view kind = GetNextToken();
  if (kind == "list") {
    return ListOrTuple::List;
  }
  if (kind == "tuple") {
    return ListOrTuple::Tuple;
  }
  JIT_ABORT("Invalid kind {}, expected list or tuple", kind);
}

Instr*
HIRParser::parseInstr(std::string_view opcode, Register* dst, int bb_index) {
  Instr* instruction = nullptr;
  if (opcode == "Branch") {
    NEW_INSTR(Branch, nullptr);
    expect("<");
    branches_.emplace(instr, GetNextInteger());
    expect(">");
  } else if (opcode == "VectorCall") {
    expect("<");
    int num_args = GetNextInteger();
    auto flags = CallFlags::None;
    while (peekNextToken() != ">") {
      expect(",");
      std::string_view tok = GetNextToken();
      if (tok == "awaited") {
        flags |= CallFlags::Awaited;
      } else if (tok == "kwnames") {
        flags |= CallFlags::KwArgs;
      } else if (tok == "static") {
        flags |= CallFlags::Static;
      } else {
        JIT_ABORT("Unexpected VectorCall immediate '{}'", tok);
      }
    }
    expect(">");
    auto func = ParseRegister();
    std::vector<Register*> args(num_args);
    std::generate(
        args.begin(),
        args.end(),
        std::bind(std::mem_fn(&HIRParser::ParseRegister), this));

    instruction = newInstr<VectorCall>(num_args + 1, dst, flags);
    instruction->SetOperand(0, func);
    for (int i = 0; i < num_args; i++) {
      instruction->SetOperand(i + 1, args[i]);
    }
  } else if (opcode == "FormatValue") {
    expect("<");
    auto tok = GetNextToken();
    auto conversion = [&] {
      if (tok == "None") {
        return FVC_NONE;
      } else if (tok == "Str") {
        return FVC_STR;
      } else if (tok == "Repr") {
        return FVC_REPR;
      } else if (tok == "ASCII") {
        return FVC_ASCII;
      }
      JIT_ABORT("Bad FormatValue conversion type: {}", tok);
    }();
    expect(">");
    Register* fmt_spec = ParseRegister();
    Register* val = ParseRegister();
    instruction = newInstr<FormatValue>(dst, fmt_spec, val, conversion);
  } else if (opcode == "CallEx") {
    auto flags = CallFlags::None;
    if (peekNextToken() == "<") {
      expect("<");
      while (peekNextToken() != ">") {
        auto tok = GetNextToken();
        if (tok == "awaited") {
          flags |= CallFlags::Awaited;
        } else if (tok == "kwargs") {
          flags |= CallFlags::KwArgs;
        }
        if (peekNextToken() == ",") {
          expect(",");
        }
      }
      expect(">");
    }
    Register* func = ParseRegister();
    Register* pargs = ParseRegister();
    Register* kwargs = ParseRegister();
    instruction = newInstr<CallEx>(dst, func, pargs, kwargs, flags);
  } else if (opcode == "ImportFrom") {
    expect("<");
    int name_idx = GetNextInteger();
    expect(">");
    Register* module = ParseRegister();
    instruction = newInstr<ImportFrom>(dst, module, name_idx);
  } else if (opcode == "ImportName") {
    expect("<");
    int name_idx = GetNextInteger();
    expect(">");
    Register* fromlist = ParseRegister();
    Register* level = ParseRegister();
    instruction = newInstr<ImportName>(dst, name_idx, fromlist, level);
  } else if (opcode == "EagerImportName") {
    expect("<");
    int name_idx = GetNextInteger();
    expect(">");
    Register* fromlist = ParseRegister();
    Register* level = ParseRegister();
    instruction = newInstr<EagerImportName>(dst, name_idx, fromlist, level);
  } else if (opcode == "MakeList") {
    expect("<");
    int nvalues = GetNextInteger();
    expect(">");
    std::vector<Register*> args(nvalues);
    std::generate(
        args.begin(),
        args.end(),
        std::bind(std::mem_fn(&HIRParser::ParseRegister), this));
    instruction = newInstr<MakeList>(nvalues, dst, args);
  } else if (opcode == "MakeTuple") {
    expect("<");
    int nvalues = GetNextInteger();
    expect(">");
    std::vector<Register*> args(nvalues);
    std::generate(
        args.begin(),
        args.end(),
        std::bind(std::mem_fn(&HIRParser::ParseRegister), this));
    instruction = newInstr<MakeTuple>(nvalues, dst, args);
  } else if (opcode == "MakeSet") {
    NEW_INSTR(MakeSet, dst);
  } else if (opcode == "SetSetItem") {
    auto receiver = ParseRegister();
    auto item = ParseRegister();
    NEW_INSTR(SetSetItem, dst, receiver, item);
  } else if (opcode == "SetUpdate") {
    auto receiver = ParseRegister();
    auto item = ParseRegister();
    NEW_INSTR(SetUpdate, dst, receiver, item);
  } else if (opcode == "LoadArg") {
    expect("<");
    int idx = GetNextNameIdx();
    Type ty = TObject;
    if (peekNextToken() == ",") {
      expect(",");
      ty = parseType(GetNextToken());
    }
    expect(">");
    NEW_INSTR(LoadArg, dst, idx, ty);
  } else if (opcode == "LoadMethod") {
    expect("<");
    int idx = GetNextNameIdx();
    expect(">");
    auto receiver = ParseRegister();
    instruction = newInstr<LoadMethod>(dst, receiver, idx);
  } else if (opcode == "LoadMethodCached") {
    expect("<");
    int idx = GetNextNameIdx();
    expect(">");
    auto receiver = ParseRegister();
    instruction = newInstr<LoadMethodCached>(dst, receiver, idx);
  } else if (opcode == "LoadTupleItem") {
    expect("<");
    int idx = GetNextNameIdx();
    expect(">");
    auto receiver = ParseRegister();
    NEW_INSTR(LoadTupleItem, dst, receiver, idx);
  } else if (opcode == "CallMethod") {
    expect("<");
    int num_args = GetNextInteger();
    auto flags = CallFlags::None;
    if (peekNextToken() == ",") {
      expect(",");
      expect("awaited");
      flags |= CallFlags::Awaited;
    }
    expect(">");
    std::vector<Register*> args(num_args);
    std::generate(
        args.begin(),
        args.end(),
        std::bind(std::mem_fn(&HIRParser::ParseRegister), this));
    instruction = newInstr<CallMethod>(args.size(), dst, flags);
    for (std::size_t i = 0; i < args.size(); i++) {
      instruction->SetOperand(i, args[i]);
    }
  } else if (opcode == "CondBranch") {
    expect("<");
    auto true_bb = GetNextInteger();
    expect(",");
    auto false_bb = GetNextInteger();
    expect(">");
    auto var = ParseRegister();
    NEW_INSTR(CondBranch, var, nullptr, nullptr);
    cond_branches_.emplace(
        std::piecewise_construct,
        std::forward_as_tuple(instr),
        std::forward_as_tuple(true_bb, false_bb));
  } else if (opcode == "CondBranchCheckType") {
    expect("<");
    auto true_bb = GetNextInteger();
    expect(",");
    auto false_bb = GetNextInteger();
    expect(",");
    Type ty = parseType(GetNextToken());
    expect(">");
    auto var = ParseRegister();
    NEW_INSTR(CondBranchCheckType, var, ty, nullptr, nullptr);
    cond_branches_.emplace(
        std::piecewise_construct,
        std::forward_as_tuple(instr),
        std::forward_as_tuple(true_bb, false_bb));
  } else if (opcode == "Decref") {
    auto var = ParseRegister();
    NEW_INSTR(Decref, var);
  } else if (opcode == "Incref") {
    auto var = ParseRegister();
    NEW_INSTR(Incref, var);
  } else if (opcode == "LoadAttr") {
    expect("<");
    int idx = GetNextNameIdx();
    expect(">");
    auto receiver = ParseRegister();
    instruction = newInstr<LoadAttr>(dst, receiver, idx);
  } else if (opcode == "LoadAttrCached") {
    expect("<");
    int idx = GetNextNameIdx();
    expect(">");
    auto receiver = ParseRegister();
    instruction = newInstr<LoadAttrCached>(dst, receiver, idx);
  } else if (opcode == "LoadConst") {
    expect("<");
    Type ty = parseType(GetNextToken());
    expect(">");
    NEW_INSTR(LoadConst, dst, ty);
  } else if (opcode == "LoadGlobal") {
    expect("<");
    int name_idx = GetNextNameIdx();
    expect(">");
    instruction = newInstr<LoadGlobal>(dst, name_idx);
  } else if (opcode == "LoadGlobalCached") {
    expect("<");
    int name_idx = GetNextNameIdx();
    expect(">");
    instruction = LoadGlobalCached::create(
        dst,
        /*code=*/nullptr,
        /*builtins=*/nullptr,
        /*globals=*/nullptr,
        name_idx);
  } else if (opcode == "StoreAttr") {
    expect("<");
    int idx = GetNextNameIdx();
    expect(">");
    auto receiver = ParseRegister();
    auto value = ParseRegister();
    instruction = newInstr<StoreAttr>(receiver, value, idx);
  } else if (opcode == "StoreAttrCached") {
    expect("<");
    int idx = GetNextNameIdx();
    expect(">");
    auto receiver = ParseRegister();
    auto value = ParseRegister();
    instruction = newInstr<StoreAttrCached>(receiver, value, idx);
  } else if (opcode == "GetLength") {
    auto container = ParseRegister();
    NEW_INSTR(GetLength, dst, container, FrameState{});
  } else if (opcode == "DeleteSubscr") {
    auto container = ParseRegister();
    auto sub = ParseRegister();
    newInstr<DeleteSubscr>(container, sub);
  } else if (opcode == "DictSubscr") {
    auto dict = ParseRegister();
    auto key = ParseRegister();
    NEW_INSTR(DictSubscr, dst, dict, key, FrameState{});
  } else if (opcode == "StoreSubscr") {
    auto receiver = ParseRegister();
    auto index = ParseRegister();
    auto value = ParseRegister();
    NEW_INSTR(StoreSubscr, receiver, index, value, FrameState{});
  } else if (opcode == "Assign") {
    auto src = ParseRegister();
    NEW_INSTR(Assign, dst, src);
  } else if (opcode == "BinaryOp") {
    expect("<");
    BinaryOpKind op = ParseBinaryOpName(GetNextToken());
    expect(">");
    auto left = ParseRegister();
    auto right = ParseRegister();
    instruction = newInstr<BinaryOp>(dst, op, left, right);
  } else if (opcode == "LongBinaryOp") {
    expect("<");
    BinaryOpKind op = ParseBinaryOpName(GetNextToken());
    expect(">");
    auto left = ParseRegister();
    auto right = ParseRegister();
    instruction = newInstr<LongBinaryOp>(dst, op, left, right);
  } else if (opcode == "LongInPlaceOp") {
    expect("<");
    InPlaceOpKind op = ParseInPlaceOpName(GetNextToken());
    expect(">");
    auto left = ParseRegister();
    auto right = ParseRegister();
    instruction = newInstr<LongInPlaceOp>(dst, op, left, right);
  } else if (opcode == "IntBinaryOp") {
    expect("<");
    BinaryOpKind op = ParseBinaryOpName(GetNextToken());
    expect(">");
    auto left = ParseRegister();
    auto right = ParseRegister();
    NEW_INSTR(IntBinaryOp, dst, op, left, right);
  } else if (opcode == "Compare") {
    expect("<");
    CompareOp op = ParseCompareOpName(GetNextToken());
    expect(">");
    auto left = ParseRegister();
    auto right = ParseRegister();
    instruction = newInstr<Compare>(dst, op, left, right);
  } else if (opcode == "LongCompare") {
    expect("<");
    CompareOp op = ParseCompareOpName(GetNextToken());
    expect(">");
    auto left = ParseRegister();
    auto right = ParseRegister();
    NEW_INSTR(LongCompare, dst, op, left, right);
  } else if (opcode == "UnicodeCompare") {
    expect("<");
    CompareOp op = ParseCompareOpName(GetNextToken());
    expect(">");
    auto left = ParseRegister();
    auto right = ParseRegister();
    NEW_INSTR(UnicodeCompare, dst, op, left, right);
  } else if (opcode == "UnicodeConcat") {
    auto left = ParseRegister();
    auto right = ParseRegister();
    NEW_INSTR(UnicodeConcat, dst, left, right, FrameState{});
  } else if (opcode == "UnicodeRepeat") {
    auto left = ParseRegister();
    auto right = ParseRegister();
    NEW_INSTR(UnicodeRepeat, dst, left, right, FrameState{});
  } else if (opcode == "UnicodeSubscr") {
    auto left = ParseRegister();
    auto right = ParseRegister();
    NEW_INSTR(UnicodeSubscr, dst, left, right, FrameState{});
  } else if (opcode == "IntConvert") {
    expect("<");
    Type type = parseType(GetNextToken());
    expect(">");
    auto src = ParseRegister();
    NEW_INSTR(IntConvert, dst, src, type);
  } else if (opcode == "PrimitiveCompare") {
    expect("<");
    PrimitiveCompareOp op = ParsePrimitiveCompareOpName(GetNextToken());
    expect(">");
    auto left = ParseRegister();
    auto right = ParseRegister();
    NEW_INSTR(PrimitiveCompare, dst, op, left, right);
  } else if (opcode == "PrimitiveUnaryOp") {
    expect("<");
    PrimitiveUnaryOpKind op = ParsePrimitiveUnaryOpName(GetNextToken());
    expect(">");
    auto operand = ParseRegister();
    NEW_INSTR(PrimitiveUnaryOp, dst, op, operand);
  } else if (opcode == "PrimitiveUnbox") {
    expect("<");
    Type type = parseType(GetNextToken());
    expect(">");
    auto operand = ParseRegister();
    NEW_INSTR(PrimitiveUnbox, dst, operand, type);
  } else if (opcode == "PrimitiveBoxBool") {
    auto operand = ParseRegister();
    NEW_INSTR(PrimitiveBoxBool, dst, operand);
  } else if (opcode == "PrimitiveBox") {
    expect("<");
    Type type = parseType(GetNextToken());
    expect(">");
    auto operand = ParseRegister();
    instruction = newInstr<PrimitiveBox>(dst, operand, type);
  } else if (opcode == "InPlaceOp") {
    expect("<");
    InPlaceOpKind op = ParseInPlaceOpName(GetNextToken());
    expect(">");
    auto left = ParseRegister();
    auto right = ParseRegister();
    instruction = newInstr<InPlaceOp>(dst, op, left, right);
  } else if (opcode == "UnaryOp") {
    expect("<");
    UnaryOpKind op = ParseUnaryOpName(GetNextToken());
    expect(">");
    auto operand = ParseRegister();
    instruction = newInstr<UnaryOp>(dst, op, operand);
  } else if (opcode == "RaiseAwaitableError") {
    expect("<");
    std::string_view error = GetNextToken();
    bool is_aenter = error == "__aenter__";
    JIT_CHECK(
        is_aenter || error == "__aexit__",
        "Bad error string for RaiseAwaitableError: {}",
        error);
    expect(">");
    auto type_reg = ParseRegister();
    NEW_INSTR(RaiseAwaitableError, type_reg, is_aenter, FrameState{});
  } else if (opcode == "Return") {
    Type type = TObject;
    if (peekNextToken() == "<") {
      GetNextToken();
      type = parseType(GetNextToken());
      expect(">");
    }
    auto var = ParseRegister();
    NEW_INSTR(Return, var, type);
  } else if (opcode == "YieldValue") {
    Register* value = ParseRegister();
    instruction = newInstr<YieldValue>(dst, value);
  } else if (opcode == "InitialYield") {
    instruction = newInstr<InitialYield>(dst);
  } else if (opcode == "GetIter") {
    auto iterable = ParseRegister();
    instruction = newInstr<GetIter>(dst, iterable);
  } else if (opcode == "GetSecondOutput") {
    expect("<");
    Type ty = parseType(GetNextToken());
    expect(">");
    Register* value = ParseRegister();
    NEW_INSTR(GetSecondOutput, dst, ty, value);
  } else if (opcode == "LoadTypeAttrCacheEntryType") {
    expect("<");
    int cache_id = GetNextInteger();
    expect(">");
    NEW_INSTR(LoadTypeAttrCacheEntryType, dst, cache_id);
  } else if (opcode == "LoadTypeAttrCacheEntryValue") {
    expect("<");
    int cache_id = GetNextInteger();
    expect(">");
    NEW_INSTR(LoadTypeAttrCacheEntryValue, dst, cache_id);
  } else if (opcode == "FillTypeAttrCache") {
    expect("<");
    int cache_id = GetNextInteger();
    int name_idx = GetNextInteger();
    expect(">");
    auto receiver = ParseRegister();
    instruction =
        newInstr<FillTypeAttrCache>(dst, receiver, name_idx, cache_id);
  } else if (opcode == "LoadArrayItem") {
    auto ob_item = ParseRegister();
    auto idx = ParseRegister();
    auto array_unused = ParseRegister();
    NEW_INSTR(LoadArrayItem, dst, ob_item, idx, array_unused, 0, TObject);
  } else if (opcode == "Phi") {
    expect("<");
    PhiInfo info{dst};
    while (true) {
      info.inputs.emplace_back(PhiInput{GetNextInteger(), nullptr});
      if (peekNextToken() == ">") {
        GetNextToken();
        break;
      }
      expect(",");
    }
    for (auto& input : info.inputs) {
      input.value = ParseRegister();
    }
    phis_[bb_index].emplace_back(std::move(info));
  } else if (opcode == "Guard") {
    auto operand = ParseRegister();
    instruction = newInstr<Guard>(operand);
  } else if (opcode == "GuardType") {
    expect("<");
    Type ty = parseType(GetNextToken());
    expect(">");
    auto operand = ParseRegister();
    instruction = newInstr<GuardType>(dst, ty, operand);
  } else if (opcode == "GuardIs") {
    expect("<");
    // Since we print raw pointer values for GuardIs, we should parse values
    // as pointers as well. However, since pointers to memory aren't stable,
    // we cannot currently turn them into meaningful values, and since we can't
    // execute parsed HIR code yet, we only support Py_None as the target object
    // for now.
    expect("Py_None");
    expect(">");
    auto operand = ParseRegister();
    NEW_INSTR(GuardIs, dst, Py_None, operand);
  } else if (opcode == "IsTruthy") {
    auto src = ParseRegister();
    instruction = newInstr<IsTruthy>(dst, src);
  } else if (opcode == "UseType") {
    expect("<");
    Type ty = parseType(GetNextToken());
    expect(">");
    auto operand = ParseRegister();
    NEW_INSTR(UseType, operand, ty);
  } else if (opcode == "HintType") {
    ProfiledTypes types;
    expect("<");
    int num_args = GetNextInteger();
    expect(",");
    while (true) {
      std::vector<Type> single_profile;
      expect("<");
      while (true) {
        Type ty = parseType(GetNextToken());
        single_profile.emplace_back(ty);
        if (peekNextToken() == ">") {
          GetNextToken();
          break;
        }
        expect(",");
      }
      types.emplace_back(single_profile);
      if (peekNextToken() == ">") {
        GetNextToken();
        break;
      }
      expect(",");
    }
    std::vector<Register*> args(num_args);
    std::generate(
        args.begin(),
        args.end(),
        std::bind(std::mem_fn(&HIRParser::ParseRegister), this));
    NEW_INSTR(HintType, num_args, types, args);
  } else if (opcode == "RefineType") {
    expect("<");
    Type ty = parseType(GetNextToken());
    expect(">");
    auto operand = ParseRegister();
    NEW_INSTR(RefineType, dst, ty, operand);
  } else if (opcode == "CheckExc") {
    auto operand = ParseRegister();
    instruction = newInstr<CheckExc>(dst, operand);
  } else if (opcode == "CheckVar") {
    expect("<");
    BorrowedRef<> name = GetNextUnicode();
    expect(">");
    auto operand = ParseRegister();
    instruction = newInstr<CheckVar>(dst, operand, name);
  } else if (opcode == "CheckSequenceBounds") {
    auto sequence = ParseRegister();
    auto idx = ParseRegister();
    NEW_INSTR(CheckSequenceBounds, dst, sequence, idx);
  } else if (opcode == "Snapshot") {
    auto snapshot = Snapshot::create();
    if (peekNextToken() == "{") {
      snapshot->setFrameState(parseFrameState());
    }
    instruction = snapshot;
  } else if (opcode == "Deopt") {
    instruction = newInstr<Deopt>();
  } else if (opcode == "Unreachable") {
    instruction = Unreachable::create();
  } else if (opcode == "MakeDict") {
    expect("<");
    auto capacity = GetNextInteger();
    expect(">");
    instruction = newInstr<MakeDict>(dst, capacity);
  } else if (opcode == "InvokeStaticFunction") {
    expect("<");
    auto name = GetNextToken();
    auto mod_name =
        Ref<>::steal(PyUnicode_FromStringAndSize(name.data(), name.size()));
    JIT_CHECK(mod_name != nullptr, "failed to allocate mod name");
    auto dot = Ref<>::steal(PyUnicode_FromString("."));
    JIT_CHECK(dot != nullptr, "failed to allocate mod name");

    auto names = Ref<PyListObject>::steal(PyUnicode_Split(mod_name, dot, -1));
    JIT_CHECK(names != nullptr, "unknown func");

    auto container_descr =
        Ref<PyTupleObject>::steal(PyTuple_New(Py_SIZE(names.get()) - 1));
    JIT_CHECK(container_descr != nullptr, "failed to allocate container");
    for (Py_ssize_t i = 0; i < Py_SIZE(names.get()) - 1; i++) {
      PyObject* comp = PyList_GET_ITEM(names.get(), i);
      PyTuple_SET_ITEM(container_descr.get(), i, comp);
      Py_INCREF(comp);
    }

    auto type_descr = Ref<>::steal(PyTuple_New(2));
    JIT_CHECK(type_descr != nullptr, "failed to allocate type_descr");

    PyTuple_SET_ITEM(type_descr.get(), 0, container_descr.getObj());
    Py_INCREF(container_descr.get());
    PyObject* func_name = PyList_GET_ITEM(names, Py_SIZE(names.get()) - 1);
    PyTuple_SET_ITEM(type_descr.get(), 1, func_name);
    Py_INCREF(func_name);

    PyObject* container = nullptr;
    auto func = Ref<PyFunctionObject>::steal(
        _PyClassLoader_ResolveFunction(type_descr, &container));
    JIT_CHECK(func != nullptr, "unknown func");
    Py_XDECREF(container);

    expect(",");
    auto argcount = GetNextInteger();
    expect(",");
    Type ty = parseType(GetNextToken());
    expect(">");

    instruction = newInstr<InvokeStaticFunction>(argcount, dst, func, ty);
  } else if (opcode == "LoadCurrentFunc") {
    NEW_INSTR(LoadCurrentFunc, dst);
  } else if (opcode == "LoadEvalBreaker") {
    NEW_INSTR(LoadEvalBreaker, dst);
  } else if (opcode == "RunPeriodicTasks") {
    instruction = newInstr<RunPeriodicTasks>(dst);
  } else if (opcode == "ListAppend") {
    auto list = ParseRegister();
    auto value = ParseRegister();
    NEW_INSTR(ListAppend, dst, list, value);
  } else {
    JIT_ABORT("Unknown opcode: {}", opcode);
  }

  return instruction;
}

std::vector<Register*> HIRParser::parseRegisterVector() {
  expect("<");
  int num_items = GetNextInteger();
  expect(">");
  std::vector<Register*> registers;
  for (int i = 0; i < num_items; i++) {
    auto name = GetNextToken();
    if (name == "<null>") {
      registers.emplace_back(nullptr);
    } else {
      registers.emplace_back(allocateRegister(name));
    }
  }
  return registers;
}

std::vector<RegState> HIRParser::parseRegStates() {
  expect("<");
  int num_vals = GetNextInteger();
  expect(">");
  std::vector<RegState> reg_states;
  for (int i = 0; i < num_vals; i++) {
    auto rs = GetNextRegState();
    reg_states.emplace_back(rs);
  }
  return reg_states;
}

FrameState HIRParser::parseFrameState() {
  FrameState fs;
  expect("{");
  auto token = GetNextToken();
  while (token != "}") {
    if (token == "CurInstrOffset") {
      fs.cur_instr_offs = BCOffset{GetNextInteger()};
    } else if (token == "Locals") {
      fs.localsplus = parseRegisterVector();
      fs.nlocals = fs.localsplus.size();
    } else if (token == "Cells") {
      for (auto reg : parseRegisterVector()) {
        fs.localsplus.push_back(reg);
      }
    } else if (token == "Stack") {
      for (Register* r : parseRegisterVector()) {
        fs.stack.push(r);
      }
    } else if (token == "BlockStack") {
      expect("{");
      while (peekNextToken() != "}") {
        ExecutionBlock block;
        expect("Opcode");
        block.opcode = GetNextInteger();
        expect("HandlerOff");
        block.handler_off = BCOffset{GetNextInteger()};
        expect("StackLevel");
        block.stack_level = GetNextInteger();
        fs.block_stack.push(block);
      }
      expect("}");
    } else {
      JIT_ABORT("Unexpected token in FrameState: {}", token);
    }
    token = GetNextToken();
  }
  return fs;
}

BasicBlock* HIRParser::ParseBasicBlock(CFG& cfg) {
  if (peekNextToken() != "bb") {
    return nullptr;
  }

  expect("bb");
  int id = GetNextInteger();
  auto bb = cfg.AllocateBlock();
  bb->id = id;

  if (peekNextToken() == "(") {
    // Skip over optional "(preds 1, 2, 3)".
    while (GetNextToken() != ")") {
    }
  }
  expect("{");

  while (peekNextToken() != "}") {
    Register* dst = nullptr;
    if (peekNextToken(1) == "=") {
      dst = ParseRegister();
      expect("=");
    }
    std::string_view token = GetNextToken();
    auto* instr = parseInstr(token, dst, id);
    if (instr != nullptr) {
      bb->Append(instr);
    }
  }
  expect("}");

  index_to_bb_.emplace(id, bb);
  return bb;
}

static bool is_whitespace(char c) {
  return c == ' ' || c == '\t' || c == '\n';
}

static bool is_single_char_token(char c) {
  return c == '=' || c == '<' || c == '>' || c == ',' || c == '{' || c == '}' ||
      c == '(' || c == ')' || c == ';';
}

std::unique_ptr<Function> HIRParser::ParseHIR(const char* hir) {
  tokens_.clear();
  phis_.clear();
  branches_.clear();
  cond_branches_.clear();
  index_to_bb_.clear();

  const char* p = hir;
  while (true) {
    while (is_whitespace(*p)) {
      p++;
    }

    if (*p == '\0') {
      break;
    }

    if (*p == '"') {
      std::string token;
      for (p++; *p != '"'; p++) {
        JIT_CHECK(*p != '\0', "End of input during string literal");
        if (*p != '\\') {
          token += *p;
          continue;
        }
        p++;
        switch (*p) {
          case 'n':
            token += '\n';
            break;
          case '"':
          case '\\':
            token += *p;
            break;
          default:
            JIT_ABORT("Bad escape sequence \\{}", *p);
        }
      }
      p++;
      tokens_.emplace_back(std::move(token));
    }

    if (is_single_char_token(*p)) {
      tokens_.emplace_back(p, 1);
      p++;
      continue;
    }

    auto q = p;
    while (!is_whitespace(*q) && !is_single_char_token(*q) && *q != '\0') {
      q++;
    }

    tokens_.emplace_back(p, q - p);
    p = q;
  }

  token_iter_ = tokens_.begin();

  expect("fun");

  auto hir_func = std::make_unique<Function>();
  env_ = &hir_func->env;
  hir_func->fullname = GetNextToken();

  expect("{");

  while (auto bb = ParseBasicBlock(hir_func->cfg)) {
    if (hir_func->cfg.entry_block == nullptr) {
      hir_func->cfg.entry_block = bb;
    }
  }

  realizePhis();

  for (auto& it : branches_) {
    it.first->set_target(index_to_bb_[it.second]);
  }

  for (auto& it : cond_branches_) {
    it.first->set_true_bb(index_to_bb_[it.second.first]);
    it.first->set_false_bb(index_to_bb_[it.second.second]);
  }

  expect("}");

  hir_func->env.setNextRegisterId(max_reg_id_ + 1);
  return hir_func;
}

void HIRParser::realizePhis() {
  for (auto& pair : phis_) {
    auto block = index_to_bb_[pair.first];
    auto& front = block->front();

    for (auto& phi : pair.second) {
      std::unordered_map<BasicBlock*, Register*> inputs;
      for (auto& info : phi.inputs) {
        inputs.emplace(index_to_bb_[info.bb], info.value);
      }
      (Phi::create(phi.dst, inputs))->InsertBefore(front);
    }
  }
}

// Parse an integer, followed by an optional ; and string name (which are
// ignored).
int HIRParser::GetNextNameIdx() {
  auto idx = GetNextInteger();
  if (peekNextToken() == ";") {
    // Ignore ; and name.
    GetNextToken();
    GetNextToken();
  }
  return idx;
}

BorrowedRef<> HIRParser::GetNextUnicode() {
  std::string_view str = GetNextToken();
  auto raw_obj = PyUnicode_FromStringAndSize(str.data(), str.size());
  JIT_CHECK(raw_obj != nullptr, "Failed to create string {}", str);
  PyUnicode_InternInPlace(&raw_obj);
  auto obj = Ref<>::steal(raw_obj);
  JIT_CHECK(obj != nullptr, "Failed to intern string {}", str);
  return env_->addReference(std::move(obj));
}

RegState HIRParser::GetNextRegState() {
  auto token = GetNextToken();
  auto end = token.find(':');
  JIT_CHECK(end != std::string::npos, "Invalid reg state: {}", token);
  RegState rs;
  rs.reg = allocateRegister(token.substr(end + 1));
  switch (token[0]) {
    case 'b':
      rs.ref_kind = RefKind::kBorrowed;
      break;
    case 'o':
      rs.ref_kind = RefKind::kOwned;
      break;
    case 'u':
      rs.ref_kind = RefKind::kUncounted;
      break;
    default:
      JIT_ABORT("Unknown ref kind: {}", token[0]);
      break;
  }

  return rs;
}

} // namespace jit::hir
