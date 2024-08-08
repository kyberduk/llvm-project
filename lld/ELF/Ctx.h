#ifndef LLD_ELF_CTX_H
#define LLD_ELF_CTX_H

#include "Config.h"
#include "LinkerScript.h"
#include "OutputSections.h"
#include "SymbolTable.h"
#include "Symbols.h"
#include "SyntheticSections.h"
#include "Target.h"

#include "llvm/ADT/DenseSet.h"
#include <mutex>

namespace lld::elf {

struct DuplicateSymbol {
  const Symbol *sym;
  const InputFile *file;
  InputSectionBase *section;
  uint64_t value;
};

// Undefined diagnostics are collected in a vector and emitted once all of
// them are known, so that some postprocessing on the list of undefined
// symbols can happen before lld emits diagnostics.
struct UndefinedDiag {
  Undefined *sym;
  struct Loc {
    InputSectionBase *sec;
    uint64_t offset;
  };
  std::vector<Loc> locs;
  bool isWarning;
};

class Ctx : public CommonLinkerContext {
public:
  ConfigWrapper config;
  LinkerDriver driver{*this};
  SmallVector<std::unique_ptr<MemoryBuffer>> memoryBuffers;
  SmallVector<ELFFileBase *, 0> objectFiles;
  SmallVector<SharedFile *, 0> sharedFiles;
  SmallVector<BinaryFile *, 0> binaryFiles;
  SmallVector<BitcodeFile *, 0> bitcodeFiles;
  SmallVector<BitcodeFile *, 0> lazyBitcodeFiles;
  SmallVector<InputSectionBase *, 0> inputSections;
  SmallVector<EhInputSection *, 0> ehInputSections;
  // Duplicate symbol candidates.
  SmallVector<DuplicateSymbol, 0> duplicates;
  // Symbols in a non-prevailing COMDAT group which should be changed to an
  // Undefined.
  SmallVector<std::pair<Symbol *, unsigned>, 0> nonPrevailingSyms;
  // A tuple of (reference, extractedFile, sym). Used by --why-extract=.
  SmallVector<std::tuple<std::string, const InputFile *, const Symbol &>, 0>
      whyExtractRecords;
  // A mapping from a symbol to an InputFile referencing it backward. Used by
  // --warn-backrefs.
  llvm::DenseMap<const Symbol *,
                 std::pair<const InputFile *, const InputFile *>>
      backwardReferences;
  llvm::SmallSet<llvm::StringRef, 0> auxiliaryFiles;
  // InputFile for linker created symbols with no source location.
  InputFile *internalFile = nullptr;
  // True if SHT_LLVM_SYMPART is used.
  std::atomic<bool> hasSympart{false};
  // True if there are TLS IE relocations. Set DF_STATIC_TLS if -shared.
  std::atomic<bool> hasTlsIe{false};
  // True if we need to reserve two .got entries for local-dynamic TLS model.
  std::atomic<bool> needsTlsLd{false};
  // True if all native vtable symbols have corresponding type info symbols
  // during LTO.
  bool ltoAllVtablesHaveTypeInfos = false;

  // Each symbol assignment and DEFINED(sym) reference is assigned an increasing
  // order. Each DEFINED(sym) evaluation checks whether the reference happens
  // before a possible `sym = expr;`.
  unsigned scriptSymOrderCounter = 1;
  llvm::DenseMap<const Symbol *, unsigned> scriptSymOrder;

  // If --reproduce is specified, all input files are written to this tar
  // archive.
  std::unique_ptr<llvm::TarWriter> tar = nullptr;

  bool isInGroup = false;
  uint32_t nextGroupId = 0;
  unsigned vernauxNum = 0;

  std::mutex relocMutex;

  std::mutex postParseMutex;
  std::mutex inputFileMutex;
  std::mutex decompressMutex;

  // elf::addPPC64SaveRestore
  uint32_t savegpr0[20], restgpr0[21], savegpr1[19], restgpr1[19];

  std::vector<Partition> partitions;

  // The set of TOC entries (.toc + addend) for which we should not apply
  // toc-indirect to toc-relative relaxation. const Symbol * refers to the
  // STT_SECTION symbol associated to the .toc input section.
  llvm::DenseSet<std::pair<const Symbol *, uint64_t>> ppc64noTocRelax;

  llvm::SmallVector<OutputSection *, 0> outputSections;

  llvm::SmallVector<SymbolAux, 0> symAux;

  SymbolTable symtab{*this};

  Partition *mainPart = nullptr;
  InStruct in;

  std::unique_ptr<TargetInfo> target;

  std::unique_ptr<LinkerScript> script;

  llvm::DenseMap<InputSection *, SmallVector<const Defined *, 0>> sectionMap;

  ElfSym es;

  Out out;

  InputSection discarded{*this, nullptr, 0, 0, 0, llvm::ArrayRef<uint8_t>(),
                         ""};

  std::vector<UndefinedDiag> undefs;

  Undefined *dummy = nullptr;

  llvm::raw_fd_ostream openAuxiliaryFile(llvm::StringRef, std::error_code &);
};

// The first two elements of versionDefinitions represent VER_NDX_LOCAL and
// VER_NDX_GLOBAL. This helper returns other elements.
ArrayRef<VersionDefinition> namedVersionDefs(Ctx &ctx);

void errorOrWarn(Ctx &ctx, const Twine &msg);

static inline void internalLinkerError(Ctx &ctx, StringRef loc,
                                       const Twine &msg) {
  errorOrWarn(ctx, loc + "internal linker error: " + msg + "\n" +
                       llvm::getBugReportMsg());
}

template <typename... T> Defined *makeDefined(Ctx &ctx, T &&...args) {

  SymbolUnion *sym;
  {
    std::lock_guard lock(ctx.makeMtx);
    sym = getSpecificAllocSingleton<SymbolUnion>(ctx).Allocate();
  }

  memset(sym, 0, sizeof(Symbol));
  auto &s =
      *new (reinterpret_cast<Defined *>(sym)) Defined(std::forward<T>(args)...);
  return &s;
}
} // namespace lld::elf

#endif