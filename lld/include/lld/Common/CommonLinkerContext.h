//===- CommonLinkerContext.h ------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Entry point for all global state in lldCommon. The objective is for LLD to be
// used "as a library" in a thread-safe manner.
//
// Instead of program-wide globals or function-local statics, we prefer
// aggregating all "global" states into a heap-based structure
// (CommonLinkerContext). This also achieves deterministic initialization &
// shutdown for all "global" states.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_COMMON_COMMONLINKINGCONTEXT_H
#define LLD_COMMON_COMMONLINKINGCONTEXT_H

#include "lld/Common/ErrorHandler.h"
#include "lld/Common/Memory.h"
#include "llvm/Support/StringSaver.h"
#include <mutex>

namespace llvm {
class raw_ostream;
} // namespace llvm

namespace lld {
struct SpecificAllocBase;
class CommonLinkerContext {
public:
  CommonLinkerContext();
  virtual ~CommonLinkerContext();

  llvm::BumpPtrAllocator bAlloc;
  llvm::StringSaver saver{bAlloc};
  llvm::DenseMap<void *, SpecificAllocBase *> instances;

  ErrorHandler e;

  std::mutex makeMtx;

  void error(const Twine &msg) { e.error(msg); }
  void error(const Twine &msg, ErrorTag tag, ArrayRef<StringRef> args) {
    e.error(msg, tag, args);
  }
  void fatal(const Twine &msg) { e.fatal(msg); }
  void log(const Twine &msg) { e.log(msg); }
  void message(const Twine &msg) { e.message(msg, e.outs()); }
  void message(const Twine &msg, llvm::raw_ostream &s) { e.message(msg, s); }
  void warn(const Twine &msg) { e.warn(msg); }
  uint64_t errorCount() { return e.errorCount; }

  raw_ostream &outs() { return e.outs(); }
  raw_ostream &errs() { return e.errs(); }

  // Creates new instances of T off a (almost) contiguous arena/object pool. The
  // instances are destroyed whenever lldMain() goes out of scope.
  template <typename T, typename... U> T *make(U &&...args) {
    T* ptr;

    {
      std::lock_guard lock(makeMtx);
      ptr = getSpecificAllocSingleton<T>(*this).Allocate();
    }

    return new (ptr) T(std::forward<U>(args)...);
  }

  template <typename T> T *makeN(size_t n) {
    T* ptr;

    {
      std::lock_guard lock(makeMtx);
      ptr = getSpecificAllocSingleton<T>(*this).Allocate(n);
    }

    return new (ptr) T[n];
  }
};

// check functions are convenient functions to strip errors
// from error-or-value objects.
template <class T> T check(CommonLinkerContext &ctx, ErrorOr<T> e) {
  if (auto ec = e.getError())
    ctx.fatal(ec.message());
  return std::move(*e);
}

template <class T> T check(CommonLinkerContext &ctx, Expected<T> e) {
  if (!e)
    ctx.fatal(llvm::toString(e.takeError()));
  return std::move(*e);
}

// Don't move from Expected wrappers around references.
template <class T> T &check(CommonLinkerContext &ctx, Expected<T &> e) {
  if (!e)
    ctx.fatal(llvm::toString(e.takeError()));
  return *e;
}

template <class T>
T check2(CommonLinkerContext &ctx, ErrorOr<T> e, llvm::function_ref<std::string()> prefix) {
  if (auto ec = e.getError())
    ctx.fatal(prefix() + ": " + ec.message());
  return std::move(*e);
}

template <class T>
T check2(CommonLinkerContext &ctx, Expected<T> e, llvm::function_ref<std::string()> prefix) {
  if (!e)
    ctx.fatal(prefix() + ": " + toString(e.takeError()));
  return std::move(*e);
}
} // namespace lld

#endif
