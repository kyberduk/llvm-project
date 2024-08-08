//===- Symbols.cpp --------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Symbols.h"
#include "Ctx.h"
#include "InputFiles.h"
#include "SyntheticSections.h"
#include "llvm/Demangle/Demangle.h"

using namespace llvm;
using namespace lld;
using namespace lld::macho;

static_assert(sizeof(void *) != 8 || sizeof(Symbol) == 64,
              "Try to minimize Symbol's size; we create many instances");

// The Microsoft ABI doesn't support using parent class tail padding for child
// members, hence the _MSC_VER check.
#if !defined(_MSC_VER)
static_assert(sizeof(void *) != 8 || sizeof(Defined) == 96,
              "Try to minimize Defined's size; we create many instances");
#endif

static_assert(sizeof(SymbolUnion) == sizeof(Defined),
              "Defined should be the largest Symbol kind");

// Returns a symbol name for an error message.
static std::string maybeDemangleSymbol(Ctx &ctx, StringRef symName) {
  if (ctx.config->demangle) {
    symName.consume_front("_");
    return demangle(symName);
  }
  return symName.str();
}

std::string lld::toString(const Symbol &sym) {
  return maybeDemangleSymbol(sym.ctx, sym.getName());
}

std::string lld::toMachOString(Ctx&ctx,const object::Archive::Symbol &b) {
  return maybeDemangleSymbol(ctx,b.getName());
}

uint64_t Symbol::getStubVA() const { return ctx.in.stubs->getVA(stubsIndex); }
uint64_t Symbol::getLazyPtrVA() const {
  return ctx.in.lazyPointers->getVA(stubsIndex);
}
uint64_t Symbol::getGotVA() const { return ctx.in.got->getVA(gotIndex); }
uint64_t Symbol::getTlvVA() const {
  return ctx.in.tlvPointers->getVA(gotIndex);
}

Symbol::Symbol(Ctx &c, Kind k, StringRefZ name, InputFile *file)
    : ctx(c), symbolKind(k), nameData(name.data), file(file),
      nameSize(name.size), isUsedInRegularObj(!file || isa<ObjFile>(file)),
      used(!ctx.config->deadStrip) {}

Defined::Defined(Ctx&ctx,StringRefZ name, InputFile *file, InputSection *isec,
                 uint64_t value, uint64_t size, bool isWeakDef, bool isExternal,
                 bool isPrivateExtern, bool includeInSymtab,
                 bool isReferencedDynamically, bool noDeadStrip,
                 bool canOverrideWeakDef, bool isWeakDefCanBeHidden,
                 bool interposable)
    : Symbol(ctx,DefinedKind, name, file), overridesWeakDef(canOverrideWeakDef),
      privateExtern(isPrivateExtern), includeInSymtab(includeInSymtab),
      wasIdenticalCodeFolded(false),
      referencedDynamically(isReferencedDynamically), noDeadStrip(noDeadStrip),
      interposable(interposable), weakDefCanBeHidden(isWeakDefCanBeHidden),
      weakDef(isWeakDef), external(isExternal), isec(isec), value(value),
      size(size) {
  if (isec) {
    isec->symbols.push_back(this);
    // Maintain sorted order.
    for (auto it = isec->symbols.rbegin(), rend = isec->symbols.rend();
         it != rend; ++it) {
      auto next = std::next(it);
      if (next == rend)
        break;
      if ((*it)->value < (*next)->value)
        std::swap(*next, *it);
      else
        break;
    }
  }
}

bool Defined::isTlv() const {
  return !isAbsolute() && isThreadLocalVariables(isec->getFlags());
}

uint64_t Defined::getVA() const {
  assert(isLive() && "this should only be called for live symbols");

  if (isAbsolute())
    return value;

  if (!isec->isFinal) {
    // A target arch that does not use thunks ought never ask for
    // the address of a function that has not yet been finalized.
    assert(ctx.target->usesThunks());

    // ConcatOutputSection::finalize() can seek the address of a
    // function before its address is assigned. The thunking algorithm
    // knows that unfinalized functions will be out of range, so it is
    // expedient to return a contrived out-of-range address.
    return TargetInfo::outOfRangeVA;
  }
  return isec->getVA(value);
}

ObjFile *Defined::getObjectFile() const {
  return isec ? dyn_cast_or_null<ObjFile>(isec->getFile()) : nullptr;
}

void Defined::canonicalize() {
  if (unwindEntry)
    unwindEntry = unwindEntry->canonical();
  if (isec)
    isec = isec->canonical();
}

std::string Defined::getSourceLocation() {
  if (!isec)
    return {};
  return isec->getSourceLocation(value);
}

uint64_t DylibSymbol::getVA() const {
  return isInStubs() ? getStubVA() : Symbol::getVA();
}

void LazyArchive::fetchArchiveMember() { getFile()->fetch(sym); }
