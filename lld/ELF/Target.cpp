//===- Target.cpp ---------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Machine-specific things, such as applying relocations, creation of
// GOT or PLT entries, etc., are handled in this file.
//
// Refer the ELF spec for the single letter variables, S, A or P, used
// in this file.
//
// Some functions defined in this file has "relaxTls" as part of their names.
// They do peephole optimization for TLS variables by rewriting instructions.
// They are not part of the ABI but optional optimization, so you can skip
// them if you are not interested in how TLS variables are optimized.
// See the following paper for the details.
//
//   Ulrich Drepper, ELF Handling For Thread-Local Storage
//   http://www.akkadia.org/drepper/tls.pdf
//
//===----------------------------------------------------------------------===//

#include "Target.h"
#include "Ctx.h"
#include "InputFiles.h"
#include "OutputSections.h"
#include "SymbolTable.h"
#include "Symbols.h"
#include "SyntheticSections.h"
#include "lld/Common/ErrorHandler.h"
#include "llvm/Object/ELF.h"

using namespace llvm;
using namespace llvm::object;
using namespace llvm::ELF;
using namespace lld;
using namespace lld::elf;

std::string lld::toString(Ctx &ctx, RelType type) {
  StringRef s = getELFRelocationTypeName(ctx.config->emachine, type);
  if (s == "Unknown")
    return ("Unknown (" + Twine(type) + ")").str();
  return std::string(s);
}

TargetInfo::TargetInfo(Ctx &c) : ctx(c) { gotEntrySize = ctx.config->wordsize; }

TargetInfo *elf::getTarget(Ctx &ctx) {
  switch (ctx.config->emachine) {
  case EM_386:
  case EM_IAMCU:
    return getX86TargetInfo(ctx);
  case EM_AARCH64:
    return getAArch64TargetInfo(ctx);
  case EM_AMDGPU:
    return getAMDGPUTargetInfo(ctx);
  case EM_ARM:
    return getARMTargetInfo(ctx);
  case EM_AVR:
    return getAVRTargetInfo(ctx);
  case EM_HEXAGON:
    return getHexagonTargetInfo(ctx);
  case EM_LOONGARCH:
    return getLoongArchTargetInfo(ctx);
  case EM_MIPS:
    switch (ctx.config->ekind) {
    case ELF32LEKind:
      return getMipsTargetInfo<ELF32LE>(ctx);
    case ELF32BEKind:
      return getMipsTargetInfo<ELF32BE>(ctx);
    case ELF64LEKind:
      return getMipsTargetInfo<ELF64LE>(ctx);
    case ELF64BEKind:
      return getMipsTargetInfo<ELF64BE>(ctx);
    default:
      llvm_unreachable("unsupported MIPS target");
    }
  case EM_MSP430:
    return getMSP430TargetInfo(ctx);
  case EM_PPC:
    return getPPCTargetInfo(ctx);
  case EM_PPC64:
    return getPPC64TargetInfo(ctx);
  case EM_RISCV:
    return getRISCVTargetInfo(ctx);
  case EM_SPARCV9:
    return getSPARCV9TargetInfo(ctx);
  case EM_S390:
    return getSystemZTargetInfo(ctx);
  case EM_X86_64:
    return getX86_64TargetInfo(ctx);
  }
  llvm_unreachable("unknown target machine");
}

ErrorPlace elf::getErrorPlace(Ctx &ctx, const uint8_t *loc) {
  assert(loc != nullptr);
  for (InputSectionBase *d : ctx.inputSections) {
    auto *isec = dyn_cast<InputSection>(d);
    if (!isec || !isec->getParent() || (isec->type & SHT_NOBITS))
      continue;

    const uint8_t *isecLoc = ctx.out.bufferStart
                                 ? (ctx.out.bufferStart +
                                    isec->getParent()->offset + isec->outSecOff)
                                 : isec->contentMaybeDecompress(ctx).data();
    if (isecLoc == nullptr) {
      assert(isa<SyntheticSection>(isec) && "No data but not synthetic?");
      continue;
    }
    if (isecLoc <= loc && loc < isecLoc + isec->getSize(ctx)) {
      std::string objLoc = isec->getLocation(ctx, loc - isecLoc);
      // Return object file location and source file location.
      // TODO: Refactor getSrcMsg not to take a variable.
      Undefined dummy(ctx.internalFile, "", STB_LOCAL, 0, 0);
      return {isec, objLoc + ": ",
              isec->file ? isec->getSrcMsg(ctx, dummy, loc - isecLoc) : ""};
    }
  }
  return {};
}

TargetInfo::~TargetInfo() {}

int64_t TargetInfo::getImplicitAddend(const uint8_t *buf, RelType type) const {
  internalLinkerError(ctx, getErrorLocation(ctx, buf),
                      "cannot read addend for relocation " +
                          toString(ctx, type));
  return 0;
}

bool TargetInfo::usesOnlyLowPageBits(RelType type) const { return false; }

bool TargetInfo::needsThunk(RelExpr expr, RelType type, const InputFile *file,
                            uint64_t branchAddr, const Symbol &s,
                            int64_t a) const {
  return false;
}

bool TargetInfo::adjustPrologueForCrossSplitStack(uint8_t *loc, uint8_t *end,
                                                  uint8_t stOther) const {
  llvm_unreachable("Target doesn't support split stacks.");
}

bool TargetInfo::inBranchRange(RelType type, uint64_t src, uint64_t dst) const {
  return true;
}

RelExpr TargetInfo::adjustTlsExpr(RelType type, RelExpr expr) const {
  return expr;
}

RelExpr TargetInfo::adjustGotPcExpr(RelType type, int64_t addend,
                                    const uint8_t *data) const {
  return R_GOT_PC;
}

void TargetInfo::relocateAlloc(InputSectionBase &sec, uint8_t *buf) const {
  const unsigned bits = ctx.config->is64 ? 64 : 32;
  uint64_t secAddr = sec.getOutputSection()->addr;
  if (auto *s = dyn_cast<InputSection>(&sec))
    secAddr += s->outSecOff;
  else if (auto *ehIn = dyn_cast<EhInputSection>(&sec))
    secAddr += ehIn->getParent()->outSecOff;
  for (const Relocation &rel : sec.relocs()) {
    uint8_t *loc = buf + rel.offset;
    const uint64_t val = SignExtend64(
        sec.getRelocTargetVA(ctx, sec.file, rel.type, rel.addend,
                             secAddr + rel.offset, *rel.sym, rel.expr),
        bits);
    if (rel.expr != R_RELAX_HINT)
      relocate(loc, rel, val);
  }
}

uint64_t TargetInfo::getImageBase() const {
  // Use --image-base if set. Fall back to the target default if not.
  if (ctx.config->imageBase)
    return *ctx.config->imageBase;
  return ctx.config->isPic ? 0 : defaultImageBase;
}

void elf::checkAlignment(Ctx &ctx, uint8_t *loc, uint64_t v, int n,
                         const Relocation &rel) {
  if ((v & (n - 1)) != 0)
    ctx.error(getErrorLocation(ctx, loc) +
              "improper alignment for relocation " +
              lld::toString(ctx, rel.type) + ": 0x" + llvm::utohexstr(v) +
              " is not aligned to " + Twine(n) + " bytes");
}

uint16_t elf::read16(Ctx &ctx, const void *p) {
  return llvm::support::endian::read16(p, ctx.config->endianness);
}

uint32_t elf::read32(Ctx &ctx, const void *p) {
  return llvm::support::endian::read32(p, ctx.config->endianness);
}

uint64_t elf::read64(Ctx &ctx, const void *p) {
  return llvm::support::endian::read64(p, ctx.config->endianness);
}

void elf::write16(Ctx &ctx, void *p, uint16_t v) {
  llvm::support::endian::write16(p, v, ctx.config->endianness);
}

void elf::write32(Ctx &ctx, void *p, uint32_t v) {
  llvm::support::endian::write32(p, v, ctx.config->endianness);
}

void elf::write64(Ctx &ctx, void *p, uint64_t v) {
  llvm::support::endian::write64(p, v, ctx.config->endianness);
}