//===- InputElement.cpp ---------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "InputElement.h"
#include "Ctx.h"

namespace lld {
namespace wasm {

InputElement::InputElement(Ctx&ctx,StringRef name, ObjFile *f)
    : file(f), live(!ctx.config->gcSections), name(name) {}

void InputGlobal::setPointerValue(Ctx&ctx,uint64_t value) {
  initExpr = intConst(value, ctx.config->is64.value_or(false));
}

} // namespace wasm
} // namespace lld