/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/loongarch64/Architecture-loongarch64.h"

#include <cstring>

#ifdef JS_SIMULATOR
# include "jit/loongarch64/Simulator-loongarch64.h"
#endif
#include "jit/RegisterSets.h"

namespace js {
namespace jit {

static uint32_t CountPhysBits(FloatRegisters::SetType bits) {
  MOZ_ASSERT((bits & ~FloatRegisters::AllPhysMask) == 0);
  return mozilla::CountPopulation32(uint32_t(bits));
}

Registers::Code Registers::FromName(const char* name) {
  for (size_t i = 0; i < TotalPhys; i++) {
    if (strcmp(GetName(i), name) == 0) {
      return Code(i);
    }
  }

  return Invalid;
}

FloatRegisters::Code FloatRegisters::FromName(const char* name) {
  for (size_t i = 0; i < TotalPhys; i++) {
    if (strcmp(GetName(i), name) == 0) {
      return Code(i);
    }
  }

  return Invalid;
}

FloatRegisterSet FloatRegister::ReduceSetForPush(const FloatRegisterSet& s) {
  SetType bits = s.bits();

  // Exclude registers already represented by a wider alias. Higher alias banks
  // are wider, so preserve only the widest live view of each physical register.
  bits &= ~(bits >> (1 * FloatRegisters::TotalPhys));
  bits &= ~(bits >> (2 * FloatRegisters::TotalPhys));

  return FloatRegisterSet(bits);
}

uint32_t FloatRegister::GetSizeInBytes(const FloatRegisterSet& s) {
  return s.size() * sizeof(FloatRegisters::RegisterContent);
}

uint32_t FloatRegister::GetPushSizeInBytes(const FloatRegisterSet& s) {
  SetType all = s.bits();
  SetType set128b =
      (all >> (uint32_t(FloatRegisters::Simd128) * FloatRegisters::TotalPhys)) &
      FloatRegisters::AllPhysMask;
  SetType doubleSet =
      (all >> (uint32_t(FloatRegisters::Double) * FloatRegisters::TotalPhys)) &
      FloatRegisters::AllPhysMask;
  SetType singleSet =
      (all >> (uint32_t(FloatRegisters::Single) * FloatRegisters::TotalPhys)) &
      FloatRegisters::AllPhysMask;

  SetType set64b = doubleSet & ~set128b;
  SetType set32b = singleSet & ~set64b & ~set128b;

  return CountPhysBits(set128b) * (4 * sizeof(int32_t)) +
         CountPhysBits(set64b) * sizeof(double) +
         CountPhysBits(set32b) * sizeof(float);
}

uint32_t FloatRegister::getRegisterDumpOffsetInBytes() {
  return encoding() * sizeof(FloatRegisters::RegisterContent);
}

bool CPUFlagsHaveBeenComputed() {
  // The LoongArch64 backend does not currently gate code generation on
  // optional CPU features.
  return true;
}

uint32_t GetLOONGARCH64Flags() { return 0; }

void FlushICache(void* code, size_t size, bool codeIsThreadLocal) {
#if defined(JS_SIMULATOR)
  js::jit::SimulatorProcess::FlushICache(code, size);

#elif defined(__GNUC__)
  intptr_t end = reinterpret_cast<intptr_t>(code) + size;
  __builtin___clear_cache(reinterpret_cast<char*>(code),
                          reinterpret_cast<char*>(end));

#else
  _flush_cache(reinterpret_cast<char*>(code), size, BCACHE);

#endif
}

}  // namespace jit
}  // namespace js
