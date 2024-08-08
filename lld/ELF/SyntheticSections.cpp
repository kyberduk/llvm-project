//===- SyntheticSections.cpp ----------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains linker-synthesized sections. Currently,
// synthetic sections are created either output sections or input sections,
// but we are rewriting code so that all synthetic sections are created as
// input sections.
//
//===----------------------------------------------------------------------===//

#include "SyntheticSections.h"
#include "Config.h"
#include "Ctx.h"
#include "DWARF.h"
#include "EhFrame.h"
#include "InputFiles.h"
#include "LinkerScript.h"
#include "OutputSections.h"
#include "SymbolTable.h"
#include "Symbols.h"
#include "Target.h"
#include "Thunks.h"
#include "Writer.h"
#include "lld/Common/CommonLinkerContext.h"
#include "lld/Common/DWARF.h"
#include "lld/Common/Strings.h"
#include "lld/Common/Version.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SetOperations.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/DebugInfo/DWARF/DWARFDebugPubTable.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/LEB128.h"
#include "llvm/Support/Parallel.h"
#include "llvm/Support/TimeProfiler.h"
#include <cstdlib>

using namespace llvm;
using namespace llvm::dwarf;
using namespace llvm::ELF;
using namespace llvm::object;
using namespace llvm::support;
using namespace lld;
using namespace lld::elf;

using llvm::support::endian::read32le;
using llvm::support::endian::write32le;
using llvm::support::endian::write64le;

static uint64_t readUint(Ctx &ctx, uint8_t *buf) {
  return ctx.config->is64 ? read64(ctx, buf) : read32(ctx, buf);
}

static void writeUint(Ctx &ctx, uint8_t *buf, uint64_t val) {
  if (ctx.config->is64)
    write64(ctx, buf, val);
  else
    write32(ctx, buf, val);
}

// Returns an LLD version string.
static ArrayRef<uint8_t> getVersion(Ctx &ctx) {
  // Check LLD_VERSION first for ease of testing.
  // You can get consistent output by using the environment variable.
  // This is only for testing.
  StringRef s = getenv("LLD_VERSION");
  if (s.empty())
    s = ctx.saver.save(Twine("Linker: ") + getLLDVersion());

  // +1 to include the terminating '\0'.
  return {(const uint8_t *)s.data(), s.size() + 1};
}

// Creates a .comment section containing LLD version info.
// With this feature, you can identify LLD-generated binaries easily
// by "readelf --string-dump .comment <file>".
// The returned object is a mergeable string section.
MergeInputSection *elf::createCommentSection(Ctx &ctx) {
  auto *sec =
      ctx.make<MergeInputSection>(ctx, SHF_MERGE | SHF_STRINGS, SHT_PROGBITS, 1,
                                  getVersion(ctx), ".comment");
  sec->splitIntoPieces(ctx);
  return sec;
}

// .MIPS.abiflags section.
template <class ELFT>
MipsAbiFlagsSection<ELFT>::MipsAbiFlagsSection(Ctx &ctx,
                                               Elf_Mips_ABIFlags flags)
    : SyntheticSection(ctx, SHF_ALLOC, SHT_MIPS_ABIFLAGS, 8, ".MIPS.abiflags"),
      flags(flags) {
  this->entsize = sizeof(Elf_Mips_ABIFlags);
}

template <class ELFT>
void MipsAbiFlagsSection<ELFT>::writeTo(Ctx &ctx, uint8_t *buf) {
  memcpy(buf, &flags, sizeof(flags));
}

template <class ELFT>
std::unique_ptr<MipsAbiFlagsSection<ELFT>>
MipsAbiFlagsSection<ELFT>::create(Ctx &ctx) {
  Elf_Mips_ABIFlags flags = {};
  bool create = false;

  for (InputSectionBase *sec : ctx.inputSections) {
    if (sec->type != SHT_MIPS_ABIFLAGS)
      continue;
    sec->markDead();
    create = true;

    std::string filename = toString(ctx, sec->file);
    const size_t size = sec->content().size();
    // Older version of BFD (such as the default FreeBSD linker) concatenate
    // .MIPS.abiflags instead of merging. To allow for this case (or potential
    // zero padding) we ignore everything after the first Elf_Mips_ABIFlags
    if (size < sizeof(Elf_Mips_ABIFlags)) {
      ctx.error(filename + ": invalid size of .MIPS.abiflags section: got " +
                Twine(size) + " instead of " +
                Twine(sizeof(Elf_Mips_ABIFlags)));
      return nullptr;
    }
    auto *s =
        reinterpret_cast<const Elf_Mips_ABIFlags *>(sec->content().data());
    if (s->version != 0) {
      ctx.error(filename + ": unexpected .MIPS.abiflags version " +
                Twine(s->version));
      return nullptr;
    }

    // LLD checks ISA compatibility in calcMipsEFlags(). Here we just
    // select the highest number of ISA/Rev/Ext.
    flags.isa_level = std::max(flags.isa_level, s->isa_level);
    flags.isa_rev = std::max(flags.isa_rev, s->isa_rev);
    flags.isa_ext = std::max(flags.isa_ext, s->isa_ext);
    flags.gpr_size = std::max(flags.gpr_size, s->gpr_size);
    flags.cpr1_size = std::max(flags.cpr1_size, s->cpr1_size);
    flags.cpr2_size = std::max(flags.cpr2_size, s->cpr2_size);
    flags.ases |= s->ases;
    flags.flags1 |= s->flags1;
    flags.flags2 |= s->flags2;
    flags.fp_abi =
        elf::getMipsFpAbiFlag(ctx, flags.fp_abi, s->fp_abi, filename);
  };

  if (create)
    return std::make_unique<MipsAbiFlagsSection<ELFT>>(ctx, flags);
  return nullptr;
}

// .MIPS.options section.
template <class ELFT>
MipsOptionsSection<ELFT>::MipsOptionsSection(Ctx &ctx, Elf_Mips_RegInfo reginfo)
    : SyntheticSection(ctx, SHF_ALLOC, SHT_MIPS_OPTIONS, 8, ".MIPS.options"),
      reginfo(reginfo) {
  this->entsize = sizeof(Elf_Mips_Options) + sizeof(Elf_Mips_RegInfo);
}

template <class ELFT>
void MipsOptionsSection<ELFT>::writeTo(Ctx &ctx, uint8_t *buf) {
  auto *options = reinterpret_cast<Elf_Mips_Options *>(buf);
  options->kind = ODK_REGINFO;
  options->size = getSize(ctx);

  if (!ctx.config->relocatable)
    reginfo.ri_gp_value = ctx.in.mipsGot->getGp(ctx);
  memcpy(buf + sizeof(Elf_Mips_Options), &reginfo, sizeof(reginfo));
}

template <class ELFT>
std::unique_ptr<MipsOptionsSection<ELFT>>
MipsOptionsSection<ELFT>::create(Ctx &ctx) {
  // N64 ABI only.
  if (!ELFT::Is64Bits)
    return nullptr;

  SmallVector<InputSectionBase *, 0> sections;
  for (InputSectionBase *sec : ctx.inputSections)
    if (sec->type == SHT_MIPS_OPTIONS)
      sections.push_back(sec);

  if (sections.empty())
    return nullptr;

  Elf_Mips_RegInfo reginfo = {};
  for (InputSectionBase *sec : sections) {
    sec->markDead();

    std::string filename = toString(ctx, sec->file);
    ArrayRef<uint8_t> d = sec->content();

    while (!d.empty()) {
      if (d.size() < sizeof(Elf_Mips_Options)) {
        ctx.error(filename + ": invalid size of .MIPS.options section");
        break;
      }

      auto *opt = reinterpret_cast<const Elf_Mips_Options *>(d.data());
      if (opt->kind == ODK_REGINFO) {
        reginfo.ri_gprmask |= opt->getRegInfo().ri_gprmask;
        sec->getFile<ELFT>()->mipsGp0 = opt->getRegInfo().ri_gp_value;
        break;
      }

      if (!opt->size)
        ctx.fatal(filename + ": zero option descriptor size");
      d = d.slice(opt->size);
    }
  };

  return std::make_unique<MipsOptionsSection<ELFT>>(ctx, reginfo);
}

// MIPS .reginfo section.
template <class ELFT>
MipsReginfoSection<ELFT>::MipsReginfoSection(Ctx &ctx, Elf_Mips_RegInfo reginfo)
    : SyntheticSection(ctx, SHF_ALLOC, SHT_MIPS_REGINFO, 4, ".reginfo"),
      reginfo(reginfo) {
  this->entsize = sizeof(Elf_Mips_RegInfo);
}

template <class ELFT>
void MipsReginfoSection<ELFT>::writeTo(Ctx &ctx, uint8_t *buf) {
  if (!ctx.config->relocatable)
    reginfo.ri_gp_value = ctx.in.mipsGot->getGp(ctx);
  memcpy(buf, &reginfo, sizeof(reginfo));
}

template <class ELFT>
std::unique_ptr<MipsReginfoSection<ELFT>>
MipsReginfoSection<ELFT>::create(Ctx &ctx) {
  // Section should be alive for O32 and N32 ABIs only.
  if (ELFT::Is64Bits)
    return nullptr;

  SmallVector<InputSectionBase *, 0> sections;
  for (InputSectionBase *sec : ctx.inputSections)
    if (sec->type == SHT_MIPS_REGINFO)
      sections.push_back(sec);

  if (sections.empty())
    return nullptr;

  Elf_Mips_RegInfo reginfo = {};
  for (InputSectionBase *sec : sections) {
    sec->markDead();

    if (sec->content().size() != sizeof(Elf_Mips_RegInfo)) {
      ctx.error(toString(ctx, sec->file) +
                ": invalid size of .reginfo section");
      return nullptr;
    }

    auto *r = reinterpret_cast<const Elf_Mips_RegInfo *>(sec->content().data());
    reginfo.ri_gprmask |= r->ri_gprmask;
    sec->getFile<ELFT>()->mipsGp0 = r->ri_gp_value;
  };

  return std::make_unique<MipsReginfoSection<ELFT>>(ctx, reginfo);
}

InputSection *elf::createInterpSection(Ctx &ctx) {
  // StringSaver guarantees that the returned string ends with '\0'.
  StringRef s = ctx.saver.save(ctx.config->dynamicLinker);
  ArrayRef<uint8_t> contents = {(const uint8_t *)s.data(), s.size() + 1};

  return ctx.make<InputSection>(ctx, ctx.internalFile, SHF_ALLOC, SHT_PROGBITS,
                                1, contents, ".interp");
}

Defined *elf::addSyntheticLocal(Ctx &ctx, StringRef name, uint8_t type,
                                uint64_t value, uint64_t size,
                                InputSectionBase &section) {
  Defined *s = makeDefined(ctx, ctx, section.file, name, STB_LOCAL, STV_DEFAULT,
                           type, value, size, &section);
  if (ctx.in.symTab)
    ctx.in.symTab->addSymbol(s);

  if (ctx.config->emachine == EM_ARM && !ctx.config->isLE &&
      ctx.config->armBe8 && (section.flags & SHF_EXECINSTR))
    // Adding Linker generated mapping symbols to the arm specific mapping
    // symbols list.
    addArmSyntheticSectionMappingSymbol(ctx, s);

  return s;
}

static size_t getHashSize(Ctx &ctx) {
  switch (ctx.config->buildId) {
  case BuildIdKind::Fast:
    return 8;
  case BuildIdKind::Md5:
  case BuildIdKind::Uuid:
    return 16;
  case BuildIdKind::Sha1:
    return 20;
  case BuildIdKind::Hexstring:
    return ctx.config->buildIdVector.size();
  default:
    llvm_unreachable("unknown BuildIdKind");
  }
}

// This class represents a linker-synthesized .note.gnu.property section.
//
// In x86 and AArch64, object files may contain feature flags indicating the
// features that they have used. The flags are stored in a .note.gnu.property
// section.
//
// lld reads the sections from input files and merges them by computing AND of
// the flags. The result is written as a new .note.gnu.property section.
//
// If the flag is zero (which indicates that the intersection of the feature
// sets is empty, or some input files didn't have .note.gnu.property sections),
// we don't create this section.
GnuPropertySection::GnuPropertySection(Ctx &ctx)
    : SyntheticSection(ctx, llvm::ELF::SHF_ALLOC, llvm::ELF::SHT_NOTE,
                       ctx.config->wordsize, ".note.gnu.property") {}

void GnuPropertySection::writeTo(Ctx &ctx, uint8_t *buf) {
  uint32_t featureAndType = ctx.config->emachine == EM_AARCH64
                                ? GNU_PROPERTY_AARCH64_FEATURE_1_AND
                                : GNU_PROPERTY_X86_FEATURE_1_AND;

  write32(ctx, buf, 4);                              // Name size
  write32(ctx, buf + 4, ctx.config->is64 ? 16 : 12); // Content size
  write32(ctx, buf + 8, NT_GNU_PROPERTY_TYPE_0);     // Type
  memcpy(buf + 12, "GNU", 4);                        // Name string
  write32(ctx, buf + 16, featureAndType);            // Feature type
  write32(ctx, buf + 20, 4);                         // Feature size
  write32(ctx, buf + 24, ctx.config->andFeatures);   // Feature flags
  if (ctx.config->is64)
    write32(ctx, buf + 28, 0); // Padding
}

size_t GnuPropertySection::getSize(Ctx &ctx) const {
  return ctx.config->is64 ? 32 : 28;
}

BuildIdSection::BuildIdSection(Ctx &ctx)
    : SyntheticSection(ctx, SHF_ALLOC, SHT_NOTE, 4, ".note.gnu.build-id"),
      hashSize(getHashSize(ctx)) {}

void BuildIdSection::writeTo(Ctx &ctx, uint8_t *buf) {
  write32(ctx, buf, 4);                   // Name size
  write32(ctx, buf + 4, hashSize);        // Content size
  write32(ctx, buf + 8, NT_GNU_BUILD_ID); // Type
  memcpy(buf + 12, "GNU", 4);             // Name string
  hashBuf = buf + 16;
}

void BuildIdSection::writeBuildId(ArrayRef<uint8_t> buf) {
  assert(buf.size() == hashSize);
  memcpy(hashBuf, buf.data(), hashSize);
}

BssSection::BssSection(Ctx &ctx, StringRef name, uint64_t size,
                       uint32_t alignment)
    : SyntheticSection(ctx, SHF_ALLOC | SHF_WRITE, SHT_NOBITS, alignment,
                       name) {
  this->bss = true;
  this->size = size;
}

EhFrameSection::EhFrameSection(Ctx &ctx)
    : SyntheticSection(ctx, SHF_ALLOC, SHT_PROGBITS, 1, ".eh_frame") {}

// Search for an existing CIE record or create a new one.
// CIE records from input object files are uniquified by their contents
// and where their relocations point to.
template <class ELFT, class RelTy>
CieRecord *EhFrameSection::addCie(Ctx &ctx, EhSectionPiece &cie,
                                  ArrayRef<RelTy> rels) {
  Symbol *personality = nullptr;
  unsigned firstRelI = cie.firstRelocation;
  if (firstRelI != (unsigned)-1)
    personality = &cie.sec->template getFile<ELFT>()->getRelocTargetSym(
        ctx, rels[firstRelI]);

  // Search for an existing CIE by CIE contents/relocation target pair.
  CieRecord *&rec = cieMap[{cie.data(), personality}];

  // If not found, create a new one.
  if (!rec) {
    rec = ctx.make<CieRecord>();
    rec->cie = &cie;
    cieRecords.push_back(rec);
  }
  return rec;
}

// There is one FDE per function. Returns a non-null pointer to the function
// symbol if the given FDE points to a live function.
template <class ELFT, class RelTy>
Defined *EhFrameSection::isFdeLive(Ctx &ctx, EhSectionPiece &fde,
                                   ArrayRef<RelTy> rels) {
  auto *sec = cast<EhInputSection>(fde.sec);
  unsigned firstRelI = fde.firstRelocation;

  // An FDE should point to some function because FDEs are to describe
  // functions. That's however not always the case due to an issue of
  // ld.gold with -r. ld.gold may discard only functions and leave their
  // corresponding FDEs, which results in creating bad .eh_frame sections.
  // To deal with that, we ignore such FDEs.
  if (firstRelI == (unsigned)-1)
    return nullptr;

  const RelTy &rel = rels[firstRelI];
  Symbol &b = sec->template getFile<ELFT>()->getRelocTargetSym(ctx, rel);

  // FDEs for garbage-collected or merged-by-ICF sections, or sections in
  // another partition, are dead.
  if (auto *d = dyn_cast<Defined>(&b))
    if (!d->folded && d->section && d->section->partition == partition)
      return d;
  return nullptr;
}

// .eh_frame is a sequence of CIE or FDE records. In general, there
// is one CIE record per input object file which is followed by
// a list of FDEs. This function searches an existing CIE or create a new
// one and associates FDEs to the CIE.
template <class ELFT, class RelTy>
void EhFrameSection::addRecords(Ctx &ctx, EhInputSection *sec,
                                ArrayRef<RelTy> rels) {
  offsetToCie.clear();
  for (EhSectionPiece &cie : sec->cies)
    offsetToCie[cie.inputOff] = addCie<ELFT>(ctx, cie, rels);
  for (EhSectionPiece &fde : sec->fdes) {
    uint32_t id = endian::read32<ELFT::TargetEndianness>(fde.data().data() + 4);
    CieRecord *rec = offsetToCie[fde.inputOff + 4 - id];
    if (!rec)
      ctx.fatal(toString(ctx, sec) + ": invalid CIE reference");

    if (!isFdeLive<ELFT>(ctx, fde, rels))
      continue;
    rec->fdes.push_back(&fde);
    numFdes++;
  }
}

template <class ELFT>
void EhFrameSection::addSectionAux(Ctx &ctx, EhInputSection *sec) {
  if (!sec->isLive())
    return;
  const RelsOrRelas<ELFT> rels = sec->template relsOrRelas<ELFT>();
  if (rels.areRelocsRel())
    addRecords<ELFT>(ctx, sec, rels.rels);
  else
    addRecords<ELFT>(ctx, sec, rels.relas);
}

// Used by ICF<ELFT>::handleLSDA(). This function is very similar to
// EhFrameSection::addRecords().
template <class ELFT, class RelTy>
void EhFrameSection::iterateFDEWithLSDAAux(
    Ctx &ctx, EhInputSection &sec, ArrayRef<RelTy> rels,
    DenseSet<size_t> &ciesWithLSDA,
    llvm::function_ref<void(InputSection &)> fn) {
  for (EhSectionPiece &cie : sec.cies)
    if (hasLSDA(ctx, cie))
      ciesWithLSDA.insert(cie.inputOff);
  for (EhSectionPiece &fde : sec.fdes) {
    uint32_t id = endian::read32<ELFT::TargetEndianness>(fde.data().data() + 4);
    if (!ciesWithLSDA.contains(fde.inputOff + 4 - id))
      continue;

    // The CIE has a LSDA argument. Call fn with d's section.
    if (Defined *d = isFdeLive<ELFT>(ctx, fde, rels))
      if (auto *s = dyn_cast_or_null<InputSection>(d->section))
        fn(*s);
  }
}

template <class ELFT>
void EhFrameSection::iterateFDEWithLSDA(
    Ctx &ctx, llvm::function_ref<void(InputSection &)> fn) {
  DenseSet<size_t> ciesWithLSDA;
  for (EhInputSection *sec : sections) {
    ciesWithLSDA.clear();
    const RelsOrRelas<ELFT> rels = sec->template relsOrRelas<ELFT>();
    if (rels.areRelocsRel())
      iterateFDEWithLSDAAux<ELFT>(ctx, *sec, rels.rels, ciesWithLSDA, fn);
    else
      iterateFDEWithLSDAAux<ELFT>(ctx, *sec, rels.relas, ciesWithLSDA, fn);
  }
}

static void writeCieFde(Ctx &ctx, uint8_t *buf, ArrayRef<uint8_t> d) {
  memcpy(buf, d.data(), d.size());
  // Fix the size field. -4 since size does not include the size field itself.
  write32(ctx, buf, d.size() - 4);
}

void EhFrameSection::finalizeContents(Ctx &ctx) {
  assert(!this->size); // Not finalized.

  switch (ctx.config->ekind) {
  case ELFNoneKind:
    llvm_unreachable("invalid ekind");
  case ELF32LEKind:
    for (EhInputSection *sec : sections)
      addSectionAux<ELF32LE>(ctx, sec);
    break;
  case ELF32BEKind:
    for (EhInputSection *sec : sections)
      addSectionAux<ELF32BE>(ctx, sec);
    break;
  case ELF64LEKind:
    for (EhInputSection *sec : sections)
      addSectionAux<ELF64LE>(ctx, sec);
    break;
  case ELF64BEKind:
    for (EhInputSection *sec : sections)
      addSectionAux<ELF64BE>(ctx, sec);
    break;
  }

  size_t off = 0;
  for (CieRecord *rec : cieRecords) {
    rec->cie->outputOff = off;
    off += rec->cie->size;

    for (EhSectionPiece *fde : rec->fdes) {
      fde->outputOff = off;
      off += fde->size;
    }
  }

  // The LSB standard does not allow a .eh_frame section with zero
  // Call Frame Information records. glibc unwind-dw2-fde.c
  // classify_object_over_fdes expects there is a CIE record length 0 as a
  // terminator. Thus we add one unconditionally.
  off += 4;

  this->size = off;
}

// Returns data for .eh_frame_hdr. .eh_frame_hdr is a binary search table
// to get an FDE from an address to which FDE is applied. This function
// returns a list of such pairs.
SmallVector<EhFrameSection::FdeData, 0>
EhFrameSection::getFdeData(Ctx &ctx) const {
  uint8_t *buf = ctx.out.bufferStart + getParent()->offset + outSecOff;
  SmallVector<FdeData, 0> ret;

  uint64_t va = getPartition(ctx).ehFrameHdr->getVA(ctx);
  for (CieRecord *rec : cieRecords) {
    uint8_t enc = getFdeEncoding(ctx, rec->cie);
    for (EhSectionPiece *fde : rec->fdes) {
      uint64_t pc = getFdePc(ctx, buf, fde->outputOff, enc);
      uint64_t fdeVA = getParent()->addr + fde->outputOff;
      if (!isInt<32>(pc - va))
        ctx.fatal(toString(ctx, fde->sec) + ": PC offset is too large: 0x" +
                  Twine::utohexstr(pc - va));
      ret.push_back({uint32_t(pc - va), uint32_t(fdeVA - va)});
    }
  }

  // Sort the FDE list by their PC and uniqueify. Usually there is only
  // one FDE for a PC (i.e. function), but if ICF merges two functions
  // into one, there can be more than one FDEs pointing to the address.
  auto less = [](const FdeData &a, const FdeData &b) {
    return a.pcRel < b.pcRel;
  };
  llvm::stable_sort(ret, less);
  auto eq = [](const FdeData &a, const FdeData &b) {
    return a.pcRel == b.pcRel;
  };
  ret.erase(std::unique(ret.begin(), ret.end(), eq), ret.end());

  return ret;
}

static uint64_t readFdeAddr(Ctx &ctx, uint8_t *buf, int size) {
  switch (size) {
  case DW_EH_PE_udata2:
    return read16(ctx, buf);
  case DW_EH_PE_sdata2:
    return (int16_t)read16(ctx, buf);
  case DW_EH_PE_udata4:
    return read32(ctx, buf);
  case DW_EH_PE_sdata4:
    return (int32_t)read32(ctx, buf);
  case DW_EH_PE_udata8:
  case DW_EH_PE_sdata8:
    return read64(ctx, buf);
  case DW_EH_PE_absptr:
    return readUint(ctx, buf);
  }
  ctx.fatal("unknown FDE size encoding");
}

// Returns the VA to which a given FDE (on a mmap'ed buffer) is applied to.
// We need it to create .eh_frame_hdr section.
uint64_t EhFrameSection::getFdePc(Ctx &ctx, uint8_t *buf, size_t fdeOff,
                                  uint8_t enc) const {
  // The starting address to which this FDE applies is
  // stored at FDE + 8 byte. And this offset is within
  // the .eh_frame section.
  size_t off = fdeOff + 8;
  uint64_t addr = readFdeAddr(ctx, buf + off, enc & 0xf);
  if ((enc & 0x70) == DW_EH_PE_absptr)
    return addr;
  if ((enc & 0x70) == DW_EH_PE_pcrel)
    return addr + getParent()->addr + off + outSecOff;
  ctx.fatal("unknown FDE size relative encoding");
}

void EhFrameSection::writeTo(Ctx &ctx, uint8_t *buf) {
  // Write CIE and FDE records.
  for (CieRecord *rec : cieRecords) {
    size_t cieOffset = rec->cie->outputOff;
    writeCieFde(ctx, buf + cieOffset, rec->cie->data());

    for (EhSectionPiece *fde : rec->fdes) {
      size_t off = fde->outputOff;
      writeCieFde(ctx, buf + off, fde->data());

      // FDE's second word should have the offset to an associated CIE.
      // Write it.
      write32(ctx, buf + off + 4, off + 4 - cieOffset);
    }
  }

  // Apply relocations. .eh_frame section contents are not contiguous
  // in the output buffer, but relocateAlloc() still works because
  // getOffset() takes care of discontiguous section pieces.
  for (EhInputSection *s : sections)
    ctx.target->relocateAlloc(*s, buf);

  if (getPartition(ctx).ehFrameHdr && getPartition(ctx).ehFrameHdr->getParent())
    getPartition(ctx).ehFrameHdr->write(ctx);
}

GotSection::GotSection(Ctx &ctx)
    : SyntheticSection(ctx, SHF_ALLOC | SHF_WRITE, SHT_PROGBITS,
                       ctx.target->gotEntrySize, ".got") {
  numEntries = ctx.target->gotHeaderEntriesNum;
}

void GotSection::addConstant(const Relocation &r) { relocations.push_back(r); }
void GotSection::addEntry(Ctx &ctx, Symbol &sym) {
  assert(sym.auxIdx == ctx.symAux.size() - 1);
  ctx.symAux.back().gotIdx = numEntries++;
}

bool GotSection::addTlsDescEntry(Ctx &ctx, Symbol &sym) {
  assert(sym.auxIdx == ctx.symAux.size() - 1);
  ctx.symAux.back().tlsDescIdx = numEntries;
  numEntries += 2;
  return true;
}

bool GotSection::addDynTlsEntry(Ctx &ctx, Symbol &sym) {
  assert(sym.auxIdx == ctx.symAux.size() - 1);
  ctx.symAux.back().tlsGdIdx = numEntries;
  // Global Dynamic TLS entries take two GOT slots.
  numEntries += 2;
  return true;
}

// Reserves TLS entries for a TLS module ID and a TLS block offset.
// In total it takes two GOT slots.
bool GotSection::addTlsIndex(Ctx &ctx) {
  if (tlsIndexOff != uint32_t(-1))
    return false;
  tlsIndexOff = numEntries * ctx.config->wordsize;
  numEntries += 2;
  return true;
}

uint32_t GotSection::getTlsDescOffset(Ctx &ctx, const Symbol &sym) const {
  return sym.getTlsDescIdx(ctx) * ctx.config->wordsize;
}

uint64_t GotSection::getTlsDescAddr(Ctx &ctx, const Symbol &sym) const {
  return getVA(ctx) + getTlsDescOffset(ctx, sym);
}

uint64_t GotSection::getGlobalDynAddr(Ctx &ctx, const Symbol &b) const {
  return this->getVA(ctx) + b.getTlsGdIdx(ctx) * ctx.config->wordsize;
}

uint64_t GotSection::getGlobalDynOffset(Ctx &ctx, const Symbol &b) const {
  return b.getTlsGdIdx(ctx) * ctx.config->wordsize;
}

void GotSection::finalizeContents(Ctx &ctx) {
  if (ctx.config->emachine == EM_PPC64 &&
      numEntries <= ctx.target->gotHeaderEntriesNum &&
      !ctx.es.globalOffsetTable)
    size = 0;
  else
    size = numEntries * ctx.config->wordsize;
}

bool GotSection::isNeeded(Ctx &ctx) const {
  // Needed if the GOT symbol is used or the number of entries is more than just
  // the header. A GOT with just the header may not be needed.
  return hasGotOffRel || numEntries > ctx.target->gotHeaderEntriesNum;
}

void GotSection::writeTo(Ctx &ctx, uint8_t *buf) {
  // On PPC64 .got may be needed but empty. Skip the write.
  if (size == 0)
    return;
  ctx.target->writeGotHeader(buf);
  ctx.target->relocateAlloc(*this, buf);
}

static uint64_t getMipsPageAddr(uint64_t addr) {
  return (addr + 0x8000) & ~0xffff;
}

static uint64_t getMipsPageCount(uint64_t size) {
  return (size + 0xfffe) / 0xffff + 1;
}

MipsGotSection::MipsGotSection(Ctx &ctx)
    : SyntheticSection(ctx, SHF_ALLOC | SHF_WRITE | SHF_MIPS_GPREL,
                       SHT_PROGBITS, 16, ".got") {}

void MipsGotSection::addEntry(Ctx &ctx, InputFile &file, Symbol &sym,
                              int64_t addend, RelExpr expr) {
  FileGot &g = getGot(file);
  if (expr == R_MIPS_GOT_LOCAL_PAGE) {
    if (const OutputSection *os = sym.getOutputSection())
      g.pagesMap.insert({os, {}});
    else
      g.local16.insert({{nullptr, getMipsPageAddr(sym.getVA(ctx, addend))}, 0});
  } else if (sym.isTls())
    g.tls.insert({&sym, 0});
  else if (sym.isPreemptible && expr == R_ABS)
    g.relocs.insert({&sym, 0});
  else if (sym.isPreemptible)
    g.global.insert({&sym, 0});
  else if (expr == R_MIPS_GOT_OFF32)
    g.local32.insert({{&sym, addend}, 0});
  else
    g.local16.insert({{&sym, addend}, 0});
}

void MipsGotSection::addDynTlsEntry(InputFile &file, Symbol &sym) {
  getGot(file).dynTlsSymbols.insert({&sym, 0});
}

void MipsGotSection::addTlsIndex(InputFile &file) {
  getGot(file).dynTlsSymbols.insert({nullptr, 0});
}

size_t MipsGotSection::FileGot::getEntriesNum() const {
  return getPageEntriesNum() + local16.size() + global.size() + relocs.size() +
         tls.size() + dynTlsSymbols.size() * 2;
}

size_t MipsGotSection::FileGot::getPageEntriesNum() const {
  size_t num = 0;
  for (const std::pair<const OutputSection *, FileGot::PageBlock> &p : pagesMap)
    num += p.second.count;
  return num;
}

size_t MipsGotSection::FileGot::getIndexedEntriesNum() const {
  size_t count = getPageEntriesNum() + local16.size() + global.size();
  // If there are relocation-only entries in the GOT, TLS entries
  // are allocated after them. TLS entries should be addressable
  // by 16-bit index so count both reloc-only and TLS entries.
  if (!tls.empty() || !dynTlsSymbols.empty())
    count += relocs.size() + tls.size() + dynTlsSymbols.size() * 2;
  return count;
}

MipsGotSection::FileGot &MipsGotSection::getGot(InputFile &f) {
  if (f.mipsGotIndex == uint32_t(-1)) {
    gots.emplace_back();
    gots.back().file = &f;
    f.mipsGotIndex = gots.size() - 1;
  }
  return gots[f.mipsGotIndex];
}

uint64_t MipsGotSection::getPageEntryOffset(Ctx &ctx, const InputFile *f,
                                            const Symbol &sym,
                                            int64_t addend) const {
  const FileGot &g = gots[f->mipsGotIndex];
  uint64_t index = 0;
  if (const OutputSection *outSec = sym.getOutputSection()) {
    uint64_t secAddr = getMipsPageAddr(outSec->addr);
    uint64_t symAddr = getMipsPageAddr(sym.getVA(ctx, addend));
    index = g.pagesMap.lookup(outSec).firstIndex + (symAddr - secAddr) / 0xffff;
  } else {
    index =
        g.local16.lookup({nullptr, getMipsPageAddr(sym.getVA(ctx, addend))});
  }
  return index * ctx.config->wordsize;
}

uint64_t MipsGotSection::getSymEntryOffset(Ctx &ctx, const InputFile *f,
                                           const Symbol &s,
                                           int64_t addend) const {
  const FileGot &g = gots[f->mipsGotIndex];
  Symbol *sym = const_cast<Symbol *>(&s);
  if (sym->isTls())
    return g.tls.lookup(sym) * ctx.config->wordsize;
  if (sym->isPreemptible)
    return g.global.lookup(sym) * ctx.config->wordsize;
  return g.local16.lookup({sym, addend}) * ctx.config->wordsize;
}

uint64_t MipsGotSection::getTlsIndexOffset(Ctx &ctx, const InputFile *f) const {
  const FileGot &g = gots[f->mipsGotIndex];
  return g.dynTlsSymbols.lookup(nullptr) * ctx.config->wordsize;
}

uint64_t MipsGotSection::getGlobalDynOffset(Ctx &ctx, const InputFile *f,
                                            const Symbol &s) const {
  const FileGot &g = gots[f->mipsGotIndex];
  Symbol *sym = const_cast<Symbol *>(&s);
  return g.dynTlsSymbols.lookup(sym) * ctx.config->wordsize;
}

const Symbol *MipsGotSection::getFirstGlobalEntry() const {
  if (gots.empty())
    return nullptr;
  const FileGot &primGot = gots.front();
  if (!primGot.global.empty())
    return primGot.global.front().first;
  if (!primGot.relocs.empty())
    return primGot.relocs.front().first;
  return nullptr;
}

unsigned MipsGotSection::getLocalEntriesNum() const {
  if (gots.empty())
    return headerEntriesNum;
  return headerEntriesNum + gots.front().getPageEntriesNum() +
         gots.front().local16.size();
}

bool MipsGotSection::tryMergeGots(Ctx &ctx, FileGot &dst, FileGot &src,
                                  bool isPrimary) {
  FileGot tmp = dst;
  set_union(tmp.pagesMap, src.pagesMap);
  set_union(tmp.local16, src.local16);
  set_union(tmp.global, src.global);
  set_union(tmp.relocs, src.relocs);
  set_union(tmp.tls, src.tls);
  set_union(tmp.dynTlsSymbols, src.dynTlsSymbols);

  size_t count = isPrimary ? headerEntriesNum : 0;
  count += tmp.getIndexedEntriesNum();

  if (count * ctx.config->wordsize > ctx.config->mipsGotSize)
    return false;

  std::swap(tmp, dst);
  return true;
}

void MipsGotSection::finalizeContents(Ctx &ctx) { updateAllocSize(ctx); }

bool MipsGotSection::updateAllocSize(Ctx &ctx) {
  size = headerEntriesNum * ctx.config->wordsize;
  for (const FileGot &g : gots)
    size += g.getEntriesNum() * ctx.config->wordsize;
  return false;
}

void MipsGotSection::build(Ctx &ctx) {
  if (gots.empty())
    return;

  std::vector<FileGot> mergedGots(1);

  // For each GOT move non-preemptible symbols from the `Global`
  // to `Local16` list. Preemptible symbol might become non-preemptible
  // one if, for example, it gets a related copy relocation.
  for (FileGot &got : gots) {
    for (auto &p : got.global)
      if (!p.first->isPreemptible)
        got.local16.insert({{p.first, 0}, 0});
    got.global.remove_if([&](const std::pair<Symbol *, size_t> &p) {
      return !p.first->isPreemptible;
    });
  }

  // For each GOT remove "reloc-only" entry if there is "global"
  // entry for the same symbol. And add local entries which indexed
  // using 32-bit value at the end of 16-bit entries.
  for (FileGot &got : gots) {
    got.relocs.remove_if([&](const std::pair<Symbol *, size_t> &p) {
      return got.global.count(p.first);
    });
    set_union(got.local16, got.local32);
    got.local32.clear();
  }

  // Evaluate number of "reloc-only" entries in the resulting GOT.
  // To do that put all unique "reloc-only" and "global" entries
  // from all GOTs to the future primary GOT.
  FileGot *primGot = &mergedGots.front();
  for (FileGot &got : gots) {
    set_union(primGot->relocs, got.global);
    set_union(primGot->relocs, got.relocs);
    got.relocs.clear();
  }

  // Evaluate number of "page" entries in each GOT.
  for (FileGot &got : gots) {
    for (std::pair<const OutputSection *, FileGot::PageBlock> &p :
         got.pagesMap) {
      const OutputSection *os = p.first;
      uint64_t secSize = 0;
      for (SectionCommand *cmd : os->commands) {
        if (auto *isd = dyn_cast<InputSectionDescription>(cmd))
          for (InputSection *isec : isd->sections) {
            uint64_t off = alignToPowerOf2(secSize, isec->addralign);
            secSize = off + isec->getSize(ctx);
          }
      }
      p.second.count = getMipsPageCount(secSize);
    }
  }

  // Merge GOTs. Try to join as much as possible GOTs but do not exceed
  // maximum GOT size. At first, try to fill the primary GOT because
  // the primary GOT can be accessed in the most effective way. If it
  // is not possible, try to fill the last GOT in the list, and finally
  // create a new GOT if both attempts failed.
  for (FileGot &srcGot : gots) {
    InputFile *file = srcGot.file;
    if (tryMergeGots(ctx, mergedGots.front(), srcGot, true)) {
      file->mipsGotIndex = 0;
    } else {
      // If this is the first time we failed to merge with the primary GOT,
      // MergedGots.back() will also be the primary GOT. We must make sure not
      // to try to merge again with isPrimary=false, as otherwise, if the
      // inputs are just right, we could allow the primary GOT to become 1 or 2
      // words bigger due to ignoring the header size.
      if (mergedGots.size() == 1 ||
          !tryMergeGots(ctx, mergedGots.back(), srcGot, false)) {
        mergedGots.emplace_back();
        std::swap(mergedGots.back(), srcGot);
      }
      file->mipsGotIndex = mergedGots.size() - 1;
    }
  }
  std::swap(gots, mergedGots);

  // Reduce number of "reloc-only" entries in the primary GOT
  // by subtracting "global" entries in the primary GOT.
  primGot = &gots.front();
  primGot->relocs.remove_if([&](const std::pair<Symbol *, size_t> &p) {
    return primGot->global.count(p.first);
  });

  // Calculate indexes for each GOT entry.
  size_t index = headerEntriesNum;
  for (FileGot &got : gots) {
    got.startIndex = &got == primGot ? 0 : index;
    for (std::pair<const OutputSection *, FileGot::PageBlock> &p :
         got.pagesMap) {
      // For each output section referenced by GOT page relocations calculate
      // and save into pagesMap an upper bound of MIPS GOT entries required
      // to store page addresses of local symbols. We assume the worst case -
      // each 64kb page of the output section has at least one GOT relocation
      // against it. And take in account the case when the section intersects
      // page boundaries.
      p.second.firstIndex = index;
      index += p.second.count;
    }
    for (auto &p : got.local16)
      p.second = index++;
    for (auto &p : got.global)
      p.second = index++;
    for (auto &p : got.relocs)
      p.second = index++;
    for (auto &p : got.tls)
      p.second = index++;
    for (auto &p : got.dynTlsSymbols) {
      p.second = index;
      index += 2;
    }
  }

  // Update SymbolAux::gotIdx field to use this
  // value later in the `sortMipsSymbols` function.
  for (auto &p : primGot->global) {
    if (p.first->auxIdx == 0)
      p.first->allocateAux(ctx);
    ctx.symAux.back().gotIdx = p.second;
  }
  for (auto &p : primGot->relocs) {
    if (p.first->auxIdx == 0)
      p.first->allocateAux(ctx);
    ctx.symAux.back().gotIdx = p.second;
  }

  // Create dynamic relocations.
  for (FileGot &got : gots) {
    // Create dynamic relocations for TLS entries.
    for (std::pair<Symbol *, size_t> &p : got.tls) {
      Symbol *s = p.first;
      uint64_t offset = p.second * ctx.config->wordsize;
      // When building a shared library we still need a dynamic relocation
      // for the TP-relative offset as we don't know how much other data will
      // be allocated before us in the static TLS block.
      if (s->isPreemptible || ctx.config->shared)
        ctx.mainPart->relaDyn->addReloc(
            {ctx.target->tlsGotRel, this, offset,
             DynamicReloc::AgainstSymbolWithTargetVA, *s, 0, R_ABS});
    }
    for (std::pair<Symbol *, size_t> &p : got.dynTlsSymbols) {
      Symbol *s = p.first;
      uint64_t offset = p.second * ctx.config->wordsize;
      if (s == nullptr) {
        if (!ctx.config->shared)
          continue;
        ctx.mainPart->relaDyn->addReloc(
            {ctx.target->tlsModuleIndexRel, this, offset});
      } else {
        // When building a shared library we still need a dynamic relocation
        // for the module index. Therefore only checking for
        // S->isPreemptible is not sufficient (this happens e.g. for
        // thread-locals that have been marked as local through a linker script)
        if (!s->isPreemptible && !ctx.config->shared)
          continue;
        ctx.mainPart->relaDyn->addSymbolReloc(
            ctx, ctx.target->tlsModuleIndexRel, *this, offset, *s);
        // However, we can skip writing the TLS offset reloc for non-preemptible
        // symbols since it is known even in shared libraries
        if (!s->isPreemptible)
          continue;
        offset += ctx.config->wordsize;
        ctx.mainPart->relaDyn->addSymbolReloc(ctx, ctx.target->tlsOffsetRel,
                                              *this, offset, *s);
      }
    }

    // Do not create dynamic relocations for non-TLS
    // entries in the primary GOT.
    if (&got == primGot)
      continue;

    // Dynamic relocations for "global" entries.
    for (const std::pair<Symbol *, size_t> &p : got.global) {
      uint64_t offset = p.second * ctx.config->wordsize;
      ctx.mainPart->relaDyn->addSymbolReloc(ctx, ctx.target->relativeRel, *this,
                                            offset, *p.first);
    }
    if (!ctx.config->isPic)
      continue;
    // Dynamic relocations for "local" entries in case of PIC.
    for (const std::pair<const OutputSection *, FileGot::PageBlock> &l :
         got.pagesMap) {
      size_t pageCount = l.second.count;
      for (size_t pi = 0; pi < pageCount; ++pi) {
        uint64_t offset = (l.second.firstIndex + pi) * ctx.config->wordsize;
        ctx.mainPart->relaDyn->addReloc({ctx.target->relativeRel, this, offset,
                                         l.first, int64_t(pi * 0x10000)});
      }
    }
    for (const std::pair<GotEntry, size_t> &p : got.local16) {
      uint64_t offset = p.second * ctx.config->wordsize;
      ctx.mainPart->relaDyn->addReloc({ctx.target->relativeRel, this, offset,
                                       DynamicReloc::AddendOnlyWithTargetVA,
                                       *p.first.first, p.first.second, R_ABS});
    }
  }
}

bool MipsGotSection::isNeeded(Ctx &ctx) const {
  // We add the .got section to the result for dynamic MIPS target because
  // its address and properties are mentioned in the .dynamic section.
  return !ctx.config->relocatable;
}

uint64_t MipsGotSection::getGp(Ctx &ctx, const InputFile *f) const {
  // For files without related GOT or files refer a primary GOT
  // returns "common" _gp value. For secondary GOTs calculate
  // individual _gp values.
  if (!f || f->mipsGotIndex == uint32_t(-1) || f->mipsGotIndex == 0)
    return ctx.es.mipsGp->getVA(ctx, 0);
  return getVA(ctx) + gots[f->mipsGotIndex].startIndex * ctx.config->wordsize +
         0x7ff0;
}

void MipsGotSection::writeTo(Ctx &ctx, uint8_t *buf) {
  // Set the MSB of the second GOT slot. This is not required by any
  // MIPS ABI documentation, though.
  //
  // There is a comment in glibc saying that "The MSB of got[1] of a
  // gnu object is set to identify gnu objects," and in GNU gold it
  // says "the second entry will be used by some runtime loaders".
  // But how this field is being used is unclear.
  //
  // We are not really willing to mimic other linkers behaviors
  // without understanding why they do that, but because all files
  // generated by GNU tools have this special GOT value, and because
  // we've been doing this for years, it is probably a safe bet to
  // keep doing this for now. We really need to revisit this to see
  // if we had to do this.
  writeUint(ctx, buf + ctx.config->wordsize,
            (uint64_t)1 << (ctx.config->wordsize * 8 - 1));
  for (const FileGot &g : gots) {
    auto write = [&](size_t i, const Symbol *s, int64_t a) {
      uint64_t va = a;
      if (s)
        va = s->getVA(ctx, a);
      writeUint(ctx, buf + i * ctx.config->wordsize, va);
    };
    // Write 'page address' entries to the local part of the GOT.
    for (const std::pair<const OutputSection *, FileGot::PageBlock> &l :
         g.pagesMap) {
      size_t pageCount = l.second.count;
      uint64_t firstPageAddr = getMipsPageAddr(l.first->addr);
      for (size_t pi = 0; pi < pageCount; ++pi)
        write(l.second.firstIndex + pi, nullptr, firstPageAddr + pi * 0x10000);
    }
    // Local, global, TLS, reloc-only  entries.
    // If TLS entry has a corresponding dynamic relocations, leave it
    // initialized by zero. Write down adjusted TLS symbol's values otherwise.
    // To calculate the adjustments use offsets for thread-local storage.
    // http://web.archive.org/web/20190324223224/https://www.linux-mips.org/wiki/NPTL
    for (const std::pair<GotEntry, size_t> &p : g.local16)
      write(p.second, p.first.first, p.first.second);
    // Write VA to the primary GOT only. For secondary GOTs that
    // will be done by REL32 dynamic relocations.
    if (&g == &gots.front())
      for (const std::pair<Symbol *, size_t> &p : g.global)
        write(p.second, p.first, 0);
    for (const std::pair<Symbol *, size_t> &p : g.relocs)
      write(p.second, p.first, 0);
    for (const std::pair<Symbol *, size_t> &p : g.tls)
      write(p.second, p.first,
            p.first->isPreemptible || ctx.config->shared ? 0 : -0x7000);
    for (const std::pair<Symbol *, size_t> &p : g.dynTlsSymbols) {
      if (p.first == nullptr && !ctx.config->shared)
        write(p.second, nullptr, 1);
      else if (p.first && !p.first->isPreemptible) {
        // If we are emitting a shared library with relocations we mustn't write
        // anything to the GOT here. When using Elf_Rel relocations the value
        // one will be treated as an addend and will cause crashes at runtime
        if (!ctx.config->shared)
          write(p.second, nullptr, 1);
        write(p.second + 1, p.first, -0x8000);
      }
    }
  }
}

// On PowerPC the .plt section is used to hold the table of function addresses
// instead of the .got.plt, and the type is SHT_NOBITS similar to a .bss
// section. I don't know why we have a BSS style type for the section but it is
// consistent across both 64-bit PowerPC ABIs as well as the 32-bit PowerPC ABI.
GotPltSection::GotPltSection(Ctx &ctx)
    : SyntheticSection(ctx, SHF_ALLOC | SHF_WRITE, SHT_PROGBITS,
                       ctx.config->wordsize, ".got.plt") {
  if (ctx.config->emachine == EM_PPC) {
    name = ".plt";
  } else if (ctx.config->emachine == EM_PPC64) {
    type = SHT_NOBITS;
    name = ".plt";
  }
}

void GotPltSection::addEntry(Ctx &ctx, Symbol &sym) {
  assert(sym.auxIdx == ctx.symAux.size() - 1 &&
         ctx.symAux.back().pltIdx == entries.size());
  entries.push_back(&sym);
}

size_t GotPltSection::getSize(Ctx &ctx) const {
  return (ctx.target->gotPltHeaderEntriesNum + entries.size()) *
         ctx.target->gotEntrySize;
}

void GotPltSection::writeTo(Ctx &ctx, uint8_t *buf) {
  ctx.target->writeGotPltHeader(buf);
  buf += ctx.target->gotPltHeaderEntriesNum * ctx.target->gotEntrySize;
  for (const Symbol *b : entries) {
    ctx.target->writeGotPlt(buf, *b);
    buf += ctx.target->gotEntrySize;
  }
}

bool GotPltSection::isNeeded(Ctx &ctx) const {
  // We need to emit GOTPLT even if it's empty if there's a relocation relative
  // to it.
  return !entries.empty() || hasGotPltOffRel;
}

static StringRef getIgotPltName(Ctx &ctx) {
  // On ARM the IgotPltSection is part of the GotSection.
  if (ctx.config->emachine == EM_ARM)
    return ".got";

  // On PowerPC64 the GotPltSection is renamed to '.plt' so the IgotPltSection
  // needs to be named the same.
  if (ctx.config->emachine == EM_PPC64)
    return ".plt";

  return ".got.plt";
}

// On PowerPC64 the GotPltSection type is SHT_NOBITS so we have to follow suit
// with the IgotPltSection.
IgotPltSection::IgotPltSection(Ctx &ctx)
    : SyntheticSection(ctx, SHF_ALLOC | SHF_WRITE,
                       ctx.config->emachine == EM_PPC64 ? SHT_NOBITS
                                                        : SHT_PROGBITS,
                       ctx.target->gotEntrySize, getIgotPltName(ctx)) {}

void IgotPltSection::addEntry(Ctx &ctx, Symbol &sym) {
  assert(ctx.symAux.back().pltIdx == entries.size());
  entries.push_back(&sym);
}

size_t IgotPltSection::getSize(Ctx &ctx) const {
  return entries.size() * ctx.target->gotEntrySize;
}

void IgotPltSection::writeTo(Ctx &ctx, uint8_t *buf) {
  for (const Symbol *b : entries) {
    ctx.target->writeIgotPlt(buf, *b);
    buf += ctx.target->gotEntrySize;
  }
}

StringTableSection::StringTableSection(Ctx &ctx, StringRef name, bool dynamic)
    : SyntheticSection(ctx, dynamic ? (uint64_t)SHF_ALLOC : 0, SHT_STRTAB, 1,
                       name),
      dynamic(dynamic) {
  // ELF string tables start with a NUL byte.
  strings.push_back("");
  stringMap.try_emplace(CachedHashStringRef(""), 0);
  size = 1;
}

// Adds a string to the string table. If `hashIt` is true we hash and check for
// duplicates. It is optional because the name of global symbols are already
// uniqued and hashing them again has a big cost for a small value: uniquing
// them with some other string that happens to be the same.
unsigned StringTableSection::addString(StringRef s, bool hashIt) {
  if (hashIt) {
    auto r = stringMap.try_emplace(CachedHashStringRef(s), size);
    if (!r.second)
      return r.first->second;
  }
  if (s.empty())
    return 0;
  unsigned ret = this->size;
  this->size = this->size + s.size() + 1;
  strings.push_back(s);
  return ret;
}

void StringTableSection::writeTo(Ctx &ctx, uint8_t *buf) {
  for (StringRef s : strings) {
    memcpy(buf, s.data(), s.size());
    buf[s.size()] = '\0';
    buf += s.size() + 1;
  }
}

// Returns the number of entries in .gnu.version_d: the number of
// non-VER_NDX_LOCAL-non-VER_NDX_GLOBAL definitions, plus 1.
// Note that we don't support vd_cnt > 1 yet.
static unsigned getVerDefNum(Ctx &ctx) {
  return namedVersionDefs(ctx).size() + 1;
}

template <class ELFT>
DynamicSection<ELFT>::DynamicSection(Ctx &ctx)
    : SyntheticSection(ctx, SHF_ALLOC | SHF_WRITE, SHT_DYNAMIC,
                       ctx.config->wordsize, ".dynamic") {
  this->entsize = ELFT::Is64Bits ? 16 : 8;

  // .dynamic section is not writable on MIPS and on Fuchsia OS
  // which passes -z rodynamic.
  // See "Special Section" in Chapter 4 in the following document:
  // ftp://www.linux-mips.org/pub/linux/mips/doc/ABI/mipsabi.pdf
  if (ctx.config->emachine == EM_MIPS || ctx.config->zRodynamic)
    this->flags = SHF_ALLOC;
}

// The output section .rela.dyn may include these synthetic sections:
//
// - part.relaDyn
// - in.relaIplt: this is included if in.relaIplt is named .rela.dyn
// - in.relaPlt: this is included if a linker script places .rela.plt inside
//   .rela.dyn
//
// DT_RELASZ is the total size of the included sections.
static uint64_t addRelaSz(Ctx &ctx, const RelocationBaseSection &relaDyn) {
  size_t size = relaDyn.getSize(ctx);
  if (ctx.in.relaIplt->getParent() == relaDyn.getParent())
    size += ctx.in.relaIplt->getSize(ctx);
  if (ctx.in.relaPlt->getParent() == relaDyn.getParent())
    size += ctx.in.relaPlt->getSize(ctx);
  return size;
}

// A Linker script may assign the RELA relocation sections to the same
// output section. When this occurs we cannot just use the OutputSection
// Size. Moreover the [DT_JMPREL, DT_JMPREL + DT_PLTRELSZ) is permitted to
// overlap with the [DT_RELA, DT_RELA + DT_RELASZ).
static uint64_t addPltRelSz(Ctx &ctx) {
  size_t size = ctx.in.relaPlt->getSize(ctx);
  if (ctx.in.relaIplt->getParent() == ctx.in.relaPlt->getParent() &&
      ctx.in.relaIplt->name == ctx.in.relaPlt->name)
    size += ctx.in.relaIplt->getSize(ctx);
  return size;
}

// Add remaining entries to complete .dynamic contents.
template <class ELFT>
std::vector<std::pair<int32_t, uint64_t>>
DynamicSection<ELFT>::computeContents(Ctx &ctx) {
  elf::Partition &part = getPartition(ctx);
  bool isMain = part.name.empty();
  std::vector<std::pair<int32_t, uint64_t>> entries;

  auto addInt = [&](int32_t tag, uint64_t val) {
    entries.emplace_back(tag, val);
  };
  auto addInSec = [&](int32_t tag, const InputSection &sec) {
    entries.emplace_back(tag, sec.getVA(ctx));
  };

  for (StringRef s : ctx.config->filterList)
    addInt(DT_FILTER, part.dynStrTab->addString(s));
  for (StringRef s : ctx.config->auxiliaryList)
    addInt(DT_AUXILIARY, part.dynStrTab->addString(s));

  if (!ctx.config->rpath.empty())
    addInt(ctx.config->enableNewDtags ? DT_RUNPATH : DT_RPATH,
           part.dynStrTab->addString(ctx.config->rpath));

  for (SharedFile *file : ctx.sharedFiles)
    if (file->isNeeded)
      addInt(DT_NEEDED, part.dynStrTab->addString(file->soName));

  if (isMain) {
    if (!ctx.config->soName.empty())
      addInt(DT_SONAME, part.dynStrTab->addString(ctx.config->soName));
  } else {
    if (!ctx.config->soName.empty())
      addInt(DT_NEEDED, part.dynStrTab->addString(ctx.config->soName));
    addInt(DT_SONAME, part.dynStrTab->addString(part.name));
  }

  // Set DT_FLAGS and DT_FLAGS_1.
  uint32_t dtFlags = 0;
  uint32_t dtFlags1 = 0;
  if (ctx.config->bsymbolic == BsymbolicKind::All)
    dtFlags |= DF_SYMBOLIC;
  if (ctx.config->zGlobal)
    dtFlags1 |= DF_1_GLOBAL;
  if (ctx.config->zInitfirst)
    dtFlags1 |= DF_1_INITFIRST;
  if (ctx.config->zInterpose)
    dtFlags1 |= DF_1_INTERPOSE;
  if (ctx.config->zNodefaultlib)
    dtFlags1 |= DF_1_NODEFLIB;
  if (ctx.config->zNodelete)
    dtFlags1 |= DF_1_NODELETE;
  if (ctx.config->zNodlopen)
    dtFlags1 |= DF_1_NOOPEN;
  if (ctx.config->pie)
    dtFlags1 |= DF_1_PIE;
  if (ctx.config->zNow) {
    dtFlags |= DF_BIND_NOW;
    dtFlags1 |= DF_1_NOW;
  }
  if (ctx.config->zOrigin) {
    dtFlags |= DF_ORIGIN;
    dtFlags1 |= DF_1_ORIGIN;
  }
  if (!ctx.config->zText)
    dtFlags |= DF_TEXTREL;
  if (ctx.hasTlsIe && ctx.config->shared)
    dtFlags |= DF_STATIC_TLS;

  if (dtFlags)
    addInt(DT_FLAGS, dtFlags);
  if (dtFlags1)
    addInt(DT_FLAGS_1, dtFlags1);

  // DT_DEBUG is a pointer to debug information used by debuggers at runtime. We
  // need it for each process, so we don't write it for DSOs. The loader writes
  // the pointer into this entry.
  //
  // DT_DEBUG is the only .dynamic entry that needs to be written to. Some
  // systems (currently only Fuchsia OS) provide other means to give the
  // debugger this information. Such systems may choose make .dynamic read-only.
  // If the target is such a system (used -z rodynamic) don't write DT_DEBUG.
  if (!ctx.config->shared && !ctx.config->relocatable &&
      !ctx.config->zRodynamic)
    addInt(DT_DEBUG, 0);

  if (part.relaDyn->isNeeded(ctx) ||
      (ctx.in.relaIplt->isNeeded(ctx) &&
       part.relaDyn->getParent() == ctx.in.relaIplt->getParent())) {
    addInSec(part.relaDyn->dynamicTag, *part.relaDyn);
    entries.emplace_back(part.relaDyn->sizeDynamicTag,
                         addRelaSz(ctx, *part.relaDyn));

    bool isRela = ctx.config->isRela;
    addInt(isRela ? DT_RELAENT : DT_RELENT,
           isRela ? sizeof(Elf_Rela) : sizeof(Elf_Rel));

    // MIPS dynamic loader does not support RELCOUNT tag.
    // The problem is in the tight relation between dynamic
    // relocations and GOT. So do not emit this tag on MIPS.
    if (ctx.config->emachine != EM_MIPS) {
      size_t numRelativeRels = part.relaDyn->getRelativeRelocCount();
      if (ctx.config->zCombreloc && numRelativeRels)
        addInt(isRela ? DT_RELACOUNT : DT_RELCOUNT, numRelativeRels);
    }
  }
  if (part.relrDyn && part.relrDyn->getParent() &&
      !part.relrDyn->relocs.empty()) {
    addInSec(ctx.config->useAndroidRelrTags ? DT_ANDROID_RELR : DT_RELR,
             *part.relrDyn);
    addInt(ctx.config->useAndroidRelrTags ? DT_ANDROID_RELRSZ : DT_RELRSZ,
           part.relrDyn->getParent()->size);
    addInt(ctx.config->useAndroidRelrTags ? DT_ANDROID_RELRENT : DT_RELRENT,
           sizeof(Elf_Relr));
  }
  // .rel[a].plt section usually consists of two parts, containing plt and
  // iplt relocations. It is possible to have only iplt relocations in the
  // output. In that case relaPlt is empty and have zero offset, the same offset
  // as relaIplt has. And we still want to emit proper dynamic tags for that
  // case, so here we always use relaPlt as marker for the beginning of
  // .rel[a].plt section.
  if (isMain &&
      (ctx.in.relaPlt->isNeeded(ctx) || ctx.in.relaIplt->isNeeded(ctx))) {
    addInSec(DT_JMPREL, *ctx.in.relaPlt);
    entries.emplace_back(DT_PLTRELSZ, addPltRelSz(ctx));
    switch (ctx.config->emachine) {
    case EM_MIPS:
      addInSec(DT_MIPS_PLTGOT, *ctx.in.gotPlt);
      break;
    case EM_S390:
      addInSec(DT_PLTGOT, *ctx.in.got);
      break;
    case EM_SPARCV9:
      addInSec(DT_PLTGOT, *ctx.in.plt);
      break;
    case EM_AARCH64:
      if (llvm::find_if(ctx.in.relaPlt->relocs,
                        [&ctx = ctx](const DynamicReloc &r) {
                          return r.type == ctx.target->pltRel &&
                                 r.sym->stOther & STO_AARCH64_VARIANT_PCS;
                        }) != ctx.in.relaPlt->relocs.end())
        addInt(DT_AARCH64_VARIANT_PCS, 0);
      addInSec(DT_PLTGOT, *ctx.in.gotPlt);
      break;
    case EM_RISCV:
      if (llvm::any_of(ctx.in.relaPlt->relocs,
                       [&ctx = ctx](const DynamicReloc &r) {
                         return r.type == ctx.target->pltRel &&
                                (r.sym->stOther & STO_RISCV_VARIANT_CC);
                       }))
        addInt(DT_RISCV_VARIANT_CC, 0);
      [[fallthrough]];
    default:
      addInSec(DT_PLTGOT, *ctx.in.gotPlt);
      break;
    }
    addInt(DT_PLTREL, ctx.config->isRela ? DT_RELA : DT_REL);
  }

  if (ctx.config->emachine == EM_AARCH64) {
    if (ctx.config->andFeatures & GNU_PROPERTY_AARCH64_FEATURE_1_BTI)
      addInt(DT_AARCH64_BTI_PLT, 0);
    if (ctx.config->zPacPlt)
      addInt(DT_AARCH64_PAC_PLT, 0);

    if (hasMemtag(ctx)) {
      addInt(DT_AARCH64_MEMTAG_MODE,
             ctx.config->androidMemtagMode == NT_MEMTAG_LEVEL_ASYNC);
      addInt(DT_AARCH64_MEMTAG_HEAP, ctx.config->androidMemtagHeap);
      addInt(DT_AARCH64_MEMTAG_STACK, ctx.config->androidMemtagStack);
      if (ctx.mainPart->memtagGlobalDescriptors->isNeeded(ctx)) {
        addInSec(DT_AARCH64_MEMTAG_GLOBALS,
                 *ctx.mainPart->memtagGlobalDescriptors);
        addInt(DT_AARCH64_MEMTAG_GLOBALSSZ,
               ctx.mainPart->memtagGlobalDescriptors->getSize(ctx));
      }
    }
  }

  addInSec(DT_SYMTAB, *part.dynSymTab);
  addInt(DT_SYMENT, sizeof(Elf_Sym));
  addInSec(DT_STRTAB, *part.dynStrTab);
  addInt(DT_STRSZ, part.dynStrTab->getSize(ctx));
  if (!ctx.config->zText)
    addInt(DT_TEXTREL, 0);
  if (part.gnuHashTab && part.gnuHashTab->getParent())
    addInSec(DT_GNU_HASH, *part.gnuHashTab);
  if (part.hashTab && part.hashTab->getParent())
    addInSec(DT_HASH, *part.hashTab);

  if (isMain) {
    if (ctx.out.preinitArray) {
      addInt(DT_PREINIT_ARRAY, ctx.out.preinitArray->addr);
      addInt(DT_PREINIT_ARRAYSZ, ctx.out.preinitArray->size);
    }
    if (ctx.out.initArray) {
      addInt(DT_INIT_ARRAY, ctx.out.initArray->addr);
      addInt(DT_INIT_ARRAYSZ, ctx.out.initArray->size);
    }
    if (ctx.out.finiArray) {
      addInt(DT_FINI_ARRAY, ctx.out.finiArray->addr);
      addInt(DT_FINI_ARRAYSZ, ctx.out.finiArray->size);
    }

    if (Symbol *b = ctx.symtab.find(ctx.config->init))
      if (b->isDefined())
        addInt(DT_INIT, b->getVA(ctx));
    if (Symbol *b = ctx.symtab.find(ctx.config->fini))
      if (b->isDefined())
        addInt(DT_FINI, b->getVA(ctx));
  }

  if (part.verSym && part.verSym->isNeeded(ctx))
    addInSec(DT_VERSYM, *part.verSym);
  if (part.verDef && part.verDef->isLive()) {
    addInSec(DT_VERDEF, *part.verDef);
    addInt(DT_VERDEFNUM, getVerDefNum(ctx));
  }
  if (part.verNeed && part.verNeed->isNeeded(ctx)) {
    addInSec(DT_VERNEED, *part.verNeed);
    unsigned needNum = 0;
    for (SharedFile *f : ctx.sharedFiles)
      if (!f->vernauxs.empty())
        ++needNum;
    addInt(DT_VERNEEDNUM, needNum);
  }

  if (ctx.config->emachine == EM_MIPS) {
    addInt(DT_MIPS_RLD_VERSION, 1);
    addInt(DT_MIPS_FLAGS, RHF_NOTPOT);
    addInt(DT_MIPS_BASE_ADDRESS, ctx.target->getImageBase());
    addInt(DT_MIPS_SYMTABNO, part.dynSymTab->getNumSymbols());
    addInt(DT_MIPS_LOCAL_GOTNO, ctx.in.mipsGot->getLocalEntriesNum());

    if (const Symbol *b = ctx.in.mipsGot->getFirstGlobalEntry())
      addInt(DT_MIPS_GOTSYM, b->dynsymIndex);
    else
      addInt(DT_MIPS_GOTSYM, part.dynSymTab->getNumSymbols());
    addInSec(DT_PLTGOT, *ctx.in.mipsGot);
    if (ctx.in.mipsRldMap) {
      if (!ctx.config->pie)
        addInSec(DT_MIPS_RLD_MAP, *ctx.in.mipsRldMap);
      // Store the offset to the .rld_map section
      // relative to the address of the tag.
      addInt(DT_MIPS_RLD_MAP_REL, ctx.in.mipsRldMap->getVA(ctx) -
                                      (getVA(ctx) + entries.size() * entsize));
    }
  }

  // DT_PPC_GOT indicates to glibc Secure PLT is used. If DT_PPC_GOT is absent,
  // glibc assumes the old-style BSS PLT layout which we don't support.
  if (ctx.config->emachine == EM_PPC)
    addInSec(DT_PPC_GOT, *ctx.in.got);

  // Glink dynamic tag is required by the V2 abi if the plt section isn't empty.
  if (ctx.config->emachine == EM_PPC64 && ctx.in.plt->isNeeded(ctx)) {
    // The Glink tag points to 32 bytes before the first lazy symbol resolution
    // stub, which starts directly after the header.
    addInt(DT_PPC64_GLINK,
           ctx.in.plt->getVA(ctx) + ctx.target->pltHeaderSize - 32);
  }

  if (ctx.config->emachine == EM_PPC64)
    addInt(DT_PPC64_OPT, ctx.target->ppc64DynamicSectionOpt);

  addInt(DT_NULL, 0);
  return entries;
}

template <class ELFT> void DynamicSection<ELFT>::finalizeContents(Ctx &ctx) {
  if (OutputSection *sec = getPartition(ctx).dynStrTab->getParent())
    getParent()->link = sec->sectionIndex;
  this->size = computeContents(ctx).size() * this->entsize;
}

template <class ELFT>
void DynamicSection<ELFT>::writeTo(Ctx &ctx, uint8_t *buf) {
  auto *p = reinterpret_cast<Elf_Dyn *>(buf);

  for (std::pair<int32_t, uint64_t> kv : computeContents(ctx)) {
    p->d_tag = kv.first;
    p->d_un.d_val = kv.second;
    ++p;
  }
}

uint64_t DynamicReloc::getOffset(Ctx &ctx) const {
  return inputSec->getVA(ctx, offsetInSec);
}

int64_t DynamicReloc::computeAddend(Ctx &ctx) const {
  switch (kind) {
  case AddendOnly:
    assert(sym == nullptr);
    return addend;
  case AgainstSymbol:
    assert(sym != nullptr);
    return addend;
  case AddendOnlyWithTargetVA:
  case AgainstSymbolWithTargetVA: {
    uint64_t ca = InputSection::getRelocTargetVA(
        ctx, inputSec->file, type, addend, getOffset(ctx), *sym, expr);
    return ctx.config->is64 ? ca : SignExtend64<32>(ca);
  }
  case MipsMultiGotPage:
    assert(sym == nullptr);
    return getMipsPageAddr(outputSec->addr) + addend;
  }
  llvm_unreachable("Unknown DynamicReloc::Kind enum");
}

uint32_t DynamicReloc::getSymIndex(Ctx &ctx,
                                   SymbolTableBaseSection *symTab) const {
  if (!needsDynSymIndex())
    return 0;

  size_t index = symTab->getSymbolIndex(ctx, sym);
  assert((index != 0 ||
          (type != ctx.target->gotRel && type != ctx.target->pltRel) ||
          !ctx.mainPart->dynSymTab->getParent()) &&
         "GOT or PLT relocation must refer to symbol in dynamic symbol table");
  return index;
}

RelocationBaseSection::RelocationBaseSection(Ctx &ctx, StringRef name,
                                             uint32_t type, int32_t dynamicTag,
                                             int32_t sizeDynamicTag,
                                             bool combreloc,
                                             unsigned concurrency)
    : SyntheticSection(ctx, SHF_ALLOC, type, ctx.config->wordsize, name),
      dynamicTag(dynamicTag), sizeDynamicTag(sizeDynamicTag),
      relocsVec(concurrency), combreloc(combreloc) {}

void RelocationBaseSection::addSymbolReloc(
    Ctx &ctx, RelType dynType, InputSectionBase &isec, uint64_t offsetInSec,
    Symbol &sym, int64_t addend, std::optional<RelType> addendRelType) {
  addReloc(ctx, DynamicReloc::AgainstSymbol, dynType, isec, offsetInSec, sym,
           addend, R_ADDEND,
           addendRelType ? *addendRelType : ctx.target->noneRel);
}

void RelocationBaseSection::addAddendOnlyRelocIfNonPreemptible(
    Ctx &ctx, RelType dynType, GotSection &sec, uint64_t offsetInSec,
    Symbol &sym, RelType addendRelType) {
  // No need to write an addend to the section for preemptible symbols.
  if (sym.isPreemptible)
    addReloc({dynType, &sec, offsetInSec, DynamicReloc::AgainstSymbol, sym, 0,
              R_ABS});
  else
    addReloc(ctx, DynamicReloc::AddendOnlyWithTargetVA, dynType, sec,
             offsetInSec, sym, 0, R_ABS, addendRelType);
}

void RelocationBaseSection::mergeRels() {
  size_t newSize = relocs.size();
  for (const auto &v : relocsVec)
    newSize += v.size();
  relocs.reserve(newSize);
  for (const auto &v : relocsVec)
    llvm::append_range(relocs, v);
  relocsVec.clear();
}

void RelocationBaseSection::partitionRels(Ctx &ctx) {
  if (!combreloc)
    return;
  const RelType relativeRel = ctx.target->relativeRel;
  numRelativeRelocs =
      llvm::partition(relocs, [=](auto &r) { return r.type == relativeRel; }) -
      relocs.begin();
}

void RelocationBaseSection::finalizeContents(Ctx &ctx) {
  SymbolTableBaseSection *symTab = getPartition(ctx).dynSymTab.get();

  // When linking glibc statically, .rel{,a}.plt contains R_*_IRELATIVE
  // relocations due to IFUNC (e.g. strcpy). sh_link will be set to 0 in that
  // case.
  if (symTab && symTab->getParent())
    getParent()->link = symTab->getParent()->sectionIndex;
  else
    getParent()->link = 0;

  if (ctx.in.relaPlt.get() == this && ctx.in.gotPlt->getParent()) {
    getParent()->flags |= ELF::SHF_INFO_LINK;
    getParent()->info = ctx.in.gotPlt->getParent()->sectionIndex;
  }
  if (ctx.in.relaIplt.get() == this && ctx.in.igotPlt->getParent()) {
    getParent()->flags |= ELF::SHF_INFO_LINK;
    getParent()->info = ctx.in.igotPlt->getParent()->sectionIndex;
  }
}

void RelocationBaseSection::addRelocPre(Ctx &ctx, InputSectionBase &sec,
                                        uint64_t offsetInSec, Symbol &sym,
                                        int64_t addend, RelExpr expr,
                                        RelType addendRelType) {
  if (ctx.config->writeAddends && (expr != R_ADDEND || addend != 0))
    sec.addReloc({expr, addendRelType, offsetInSec, addend, &sym});
}

void DynamicReloc::computeRaw(Ctx &ctx, SymbolTableBaseSection *symtab) {
  r_offset = getOffset(ctx);
  r_sym = getSymIndex(ctx, symtab);
  addend = computeAddend(ctx);
  kind = AddendOnly; // Catch errors
}

void RelocationBaseSection::computeRels(Ctx &ctx) {
  SymbolTableBaseSection *symTab = getPartition(ctx).dynSymTab.get();
  parallelForEach(relocs, [symTab, &ctx](DynamicReloc &rel) {
    rel.computeRaw(ctx, symTab);
  });
  // Sort by (!IsRelative,SymIndex,r_offset). DT_REL[A]COUNT requires us to
  // place R_*_RELATIVE first. SymIndex is to improve locality, while r_offset
  // is to make results easier to read.
  if (combreloc) {
    auto nonRelative = relocs.begin() + numRelativeRelocs;
    parallelSort(relocs.begin(), nonRelative,
                 [&](auto &a, auto &b) { return a.r_offset < b.r_offset; });
    // Non-relative relocations are few, so don't bother with parallelSort.
    llvm::sort(nonRelative, relocs.end(), [&](auto &a, auto &b) {
      return std::tie(a.r_sym, a.r_offset) < std::tie(b.r_sym, b.r_offset);
    });
  }
}

template <class ELFT>
RelocationSection<ELFT>::RelocationSection(Ctx &ctx, StringRef name,
                                           bool combreloc, unsigned concurrency)
    : RelocationBaseSection(ctx, name, ctx.config->isRela ? SHT_RELA : SHT_REL,
                            ctx.config->isRela ? DT_RELA : DT_REL,
                            ctx.config->isRela ? DT_RELASZ : DT_RELSZ,
                            combreloc, concurrency) {
  this->entsize = ctx.config->isRela ? sizeof(Elf_Rela) : sizeof(Elf_Rel);
}

template <class ELFT>
void RelocationSection<ELFT>::writeTo(Ctx &ctx, uint8_t *buf) {
  computeRels(ctx);
  for (const DynamicReloc &rel : relocs) {
    auto *p = reinterpret_cast<Elf_Rela *>(buf);
    p->r_offset = rel.r_offset;
    p->setSymbolAndType(rel.r_sym, rel.type, ctx.config->isMips64EL);
    if (ctx.config->isRela)
      p->r_addend = rel.addend;
    buf += ctx.config->isRela ? sizeof(Elf_Rela) : sizeof(Elf_Rel);
  }
}

RelrBaseSection::RelrBaseSection(Ctx &ctx, unsigned concurrency)
    : SyntheticSection(ctx, SHF_ALLOC,
                       ctx.config->useAndroidRelrTags ? SHT_ANDROID_RELR
                                                      : SHT_RELR,
                       ctx.config->wordsize, ".relr.dyn"),
      relocsVec(concurrency) {}

void RelrBaseSection::mergeRels() {
  size_t newSize = relocs.size();
  for (const auto &v : relocsVec)
    newSize += v.size();
  relocs.reserve(newSize);
  for (const auto &v : relocsVec)
    llvm::append_range(relocs, v);
  relocsVec.clear();
}

template <class ELFT>
AndroidPackedRelocationSection<ELFT>::AndroidPackedRelocationSection(
    Ctx &ctx, StringRef name, unsigned concurrency)
    : RelocationBaseSection(
          ctx, name, ctx.config->isRela ? SHT_ANDROID_RELA : SHT_ANDROID_REL,
          ctx.config->isRela ? DT_ANDROID_RELA : DT_ANDROID_REL,
          ctx.config->isRela ? DT_ANDROID_RELASZ : DT_ANDROID_RELSZ,
          /*combreloc=*/false, concurrency) {
  this->entsize = 1;
}

template <class ELFT>
bool AndroidPackedRelocationSection<ELFT>::updateAllocSize(Ctx &ctx) {
  // This function computes the contents of an Android-format packed relocation
  // section.
  //
  // This format compresses relocations by using relocation groups to factor out
  // fields that are common between relocations and storing deltas from previous
  // relocations in SLEB128 format (which has a short representation for small
  // numbers). A good example of a relocation type with common fields is
  // R_*_RELATIVE, which is normally used to represent function pointers in
  // vtables. In the REL format, each relative relocation has the same r_info
  // field, and is only different from other relative relocations in terms of
  // the r_offset field. By sorting relocations by offset, grouping them by
  // r_info and representing each relocation with only the delta from the
  // previous offset, each 8-byte relocation can be compressed to as little as 1
  // byte (or less with run-length encoding). This relocation packer was able to
  // reduce the size of the relocation section in an Android Chromium DSO from
  // 2,911,184 bytes to 174,693 bytes, or 6% of the original size.
  //
  // A relocation section consists of a header containing the literal bytes
  // 'APS2' followed by a sequence of SLEB128-encoded integers. The first two
  // elements are the total number of relocations in the section and an initial
  // r_offset value. The remaining elements define a sequence of relocation
  // groups. Each relocation group starts with a header consisting of the
  // following elements:
  //
  // - the number of relocations in the relocation group
  // - flags for the relocation group
  // - (if RELOCATION_GROUPED_BY_OFFSET_DELTA_FLAG is set) the r_offset delta
  //   for each relocation in the group.
  // - (if RELOCATION_GROUPED_BY_INFO_FLAG is set) the value of the r_info
  //   field for each relocation in the group.
  // - (if RELOCATION_GROUP_HAS_ADDEND_FLAG and
  //   RELOCATION_GROUPED_BY_ADDEND_FLAG are set) the r_addend delta for
  //   each relocation in the group.
  //
  // Following the relocation group header are descriptions of each of the
  // relocations in the group. They consist of the following elements:
  //
  // - (if RELOCATION_GROUPED_BY_OFFSET_DELTA_FLAG is not set) the r_offset
  //   delta for this relocation.
  // - (if RELOCATION_GROUPED_BY_INFO_FLAG is not set) the value of the r_info
  //   field for this relocation.
  // - (if RELOCATION_GROUP_HAS_ADDEND_FLAG is set and
  //   RELOCATION_GROUPED_BY_ADDEND_FLAG is not set) the r_addend delta for
  //   this relocation.

  size_t oldSize = relocData.size();

  relocData = {'A', 'P', 'S', '2'};
  raw_svector_ostream os(relocData);
  auto add = [&](int64_t v) { encodeSLEB128(v, os); };

  // The format header includes the number of relocations and the initial
  // offset (we set this to zero because the first relocation group will
  // perform the initial adjustment).
  add(relocs.size());
  add(0);

  std::vector<Elf_Rela> relatives, nonRelatives;

  for (const DynamicReloc &rel : relocs) {
    Elf_Rela r;
    r.r_offset = rel.getOffset(ctx);
    r.setSymbolAndType(rel.getSymIndex(ctx, getPartition(ctx).dynSymTab.get()),
                       rel.type, false);
    r.r_addend = ctx.config->isRela ? rel.computeAddend(ctx) : 0;

    if (r.getType(ctx.config->isMips64EL) == ctx.target->relativeRel)
      relatives.push_back(r);
    else
      nonRelatives.push_back(r);
  }

  llvm::sort(relatives, [](const Elf_Rel &a, const Elf_Rel &b) {
    return a.r_offset < b.r_offset;
  });

  // Try to find groups of relative relocations which are spaced one word
  // apart from one another. These generally correspond to vtable entries. The
  // format allows these groups to be encoded using a sort of run-length
  // encoding, but each group will cost 7 bytes in addition to the offset from
  // the previous group, so it is only profitable to do this for groups of
  // size 8 or larger.
  std::vector<Elf_Rela> ungroupedRelatives;
  std::vector<std::vector<Elf_Rela>> relativeGroups;
  for (auto i = relatives.begin(), e = relatives.end(); i != e;) {
    std::vector<Elf_Rela> group;
    do {
      group.push_back(*i++);
    } while (i != e && (i - 1)->r_offset + ctx.config->wordsize == i->r_offset);

    if (group.size() < 8)
      ungroupedRelatives.insert(ungroupedRelatives.end(), group.begin(),
                                group.end());
    else
      relativeGroups.emplace_back(std::move(group));
  }

  // For non-relative relocations, we would like to:
  //   1. Have relocations with the same symbol offset to be consecutive, so
  //      that the runtime linker can speed-up symbol lookup by implementing an
  //      1-entry cache.
  //   2. Group relocations by r_info to reduce the size of the relocation
  //      section.
  // Since the symbol offset is the high bits in r_info, sorting by r_info
  // allows us to do both.
  //
  // For Rela, we also want to sort by r_addend when r_info is the same. This
  // enables us to group by r_addend as well.
  llvm::sort(nonRelatives, [](const Elf_Rela &a, const Elf_Rela &b) {
    if (a.r_info != b.r_info)
      return a.r_info < b.r_info;
    if (a.r_addend != b.r_addend)
      return a.r_addend < b.r_addend;
    return a.r_offset < b.r_offset;
  });

  // Group relocations with the same r_info. Note that each group emits a group
  // header and that may make the relocation section larger. It is hard to
  // estimate the size of a group header as the encoded size of that varies
  // based on r_info. However, we can approximate this trade-off by the number
  // of values encoded. Each group header contains 3 values, and each relocation
  // in a group encodes one less value, as compared to when it is not grouped.
  // Therefore, we only group relocations if there are 3 or more of them with
  // the same r_info.
  //
  // For Rela, the addend for most non-relative relocations is zero, and thus we
  // can usually get a smaller relocation section if we group relocations with 0
  // addend as well.
  std::vector<Elf_Rela> ungroupedNonRelatives;
  std::vector<std::vector<Elf_Rela>> nonRelativeGroups;
  for (auto i = nonRelatives.begin(), e = nonRelatives.end(); i != e;) {
    auto j = i + 1;
    while (j != e && i->r_info == j->r_info &&
           (!ctx.config->isRela || i->r_addend == j->r_addend))
      ++j;
    if (j - i < 3 || (ctx.config->isRela && i->r_addend != 0))
      ungroupedNonRelatives.insert(ungroupedNonRelatives.end(), i, j);
    else
      nonRelativeGroups.emplace_back(i, j);
    i = j;
  }

  // Sort ungrouped relocations by offset to minimize the encoded length.
  llvm::sort(ungroupedNonRelatives, [](const Elf_Rela &a, const Elf_Rela &b) {
    return a.r_offset < b.r_offset;
  });

  unsigned hasAddendIfRela =
      ctx.config->isRela ? RELOCATION_GROUP_HAS_ADDEND_FLAG : 0;

  uint64_t offset = 0;
  uint64_t addend = 0;

  // Emit the run-length encoding for the groups of adjacent relative
  // relocations. Each group is represented using two groups in the packed
  // format. The first is used to set the current offset to the start of the
  // group (and also encodes the first relocation), and the second encodes the
  // remaining relocations.
  for (std::vector<Elf_Rela> &g : relativeGroups) {
    // The first relocation in the group.
    add(1);
    add(RELOCATION_GROUPED_BY_OFFSET_DELTA_FLAG |
        RELOCATION_GROUPED_BY_INFO_FLAG | hasAddendIfRela);
    add(g[0].r_offset - offset);
    add(ctx.target->relativeRel);
    if (ctx.config->isRela) {
      add(g[0].r_addend - addend);
      addend = g[0].r_addend;
    }

    // The remaining relocations.
    add(g.size() - 1);
    add(RELOCATION_GROUPED_BY_OFFSET_DELTA_FLAG |
        RELOCATION_GROUPED_BY_INFO_FLAG | hasAddendIfRela);
    add(ctx.config->wordsize);
    add(ctx.target->relativeRel);
    if (ctx.config->isRela) {
      for (const auto &i : llvm::drop_begin(g)) {
        add(i.r_addend - addend);
        addend = i.r_addend;
      }
    }

    offset = g.back().r_offset;
  }

  // Now the ungrouped relatives.
  if (!ungroupedRelatives.empty()) {
    add(ungroupedRelatives.size());
    add(RELOCATION_GROUPED_BY_INFO_FLAG | hasAddendIfRela);
    add(ctx.target->relativeRel);
    for (Elf_Rela &r : ungroupedRelatives) {
      add(r.r_offset - offset);
      offset = r.r_offset;
      if (ctx.config->isRela) {
        add(r.r_addend - addend);
        addend = r.r_addend;
      }
    }
  }

  // Grouped non-relatives.
  for (ArrayRef<Elf_Rela> g : nonRelativeGroups) {
    add(g.size());
    add(RELOCATION_GROUPED_BY_INFO_FLAG);
    add(g[0].r_info);
    for (const Elf_Rela &r : g) {
      add(r.r_offset - offset);
      offset = r.r_offset;
    }
    addend = 0;
  }

  // Finally the ungrouped non-relative relocations.
  if (!ungroupedNonRelatives.empty()) {
    add(ungroupedNonRelatives.size());
    add(hasAddendIfRela);
    for (Elf_Rela &r : ungroupedNonRelatives) {
      add(r.r_offset - offset);
      offset = r.r_offset;
      add(r.r_info);
      if (ctx.config->isRela) {
        add(r.r_addend - addend);
        addend = r.r_addend;
      }
    }
  }

  // Don't allow the section to shrink; otherwise the size of the section can
  // oscillate infinitely.
  if (relocData.size() < oldSize)
    relocData.append(oldSize - relocData.size(), 0);

  // Returns whether the section size changed. We need to keep recomputing both
  // section layout and the contents of this section until the size converges
  // because changing this section's size can affect section layout, which in
  // turn can affect the sizes of the LEB-encoded integers stored in this
  // section.
  return relocData.size() != oldSize;
}

template <class ELFT>
RelrSection<ELFT>::RelrSection(Ctx &ctx, unsigned concurrency)
    : RelrBaseSection(ctx, concurrency) {
  this->entsize = ctx.config->wordsize;
}

template <class ELFT> bool RelrSection<ELFT>::updateAllocSize(Ctx &ctx) {
  // This function computes the contents of an SHT_RELR packed relocation
  // section.
  //
  // Proposal for adding SHT_RELR sections to generic-abi is here:
  //   https://groups.google.com/forum/#!topic/generic-abi/bX460iggiKg
  //
  // The encoded sequence of Elf64_Relr entries in a SHT_RELR section looks
  // like [ AAAAAAAA BBBBBBB1 BBBBBBB1 ... AAAAAAAA BBBBBB1 ... ]
  //
  // i.e. start with an address, followed by any number of bitmaps. The address
  // entry encodes 1 relocation. The subsequent bitmap entries encode up to 63
  // relocations each, at subsequent offsets following the last address entry.
  //
  // The bitmap entries must have 1 in the least significant bit. The assumption
  // here is that an address cannot have 1 in lsb. Odd addresses are not
  // supported.
  //
  // Excluding the least significant bit in the bitmap, each non-zero bit in
  // the bitmap represents a relocation to be applied to a corresponding machine
  // word that follows the base address word. The second least significant bit
  // represents the machine word immediately following the initial address, and
  // each bit that follows represents the next word, in linear order. As such,
  // a single bitmap can encode up to 31 relocations in a 32-bit object, and
  // 63 relocations in a 64-bit object.
  //
  // This encoding has a couple of interesting properties:
  // 1. Looking at any entry, it is clear whether it's an address or a bitmap:
  //    even means address, odd means bitmap.
  // 2. Just a simple list of addresses is a valid encoding.

  size_t oldSize = relrRelocs.size();
  relrRelocs.clear();

  // Same as Config->Wordsize but faster because this is a compile-time
  // constant.
  const size_t wordsize = sizeof(typename ELFT::uint);

  // Number of bits to use for the relocation offsets bitmap.
  // Must be either 63 or 31.
  const size_t nBits = wordsize * 8 - 1;

  // Get offsets for all relative relocations and sort them.
  std::unique_ptr<uint64_t[]> offsets(new uint64_t[relocs.size()]);
  for (auto [i, r] : llvm::enumerate(relocs))
    offsets[i] = r.getOffset(ctx);
  llvm::sort(offsets.get(), offsets.get() + relocs.size());

  // For each leading relocation, find following ones that can be folded
  // as a bitmap and fold them.
  for (size_t i = 0, e = relocs.size(); i != e;) {
    // Add a leading relocation.
    relrRelocs.push_back(Elf_Relr(offsets[i]));
    uint64_t base = offsets[i] + wordsize;
    ++i;

    // Find foldable relocations to construct bitmaps.
    for (;;) {
      uint64_t bitmap = 0;
      for (; i != e; ++i) {
        uint64_t d = offsets[i] - base;
        if (d >= nBits * wordsize || d % wordsize)
          break;
        bitmap |= uint64_t(1) << (d / wordsize);
      }
      if (!bitmap)
        break;
      relrRelocs.push_back(Elf_Relr((bitmap << 1) | 1));
      base += nBits * wordsize;
    }
  }

  // Don't allow the section to shrink; otherwise the size of the section can
  // oscillate infinitely. Trailing 1s do not decode to more relocations.
  if (relrRelocs.size() < oldSize) {
    ctx.log(".relr.dyn needs " + Twine(oldSize - relrRelocs.size()) +
            " padding word(s)");
    relrRelocs.resize(oldSize, Elf_Relr(1));
  }

  return relrRelocs.size() != oldSize;
}

SymbolTableBaseSection::SymbolTableBaseSection(Ctx &ctx,
                                               StringTableSection &strTabSec)
    : SyntheticSection(ctx, strTabSec.isDynamic() ? (uint64_t)SHF_ALLOC : 0,
                       strTabSec.isDynamic() ? SHT_DYNSYM : SHT_SYMTAB,
                       ctx.config->wordsize,
                       strTabSec.isDynamic() ? ".dynsym" : ".symtab"),
      strTabSec(strTabSec) {}

// Orders symbols according to their positions in the GOT,
// in compliance with MIPS ABI rules.
// See "Global Offset Table" in Chapter 5 in the following document
// for detailed description:
// ftp://www.linux-mips.org/pub/linux/mips/doc/ABI/mipsabi.pdf
static bool sortMipsSymbols(Ctx &ctx, const SymbolTableEntry &l,
                            const SymbolTableEntry &r) {
  // Sort entries related to non-local preemptible symbols by GOT indexes.
  // All other entries go to the beginning of a dynsym in arbitrary order.
  if (l.sym->isInGot(ctx) && r.sym->isInGot(ctx))
    return l.sym->getGotIdx(ctx) < r.sym->getGotIdx(ctx);
  if (!l.sym->isInGot(ctx) && !r.sym->isInGot(ctx))
    return false;
  return !l.sym->isInGot(ctx);
}

void SymbolTableBaseSection::finalizeContents(Ctx &ctx) {
  if (OutputSection *sec = strTabSec.getParent())
    getParent()->link = sec->sectionIndex;

  if (this->type != SHT_DYNSYM) {
    sortSymTabSymbols();
    return;
  }

  // If it is a .dynsym, there should be no local symbols, but we need
  // to do a few things for the dynamic linker.

  // Section's Info field has the index of the first non-local symbol.
  // Because the first symbol entry is a null entry, 1 is the first.
  getParent()->info = 1;

  if (getPartition(ctx).gnuHashTab) {
    // NB: It also sorts Symbols to meet the GNU hash table requirements.
    getPartition(ctx).gnuHashTab->addSymbols(symbols);
  } else if (ctx.config->emachine == EM_MIPS) {
    llvm::stable_sort(symbols, [&ctx = ctx](const SymbolTableEntry &l,
                                            const SymbolTableEntry &r) {
      return sortMipsSymbols(ctx, l, r);
    });
  }

  // Only the main partition's dynsym indexes are stored in the symbols
  // themselves. All other partitions use a lookup table.
  if (this == ctx.mainPart->dynSymTab.get()) {
    size_t i = 0;
    for (const SymbolTableEntry &s : symbols)
      s.sym->dynsymIndex = ++i;
  }
}

// The ELF spec requires that all local symbols precede global symbols, so we
// sort symbol entries in this function. (For .dynsym, we don't do that because
// symbols for dynamic linking are inherently all globals.)
//
// Aside from above, we put local symbols in groups starting with the STT_FILE
// symbol. That is convenient for purpose of identifying where are local symbols
// coming from.
void SymbolTableBaseSection::sortSymTabSymbols() {
  // Move all local symbols before global symbols.
  auto e = std::stable_partition(
      symbols.begin(), symbols.end(),
      [](const SymbolTableEntry &s) { return s.sym->isLocal(); });
  size_t numLocals = e - symbols.begin();
  getParent()->info = numLocals + 1;

  // We want to group the local symbols by file. For that we rebuild the local
  // part of the symbols vector. We do not need to care about the STT_FILE
  // symbols, they are already naturally placed first in each group. That
  // happens because STT_FILE is always the first symbol in the object and hence
  // precede all other local symbols we add for a file.
  MapVector<InputFile *, SmallVector<SymbolTableEntry, 0>> arr;
  for (const SymbolTableEntry &s : llvm::make_range(symbols.begin(), e))
    arr[s.sym->file].push_back(s);

  auto i = symbols.begin();
  for (auto &p : arr)
    for (SymbolTableEntry &entry : p.second)
      *i++ = entry;
}

void SymbolTableBaseSection::addSymbol(Symbol *b) {
  // Adding a local symbol to a .dynsym is a bug.
  assert(this->type != SHT_DYNSYM || !b->isLocal());
  symbols.push_back({b, strTabSec.addString(b->getName(), false)});
}

size_t SymbolTableBaseSection::getSymbolIndex(Ctx &ctx, Symbol *sym) {
  if (this == ctx.mainPart->dynSymTab.get())
    return sym->dynsymIndex;

  // Initializes symbol lookup tables lazily. This is used only for -r,
  // --emit-relocs and dynsyms in partitions other than the main one.
  llvm::call_once(onceFlag, [&] {
    symbolIndexMap.reserve(symbols.size());
    size_t i = 0;
    for (const SymbolTableEntry &e : symbols) {
      if (e.sym->type == STT_SECTION)
        sectionIndexMap[e.sym->getOutputSection()] = ++i;
      else
        symbolIndexMap[e.sym] = ++i;
    }
  });

  // Section symbols are mapped based on their output sections
  // to maintain their semantics.
  if (sym->type == STT_SECTION)
    return sectionIndexMap.lookup(sym->getOutputSection());
  return symbolIndexMap.lookup(sym);
}

template <class ELFT>
SymbolTableSection<ELFT>::SymbolTableSection(Ctx &ctx,
                                             StringTableSection &strTabSec)
    : SymbolTableBaseSection(ctx, strTabSec) {
  this->entsize = sizeof(Elf_Sym);
}

static BssSection *getCommonSec(Ctx &ctx, Symbol *sym) {
  if (ctx.config->relocatable)
    if (auto *d = dyn_cast<Defined>(sym))
      return dyn_cast_or_null<BssSection>(d->section);
  return nullptr;
}

static uint32_t getSymSectionIndex(Symbol *sym) {
  assert(!(sym->hasFlag(NEEDS_COPY) && sym->isObject()));
  if (!isa<Defined>(sym) || sym->hasFlag(NEEDS_COPY))
    return SHN_UNDEF;
  if (const OutputSection *os = sym->getOutputSection())
    return os->sectionIndex >= SHN_LORESERVE ? (uint32_t)SHN_XINDEX
                                             : os->sectionIndex;
  return SHN_ABS;
}

// Write the internal symbol table contents to the output symbol table.
template <class ELFT>
void SymbolTableSection<ELFT>::writeTo(Ctx &ctx, uint8_t *buf) {
  // The first entry is a null entry as per the ELF spec.
  buf += sizeof(Elf_Sym);

  auto *eSym = reinterpret_cast<Elf_Sym *>(buf);

  for (SymbolTableEntry &ent : symbols) {
    Symbol *sym = ent.sym;
    bool isDefinedHere = type == SHT_SYMTAB || sym->partition == partition;

    // Set st_name, st_info and st_other.
    eSym->st_name = ent.strTabOffset;
    eSym->setBindingAndType(sym->binding, sym->type);
    eSym->st_other = sym->stOther;

    if (BssSection *commonSec = getCommonSec(ctx, sym)) {
      // When -r is specified, a COMMON symbol is not allocated. Its st_shndx
      // holds SHN_COMMON and st_value holds the alignment.
      eSym->st_shndx = SHN_COMMON;
      eSym->st_value = commonSec->addralign;
      eSym->st_size = cast<Defined>(sym)->size;
    } else {
      const uint32_t shndx = getSymSectionIndex(sym);
      if (isDefinedHere) {
        eSym->st_shndx = shndx;
        eSym->st_value = sym->getVA(ctx);
        // Copy symbol size if it is a defined symbol. st_size is not
        // significant for undefined symbols, so whether copying it or not is up
        // to us if that's the case. We'll leave it as zero because by not
        // setting a value, we can get the exact same outputs for two sets of
        // input files that differ only in undefined symbol size in DSOs.
        eSym->st_size = shndx != SHN_UNDEF ? cast<Defined>(sym)->size : 0;
      } else {
        eSym->st_shndx = 0;
        eSym->st_value = 0;
        eSym->st_size = 0;
      }
    }

    ++eSym;
  }

  // On MIPS we need to mark symbol which has a PLT entry and requires
  // pointer equality by STO_MIPS_PLT flag. That is necessary to help
  // dynamic linker distinguish such symbols and MIPS lazy-binding stubs.
  // https://sourceware.org/ml/binutils/2008-07/txt00000.txt
  if (ctx.config->emachine == EM_MIPS) {
    auto *eSym = reinterpret_cast<Elf_Sym *>(buf);

    for (SymbolTableEntry &ent : symbols) {
      Symbol *sym = ent.sym;
      if (sym->isInPlt(ctx) && sym->hasFlag(NEEDS_COPY))
        eSym->st_other |= STO_MIPS_PLT;
      if (isMicroMips(ctx)) {
        // We already set the less-significant bit for symbols
        // marked by the `STO_MIPS_MICROMIPS` flag and for microMIPS PLT
        // records. That allows us to distinguish such symbols in
        // the `MIPS<ELFT>::relocate()` routine. Now we should
        // clear that bit for non-dynamic symbol table, so tools
        // like `objdump` will be able to deal with a correct
        // symbol position.
        if (sym->isDefined() &&
            ((sym->stOther & STO_MIPS_MICROMIPS) || sym->hasFlag(NEEDS_COPY))) {
          if (!strTabSec.isDynamic())
            eSym->st_value &= ~1;
          eSym->st_other |= STO_MIPS_MICROMIPS;
        }
      }
      if (ctx.config->relocatable)
        if (auto *d = dyn_cast<Defined>(sym))
          if (isMipsPIC<ELFT>(ctx, d))
            eSym->st_other |= STO_MIPS_PIC;
      ++eSym;
    }
  }
}

SymtabShndxSection::SymtabShndxSection(Ctx &ctx)
    : SyntheticSection(ctx, 0, SHT_SYMTAB_SHNDX, 4, ".symtab_shndx") {
  this->entsize = 4;
}

void SymtabShndxSection::writeTo(Ctx &ctx, uint8_t *buf) {
  // We write an array of 32 bit values, where each value has 1:1 association
  // with an entry in .symtab. If the corresponding entry contains SHN_XINDEX,
  // we need to write actual index, otherwise, we must write SHN_UNDEF(0).
  buf += 4; // Ignore .symtab[0] entry.
  for (const SymbolTableEntry &entry : ctx.in.symTab->getSymbols()) {
    if (!getCommonSec(ctx, entry.sym) &&
        getSymSectionIndex(entry.sym) == SHN_XINDEX)
      write32(ctx, buf, entry.sym->getOutputSection()->sectionIndex);
    buf += 4;
  }
}

bool SymtabShndxSection::isNeeded(Ctx &ctx) const {
  // SHT_SYMTAB can hold symbols with section indices values up to
  // SHN_LORESERVE. If we need more, we want to use extension SHT_SYMTAB_SHNDX
  // section. Problem is that we reveal the final section indices a bit too
  // late, and we do not know them here. For simplicity, we just always create
  // a .symtab_shndx section when the amount of output sections is huge.
  size_t size = 0;
  for (SectionCommand *cmd : ctx.script->sectionCommands)
    if (isa<OutputDesc>(cmd))
      ++size;
  return size >= SHN_LORESERVE;
}

void SymtabShndxSection::finalizeContents(Ctx &ctx) {
  getParent()->link = ctx.in.symTab->getParent()->sectionIndex;
}

size_t SymtabShndxSection::getSize(Ctx &ctx) const {
  return ctx.in.symTab->getNumSymbols() * 4;
}

// .hash and .gnu.hash sections contain on-disk hash tables that map
// symbol names to their dynamic symbol table indices. Their purpose
// is to help the dynamic linker resolve symbols quickly. If ELF files
// don't have them, the dynamic linker has to do linear search on all
// dynamic symbols, which makes programs slower. Therefore, a .hash
// section is added to a DSO by default.
//
// The Unix semantics of resolving dynamic symbols is somewhat expensive.
// Each ELF file has a list of DSOs that the ELF file depends on and a
// list of dynamic symbols that need to be resolved from any of the
// DSOs. That means resolving all dynamic symbols takes O(m)*O(n)
// where m is the number of DSOs and n is the number of dynamic
// symbols. For modern large programs, both m and n are large.  So
// making each step faster by using hash tables substantially
// improves time to load programs.
//
// (Note that this is not the only way to design the shared library.
// For instance, the Windows DLL takes a different approach. On
// Windows, each dynamic symbol has a name of DLL from which the symbol
// has to be resolved. That makes the cost of symbol resolution O(n).
// This disables some hacky techniques you can use on Unix such as
// LD_PRELOAD, but this is arguably better semantics than the Unix ones.)
//
// Due to historical reasons, we have two different hash tables, .hash
// and .gnu.hash. They are for the same purpose, and .gnu.hash is a new
// and better version of .hash. .hash is just an on-disk hash table, but
// .gnu.hash has a bloom filter in addition to a hash table to skip
// DSOs very quickly. If you are sure that your dynamic linker knows
// about .gnu.hash, you want to specify --hash-style=gnu. Otherwise, a
// safe bet is to specify --hash-style=both for backward compatibility.
GnuHashTableSection::GnuHashTableSection(Ctx &ctx)
    : SyntheticSection(ctx, SHF_ALLOC, SHT_GNU_HASH, ctx.config->wordsize,
                       ".gnu.hash") {}

void GnuHashTableSection::finalizeContents(Ctx &ctx) {
  if (OutputSection *sec = getPartition(ctx).dynSymTab->getParent())
    getParent()->link = sec->sectionIndex;

  // Computes bloom filter size in word size. We want to allocate 12
  // bits for each symbol. It must be a power of two.
  if (symbols.empty()) {
    maskWords = 1;
  } else {
    uint64_t numBits = symbols.size() * 12;
    maskWords = NextPowerOf2(numBits / (ctx.config->wordsize * 8));
  }

  size = 16;                                // Header
  size += ctx.config->wordsize * maskWords; // Bloom filter
  size += nBuckets * 4;                     // Hash buckets
  size += symbols.size() * 4;               // Hash values
}

void GnuHashTableSection::writeTo(Ctx &ctx, uint8_t *buf) {
  // Write a header.
  write32(ctx, buf, nBuckets);
  write32(ctx, buf + 4,
          getPartition(ctx).dynSymTab->getNumSymbols() - symbols.size());
  write32(ctx, buf + 8, maskWords);
  write32(ctx, buf + 12, Shift2);
  buf += 16;

  // Write the 2-bit bloom filter.
  const unsigned c = ctx.config->is64 ? 64 : 32;
  for (const Entry &sym : symbols) {
    // When C = 64, we choose a word with bits [6:...] and set 1 to two bits in
    // the word using bits [0:5] and [26:31].
    size_t i = (sym.hash / c) & (maskWords - 1);
    uint64_t val = readUint(ctx, buf + i * ctx.config->wordsize);
    val |= uint64_t(1) << (sym.hash % c);
    val |= uint64_t(1) << ((sym.hash >> Shift2) % c);
    writeUint(ctx, buf + i * ctx.config->wordsize, val);
  }
  buf += ctx.config->wordsize * maskWords;

  // Write the hash table.
  uint32_t *buckets = reinterpret_cast<uint32_t *>(buf);
  uint32_t oldBucket = -1;
  uint32_t *values = buckets + nBuckets;
  for (auto i = symbols.begin(), e = symbols.end(); i != e; ++i) {
    // Write a hash value. It represents a sequence of chains that share the
    // same hash modulo value. The last element of each chain is terminated by
    // LSB 1.
    uint32_t hash = i->hash;
    bool isLastInChain = (i + 1) == e || i->bucketIdx != (i + 1)->bucketIdx;
    hash = isLastInChain ? hash | 1 : hash & ~1;
    write32(ctx, values++, hash);

    if (i->bucketIdx == oldBucket)
      continue;
    // Write a hash bucket. Hash buckets contain indices in the following hash
    // value table.
    write32(ctx, buckets + i->bucketIdx,
            getPartition(ctx).dynSymTab->getSymbolIndex(ctx, i->sym));
    oldBucket = i->bucketIdx;
  }
}

// Add symbols to this symbol hash table. Note that this function
// destructively sort a given vector -- which is needed because
// GNU-style hash table places some sorting requirements.
void GnuHashTableSection::addSymbols(SmallVectorImpl<SymbolTableEntry> &v) {
  // We cannot use 'auto' for Mid because GCC 6.1 cannot deduce
  // its type correctly.
  auto mid =
      std::stable_partition(v.begin(), v.end(), [&](const SymbolTableEntry &s) {
        return !s.sym->isDefined() || s.sym->partition != partition;
      });

  // We chose load factor 4 for the on-disk hash table. For each hash
  // collision, the dynamic linker will compare a uint32_t hash value.
  // Since the integer comparison is quite fast, we believe we can
  // make the load factor even larger. 4 is just a conservative choice.
  //
  // Note that we don't want to create a zero-sized hash table because
  // Android loader as of 2018 doesn't like a .gnu.hash containing such
  // table. If that's the case, we create a hash table with one unused
  // dummy slot.
  nBuckets = std::max<size_t>((v.end() - mid) / 4, 1);

  if (mid == v.end())
    return;

  for (SymbolTableEntry &ent : llvm::make_range(mid, v.end())) {
    Symbol *b = ent.sym;
    uint32_t hash = hashGnu(b->getName());
    uint32_t bucketIdx = hash % nBuckets;
    symbols.push_back({b, ent.strTabOffset, hash, bucketIdx});
  }

  llvm::sort(symbols, [](const Entry &l, const Entry &r) {
    return std::tie(l.bucketIdx, l.strTabOffset) <
           std::tie(r.bucketIdx, r.strTabOffset);
  });

  v.erase(mid, v.end());
  for (const Entry &ent : symbols)
    v.push_back({ent.sym, ent.strTabOffset});
}

HashTableSection::HashTableSection(Ctx &ctx)
    : SyntheticSection(ctx, SHF_ALLOC, SHT_HASH, 4, ".hash") {
  this->entsize = 4;
}

void HashTableSection::finalizeContents(Ctx &ctx) {
  SymbolTableBaseSection *symTab = getPartition(ctx).dynSymTab.get();

  if (OutputSection *sec = symTab->getParent())
    getParent()->link = sec->sectionIndex;

  unsigned numEntries = 2;               // nbucket and nchain.
  numEntries += symTab->getNumSymbols(); // The chain entries.

  // Create as many buckets as there are symbols.
  numEntries += symTab->getNumSymbols();
  this->size = numEntries * 4;
}

void HashTableSection::writeTo(Ctx &ctx, uint8_t *buf) {
  SymbolTableBaseSection *symTab = getPartition(ctx).dynSymTab.get();
  unsigned numSymbols = symTab->getNumSymbols();

  uint32_t *p = reinterpret_cast<uint32_t *>(buf);
  write32(ctx, p++, numSymbols); // nbucket
  write32(ctx, p++, numSymbols); // nchain

  uint32_t *buckets = p;
  uint32_t *chains = p + numSymbols;

  for (const SymbolTableEntry &s : symTab->getSymbols()) {
    Symbol *sym = s.sym;
    StringRef name = sym->getName();
    unsigned i = sym->dynsymIndex;
    uint32_t hash = hashSysV(name) % numSymbols;
    chains[i] = buckets[hash];
    write32(ctx, buckets + hash, i);
  }
}

PltSection::PltSection(Ctx &ctx)
    : SyntheticSection(ctx, SHF_ALLOC | SHF_EXECINSTR, SHT_PROGBITS, 16,
                       ".plt"),
      headerSize(ctx.target->pltHeaderSize) {
  // On PowerPC, this section contains lazy symbol resolvers.
  if (ctx.config->emachine == EM_PPC64) {
    name = ".glink";
    addralign = 4;
  }

  // On x86 when IBT is enabled, this section contains the second PLT (lazy
  // symbol resolvers).
  if ((ctx.config->emachine == EM_386 || ctx.config->emachine == EM_X86_64) &&
      (ctx.config->andFeatures & GNU_PROPERTY_X86_FEATURE_1_IBT))
    name = ".plt.sec";

  // The PLT needs to be writable on SPARC as the dynamic linker will
  // modify the instructions in the PLT entries.
  if (ctx.config->emachine == EM_SPARCV9)
    this->flags |= SHF_WRITE;
}

void PltSection::writeTo(Ctx &ctx, uint8_t *buf) {
  // At beginning of PLT, we have code to call the dynamic
  // linker to resolve dynsyms at runtime. Write such code.
  ctx.target->writePltHeader(buf);
  size_t off = headerSize;

  for (const Symbol *sym : entries) {
    ctx.target->writePlt(buf + off, *sym, getVA(ctx) + off);
    off += ctx.target->pltEntrySize;
  }
}

void PltSection::addEntry(Ctx &ctx, Symbol &sym) {
  assert(sym.auxIdx == ctx.symAux.size() - 1);
  ctx.symAux.back().pltIdx = entries.size();
  entries.push_back(&sym);
}

size_t PltSection::getSize(Ctx &ctx) const {
  return headerSize + entries.size() * ctx.target->pltEntrySize;
}

bool PltSection::isNeeded(Ctx &ctx) const {
  // For -z retpolineplt, .iplt needs the .plt header.
  return !entries.empty() ||
         (ctx.config->zRetpolineplt && ctx.in.iplt->isNeeded(ctx));
}

// Used by ARM to add mapping symbols in the PLT section, which aid
// disassembly.
void PltSection::addSymbols(Ctx &ctx) {
  ctx.target->addPltHeaderSymbols(*this);

  size_t off = headerSize;
  for (size_t i = 0; i < entries.size(); ++i) {
    ctx.target->addPltSymbols(*this, off);
    off += ctx.target->pltEntrySize;
  }
}

IpltSection::IpltSection(Ctx &ctx)
    : SyntheticSection(ctx, SHF_ALLOC | SHF_EXECINSTR, SHT_PROGBITS, 16,
                       ".iplt") {
  if (ctx.config->emachine == EM_PPC || ctx.config->emachine == EM_PPC64) {
    name = ".glink";
    addralign = 4;
  }
}

void IpltSection::writeTo(Ctx &ctx, uint8_t *buf) {
  uint32_t off = 0;
  for (const Symbol *sym : entries) {
    ctx.target->writeIplt(buf + off, *sym, getVA(ctx) + off);
    off += ctx.target->ipltEntrySize;
  }
}

size_t IpltSection::getSize(Ctx &ctx) const {
  return entries.size() * ctx.target->ipltEntrySize;
}

void IpltSection::addEntry(Ctx &ctx, Symbol &sym) {
  assert(sym.auxIdx == ctx.symAux.size() - 1);
  ctx.symAux.back().pltIdx = entries.size();
  entries.push_back(&sym);
}

// ARM uses mapping symbols to aid disassembly.
void IpltSection::addSymbols(Ctx &ctx) {
  size_t off = 0;
  for (size_t i = 0, e = entries.size(); i != e; ++i) {
    ctx.target->addPltSymbols(*this, off);
    off += ctx.target->pltEntrySize;
  }
}

PPC32GlinkSection::PPC32GlinkSection(Ctx &ctx) : PltSection(ctx) {
  name = ".glink";
  addralign = 4;
}

void PPC32GlinkSection::writeTo(Ctx &ctx, uint8_t *buf) {
  writePPC32GlinkSection(ctx, buf, entries.size());
}

size_t PPC32GlinkSection::getSize(Ctx &ctx) const {
  return headerSize + entries.size() * ctx.target->pltEntrySize + footerSize;
}

// This is an x86-only extra PLT section and used only when a security
// enhancement feature called CET is enabled. In this comment, I'll explain what
// the feature is and why we have two PLT sections if CET is enabled.
//
// So, what does CET do? CET introduces a new restriction to indirect jump
// instructions. CET works this way. Assume that CET is enabled. Then, if you
// execute an indirect jump instruction, the processor verifies that a special
// "landing pad" instruction (which is actually a repurposed NOP instruction and
// now called "endbr32" or "endbr64") is at the jump target. If the jump target
// does not start with that instruction, the processor raises an exception
// instead of continuing executing code.
//
// If CET is enabled, the compiler emits endbr to all locations where indirect
// jumps may jump to.
//
// This mechanism makes it extremely hard to transfer the control to a middle of
// a function that is not supporsed to be a indirect jump target, preventing
// certain types of attacks such as ROP or JOP.
//
// Note that the processors in the market as of 2019 don't actually support the
// feature. Only the spec is available at the moment.
//
// Now, I'll explain why we have this extra PLT section for CET.
//
// Since you can indirectly jump to a PLT entry, we have to make PLT entries
// start with endbr. The problem is there's no extra space for endbr (which is 4
// bytes long), as the PLT entry is only 16 bytes long and all bytes are already
// used.
//
// In order to deal with the issue, we split a PLT entry into two PLT entries.
// Remember that each PLT entry contains code to jump to an address read from
// .got.plt AND code to resolve a dynamic symbol lazily. With the 2-PLT scheme,
// the former code is written to .plt.sec, and the latter code is written to
// .plt.
//
// Lazy symbol resolution in the 2-PLT scheme works in the usual way, except
// that the regular .plt is now called .plt.sec and .plt is repurposed to
// contain only code for lazy symbol resolution.
//
// In other words, this is how the 2-PLT scheme works. Application code is
// supposed to jump to .plt.sec to call an external function. Each .plt.sec
// entry contains code to read an address from a corresponding .got.plt entry
// and jump to that address. Addresses in .got.plt initially point to .plt, so
// when an application calls an external function for the first time, the
// control is transferred to a function that resolves a symbol name from
// external shared object files. That function then rewrites a .got.plt entry
// with a resolved address, so that the subsequent function calls directly jump
// to a desired location from .plt.sec.
//
// There is an open question as to whether the 2-PLT scheme was desirable or
// not. We could have simply extended the PLT entry size to 32-bytes to
// accommodate endbr, and that scheme would have been much simpler than the
// 2-PLT scheme. One reason to split PLT was, by doing that, we could keep hot
// code (.plt.sec) from cold code (.plt). But as far as I know no one proved
// that the optimization actually makes a difference.
//
// That said, the 2-PLT scheme is a part of the ABI, debuggers and other tools
// depend on it, so we implement the ABI.
IBTPltSection::IBTPltSection(Ctx &ctx)
    : SyntheticSection(ctx, SHF_ALLOC | SHF_EXECINSTR, SHT_PROGBITS, 16,
                       ".plt") {}

void IBTPltSection::writeTo(Ctx &ctx, uint8_t *buf) {
  ctx.target->writeIBTPlt(buf, ctx.in.plt->getNumEntries());
}

size_t IBTPltSection::getSize(Ctx &ctx) const {
  // 16 is the header size of .plt.
  return 16 + ctx.in.plt->getNumEntries() * ctx.target->pltEntrySize;
}

bool IBTPltSection::isNeeded(Ctx &ctx) const {
  return ctx.in.plt->getNumEntries() > 0;
}

RelroPaddingSection::RelroPaddingSection(Ctx &ctx)
    : SyntheticSection(ctx, SHF_ALLOC | SHF_WRITE, SHT_NOBITS, 1,
                       ".relro_padding") {}

// The string hash function for .gdb_index.
static uint32_t computeGdbHash(StringRef s) {
  uint32_t h = 0;
  for (uint8_t c : s)
    h = h * 67 + toLower(c) - 113;
  return h;
}

GdbIndexSection::GdbIndexSection(Ctx &ctx)
    : SyntheticSection(ctx, 0, SHT_PROGBITS, 1, ".gdb_index") {}

// Returns the desired size of an on-disk hash table for a .gdb_index section.
// There's a tradeoff between size and collision rate. We aim 75% utilization.
size_t GdbIndexSection::computeSymtabSize() const {
  return std::max<size_t>(NextPowerOf2(symbols.size() * 4 / 3), 1024);
}

static SmallVector<GdbIndexSection::CuEntry, 0>
readCuList(DWARFContext &dwarf) {
  SmallVector<GdbIndexSection::CuEntry, 0> ret;
  for (std::unique_ptr<DWARFUnit> &cu : dwarf.compile_units())
    ret.push_back({cu->getOffset(), cu->getLength() + 4});
  return ret;
}

static SmallVector<GdbIndexSection::AddressEntry, 0>
readAddressAreas(Ctx &ctx, DWARFContext &dwarf, InputSection *sec) {
  SmallVector<GdbIndexSection::AddressEntry, 0> ret;

  uint32_t cuIdx = 0;
  for (std::unique_ptr<DWARFUnit> &cu : dwarf.compile_units()) {
    if (Error e = cu->tryExtractDIEsIfNeeded(false)) {
      ctx.warn(toString(ctx, sec) + ": " + toString(std::move(e)));
      return {};
    }
    Expected<DWARFAddressRangesVector> ranges = cu->collectAddressRanges();
    if (!ranges) {
      ctx.warn(toString(ctx, sec) + ": " + toString(ranges.takeError()));
      return {};
    }

    ArrayRef<InputSectionBase *> sections = sec->file->getSections();
    for (DWARFAddressRange &r : *ranges) {
      if (r.SectionIndex == -1ULL)
        continue;
      // Range list with zero size has no effect.
      InputSectionBase *s = sections[r.SectionIndex];
      if (s && s != &ctx.discarded && s->isLive())
        if (r.LowPC != r.HighPC)
          ret.push_back({cast<InputSection>(s), r.LowPC, r.HighPC, cuIdx});
    }
    ++cuIdx;
  }

  return ret;
}

template <class ELFT>
static SmallVector<GdbIndexSection::NameAttrEntry, 0>
readPubNamesAndTypes(Ctx &ctx, const LLDDwarfObj<ELFT> &obj,
                     const SmallVectorImpl<GdbIndexSection::CuEntry> &cus) {
  const LLDDWARFSection &pubNames = obj.getGnuPubnamesSection();
  const LLDDWARFSection &pubTypes = obj.getGnuPubtypesSection();

  SmallVector<GdbIndexSection::NameAttrEntry, 0> ret;
  for (const LLDDWARFSection *pub : {&pubNames, &pubTypes}) {
    DWARFDataExtractor data(obj, *pub, ctx.config->isLE, ctx.config->wordsize);
    DWARFDebugPubTable table;
    table.extract(data, /*GnuStyle=*/true, [&](Error e) {
      ctx.warn(toString(ctx, pub->sec) + ": " + toString(std::move(e)));
    });
    for (const DWARFDebugPubTable::Set &set : table.getData()) {
      // The value written into the constant pool is kind << 24 | cuIndex. As we
      // don't know how many compilation units precede this object to compute
      // cuIndex, we compute (kind << 24 | cuIndexInThisObject) instead, and add
      // the number of preceding compilation units later.
      uint32_t i = llvm::partition_point(cus,
                                         [&](GdbIndexSection::CuEntry cu) {
                                           return cu.cuOffset < set.Offset;
                                         }) -
                   cus.begin();
      for (const DWARFDebugPubTable::Entry &ent : set.Entries)
        ret.push_back({{ent.Name, computeGdbHash(ent.Name)},
                       (ent.Descriptor.toBits() << 24) | i});
    }
  }
  return ret;
}

// Create a list of symbols from a given list of symbol names and types
// by uniquifying them by name.
static std::pair<SmallVector<GdbIndexSection::GdbSymbol, 0>, size_t>
createSymbols(
    Ctx &ctx,
    ArrayRef<SmallVector<GdbIndexSection::NameAttrEntry, 0>> nameAttrs,
    const SmallVector<GdbIndexSection::GdbChunk, 0> &chunks) {
  using GdbSymbol = GdbIndexSection::GdbSymbol;
  using NameAttrEntry = GdbIndexSection::NameAttrEntry;

  // For each chunk, compute the number of compilation units preceding it.
  uint32_t cuIdx = 0;
  std::unique_ptr<uint32_t[]> cuIdxs(new uint32_t[chunks.size()]);
  for (uint32_t i = 0, e = chunks.size(); i != e; ++i) {
    cuIdxs[i] = cuIdx;
    cuIdx += chunks[i].compilationUnits.size();
  }

  // The number of symbols we will handle in this function is of the order
  // of millions for very large executables, so we use multi-threading to
  // speed it up.
  constexpr size_t numShards = 32;
  const size_t concurrency =
      llvm::bit_floor(std::min<size_t>(ctx.config->threadCount, numShards));

  // A sharded map to uniquify symbols by name.
  auto map =
      std::make_unique<DenseMap<CachedHashStringRef, size_t>[]>(numShards);
  size_t shift = 32 - llvm::countr_zero(numShards);

  // Instantiate GdbSymbols while uniqufying them by name.
  auto symbols = std::make_unique<SmallVector<GdbSymbol, 0>[]>(numShards);

  parallelFor(0, concurrency, [&](size_t threadId) {
    uint32_t i = 0;
    for (ArrayRef<NameAttrEntry> entries : nameAttrs) {
      for (const NameAttrEntry &ent : entries) {
        size_t shardId = ent.name.hash() >> shift;
        if ((shardId & (concurrency - 1)) != threadId)
          continue;

        uint32_t v = ent.cuIndexAndAttrs + cuIdxs[i];
        size_t &idx = map[shardId][ent.name];
        if (idx) {
          symbols[shardId][idx - 1].cuVector.push_back(v);
          continue;
        }

        idx = symbols[shardId].size() + 1;
        symbols[shardId].push_back({ent.name, {v}, 0, 0});
      }
      ++i;
    }
  });

  size_t numSymbols = 0;
  for (ArrayRef<GdbSymbol> v : ArrayRef(symbols.get(), numShards))
    numSymbols += v.size();

  // The return type is a flattened vector, so we'll copy each vector
  // contents to Ret.
  SmallVector<GdbSymbol, 0> ret;
  ret.reserve(numSymbols);
  for (SmallVector<GdbSymbol, 0> &vec :
       MutableArrayRef(symbols.get(), numShards))
    for (GdbSymbol &sym : vec)
      ret.push_back(std::move(sym));

  // CU vectors and symbol names are adjacent in the output file.
  // We can compute their offsets in the output file now.
  size_t off = 0;
  for (GdbSymbol &sym : ret) {
    sym.cuVectorOff = off;
    off += (sym.cuVector.size() + 1) * 4;
  }
  for (GdbSymbol &sym : ret) {
    sym.nameOff = off;
    off += sym.name.size() + 1;
  }
  // If off overflows, the last symbol's nameOff likely overflows.
  if (!isUInt<32>(off))
    errorOrWarn(ctx, "--gdb-index: constant pool size (" + Twine(off) +
                         ") exceeds UINT32_MAX");

  return {ret, off};
}

// Returns a newly-created .gdb_index section.
template <class ELFT> GdbIndexSection *GdbIndexSection::create(Ctx &ctx) {
  llvm::TimeTraceScope timeScope("Create gdb index");

  // Collect InputFiles with .debug_info. See the comment in
  // LLDDwarfObj<ELFT>::LLDDwarfObj. If we do lightweight parsing in the future,
  // note that isec->data() may uncompress the full content, which should be
  // parallelized.
  SetVector<InputFile *> files;
  for (InputSectionBase *s : ctx.inputSections) {
    InputSection *isec = dyn_cast<InputSection>(s);
    if (!isec)
      continue;
    // .debug_gnu_pub{names,types} are useless in executables.
    // They are present in input object files solely for creating
    // a .gdb_index. So we can remove them from the output.
    if (s->name == ".debug_gnu_pubnames" || s->name == ".debug_gnu_pubtypes")
      s->markDead();
    else if (isec->name == ".debug_info")
      files.insert(isec->file);
  }
  // Drop .rel[a].debug_gnu_pub{names,types} for --emit-relocs.
  llvm::erase_if(ctx.inputSections, [](InputSectionBase *s) {
    if (auto *isec = dyn_cast<InputSection>(s))
      if (InputSectionBase *rel = isec->getRelocatedSection())
        return !rel->isLive();
    return !s->isLive();
  });

  SmallVector<GdbChunk, 0> chunks(files.size());
  SmallVector<SmallVector<NameAttrEntry, 0>, 0> nameAttrs(files.size());

  parallelFor(0, files.size(), [&](size_t i) {
    // To keep memory usage low, we don't want to keep cached DWARFContext, so
    // avoid getDwarf() here.
    ObjFile<ELFT> *file = cast<ObjFile<ELFT>>(files[i]);
    DWARFContext dwarf(std::make_unique<LLDDwarfObj<ELFT>>(ctx, file));
    auto &dobj = static_cast<const LLDDwarfObj<ELFT> &>(dwarf.getDWARFObj());

    // If the are multiple compile units .debug_info (very rare ld -r --unique),
    // this only picks the last one. Other address ranges are lost.
    chunks[i].sec = dobj.getInfoSection();
    chunks[i].compilationUnits = readCuList(dwarf);
    chunks[i].addressAreas = readAddressAreas(ctx, dwarf, chunks[i].sec);
    nameAttrs[i] =
        readPubNamesAndTypes<ELFT>(ctx, dobj, chunks[i].compilationUnits);
  });

  auto *ret = ctx.make<GdbIndexSection>(ctx);
  ret->chunks = std::move(chunks);
  std::tie(ret->symbols, ret->size) =
      createSymbols(ctx, nameAttrs, ret->chunks);

  // Count the areas other than the constant pool.
  ret->size += sizeof(GdbIndexHeader) + ret->computeSymtabSize() * 8;
  for (GdbChunk &chunk : ret->chunks)
    ret->size +=
        chunk.compilationUnits.size() * 16 + chunk.addressAreas.size() * 20;

  return ret;
}

void GdbIndexSection::writeTo(Ctx &ctx, uint8_t *buf) {
  // Write the header.
  auto *hdr = reinterpret_cast<GdbIndexHeader *>(buf);
  uint8_t *start = buf;
  hdr->version = 7;
  buf += sizeof(*hdr);

  // Write the CU list.
  hdr->cuListOff = buf - start;
  for (GdbChunk &chunk : chunks) {
    for (CuEntry &cu : chunk.compilationUnits) {
      write64le(buf, chunk.sec->outSecOff + cu.cuOffset);
      write64le(buf + 8, cu.cuLength);
      buf += 16;
    }
  }

  // Write the address area.
  hdr->cuTypesOff = buf - start;
  hdr->addressAreaOff = buf - start;
  uint32_t cuOff = 0;
  for (GdbChunk &chunk : chunks) {
    for (AddressEntry &e : chunk.addressAreas) {
      // In the case of ICF there may be duplicate address range entries.
      const uint64_t baseAddr = e.section->repl->getVA(ctx, 0);
      write64le(buf, baseAddr + e.lowAddress);
      write64le(buf + 8, baseAddr + e.highAddress);
      write32le(buf + 16, e.cuIndex + cuOff);
      buf += 20;
    }
    cuOff += chunk.compilationUnits.size();
  }

  // Write the on-disk open-addressing hash table containing symbols.
  hdr->symtabOff = buf - start;
  size_t symtabSize = computeSymtabSize();
  uint32_t mask = symtabSize - 1;

  for (GdbSymbol &sym : symbols) {
    uint32_t h = sym.name.hash();
    uint32_t i = h & mask;
    uint32_t step = ((h * 17) & mask) | 1;

    while (read32le(buf + i * 8))
      i = (i + step) & mask;

    write32le(buf + i * 8, sym.nameOff);
    write32le(buf + i * 8 + 4, sym.cuVectorOff);
  }

  buf += symtabSize * 8;

  // Write the string pool.
  hdr->constantPoolOff = buf - start;
  parallelForEach(symbols, [&](GdbSymbol &sym) {
    memcpy(buf + sym.nameOff, sym.name.data(), sym.name.size());
  });

  // Write the CU vectors.
  for (GdbSymbol &sym : symbols) {
    write32le(buf, sym.cuVector.size());
    buf += 4;
    for (uint32_t val : sym.cuVector) {
      write32le(buf, val);
      buf += 4;
    }
  }
}

bool GdbIndexSection::isNeeded(Ctx &ctx) const { return !chunks.empty(); }

EhFrameHeader::EhFrameHeader(Ctx &ctx)
    : SyntheticSection(ctx, SHF_ALLOC, SHT_PROGBITS, 4, ".eh_frame_hdr") {}

void EhFrameHeader::writeTo(Ctx &ctx, uint8_t *buf) {
  // Unlike most sections, the EhFrameHeader section is written while writing
  // another section, namely EhFrameSection, which calls the write() function
  // below from its writeTo() function. This is necessary because the contents
  // of EhFrameHeader depend on the relocated contents of EhFrameSection and we
  // don't know which order the sections will be written in.
}

// .eh_frame_hdr contains a binary search table of pointers to FDEs.
// Each entry of the search table consists of two values,
// the starting PC from where FDEs covers, and the FDE's address.
// It is sorted by PC.
void EhFrameHeader::write(Ctx &ctx) {
  uint8_t *buf = ctx.out.bufferStart + getParent()->offset + outSecOff;
  using FdeData = EhFrameSection::FdeData;
  SmallVector<FdeData, 0> fdes = getPartition(ctx).ehFrame->getFdeData(ctx);

  buf[0] = 1;
  buf[1] = DW_EH_PE_pcrel | DW_EH_PE_sdata4;
  buf[2] = DW_EH_PE_udata4;
  buf[3] = DW_EH_PE_datarel | DW_EH_PE_sdata4;
  write32(ctx, buf + 4,
          getPartition(ctx).ehFrame->getParent()->addr - this->getVA(ctx) - 4);
  write32(ctx, buf + 8, fdes.size());
  buf += 12;

  for (FdeData &fde : fdes) {
    write32(ctx, buf, fde.pcRel);
    write32(ctx, buf + 4, fde.fdeVARel);
    buf += 8;
  }
}

size_t EhFrameHeader::getSize(Ctx &ctx) const {
  // .eh_frame_hdr has a 12 bytes header followed by an array of FDEs.
  return 12 + getPartition(ctx).ehFrame->numFdes * 8;
}

bool EhFrameHeader::isNeeded(Ctx &ctx) const {
  return isLive() && getPartition(ctx).ehFrame->isNeeded(ctx);
}

VersionDefinitionSection::VersionDefinitionSection(Ctx &ctx)
    : SyntheticSection(ctx, SHF_ALLOC, SHT_GNU_verdef, sizeof(uint32_t),
                       ".gnu.version_d") {}

StringRef VersionDefinitionSection::getFileDefName(Ctx &ctx) {
  if (!getPartition(ctx).name.empty())
    return getPartition(ctx).name;
  if (!ctx.config->soName.empty())
    return ctx.config->soName;
  return ctx.config->outputFile;
}

void VersionDefinitionSection::finalizeContents(Ctx &ctx) {
  fileDefNameOff = getPartition(ctx).dynStrTab->addString(getFileDefName(ctx));
  for (const VersionDefinition &v : namedVersionDefs(ctx))
    verDefNameOffs.push_back(getPartition(ctx).dynStrTab->addString(v.name));

  if (OutputSection *sec = getPartition(ctx).dynStrTab->getParent())
    getParent()->link = sec->sectionIndex;

  // sh_info should be set to the number of definitions. This fact is missed in
  // documentation, but confirmed by binutils community:
  // https://sourceware.org/ml/binutils/2014-11/msg00355.html
  getParent()->info = getVerDefNum(ctx);
}

void VersionDefinitionSection::writeOne(Ctx &ctx, uint8_t *buf, uint32_t index,
                                        StringRef name, size_t nameOff) {
  uint16_t flags = index == 1 ? VER_FLG_BASE : 0;

  // Write a verdef.
  write16(ctx, buf, 1);                  // vd_version
  write16(ctx, buf + 2, flags);          // vd_flags
  write16(ctx, buf + 4, index);          // vd_ndx
  write16(ctx, buf + 6, 1);              // vd_cnt
  write32(ctx, buf + 8, hashSysV(name)); // vd_hash
  write32(ctx, buf + 12, 20);            // vd_aux
  write32(ctx, buf + 16, 28);            // vd_next

  // Write a veraux.
  write32(ctx, buf + 20, nameOff); // vda_name
  write32(ctx, buf + 24, 0);       // vda_next
}

void VersionDefinitionSection::writeTo(Ctx &ctx, uint8_t *buf) {
  writeOne(ctx, buf, 1, getFileDefName(ctx), fileDefNameOff);

  auto nameOffIt = verDefNameOffs.begin();
  for (const VersionDefinition &v : namedVersionDefs(ctx)) {
    buf += EntrySize;
    writeOne(ctx, buf, v.id, v.name, *nameOffIt++);
  }

  // Need to terminate the last version definition.
  write32(ctx, buf + 16, 0); // vd_next
}

size_t VersionDefinitionSection::getSize(Ctx &ctx) const {
  return EntrySize * getVerDefNum(ctx);
}

// .gnu.version is a table where each entry is 2 byte long.
VersionTableSection::VersionTableSection(Ctx &ctx)
    : SyntheticSection(ctx, SHF_ALLOC, SHT_GNU_versym, sizeof(uint16_t),
                       ".gnu.version") {
  this->entsize = 2;
}

void VersionTableSection::finalizeContents(Ctx &ctx) {
  // At the moment of june 2016 GNU docs does not mention that sh_link field
  // should be set, but Sun docs do. Also readelf relies on this field.
  getParent()->link = getPartition(ctx).dynSymTab->getParent()->sectionIndex;
}

size_t VersionTableSection::getSize(Ctx &ctx) const {
  return (getPartition(ctx).dynSymTab->getSymbols().size() + 1) * 2;
}

void VersionTableSection::writeTo(Ctx &ctx, uint8_t *buf) {
  buf += 2;
  for (const SymbolTableEntry &s : getPartition(ctx).dynSymTab->getSymbols()) {
    // For an unextracted lazy symbol (undefined weak), it must have been
    // converted to Undefined and have VER_NDX_GLOBAL version here.
    assert(!s.sym->isLazy());
    write16(ctx, buf, s.sym->versionId);
    buf += 2;
  }
}

bool VersionTableSection::isNeeded(Ctx &ctx) const {
  return isLive() &&
         (getPartition(ctx).verDef || getPartition(ctx).verNeed->isNeeded(ctx));
}

void elf::addVerneed(Ctx &ctx, Symbol *ss) {
  auto &file = cast<SharedFile>(*ss->file);
  if (ss->versionId == VER_NDX_GLOBAL)
    return;

  if (file.vernauxs.empty())
    file.vernauxs.resize(file.verdefs.size());

  // Select a version identifier for the vernaux data structure, if we haven't
  // already allocated one. The verdef identifiers cover the range
  // [1..getVerDefNum()]; this causes the vernaux identifiers to start from
  // getVerDefNum()+1.
  if (file.vernauxs[ss->versionId] == 0)
    file.vernauxs[ss->versionId] = ++ctx.vernauxNum + getVerDefNum(ctx);

  ss->versionId = file.vernauxs[ss->versionId];
}

unsigned Partition::getNumber(Ctx &ctx) const {
  return this - &ctx.partitions[0] + 1;
}

Partition &SectionBase::getPartition(Ctx &ctx) const {
  assert(isLive());
  return ctx.partitions[partition - 1];
}

template <class ELFT>
VersionNeedSection<ELFT>::VersionNeedSection(Ctx &ctx)
    : SyntheticSection(ctx, SHF_ALLOC, SHT_GNU_verneed, sizeof(uint32_t),
                       ".gnu.version_r") {}

template <class ELFT>
void VersionNeedSection<ELFT>::finalizeContents(Ctx &ctx) {
  for (SharedFile *f : ctx.sharedFiles) {
    if (f->vernauxs.empty())
      continue;
    verneeds.emplace_back();
    Verneed &vn = verneeds.back();
    vn.nameStrTab = getPartition(ctx).dynStrTab->addString(f->soName);
    bool isLibc = ctx.config->relrGlibc && f->soName.starts_with("libc.so.");
    bool isGlibc2 = false;
    for (unsigned i = 0; i != f->vernauxs.size(); ++i) {
      if (f->vernauxs[i] == 0)
        continue;
      auto *verdef =
          reinterpret_cast<const typename ELFT::Verdef *>(f->verdefs[i]);
      StringRef ver(f->getStringTable().data() + verdef->getAux()->vda_name);
      if (isLibc && ver.starts_with("GLIBC_2."))
        isGlibc2 = true;
      vn.vernauxs.push_back({verdef->vd_hash, f->vernauxs[i],
                             getPartition(ctx).dynStrTab->addString(ver)});
    }
    if (isGlibc2) {
      const char *ver = "GLIBC_ABI_DT_RELR";
      vn.vernauxs.push_back({hashSysV(ver),
                             ++ctx.vernauxNum + getVerDefNum(ctx),
                             getPartition(ctx).dynStrTab->addString(ver)});
    }
  }

  if (OutputSection *sec = getPartition(ctx).dynStrTab->getParent())
    getParent()->link = sec->sectionIndex;
  getParent()->info = verneeds.size();
}

template <class ELFT>
void VersionNeedSection<ELFT>::writeTo(Ctx &ctx, uint8_t *buf) {
  // The Elf_Verneeds need to appear first, followed by the Elf_Vernauxs.
  auto *verneed = reinterpret_cast<Elf_Verneed *>(buf);
  auto *vernaux = reinterpret_cast<Elf_Vernaux *>(verneed + verneeds.size());

  for (auto &vn : verneeds) {
    // Create an Elf_Verneed for this DSO.
    verneed->vn_version = 1;
    verneed->vn_cnt = vn.vernauxs.size();
    verneed->vn_file = vn.nameStrTab;
    verneed->vn_aux =
        reinterpret_cast<char *>(vernaux) - reinterpret_cast<char *>(verneed);
    verneed->vn_next = sizeof(Elf_Verneed);
    ++verneed;

    // Create the Elf_Vernauxs for this Elf_Verneed.
    for (auto &vna : vn.vernauxs) {
      vernaux->vna_hash = vna.hash;
      vernaux->vna_flags = 0;
      vernaux->vna_other = vna.verneedIndex;
      vernaux->vna_name = vna.nameStrTab;
      vernaux->vna_next = sizeof(Elf_Vernaux);
      ++vernaux;
    }

    vernaux[-1].vna_next = 0;
  }
  verneed[-1].vn_next = 0;
}

template <class ELFT> size_t VersionNeedSection<ELFT>::getSize(Ctx &ctx) const {
  return verneeds.size() * sizeof(Elf_Verneed) +
         ctx.vernauxNum * sizeof(Elf_Vernaux);
}

template <class ELFT> bool VersionNeedSection<ELFT>::isNeeded(Ctx &ctx) const {
  return isLive() && ctx.vernauxNum != 0;
}

void MergeSyntheticSection::addSection(MergeInputSection *ms) {
  ms->parent = this;
  sections.push_back(ms);
  assert(addralign == ms->addralign || !(ms->flags & SHF_STRINGS));
  addralign = std::max(addralign, ms->addralign);
}

MergeTailSection::MergeTailSection(Ctx &ctx, StringRef name, uint32_t type,
                                   uint64_t flags, uint32_t alignment)
    : MergeSyntheticSection(ctx, name, type, flags, alignment),
      builder(StringTableBuilder::RAW, llvm::Align(alignment)) {}

size_t MergeTailSection::getSize(Ctx &ctx) const { return builder.getSize(); }

void MergeTailSection::writeTo(Ctx &ctx, uint8_t *buf) { builder.write(buf); }

void MergeTailSection::finalizeContents(Ctx &ctx) {
  // Add all string pieces to the string table builder to create section
  // contents.
  for (MergeInputSection *sec : sections)
    for (size_t i = 0, e = sec->pieces.size(); i != e; ++i)
      if (sec->pieces[i].live)
        builder.add(sec->getData(i));

  // Fix the string table content. After this, the contents will never change.
  builder.finalize();

  // finalize() fixed tail-optimized strings, so we can now get
  // offsets of strings. Get an offset for each string and save it
  // to a corresponding SectionPiece for easy access.
  for (MergeInputSection *sec : sections)
    for (size_t i = 0, e = sec->pieces.size(); i != e; ++i)
      if (sec->pieces[i].live)
        sec->pieces[i].outputOff = builder.getOffset(sec->getData(i));
}

void MergeNoTailSection::writeTo(Ctx &ctx, uint8_t *buf) {
  parallelFor(0, numShards,
              [&](size_t i) { shards[i].write(buf + shardOffsets[i]); });
}

// This function is very hot (i.e. it can take several seconds to finish)
// because sometimes the number of inputs is in an order of magnitude of
// millions. So, we use multi-threading.
//
// For any strings S and T, we know S is not mergeable with T if S's hash
// value is different from T's. If that's the case, we can safely put S and
// T into different string builders without worrying about merge misses.
// We do it in parallel.
void MergeNoTailSection::finalizeContents(Ctx &ctx) {
  // Initializes string table builders.
  for (size_t i = 0; i < numShards; ++i)
    shards.emplace_back(StringTableBuilder::RAW, llvm::Align(addralign));

  // Concurrency level. Must be a power of 2 to avoid expensive modulo
  // operations in the following tight loop.
  const size_t concurrency =
      llvm::bit_floor(std::min<size_t>(ctx.config->threadCount, numShards));

  // Add section pieces to the builders.
  parallelFor(0, concurrency, [&](size_t threadId) {
    for (MergeInputSection *sec : sections) {
      for (size_t i = 0, e = sec->pieces.size(); i != e; ++i) {
        if (!sec->pieces[i].live)
          continue;
        size_t shardId = getShardId(sec->pieces[i].hash);
        if ((shardId & (concurrency - 1)) == threadId)
          sec->pieces[i].outputOff = shards[shardId].add(sec->getData(i));
      }
    }
  });

  // Compute an in-section offset for each shard.
  size_t off = 0;
  for (size_t i = 0; i < numShards; ++i) {
    shards[i].finalizeInOrder();
    if (shards[i].getSize() > 0)
      off = alignToPowerOf2(off, addralign);
    shardOffsets[i] = off;
    off += shards[i].getSize();
  }
  size = off;

  // So far, section pieces have offsets from beginning of shards, but
  // we want offsets from beginning of the whole section. Fix them.
  parallelForEach(sections, [&](MergeInputSection *sec) {
    for (size_t i = 0, e = sec->pieces.size(); i != e; ++i)
      if (sec->pieces[i].live)
        sec->pieces[i].outputOff +=
            shardOffsets[getShardId(sec->pieces[i].hash)];
  });
}

template <class ELFT> void elf::splitSections(Ctx &ctx) {
  llvm::TimeTraceScope timeScope("Split sections");
  // splitIntoPieces needs to be called on each MergeInputSection
  // before calling finalizeContents().
  parallelForEach(ctx.objectFiles, [&ctx](ELFFileBase *file) {
    for (InputSectionBase *sec : file->getSections()) {
      if (!sec)
        continue;
      if (auto *s = dyn_cast<MergeInputSection>(sec))
        s->splitIntoPieces(ctx);
      else if (auto *eh = dyn_cast<EhInputSection>(sec))
        eh->split<ELFT>(ctx);
    }
  });
}

void elf::combineEhSections(Ctx &ctx) {
  llvm::TimeTraceScope timeScope("Combine EH sections");
  for (EhInputSection *sec : ctx.ehInputSections) {
    EhFrameSection &eh = *sec->getPartition(ctx).ehFrame;
    sec->parent = &eh;
    eh.addralign = std::max(eh.addralign, sec->addralign);
    eh.sections.push_back(sec);
    llvm::append_range(eh.dependentSections, sec->dependentSections);
  }

  if (!ctx.mainPart->armExidx)
    return;
  llvm::erase_if(ctx.inputSections, [&ctx](InputSectionBase *s) {
    // Ignore dead sections and the partition end marker (.part.end),
    // whose partition number is out of bounds.
    if (!s->isLive() || s->partition == 255)
      return false;
    Partition &part = s->getPartition(ctx);
    return s->kind() == SectionBase::Regular && part.armExidx &&
           part.armExidx->addSection(ctx, cast<InputSection>(s));
  });
}

MipsRldMapSection::MipsRldMapSection(Ctx &ctx)
    : SyntheticSection(ctx, SHF_ALLOC | SHF_WRITE, SHT_PROGBITS,
                       ctx.config->wordsize, ".rld_map") {}

size_t MipsRldMapSection::getSize(Ctx &ctx) const {
  return ctx.config->wordsize;
}

ARMExidxSyntheticSection::ARMExidxSyntheticSection(Ctx &ctx)
    : SyntheticSection(ctx, SHF_ALLOC | SHF_LINK_ORDER, SHT_ARM_EXIDX,
                       ctx.config->wordsize, ".ARM.exidx") {}

static InputSection *findExidxSection(InputSection *isec) {
  for (InputSection *d : isec->dependentSections)
    if (d->type == SHT_ARM_EXIDX && d->isLive())
      return d;
  return nullptr;
}

static bool isValidExidxSectionDep(Ctx &ctx, InputSection *isec) {
  return (isec->flags & SHF_ALLOC) && (isec->flags & SHF_EXECINSTR) &&
         isec->getSize(ctx) > 0;
}

bool ARMExidxSyntheticSection::addSection(Ctx &ctx, InputSection *isec) {
  if (isec->type == SHT_ARM_EXIDX) {
    if (InputSection *dep = isec->getLinkOrderDep())
      if (isValidExidxSectionDep(ctx, dep)) {
        exidxSections.push_back(isec);
        // Every exidxSection is 8 bytes, we need an estimate of
        // size before assignAddresses can be called. Final size
        // will only be known after finalize is called.
        size += 8;
      }
    return true;
  }

  if (isValidExidxSectionDep(ctx, isec)) {
    executableSections.push_back(isec);
    return false;
  }

  // FIXME: we do not output a relocation section when --emit-relocs is used
  // as we do not have relocation sections for linker generated table entries
  // and we would have to erase at a late stage relocations from merged entries.
  // Given that exception tables are already position independent and a binary
  // analyzer could derive the relocations we choose to erase the relocations.
  if (ctx.config->emitRelocs && isec->type == SHT_REL)
    if (InputSectionBase *ex = isec->getRelocatedSection())
      if (isa<InputSection>(ex) && ex->type == SHT_ARM_EXIDX)
        return true;

  return false;
}

// References to .ARM.Extab Sections have bit 31 clear and are not the
// special EXIDX_CANTUNWIND bit-pattern.
static bool isExtabRef(uint32_t unwind) {
  return (unwind & 0x80000000) == 0 && unwind != 0x1;
}

// Return true if the .ARM.exidx section Cur can be merged into the .ARM.exidx
// section Prev, where Cur follows Prev in the table. This can be done if the
// unwinding instructions in Cur are identical to Prev. Linker generated
// EXIDX_CANTUNWIND entries are represented by nullptr as they do not have an
// InputSection.
static bool isDuplicateArmExidxSec(Ctx &ctx, InputSection *prev,
                                   InputSection *cur) {
  // Get the last table Entry from the previous .ARM.exidx section. If Prev is
  // nullptr then it will be a synthesized EXIDX_CANTUNWIND entry.
  uint32_t prevUnwind = 1;
  if (prev)
    prevUnwind =
        read32(ctx, prev->content().data() + prev->content().size() - 4);
  if (isExtabRef(prevUnwind))
    return false;

  // We consider the unwind instructions of an .ARM.exidx table entry
  // a duplicate if the previous unwind instructions if:
  // - Both are the special EXIDX_CANTUNWIND.
  // - Both are the same inline unwind instructions.
  // We do not attempt to follow and check links into .ARM.extab tables as
  // consecutive identical entries are rare and the effort to check that they
  // are identical is high.

  // If Cur is nullptr then this is synthesized EXIDX_CANTUNWIND entry.
  if (cur == nullptr)
    return prevUnwind == 1;

  for (uint32_t offset = 4; offset < (uint32_t)cur->content().size();
       offset += 8) {
    uint32_t curUnwind = read32(ctx, cur->content().data() + offset);
    if (isExtabRef(curUnwind) || curUnwind != prevUnwind)
      return false;
  }
  // All table entries in this .ARM.exidx Section can be merged into the
  // previous Section.
  return true;
}

// The .ARM.exidx table must be sorted in ascending order of the address of the
// functions the table describes. std::optionally duplicate adjacent table
// entries can be removed. At the end of the function the executableSections
// must be sorted in ascending order of address, Sentinel is set to the
// InputSection with the highest address and any InputSections that have
// mergeable .ARM.exidx table entries are removed from it.
void ARMExidxSyntheticSection::finalizeContents(Ctx &ctx) {
  // The executableSections and exidxSections that we use to derive the final
  // contents of this SyntheticSection are populated before
  // processSectionCommands() and ICF. A /DISCARD/ entry in SECTIONS command or
  // ICF may remove executable InputSections and their dependent .ARM.exidx
  // section that we recorded earlier.
  auto isDiscarded = [](const InputSection *isec) { return !isec->isLive(); };
  llvm::erase_if(exidxSections, isDiscarded);
  // We need to remove discarded InputSections and InputSections without
  // .ARM.exidx sections that if we generated the .ARM.exidx it would be out
  // of range.
  auto isDiscardedOrOutOfRange = [this, &ctx](InputSection *isec) {
    if (!isec->isLive())
      return true;
    if (findExidxSection(isec))
      return false;
    int64_t off = static_cast<int64_t>(isec->getVA(ctx) - getVA(ctx));
    return off != llvm::SignExtend64(off, 31);
  };
  llvm::erase_if(executableSections, isDiscardedOrOutOfRange);

  // Sort the executable sections that may or may not have associated
  // .ARM.exidx sections by order of ascending address. This requires the
  // relative positions of InputSections and OutputSections to be known.
  auto compareByFilePosition = [](const InputSection *a,
                                  const InputSection *b) {
    OutputSection *aOut = a->getParent();
    OutputSection *bOut = b->getParent();

    if (aOut != bOut)
      return aOut->addr < bOut->addr;
    return a->outSecOff < b->outSecOff;
  };
  llvm::stable_sort(executableSections, compareByFilePosition);
  sentinel = executableSections.back();
  // std::optionally merge adjacent duplicate entries.
  if (ctx.config->mergeArmExidx) {
    SmallVector<InputSection *, 0> selectedSections;
    selectedSections.reserve(executableSections.size());
    selectedSections.push_back(executableSections[0]);
    size_t prev = 0;
    for (size_t i = 1; i < executableSections.size(); ++i) {
      InputSection *ex1 = findExidxSection(executableSections[prev]);
      InputSection *ex2 = findExidxSection(executableSections[i]);
      if (!isDuplicateArmExidxSec(ctx, ex1, ex2)) {
        selectedSections.push_back(executableSections[i]);
        prev = i;
      }
    }
    executableSections = std::move(selectedSections);
  }
  // offset is within the SyntheticSection.
  size_t offset = 0;
  size = 0;
  for (InputSection *isec : executableSections) {
    if (InputSection *d = findExidxSection(isec)) {
      d->outSecOff = offset;
      d->parent = getParent();
      offset += d->getSize(ctx);
    } else {
      offset += 8;
    }
  }
  // Size includes Sentinel.
  size = offset + 8;
}

InputSection *ARMExidxSyntheticSection::getLinkOrderDep() const {
  return executableSections.front();
}

// To write the .ARM.exidx table from the ExecutableSections we have three cases
// 1.) The InputSection has a .ARM.exidx InputSection in its dependent sections.
//     We write the .ARM.exidx section contents and apply its relocations.
// 2.) The InputSection does not have a dependent .ARM.exidx InputSection. We
//     must write the contents of an EXIDX_CANTUNWIND directly. We use the
//     start of the InputSection as the purpose of the linker generated
//     section is to terminate the address range of the previous entry.
// 3.) A trailing EXIDX_CANTUNWIND sentinel section is required at the end of
//     the table to terminate the address range of the final entry.
void ARMExidxSyntheticSection::writeTo(Ctx &ctx, uint8_t *buf) {

  // A linker generated CANTUNWIND entry is made up of two words:
  // 0x0 with R_ARM_PREL31 relocation to target.
  // 0x1 with EXIDX_CANTUNWIND.
  uint64_t offset = 0;
  for (InputSection *isec : executableSections) {
    assert(isec->getParent() != nullptr);
    if (InputSection *d = findExidxSection(isec)) {
      for (int dataOffset = 0; dataOffset != (int)d->content().size();
           dataOffset += 4)
        write32(ctx, buf + offset + dataOffset,
                read32(ctx, d->content().data() + dataOffset));
      // Recalculate outSecOff as finalizeAddressDependentContent()
      // may have altered syntheticSection outSecOff.
      d->outSecOff = offset + outSecOff;
      ctx.target->relocateAlloc(*d, buf + offset);
      offset += d->getSize(ctx);
    } else {
      // A Linker generated CANTUNWIND section.
      write32(ctx, buf + offset + 0, 0x0);
      write32(ctx, buf + offset + 4, 0x1);
      uint64_t s = isec->getVA(ctx);
      uint64_t p = getVA(ctx) + offset;
      ctx.target->relocateNoSym(buf + offset, R_ARM_PREL31, s - p);
      offset += 8;
    }
  }
  // Write Sentinel CANTUNWIND entry.
  write32(ctx, buf + offset + 0, 0x0);
  write32(ctx, buf + offset + 4, 0x1);
  uint64_t s = sentinel->getVA(ctx, sentinel->getSize(ctx));
  uint64_t p = getVA(ctx) + offset;
  ctx.target->relocateNoSym(buf + offset, R_ARM_PREL31, s - p);
  assert(size == offset + 8);
}

bool ARMExidxSyntheticSection::isNeeded(Ctx &ctx) const {
  return llvm::any_of(exidxSections,
                      [](InputSection *isec) { return isec->isLive(); });
}

ThunkSection::ThunkSection(Ctx &ctx, OutputSection *os, uint64_t off)
    : SyntheticSection(ctx, SHF_ALLOC | SHF_EXECINSTR, SHT_PROGBITS,
                       ctx.config->emachine == EM_PPC64 ? 16 : 4,
                       ".text.thunk") {
  this->parent = os;
  this->outSecOff = off;
}

size_t ThunkSection::getSize(Ctx &ctx) const {
  if (roundUpSizeForErrata)
    return alignTo(size, 4096);
  return size;
}

void ThunkSection::addThunk(Thunk *t) {
  thunks.push_back(t);
  t->addSymbols(*this);
}

void ThunkSection::writeTo(Ctx &ctx, uint8_t *buf) {
  for (Thunk *t : thunks)
    t->writeTo(buf + t->offset);
}

InputSection *ThunkSection::getTargetInputSection() const {
  if (thunks.empty())
    return nullptr;
  const Thunk *t = thunks.front();
  return t->getTargetInputSection();
}

bool ThunkSection::assignOffsets() {
  uint64_t off = 0;
  for (Thunk *t : thunks) {
    off = alignToPowerOf2(off, t->alignment);
    t->setOffset(off);
    uint32_t size = t->size();
    t->getThunkTargetSym()->size = size;
    off += size;
  }
  bool changed = off != size;
  size = off;
  return changed;
}

PPC32Got2Section::PPC32Got2Section(Ctx &ctx)
    : SyntheticSection(ctx, SHF_ALLOC | SHF_WRITE, SHT_PROGBITS, 4, ".got2") {}

bool PPC32Got2Section::isNeeded(Ctx &ctx) const {
  // See the comment below. This is not needed if there is no other
  // InputSection.
  for (SectionCommand *cmd : getParent()->commands)
    if (auto *isd = dyn_cast<InputSectionDescription>(cmd))
      for (InputSection *isec : isd->sections)
        if (isec != this)
          return true;
  return false;
}

void PPC32Got2Section::finalizeContents(Ctx &ctx) {
  // PPC32 may create multiple GOT sections for -fPIC/-fPIE, one per file in
  // .got2 . This function computes outSecOff of each .got2 to be used in
  // PPC32PltCallStub::writeTo(). The purpose of this empty synthetic section is
  // to collect input sections named ".got2".
  for (SectionCommand *cmd : getParent()->commands)
    if (auto *isd = dyn_cast<InputSectionDescription>(cmd)) {
      for (InputSection *isec : isd->sections) {
        // isec->file may be nullptr for MergeSyntheticSection.
        if (isec != this && isec->file)
          isec->file->ppc32Got2 = isec;
      }
    }
}

// If linking position-dependent code then the table will store the addresses
// directly in the binary so the section has type SHT_PROGBITS. If linking
// position-independent code the section has type SHT_NOBITS since it will be
// allocated and filled in by the dynamic linker.
PPC64LongBranchTargetSection::PPC64LongBranchTargetSection(Ctx &ctx)
    : SyntheticSection(ctx, SHF_ALLOC | SHF_WRITE,
                       ctx.config->isPic ? SHT_NOBITS : SHT_PROGBITS, 8,
                       ".branch_lt") {}

uint64_t PPC64LongBranchTargetSection::getEntryVA(Ctx &ctx, const Symbol *sym,
                                                  int64_t addend) {
  return getVA(ctx) + entry_index.find({sym, addend})->second * 8;
}

std::optional<uint32_t>
PPC64LongBranchTargetSection::addEntry(const Symbol *sym, int64_t addend) {
  auto res =
      entry_index.try_emplace(std::make_pair(sym, addend), entries.size());
  if (!res.second)
    return std::nullopt;
  entries.emplace_back(sym, addend);
  return res.first->second;
}

size_t PPC64LongBranchTargetSection::getSize(Ctx &ctx) const {
  return entries.size() * 8;
}

void PPC64LongBranchTargetSection::writeTo(Ctx &ctx, uint8_t *buf) {
  // If linking non-pic we have the final addresses of the targets and they get
  // written to the table directly. For pic the dynamic linker will allocate
  // the section and fill it.
  if (ctx.config->isPic)
    return;

  for (auto entry : entries) {
    const Symbol *sym = entry.first;
    int64_t addend = entry.second;
    assert(sym->getVA(ctx));
    // Need calls to branch to the local entry-point since a long-branch
    // must be a local-call.
    write64(ctx, buf,
            sym->getVA(ctx, addend) +
                getPPC64GlobalEntryToLocalEntryOffset(ctx, sym->stOther));
    buf += 8;
  }
}

bool PPC64LongBranchTargetSection::isNeeded(Ctx &ctx) const {
  // `removeUnusedSyntheticSections()` is called before thunk allocation which
  // is too early to determine if this section will be empty or not. We need
  // Finalized to keep the section alive until after thunk creation. Finalized
  // only gets set to true once `finalizeSections()` is called after thunk
  // creation. Because of this, if we don't create any long-branch thunks we end
  // up with an empty .branch_lt section in the binary.
  return !finalized || !entries.empty();
}

static uint8_t getAbiVersion(Ctx &ctx) {
  // MIPS non-PIC executable gets ABI version 1.
  if (ctx.config->emachine == EM_MIPS) {
    if (!ctx.config->isPic && !ctx.config->relocatable &&
        (ctx.config->eflags & (EF_MIPS_PIC | EF_MIPS_CPIC)) == EF_MIPS_CPIC)
      return 1;
    return 0;
  }

  if (ctx.config->emachine == EM_AMDGPU && !ctx.objectFiles.empty()) {
    uint8_t ver = ctx.objectFiles[0]->abiVersion;
    for (InputFile *file : ArrayRef(ctx.objectFiles).slice(1))
      if (file->abiVersion != ver)
        ctx.error("incompatible ABI version: " + toString(ctx, file));
    return ver;
  }

  return 0;
}

template <typename ELFT>
void elf::writeEhdr(Ctx &ctx, uint8_t *buf, Partition &part) {
  memcpy(buf, "\177ELF", 4);

  auto *eHdr = reinterpret_cast<typename ELFT::Ehdr *>(buf);
  eHdr->e_ident[EI_CLASS] = ctx.config->is64 ? ELFCLASS64 : ELFCLASS32;
  eHdr->e_ident[EI_DATA] = ctx.config->isLE ? ELFDATA2LSB : ELFDATA2MSB;
  eHdr->e_ident[EI_VERSION] = EV_CURRENT;
  eHdr->e_ident[EI_OSABI] = ctx.config->osabi;
  eHdr->e_ident[EI_ABIVERSION] = getAbiVersion(ctx);
  eHdr->e_machine = ctx.config->emachine;
  eHdr->e_version = EV_CURRENT;
  eHdr->e_flags = ctx.config->eflags;
  eHdr->e_ehsize = sizeof(typename ELFT::Ehdr);
  eHdr->e_phnum = part.phdrs.size();
  eHdr->e_shentsize = sizeof(typename ELFT::Shdr);

  if (!ctx.config->relocatable) {
    eHdr->e_phoff = sizeof(typename ELFT::Ehdr);
    eHdr->e_phentsize = sizeof(typename ELFT::Phdr);
  }
}

template <typename ELFT> void elf::writePhdrs(uint8_t *buf, Partition &part) {
  // Write the program header table.
  auto *hBuf = reinterpret_cast<typename ELFT::Phdr *>(buf);
  for (PhdrEntry *p : part.phdrs) {
    hBuf->p_type = p->p_type;
    hBuf->p_flags = p->p_flags;
    hBuf->p_offset = p->p_offset;
    hBuf->p_vaddr = p->p_vaddr;
    hBuf->p_paddr = p->p_paddr;
    hBuf->p_filesz = p->p_filesz;
    hBuf->p_memsz = p->p_memsz;
    hBuf->p_align = p->p_align;
    ++hBuf;
  }
}

template <typename ELFT>
PartitionElfHeaderSection<ELFT>::PartitionElfHeaderSection(Ctx &ctx)
    : SyntheticSection(ctx, SHF_ALLOC, SHT_LLVM_PART_EHDR, 1, "") {}

template <typename ELFT>
size_t PartitionElfHeaderSection<ELFT>::getSize(Ctx &ctx) const {
  return sizeof(typename ELFT::Ehdr);
}

template <typename ELFT>
void PartitionElfHeaderSection<ELFT>::writeTo(Ctx &ctx, uint8_t *buf) {
  writeEhdr<ELFT>(ctx, buf, getPartition(ctx));

  // Loadable partitions are always ET_DYN.
  auto *eHdr = reinterpret_cast<typename ELFT::Ehdr *>(buf);
  eHdr->e_type = ET_DYN;
}

template <typename ELFT>
PartitionProgramHeadersSection<ELFT>::PartitionProgramHeadersSection(Ctx &ctx)
    : SyntheticSection(ctx, SHF_ALLOC, SHT_LLVM_PART_PHDR, 1, ".phdrs") {}

template <typename ELFT>
size_t PartitionProgramHeadersSection<ELFT>::getSize(Ctx &ctx) const {
  return sizeof(typename ELFT::Phdr) * getPartition(ctx).phdrs.size();
}

template <typename ELFT>
void PartitionProgramHeadersSection<ELFT>::writeTo(Ctx &ctx, uint8_t *buf) {
  writePhdrs<ELFT>(buf, getPartition(ctx));
}

PartitionIndexSection::PartitionIndexSection(Ctx &ctx)
    : SyntheticSection(ctx, SHF_ALLOC, SHT_PROGBITS, 4, ".rodata") {}

size_t PartitionIndexSection::getSize(Ctx &ctx) const {
  return 12 * (ctx.partitions.size() - 1);
}

void PartitionIndexSection::finalizeContents(Ctx &ctx) {
  for (size_t i = 1; i != ctx.partitions.size(); ++i)
    ctx.partitions[i].nameStrTab =
        ctx.mainPart->dynStrTab->addString(ctx.partitions[i].name);
}

void PartitionIndexSection::writeTo(Ctx &ctx, uint8_t *buf) {
  uint64_t va = getVA(ctx);
  for (size_t i = 1; i != ctx.partitions.size(); ++i) {
    write32(ctx, buf,
            ctx.mainPart->dynStrTab->getVA(ctx) + ctx.partitions[i].nameStrTab -
                va);
    write32(ctx, buf + 4, ctx.partitions[i].elfHeader->getVA(ctx) - (va + 4));

    SyntheticSection *next = i == ctx.partitions.size() - 1
                                 ? ctx.in.partEnd.get()
                                 : ctx.partitions[i + 1].elfHeader.get();
    write32(ctx, buf + 8,
            next->getVA(ctx) - ctx.partitions[i].elfHeader->getVA(ctx));

    va += 12;
    buf += 12;
  }
}

void InStruct::reset() {
  attributes.reset();
  riscvAttributes.reset();
  bss.reset();
  bssRelRo.reset();
  got.reset();
  gotPlt.reset();
  igotPlt.reset();
  relroPadding.reset();
  armCmseSGSection.reset();
  ppc64LongBranchTarget.reset();
  mipsAbiFlags.reset();
  mipsGot.reset();
  mipsOptions.reset();
  mipsReginfo.reset();
  mipsRldMap.reset();
  partEnd.reset();
  partIndex.reset();
  plt.reset();
  iplt.reset();
  ppc32Got2.reset();
  ibtPlt.reset();
  relaPlt.reset();
  relaIplt.reset();
  shStrTab.reset();
  strTab.reset();
  symTab.reset();
  symTabShndx.reset();
}

constexpr char kMemtagAndroidNoteName[] = "Android";
void MemtagAndroidNote::writeTo(Ctx &ctx, uint8_t *buf) {
  static_assert(
      sizeof(kMemtagAndroidNoteName) == 8,
      "Android 11 & 12 have an ABI that the note name is 8 bytes long. Keep it "
      "that way for backwards compatibility.");

  write32(ctx, buf, sizeof(kMemtagAndroidNoteName));
  write32(ctx, buf + 4, sizeof(uint32_t));
  write32(ctx, buf + 8, ELF::NT_ANDROID_TYPE_MEMTAG);
  memcpy(buf + 12, kMemtagAndroidNoteName, sizeof(kMemtagAndroidNoteName));
  buf += 12 + alignTo(sizeof(kMemtagAndroidNoteName), 4);

  uint32_t value = 0;
  value |= ctx.config->androidMemtagMode;
  if (ctx.config->androidMemtagHeap)
    value |= ELF::NT_MEMTAG_HEAP;
  // Note, MTE stack is an ABI break. Attempting to run an MTE stack-enabled
  // binary on Android 11 or 12 will result in a checkfail in the loader.
  if (ctx.config->androidMemtagStack)
    value |= ELF::NT_MEMTAG_STACK;
  write32(ctx, buf, value); // note value
}

size_t MemtagAndroidNote::getSize(Ctx &ctx) const {
  return sizeof(llvm::ELF::Elf64_Nhdr) +
         /*namesz=*/alignTo(sizeof(kMemtagAndroidNoteName), 4) +
         /*descsz=*/sizeof(uint32_t);
}

void PackageMetadataNote::writeTo(Ctx &ctx, uint8_t *buf) {
  write32(ctx, buf, 4);
  write32(ctx, buf + 4, ctx.config->packageMetadata.size() + 1);
  write32(ctx, buf + 8, FDO_PACKAGING_METADATA);
  memcpy(buf + 12, "FDO", 4);
  memcpy(buf + 16, ctx.config->packageMetadata.data(),
         ctx.config->packageMetadata.size());
}

size_t PackageMetadataNote::getSize(Ctx &ctx) const {
  return sizeof(llvm::ELF::Elf64_Nhdr) + 4 +
         alignTo(ctx.config->packageMetadata.size() + 1, 4);
}

// Helper function, return the size of the ULEB128 for 'v', optionally writing
// it to `*(buf + offset)` if `buf` is non-null.
static size_t computeOrWriteULEB128(uint64_t v, uint8_t *buf, size_t offset) {
  if (buf)
    return encodeULEB128(v, buf + offset);
  return getULEB128Size(v);
}

// https://github.com/ARM-software/abi-aa/blob/main/memtagabielf64/memtagabielf64.rst#83encoding-of-sht_aarch64_memtag_globals_dynamic
constexpr uint64_t kMemtagStepSizeBits = 3;
constexpr uint64_t kMemtagGranuleSize = 16;
static size_t
createMemtagGlobalDescriptors(Ctx &ctx,
                              const SmallVector<const Symbol *, 0> &symbols,
                              uint8_t *buf = nullptr) {
  size_t sectionSize = 0;
  uint64_t lastGlobalEnd = 0;

  for (const Symbol *sym : symbols) {
    if (!includeInSymtab(ctx, *sym))
      continue;
    const uint64_t addr = sym->getVA(ctx);
    const uint64_t size = sym->getSize();

    if (addr <= kMemtagGranuleSize && buf != nullptr)
      errorOrWarn(ctx,
                  "address of the tagged symbol \"" + sym->getName() +
                      "\" falls in the ELF header. This is indicative of a "
                      "compiler/linker bug");
    if (addr % kMemtagGranuleSize != 0)
      errorOrWarn(ctx, "address of the tagged symbol \"" + sym->getName() +
                           "\" at 0x" + Twine::utohexstr(addr) +
                           "\" is not granule (16-byte) aligned");
    if (size == 0)
      errorOrWarn(ctx, "size of the tagged symbol \"" + sym->getName() +
                           "\" is not allowed to be zero");
    if (size % kMemtagGranuleSize != 0)
      errorOrWarn(ctx, "size of the tagged symbol \"" + sym->getName() +
                           "\" (size 0x" + Twine::utohexstr(size) +
                           ") is not granule (16-byte) aligned");

    const uint64_t sizeToEncode = size / kMemtagGranuleSize;
    const uint64_t stepToEncode = ((addr - lastGlobalEnd) / kMemtagGranuleSize)
                                  << kMemtagStepSizeBits;
    if (sizeToEncode < (1 << kMemtagStepSizeBits)) {
      sectionSize +=
          computeOrWriteULEB128(stepToEncode | sizeToEncode, buf, sectionSize);
    } else {
      sectionSize += computeOrWriteULEB128(stepToEncode, buf, sectionSize);
      sectionSize += computeOrWriteULEB128(sizeToEncode - 1, buf, sectionSize);
    }
    lastGlobalEnd = addr + size;
  }

  return sectionSize;
}

bool MemtagGlobalDescriptors::updateAllocSize(Ctx &ctx) {
  size_t oldSize = getSize(ctx);
  std::stable_sort(symbols.begin(), symbols.end(),
                   [&ctx = ctx](const Symbol *s1, const Symbol *s2) {
                     return s1->getVA(ctx) < s2->getVA(ctx);
                   });
  return oldSize != getSize(ctx);
}

void MemtagGlobalDescriptors::writeTo(Ctx &ctx, uint8_t *buf) {
  createMemtagGlobalDescriptors(ctx, symbols, buf);
}

size_t MemtagGlobalDescriptors::getSize(Ctx &ctx) const {
  return createMemtagGlobalDescriptors(ctx, symbols);
}

template GdbIndexSection *GdbIndexSection::create<ELF32LE>(Ctx &ctx);
template GdbIndexSection *GdbIndexSection::create<ELF32BE>(Ctx &ctx);
template GdbIndexSection *GdbIndexSection::create<ELF64LE>(Ctx &ctx);
template GdbIndexSection *GdbIndexSection::create<ELF64BE>(Ctx &ctx);

template void elf::splitSections<ELF32LE>(Ctx &ctx);
template void elf::splitSections<ELF32BE>(Ctx &ctx);
template void elf::splitSections<ELF64LE>(Ctx &ctx);
template void elf::splitSections<ELF64BE>(Ctx &ctx);

template class elf::MipsAbiFlagsSection<ELF32LE>;
template class elf::MipsAbiFlagsSection<ELF32BE>;
template class elf::MipsAbiFlagsSection<ELF64LE>;
template class elf::MipsAbiFlagsSection<ELF64BE>;

template class elf::MipsOptionsSection<ELF32LE>;
template class elf::MipsOptionsSection<ELF32BE>;
template class elf::MipsOptionsSection<ELF64LE>;
template class elf::MipsOptionsSection<ELF64BE>;

template void
EhFrameSection::iterateFDEWithLSDA<ELF32LE>(Ctx &ctx,
                                            function_ref<void(InputSection &)>);
template void
EhFrameSection::iterateFDEWithLSDA<ELF32BE>(Ctx &ctx,
                                            function_ref<void(InputSection &)>);
template void
EhFrameSection::iterateFDEWithLSDA<ELF64LE>(Ctx &ctx,
                                            function_ref<void(InputSection &)>);
template void
EhFrameSection::iterateFDEWithLSDA<ELF64BE>(Ctx &ctx,
                                            function_ref<void(InputSection &)>);

template class elf::MipsReginfoSection<ELF32LE>;
template class elf::MipsReginfoSection<ELF32BE>;
template class elf::MipsReginfoSection<ELF64LE>;
template class elf::MipsReginfoSection<ELF64BE>;

template class elf::DynamicSection<ELF32LE>;
template class elf::DynamicSection<ELF32BE>;
template class elf::DynamicSection<ELF64LE>;
template class elf::DynamicSection<ELF64BE>;

template class elf::RelocationSection<ELF32LE>;
template class elf::RelocationSection<ELF32BE>;
template class elf::RelocationSection<ELF64LE>;
template class elf::RelocationSection<ELF64BE>;

template class elf::AndroidPackedRelocationSection<ELF32LE>;
template class elf::AndroidPackedRelocationSection<ELF32BE>;
template class elf::AndroidPackedRelocationSection<ELF64LE>;
template class elf::AndroidPackedRelocationSection<ELF64BE>;

template class elf::RelrSection<ELF32LE>;
template class elf::RelrSection<ELF32BE>;
template class elf::RelrSection<ELF64LE>;
template class elf::RelrSection<ELF64BE>;

template class elf::SymbolTableSection<ELF32LE>;
template class elf::SymbolTableSection<ELF32BE>;
template class elf::SymbolTableSection<ELF64LE>;
template class elf::SymbolTableSection<ELF64BE>;

template class elf::VersionNeedSection<ELF32LE>;
template class elf::VersionNeedSection<ELF32BE>;
template class elf::VersionNeedSection<ELF64LE>;
template class elf::VersionNeedSection<ELF64BE>;

template void elf::writeEhdr<ELF32LE>(Ctx &ctx, uint8_t *Buf, Partition &Part);
template void elf::writeEhdr<ELF32BE>(Ctx &ctx, uint8_t *Buf, Partition &Part);
template void elf::writeEhdr<ELF64LE>(Ctx &ctx, uint8_t *Buf, Partition &Part);
template void elf::writeEhdr<ELF64BE>(Ctx &ctx, uint8_t *Buf, Partition &Part);

template void elf::writePhdrs<ELF32LE>(uint8_t *Buf, Partition &Part);
template void elf::writePhdrs<ELF32BE>(uint8_t *Buf, Partition &Part);
template void elf::writePhdrs<ELF64LE>(uint8_t *Buf, Partition &Part);
template void elf::writePhdrs<ELF64BE>(uint8_t *Buf, Partition &Part);

template class elf::PartitionElfHeaderSection<ELF32LE>;
template class elf::PartitionElfHeaderSection<ELF32BE>;
template class elf::PartitionElfHeaderSection<ELF64LE>;
template class elf::PartitionElfHeaderSection<ELF64BE>;

template class elf::PartitionProgramHeadersSection<ELF32LE>;
template class elf::PartitionProgramHeadersSection<ELF32BE>;
template class elf::PartitionProgramHeadersSection<ELF64LE>;
template class elf::PartitionProgramHeadersSection<ELF64BE>;
