#ifndef LLD_MACHO_CTX_H
#define LLD_MACHO_CTX_H

#include "ConcatOutputSection.h"
#include "Config.h"
#include "Driver.h"
#include "InputFiles.h"
#include "InputSection.h"
#include "OutputSegment.h"
#include "Relocations.h"
#include "SectionPriorities.h"
#include "SymbolTable.h"
#include "Symbols.h"
#include "SyntheticSections.h"
#include "Target.h"
#include "Writer.h"
#include "lld/Common/CommonLinkerContext.h"

#include "llvm/ADT/CachedHashString.h"

namespace lld::macho {

struct ArchiveFileInfo {
  ArchiveFile *file;
  bool isCommandLineLoad;
};

struct DuplicateSymbolDiag {
  // Pair containing source location and source file
  const std::pair<std::string, std::string> src1;
  const std::pair<std::string, std::string> src2;
  const Symbol *sym;

  DuplicateSymbolDiag(const std::pair<std::string, std::string> src1,
                      const std::pair<std::string, std::string> src2,
                      const Symbol *sym)
      : src1(src1), src2(src2), sym(sym) {}
};

struct UndefinedDiag {
  struct SectionAndOffset {
    const InputSection *isec;
    uint64_t offset;
  };

  std::vector<SectionAndOffset> codeReferences;
  std::vector<std::string> otherReferences;
};

class Ctx : public CommonLinkerContext {
public:
  int idCount = 0;
  uint32_t instanceCount = 0;

  std::unique_ptr<Configuration> config;

  // Output sections are added to output segments in iteration order
  // of ConcatOutputSection, so must have deterministic iteration order.
  llvm::MapVector<NamePair, ConcatOutputSection *> concatOutputSections;

  llvm::DenseMap<Symbol *, ThunkInfo> thunkMap;

  std::unique_ptr<DependencyTracker> depTracker;

  // If --reproduce option is given, all input files are written
  // to this tar archive.
  std::unique_ptr<llvm::TarWriter> tar;

  llvm::SetVector<InputFile *> inputFiles;

  // This cache mostly exists to store system libraries (and .tbds) as they're
  // loaded, rather than the input archives, which are already cached at a
  // higher level, and other files like the filelist that are only read once.
  // Theoretically this caching could be more efficient by hoisting it, but that
  // would require altering many callers to track the state.
  llvm::DenseMap<llvm::CachedHashStringRef, MemoryBufferRef> cachedReads;

  llvm::SmallVector<StringRef> unprocessedLCLinkerOptions;

  std::vector<ConcatInputSection *> inputSections;

  std::vector<OutputSegment *> outputSegments;
  llvm::DenseMap<StringRef, OutputSegment *> nameToOutputSegment;

  const RelocAttrs invalidRelocAttrs{"INVALID", RelocAttrBits::_0};

  PriorityBuilder priorityBuilder{*this};

  std::unique_ptr<SymbolTable> symtab;

  InStruct in;
  std::vector<SyntheticSection *> syntheticSections;

  std::unique_ptr<TargetInfo> target = nullptr;

  OutputSection *firstTLVDataSection = nullptr;

  llvm::DenseMap<llvm::CachedHashStringRef, StringRef> resolvedLibraries;
  llvm::DenseMap<llvm::CachedHashStringRef, StringRef> resolvedFrameworks;
  llvm::DenseMap<StringRef, ArchiveFileInfo> loadedArchives;
  std::vector<StringRef> missingAutolinkWarnings;
  llvm::DenseSet<StringRef> loadedObjectFrameworks;

  // It's not uncommon to have multiple attempts to load a single dylib,
  // especially if it's a commonly re-exported core library.
  llvm::DenseMap<llvm::CachedHashStringRef, DylibFile *> loadedDylibs;

  size_t highestAvailablePriority = std::numeric_limits<size_t>::max();

  llvm::SmallVector<DuplicateSymbolDiag> dupSymDiags;

  llvm::MapVector<const Undefined *, UndefinedDiag> undefs;
};
} // namespace lld::macho

#endif