//===- Target.cpp ---------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Target.h"
#include "Ctx.h"

using namespace lld;
using namespace lld::macho;

const RelocAttrs &TargetInfo::getRelocAttrs(uint8_t type) const {
  assert(type < relocAttrs.size() && "invalid relocation type");
  if (type >= relocAttrs.size())
    return ctx.invalidRelocAttrs;
  return relocAttrs[type];
}
