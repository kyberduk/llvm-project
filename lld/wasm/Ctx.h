#ifndef LLD_WASM_CTX_H
#define LLD_WASM_CTX_H

#include "Config.h"
#include "SymbolTable.h"
#include "Symbols.h"
#include "SyntheticSections.h"
#include "lld/Common/CommonLinkerContext.h"

#include "llvm/Support/TarWriter.h"

namespace lld::wasm {
class Ctx : public CommonLinkerContext {
public:
  llvm::SmallVector<ObjFile *, 0> objectFiles;
  llvm::SmallVector<StubFile *, 0> stubFiles;
  llvm::SmallVector<SharedFile *, 0> sharedFiles;
  llvm::SmallVector<BitcodeFile *, 0> bitcodeFiles;
  llvm::SmallVector<InputFunction *, 0> syntheticFunctions;
  llvm::SmallVector<InputGlobal *, 0> syntheticGlobals;
  llvm::SmallVector<InputTable *, 0> syntheticTables;

  // True if we are creating position-independent code.
  bool isPic = false;

  // True if we have an MVP input that uses __indirect_function_table and which
  // requires it to be allocated to table number 0.
  bool legacyFunctionTable = false;

  // Will be set to true if bss data segments should be emitted. In most cases
  // this is not necessary.
  bool emitBssSegments = false;

  // A tuple of (reference, extractedFile, sym). Used by --why-extract=.
  llvm::SmallVector<std::tuple<std::string, const InputFile *, const Symbol &>,
                    0>
      whyExtractRecords;

  // The only instance of Configuration struct.
  Configuration *config;

  // If --reproduce option is given, all input files are written
  // to this tar archive.
  std::unique_ptr<llvm::TarWriter> tar;

  bool doneLTO = false;

  WasmSym ws;

  SymbolTable *symtab = nullptr;

  OutStruct out;
};
} // namespace lld::wasm

#endif