//===- Writer.h -------------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_ELF_WRITER_H
#define LLD_ELF_WRITER_H

#include "Config.h"
#include "llvm/ADT/StringRef.h"
#include <cstdint>

namespace lld::elf {
class InputFile;
class OutputSection;
void copySectionsIntoPartitions(Ctx &ctx);
template <class ELFT> void createSyntheticSections(Ctx &ctx);
template <class ELFT> void writeResult(Ctx &ctx);

// This describes a program header entry.
// Each contains type, access flags and range of output sections that will be
// placed in it.
struct PhdrEntry {
  PhdrEntry(Ctx &ctx, unsigned type, unsigned flags);

  void add(OutputSection *sec);

  uint64_t p_paddr = 0;
  uint64_t p_vaddr = 0;
  uint64_t p_memsz = 0;
  uint64_t p_filesz = 0;
  uint64_t p_offset = 0;
  uint32_t p_align = 0;
  uint32_t p_type = 0;
  uint32_t p_flags = 0;

  OutputSection *firstSec = nullptr;
  OutputSection *lastSec = nullptr;
  bool hasLMA = false;

  uint64_t lmaOffset = 0;
};

void addReservedSymbols(Ctx &ctx);
bool includeInSymtab(Ctx &ctx, const Symbol &b);

template <class ELFT> uint32_t calcMipsEFlags(Ctx &ctx);

uint8_t getMipsFpAbiFlag(Ctx &ctx, uint8_t oldFlag, uint8_t newFlag,
                         llvm::StringRef fileName);

bool isMipsN32Abi(Ctx &ctx, const InputFile *f);
bool isMicroMips(Ctx &ctx);
bool isMipsR6(Ctx &ctx);

bool hasMemtag(Ctx &ctx);
bool canHaveMemtagGlobals(Ctx &ctx);
} // namespace lld::elf

#endif
