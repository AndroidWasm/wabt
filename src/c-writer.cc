/*
 * Copyright 2017 WebAssembly Community Group participants
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "wabt/c-writer.h"

#include <cctype>
#include <cinttypes>
#include <limits>
#include <map>
#include <set>
#include <string_view>

#include "wabt/cast.h"
#include "wabt/common.h"
#include "wabt/ir.h"
#include "wabt/literal.h"
#include "wabt/sha256.h"
#include "wabt/stream.h"
#include "wabt/string-util.h"

#define WASM_SPECIAL_PREFIX "wasm_"
#define WASM_ALLOCATE_VARARGS "__wasm_allocate_varargs"
#define INTERNAL_MAIN_NAME "__main_argc_argv"

#define INDENT_SIZE 2

#define UNIMPLEMENTED(x) printf("unimplemented: %s\n", (x)), abort()

// code to be inserted into the generated output
extern const char* s_header_top;
extern const char* s_header_bottom;
extern const char* s_source_includes;
extern const char* s_source_declarations;

namespace wabt {

namespace {

struct Label {
  Label(LabelType label_type,
        const std::string& name,
        const TypeVector& sig,
        size_t type_stack_size,
        size_t try_catch_stack_size,
        bool used = false)
      : label_type(label_type),
        name(name),
        sig(sig),
        type_stack_size(type_stack_size),
        try_catch_stack_size(try_catch_stack_size),
        used(used) {}

  bool HasValue() const { return !sig.empty(); }

  LabelType label_type;
  const std::string& name;
  const TypeVector& sig;
  size_t type_stack_size;
  size_t try_catch_stack_size;
  bool used = false;
};

struct LocalName {
  explicit LocalName(const std::string& name) : name(name) {}
  const std::string& name;
};

struct ParamName : LocalName {
  using LocalName::LocalName;
  ParamName(const Var& var) : LocalName(var.name()) {}
};

struct LabelName : LocalName {
  using LocalName::LocalName;
};

struct GlobalName {
  GlobalName(ModuleFieldType type, const std::string& name)
      : type(type), name(name) {}
  ModuleFieldType type;
  const std::string& name;
};

struct ExternalPtr : GlobalName {
  using GlobalName::GlobalName;
};

struct ExternalRef : GlobalName {
  using GlobalName::GlobalName;
};

struct ExternalInstancePtr : GlobalName {
  using GlobalName::GlobalName;
};

struct ExternalInstanceRef : GlobalName {
  using GlobalName::GlobalName;
};

struct GotoLabel {
  explicit GotoLabel(const Var& var) : var(var) {}
  const Var& var;
};

struct LabelDecl {
  explicit LabelDecl(const std::string& name) : name(name) {}
  std::string name;
};

struct GlobalInstanceVar {
  explicit GlobalInstanceVar(const Var& var) : var(var) {}
  const Var& var;
};

struct StackVar {
  explicit StackVar(Index index, Type type = Type::Any)
      : index(index), type(type) {}
  Index index;
  Type type;
};

struct VaTempVar {
  explicit VaTempVar(Index index) : index(index) {}
  Index index;
};

struct TypeEnum {
  explicit TypeEnum(Type type) : type(type) {}
  Type type;
};

struct SignedType {
  explicit SignedType(Type type) : type(type) {}
  Type type;
};

struct ResultType {
  explicit ResultType(const TypeVector& types) : types(types) {}
  const TypeVector& types;
};

struct TryCatchLabel {
  TryCatchLabel(const std::string& name, size_t try_catch_stack_size)
      : name(name), try_catch_stack_size(try_catch_stack_size), used(false) {}
  std::string name;
  size_t try_catch_stack_size;
  bool used;
};

struct FuncTypeExpr {
  const FuncType* func_type;
  FuncTypeExpr(const FuncType* f) : func_type(f) {}
};

struct Newline {};
struct OpenBrace {};
struct CloseBrace {};

int GetShiftMask(Type type) {
  // clang-format off
  switch (type) {
    case Type::I32: return 31;
    case Type::I64: return 63;
    default: WABT_UNREACHABLE; return 0;
  }
  // clang-format on
}

class CWriter {
 public:
  CWriter(Stream* c_stream,
          Stream* h_stream,
          const char* header_name,
          const WriteCOptions& options)
      : options_(options),
        c_stream_(c_stream),
        h_stream_(h_stream),
        header_name_(header_name) {
    module_prefix_ = MangleModuleName(options_.module_name);
  }

  Result WriteModule(const Module&);

 private:
  using SymbolSet = std::set<std::string>;
  using SymbolMap = std::map<std::string, std::string>;
  using StackTypePair = std::pair<Index, Type>;
  using StackVarSymbolMap = std::map<StackTypePair, std::string>;

  void WriteCHeader();
  void WriteCSource();

  bool SkipImport(const Import* import) const;

  size_t MarkTypeStack() const;
  void ResetTypeStack(size_t mark);
  Type StackType(Index) const;
  void PushType(Type);
  void PushTypes(const TypeVector&);
  void DropTypes(size_t count);

  void PrepareVarargs(Index count);
  bool IsVarargs() const;
  Index TakeVarargs();

  void PushLabel(LabelType,
                 const std::string& name,
                 const FuncSignature&,
                 bool used = false);
  const Label* FindLabel(const Var& var, bool mark_used = true);
  bool IsTopLabelUsed() const;
  void PopLabel();

  static constexpr char MangleType(Type);
  static constexpr char MangleField(ModuleFieldType);
  static std::string MangleMultivalueTypes(const TypeVector&);
  static std::string MangleTagTypes(const TypeVector&);
  static std::string Mangle(std::string_view name, bool double_underscores);
  static std::string MangleName(std::string_view);
  static std::string MangleModuleName(std::string_view);
  std::string ExportName(std::string_view module_name,
                         std::string_view export_name);
  std::string ExportName(std::string_view export_name);
  std::string ModuleInstanceTypeName() const;
  static std::string ModuleInstanceTypeName(std::string_view module_name);
  void ClaimName(SymbolSet& set,
                 SymbolMap& map,
                 char type_suffix,
                 std::string_view wasm_name,
                 const std::string& c_name);
  std::string FindUniqueName(SymbolSet& set, std::string_view proposed_name);
  std::string ClaimUniqueName(SymbolSet& set,
                              SymbolMap& map,
                              char type_suffix,
                              std::string_view wasm_name,
                              const std::string& proposed_c_name);
  void DefineImportName(const Import* import,
                        std::string_view module_name,
                        std::string_view field_name);
  void ReserveExportNames();
  void ReserveExportName(std::string_view);
  std::string DefineImportedModuleInstanceName(std::string_view name);
  std::string DefineInstanceMemberName(ModuleFieldType, std::string_view);
  std::string DefineGlobalScopeName(ModuleFieldType, std::string_view);
  std::string DefineLocalScopeName(std::string_view name, bool is_label);
  std::string DefineParamName(std::string_view);
  std::string DefineLabelName(std::string_view);
  std::string DefineStackVarName(Index, Type, std::string_view);

  static void SerializeFuncType(const FuncType&, std::string&);

  void Indent(int size = INDENT_SIZE);
  void Dedent(int size = INDENT_SIZE);
  void WriteIndent();
  void WriteData(const char* src, size_t size);
  void Writef(const char* format, ...);

  template <typename T, typename U, typename... Args>
  void Write(T&& t, U&& u, Args&&... args) {
    Write(std::forward<T>(t));
    Write(std::forward<U>(u));
    Write(std::forward<Args>(args)...);
  }

  static const char* GetReferenceTypeName(const Type& type);
  static const char* GetReferenceNullValue(const Type& type);

  enum class CWriterPhase {
    Declarations,
    Definitions,
  };

  void Write() {}
  void Write(Newline);
  void Write(OpenBrace);
  void Write(CloseBrace);
  void Write(Index);
  void Write(std::string_view);
  void Write(const ParamName&);
  void Write(const LabelName&);
  void Write(const GlobalName&);
  void Write(const ExternalPtr&);
  void Write(const ExternalRef&);
  void Write(const ExternalInstancePtr&);
  void Write(const ExternalInstanceRef&);
  void Write(Type);
  void Write(SignedType);
  void Write(TypeEnum);
  void Write(const GotoLabel&);
  void Write(const LabelDecl&);
  void Write(const GlobalInstanceVar&);
  void Write(const StackVar&);
  void Write(const VaTempVar&);
  void Write(const ResultType&);
  void Write(const Const&);
  void WriteInitExpr(const ExprList&);
  std::string GenerateHeaderGuard() const;
  void WriteSourceTop();
  void WriteMultivalueTypes();
  void WriteTagTypes();
  void WriteFuncTypes();
  void Write(const FuncTypeExpr&);
  void WriteTags();
  void ComputeUniqueImports();
  void BeginInstance();
  void WriteUndefinedSymbolDeclarationsNoSandbox();
  void WriteImportsNoSandbox();
  void WriteImports();
  void WriteFuncDeclarations();
  void WriteFuncDeclaration(const FuncDeclaration&, const std::string&);
  void WriteImportFuncDeclaration(const FuncDeclaration&,
                                  const std::string& module_name,
                                  const std::string& name,
                                  bool is_varargs = false);
  void WriteCallIndirectFuncDeclaration(const FuncDeclaration&,
                                        const std::string&,
                                        bool is_varargs = false);
  void WriteModuleInstance();
  void WriteGlobals();
  void WriteStackPointerGlobal(const Global& global);
  void WriteGlobal(const Global&, const std::string&);
  void WriteGlobalPtr(const Global&, const std::string&);
  void WriteMemories();
  void WriteMemory(const std::string&);
  void WriteMemoryPtr(const std::string&);
  void WriteTables();
  void WriteTable(const std::string&, const wabt::Type&);
  void WriteTablePtr(const std::string&, const Table&);
  void WriteTableType(const wabt::Type&);
  void WriteBytes(const DataSegment* segment,
                  size_t start_index,
                  size_t end_index);
  void WriteDataInstances();
  void WriteElemInstances();
  void WriteGlobalInitializers();
  void WriteNoSandboxDataSegments();
  void WriteDataInitializers();
  void WriteElemInitializers();
  void WriteElemTableInit(bool, const ElemSegment*, const Table*);
  void WriteExports(CWriterPhase);
  void WriteInitDecl();
  void WriteFreeDecl();
  void WriteGetFuncTypeDecl();
  void WriteInit();
  void WriteFree();
  void WriteGetFuncType();
  void WriteInitInstanceImport();
  void WriteImportProperties(CWriterPhase);
  void WriteFuncs();
  void Write(const Func&);
  void WriteParamsAndLocals();
  void WriteParams(const std::vector<std::string>& index_to_name);
  void WriteParamSymbols(const std::vector<std::string>& index_to_name);
  void WriteParamTypes(const FuncDeclaration& decl, bool is_varargs = false);
  void WriteLocals(const std::vector<std::string>& index_to_name);
  void WriteStackVarDeclarations();
  void Write(const ExprList&);

  enum class AssignOp {
    Disallowed,
    Allowed,
  };

  void WriteFunctionAddress(Index symbol_index, bool is64bit);
  void WriteDataAddress(Index symbol_index, uint32_t addend, bool is64bit);
  bool TryWriteNoSandboxFunctionAddress(Offset instruction_offset,
                                        bool is64bit);
  bool TryWriteNoSandboxMemoryAddress(Offset instruction_offset,
                                      bool is64bit,
                                      const std::string& prefix = "");
  void WriteMemoryAddress(Index stack_index,
                          const Memory* memory,
                          size_t loc_offset,
                          Address offset);

  void WriteSimpleUnaryExpr(Opcode, const char* op);
  void WriteInfixBinaryExpr(Opcode,
                            const char* op,
                            AssignOp = AssignOp::Allowed);
  void WritePrefixBinaryExpr(Opcode, const char* op);
  void WriteSignedBinaryExpr(Opcode, const char* op);
  void Write(const BinaryExpr&);
  void Write(const CompareExpr&);
  void Write(const ConvertExpr&);
  void Write(const ConstExpr&);
  void Write(const LoadExpr&);
  void Write(const StoreExpr&);
  void Write(const UnaryExpr&);
  void Write(const TernaryExpr&);
  void Write(const SimdLaneOpExpr&);
  void Write(const SimdLoadLaneExpr&);
  void Write(const SimdStoreLaneExpr&);
  void Write(const SimdShuffleOpExpr&);
  void Write(const LoadSplatExpr&);
  void Write(const LoadZeroExpr&);
  void Write(const Block&);

  size_t BeginTry(const TryExpr& tryexpr);
  void WriteTryCatch(const TryExpr& tryexpr);
  void WriteTryDelegate(const TryExpr& tryexpr);
  void Write(const Catch& c);
  void WriteThrow();

  void PushTryCatch(const std::string& name);
  void PopTryCatch();

  void PushFuncSection(std::string_view include_condition = "");

  bool IsImport(const std::string& name) const;

  const WriteCOptions& options_;
  const Module* module_ = nullptr;
  const Func* func_ = nullptr;
  Stream* stream_ = nullptr;
  Stream* c_stream_ = nullptr;
  Stream* h_stream_ = nullptr;
  std::string header_name_;
  Result result_ = Result::Ok;
  int indent_ = 0;
  bool should_write_indent_next_ = false;
  int consecutive_newline_count_ = 0;

  SymbolMap global_sym_map_;
  SymbolMap local_sym_map_;
  SymbolMap import_module_sym_map_;
  StackVarSymbolMap stack_var_sym_map_;
  SymbolSet global_syms_;
  SymbolSet local_syms_;
  TypeVector type_stack_;
  std::vector<Label> label_stack_;
  std::vector<TryCatchLabel> try_catch_stack_;
  std::string module_prefix_;

  std::vector<const Import*> unique_imports_;
  SymbolSet import_module_set_;       // modules that are imported from
  SymbolSet import_func_module_set_;  // modules that funcs are imported from

  std::vector<std::pair<std::string, MemoryStream>> func_sections_;
  SymbolSet func_includes_;

  std::vector<std::string> unique_func_type_names_;
  const Global* stack_pointer_global_ = nullptr;

  bool varargs_prepared_ = false;
  TypeVector varargs_types_;
};

// TODO: if WABT begins supporting debug names for labels,
// will need to avoid conflict between a label named "$Bfunc" and
// the implicit func label
static constexpr char kImplicitFuncLabel[] = "$Bfunc";

// These should be greater than any ModuleFieldType (used for MangleField).
static constexpr char kParamSuffix =
    'a' + static_cast<char>(ModuleFieldType::Tag) + 1;
static constexpr char kLabelSuffix = kParamSuffix + 1;

static constexpr char kSymbolPrefix[] = "w2c_";
static constexpr char kAdminSymbolPrefix[] = "wasm2c_";

constexpr std::string_view kEnvModuleName = "env";
constexpr std::string_view kStackPointerName = "$__stack_pointer.1";
constexpr std::string_view kStackPointerNameWithoutDollar = "__stack_pointer.1";

constexpr size_t kStackSize = 64 * 1024;
constexpr size_t kStackBufferSize = 4 * 1024;
constexpr std::string_view kStackArrayVariableName = "g_w2c_stack_array";
constexpr std::string_view kStackOffsetGlobalVariableName =
    "g_w2c_stack_offset";

size_t CWriter::MarkTypeStack() const {
  return type_stack_.size();
}

void CWriter::ResetTypeStack(size_t mark) {
  assert(mark <= type_stack_.size());
  type_stack_.erase(type_stack_.begin() + mark, type_stack_.end());
}

Type CWriter::StackType(Index index) const {
  assert(index < type_stack_.size());
  return *(type_stack_.rbegin() + index);
}

void CWriter::PushType(Type type) {
  type_stack_.push_back(type);
}

void CWriter::PushTypes(const TypeVector& types) {
  type_stack_.insert(type_stack_.end(), types.begin(), types.end());
}

void CWriter::DropTypes(size_t count) {
  assert(count <= type_stack_.size());
  type_stack_.erase(type_stack_.end() - count, type_stack_.end());
}

void CWriter::PrepareVarargs(Index count) {
  assert(!varargs_prepared_);
  assert(varargs_types_.empty());
  varargs_prepared_ = true;
  for (Index i = 0; i < count; ++i) {
    varargs_types_.push_back(StackType(count - i - 1));
  }
  DropTypes(count);
}

bool CWriter::IsVarargs() const {
  return varargs_prepared_;
}

Index CWriter::TakeVarargs() {
  assert(varargs_prepared_);
  Index l = varargs_types_.size();
  varargs_prepared_ = false;
  varargs_types_.clear();
  return l;
}

static bool isVarargsAllocatorFun(const std::string& s) {
  return s.rfind(WASM_ALLOCATE_VARARGS, 1) < 2; // tolerate $ at index 0
}

void CWriter::PushLabel(LabelType label_type,
                        const std::string& name,
                        const FuncSignature& sig,
                        bool used) {
  if (label_type == LabelType::Loop)
    label_stack_.emplace_back(label_type, name, sig.param_types,
                              type_stack_.size(), try_catch_stack_.size(),
                              used);
  else
    label_stack_.emplace_back(label_type, name, sig.result_types,
                              type_stack_.size(), try_catch_stack_.size(),
                              used);
}

const Label* CWriter::FindLabel(const Var& var, bool mark_used) {
  Label* label = nullptr;

  if (var.is_index()) {
    // We've generated names for all labels, so we should only be using an
    // index when branching to the implicit function label, which can't be
    // named.
    assert(var.index() + 1 == label_stack_.size());
    label = &label_stack_[0];
  } else {
    assert(var.is_name());
    for (Index i = label_stack_.size(); i > 0; --i) {
      label = &label_stack_[i - 1];
      if (label->name == var.name())
        break;
    }
  }

  assert(label);
  if (mark_used) {
    label->used = true;
  }
  return label;
}

bool CWriter::IsTopLabelUsed() const {
  assert(!label_stack_.empty());
  return label_stack_.back().used;
}

void CWriter::PopLabel() {
  label_stack_.pop_back();
}

// static
constexpr char CWriter::MangleType(Type type) {
  // clang-format off
  switch (type) {
    case Type::I32: return 'i';
    case Type::I64: return 'j';
    case Type::F32: return 'f';
    case Type::F64: return 'd';
    case Type::V128: return 'o';
    case Type::FuncRef: return 'r';
    case Type::ExternRef: return 'e';
    default:
      WABT_UNREACHABLE;
  }
  // clang-format on
}

// static
constexpr char CWriter::MangleField(ModuleFieldType type) {
  assert(static_cast<std::underlying_type<ModuleFieldType>::type>(type) <
         std::numeric_limits<char>::max());
  return 'a' + static_cast<char>(type);
}

// remove risky characters for pasting into a C-style comment
static std::string SanitizeForComment(std::string_view str) {
  std::string result;

  for (const uint8_t ch : str) {
    // escape control chars, DEL, >7-bit chars, trigraphs, and end of comment
    if (ch < ' ' || ch > '~' || ch == '?' || ch == '/') {
      result += "\\" + StringPrintf("%02X", ch);
    } else {
      result += ch;
    }
  }

  return result;
}

// static
std::string CWriter::MangleMultivalueTypes(const TypeVector& types) {
  assert(types.size() >= 2);
  std::string result = "wasm_multi_";
  for (auto type : types) {
    result += MangleType(type);
  }
  return result;
}

// static
std::string CWriter::MangleTagTypes(const TypeVector& types) {
  assert(types.size() >= 2);
  std::string result = "wasm_tag_";
  for (auto type : types) {
    result += MangleType(type);
  }
  return result;
}

/* The C symbol for an export from this module. */
std::string CWriter::ExportName(std::string_view export_name) {
  return kSymbolPrefix + module_prefix_ + '_' + MangleName(export_name);
}

/* The C symbol for an export from an arbitrary module. */
// static
std::string CWriter::ExportName(std::string_view module_name,
                                std::string_view export_name) {
  return kSymbolPrefix + MangleModuleName(module_name) + '_' +
         MangleName(export_name);
}

/* The type name of an instance of this module. */
std::string CWriter::ModuleInstanceTypeName() const {
  return kSymbolPrefix + module_prefix_;
}

/* The type name of an instance of an arbitrary module. */
// static
std::string CWriter::ModuleInstanceTypeName(std::string_view module_name) {
  return kSymbolPrefix + MangleModuleName(module_name);
}

/*
 * Hardcoded "C"-locale versions of isalpha/isdigit/isalnum/isxdigit for use
 * in CWriter::Mangle(). We don't use the standard isalpha/isdigit/isalnum
 * because the caller might have changed the current locale.
 */
static bool internal_isalpha(uint8_t ch) {
  return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
}

static bool internal_isdigit(uint8_t ch) {
  return (ch >= '0' && ch <= '9');
}

static bool internal_isalnum(uint8_t ch) {
  return internal_isalpha(ch) || internal_isdigit(ch);
}

static bool internal_ishexdigit(uint8_t ch) {
  return internal_isdigit(ch) || (ch >= 'A' && ch <= 'F');  // capitals only
}

// static
std::string CWriter::Mangle(std::string_view name, bool double_underscores) {
  /*
   * Name mangling transforms arbitrary Wasm names into "safe" C names
   * in a deterministic way. To avoid collisions, distinct Wasm names must be
   * transformed into distinct C names.
   *
   * The rules implemented here are:
   * 1) any hex digit ('A' through 'F') that follows the sequence "0x"
   *    is escaped
   * 2) any underscore at the beginning, at the end, or following another
   *    underscore, is escaped
   * 3) if double_underscores is set, underscores are replaced with
   *    two underscores.
   * 4) otherwise, any alphanumeric character is kept as-is,
   *    and any other character is escaped
   *
   * "Escaped" means the character is represented with the sequence "0xAB",
   * where A B are hex digits ('0'-'9' or 'A'-'F') representing the character's
   * numeric value.
   *
   * Module names are mangled with double_underscores=true to prevent
   * collisions between, e.g., a module "alfa" with export
   * "bravo_charlie" vs. a module "alfa_bravo" with export "charlie".
   */

  enum State { Any, Zero, ZeroX, ZeroXHexDigit } state{Any};
  bool last_was_underscore = false;

  std::string result;
  auto append_escaped = [&](const uint8_t ch) {
    result += "0x" + StringPrintf("%02X", ch);
    last_was_underscore = false;
    state = Any;
  };

  auto append_verbatim = [&](const uint8_t ch) {
    result += ch;
    last_was_underscore = (ch == '_');
  };

  for (auto it = name.begin(); it != name.end(); ++it) {
    const uint8_t ch = *it;
    switch (state) {
      case Any:
        state = (ch == '0') ? Zero : Any;
        break;
      case Zero:
        state = (ch == 'x') ? ZeroX : Any;
        break;
      case ZeroX:
        state = internal_ishexdigit(ch) ? ZeroXHexDigit : Any;
        break;
      case ZeroXHexDigit:
        WABT_UNREACHABLE;
        break;
    }

    /* rule 1 */
    if (state == ZeroXHexDigit) {
      append_escaped(ch);
      continue;
    }

    /* rule 2 */
    if ((ch == '_') && ((it == name.begin()) || (std::next(it) == name.end()) ||
                        last_was_underscore)) {
      append_escaped(ch);
      continue;
    }

    /* rule 3 */
    if (double_underscores && ch == '_') {
      append_verbatim(ch);
      append_verbatim(ch);
      continue;
    }

    /* rule 4 */
    if (internal_isalnum(ch) || (ch == '_')) {
      append_verbatim(ch);
    } else {
      append_escaped(ch);
    }
  }

  return result;
}

// static
std::string CWriter::MangleName(std::string_view name) {
  return Mangle(name, false);
}

// static
std::string CWriter::MangleModuleName(std::string_view name) {
  return Mangle(name, true);
}

/*
 * Allocate a C symbol (must be unused) in the SymbolSet,
 * and a mapping from the Wasm name (tagged with
 * the index space of the name) to that C symbol.
 */
void CWriter::ClaimName(SymbolSet& set,
                        SymbolMap& map,
                        char type_suffix,
                        std::string_view wasm_name,
                        const std::string& c_name) {
  const std::string type_tagged_wasm_name =
      std::string(wasm_name) + type_suffix;

  [[maybe_unused]] bool success;
  success = set.insert(c_name).second;
  assert(success);

  success = map.emplace(type_tagged_wasm_name, c_name).second;
  assert(success);
}

/*
 * Make a proposed C symbol unique in a given symbol set by appending
 * an integer to the symbol if necessary.
 */
std::string CWriter::FindUniqueName(SymbolSet& set,
                                    std::string_view proposed_name) {
  std::string unique{proposed_name};
  if (set.find(unique) != set.end()) {
    std::string base = unique + "_";
    size_t count = 0;
    do {
      unique = base + std::to_string(count++);
    } while (set.find(unique) != set.end());
  }
  return unique;
}

/*
 * Find a unique C symbol in the symbol set and claim it (mapping the
 * type-tagged Wasm name to it).
 */
std::string CWriter::ClaimUniqueName(SymbolSet& set,
                                     SymbolMap& map,
                                     char type_suffix,
                                     std::string_view wasm_name,
                                     const std::string& proposed_c_name) {
  const std::string unique = FindUniqueName(set, proposed_c_name);
  ClaimName(set, map, type_suffix, wasm_name, unique);
  return unique;
}

std::string_view StripLeadingDollar(std::string_view name) {
  assert(!name.empty());
  assert(name.front() == '$');
  name.remove_prefix(1);
  return name;
}

void CWriter::DefineImportName(const Import* import,
                               std::string_view module,
                               std::string_view field_name) {
  std::string name;
  ModuleFieldType type;

  switch (import->kind()) {
    case ExternalKind::Func:
      type = ModuleFieldType::Func;
      name = cast<FuncImport>(import)->func.name;
      break;
    case ExternalKind::Tag:
      type = ModuleFieldType::Tag;
      name = cast<TagImport>(import)->tag.name;
      break;
    case ExternalKind::Global:
      type = ModuleFieldType::Global;
      name = cast<GlobalImport>(import)->global.name;
      break;
    case ExternalKind::Memory:
      type = ModuleFieldType::Memory;
      name = cast<MemoryImport>(import)->memory.name;
      break;
    case ExternalKind::Table:
      type = ModuleFieldType::Table;
      name = cast<TableImport>(import)->table.name;
      break;
  }

  import_module_sym_map_.emplace(name, import->module_name);

  std::string mangled;
  if (!options_.features.sandbox_enabled() && module == "env") {
    mangled = field_name;
  } else {
    mangled = ExportName(module, field_name);
  }

  global_syms_.erase(mangled);  // duplicate imports are allowed
  ClaimName(global_syms_, global_sym_map_, MangleField(type), name, mangled);
}

/*
 * Reserve a C symbol for the public name of a module's export.  The
 * format of these is "w2c_" + the module prefix + "_" + the mangled
 * export name. Reserving the symbol prevents internal functions and
 * other names from shadowing/overlapping the exports.
 */
void CWriter::ReserveExportName(std::string_view name) {
  ClaimName(global_syms_, global_sym_map_, MangleField(ModuleFieldType::Export),
            name, ExportName(name));
}

/*
 * Names for functions, function types, tags, and segments are globally unique
 * across modules (formatted the same as an export, as "w2c_" + module prefix +
 * "_" + the name, made unique if necessary).
 */
std::string CWriter::DefineGlobalScopeName(ModuleFieldType type,
                                           std::string_view name) {
  return ClaimUniqueName(global_syms_, global_sym_map_, MangleField(type), name,
                         ExportName(StripLeadingDollar(name)));
}

/* Names for params, locals, and stack vars are formatted as "w2c_" + name. */
std::string CWriter::DefineLocalScopeName(std::string_view name,
                                          bool is_label) {
  return ClaimUniqueName(local_syms_, local_sym_map_,
                         is_label ? kLabelSuffix : kParamSuffix, name,
                         kSymbolPrefix + MangleName(StripLeadingDollar(name)));
}

std::string CWriter::DefineParamName(std::string_view name) {
  return DefineLocalScopeName(name, false);
}

std::string CWriter::DefineLabelName(std::string_view name) {
  return DefineLocalScopeName(name, true);
}

std::string CWriter::DefineStackVarName(Index index,
                                        Type type,
                                        std::string_view name) {
  std::string unique =
      FindUniqueName(local_syms_, kSymbolPrefix + MangleName(name));
  StackTypePair stp = {index, type};
  [[maybe_unused]] bool success =
      stack_var_sym_map_.emplace(stp, unique).second;
  assert(success);
  return unique;
}

/*
 * Members of the module instance (globals, tables, and memories) are formatted
 * as "w2c_" + the mangled name of the element (made unique if necessary).
 */
std::string CWriter::DefineInstanceMemberName(ModuleFieldType type,
                                              std::string_view name) {
  return ClaimUniqueName(global_syms_, global_sym_map_, MangleField(type), name,
                         kSymbolPrefix + MangleName(StripLeadingDollar(name)));
}

/*
 * The name of a module-instance member that points to the originating
 * instance of an imported function is formatted as "w2c_" + originating
 * module prefix + "_instance".
 */
std::string CWriter::DefineImportedModuleInstanceName(std::string_view name) {
  return ClaimUniqueName(global_syms_, global_sym_map_,
                         MangleField(ModuleFieldType::Import), name,
                         ExportName(name, "instance"));
}

void CWriter::Indent(int size) {
  indent_ += size;
}

void CWriter::Dedent(int size) {
  indent_ -= size;
  assert(indent_ >= 0);
}

void CWriter::WriteIndent() {
  static char s_indent[] =
      "                                                                       "
      "                                                                       ";
  static size_t s_indent_len = sizeof(s_indent) - 1;
  size_t to_write = indent_;
  while (to_write >= s_indent_len) {
    stream_->WriteData(s_indent, s_indent_len);
    to_write -= s_indent_len;
  }
  if (to_write > 0) {
    stream_->WriteData(s_indent, to_write);
  }
}

void CWriter::WriteData(const char* src, size_t size) {
  if (should_write_indent_next_) {
    WriteIndent();
    should_write_indent_next_ = false;
  }
  if (size > 0 && src[0] != '\n') {
    consecutive_newline_count_ = 0;
  }
  stream_->WriteData(src, size);
}

void WABT_PRINTF_FORMAT(2, 3) CWriter::Writef(const char* format, ...) {
  WABT_SNPRINTF_ALLOCA(buffer, length, format);
  WriteData(buffer, length);
}

void CWriter::Write(Newline) {
  // Allow maximum one blank line between sections
  if (consecutive_newline_count_ < 2) {
    Write("\n");
    consecutive_newline_count_++;
  }
  should_write_indent_next_ = true;
}

void CWriter::Write(OpenBrace) {
  Write("{");
  Indent();
  Write(Newline());
}

void CWriter::Write(CloseBrace) {
  Dedent();
  Write("}");
}

void CWriter::Write(Index index) {
  Writef("%" PRIindex, index);
}

void CWriter::Write(std::string_view s) {
  WriteData(s.data(), s.size());
}

void CWriter::Write(const ParamName& name) {
  std::string mangled = name.name + kParamSuffix;
  assert(local_sym_map_.count(mangled) == 1);
  Write(local_sym_map_[mangled]);
}

void CWriter::Write(const LabelName& name) {
  std::string mangled = name.name + kLabelSuffix;
  assert(local_sym_map_.count(mangled) == 1);
  Write(local_sym_map_[mangled]);
}

void CWriter::Write(const GlobalName& name) {
  std::string mangled = name.name + MangleField(name.type);
  assert(global_sym_map_.count(mangled) == 1);
  Write(global_sym_map_.at(mangled));
}

void CWriter::Write(const ExternalPtr& name) {
  if (!IsImport(name.name) || !options_.features.sandbox_enabled()) {
    Write("&");
  }
  Write(GlobalName(name));
}

void CWriter::Write(const ExternalInstancePtr& name) {
  if (!IsImport(name.name)) {
    Write("&");
  }
  Write("instance->", GlobalName(name));
}

void CWriter::Write(const ExternalRef& name) {
  if (IsImport(name.name) && options_.features.sandbox_enabled()) {
    Write("(*", GlobalName(name), ")");
  } else {
    Write(GlobalName(name));
  }
}

void CWriter::Write(const ExternalInstanceRef& name) {
  if (IsImport(name.name)) {
    Write("(*instance->", GlobalName(name), ")");
  } else {
    Write("instance->", GlobalName(name));
  }
}

void CWriter::Write(const GotoLabel& goto_label) {
  const Label* label = FindLabel(goto_label.var);
  if (label->HasValue()) {
    size_t amount = label->sig.size();
    assert(type_stack_.size() >= label->type_stack_size);
    assert(type_stack_.size() >= amount);
    assert(type_stack_.size() - amount >= label->type_stack_size);
    Index offset = type_stack_.size() - label->type_stack_size - amount;
    if (offset != 0) {
      for (Index i = 0; i < amount; ++i) {
        Write(StackVar(amount - i - 1 + offset, label->sig[i]), " = ",
              StackVar(amount - i - 1), "; ");
      }
    }

    assert(try_catch_stack_.size() >= label->try_catch_stack_size);

    if (try_catch_stack_.size() != label->try_catch_stack_size) {
      const std::string& name =
          try_catch_stack_.at(label->try_catch_stack_size).name;

      Write("wasm_rt_set_unwind_target(", name, "_outer_target);", Newline());
    }
  }

  if (goto_label.var.is_name()) {
    Write("goto ", LabelName(goto_label.var.name()), ";");
  } else {
    // We've generated names for all labels, so we should only be using an
    // index when branching to the implicit function label, which can't be
    // named.
    Write("goto ", LabelName(kImplicitFuncLabel), ";");
  }
}

void CWriter::Write(const LabelDecl& label) {
  if (IsTopLabelUsed())
    Write(label.name, ":;", Newline());
}

void CWriter::Write(const GlobalInstanceVar& var) {
  assert(var.var.is_name());
  Write(ExternalInstanceRef(ModuleFieldType::Global, var.var.name()));
}

void CWriter::Write(const StackVar& sv) {
  Index index = type_stack_.size() - 1 - sv.index;
  Type type = sv.type;
  if (type == Type::Any) {
    assert(index < type_stack_.size());
    type = type_stack_[index];
  }

  StackTypePair stp = {index, type};
  auto iter = stack_var_sym_map_.find(stp);
  if (iter == stack_var_sym_map_.end()) {
    std::string name = MangleType(type) + std::to_string(index);
    Write(DefineStackVarName(index, type, name));
  } else {
    Write(iter->second);
  }
}

void CWriter::Write(const VaTempVar& v) {
  Write(kSymbolPrefix, "va_", std::to_string(v.index));
}

void CWriter::Write(Type type) {
  // clang-format off
  switch (type) {
    case Type::I32: Write("u32"); break;
    case Type::I64: Write("u64"); break;
    case Type::F32: Write("f32"); break;
    case Type::F64: Write("f64"); break;
    case Type::V128: Write("v128"); break;
    case Type::FuncRef: Write("wasm_rt_funcref_t"); break;
    case Type::ExternRef: Write("wasm_rt_externref_t"); break;
    default:
      WABT_UNREACHABLE;
  }
  // clang-format on
}

void CWriter::Write(TypeEnum type) {
  // clang-format off
  switch (type.type) {
    case Type::I32: Write("WASM_RT_I32"); break;
    case Type::I64: Write("WASM_RT_I64"); break;
    case Type::F32: Write("WASM_RT_F32"); break;
    case Type::F64: Write("WASM_RT_F64"); break;
    case Type::V128: Write("WASM_RT_V128"); break;
    case Type::FuncRef: Write("WASM_RT_FUNCREF"); break;
    case Type::ExternRef: Write("WASM_RT_EXTERNREF"); break;
    default:
      WABT_UNREACHABLE;
  }
  // clang-format on
}

void CWriter::Write(SignedType type) {
  // clang-format off
  switch (type.type) {
    case Type::I32: Write("s32"); break;
    case Type::I64: Write("s64"); break;
    default:
      WABT_UNREACHABLE;
  }
  // clang-format on
}

void CWriter::Write(const ResultType& rt) {
  if (rt.types.empty()) {
    Write("void");
  } else if (rt.types.size() == 1) {
    Write(rt.types[0]);
  } else {
    Write("struct ", MangleMultivalueTypes(rt.types));
  }
}

void CWriter::Write(const Const& const_) {
  switch (const_.type()) {
    case Type::I32:
      Writef("%uu", static_cast<int32_t>(const_.u32()));
      break;

    case Type::I64:
      Writef("%" PRIu64 "ull", static_cast<int64_t>(const_.u64()));
      break;

    case Type::F32: {
      uint32_t f32_bits = const_.f32_bits();
      // TODO(binji): Share with similar float info in interp.cc and literal.cc
      if ((f32_bits & 0x7f800000u) == 0x7f800000u) {
        const char* sign = (f32_bits & 0x80000000) ? "-" : "";
        uint32_t significand = f32_bits & 0x7fffffu;
        if (significand == 0) {
          // Infinity.
          Writef("%sINFINITY", sign);
        } else {
          // Nan.
          Writef("f32_reinterpret_i32(0x%08x) /* %snan:0x%06x */", f32_bits,
                 sign, significand);
        }
      } else if (f32_bits == 0x80000000) {
        // Negative zero. Special-cased so it isn't written as -0 below.
        Writef("-0.f");
      } else {
        Writef("%.9g", Bitcast<float>(f32_bits));
      }
      break;
    }

    case Type::F64: {
      uint64_t f64_bits = const_.f64_bits();
      // TODO(binji): Share with similar float info in interp.cc and literal.cc
      if ((f64_bits & 0x7ff0000000000000ull) == 0x7ff0000000000000ull) {
        const char* sign = (f64_bits & 0x8000000000000000ull) ? "-" : "";
        uint64_t significand = f64_bits & 0xfffffffffffffull;
        if (significand == 0) {
          // Infinity.
          Writef("%sINFINITY", sign);
        } else {
          // Nan.
          Writef("f64_reinterpret_i64(0x%016" PRIx64 ") /* %snan:0x%013" PRIx64
                 " */",
                 f64_bits, sign, significand);
        }
      } else if (f64_bits == 0x8000000000000000ull) {
        // Negative zero. Special-cased so it isn't written as -0 below.
        Writef("-0.0");
      } else {
        Writef("%.17g", Bitcast<double>(f64_bits));
      }
      break;
    }
    case Type::V128: {
      Writef("simde_wasm_i32x4_const(0x%08x, 0x%08x, 0x%08x, 0x%08x)",
             const_.vec128().u32(0), const_.vec128().u32(1),
             const_.vec128().u32(2), const_.vec128().u32(3));
      break;
    }

    default:
      WABT_UNREACHABLE;
  }
}

void CWriter::WriteInitDecl() {
  Write("void ", kAdminSymbolPrefix, module_prefix_, "_instantiate(",
        ModuleInstanceTypeName(), "*");
  for (const auto& import_module_name : import_module_set_) {
    Write(", struct ", ModuleInstanceTypeName(import_module_name), "*");
  }
  Write(");", Newline());
}

void CWriter::WriteFreeDecl() {
  Write("void ", kAdminSymbolPrefix, module_prefix_, "_free(",
        ModuleInstanceTypeName(), "*);", Newline());
}

void CWriter::WriteGetFuncTypeDecl() {
  Write("wasm_rt_func_type_t ", kAdminSymbolPrefix, module_prefix_,
        "_get_func_type(uint32_t param_count, uint32_t result_count, ...);",
        Newline());
}

void CWriter::WriteInitExpr(const ExprList& expr_list) {
  if (expr_list.empty())
    return;

  assert(expr_list.size() == 1);
  const Expr* expr = &expr_list.front();
  switch (expr_list.front().type()) {
    case ExprType::Const:
      Write(cast<ConstExpr>(expr)->const_);
      break;

    case ExprType::GlobalGet:
      Write(GlobalInstanceVar(cast<GlobalGetExpr>(expr)->var));
      break;

    case ExprType::RefFunc: {
      const Func* func = module_->GetFunc(cast<RefFuncExpr>(expr)->var);
      const FuncDeclaration& decl = func->decl;

      assert(decl.has_func_type);
      const FuncType* func_type = module_->GetFuncType(decl.type_var);

      Write("(wasm_rt_funcref_t){", FuncTypeExpr(func_type), ", ",
            "(wasm_rt_function_ptr_t)",
            ExternalPtr(ModuleFieldType::Func, func->name), ", ");

      bool is_import = IsImport(func->name);
      if (is_import) {
        Write("instance->", GlobalName(ModuleFieldType::Import,
                                       import_module_sym_map_[func->name]));
      } else {
        Write("instance");
      }

      Write("};", Newline());
    } break;

    case ExprType::RefNull:
      Write(GetReferenceNullValue(cast<RefNullExpr>(expr)->type));
      break;

    default:
      WABT_UNREACHABLE;
  }
}

std::string CWriter::GenerateHeaderGuard() const {
  std::string result = "_";
  for (char c : header_name_) {
    if (isalnum(c) || c == '_') {
      result += toupper(c);
    } else {
      result += '_';
    }
  }
  result += "_GENERATED_";
  return result;
}

void CWriter::WriteSourceTop() {
  Write(Newline(), "#include \"", header_name_, "\"", Newline(), Newline());
  // TODO: This needs to be simplified and cleaned up.  There is no reason
  // to have the source includes be separate from the source declarations.
  if (options_.features.sandbox_enabled()) {
    Write(s_source_includes, Newline());
    Write(s_source_declarations);
  } else {
    Write("#include \"wasm-rt-no-sandbox-declarations.h\"", Newline(),
          Newline());
  }
}

void CWriter::WriteMultivalueTypes() {
  for (TypeEntry* type : module_->types) {
    FuncType* func_type = cast<FuncType>(type);
    Index num_results = func_type->GetNumResults();
    if (num_results <= 1) {
      continue;
    }
    std::string name = MangleMultivalueTypes(func_type->sig.result_types);
    // these ifndefs are actually to support importing multiple modules
    // incidentally they also mean we don't have to bother with deduplication
    Write("#ifndef ", name, Newline());
    Write("#define ", name, " ", name, Newline());
    Write("struct ", name, " ", OpenBrace());
    for (Index i = 0; i < num_results; ++i) {
      Type type = func_type->GetResultType(i);
      Write(type);
      Writef(" %c%d;", MangleType(type), i);
      Write(Newline());
    }
    Write(CloseBrace(), ";", Newline(), "#endif  /* ", name, " */", Newline());
  }
}

void CWriter::WriteTagTypes() {
  for (const Tag* tag : module_->tags) {
    const FuncDeclaration& tag_type = tag->decl;
    Index num_params = tag_type.GetNumParams();
    if (num_params <= 1) {
      continue;
    }
    const std::string name = MangleTagTypes(tag_type.sig.param_types);
    // use same method as WriteMultivalueTypes
    Write("#ifndef ", name, Newline());
    Write("#define ", name, " ", name, Newline());
    Write("struct ", name, " ", OpenBrace());
    for (Index i = 0; i < num_params; ++i) {
      Type type = tag_type.GetParamType(i);
      Write(type);
      Writef(" %c%d;", MangleType(type), i);
      Write(Newline());
    }
    Write(CloseBrace(), ";", Newline(), "#endif  /* ", name, " */", Newline());
  }
}

void CWriter::WriteFuncTypes() {
  if (!options_.features.sandbox_enabled()) {
    return;
  }

  if (module_->types.empty()) {
    return;
  }

  Write(Newline());

  std::unordered_map<std::string, std::string> type_hash;

  std::string serialized_type;
  for (const TypeEntry* type : module_->types) {
    const std::string name =
        DefineGlobalScopeName(ModuleFieldType::Type, type->name);
    SerializeFuncType(*cast<FuncType>(type), serialized_type);

    auto prior_type = type_hash.find(serialized_type);
    if (prior_type != type_hash.end()) {
      /* duplicate function type */
      unique_func_type_names_.push_back(prior_type->second);
    } else {
      unique_func_type_names_.push_back(name);
      type_hash.emplace(serialized_type, name);
      Write("FUNC_TYPE_T(", name, ") = \"");
      for (uint8_t x : serialized_type) {
        Writef("\\x%02x", x);
      }
      Write("\";", Newline());
    }
  }
}

void CWriter::Write(const FuncTypeExpr& expr) {
  Index func_type_index = module_->GetFuncTypeIndex(expr.func_type->sig);
  Write(unique_func_type_names_.at(func_type_index));
}

// static
void CWriter::SerializeFuncType(const FuncType& func_type,
                                std::string& serialized_type) {
  unsigned int len = func_type.GetNumParams() + func_type.GetNumResults() + 1;

  char* const mangled_signature = static_cast<char*>(alloca(len));
  char* next_byte = mangled_signature;

  // step 1: serialize each param type
  for (Index i = 0; i < func_type.GetNumParams(); ++i) {
    *next_byte++ = MangleType(func_type.GetParamType(i));
  }

  // step 2: separate params and results with a space
  *next_byte++ = ' ';

  // step 3: serialize each result type
  for (Index i = 0; i < func_type.GetNumResults(); ++i) {
    *next_byte++ = MangleType(func_type.GetResultType(i));
  }

  assert(next_byte - mangled_signature == len);

  // step 4: SHA-256 the whole string
  sha256({mangled_signature, len}, serialized_type);
}

void CWriter::WriteTags() {
  Index tag_index = 0;
  for (const Tag* tag : module_->tags) {
    bool is_import = tag_index < module_->num_tag_imports;
    if (!is_import) {
      // Tags are identified and compared solely by their (unique) address.
      // The data stored in this variable is never read.
      if (tag_index == module_->num_tag_imports) {
        Write(Newline());
        Write("typedef char wasm_tag_placeholder_t;", Newline());
      }
      Write("static const wasm_tag_placeholder_t ",
            DefineGlobalScopeName(ModuleFieldType::Tag, tag->name), ";",
            Newline());
    }
    tag_index++;
  }
}

bool CWriter::SkipImport(const Import* import) const {
  return !options_.features.sandbox_enabled() &&
         import->module_name == kEnvModuleName &&
         import->field_name == kStackPointerNameWithoutDollar;
}

void CWriter::ComputeUniqueImports() {
  using modname_name_pair = std::pair<std::string, std::string>;
  std::map<modname_name_pair, const Import*> import_map;
  for (const Import* import : module_->imports) {
    if (SkipImport(import)) {
      continue;
    }
    // After emplacing, the returned bool says whether the insert happened;
    // i.e., was there already an import with the same modname and name?
    // If there was, make sure it was at least the same kind of import.
    const auto iterator_and_insertion_bool = import_map.emplace(
        modname_name_pair(import->module_name, import->field_name), import);
    if (!iterator_and_insertion_bool.second) {
      if (iterator_and_insertion_bool.first->second->kind() != import->kind()) {
        UNIMPLEMENTED("contradictory import declaration");
      } else {
        fprintf(stderr, "warning: duplicate import declaration \"%s\" \"%s\"\n",
                import->module_name.c_str(), import->field_name.c_str());
      }
    }
    import_module_set_.insert(import->module_name);
    if (import->kind() == ExternalKind::Func) {
      import_func_module_set_.insert(import->module_name);
    }
  }

  for (const auto& node : import_map) {
    unique_imports_.push_back(node.second);
  }
}

void CWriter::BeginInstance() {
  ComputeUniqueImports();

  if (unique_imports_.empty()) {
    Write("typedef struct ", ModuleInstanceTypeName(), " ", OpenBrace());
    return;
  }

  // define names of per-instance imports
  for (const Import* import : module_->imports) {
    if (SkipImport(import)) {
      continue;
    }
    DefineImportName(import, import->module_name, import->field_name);
  }

  // Forward declaring module instance types
  for (const auto& import_module : import_module_set_) {
    DefineImportedModuleInstanceName(import_module);
    Write("struct ", ModuleInstanceTypeName(import_module), ";", Newline());
  }

  // Forward declaring module imports
  for (const Import* import : unique_imports_) {
    if ((import->kind() == ExternalKind::Func) ||
        (import->kind() == ExternalKind::Tag)) {
      continue;
    }

    Write("extern ");
    switch (import->kind()) {
      case ExternalKind::Global: {
        const Global& global = cast<GlobalImport>(import)->global;
        Write(global.type);
        break;
      }

      case ExternalKind::Memory: {
        Write("wasm_rt_memory_t");
        break;
      }

      case ExternalKind::Table:
        WriteTableType(cast<TableImport>(import)->table.elem_type);
        break;

      default:
        WABT_UNREACHABLE;
    }
    Write("* ", ExportName(import->module_name, import->field_name), "(struct ",
          ModuleInstanceTypeName(import->module_name), "*);", Newline());
  }
  Write(Newline());

  // Add pointers to module instances that any func is imported from,
  // so that imported functions can be given their own module instances
  // when invoked
  Write("typedef struct ", ModuleInstanceTypeName(), " ", OpenBrace());
  for (const auto& import_module : import_func_module_set_) {
    Write("struct ", ModuleInstanceTypeName(import_module), "* ",
          GlobalName(ModuleFieldType::Import, import_module), ";", Newline());
  }

  for (const Import* import : unique_imports_) {
    if ((import->kind() == ExternalKind::Func) ||
        (import->kind() == ExternalKind::Tag)) {
      continue;
    }

    Write("/* import: '", SanitizeForComment(import->module_name), "' '",
          SanitizeForComment(import->field_name), "' */", Newline());

    if (!options_.features.sandbox_enabled() &&
        import->kind() == ExternalKind::Global &&
        import->field_name == kStackPointerName) {
      Write("/* ", kStackPointerName,
            " implementation is replaced with thread-local array */",
            Newline());
      continue;
    }
    switch (import->kind()) {
      case ExternalKind::Global:
        WriteGlobal(cast<GlobalImport>(import)->global,
                    std::string("*") +
                        ExportName(import->module_name, import->field_name));
        break;

      case ExternalKind::Memory:
        WriteMemory(std::string("*") +
                    ExportName(import->module_name, import->field_name));
        break;

      case ExternalKind::Table: {
        const Table& table = cast<TableImport>(import)->table;
        WriteTable(std::string("*") +
                       ExportName(import->module_name, import->field_name),
                   table.elem_type);
      } break;

      default:
        WABT_UNREACHABLE;
    }

    Write(Newline());
  }
}

void CWriter::WriteUndefinedSymbolDeclarationsNoSandbox() {
  if (module_->undefined_data_symbols_.empty()) {
    return;
  }

  Write(Newline());

  for (auto& entry : module_->undefined_data_symbols_) {
    Write("extern char ", entry.second, ";", Newline());
  }
}

// Checks for names that are already defined in the header and would therefore
// cause clashes between incompatible declarations.
// (This currently only concerns certain functions from <math.h> that are needed
// to implement floating point primitives and write which is used in wasm-rt
// implementation of printf)
static bool isExcludedNoSandboxImport(const std::string& name) {
  return name == "floor" || name == "floorf" || name == "ceil" ||
         name == "ceilf" || name == "trunc" || name == "truncf" ||
         name == "nearbyint" || name == "nearbyintf" || name == "fabs" ||
         name == "fabsf" || name == "sqrt" || name == "sqrtf" ||
         name == "write";
}

// Write module-wide imports (funcs & tags), which aren't tied to an instance.
void CWriter::WriteImportsNoSandbox() {
  if (module_->imports.empty())
    return;

  Write(Newline());

  std::unordered_set<std::string> varargs_functions;
  for (const Import* import : unique_imports_) {
    if (import->kind() == ExternalKind::Func && isVarargsAllocatorFun(import->field_name)) {
      size_t percent_pos = import->field_name.rfind('%');
      if (percent_pos != std::string::npos) {
        varargs_functions.insert(import->field_name.substr(percent_pos + 1));
      }
    }
  }

  for (const Import* import : unique_imports_) {
    if (import->kind() == ExternalKind::Func) {
      if (import->field_name.rfind(WASM_SPECIAL_PREFIX, 0) != 0 &&
          !isVarargsAllocatorFun(import->field_name)) {
        const Func& func = cast<FuncImport>(import)->func;
        Write("/* import: '", import->module_name, "' '", import->field_name,
              "' */", Newline());
        // TODO: this logic needs work - we don't expect anything but
        // kEnvModuleName in no-sandbox mode.
        if (import->module_name == kEnvModuleName) {
          if (!isExcludedNoSandboxImport(import->field_name)) {
            WriteImportFuncDeclaration(func.decl, import->module_name,
                                       import->field_name,
                                       varargs_functions.find(import->field_name) !=
                                         varargs_functions.end());
          }
        } else {
          WriteImportFuncDeclaration(
              func.decl, import->module_name,
              MangleName(import->module_name) + MangleName(import->field_name));
        }
        Write(";");
        Write(Newline());
      }
    } else if (import->kind() == ExternalKind::Tag) {
      Write("/* import: '", import->module_name, "' '", import->field_name,
            "' */", Newline());
      Write("extern const u32 *", MangleName(import->module_name),
            MangleName(import->field_name), ";", Newline());
    }
  }
}

// Write module-wide imports (funcs & tags), which aren't tied to an instance.
void CWriter::WriteImports() {
  if (unique_imports_.empty())
    return;

  Write(Newline());

  for (const Import* import : unique_imports_) {
    if (import->kind() == ExternalKind::Func) {
      Write(Newline(), "/* import: '", SanitizeForComment(import->module_name),
            "' '", SanitizeForComment(import->field_name), "' */", Newline());
      const Func& func = cast<FuncImport>(import)->func;
      WriteImportFuncDeclaration(
          func.decl, import->module_name,
          ExportName(import->module_name, import->field_name));
      Write(";");
      Write(Newline());
    } else if (import->kind() == ExternalKind::Tag) {
      Write(Newline(), "/* import: '", SanitizeForComment(import->module_name),
            "' '", SanitizeForComment(import->field_name), "' */", Newline());
      Write("extern const wasm_rt_tag_t ",
            ExportName(import->module_name, import->field_name), ";",
            Newline());
    }
  }
}

void CWriter::WriteFuncDeclarations() {
  if (module_->funcs.size() == module_->num_func_imports)
    return;

  Write(Newline());

  Index func_index = 0;
  for (const Func* func : module_->funcs) {
    bool is_import = func_index < module_->num_func_imports;
    if (!is_import) {
      Write("static ");
      WriteFuncDeclaration(
          func->decl, DefineGlobalScopeName(ModuleFieldType::Func, func->name));
      Write(";", Newline());
    }
    ++func_index;
  }
}

void CWriter::WriteFuncDeclaration(const FuncDeclaration& decl,
                                   const std::string& name) {
  Write(ResultType(decl.sig.result_types), " ", name, "(");
  if (options_.features.sandbox_enabled()) {
    Write(ModuleInstanceTypeName(), "*");
  }
  WriteParamTypes(decl);
  Write(")");
}

void CWriter::WriteImportFuncDeclaration(const FuncDeclaration& decl,
                                         const std::string& module_name,
                                         const std::string& name,
                                         bool is_varargs) {
  Write(ResultType(decl.sig.result_types), " ", name, "(");
  if (options_.features.sandbox_enabled()) {
    Write("struct ", ModuleInstanceTypeName(module_name), "*");
  }
  WriteParamTypes(decl, is_varargs);
  Write(")");
}

void CWriter::WriteCallIndirectFuncDeclaration(const FuncDeclaration& decl,
                                               const std::string& name,
                                               bool is_varargs) {
  Write(ResultType(decl.sig.result_types), " ", name, "(");
  if (options_.features.sandbox_enabled()) {
    Write("void*");
  }
  WriteParamTypes(decl, is_varargs);
  Write(")");
}

void CWriter::WriteModuleInstance() {
  BeginInstance();
  WriteGlobals();
  WriteMemories();
  WriteTables();
  WriteDataInstances();
  WriteElemInstances();

  // C forbids an empty struct
  if (module_->globals.empty() && module_->memories.empty() &&
      module_->tables.empty() && import_func_module_set_.empty()) {
    Write("char dummy_member;", Newline());
  }

  Write(CloseBrace(), " ", ModuleInstanceTypeName(), ";", Newline());
  Write(Newline());
}

void CWriter::WriteGlobals() {
  Index global_index = 0;
  for (const Global* global : module_->globals) {
    if (!options_.features.sandbox_enabled() &&
        global->name == kStackPointerName) {
      // Remember the global to write it later (it needs to be part of .c file)
      stack_pointer_global_ = global;
    } else {
      bool is_import = global_index < module_->num_global_imports;
      if (!is_import) {
        WriteGlobal(*global, DefineInstanceMemberName(ModuleFieldType::Global,
                                                      global->name));
        Write(Newline());
      }
    }
    ++global_index;
  }
}

void CWriter::WriteStackPointerGlobal(const Global& global) {
  assert(!options_.features.sandbox_enabled());
  // Ideally we would want to have a guard page here in order to
  // catch stack-overflows. For now just add 4k as a buffer page.
  // We also add a range check against kStackSize on GlobalSet,
  // note that since leaf functions do not set the __stack_pointer
  // they won't trigger the range check in global.set
  Write("WASM_RT_THREAD_LOCAL uint8_t __attribute__((aligned(16))) ",
        kStackArrayVariableName, "[", kStackSize, " + ", kStackBufferSize, "];",
        Newline());
  Write("WASM_RT_THREAD_LOCAL uintptr_t ", kStackOffsetGlobalVariableName,
        " = sizeof(", kStackArrayVariableName, ");", Newline());
}

void CWriter::WriteGlobal(const Global& global, const std::string& name) {
  Write(global.type, " ", name, ";");
}

void CWriter::WriteGlobalPtr(const Global& global, const std::string& name) {
  Write(global.type, "* ", name, "(", ModuleInstanceTypeName(), "* instance)");
}

void CWriter::WriteMemories() {
  if (module_->memories.size() == module_->num_memory_imports)
    return;

  Index memory_index = 0;
  for (const Memory* memory : module_->memories) {
    bool is_import = memory_index < module_->num_memory_imports;
    if (!is_import) {
      WriteMemory(
          DefineInstanceMemberName(ModuleFieldType::Memory, memory->name));
      Write(Newline());
    }
    ++memory_index;
  }
}

void CWriter::WriteMemory(const std::string& name) {
  Write("wasm_rt_memory_t ", name, ";");
}

void CWriter::WriteMemoryPtr(const std::string& name) {
  Write("wasm_rt_memory_t* ", name, "(", ModuleInstanceTypeName(),
        "* instance)");
}

void CWriter::WriteTables() {
  if (module_->tables.size() == module_->num_table_imports) {
    return;
  }

  Index table_index = 0;
  for (const Table* table : module_->tables) {
    bool is_import = table_index < module_->num_table_imports;
    if (!is_import) {
      WriteTable(DefineInstanceMemberName(ModuleFieldType::Table, table->name),
                 table->elem_type);
      Write(Newline());
    }
    ++table_index;
  }
}

void CWriter::WriteTable(const std::string& name, const wabt::Type& type) {
  WriteTableType(type);
  Write(" ", name, ";");
}

void CWriter::WriteTablePtr(const std::string& name, const Table& table) {
  WriteTableType(table.elem_type);
  Write("* ", name, "(", ModuleInstanceTypeName(), "* instance)");
}

void CWriter::WriteTableType(const wabt::Type& type) {
  Write("wasm_rt_", GetReferenceTypeName(type), "_table_t");
}

void CWriter::WriteGlobalInitializers() {
  if (!options_.features.sandbox_enabled()) {
    if (stack_pointer_global_ != nullptr) {
      WriteStackPointerGlobal(*stack_pointer_global_);
    }
    return;
  }

  if (module_->globals.empty())
    return;

  Write(Newline(), "static void init_globals(", ModuleInstanceTypeName(),
        "* instance) ", OpenBrace());
  Index global_index = 0;
  for (const Global* global : module_->globals) {
    // Ignore __stack_pointer in no-sandbox mode
    if (!options_.features.sandbox_enabled() &&
        global->name == kStackPointerName)
      continue;

    bool is_import = global_index < module_->num_global_imports;
    if (!is_import) {
      assert(!global->init_expr.empty());
      Write(ExternalInstanceRef(ModuleFieldType::Global, global->name), " = ");
      WriteInitExpr(global->init_expr);
      Write(";", Newline());
    }
    ++global_index;
  }
  Write(CloseBrace(), Newline(), Newline());
}

static inline bool is_droppable(const DataSegment* data_segment) {
  return (data_segment->kind == SegmentKind::Passive) &&
         (!data_segment->data.empty());
}

static inline bool is_droppable(const ElemSegment* elem_segment) {
  return (elem_segment->kind == SegmentKind::Passive) &&
         (!elem_segment->elem_exprs.empty());
}

void CWriter::WriteDataInstances() {
  for (const DataSegment* data_segment : module_->data_segments) {
    std::string name =
        DefineGlobalScopeName(ModuleFieldType::DataSegment, data_segment->name);
    if (is_droppable(data_segment)) {
      Write("bool ", "data_segment_dropped_", name, " : 1;", Newline());
    }
  }
}

// Determines whether or not the selected data range contains only zero bytes.
static bool is_all_zero(const DataSegment* segment,
                        size_t start_index,
                        size_t end_index) {
  while (start_index < end_index) {
    if (segment->data[start_index] != 0) {
      return false;
    }
    ++start_index;
  }
  return true;
}

void CWriter::WriteBytes(const DataSegment* segment,
                         size_t start_index,
                         size_t end_index) {
  Write(OpenBrace());
  if (!is_all_zero(segment, start_index, end_index)) {
    for (size_t i = start_index; i < end_index; ++i) {
      Writef("0x%02x,", segment->data.at(i));
      if ((i - start_index + 1) % 12 == 0) {
        Write(Newline());
      } else if (i < end_index - 1) {
        Write(" ");
      }
    }
    if ((end_index - start_index) % 12 != 0) {
      Write(Newline());
    }
  } else {
    Write("/* all zero */", Newline());
  }
  Write(CloseBrace());
}

void CWriter::WriteNoSandboxDataSegments() {
  // Forward-declare all struct names and data segments.
  Write(Newline());
  for (const DataSegment* data_segment : module_->data_segments) {
    Write("extern struct ", "data_struct_",
          GlobalName(ModuleFieldType::DataSegment, data_segment->name), " ",
          "data_segment_data_",
          GlobalName(ModuleFieldType::DataSegment, data_segment->name), ";",
          Newline());
  }

  // Define data segments with initializers.
  for (const DataSegment* data_segment : module_->data_segments) {
    if (data_segment->data.empty()) {
      continue;
    }
    if (is_droppable(data_segment)) {
      UNIMPLEMENTED("droppable data segment in no-sandbox mode");
    }

    Offset base = data_segment->data_offset;
    Offset ceiling = base + data_segment->data.size();

    // Survey the data initialiser relocation maps.
    bool is64bit = false;  // found at least one 64-bit pointer initializer
    std::map<Offset, bool>
        reloc_map;  // true for function pointers, false for data pointers

    const auto& f64map = module_->function_symbol_by_fptr64_init_offset_;
    const auto& d64map = module_->data_symbol_and_addend_by_mptr64_init_offset_;
    const auto& f32map = module_->function_symbol_by_fptr32_init_offset_;
    const auto& d32map = module_->data_symbol_and_addend_by_mptr32_init_offset_;

    for (auto f64 = f64map.lower_bound(base);
         f64 != f64map.end() && f64->first < ceiling; ++f64) {
      is64bit = true;
      reloc_map[f64->first] = true;
    }

    for (auto d64 = d64map.lower_bound(base);
         d64 != d64map.end() && d64->first < ceiling; ++d64) {
      is64bit = true;
      reloc_map[d64->first] = false;
    }

    for (auto f32 = f32map.lower_bound(base);
         f32 != f32map.end() && f32->first < ceiling; ++f32) {
      if (is64bit) {
        UNIMPLEMENTED(
            "32-bit function pointer initializer in 64-bit no-sandbox mode");
        reloc_map[f32->first] = true;
      }
    }

    for (auto d32 = d32map.lower_bound(base);
         d32 != d32map.end() && d32->first < ceiling; ++d32) {
      if (is64bit) {
        UNIMPLEMENTED(
            "32-bit data pointer initializer in 64-bit no-sandbox mode");
      }
      reloc_map[d32->first] = false;
    }

    const auto& fmap = is64bit ? f64map : f32map;
    const auto& dmap = is64bit ? d64map : d32map;

    Write(Newline(), "struct __attribute__((__packed__)) data_struct_",
          GlobalName(ModuleFieldType::DataSegment, data_segment->name), " ",
          OpenBrace());
    {
      Offset cur = base;
      while (cur < ceiling) {
        auto next_reloc = reloc_map.lower_bound(cur);
        Offset next =
            next_reloc == reloc_map.end() || next_reloc->first >= ceiling
                ? ceiling
                : next_reloc->first;
        if (next != cur) {
          Write("u8 plain_data_at_", cur - base, "[", next - cur, "];",
                Newline());
        }
        if (next < ceiling) {
          bool is_function = next_reloc->second;
          Write(is64bit ? "u64" : "u32", " ", is_function ? "function" : "data",
                "_pointer_at_", next - base, ";", Newline());
          next += is64bit ? 8 : 4;
        }
        cur = next;
      }
    }

    Write(CloseBrace(), " data_segment_data_",
          GlobalName(ModuleFieldType::DataSegment, data_segment->name), " = ",
          OpenBrace());
    {
      Offset cur = base;
      while (cur < ceiling) {
        auto next_reloc = reloc_map.lower_bound(cur);
        Offset next =
            next_reloc == reloc_map.end() || next_reloc->first >= ceiling
                ? ceiling
                : next_reloc->first;
        if (next != cur) {
          WriteBytes(data_segment, cur - base, next - base);
          Write(",", Newline());
        }
        if (next < ceiling) {
          bool is_function = next_reloc->second;
          if (is_function) {
            WriteFunctionAddress(fmap.at(next), is64bit);
          } else {
            auto [data_symbol_index, addend] = dmap.at(next);
            WriteDataAddress(data_symbol_index, addend, is64bit);
          }
          Write(",", Newline());
          next += is64bit ? 8 : 4;
        }
        cur = next;
      }
    }

    Write(CloseBrace(), ";", Newline());
  }
}

void CWriter::WriteDataInitializers() {
  if (module_->memories.empty()) {
    return;
  }

  if (!options_.features.sandbox_enabled()) {
    WriteNoSandboxDataSegments();
    return;
  }

  for (const DataSegment* data_segment : module_->data_segments) {
    if (data_segment->data.empty()) {
      continue;
    }
    Write(Newline(), "static const u8 data_segment_data_",
          GlobalName(ModuleFieldType::DataSegment, data_segment->name),
          "[] = ", OpenBrace());
    size_t i = 0;
    for (uint8_t x : data_segment->data) {
      Writef("0x%02x, ", x);
      if ((++i % 12) == 0)
        Write(Newline());
    }
    if (i > 0)
      Write(Newline());
    Write(CloseBrace(), ";", Newline());
  }

  Write(Newline(), "static void init_memories(", ModuleInstanceTypeName(),
        "* instance) ", OpenBrace());
  if (module_->memories.size() > module_->num_memory_imports) {
    Index memory_idx = module_->num_memory_imports;
    for (Index i = memory_idx; i < module_->memories.size(); i++) {
      const Memory* memory = module_->memories[i];
      uint32_t max =
          memory->page_limits.has_max ? memory->page_limits.max : 65536;
      Write("wasm_rt_allocate_memory(",
            ExternalInstancePtr(ModuleFieldType::Memory, memory->name), ", ",
            memory->page_limits.initial, ", ", max, ", ",
            memory->page_limits.is_64, ");", Newline());
    }
  }

  for (const DataSegment* data_segment : module_->data_segments) {
    if (data_segment->kind != SegmentKind::Active) {
      continue;
    }
    const Memory* memory =
        module_->memories[module_->GetMemoryIndex(data_segment->memory_var)];
    Write("LOAD_DATA(",
          ExternalInstanceRef(ModuleFieldType::Memory, memory->name), ", ");
    WriteInitExpr(data_segment->offset);
    if (data_segment->data.empty()) {
      Write(", NULL, 0");
    } else {
      Write(", data_segment_data_",
            GlobalName(ModuleFieldType::DataSegment, data_segment->name), ", ",
            data_segment->data.size());
    }
    Write(");", Newline());
  }

  Write(CloseBrace(), Newline());

  if (!module_->data_segments.empty()) {
    Write(Newline(), "static void init_data_instances(",
          ModuleInstanceTypeName(), " *instance) ", OpenBrace());

    for (const DataSegment* data_segment : module_->data_segments) {
      if (is_droppable(data_segment)) {
        Write("instance->data_segment_dropped_",
              GlobalName(ModuleFieldType::DataSegment, data_segment->name),
              " = false;", Newline());
      }
    }

    Write(CloseBrace(), Newline());
  }
}

void CWriter::WriteElemInstances() {
  for (const ElemSegment* elem_segment : module_->elem_segments) {
    std::string name =
        DefineGlobalScopeName(ModuleFieldType::ElemSegment, elem_segment->name);
    if (is_droppable(elem_segment)) {
      Write("bool ", "elem_segment_dropped_", name, " : 1;", Newline());
    }
  }
}

void CWriter::WriteElemInitializers() {
  if (!options_.features.sandbox_enabled()) {
    return;
  }

  if (module_->tables.empty()) {
    return;
  }

  for (const ElemSegment* elem_segment : module_->elem_segments) {
    if (elem_segment->elem_exprs.empty()) {
      continue;
    }

    if (elem_segment->elem_type == Type::ExternRef) {
      // no need to store externref elem initializers because only
      // ref.null is possible
      continue;
    }

    Write(Newline(),
          "static const wasm_elem_segment_expr_t elem_segment_exprs_",
          GlobalName(ModuleFieldType::ElemSegment, elem_segment->name),
          "[] = ", OpenBrace());

    for (const ExprList& elem_expr : elem_segment->elem_exprs) {
      assert(elem_expr.size() == 1);
      const Expr& expr = elem_expr.front();
      switch (expr.type()) {
        case ExprType::RefFunc: {
          const Func* func = module_->GetFunc(cast<RefFuncExpr>(&expr)->var);
          const FuncType* func_type = module_->GetFuncType(func->decl.type_var);
          Write("{", FuncTypeExpr(func_type), ", (wasm_rt_function_ptr_t)",
                ExternalPtr(ModuleFieldType::Func, func->name), ", ");
          const bool is_import = IsImport(func->name);
          if (is_import) {
            Write("offsetof(", ModuleInstanceTypeName(), ", ",
                  GlobalName(ModuleFieldType::Import,
                             import_module_sym_map_[func->name]),
                  ")");
          } else {
            Write("0");
          }
          Write("},", Newline());
        } break;
        case ExprType::RefNull:
          Write("{NULL, NULL, 0},", Newline());
          break;
        default:
          WABT_UNREACHABLE;
      }
    }
    Write(CloseBrace(), ";", Newline());
  }

  Write(Newline(), "static void init_tables(", ModuleInstanceTypeName(),
        "* instance) ", OpenBrace());

  if (module_->tables.size() > module_->num_table_imports) {
    Index table_idx = module_->num_table_imports;
    for (Index i = table_idx; i < module_->tables.size(); i++) {
      const Table* table = module_->tables[i];
      uint32_t max =
          table->elem_limits.has_max ? table->elem_limits.max : UINT32_MAX;
      Write("wasm_rt_allocate_", GetReferenceTypeName(table->elem_type),
            "_table(", ExternalInstancePtr(ModuleFieldType::Table, table->name),
            ", ", table->elem_limits.initial, ", ", max, ");", Newline());
    }
  }

  for (const ElemSegment* elem_segment : module_->elem_segments) {
    if (elem_segment->kind != SegmentKind::Active) {
      continue;
    }

    const Table* table = module_->GetTable(elem_segment->table_var);

    WriteElemTableInit(true, elem_segment, table);
  }

  Write(CloseBrace(), Newline());

  if (!module_->elem_segments.empty()) {
    Write(Newline(), "static void init_elem_instances(",
          ModuleInstanceTypeName(), " *instance) ", OpenBrace());

    for (const ElemSegment* elem_segment : module_->elem_segments) {
      if (is_droppable(elem_segment)) {
        Write("instance->elem_segment_dropped_",
              GlobalName(ModuleFieldType::ElemSegment, elem_segment->name),
              " = false;", Newline());
      }
    }

    Write(CloseBrace(), Newline());
  }
}

void CWriter::WriteElemTableInit(bool active_initialization,
                                 const ElemSegment* src_segment,
                                 const Table* dst_table) {
  assert(dst_table->elem_type == Type::FuncRef ||
         dst_table->elem_type == Type::ExternRef);
  assert(dst_table->elem_type == src_segment->elem_type);

  Write(GetReferenceTypeName(dst_table->elem_type), "_table_init(",
        ExternalInstancePtr(ModuleFieldType::Table, dst_table->name), ", ");

  // elem segment exprs needed only for funcref tables
  // because externref tables can only be initialized with ref.null
  if (dst_table->elem_type == Type::FuncRef) {
    if (src_segment->elem_exprs.empty()) {
      Write("NULL, ");
    } else {
      Write("elem_segment_exprs_",
            GlobalName(ModuleFieldType::ElemSegment, src_segment->name), ", ");
    }
  }

  // src_size, dest_addr, src_addr, N
  if (active_initialization) {
    Write(src_segment->elem_exprs.size(), ", ");
    WriteInitExpr(src_segment->offset);
    Write(", 0, ", src_segment->elem_exprs.size());
  } else {
    if (is_droppable(src_segment)) {
      Write("(instance->elem_segment_dropped_",
            GlobalName(ModuleFieldType::ElemSegment, src_segment->name),
            " ? 0 : ", src_segment->elem_exprs.size(), "), ");
    } else {
      Write("0, ");
    }
    Write(StackVar(2), ", ", StackVar(1), ", ", StackVar(0));
  }

  if (dst_table->elem_type == Type::FuncRef) {
    Write(", instance");
  }

  Write(");", Newline());
}

void CWriter::WriteExports(CWriterPhase kind) {
  if (module_->exports.empty())
    return;

  for (const Export* export_ : module_->exports) {
    if (!options_.features.sandbox_enabled() &&
        export_->kind == ExternalKind::Memory) {
      continue;
    }

    Write(Newline(), "/* export: '", SanitizeForComment(export_->name), "' */",
          Newline());

    const std::string mangled_name = ExportName(export_->name);
    if (!options_.features.sandbox_enabled() &&
        kind == CWriterPhase::Declarations) {
      // In no-sandbox mode the mangled name is consistently replaced with the
      // "real" export name, i.e., the name that linked code expects.  Since
      // some usages (especially in tests) still refer to the mangled name, we
      // implement this replacement via #define, making the mangled name
      // internally an alias of the real name.
      Write("#define ", mangled_name, " ", export_->name, Newline());
    }
    std::string internal_name;
    std::vector<std::string> index_to_name;

    switch (export_->kind) {
      case ExternalKind::Func: {
        const Func* func = module_->GetFunc(export_->var);
        internal_name = func->name;
        if (kind == CWriterPhase::Declarations) {
          WriteFuncDeclaration(func->decl, mangled_name);
        } else {
          func_ = func;
          local_syms_ = global_syms_;
          local_sym_map_.clear();
          stack_var_sym_map_.clear();
          Write(ResultType(func_->decl.sig.result_types), " ", mangled_name,
                "(");
          MakeTypeBindingReverseMapping(func_->GetNumParamsAndLocals(),
                                        func_->bindings, &index_to_name);
          WriteParams(index_to_name);
        }
        break;
      }

      case ExternalKind::Global: {
        const Global* global = module_->GetGlobal(export_->var);
        internal_name = global->name;
        WriteGlobalPtr(*global, mangled_name);
        break;
      }

      case ExternalKind::Memory: {
        const Memory* memory = module_->GetMemory(export_->var);
        internal_name = memory->name;
        WriteMemoryPtr(mangled_name);
        break;
      }

      case ExternalKind::Table: {
        const Table* table = module_->GetTable(export_->var);
        internal_name = table->name;
        WriteTablePtr(mangled_name, *table);
        break;
      }

      case ExternalKind::Tag: {
        const Tag* tag = module_->GetTag(export_->var);
        internal_name = tag->name;
        if (kind == CWriterPhase::Declarations) {
          Write("extern ");
        }
        Write("const wasm_rt_tag_t ", mangled_name);
        break;
      }

      default:
        WABT_UNREACHABLE;
    }

    if (kind == CWriterPhase::Declarations) {
      Write(";", Newline());
      continue;
    }

    Write(" ");
    switch (export_->kind) {
      case ExternalKind::Func: {
        Write(OpenBrace());
        Write("return ", ExternalRef(ModuleFieldType::Func, internal_name),
              "(");
        if (options_.features.sandbox_enabled()) {
          bool is_import = IsImport(internal_name);;
          if (is_import) {
            Write("instance->",
                  GlobalName(ModuleFieldType::Import,
                             import_module_sym_map_[internal_name]));
          } else {
            Write("instance");
          }
        }
        WriteParamSymbols(index_to_name);
        Write(CloseBrace(), Newline());

        local_sym_map_.clear();
        stack_var_sym_map_.clear();
        func_ = nullptr;

        if (!options_.features.sandbox_enabled() &&
            export_->name == INTERNAL_MAIN_NAME) {
          Write(Newline(), "int main(int argc, char **argv) ", OpenBrace());
          Write("return (int)", INTERNAL_MAIN_NAME, "((u32)argc, (u64)argv);",
                Newline());
          Write(CloseBrace());
        }
        break;
      }

      case ExternalKind::Global:
        Write(OpenBrace());
        Write("return ",
              ExternalInstancePtr(ModuleFieldType::Global, internal_name), ";",
              Newline());
        Write(CloseBrace(), Newline());
        break;

      case ExternalKind::Memory:
        Write(OpenBrace());
        Write("return ",
              ExternalInstancePtr(ModuleFieldType::Memory, internal_name), ";",
              Newline());
        Write(CloseBrace(), Newline());
        break;

      case ExternalKind::Table:
        Write(OpenBrace());
        Write("return ",
              ExternalInstancePtr(ModuleFieldType::Table, internal_name), ";",
              Newline());
        Write(CloseBrace(), Newline());
        break;

      case ExternalKind::Tag:
        Write("= ", ExternalPtr(ModuleFieldType::Tag, internal_name), ";",
              Newline());
        break;

      default:
        WABT_UNREACHABLE;
    }
  }
}

void CWriter::WriteInit() {
  if (!options_.features.sandbox_enabled()) {
    return;
  }
  Write(Newline(), "void ", kAdminSymbolPrefix, module_prefix_, "_instantiate(",
        ModuleInstanceTypeName(), "* instance");
  for (const auto& import_module_name : import_module_set_) {
    Write(", struct ", ModuleInstanceTypeName(import_module_name), "* ",
          GlobalName(ModuleFieldType::Import, import_module_name));
  }
  Write(") ", OpenBrace());

  Write("assert(wasm_rt_is_initialized());", Newline());

  if (!import_module_set_.empty()) {
    Write("init_instance_import(instance");
    for (const auto& import_module_name : import_module_set_) {
      Write(", ", GlobalName(ModuleFieldType::Import, import_module_name));
    }
    Write(");", Newline());
  }

  if (!module_->globals.empty()) {
    Write("init_globals(instance);", Newline());
  }
  if (!module_->memories.empty()) {
    Write("init_memories(instance);", Newline());
  }
  if (!module_->tables.empty()) {
    Write("init_tables(instance);", Newline());
  }
  if (!module_->memories.empty() && !module_->data_segments.empty()) {
    Write("init_data_instances(instance);", Newline());
  }
  if (!module_->tables.empty() && !module_->elem_segments.empty()) {
    Write("init_elem_instances(instance);", Newline());
  }

  for (Var* var : module_->starts) {
    Write(ExternalRef(ModuleFieldType::Func, module_->GetFunc(*var)->name));
    bool is_import = IsImport(module_->GetFunc(*var)->name);
    if (is_import) {
      Write("(instance->",
            GlobalName(ModuleFieldType::Import,
                       import_module_sym_map_[module_->GetFunc(*var)->name]),
            ");");
    } else {
      Write("(instance);");
    }
    Write(Newline());
  }
  Write(CloseBrace(), Newline());
}

void CWriter::WriteGetFuncType() {
  if (!options_.features.sandbox_enabled()) {
    return;
  }
  Write(Newline(), "wasm_rt_func_type_t ", kAdminSymbolPrefix, module_prefix_,
        "_get_func_type(uint32_t param_count, uint32_t result_count, "
        "...) ",
        OpenBrace());

  Write("va_list args;", Newline());

  for (const TypeEntry* type : module_->types) {
    const FuncType* func_type = cast<FuncType>(type);
    const FuncSignature& signature = func_type->sig;

    Write(Newline(), "if (param_count == ", signature.GetNumParams(),
          " && result_count == ", signature.GetNumResults(), ") ", OpenBrace());
    Write("va_start(args, result_count);", Newline());
    Write("if (true");
    for (const auto& t : signature.param_types) {
      Write(" && va_arg(args, wasm_rt_type_t) == ", TypeEnum(t));
    }
    for (const auto& t : signature.result_types) {
      Write(" && va_arg(args, wasm_rt_type_t) == ", TypeEnum(t));
    }
    Write(") ", OpenBrace());
    Write("va_end(args);", Newline());
    Write("return ", FuncTypeExpr(func_type), ";", Newline());
    Write(CloseBrace(), Newline());
    Write("va_end(args);", Newline());
    Write(CloseBrace(), Newline());
  }

  Write(Newline(), "return NULL;", Newline());
  Write(CloseBrace(), Newline());
}

void CWriter::WriteInitInstanceImport() {
  if (!options_.features.sandbox_enabled()) {
    return;
  }

  if (import_module_set_.empty())
    return;

  Write(Newline(), "static void init_instance_import(",
        ModuleInstanceTypeName(), "* instance");
  for (const auto& import_module_name : import_module_set_) {
    Write(", struct ", ModuleInstanceTypeName(import_module_name), "* ",
          GlobalName(ModuleFieldType::Import, import_module_name));
  }
  Write(")", OpenBrace());

  for (const auto& import_module : import_func_module_set_) {
    Write("instance->", GlobalName(ModuleFieldType::Import, import_module),
          " = ", GlobalName(ModuleFieldType::Import, import_module), ";",
          Newline());
  }

  for (const Import* import : unique_imports_) {
    switch (import->kind()) {
      case ExternalKind::Func:
      case ExternalKind::Tag:
        break;

      case ExternalKind::Global:
      case ExternalKind::Memory:
      case ExternalKind::Table: {
        Write("instance->", ExportName(import->module_name, import->field_name),
              " = ", ExportName(import->module_name, import->field_name), "(",
              GlobalName(ModuleFieldType::Import, import->module_name), ");",
              Newline());
        break;
      }

      default:
        WABT_UNREACHABLE;
    }
  }
  Write(CloseBrace(), Newline());
}

void CWriter::WriteImportProperties(CWriterPhase kind) {
  if (import_module_set_.empty())
    return;

  Write(Newline());

  auto write_import_prop = [&](const Import* import, std::string prop,
                               uint32_t value) {
    if (kind == CWriterPhase::Declarations) {
      Write("extern ");
    }
    Write("const u32 ", kAdminSymbolPrefix, module_prefix_, "_", prop, "_",
          MangleModuleName(import->module_name), "_",
          MangleName(import->field_name));
    if (kind == CWriterPhase::Definitions) {
      Write(" = ", value);
    }

    Write(";", Newline());
  };

  for (const Import* import : unique_imports_) {
    if (import->kind() == ExternalKind::Memory) {
      const Limits* memory_limits =
          &(cast<MemoryImport>(import)->memory.page_limits);
      write_import_prop(import, "min", memory_limits->initial);
      write_import_prop(import, "max",
                        memory_limits->has_max
                            ? memory_limits->max
                            : std::numeric_limits<uint32_t>::max());
    }
  }
}

void CWriter::WriteFree() {
  if (!options_.features.sandbox_enabled()) {
    return;
  }
  Write(Newline(), "void ", kAdminSymbolPrefix, module_prefix_, "_free(",
        ModuleInstanceTypeName(), "* instance) ", OpenBrace());

  {
    Index table_index = 0;
    for (const Table* table : module_->tables) {
      bool is_import = table_index < module_->num_table_imports;
      if (!is_import) {
        Write("wasm_rt_free_", GetReferenceTypeName(table->elem_type),
              "_table(",
              ExternalInstancePtr(ModuleFieldType::Table, table->name), ");",
              Newline());
      }
      ++table_index;
    }
  }

  {
    Index memory_index = 0;
    for (const Memory* memory : module_->memories) {
      bool is_import = memory_index < module_->num_memory_imports;
      if (!is_import) {
        Write("wasm_rt_free_memory(",
              ExternalInstancePtr(ModuleFieldType::Memory, memory->name), ");",
              Newline());
      }
      ++memory_index;
    }
  }

  Write(CloseBrace(), Newline());
}

void CWriter::WriteFuncs() {
  Index func_index = 0;
  for (const Func* func : module_->funcs) {
    bool is_import = func_index < module_->num_func_imports;
    if (!is_import)
      Write(Newline(), *func, Newline());
    ++func_index;
  }
}

void CWriter::PushFuncSection(std::string_view include_condition) {
  func_sections_.emplace_back(include_condition, MemoryStream{});
  stream_ = &func_sections_.back().second;
}

bool CWriter::IsImport(const std::string& name) const {
  return import_module_sym_map_.find(name) != import_module_sym_map_.end();
}

void CWriter::Write(const Func& func) {
  func_ = &func;
  // Copy symbols from global symbol table so we don't shadow them.
  local_syms_ = global_syms_;
  local_sym_map_.clear();
  stack_var_sym_map_.clear();
  func_sections_.clear();
  func_includes_.clear();

  Write("static ", ResultType(func.decl.sig.result_types), " ",
        GlobalName(ModuleFieldType::Func, func.name), "(");
  WriteParamsAndLocals();
  Write("FUNC_PROLOGUE;", Newline());

  PushFuncSection();

  std::string label = DefineLabelName(kImplicitFuncLabel);
  ResetTypeStack(0);
  std::string empty;  // Must not be temporary, since address is taken by Label.
  PushLabel(LabelType::Func, empty, func.decl.sig);
  Write(func.exprs, LabelDecl(label));
  PopLabel();
  ResetTypeStack(0);
  PushTypes(func.decl.sig.result_types);
  Write("FUNC_EPILOGUE;", Newline());

  // Return the top of the stack implicitly.
  Index num_results = func.GetNumResults();
  if (num_results == 1) {
    Write("return ", StackVar(0), ";", Newline());
  } else if (num_results >= 2) {
    Write(OpenBrace());
    Write(ResultType(func.decl.sig.result_types), " tmp;", Newline());
    for (Index i = 0; i < num_results; ++i) {
      Type type = func.GetResultType(i);
      Writef("tmp.%c%d = ", MangleType(type), i);
      Write(StackVar(num_results - i - 1), ";", Newline());
    }
    Write("return tmp;", Newline());
    Write(CloseBrace(), Newline());
  }

  stream_ = c_stream_;

  WriteStackVarDeclarations();

  for (auto& [condition, stream] : func_sections_) {
    std::unique_ptr<OutputBuffer> buf = stream.ReleaseOutputBuffer();
    if (condition.empty() || func_includes_.count(condition)) {
      stream_->WriteData(buf->data.data(), buf->data.size());
    }
  }

  Write(CloseBrace());

  func_ = nullptr;
}

void CWriter::WriteParamsAndLocals() {
  std::vector<std::string> index_to_name;
  MakeTypeBindingReverseMapping(func_->GetNumParamsAndLocals(), func_->bindings,
                                &index_to_name);
  WriteParams(index_to_name);
  Write(" ", OpenBrace());
  WriteLocals(index_to_name);
}

void CWriter::WriteParams(const std::vector<std::string>& index_to_name) {
  bool need_comma = false;
  if (options_.features.sandbox_enabled()) {
    Write(ModuleInstanceTypeName(), "* instance");
    need_comma = true;
  }
  if (func_->GetNumParams() != 0) {
    Indent(4);
    for (Index i = 0; i < func_->GetNumParams(); ++i) {
      if (need_comma) {
        Write(", ");
      } else {
        need_comma = true;
      }

      if (i != 0 && (i % 8) == 0) {
        Write(Newline());
      }
      Write(func_->GetParamType(i), " ", DefineParamName(index_to_name[i]));
    }
    Dedent(4);
  }
  Write(")");
}

void CWriter::WriteParamSymbols(const std::vector<std::string>& index_to_name) {
  bool needs_comma = options_.features.sandbox_enabled();
  if (func_->GetNumParams() != 0) {
    Indent(4);
    for (Index i = 0; i < func_->GetNumParams(); ++i) {
      if (needs_comma) {
        Write(", ");
      } else {
        needs_comma = true;
      }
      if (i != 0 && (i % 8) == 0) {
        Write(Newline());
      }
      Write(ParamName(index_to_name[i]));
    }
    Dedent(4);
  }
  Write(");", Newline());
}

void CWriter::WriteParamTypes(const FuncDeclaration& decl, bool is_varargs) {
  bool needs_comma = options_.features.sandbox_enabled();
  if (decl.GetNumParams() != 0) {
    for (Index i = 0; i < decl.GetNumParams(); ++i) {
      if (needs_comma) {
        Write(", ");
      } else {
        needs_comma = true;
      }
      if (is_varargs && i == decl.GetNumParams() - 1) {
        Write("...");
      } else {
        Write(decl.GetParamType(i));
      }
    }
  }
}

void CWriter::WriteLocals(const std::vector<std::string>& index_to_name) {
  Index num_params = func_->GetNumParams();
  for (Type type : {Type::I32, Type::I64, Type::F32, Type::F64, Type::V128,
                    Type::FuncRef, Type::ExternRef}) {
    Index local_index = 0;
    size_t count = 0;
    for (Type local_type : func_->local_types) {
      if (local_type == type) {
        if (count == 0) {
          Write(type, " ");
          Indent(4);
        } else {
          Write(", ");
          if ((count % 8) == 0)
            Write(Newline());
        }

        Write(DefineParamName(index_to_name[num_params + local_index]), " = ");
        if (local_type == Type::FuncRef || local_type == Type::ExternRef) {
          Write(GetReferenceNullValue(local_type));
        } else if (local_type == Type::V128) {
          Write("simde_wasm_i64x2_make(0, 0)");
        } else {
          Write("0");
        }
        ++count;
      }
      ++local_index;
    }
    if (count != 0) {
      Dedent(4);
      Write(";", Newline());
    }
  }
}

void CWriter::WriteStackVarDeclarations() {
  for (Type type : {Type::I32, Type::I64, Type::F32, Type::F64, Type::V128,
                    Type::FuncRef, Type::ExternRef}) {
    size_t count = 0;
    for (const auto& [pair, name] : stack_var_sym_map_) {
      Type stp_type = pair.second;

      if (stp_type == type) {
        if (count == 0) {
          Write(type, " ");
          Indent(4);
        } else {
          Write(", ");
          if ((count % 8) == 0)
            Write(Newline());
        }

        Write(name);
        ++count;
      }
    }
    if (count != 0) {
      Dedent(4);
      Write(";", Newline());
    }
  }
}

void CWriter::Write(const Block& block) {
  std::string label = DefineLabelName(block.label);
  DropTypes(block.decl.GetNumParams());
  size_t mark = MarkTypeStack();
  PushLabel(LabelType::Block, block.label, block.decl.sig);
  PushTypes(block.decl.sig.param_types);
  Write(block.exprs, LabelDecl(label));
  ResetTypeStack(mark);
  PopLabel();
  PushTypes(block.decl.sig.result_types);
}

size_t CWriter::BeginTry(const TryExpr& tryexpr) {
  Write(OpenBrace()); /* beginning of try-catch */
  const std::string tlabel = DefineLabelName(tryexpr.block.label);
  Write("WASM_RT_UNWIND_TARGET *", tlabel,
        "_outer_target = wasm_rt_get_unwind_target();", Newline());
  Write("WASM_RT_UNWIND_TARGET ", tlabel, "_unwind_target;", Newline());
  Write("if (!wasm_rt_try(", tlabel, "_unwind_target)) ");
  Write(OpenBrace()); /* beginning of try block */
  DropTypes(tryexpr.block.decl.GetNumParams());
  const size_t mark = MarkTypeStack();
  PushLabel(LabelType::Try, tryexpr.block.label, tryexpr.block.decl.sig);
  PushTypes(tryexpr.block.decl.sig.param_types);
  Write("wasm_rt_set_unwind_target(&", tlabel, "_unwind_target);", Newline());
  PushTryCatch(tlabel);
  Write(tryexpr.block.exprs);
  ResetTypeStack(mark);
  Write("wasm_rt_set_unwind_target(", tlabel, "_outer_target);", Newline());
  Write(CloseBrace());          /* end of try block */
  Write(" else ", OpenBrace()); /* beginning of catch blocks or delegate */
  assert(label_stack_.back().name == tryexpr.block.label);
  assert(label_stack_.back().label_type == LabelType::Try);
  label_stack_.back().label_type = LabelType::Catch;
  if (try_catch_stack_.back().used) {
    Write(tlabel, "_catch:;", Newline());
  }

  return mark;
}

void CWriter::WriteTryCatch(const TryExpr& tryexpr) {
  const size_t mark = BeginTry(tryexpr);

  /* exception has been thrown -- do we catch it? */

  const LabelName tlabel = LabelName(tryexpr.block.label);

  Write("wasm_rt_set_unwind_target(", tlabel, "_outer_target);", Newline());
  PopTryCatch();

  /* save the thrown exception to the stack if it might be rethrown later */
  PushFuncSection(tryexpr.block.label);
  Write("/* save exception ", tlabel, " for rethrow */", Newline());
  Write("const wasm_rt_tag_t ", tlabel, "_tag = wasm_rt_exception_tag();",
        Newline());
  Write("uint32_t ", tlabel, "_size = wasm_rt_exception_size();", Newline());
  Write("void *", tlabel, " = alloca(", tlabel, "_size);", Newline());
  Write("wasm_rt_memcpy(", tlabel, ", wasm_rt_exception(), ", tlabel, "_size);",
        Newline());
  PushFuncSection();

  assert(!tryexpr.catches.empty());
  bool has_catch_all{};
  for (auto it = tryexpr.catches.cbegin(); it != tryexpr.catches.cend(); ++it) {
    if (it == tryexpr.catches.cbegin()) {
      Write(Newline());
    } else {
      Write(" else ");
    }
    ResetTypeStack(mark);
    Write(*it);
    if (it->IsCatchAll()) {
      has_catch_all = true;
      break;
    }
  }
  if (!has_catch_all) {
    /* if not caught, rethrow */
    Write(" else ", OpenBrace());
    WriteThrow();
    Write(CloseBrace(), Newline());
  }
  Write(CloseBrace(), Newline()); /* end of catch blocks */
  Write(CloseBrace(), Newline()); /* end of try-catch */

  ResetTypeStack(mark);
  Write(LabelDecl(label_stack_.back().name));
  PopLabel();
  PushTypes(tryexpr.block.decl.sig.result_types);
}

void CWriter::Write(const Catch& c) {
  if (c.IsCatchAll()) {
    Write(c.exprs);
    return;
  }

  Write("if (wasm_rt_exception_tag() == ",
        ExternalPtr(ModuleFieldType::Tag, module_->GetTag(c.var)->name), ") ",
        OpenBrace());

  const Tag* tag = module_->GetTag(c.var);
  const FuncDeclaration& tag_type = tag->decl;
  const Index num_params = tag_type.GetNumParams();
  if (num_params == 1) {
    PushType(tag_type.GetParamType(0));
    Write("wasm_rt_memcpy(&", StackVar(0), ", wasm_rt_exception(), sizeof(",
          tag_type.GetParamType(0), "));", Newline());
  } else if (num_params > 1) {
    for (const auto& type : tag_type.sig.param_types) {
      PushType(type);
    }
    Write(OpenBrace());
    Write("struct ", MangleTagTypes(tag_type.sig.param_types), " tmp;",
          Newline());
    Write("wasm_rt_memcpy(&tmp, wasm_rt_exception(), sizeof(tmp));", Newline());
    for (unsigned int i = 0; i < tag_type.sig.param_types.size(); ++i) {
      Write(StackVar(i));
      Writef(" = tmp.%c%d;", MangleType(tag_type.sig.param_types.at(i)), i);
      Write(Newline());
    }

    Write(CloseBrace(), Newline());
  }

  Write(c.exprs);
  Write(CloseBrace());
}

void CWriter::WriteThrow() {
  if (try_catch_stack_.empty()) {
    Write("wasm_rt_throw();", Newline());
  } else {
    Write("goto ", try_catch_stack_.back().name, "_catch;", Newline());
    try_catch_stack_.back().used = true;
  }
}

void CWriter::PushTryCatch(const std::string& name) {
  try_catch_stack_.emplace_back(name, try_catch_stack_.size());
}

void CWriter::PopTryCatch() {
  assert(!try_catch_stack_.empty());
  try_catch_stack_.pop_back();
}

void CWriter::WriteTryDelegate(const TryExpr& tryexpr) {
  const size_t mark = BeginTry(tryexpr);

  /* exception has been thrown -- where do we delegate it? */

  if (tryexpr.delegate_target.is_index()) {
    /* must be the implicit function label */
    assert(!try_catch_stack_.empty());
    const std::string& unwind_name = try_catch_stack_.at(0).name;
    Write("wasm_rt_set_unwind_target(", unwind_name, "_outer_target);",
          Newline());

    Write("wasm_rt_throw();", Newline());
  } else {
    const Label* label = FindLabel(tryexpr.delegate_target, false);

    assert(try_catch_stack_.size() >= label->try_catch_stack_size);

    if (label->label_type == LabelType::Try) {
      Write("goto ", LabelName(label->name), "_catch;", Newline());
      try_catch_stack_.at(label->try_catch_stack_size).used = true;
    } else if (label->try_catch_stack_size == 0) {
      assert(!try_catch_stack_.empty());
      const std::string& unwind_name = try_catch_stack_.at(0).name;
      Write("wasm_rt_set_unwind_target(", unwind_name, "_outer_target);",
            Newline());

      Write("wasm_rt_throw();", Newline());
    } else {
      const std::string label_target =
          try_catch_stack_.at(label->try_catch_stack_size - 1).name + "_catch";
      Write("goto ", label_target, ";", Newline());
      try_catch_stack_.at(label->try_catch_stack_size - 1).used = true;
    }
  }

  Write(CloseBrace(), Newline());
  Write(CloseBrace(), Newline());

  PopTryCatch();
  ResetTypeStack(mark);
  Write(LabelDecl(label_stack_.back().name));
  PopLabel();
  PushTypes(tryexpr.block.decl.sig.result_types);
}

void CWriter::Write(const ExprList& exprs) {
  for (const Expr& expr : exprs) {
    switch (expr.type()) {
      case ExprType::Binary:
        Write(*cast<BinaryExpr>(&expr));
        break;

      case ExprType::Block:
        Write(cast<BlockExpr>(&expr)->block);
        break;

      case ExprType::Br:
        Write(GotoLabel(cast<BrExpr>(&expr)->var), Newline());
        // Stop processing this ExprList, since the following are unreachable.
        return;

      case ExprType::BrIf:
        Write("if (", StackVar(0), ") {");
        DropTypes(1);
        Write(GotoLabel(cast<BrIfExpr>(&expr)->var), "}", Newline());
        break;

      case ExprType::BrTable: {
        const auto* bt_expr = cast<BrTableExpr>(&expr);
        Write("switch (", StackVar(0), ") ", OpenBrace());
        DropTypes(1);
        Index i = 0;
        for (const Var& var : bt_expr->targets) {
          Write("case ", i++, ": ", GotoLabel(var), Newline());
        }
        Write("default: ");
        Write(GotoLabel(bt_expr->default_target), Newline(), CloseBrace(),
              Newline());
        // Stop processing this ExprList, since the following are unreachable.
        return;
      }

      case ExprType::Call: {
        const Var& var = cast<CallExpr>(&expr)->var;
        assert(var.is_name());

        const Func& func = *module_->GetFunc(var);
        Index num_params = func.GetNumParams();
        Index num_results = func.GetNumResults();

        if (isVarargsAllocatorFun(var.name())) {
          assert(num_results == 1);
          Write(OpenBrace());
          for (Index i = 0; i < num_params; ++i) {
            Write(StackType(num_params - i - 1), " ",
                  VaTempVar(i), " = ",
                  StackVar(num_params - i - 1),
                  ";", Newline());
          }
          PrepareVarargs(num_params);
          PushType(Type::VarargsPlaceholder);
          break;
        }

        Index num_vparams = 0;
        bool is_varargs = IsVarargs();
        if (is_varargs) {
          num_vparams = TakeVarargs();
          num_params -= 1;
          assert(StackType(0) == Type::VarargsPlaceholder);
          DropTypes(1);
        }

        assert(type_stack_.size() >= num_params);

        bool is_import = IsImport(func.name);

        if (is_varargs && !is_import) {
          Write("struct ", OpenBrace());
          for (Index i = 0; i < num_vparams; ++i) {
            Write(varargs_types_[i], " ", VaTempVar(i), ";", Newline());
          }
          Write(CloseBrace(), " ", kSymbolPrefix, "va_struct = ", OpenBrace());
          for (Index i = 0; i < num_vparams; ++i) {
            Write(VaTempVar(i), ",", Newline());
          }
          Write(CloseBrace(), ";", Newline());
        }

        if (num_results > 1) {
          Write(OpenBrace());
          Write("struct ", MangleMultivalueTypes(func.decl.sig.result_types));
          Write(" tmp = ");
        } else if (num_results == 1) {
          Write(StackVar(num_params - 1, func.GetResultType(0)), " = ");
        }

        Write(ExternalRef(ModuleFieldType::Func, var.name()), "(");

        if (options_.features.sandbox_enabled()) {
          if (is_import) {
            Write("instance->", GlobalName(ModuleFieldType::Import,
                                           import_module_sym_map_[func.name]));
          } else {
            Write("instance");
          }
        }
 
        bool needs_comma = options_.features.sandbox_enabled();
        for (Index i = 0; i < num_params; ++i) {
          if (needs_comma) {
            Write(", ");
          } else {
            needs_comma = true;
          }
          Write(StackVar(num_params - i - 1));
        }

        // Write variable portion of argument list.
        if (is_varargs) {
          if (is_import) {
            for (Index i = 0; i < num_vparams; ++i) {
              if (needs_comma) {
                Write(", ");
              } else {
                needs_comma = true;
              }
              Write(VaTempVar(i));
            }
          } else {
            if (needs_comma) {
              Write(", ");
            }
            Write("&", kSymbolPrefix, "va_struct");
          }
        }

        // Close the argument list and finish the call.
        Write(");", Newline());
 
        DropTypes(num_params);
        if (num_results > 1) {
          for (Index i = 0; i < num_results; ++i) {
            Type type = func.GetResultType(i);
            PushType(type);
            Write(StackVar(0));
            Writef(" = tmp.%c%d;", MangleType(type), i);
            Write(Newline());
          }
          Write(CloseBrace(), Newline());
        } else {
          PushTypes(func.decl.sig.result_types);
        }

        // Leave scope of varargs temp
        if (is_varargs) {
          Write(CloseBrace(), Newline());
        }
       break;
      }

      case ExprType::CallIndirect: {
        const FuncDeclaration& decl = cast<CallIndirectExpr>(&expr)->decl;
        Index num_params = decl.GetNumParams();
        Index num_results = decl.GetNumResults();

        bool is_varargs = IsVarargs();
        
        assert(type_stack_.size() > num_params);
        if (num_results > 1) {
          Write(OpenBrace());
          Write("struct ", MangleMultivalueTypes(decl.sig.result_types));
          Write(" tmp = ");
        } else if (num_results == 1) {
          Write(StackVar(num_params, decl.GetResultType(0)), " = ");
        }

        bool needs_comma = false;
        if (!options_.features.sandbox_enabled()) {
          Write("((");
          WriteCallIndirectFuncDeclaration(decl, "(*)", is_varargs);
          Write(")", StackVar(0), ")(");
        } else {
          const Table* table =
              module_->GetTable(cast<CallIndirectExpr>(&expr)->table);

          assert(decl.has_func_type);
          const FuncType* func_type = module_->GetFuncType(decl.type_var);

          Write("CALL_INDIRECT(",
                ExternalInstanceRef(ModuleFieldType::Table, table->name), ", ");
          WriteCallIndirectFuncDeclaration(decl, "(*)");
          Write(", ", FuncTypeExpr(func_type), ", ", StackVar(0));
          Write(", ", ExternalInstanceRef(ModuleFieldType::Table, table->name),
                ".data[", StackVar(0), "].module_instance");
          needs_comma = true;
        }
        DropTypes(1);

        Index num_vparams = 0;
        if (is_varargs) {
          assert(num_params > 1);
          num_params -= 1;
          num_vparams = TakeVarargs();
          assert(StackType(0) == Type::VarargsPlaceholder);
          DropTypes(1);
        }

        for (Index i = 0; i < num_params; ++i) {
          if (needs_comma) {
            Write(", ");
          } else {
            needs_comma = true;
          }
          Write(StackVar(num_params - i - 1));
        }
        for (Index i = 0; i < num_vparams; ++i) {
          if (needs_comma) {
            Write(", ");
          } else {
            needs_comma = true;
          }
          Write(VaTempVar(i));
        }
        Write(");", Newline());
        DropTypes(num_params);
        if (num_results > 1) {
          for (Index i = 0; i < num_results; ++i) {
            Type type = decl.GetResultType(i);
            PushType(type);
            Write(StackVar(0));
            Writef(" = tmp.%c%d;", MangleType(type), i);
            Write(Newline());
          }
          Write(CloseBrace(), Newline());
        } else {
          PushTypes(decl.sig.result_types);
        }
        if (is_varargs) {
          Write(CloseBrace(), Newline());
        }
        break;
      }

      case ExprType::CodeMetadata:
        break;

      case ExprType::Compare:
        Write(*cast<CompareExpr>(&expr));
        break;

      case ExprType::Const:
        Write(*cast<ConstExpr>(&expr));
        break;

      case ExprType::Convert:
        Write(*cast<ConvertExpr>(&expr));
        break;

      case ExprType::Drop:
        DropTypes(1);
        break;

      case ExprType::GlobalGet: {
        const Var& var = cast<GlobalGetExpr>(&expr)->var;
        if (!options_.features.sandbox_enabled() &&
            var.name() == kStackPointerName) {
          PushType(module_->GetGlobal(var)->type);
          Write(StackVar(0), " = ((uintptr_t)&", kStackArrayVariableName,
                "[0]) + ", kStackOffsetGlobalVariableName, ";", Newline());
        } else {
          PushType(module_->GetGlobal(var)->type);
          Write(StackVar(0), " = ", GlobalInstanceVar(var), ";", Newline());
        }
        break;
      }

      case ExprType::GlobalSet: {
        const Var& var = cast<GlobalSetExpr>(&expr)->var;
        if (!options_.features.sandbox_enabled() &&
            var.name() == kStackPointerName) {
          Write("if (UNLIKELY(", StackVar(0), " < ", kStackBufferSize,
                " + (uintptr_t)", kStackArrayVariableName, ")) TRAP(OOB);",
                Newline());
          Write("if (UNLIKELY(", StackVar(0), " > ",
                kStackBufferSize + kStackSize, " + (uintptr_t)",
                kStackArrayVariableName, ")) TRAP(OOB);", Newline());
          Write(kStackOffsetGlobalVariableName, " = ", StackVar(0),
                " - ((uintptr_t)&", kStackArrayVariableName, "[0]);",
                Newline());
        } else {
          Write(GlobalInstanceVar(var), " = ", StackVar(0), ";", Newline());
        }
        DropTypes(1);
        break;
      }

      case ExprType::If: {
        const IfExpr& if_ = *cast<IfExpr>(&expr);
        Write("if (", StackVar(0), ") ", OpenBrace());
        DropTypes(1);
        std::string label = DefineLabelName(if_.true_.label);
        DropTypes(if_.true_.decl.GetNumParams());
        size_t mark = MarkTypeStack();
        PushLabel(LabelType::If, if_.true_.label, if_.true_.decl.sig);
        PushTypes(if_.true_.decl.sig.param_types);
        Write(if_.true_.exprs, CloseBrace());
        if (!if_.false_.empty()) {
          ResetTypeStack(mark);
          PushTypes(if_.true_.decl.sig.param_types);
          Write(" else ", OpenBrace(), if_.false_, CloseBrace());
        }
        ResetTypeStack(mark);
        Write(Newline(), LabelDecl(label));
        PopLabel();
        PushTypes(if_.true_.decl.sig.result_types);
        break;
      }

      case ExprType::Load:
        Write(*cast<LoadExpr>(&expr));
        break;

      case ExprType::LocalGet: {
        const Var& var = cast<LocalGetExpr>(&expr)->var;
        PushType(func_->GetLocalType(var));
        Write(StackVar(0), " = ", var, ";", Newline());
        break;
      }

      case ExprType::LocalSet: {
        const Var& var = cast<LocalSetExpr>(&expr)->var;
        Write(var, " = ", StackVar(0), ";", Newline());
        DropTypes(1);
        break;
      }

      case ExprType::LocalTee: {
        const Var& var = cast<LocalTeeExpr>(&expr)->var;
        Write(var, " = ", StackVar(0), ";", Newline());
        break;
      }

      case ExprType::Loop: {
        const Block& block = cast<LoopExpr>(&expr)->block;
        if (!block.exprs.empty()) {
          Write(DefineLabelName(block.label), ": ");
          Indent();
          DropTypes(block.decl.GetNumParams());
          size_t mark = MarkTypeStack();
          PushLabel(LabelType::Loop, block.label, block.decl.sig);
          PushTypes(block.decl.sig.param_types);
          Write(Newline(), block.exprs);
          ResetTypeStack(mark);
          PopLabel();
          PushTypes(block.decl.sig.result_types);
          Dedent();
        }
        break;
      }

      case ExprType::MemoryFill: {
        const auto inst = cast<MemoryFillExpr>(&expr);
        Memory* memory =
            module_->memories[module_->GetMemoryIndex(inst->memidx)];
        Write("memory_fill(",
              ExternalInstancePtr(ModuleFieldType::Memory, memory->name), ", ",
              StackVar(2), ", ", StackVar(1), ", ", StackVar(0), ");",
              Newline());
        DropTypes(3);
      } break;

      case ExprType::MemoryCopy: {
        const auto inst = cast<MemoryCopyExpr>(&expr);
        Memory* dest_memory =
            module_->memories[module_->GetMemoryIndex(inst->destmemidx)];
        const Memory* src_memory = module_->GetMemory(inst->srcmemidx);
        Write("memory_copy(",
              ExternalInstancePtr(ModuleFieldType::Memory, dest_memory->name),
              ", ",
              ExternalInstancePtr(ModuleFieldType::Memory, src_memory->name),
              ", ", StackVar(2), ", ", StackVar(1), ", ", StackVar(0), ");",
              Newline());
        DropTypes(3);
      } break;

      case ExprType::MemoryInit: {
        const auto inst = cast<MemoryInitExpr>(&expr);
        Memory* dest_memory =
            module_->memories[module_->GetMemoryIndex(inst->memidx)];
        const DataSegment* src_data = module_->GetDataSegment(inst->var);
        Write("memory_init(",
              ExternalInstancePtr(ModuleFieldType::Memory, dest_memory->name),
              ", ");
        if (src_data->data.empty()) {
          Write("NULL, 0");
        } else {
          Write("data_segment_data_",
                GlobalName(ModuleFieldType::DataSegment, src_data->name), ", ");
          if (is_droppable(src_data)) {
            Write("(", "instance->data_segment_dropped_",
                  GlobalName(ModuleFieldType::DataSegment, src_data->name),
                  " ? 0 : ", src_data->data.size(), ")");
          } else {
            Write("0");
          }
        }

        Write(", ", StackVar(2), ", ", StackVar(1), ", ", StackVar(0), ");",
              Newline());
        DropTypes(3);
      } break;

      case ExprType::TableInit: {
        const auto inst = cast<TableInitExpr>(&expr);
        Table* dest_table =
            module_->tables[module_->GetTableIndex(inst->table_index)];
        const ElemSegment* src_segment =
            module_->GetElemSegment(inst->segment_index);

        WriteElemTableInit(false, src_segment, dest_table);
        DropTypes(3);
      } break;

      case ExprType::DataDrop: {
        const auto inst = cast<DataDropExpr>(&expr);
        const DataSegment* data = module_->GetDataSegment(inst->var);
        if (is_droppable(data)) {
          Write("instance->data_segment_dropped_",
                GlobalName(ModuleFieldType::DataSegment, data->name),
                " = true;", Newline());
        }
      } break;

      case ExprType::ElemDrop: {
        const auto inst = cast<ElemDropExpr>(&expr);
        const ElemSegment* seg = module_->GetElemSegment(inst->var);
        if (is_droppable(seg)) {
          Write("instance->elem_segment_dropped_",
                GlobalName(ModuleFieldType::ElemSegment, seg->name), " = true;",
                Newline());
        }
      } break;

      case ExprType::TableCopy: {
        const auto inst = cast<TableCopyExpr>(&expr);
        Table* dest_table =
            module_->tables[module_->GetTableIndex(inst->dst_table)];
        const Table* src_table = module_->GetTable(inst->src_table);
        if (dest_table->elem_type != src_table->elem_type) {
          WABT_UNREACHABLE;
        }

        Write(
            GetReferenceTypeName(dest_table->elem_type), "_table_copy(",
            ExternalInstancePtr(ModuleFieldType::Table, dest_table->name), ", ",
            ExternalInstancePtr(ModuleFieldType::Table, src_table->name), ", ",
            StackVar(2), ", ", StackVar(1), ", ", StackVar(0), ");", Newline());
        DropTypes(3);
      } break;

      case ExprType::TableGet: {
        const Table* table = module_->GetTable(cast<TableGetExpr>(&expr)->var);
        Write(StackVar(0, table->elem_type), " = ",
              GetReferenceTypeName(table->elem_type), "_table_get(",
              ExternalInstancePtr(ModuleFieldType::Table, table->name), ", ",
              StackVar(0), ");", Newline());
        DropTypes(1);
        PushType(table->elem_type);
      } break;

      case ExprType::TableSet: {
        const Table* table = module_->GetTable(cast<TableSetExpr>(&expr)->var);
        Write(GetReferenceTypeName(table->elem_type), "_table_set(",
              ExternalInstancePtr(ModuleFieldType::Table, table->name), ", ",
              StackVar(1), ", ", StackVar(0), ");", Newline());
        DropTypes(2);
      } break;

      case ExprType::TableGrow: {
        const Table* table = module_->GetTable(cast<TableGrowExpr>(&expr)->var);
        Write(StackVar(1, Type::I32), " = wasm_rt_grow_",
              GetReferenceTypeName(table->elem_type), "_table(",
              ExternalInstancePtr(ModuleFieldType::Table, table->name), ", ",
              StackVar(0), ", ", StackVar(1), ");", Newline());
        DropTypes(2);
        PushType(Type::I32);
      } break;

      case ExprType::TableSize: {
        const Table* table = module_->GetTable(cast<TableSizeExpr>(&expr)->var);

        PushType(Type::I32);
        Write(StackVar(0), " = ",
              ExternalInstanceRef(ModuleFieldType::Table, table->name),
              ".size;", Newline());
      } break;

      case ExprType::TableFill: {
        const Table* table = module_->GetTable(cast<TableFillExpr>(&expr)->var);
        Write(GetReferenceTypeName(table->elem_type), "_table_fill(",
              ExternalInstancePtr(ModuleFieldType::Table, table->name), ", ",
              StackVar(2), ", ", StackVar(1), ", ", StackVar(0), ");",
              Newline());
        DropTypes(3);
      } break;

      case ExprType::RefFunc: {
        const Func* func = module_->GetFunc(cast<RefFuncExpr>(&expr)->var);
        PushType(Type::FuncRef);
        const FuncDeclaration& decl = func->decl;

        assert(decl.has_func_type);
        const FuncType* func_type = module_->GetFuncType(decl.type_var);

        Write(StackVar(0), " = (wasm_rt_funcref_t){", FuncTypeExpr(func_type),
              ", (wasm_rt_function_ptr_t)",
              ExternalPtr(ModuleFieldType::Func, func->name), ", ");

        bool is_import = import_module_sym_map_.count(func->name) != 0;
        if (is_import) {
          Write("instance->", GlobalName(ModuleFieldType::Import,
                                         import_module_sym_map_[func->name]));
        } else {
          Write("instance");
        }

        Write("};", Newline());
      } break;

      case ExprType::RefNull:
        PushType(cast<RefNullExpr>(&expr)->type);
        Write(StackVar(0), " = ",
              GetReferenceNullValue(cast<RefNullExpr>(&expr)->type), ";",
              Newline());
        break;

      case ExprType::RefIsNull:
        switch (StackType(0)) {
          case Type::FuncRef:
            Write(StackVar(0, Type::I32), " = (", StackVar(0), ".func == NULL",
                  ");", Newline());
            break;
          case Type::ExternRef:
            Write(StackVar(0, Type::I32), " = (", StackVar(0),
                  " == ", GetReferenceNullValue(Type::ExternRef), ");",
                  Newline());
            break;
          default:
            WABT_UNREACHABLE;
        }

        DropTypes(1);
        PushType(Type::I32);
        break;

      case ExprType::MemoryGrow: {
        Memory* memory = module_->memories[module_->GetMemoryIndex(
            cast<MemoryGrowExpr>(&expr)->memidx)];

        Write(StackVar(0), " = wasm_rt_grow_memory(",
              ExternalInstancePtr(ModuleFieldType::Memory, memory->name), ", ",
              StackVar(0), ");", Newline());
        break;
      }

      case ExprType::MemorySize: {
        Memory* memory = module_->memories[module_->GetMemoryIndex(
            cast<MemorySizeExpr>(&expr)->memidx)];

        PushType(memory->page_limits.IndexType());
        Write(StackVar(0), " = ",
              ExternalInstanceRef(ModuleFieldType::Memory, memory->name),
              ".pages;", Newline());
        break;
      }

      case ExprType::Nop:
        break;

      case ExprType::Return:
        // Goto the function label instead; this way we can do shared function
        // cleanup code in one place.
        Write(GotoLabel(Var(label_stack_.size() - 1, {})), Newline());
        // Stop processing this ExprList, since the following are unreachable.
        return;

      case ExprType::Select: {
        Type type = StackType(1);
        Write(StackVar(2), " = ", StackVar(0), " ? ", StackVar(2), " : ",
              StackVar(1), ";", Newline());
        DropTypes(3);
        PushType(type);
        break;
      }

      case ExprType::Store:
        Write(*cast<StoreExpr>(&expr));
        break;

      case ExprType::Unary:
        Write(*cast<UnaryExpr>(&expr));
        break;

      case ExprType::Ternary:
        Write(*cast<TernaryExpr>(&expr));
        break;

      case ExprType::SimdLaneOp: {
        Write(*cast<SimdLaneOpExpr>(&expr));
        break;
      }

      case ExprType::SimdLoadLane: {
        Write(*cast<SimdLoadLaneExpr>(&expr));
        break;
      }

      case ExprType::SimdStoreLane: {
        Write(*cast<SimdStoreLaneExpr>(&expr));
        break;
      }

      case ExprType::SimdShuffleOp: {
        Write(*cast<SimdShuffleOpExpr>(&expr));
        break;
      }

      case ExprType::LoadSplat:
        Write(*cast<LoadSplatExpr>(&expr));
        break;

      case ExprType::LoadZero:
        Write(*cast<LoadZeroExpr>(&expr));
        break;

      case ExprType::Unreachable:
        Write("UNREACHABLE;", Newline());
        return;

      case ExprType::Throw: {
        const Var& var = cast<ThrowExpr>(&expr)->var;
        const Tag* tag = module_->GetTag(var);

        Index num_params = tag->decl.GetNumParams();
        if (num_params == 0) {
          Write("wasm_rt_load_exception(",
                ExternalPtr(ModuleFieldType::Tag, tag->name), ", 0, NULL);",
                Newline());
        } else if (num_params == 1) {
          Write("wasm_rt_load_exception(",
                ExternalPtr(ModuleFieldType::Tag, tag->name), ", sizeof(",
                tag->decl.GetParamType(0), "), &", StackVar(0), ");",
                Newline());
        } else {
          Write(OpenBrace());
          Write("struct ", MangleTagTypes(tag->decl.sig.param_types));
          Write(" tmp = {");
          for (Index i = 0; i < num_params; ++i) {
            Write(StackVar(i), ", ");
          }
          Write("};", Newline());
          Write("wasm_rt_load_exception(",
                ExternalPtr(ModuleFieldType::Tag, tag->name),
                ", sizeof(tmp), &tmp);", Newline());
          Write(CloseBrace(), Newline());
        }

        WriteThrow();
      } break;

      case ExprType::Rethrow: {
        const RethrowExpr* rethrow = cast<RethrowExpr>(&expr);
        assert(rethrow->var.is_name());
        const LabelName ex{rethrow->var.name()};
        func_includes_.insert(rethrow->var.name());
        Write("wasm_rt_load_exception(", ex, "_tag, ", ex, "_size, ", ex, ");",
              Newline());
        WriteThrow();
      } break;

      case ExprType::Try: {
        const TryExpr& tryexpr = *cast<TryExpr>(&expr);
        switch (tryexpr.kind) {
          case TryKind::Plain:
            Write(tryexpr.block);
            break;
          case TryKind::Catch:
            WriteTryCatch(tryexpr);
            break;
          case TryKind::Delegate:
            WriteTryDelegate(tryexpr);
            break;
        }
      } break;

      case ExprType::AtomicLoad:
      case ExprType::AtomicRmw:
      case ExprType::AtomicRmwCmpxchg:
      case ExprType::AtomicStore:
      case ExprType::AtomicWait:
      case ExprType::AtomicFence:
      case ExprType::AtomicNotify:
      case ExprType::ReturnCall:
      case ExprType::ReturnCallIndirect:
      case ExprType::CallRef:
        UNIMPLEMENTED("...");
        break;
    }
  }
}

void CWriter::WriteSimpleUnaryExpr(Opcode opcode, const char* op) {
  Type result_type = opcode.GetResultType();
  Write(StackVar(0, result_type), " = ", op, "(", StackVar(0), ");", Newline());
  DropTypes(1);
  PushType(opcode.GetResultType());
}

void CWriter::WriteInfixBinaryExpr(Opcode opcode,
                                   const char* op,
                                   AssignOp assign_op) {
  Type result_type = opcode.GetResultType();
  Write(StackVar(1, result_type));
  if (assign_op == AssignOp::Allowed) {
    Write(" ", op, "= ", StackVar(0));
  } else {
    Write(" = ", StackVar(1), " ", op, " ", StackVar(0));
  }
  Write(";", Newline());
  DropTypes(2);
  PushType(result_type);
}

void CWriter::WritePrefixBinaryExpr(Opcode opcode, const char* op) {
  Type result_type = opcode.GetResultType();
  Write(StackVar(1, result_type), " = ", op, "(", StackVar(1), ", ",
        StackVar(0), ");", Newline());
  DropTypes(2);
  PushType(result_type);
}

void CWriter::WriteSignedBinaryExpr(Opcode opcode, const char* op) {
  Type result_type = opcode.GetResultType();
  Type type = opcode.GetParamType1();
  assert(opcode.GetParamType2() == type);
  Write(StackVar(1, result_type), " = (", type, ")((", SignedType(type), ")",
        StackVar(1), " ", op, " (", SignedType(type), ")", StackVar(0), ");",
        Newline());
  DropTypes(2);
  PushType(result_type);
}

void CWriter::Write(const BinaryExpr& expr) {
  switch (expr.opcode) {
    case Opcode::I32Add:
    case Opcode::I64Add:
    case Opcode::F32Add:
    case Opcode::F64Add:
      WriteInfixBinaryExpr(expr.opcode, "+");
      break;

    case Opcode::I32Sub:
    case Opcode::I64Sub:
    case Opcode::F32Sub:
    case Opcode::F64Sub:
      WriteInfixBinaryExpr(expr.opcode, "-");
      break;

    case Opcode::I32Mul:
    case Opcode::I64Mul:
    case Opcode::F32Mul:
    case Opcode::F64Mul:
      WriteInfixBinaryExpr(expr.opcode, "*");
      break;

    case Opcode::I32DivS:
      WritePrefixBinaryExpr(expr.opcode, "I32_DIV_S");
      break;

    case Opcode::I64DivS:
      WritePrefixBinaryExpr(expr.opcode, "I64_DIV_S");
      break;

    case Opcode::I32DivU:
    case Opcode::I64DivU:
      WritePrefixBinaryExpr(expr.opcode, "DIV_U");
      break;

    case Opcode::F32Div:
    case Opcode::F64Div:
      WriteInfixBinaryExpr(expr.opcode, "/");
      break;

    case Opcode::I32RemS:
      WritePrefixBinaryExpr(expr.opcode, "I32_REM_S");
      break;

    case Opcode::I64RemS:
      WritePrefixBinaryExpr(expr.opcode, "I64_REM_S");
      break;

    case Opcode::I32RemU:
    case Opcode::I64RemU:
      WritePrefixBinaryExpr(expr.opcode, "REM_U");
      break;

    case Opcode::I32And:
    case Opcode::I64And:
      WriteInfixBinaryExpr(expr.opcode, "&");
      break;

    case Opcode::I32Or:
    case Opcode::I64Or:
      WriteInfixBinaryExpr(expr.opcode, "|");
      break;

    case Opcode::I32Xor:
    case Opcode::I64Xor:
      WriteInfixBinaryExpr(expr.opcode, "^");
      break;

    case Opcode::I32Shl:
    case Opcode::I64Shl:
      Write(StackVar(1), " <<= (", StackVar(0), " & ",
            GetShiftMask(expr.opcode.GetResultType()), ");", Newline());
      DropTypes(1);
      break;

    case Opcode::I32ShrS:
    case Opcode::I64ShrS: {
      Type type = expr.opcode.GetResultType();
      Write(StackVar(1), " = (", type, ")((", SignedType(type), ")",
            StackVar(1), " >> (", StackVar(0), " & ", GetShiftMask(type), "));",
            Newline());
      DropTypes(1);
      break;
    }

    case Opcode::I32ShrU:
    case Opcode::I64ShrU:
      Write(StackVar(1), " >>= (", StackVar(0), " & ",
            GetShiftMask(expr.opcode.GetResultType()), ");", Newline());
      DropTypes(1);
      break;

    case Opcode::I32Rotl:
      WritePrefixBinaryExpr(expr.opcode, "I32_ROTL");
      break;

    case Opcode::I64Rotl:
      WritePrefixBinaryExpr(expr.opcode, "I64_ROTL");
      break;

    case Opcode::I32Rotr:
      WritePrefixBinaryExpr(expr.opcode, "I32_ROTR");
      break;

    case Opcode::I64Rotr:
      WritePrefixBinaryExpr(expr.opcode, "I64_ROTR");
      break;

    case Opcode::F32Min:
    case Opcode::F64Min:
      WritePrefixBinaryExpr(expr.opcode, "FMIN");
      break;

    case Opcode::F32Max:
    case Opcode::F64Max:
      WritePrefixBinaryExpr(expr.opcode, "FMAX");
      break;

    case Opcode::F32Copysign:
      WritePrefixBinaryExpr(expr.opcode, "copysignf");
      break;

    case Opcode::F64Copysign:
      WritePrefixBinaryExpr(expr.opcode, "copysign");
      break;

    case Opcode::I8X16Add:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i8x16_add");
      break;

    case Opcode::I8X16AddSatS:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i8x16_add_sat");
      break;

    case Opcode::I8X16AddSatU:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_u8x16_add_sat");
      break;

    case Opcode::I8X16AvgrU:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_u8x16_avgr");
      break;

    case Opcode::I8X16MaxS:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i8x16_max");
      break;

    case Opcode::I8X16MaxU:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_u8x16_max");
      break;

    case Opcode::I8X16MinS:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i8x16_min");
      break;

    case Opcode::I8X16MinU:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_u8x16_min");
      break;

    case Opcode::I8X16NarrowI16X8S:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i8x16_narrow_i16x8");
      break;

    case Opcode::I8X16NarrowI16X8U:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_u8x16_narrow_i16x8");
      break;

    case Opcode::I8X16Shl:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i8x16_shl");
      break;

    case Opcode::I8X16ShrS:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i8x16_shr");
      break;

    case Opcode::I8X16ShrU:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_u8x16_shr");
      break;

    case Opcode::I8X16Sub:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i8x16_sub");
      break;

    case Opcode::I8X16SubSatS:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i8x16_sub_sat");
      break;

    case Opcode::I8X16SubSatU:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_u8x16_sub_sat");
      break;

    case Opcode::I8X16Swizzle:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i8x16_swizzle");
      break;

    case Opcode::I16X8Add:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i16x8_add");
      break;

    case Opcode::I16X8AvgrU:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_u16x8_avgr");
      break;

    case Opcode::I16X8AddSatS:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i16x8_add_sat");
      break;

    case Opcode::I16X8AddSatU:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_u16x8_add_sat");
      break;

    case Opcode::I16X8ExtmulHighI8X16S:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i16x8_extmul_high_i8x16");
      break;

    case Opcode::I16X8ExtmulHighI8X16U:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_u16x8_extmul_high_u8x16");
      break;

    case Opcode::I16X8ExtmulLowI8X16S:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i16x8_extmul_low_i8x16");
      break;

    case Opcode::I16X8ExtmulLowI8X16U:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_u16x8_extmul_low_u8x16");
      break;

    case Opcode::I16X8MaxS:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i16x8_max");
      break;

    case Opcode::I16X8MaxU:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_u16x8_max");
      break;

    case Opcode::I16X8MinS:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i16x8_min");
      break;

    case Opcode::I16X8MinU:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_u16x8_min");
      break;

    case Opcode::I16X8Mul:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i16x8_mul");
      break;

    case Opcode::I16X8NarrowI32X4S:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i16x8_narrow_i32x4");
      break;

    case Opcode::I16X8NarrowI32X4U:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_u16x8_narrow_i32x4");
      break;

    case Opcode::I16X8Q15mulrSatS:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i16x8_q15mulr_sat");
      break;

    case Opcode::I16X8Shl:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i16x8_shl");
      break;

    case Opcode::I16X8ShrS:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i16x8_shr");
      break;

    case Opcode::I16X8ShrU:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_u16x8_shr");
      break;

    case Opcode::I16X8Sub:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i16x8_sub");
      break;

    case Opcode::I16X8SubSatS:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i16x8_sub_sat");
      break;

    case Opcode::I16X8SubSatU:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_u16x8_sub_sat");
      break;

    case Opcode::I32X4Add:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i32x4_add");
      break;

    case Opcode::I32X4DotI16X8S:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i32x4_dot_i16x8");
      break;

    case Opcode::I32X4ExtmulHighI16X8S:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i32x4_extmul_high_i16x8");
      break;

    case Opcode::I32X4ExtmulHighI16X8U:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_u32x4_extmul_high_u16x8");
      break;

    case Opcode::I32X4ExtmulLowI16X8S:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i32x4_extmul_low_i16x8");
      break;

    case Opcode::I32X4ExtmulLowI16X8U:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_u32x4_extmul_low_u16x8");
      break;

    case Opcode::I32X4MaxS:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i32x4_max");
      break;

    case Opcode::I32X4MaxU:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_u32x4_max");
      break;

    case Opcode::I32X4MinS:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i32x4_min");
      break;

    case Opcode::I32X4MinU:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_u32x4_min");
      break;

    case Opcode::I32X4Mul:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i32x4_mul");
      break;

    case Opcode::I32X4Shl:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i32x4_shl");
      break;

    case Opcode::I32X4ShrS:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i32x4_shr");
      break;

    case Opcode::I32X4ShrU:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_u32x4_shr");
      break;

    case Opcode::I32X4Sub:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i32x4_sub");
      break;

    case Opcode::I64X2Add:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i64x2_add");
      break;

    case Opcode::I64X2ExtmulHighI32X4S:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i64x2_extmul_high_i32x4");
      break;

    case Opcode::I64X2ExtmulHighI32X4U:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_u64x2_extmul_high_u32x4");
      break;

    case Opcode::I64X2ExtmulLowI32X4S:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i64x2_extmul_low_i32x4");
      break;

    case Opcode::I64X2ExtmulLowI32X4U:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_u64x2_extmul_low_u32x4");
      break;

    case Opcode::I64X2Mul:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i64x2_mul");
      break;

    case Opcode::I64X2Shl:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i64x2_shl");
      break;

    case Opcode::I64X2ShrS:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i64x2_shr");
      break;

    case Opcode::I64X2ShrU:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_u64x2_shr");
      break;

    case Opcode::I64X2Sub:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i64x2_sub");
      break;

    case Opcode::F32X4Add:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_f32x4_add");
      break;

    case Opcode::F32X4Div:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_f32x4_div");
      break;

    case Opcode::F32X4Max:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_f32x4_max");
      break;

    case Opcode::F32X4Mul:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_f32x4_mul");
      break;

    case Opcode::F32X4Min:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_f32x4_min");
      break;

    case Opcode::F32X4PMax:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_f32x4_pmax");
      break;

    case Opcode::F32X4PMin:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_f32x4_pmin");
      break;

    case Opcode::F32X4Sub:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_f32x4_sub");
      break;

    case Opcode::F64X2Add:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_f64x2_add");
      break;

    case Opcode::F64X2Div:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_f64x2_div");
      break;

    case Opcode::F64X2Max:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_f64x2_max");
      break;

    case Opcode::F64X2Mul:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_f64x2_mul");
      break;

    case Opcode::F64X2Min:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_f64x2_min");
      break;

    case Opcode::F64X2PMax:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_f64x2_pmax");
      break;

    case Opcode::F64X2PMin:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_f64x2_pmin");
      break;

    case Opcode::F64X2Sub:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_f64x2_sub");
      break;

    case Opcode::V128And:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_v128_and");
      break;

    case Opcode::V128Andnot:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_v128_andnot");
      break;

    case Opcode::V128Or:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_v128_or");
      break;

    case Opcode::V128Xor:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_v128_xor");
      break;

    default:
      WABT_UNREACHABLE;
  }
}

void CWriter::Write(const CompareExpr& expr) {
  switch (expr.opcode) {
    case Opcode::I32Eq:
    case Opcode::I64Eq:
    case Opcode::F32Eq:
    case Opcode::F64Eq:
      WriteInfixBinaryExpr(expr.opcode, "==", AssignOp::Disallowed);
      break;

    case Opcode::I32Ne:
    case Opcode::I64Ne:
    case Opcode::F32Ne:
    case Opcode::F64Ne:
      WriteInfixBinaryExpr(expr.opcode, "!=", AssignOp::Disallowed);
      break;

    case Opcode::I32LtS:
    case Opcode::I64LtS:
      WriteSignedBinaryExpr(expr.opcode, "<");
      break;

    case Opcode::I32LtU:
    case Opcode::I64LtU:
    case Opcode::F32Lt:
    case Opcode::F64Lt:
      WriteInfixBinaryExpr(expr.opcode, "<", AssignOp::Disallowed);
      break;

    case Opcode::I32LeS:
    case Opcode::I64LeS:
      WriteSignedBinaryExpr(expr.opcode, "<=");
      break;

    case Opcode::I32LeU:
    case Opcode::I64LeU:
    case Opcode::F32Le:
    case Opcode::F64Le:
      WriteInfixBinaryExpr(expr.opcode, "<=", AssignOp::Disallowed);
      break;

    case Opcode::I32GtS:
    case Opcode::I64GtS:
      WriteSignedBinaryExpr(expr.opcode, ">");
      break;

    case Opcode::I32GtU:
    case Opcode::I64GtU:
    case Opcode::F32Gt:
    case Opcode::F64Gt:
      WriteInfixBinaryExpr(expr.opcode, ">", AssignOp::Disallowed);
      break;

    case Opcode::I32GeS:
    case Opcode::I64GeS:
      WriteSignedBinaryExpr(expr.opcode, ">=");
      break;

    case Opcode::I32GeU:
    case Opcode::I64GeU:
    case Opcode::F32Ge:
    case Opcode::F64Ge:
      WriteInfixBinaryExpr(expr.opcode, ">=", AssignOp::Disallowed);
      break;

    case Opcode::I8X16Eq:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i8x16_eq");
      break;

    case Opcode::I8X16GeS:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i8x16_ge");
      break;

    case Opcode::I8X16GeU:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_u8x16_ge");
      break;

    case Opcode::I8X16GtS:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i8x16_gt");
      break;

    case Opcode::I8X16GtU:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_u8x16_gt");
      break;

    case Opcode::I8X16LeS:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i8x16_le");
      break;

    case Opcode::I8X16LeU:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_u8x16_le");
      break;

    case Opcode::I8X16LtS:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i8x16_lt");
      break;

    case Opcode::I8X16LtU:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_u8x16_lt");
      break;

    case Opcode::I8X16Ne:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i8x16_ne");
      break;

    case Opcode::I16X8Eq:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i16x8_eq");
      break;

    case Opcode::I16X8GeS:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i16x8_ge");
      break;

    case Opcode::I16X8GeU:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_u16x8_ge");
      break;

    case Opcode::I16X8GtS:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i16x8_gt");
      break;
    case Opcode::I16X8GtU:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_u16x8_gt");
      break;

    case Opcode::I16X8LeS:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i16x8_le");
      break;

    case Opcode::I16X8LeU:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_u16x8_le");
      break;

    case Opcode::I16X8LtS:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i16x8_lt");
      break;

    case Opcode::I16X8LtU:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_u16x8_lt");
      break;

    case Opcode::I16X8Ne:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i16x8_ne");
      break;

    case Opcode::I32X4Eq:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i32x4_eq");
      break;

    case Opcode::I32X4GeS:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i32x4_ge");
      break;

    case Opcode::I32X4GeU:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_u32x4_ge");
      break;

    case Opcode::I32X4GtS:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i32x4_gt");
      break;

    case Opcode::I32X4GtU:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_u32x4_gt");
      break;

    case Opcode::I32X4LeS:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i32x4_le");
      break;

    case Opcode::I32X4LeU:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_u32x4_le");
      break;

    case Opcode::I32X4LtS:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i32x4_lt");
      break;

    case Opcode::I32X4LtU:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_u32x4_lt");
      break;

    case Opcode::I32X4Ne:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i32x4_ne");
      break;

    case Opcode::I64X2Eq:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i64x2_eq");
      break;

    case Opcode::I64X2GeS:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i64x2_ge");
      break;

    case Opcode::I64X2GtS:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i64x2_gt");
      break;

    case Opcode::I64X2LeS:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i64x2_le");
      break;

    case Opcode::I64X2LtS:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i64x2_lt");
      break;

    case Opcode::I64X2Ne:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i64x2_ne");
      break;

    case Opcode::F32X4Eq:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_f32x4_eq");
      break;

    case Opcode::F32X4Ge:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_f32x4_ge");
      break;

    case Opcode::F32X4Gt:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_f32x4_gt");
      break;

    case Opcode::F32X4Le:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_f32x4_le");
      break;

    case Opcode::F32X4Lt:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_f32x4_lt");
      break;

    case Opcode::F32X4Ne:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_f32x4_ne");
      break;

    case Opcode::F64X2Eq:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_f64x2_eq");
      break;

    case Opcode::F64X2Ge:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_f64x2_ge");
      break;

    case Opcode::F64X2Gt:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_f64x2_gt");
      break;

    case Opcode::F64X2Le:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_f64x2_le");
      break;

    case Opcode::F64X2Lt:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_f64x2_lt");
      break;

    case Opcode::F64X2Ne:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_f64x2_ne");
      break;

    default:
      WABT_UNREACHABLE;
  }
}

void CWriter::Write(const ConvertExpr& expr) {
  switch (expr.opcode) {
    case Opcode::I32Eqz:
    case Opcode::I64Eqz:
      WriteSimpleUnaryExpr(expr.opcode, "!");
      break;

    case Opcode::I64ExtendI32S:
      WriteSimpleUnaryExpr(expr.opcode, "(u64)(s64)(s32)");
      break;

    case Opcode::I64ExtendI32U:
      WriteSimpleUnaryExpr(expr.opcode, "(u64)");
      break;

    case Opcode::I32WrapI64:
      WriteSimpleUnaryExpr(expr.opcode, "(u32)");
      break;

    case Opcode::I32TruncF32S:
      WriteSimpleUnaryExpr(expr.opcode, "I32_TRUNC_S_F32");
      break;

    case Opcode::I64TruncF32S:
      WriteSimpleUnaryExpr(expr.opcode, "I64_TRUNC_S_F32");
      break;

    case Opcode::I32TruncF64S:
      WriteSimpleUnaryExpr(expr.opcode, "I32_TRUNC_S_F64");
      break;

    case Opcode::I64TruncF64S:
      WriteSimpleUnaryExpr(expr.opcode, "I64_TRUNC_S_F64");
      break;

    case Opcode::I32TruncF32U:
      WriteSimpleUnaryExpr(expr.opcode, "I32_TRUNC_U_F32");
      break;

    case Opcode::I64TruncF32U:
      WriteSimpleUnaryExpr(expr.opcode, "I64_TRUNC_U_F32");
      break;

    case Opcode::I32TruncF64U:
      WriteSimpleUnaryExpr(expr.opcode, "I32_TRUNC_U_F64");
      break;

    case Opcode::I64TruncF64U:
      WriteSimpleUnaryExpr(expr.opcode, "I64_TRUNC_U_F64");
      break;

    case Opcode::I32TruncSatF32S:
      WriteSimpleUnaryExpr(expr.opcode, "I32_TRUNC_SAT_S_F32");
      break;

    case Opcode::I64TruncSatF32S:
      WriteSimpleUnaryExpr(expr.opcode, "I64_TRUNC_SAT_S_F32");
      break;

    case Opcode::I32TruncSatF64S:
      WriteSimpleUnaryExpr(expr.opcode, "I32_TRUNC_SAT_S_F64");
      break;

    case Opcode::I64TruncSatF64S:
      WriteSimpleUnaryExpr(expr.opcode, "I64_TRUNC_SAT_S_F64");
      break;

    case Opcode::I32TruncSatF32U:
      WriteSimpleUnaryExpr(expr.opcode, "I32_TRUNC_SAT_U_F32");
      break;

    case Opcode::I64TruncSatF32U:
      WriteSimpleUnaryExpr(expr.opcode, "I64_TRUNC_SAT_U_F32");
      break;

    case Opcode::I32TruncSatF64U:
      WriteSimpleUnaryExpr(expr.opcode, "I32_TRUNC_SAT_U_F64");
      break;

    case Opcode::I64TruncSatF64U:
      WriteSimpleUnaryExpr(expr.opcode, "I64_TRUNC_SAT_U_F64");
      break;

    case Opcode::F32ConvertI32S:
      WriteSimpleUnaryExpr(expr.opcode, "(f32)(s32)");
      break;

    case Opcode::F32ConvertI64S:
      WriteSimpleUnaryExpr(expr.opcode, "(f32)(s64)");
      break;

    case Opcode::F32ConvertI32U:
      WriteSimpleUnaryExpr(expr.opcode, "(f32)");
      break;

    case Opcode::F32DemoteF64:
      WriteSimpleUnaryExpr(expr.opcode, "(f32)wasm_quiet");
      break;

    case Opcode::F32ConvertI64U:
      // TODO(binji): This needs to be handled specially (see
      // wabt_convert_uint64_to_float).
      WriteSimpleUnaryExpr(expr.opcode, "(f32)");
      break;

    case Opcode::F64ConvertI32S:
      WriteSimpleUnaryExpr(expr.opcode, "(f64)(s32)");
      break;

    case Opcode::F64ConvertI64S:
      WriteSimpleUnaryExpr(expr.opcode, "(f64)(s64)");
      break;

    case Opcode::F64ConvertI32U:
      WriteSimpleUnaryExpr(expr.opcode, "(f64)");
      break;

    case Opcode::F64PromoteF32:
      WriteSimpleUnaryExpr(expr.opcode, "(f64)wasm_quietf");
      break;

    case Opcode::F64ConvertI64U:
      // TODO(binji): This needs to be handled specially (see
      // wabt_convert_uint64_to_double).
      WriteSimpleUnaryExpr(expr.opcode, "(f64)");
      break;

    case Opcode::F32ReinterpretI32:
      WriteSimpleUnaryExpr(expr.opcode, "f32_reinterpret_i32");
      break;

    case Opcode::I32ReinterpretF32:
      WriteSimpleUnaryExpr(expr.opcode, "i32_reinterpret_f32");
      break;

    case Opcode::F64ReinterpretI64:
      WriteSimpleUnaryExpr(expr.opcode, "f64_reinterpret_i64");
      break;

    case Opcode::I64ReinterpretF64:
      WriteSimpleUnaryExpr(expr.opcode, "i64_reinterpret_f64");
      break;

    case Opcode::I32X4TruncSatF32X4S:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_i32x4_trunc_sat_f32x4");
      break;

    case Opcode::I32X4TruncSatF32X4U:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_u32x4_trunc_sat_f32x4");
      break;

    case Opcode::I32X4TruncSatF64X2SZero:
      WriteSimpleUnaryExpr(expr.opcode,
                           "simde_wasm_i32x4_trunc_sat_f64x2_zero");
      break;

    case Opcode::I32X4TruncSatF64X2UZero:
      WriteSimpleUnaryExpr(expr.opcode,
                           "simde_wasm_u32x4_trunc_sat_f64x2_zero");
      break;

    case Opcode::F32X4ConvertI32X4S:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_f32x4_convert_i32x4");
      break;

    case Opcode::F32X4ConvertI32X4U:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_f32x4_convert_u32x4");
      break;

    case Opcode::F32X4DemoteF64X2Zero:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_f32x4_demote_f64x2_zero");
      break;

    case Opcode::F64X2ConvertLowI32X4S:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_f64x2_convert_low_i32x4");
      break;

    case Opcode::F64X2ConvertLowI32X4U:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_f64x2_convert_low_u32x4");
      break;

    case Opcode::F64X2PromoteLowF32X4:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_f64x2_promote_low_f32x4");
      break;

    default:
      WABT_UNREACHABLE;
  }
}

void CWriter::WriteFunctionAddress(Index symbol_index, bool is64bit) {
  Index func_index = module_->function_symbols_.at(symbol_index);
  const std::string& func_name = module_->funcs[func_index]->name;
  Write("(u", is64bit ? "64" : "32", ")(",
        ExternalPtr(ModuleFieldType::Func, func_name), ")");
}

void CWriter::WriteDataAddress(Index symbol_index,
                               uint32_t addend,
                               bool is64bit) {
  Write("(u", is64bit ? "64" : "32", ")(&");
  auto data_segment_index_ptr = module_->data_symbols_.find(symbol_index);
  if (data_segment_index_ptr == module_->data_symbols_.end()) {
    const std::string& external_name =
        module_->undefined_data_symbols_.at(symbol_index);
    Write(external_name);
  } else {
    auto [data_segment_index, offset] = data_segment_index_ptr->second;
    const std::string& data_segment_name =
        module_->data_segments[data_segment_index]->name;
    Write("data_segment_data_",
          GlobalName(ModuleFieldType::DataSegment, data_segment_name));
    addend += offset;
  }
  Write(")");
  if (addend > 0) {
    Write(" + ", addend, "u");
  }
}

bool CWriter::TryWriteNoSandboxFunctionAddress(Offset instruction_offset,
                                               bool is64bit) {
  // For i32.const the constant value is encoded as a 5-byte varint32;
  // for i64.const it is a 10-byte varint64.
  int index_byte_length = is64bit ? 10 : 5;
  Offset reloc_offset =
      instruction_offset - index_byte_length - module_->code_section_base_;
  auto& function_reloc_map = module_->function_symbol_by_fptr_load_offset_;
  auto function_reloc = function_reloc_map.find(reloc_offset);
  if (function_reloc == function_reloc_map.end()) {
    return false;
  }
  WriteFunctionAddress(function_reloc->second, is64bit);
  return true;
}

bool CWriter::TryWriteNoSandboxMemoryAddress(Offset instruction_offset,
                                             bool is64bit,
                                             const std::string& prefix) {
  // For i32.{load*,store} the offset is encoded as a 5-byte varuint32;
  // for the i64.* variety it is a 10-byte varuint64.
  int index_byte_length = is64bit ? 10 : 5;
  Offset reloc_offset =
      instruction_offset - index_byte_length - module_->code_section_base_;
  const auto& data_reloc_map =
      module_->data_symbol_and_addend_by_mptr_load_offset_;
  auto data_reloc = data_reloc_map.find(reloc_offset);
  if (data_reloc == data_reloc_map.end()) {
    return false;
  }
  Write(prefix);
  auto [data_symbol_index, addend] = data_reloc->second;
  WriteDataAddress(data_symbol_index, addend, is64bit);
  return true;
}

void CWriter::WriteMemoryAddress(Index stack_index,
                                 const Memory* memory,
                                 size_t loc_offset,
                                 Address offset) {
  if (!options_.features.sandbox_enabled()) {
    Write("(u64)(", StackVar(stack_index), ")");
    if (TryWriteNoSandboxMemoryAddress(loc_offset, memory->page_limits.is_64,
                                       " + ")) {
      return;
    }
  } else {
    Write(ExternalInstancePtr(ModuleFieldType::Memory, memory->name),
          ", (u64)(", StackVar(stack_index), ")");
  }
  if (offset != 0) {
    Write(" + ", offset, "u");
  }
}

void CWriter::Write(const ConstExpr& expr) {
  const Const& const_ = expr.const_;
  bool is64bit = const_.type() == Type::I64;
  PushType(const_.type());
  Write(StackVar(0), " = ");
  if (options_.features.sandbox_enabled() ||
      (!TryWriteNoSandboxFunctionAddress(expr.loc.offset, is64bit) &&
       !TryWriteNoSandboxMemoryAddress(expr.loc.offset, is64bit))) {
    Write(const_);
  }
  Write(";", Newline());
}

void CWriter::Write(const LoadExpr& expr) {
  const char* func = nullptr;
  // clang-format off
  switch (expr.opcode) {
    case Opcode::I32Load: func = "i32_load"; break;
    case Opcode::I64Load: func = "i64_load"; break;
    case Opcode::F32Load: func = "f32_load"; break;
    case Opcode::F64Load: func = "f64_load"; break;
    case Opcode::I32Load8S: func = "i32_load8_s"; break;
    case Opcode::I64Load8S: func = "i64_load8_s"; break;
    case Opcode::I32Load8U: func = "i32_load8_u"; break;
    case Opcode::I64Load8U: func = "i64_load8_u"; break;
    case Opcode::I32Load16S: func = "i32_load16_s"; break;
    case Opcode::I64Load16S: func = "i64_load16_s"; break;
    case Opcode::I32Load16U: func = "i32_load16_u"; break;
    case Opcode::I64Load16U: func = "i64_load16_u"; break;
    case Opcode::I64Load32S: func = "i64_load32_s"; break;
    case Opcode::I64Load32U: func = "i64_load32_u"; break;
    case Opcode::V128Load: func = "v128_load"; break;
    case Opcode::V128Load8X8S: func = "i16x8_load8x8"; break;
    case Opcode::V128Load8X8U: func = "u16x8_load8x8"; break;
    case Opcode::V128Load16X4S: func = "i32x4_load16x4"; break;
    case Opcode::V128Load16X4U: func = "u32x4_load16x4"; break;
    case Opcode::V128Load32X2S: func = "i64x2_load32x2"; break;
    case Opcode::V128Load32X2U: func = "u64x2_load32x2"; break;

    default:
      WABT_UNREACHABLE;
  }
  // clang-format on

  Memory* memory = module_->memories[module_->GetMemoryIndex(expr.memidx)];

  Type result_type = expr.opcode.GetResultType();
  Write(StackVar(0, result_type), " = ", func, "(");
  WriteMemoryAddress(0, memory, expr.loc.offset, expr.offset);
  Write(");", Newline());
  DropTypes(1);
  PushType(result_type);
}

void CWriter::Write(const StoreExpr& expr) {
  const char* func = nullptr;
  // clang-format off
  switch (expr.opcode) {
    case Opcode::I32Store: func = "i32_store"; break;
    case Opcode::I64Store: func = "i64_store"; break;
    case Opcode::F32Store: func = "f32_store"; break;
    case Opcode::F64Store: func = "f64_store"; break;
    case Opcode::I32Store8: func = "i32_store8"; break;
    case Opcode::I64Store8: func = "i64_store8"; break;
    case Opcode::I32Store16: func = "i32_store16"; break;
    case Opcode::I64Store16: func = "i64_store16"; break;
    case Opcode::I64Store32: func = "i64_store32"; break;
    case Opcode::V128Store: func = "v128_store"; break;

    default:
      WABT_UNREACHABLE;
  }
  // clang-format on

  Memory* memory = module_->memories[module_->GetMemoryIndex(expr.memidx)];

  Write(func, "(");
  WriteMemoryAddress(1, memory, expr.loc.offset, expr.offset);
  Write(", ", StackVar(0), ");", Newline());
  DropTypes(2);
}

void CWriter::Write(const UnaryExpr& expr) {
  switch (expr.opcode) {
    case Opcode::I32Clz:
      WriteSimpleUnaryExpr(expr.opcode, "I32_CLZ");
      break;

    case Opcode::I64Clz:
      WriteSimpleUnaryExpr(expr.opcode, "I64_CLZ");
      break;

    case Opcode::I32Ctz:
      WriteSimpleUnaryExpr(expr.opcode, "I32_CTZ");
      break;

    case Opcode::I64Ctz:
      WriteSimpleUnaryExpr(expr.opcode, "I64_CTZ");
      break;

    case Opcode::I32Popcnt:
      WriteSimpleUnaryExpr(expr.opcode, "I32_POPCNT");
      break;

    case Opcode::I64Popcnt:
      WriteSimpleUnaryExpr(expr.opcode, "I64_POPCNT");
      break;

    case Opcode::F32Neg:
    case Opcode::F64Neg:
      WriteSimpleUnaryExpr(expr.opcode, "-");
      break;

    case Opcode::F32Abs:
      WriteSimpleUnaryExpr(expr.opcode, "wasm_fabsf");
      break;

    case Opcode::F64Abs:
      WriteSimpleUnaryExpr(expr.opcode, "wasm_fabs");
      break;

    case Opcode::F32Sqrt:
      WriteSimpleUnaryExpr(expr.opcode, "wasm_sqrtf");
      break;

    case Opcode::F64Sqrt:
      WriteSimpleUnaryExpr(expr.opcode, "wasm_sqrt");
      break;

    case Opcode::F32Ceil:
      WriteSimpleUnaryExpr(expr.opcode, "wasm_ceilf");
      break;

    case Opcode::F64Ceil:
      WriteSimpleUnaryExpr(expr.opcode, "wasm_ceil");
      break;

    case Opcode::F32Floor:
      WriteSimpleUnaryExpr(expr.opcode, "wasm_floorf");
      break;

    case Opcode::F64Floor:
      WriteSimpleUnaryExpr(expr.opcode, "wasm_floor");
      break;

    case Opcode::F32Trunc:
      WriteSimpleUnaryExpr(expr.opcode, "wasm_truncf");
      break;

    case Opcode::F64Trunc:
      WriteSimpleUnaryExpr(expr.opcode, "wasm_trunc");
      break;

    case Opcode::F32Nearest:
      WriteSimpleUnaryExpr(expr.opcode, "wasm_nearbyintf");
      break;

    case Opcode::F64Nearest:
      WriteSimpleUnaryExpr(expr.opcode, "wasm_nearbyint");
      break;

    case Opcode::I32Extend8S:
      WriteSimpleUnaryExpr(expr.opcode, "(u32)(s32)(s8)(u8)");
      break;

    case Opcode::I32Extend16S:
      WriteSimpleUnaryExpr(expr.opcode, "(u32)(s32)(s16)(u16)");
      break;

    case Opcode::I64Extend8S:
      WriteSimpleUnaryExpr(expr.opcode, "(u64)(s64)(s8)(u8)");
      break;

    case Opcode::I64Extend16S:
      WriteSimpleUnaryExpr(expr.opcode, "(u64)(s64)(s16)(u16)");
      break;

    case Opcode::I64Extend32S:
      WriteSimpleUnaryExpr(expr.opcode, "(u64)(s64)(s32)(u32)");
      break;

    case Opcode::I8X16Abs:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_i8x16_abs");
      break;

    case Opcode::I8X16AllTrue:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_i8x16_all_true");
      break;

    case Opcode::I8X16Bitmask:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_i8x16_bitmask");
      break;

    case Opcode::I8X16Neg:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_i8x16_neg");
      break;

    case Opcode::I8X16Popcnt:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_i8x16_popcnt");
      break;

    case Opcode::I8X16Splat:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_i8x16_splat");
      break;

    case Opcode::I16X8Abs:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_i16x8_abs");
      break;

    case Opcode::I16X8AllTrue:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_i16x8_all_true");
      break;

    case Opcode::I16X8Bitmask:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_i16x8_bitmask");
      break;

    case Opcode::I16X8ExtaddPairwiseI8X16S:
      WriteSimpleUnaryExpr(expr.opcode,
                           "simde_wasm_i16x8_extadd_pairwise_i8x16");
      break;

    case Opcode::I16X8ExtaddPairwiseI8X16U:
      WriteSimpleUnaryExpr(expr.opcode,
                           "simde_wasm_u16x8_extadd_pairwise_u8x16");
      break;

    case Opcode::I16X8ExtendHighI8X16S:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_i16x8_extend_high_i8x16");
      break;

    case Opcode::I16X8ExtendHighI8X16U:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_u16x8_extend_high_u8x16");
      break;

    case Opcode::I16X8ExtendLowI8X16S:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_i16x8_extend_low_i8x16");
      break;

    case Opcode::I16X8ExtendLowI8X16U:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_u16x8_extend_low_u8x16");
      break;

    case Opcode::I16X8Neg:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_i16x8_neg");
      break;

    case Opcode::I16X8Splat:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_i16x8_splat");
      break;

    case Opcode::I32X4Abs:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_i32x4_abs");
      break;

    case Opcode::I32X4AllTrue:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_i32x4_all_true");
      break;

    case Opcode::I32X4Bitmask:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_i32x4_bitmask");
      break;

    case Opcode::I32X4ExtaddPairwiseI16X8S:
      WriteSimpleUnaryExpr(expr.opcode,
                           "simde_wasm_i32x4_extadd_pairwise_i16x8");
      break;

    case Opcode::I32X4ExtaddPairwiseI16X8U:
      WriteSimpleUnaryExpr(expr.opcode,
                           "simde_wasm_u32x4_extadd_pairwise_u16x8");
      break;

    case Opcode::I32X4ExtendHighI16X8S:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_i32x4_extend_high_i16x8");
      break;

    case Opcode::I32X4ExtendHighI16X8U:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_u32x4_extend_high_u16x8");
      break;

    case Opcode::I32X4ExtendLowI16X8S:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_i32x4_extend_low_i16x8");
      break;

    case Opcode::I32X4ExtendLowI16X8U:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_u32x4_extend_low_u16x8");
      break;

    case Opcode::I32X4Neg:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_i32x4_neg");
      break;

    case Opcode::I32X4Splat:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_i32x4_splat");
      break;

    case Opcode::I64X2Abs:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_i64x2_abs");
      break;

    case Opcode::I64X2AllTrue:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_i64x2_all_true");
      break;

    case Opcode::I64X2Bitmask:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_i64x2_bitmask");
      break;

    case Opcode::I64X2ExtendHighI32X4S:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_i64x2_extend_high_i32x4");
      break;

    case Opcode::I64X2ExtendHighI32X4U:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_u64x2_extend_high_u32x4");
      break;

    case Opcode::I64X2ExtendLowI32X4S:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_i64x2_extend_low_i32x4");
      break;

    case Opcode::I64X2ExtendLowI32X4U:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_u64x2_extend_low_u32x4");
      break;

    case Opcode::I64X2Neg:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_i64x2_neg");
      break;

    case Opcode::I64X2Splat:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_i64x2_splat");
      break;

    case Opcode::F32X4Abs:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_f32x4_abs");
      break;

    case Opcode::F32X4Ceil:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_f32x4_ceil");
      break;

    case Opcode::F32X4Floor:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_f32x4_floor");
      break;

    case Opcode::F32X4Nearest:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_f32x4_nearest");
      break;

    case Opcode::F32X4Neg:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_f32x4_neg");
      break;

    case Opcode::F32X4Splat:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_f32x4_splat");
      break;

    case Opcode::F32X4Sqrt:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_f32x4_sqrt");
      break;

    case Opcode::F32X4Trunc:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_f32x4_trunc");
      break;

    case Opcode::F64X2Abs:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_f64x2_abs");
      break;

    case Opcode::F64X2Ceil:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_f64x2_ceil");
      break;

    case Opcode::F64X2Floor:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_f64x2_floor");
      break;

    case Opcode::F64X2Nearest:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_f64x2_nearest");
      break;

    case Opcode::F64X2Neg:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_f64x2_neg");
      break;

    case Opcode::F64X2Splat:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_f64x2_splat");
      break;

    case Opcode::F64X2Sqrt:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_f64x2_sqrt");
      break;

    case Opcode::F64X2Trunc:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_f64x2_trunc");
      break;

    case Opcode::V128AnyTrue:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_v128_any_true");
      break;

    case Opcode::V128Not:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_v128_not");
      break;

    default:
      WABT_UNREACHABLE;
  }
}

void CWriter::Write(const TernaryExpr& expr) {
  switch (expr.opcode) {
    case Opcode::V128BitSelect: {
      Type result_type = expr.opcode.GetResultType();
      Write(StackVar(2, result_type), " = ", "simde_wasm_v128_bitselect", "(",
            StackVar(2), ", ", StackVar(1), ", ", StackVar(0), ");", Newline());
      DropTypes(3);
      PushType(result_type);
      break;
    }
    default:
      WABT_UNREACHABLE;
  }
}

void CWriter::Write(const SimdLaneOpExpr& expr) {
  Type result_type = expr.opcode.GetResultType();

  switch (expr.opcode) {
    case Opcode::I8X16ExtractLaneS: {
      Write(StackVar(0, result_type), " = simde_wasm_i8x16_extract_lane(",
            StackVar(0), ", ", expr.val, ");", Newline());
      DropTypes(1);
      break;
    }
    case Opcode::I8X16ExtractLaneU: {
      Write(StackVar(0, result_type), " = simde_wasm_u8x16_extract_lane(",
            StackVar(0), ", ", expr.val, ");", Newline());
      DropTypes(1);
      break;
    }
    case Opcode::I16X8ExtractLaneS: {
      Write(StackVar(0, result_type), " = simde_wasm_i16x8_extract_lane(",
            StackVar(0), ", ", expr.val, ");", Newline());
      DropTypes(1);
      break;
    }
    case Opcode::I16X8ExtractLaneU: {
      Write(StackVar(0, result_type), " = simde_wasm_u16x8_extract_lane(",
            StackVar(0), ", ", expr.val, ");", Newline());
      DropTypes(1);
      break;
    }
    case Opcode::I32X4ExtractLane: {
      Write(StackVar(0, result_type), " = simde_wasm_i32x4_extract_lane(",
            StackVar(0), ", ", expr.val, ");", Newline());
      DropTypes(1);
      break;
    }
    case Opcode::I64X2ExtractLane: {
      Write(StackVar(0, result_type), " = simde_wasm_i64x2_extract_lane(",
            StackVar(0), ", ", expr.val, ");", Newline());
      DropTypes(1);
      break;
    }
    case Opcode::F32X4ExtractLane: {
      Write(StackVar(0, result_type), " = simde_wasm_f32x4_extract_lane(",
            StackVar(0), ", ", expr.val, ");", Newline());
      DropTypes(1);
      break;
    }
    case Opcode::F64X2ExtractLane: {
      Write(StackVar(0, result_type), " = simde_wasm_f64x2_extract_lane(",
            StackVar(0), ", ", expr.val, ");", Newline());
      DropTypes(1);
      break;
    }
    case Opcode::I8X16ReplaceLane: {
      Write(StackVar(1, result_type), " = simde_wasm_i8x16_replace_lane(",
            StackVar(1), ", ", expr.val, ", ", StackVar(0), ");", Newline());
      DropTypes(2);
      break;
    }
    case Opcode::I16X8ReplaceLane: {
      Write(StackVar(1, result_type), " = simde_wasm_i16x8_replace_lane(",
            StackVar(1), ", ", expr.val, ", ", StackVar(0), ");", Newline());
      DropTypes(2);
      break;
    }
    case Opcode::I32X4ReplaceLane: {
      Write(StackVar(1, result_type), " = simde_wasm_i32x4_replace_lane(",
            StackVar(1), ", ", expr.val, ", ", StackVar(0), ");", Newline());
      DropTypes(2);
      break;
    }
    case Opcode::I64X2ReplaceLane: {
      Write(StackVar(1, result_type), " = simde_wasm_i64x2_replace_lane(",
            StackVar(1), ", ", expr.val, ", ", StackVar(0), ");", Newline());
      DropTypes(2);
      break;
    }
    case Opcode::F32X4ReplaceLane: {
      Write(StackVar(1, result_type), " = simde_wasm_f32x4_replace_lane(",
            StackVar(1), ", ", expr.val, ", ", StackVar(0), ");", Newline());
      DropTypes(2);
      break;
    }
    case Opcode::F64X2ReplaceLane: {
      Write(StackVar(1, result_type), " = simde_wasm_f64x2_replace_lane(",
            StackVar(1), ", ", expr.val, ", ", StackVar(0), ");", Newline());
      DropTypes(2);
      break;
    }
    default:
      WABT_UNREACHABLE;
  }

  PushType(result_type);
}

void CWriter::Write(const SimdLoadLaneExpr& expr) {
  const char* func = nullptr;
  // clang-format off
  switch (expr.opcode) {
    case Opcode::V128Load8Lane: func = "v128_load8_lane"; break;
    case Opcode::V128Load16Lane: func = "v128_load16_lane"; break;
    case Opcode::V128Load32Lane: func = "v128_load32_lane"; break;
    case Opcode::V128Load64Lane: func = "v128_load64_lane"; break;
    default:
      WABT_UNREACHABLE;
  }
  // clang-format on
  Memory* memory = module_->memories[module_->GetMemoryIndex(expr.memidx)];
  Type result_type = expr.opcode.GetResultType();
  Write(StackVar(1, result_type), " = ", func, expr.val, "(");
  WriteMemoryAddress(1, memory, expr.loc.offset, expr.offset);
  Write(",", StackVar(0), ");", Newline());

  DropTypes(2);
  PushType(result_type);
}

void CWriter::Write(const SimdStoreLaneExpr& expr) {
  const char* func = nullptr;
  // clang-format off
  switch (expr.opcode) {
    case Opcode::V128Store8Lane: func = "v128_store8_lane"; break;
    case Opcode::V128Store16Lane: func = "v128_store16_lane"; break;
    case Opcode::V128Store32Lane: func = "v128_store32_lane"; break;
    case Opcode::V128Store64Lane: func = "v128_store64_lane"; break;
    default:
      WABT_UNREACHABLE;
  }
  // clang-format on
  Memory* memory = module_->memories[module_->GetMemoryIndex(expr.memidx)];

  Write(func, expr.val, "(");
  WriteMemoryAddress(1, memory, expr.loc.offset, expr.offset);
  Write(", ", StackVar(0), ");", Newline());

  DropTypes(2);
}

void CWriter::Write(const SimdShuffleOpExpr& expr) {
  Type result_type = expr.opcode.GetResultType();
  switch (expr.opcode) {
    case Opcode::I8X16Shuffle: {
      Write(StackVar(1, result_type), " = simde_wasm_i8x16_shuffle(",
            StackVar(1), ", ", StackVar(0), ", ", expr.val.u8(0), ", ",
            expr.val.u8(1), ", ", expr.val.u8(2), ", ", expr.val.u8(3), ", ",
            expr.val.u8(4), ", ", expr.val.u8(5), ", ", expr.val.u8(6), ", ",
            expr.val.u8(7), ", ", expr.val.u8(8), ", ", expr.val.u8(9), ", ",
            expr.val.u8(10), ", ", expr.val.u8(11), ", ", expr.val.u8(12), ", ",
            expr.val.u8(13), ", ", expr.val.u8(14), ", ", expr.val.u8(15), ");",
            Newline());
      DropTypes(2);
      break;
    }
    default:
      WABT_UNREACHABLE;
  }
  PushType(result_type);
}

void CWriter::Write(const LoadSplatExpr& expr) {
  Memory* memory = module_->memories[module_->GetMemoryIndex(expr.memidx)];

  const char* func = nullptr;
  // clang-format off
  switch (expr.opcode) {
    case Opcode::V128Load8Splat: func = "v128_load8_splat"; break;
    case Opcode::V128Load16Splat: func = "v128_load16_splat"; break;
    case Opcode::V128Load32Splat: func = "v128_load32_splat"; break;
    case Opcode::V128Load64Splat: func = "v128_load64_splat"; break;
    default:
      WABT_UNREACHABLE;
  }
  // clang-format on
  Type result_type = expr.opcode.GetResultType();
  Write(StackVar(0, result_type), " = ", func, "(");
  WriteMemoryAddress(0, memory, expr.loc.offset, expr.offset);
  Write(");", Newline());

  DropTypes(1);
  PushType(result_type);
}

void CWriter::Write(const LoadZeroExpr& expr) {
  Memory* memory = module_->memories[module_->GetMemoryIndex(expr.memidx)];

  const char* func = nullptr;
  // clang-format off
  switch (expr.opcode) {
    case Opcode::V128Load32Zero: func = "v128_load32_zero"; break;
    case Opcode::V128Load64Zero: func = "v128_load64_zero"; break;
    default:
      WABT_UNREACHABLE;
  }
  // clang-format on

  Type result_type = expr.opcode.GetResultType();
  Write(StackVar(0, result_type), " = ", func, "(");
  WriteMemoryAddress(0, memory, expr.loc.offset, expr.offset);
  Write(");", Newline());

  DropTypes(1);
  PushType(result_type);
}

void CWriter::ReserveExportNames() {
  for (const Export* export_ : module_->exports) {
    ReserveExportName(export_->name);
  }
}

void CWriter::WriteCHeader() {
  ReserveExportNames();

  stream_ = h_stream_;
  std::string guard = GenerateHeaderGuard();
  Write("/* Automatically generated by wasm2c */", Newline());
  Write("#ifndef ", guard, Newline());
  Write("#define ", guard, Newline(), Newline());
  if (!options_.features.sandbox_enabled()) {
    Write("#define NO_SANDBOX 1", Newline(), Newline());
    Write("#include \"wasm-rt-no-sandbox.h\"", Newline(), Newline());
  } else {
    Write("#include \"wasm-rt.h\"", Newline(), Newline());
  }
  Write(s_header_top);
  Write(Newline());

  // TODO: The module instance should not be written in no-sandbox mode.
  // However, the logic for writing is currently intermingled with code that
  // initializes global_sym_map_, so it must be invoked. Best would be to
  // disentangle data structure initialization from rendering of code.
  Write("#ifndef NO_SANDBOX", Newline());
  WriteModuleInstance();
  Write("#endif", Newline());
  if (options_.features.sandbox_enabled()) {
    WriteInitDecl();
    WriteFreeDecl();
    WriteGetFuncTypeDecl();
  }
  WriteMultivalueTypes();
  if (options_.features.sandbox_enabled()) {
    WriteImports();
  } else {
    WriteImportsNoSandbox();
  }
  WriteImportProperties(CWriterPhase::Declarations);
  WriteExports(CWriterPhase::Declarations);
  Write(Newline());
  Write(s_header_bottom);
  Write(Newline(), "#endif  /* ", guard, " */", Newline());
}

void CWriter::WriteCSource() {
  stream_ = c_stream_;
  Write("/* Automatically generated by wasm2c */", Newline());
  WriteSourceTop();
  WriteFuncTypes();
  WriteTagTypes();
  WriteTags();
  if (!options_.features.sandbox_enabled()) {
    WriteUndefinedSymbolDeclarationsNoSandbox();
  }
  WriteFuncDeclarations();
  WriteGlobalInitializers();
  WriteDataInitializers();
  WriteElemInitializers();
  WriteFuncs();
  WriteExports(CWriterPhase::Definitions);
  WriteInitInstanceImport();
  WriteImportProperties(CWriterPhase::Definitions);
  WriteInit();
  WriteFree();
  WriteGetFuncType();
}

Result CWriter::WriteModule(const Module& module) {
  WABT_USE(options_);
  module_ = &module;
  WriteCHeader();
  WriteCSource();
  return result_;
}

// static
const char* CWriter::GetReferenceTypeName(const Type& type) {
  switch (type) {
    case Type::FuncRef:
      return "funcref";
    case Type::ExternRef:
      return "externref";
    default:
      WABT_UNREACHABLE;
  }
}

// static
const char* CWriter::GetReferenceNullValue(const Type& type) {
  switch (type) {
    case Type::FuncRef:
      return "wasm_rt_funcref_null_value";
    case Type::ExternRef:
      return "wasm_rt_externref_null_value";
    default:
      WABT_UNREACHABLE;
  }
}

}  // end anonymous namespace

Result WriteC(Stream* c_stream,
              Stream* h_stream,
              const char* header_name,
              const Module* module,
              const WriteCOptions& options) {
  CWriter c_writer(c_stream, h_stream, header_name, options);
  return c_writer.WriteModule(*module);
}

}  // namespace wabt
