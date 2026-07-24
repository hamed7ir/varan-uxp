/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/arm/Assembler-arm.h"

#include "mozilla/DebugOnly.h"
#include "mozilla/MathAlgorithms.h"

#include "jscompartment.h"
#ifdef JS_DISASM_ARM
#include "jsprf.h"
#endif
#include "jsutil.h"

#include "gc/Marking.h"
#include "jit/arm/disasm/Disasm-arm.h"
#include "jit/arm/MacroAssembler-arm.h"
#include "jit/ExecutableAllocator.h"
#include "jit/JitCompartment.h"
#include "jit/MacroAssembler.h"

using namespace js;
using namespace js::jit;

using mozilla::CountLeadingZeroes32;

void dbg_break() {}

// The ABIArgGenerator is used for making system ABI calls and for inter-wasm
// calls. The system ABI can either be SoftFp or HardFp, and inter-wasm calls
// are always HardFp calls. The initialization defaults to HardFp, and the ABI
// choice is made before any system ABI calls with the method "setUseHardFp".
ABIArgGenerator::ABIArgGenerator()
  : intRegIndex_(0),
    floatRegIndex_(0),
    stackOffset_(0),
    current_(),
    useHardFp_(true)
{ }

// See the "Parameter Passing" section of the "Procedure Call Standard for the
// ARM Architecture" documentation.
ABIArg
ABIArgGenerator::softNext(MIRType type)
{
    switch (type) {
      case MIRType::Int32:
      case MIRType::Pointer:
        if (intRegIndex_ == NumIntArgRegs) {
            current_ = ABIArg(stackOffset_);
            stackOffset_ += sizeof(uint32_t);
            break;
        }
        current_ = ABIArg(Register::FromCode(intRegIndex_));
        intRegIndex_++;
        break;
      case MIRType::Int64:
        // Make sure to use an even register index. Increase to next even number
        // when odd.
        intRegIndex_ = (intRegIndex_ + 1) & ~1;
        if (intRegIndex_ == NumIntArgRegs) {
            // Align the stack on 8 bytes.
            static const uint32_t align = sizeof(uint64_t) - 1;
            stackOffset_ = (stackOffset_ + align) & ~align;
            current_ = ABIArg(stackOffset_);
            stackOffset_ += sizeof(uint64_t);
            break;
        }
        current_ = ABIArg(Register::FromCode(intRegIndex_), Register::FromCode(intRegIndex_ + 1));
        intRegIndex_ += 2;
        break;
      case MIRType::Float32:
        if (intRegIndex_ == NumIntArgRegs) {
            current_ = ABIArg(stackOffset_);
            stackOffset_ += sizeof(uint32_t);
            break;
        }
        current_ = ABIArg(Register::FromCode(intRegIndex_));
        intRegIndex_++;
        break;
      case MIRType::Double:
        // Make sure to use an even register index. Increase to next even number
        // when odd.
        intRegIndex_ = (intRegIndex_ + 1) & ~1;
        if (intRegIndex_ == NumIntArgRegs) {
            // Align the stack on 8 bytes.
            static const uint32_t align = sizeof(double) - 1;
            stackOffset_ = (stackOffset_ + align) & ~align;
            current_ = ABIArg(stackOffset_);
            stackOffset_ += sizeof(double);
            break;
        }
        current_ = ABIArg(Register::FromCode(intRegIndex_), Register::FromCode(intRegIndex_ + 1));
        intRegIndex_ += 2;
        break;
      default:
        MOZ_CRASH("Unexpected argument type");
    }

    return current_;
}

ABIArg
ABIArgGenerator::hardNext(MIRType type)
{
    switch (type) {
      case MIRType::Int32:
      case MIRType::Pointer:
        if (intRegIndex_ == NumIntArgRegs) {
            current_ = ABIArg(stackOffset_);
            stackOffset_ += sizeof(uint32_t);
            break;
        }
        current_ = ABIArg(Register::FromCode(intRegIndex_));
        intRegIndex_++;
        break;
      case MIRType::Int64:
        // Make sure to use an even register index. Increase to next even number
        // when odd.
        intRegIndex_ = (intRegIndex_ + 1) & ~1;
        if (intRegIndex_ == NumIntArgRegs) {
            // Align the stack on 8 bytes.
            static const uint32_t align = sizeof(uint64_t) - 1;
            stackOffset_ = (stackOffset_ + align) & ~align;
            current_ = ABIArg(stackOffset_);
            stackOffset_ += sizeof(uint64_t);
            break;
        }
        current_ = ABIArg(Register::FromCode(intRegIndex_), Register::FromCode(intRegIndex_ + 1));
        intRegIndex_ += 2;
        break;
      case MIRType::Float32:
        if (floatRegIndex_ == NumFloatArgRegs) {
            current_ = ABIArg(stackOffset_);
            stackOffset_ += sizeof(uint32_t);
            break;
        }
        current_ = ABIArg(VFPRegister(floatRegIndex_, VFPRegister::Single));
        floatRegIndex_++;
        break;
      case MIRType::Double:
        // Double register are composed of 2 float registers, thus we have to
        // skip any float register which cannot be used in a pair of float
        // registers in which a double value can be stored.
        floatRegIndex_ = (floatRegIndex_ + 1) & ~1;
        if (floatRegIndex_ == NumFloatArgRegs) {
            static const uint32_t align = sizeof(double) - 1;
            stackOffset_ = (stackOffset_ + align) & ~align;
            current_ = ABIArg(stackOffset_);
            stackOffset_ += sizeof(uint64_t);
            break;
        }
        current_ = ABIArg(VFPRegister(floatRegIndex_ >> 1, VFPRegister::Double));
        floatRegIndex_ += 2;
        break;
      default:
        MOZ_CRASH("Unexpected argument type");
    }

    return current_;
}

ABIArg
ABIArgGenerator::next(MIRType type)
{
    if (useHardFp_)
        return hardNext(type);
    return softNext(type);
}

bool
js::jit::IsUnaligned(const wasm::MemoryAccessDesc& access)
{
    if (!access.align())
        return false;

    if (access.type() == Scalar::Float64 && access.align() >= 4)
        return false;

    return access.align() < access.byteSize();
}

// Encode a standard register when it is being used as src1, the dest, and an
// extra register. These should never be called with an InvalidReg.
uint32_t
js::jit::RT(Register r)
{
    MOZ_ASSERT((r.code() & ~0xf) == 0);
    return r.code() << 12;
}

uint32_t
js::jit::RN(Register r)
{
    MOZ_ASSERT((r.code() & ~0xf) == 0);
    return r.code() << 16;
}

uint32_t
js::jit::RD(Register r)
{
    MOZ_ASSERT((r.code() & ~0xf) == 0);
    return r.code() << 12;
}

uint32_t
js::jit::RM(Register r)
{
    MOZ_ASSERT((r.code() & ~0xf) == 0);
    return r.code() << 8;
}

// Encode a standard register when it is being used as src1, the dest, and an
// extra register. For these, an InvalidReg is used to indicate a optional
// register that has been omitted.
uint32_t
js::jit::maybeRT(Register r)
{
    if (r == InvalidReg)
        return 0;

    MOZ_ASSERT((r.code() & ~0xf) == 0);
    return r.code() << 12;
}

uint32_t
js::jit::maybeRN(Register r)
{
    if (r == InvalidReg)
        return 0;

    MOZ_ASSERT((r.code() & ~0xf) == 0);
    return r.code() << 16;
}

uint32_t
js::jit::maybeRD(Register r)
{
    if (r == InvalidReg)
        return 0;

    MOZ_ASSERT((r.code() & ~0xf) == 0);
    return r.code() << 12;
}

Register
js::jit::toRD(Instruction i)
{
    return Register::FromCode((i.encode() >> 12) & 0xf);
}
Register
js::jit::toR(Instruction i)
{
#if defined(VARAN_THUMB2)
    // T2 register branch (BX/BLX packed in the high halfword): Rm is in hw1 bits[6:3] = (enc>>19)&0xf.
    return Register::FromCode((i.encode() >> 19) & 0xf);
#else
    return Register::FromCode(i.encode() & 0xf);
#endif
}

Register
js::jit::toRM(Instruction i)
{
    return Register::FromCode((i.encode() >> 8) & 0xf);
}

Register
js::jit::toRN(Instruction i)
{
    return Register::FromCode((i.encode() >> 16) & 0xf);
}

uint32_t
js::jit::VD(VFPRegister vr)
{
    if (vr.isMissing())
        return 0;

    // Bits 15,14,13,12, 22.
    VFPRegister::VFPRegIndexSplit s = vr.encode();
    return s.bit << 22 | s.block << 12;
}
uint32_t
js::jit::VN(VFPRegister vr)
{
    if (vr.isMissing())
        return 0;

    // Bits 19,18,17,16, 7.
    VFPRegister::VFPRegIndexSplit s = vr.encode();
    return s.bit << 7 | s.block << 16;
}
uint32_t
js::jit::VM(VFPRegister vr)
{
    if (vr.isMissing())
        return 0;

    // Bits 5, 3,2,1,0.
    VFPRegister::VFPRegIndexSplit s = vr.encode();
    return s.bit << 5 | s.block;
}

VFPRegister::VFPRegIndexSplit
jit::VFPRegister::encode()
{
    MOZ_ASSERT(!_isInvalid);

    switch (kind) {
      case Double:
        return VFPRegIndexSplit(code_ & 0xf, code_ >> 4);
      case Single:
        return VFPRegIndexSplit(code_ >> 1, code_ & 1);
      default:
        // VFP register treated as an integer, NOT a gpr.
        return VFPRegIndexSplit(code_ >> 1, code_ & 1);
    }
}

bool
InstDTR::IsTHIS(const Instruction& i)
{
#if defined(VARAN_THUMB2)
    // The only DTR VARAN patches in place is the pc-relative pool LDR-literal (ldr.w Rt,[pc,#imm],
    // hw0 = 0xF85F/0xF8DF). General ldr/str go through varanEmitDtr and are never round-tripped as
    // InstDTR. Match on hw0 (low 16 bits) with the U bit (0x80) masked out.
    return (i.encode() & 0x0000ff7fu) == 0x0000f85fu;
#else
    return (i.encode() & IsDTRMask) == (uint32_t)IsDTR;
#endif
}

InstDTR*
InstDTR::AsTHIS(const Instruction& i)
{
    if (IsTHIS(i))
        return (InstDTR*)&i;
    return nullptr;
}

bool
InstLDR::IsTHIS(const Instruction& i)
{
#if defined(VARAN_THUMB2)
    return (i.encode() & 0x0000ff7fu) == 0x0000f85fu;   // T2 ldr.w Rt,[pc,#imm] literal
#else
    return (i.encode() & IsDTRMask) == (uint32_t)IsDTR;
#endif
}

InstLDR*
InstLDR::AsTHIS(const Instruction& i)
{
    if (IsTHIS(i))
        return (InstLDR*)&i;
    return nullptr;
}

InstNOP*
InstNOP::AsTHIS(Instruction& i)
{
    if (IsTHIS(i))
        return (InstNOP*)&i;
    return nullptr;
}

bool
InstNOP::IsTHIS(const Instruction& i)
{
#if defined(VARAN_THUMB2)
    return i.encode() == NopInst;   // exact NOP.W word 0x8000F3AF (no A32 cond mask)
#else
    return (i.encode() & 0x0fffffff) == NopInst;
#endif
}

bool
InstBranchReg::IsTHIS(const Instruction& i)
{
    return InstBXReg::IsTHIS(i) || InstBLXReg::IsTHIS(i);
}

InstBranchReg*
InstBranchReg::AsTHIS(const Instruction& i)
{
    if (IsTHIS(i))
        return (InstBranchReg*)&i;
    return nullptr;
}
void
InstBranchReg::extractDest(Register* dest)
{
    *dest = toR(*this);
}
bool
InstBranchReg::checkDest(Register dest)
{
    return dest == toR(*this);
}

bool
InstBranchImm::IsTHIS(const Instruction& i)
{
    return InstBImm::IsTHIS(i) || InstBLImm::IsTHIS(i);
}

InstBranchImm*
InstBranchImm::AsTHIS(const Instruction& i)
{
    if (IsTHIS(i))
        return (InstBranchImm*)&i;
    return nullptr;
}

void
InstBranchImm::extractImm(BOffImm* dest)
{
    *dest = BOffImm(*this);
}

bool
InstBXReg::IsTHIS(const Instruction& i)
{
#if defined(VARAN_THUMB2)
    // Packed T2 BX: high halfword = 0x4700|(Rm<<3), low = 0xbf00. Mask out the Rm field (bits 6:3).
    return ((i.encode() >> 16) & 0xff87u) == 0x4700u;
#else
    return (i.encode() & IsBRegMask) == IsBX;
#endif
}

InstBXReg*
InstBXReg::AsTHIS(const Instruction& i)
{
    if (IsTHIS(i))
        return (InstBXReg*)&i;
    return nullptr;
}

bool
InstBLXReg::IsTHIS(const Instruction& i)
{
#if defined(VARAN_THUMB2)
    // Packed T2 BLX: high halfword = 0x4780|(Rm<<3), low = 0xbf00. Mask out the Rm field (bits 6:3).
    return ((i.encode() >> 16) & 0xff87u) == 0x4780u;
#else
    return (i.encode() & IsBRegMask) == IsBLX;
#endif
}
InstBLXReg*
InstBLXReg::AsTHIS(const Instruction& i)
{
    if (IsTHIS(i))
        return (InstBLXReg*)&i;
    return nullptr;
}

bool
InstBImm::IsTHIS(const Instruction& i)
{
#if defined(VARAN_THUMB2)
    return VaranBranchKind(i.encode()) == 1;   // B.W (T4) or B<c>.W (T3)
#else
    return (i.encode () & IsBImmMask) == IsB;
#endif
}
InstBImm*
InstBImm::AsTHIS(const Instruction& i)
{
    if (IsTHIS(i))
        return (InstBImm*)&i;
    return nullptr;
}

bool
InstBLImm::IsTHIS(const Instruction& i)
{
#if defined(VARAN_THUMB2)
    return VaranBranchKind(i.encode()) == 2;   // BL (T1)
#else
    return (i.encode () & IsBImmMask) == IsBL;
#endif
}
InstBLImm*
InstBLImm::AsTHIS(const Instruction& i)
{
    if (IsTHIS(i))
        return (InstBLImm*)&i;
    return nullptr;
}

bool
InstMovWT::IsTHIS(Instruction& i)
{
    return  InstMovW::IsTHIS(i) || InstMovT::IsTHIS(i);
}
InstMovWT*
InstMovWT::AsTHIS(Instruction& i)
{
    if (IsTHIS(i))
        return (InstMovWT*)&i;
    return nullptr;
}

void
InstMovWT::extractImm(Imm16* imm)
{
#if defined(VARAN_THUMB2)
    // T3 read-back: imm16 = imm4:i:imm3:imm8 from (hw0, hw1). enc = (hw1<<16)|hw0.
    uint32_t enc = this->encode();
    uint32_t hw0 = enc & 0xffff, hw1 = enc >> 16;
    uint32_t imm16 = ((hw0 & 0xf) << 12) | (((hw0 >> 10) & 1) << 11) |
                     (((hw1 >> 12) & 0x7) << 8) | (hw1 & 0xff);
    *imm = Imm16(imm16);
#else
    *imm = Imm16(*this);
#endif
}
bool
InstMovWT::checkImm(Imm16 imm)
{
#if defined(VARAN_THUMB2)
    Imm16 mine; extractImm(&mine);
    return imm.decode() == mine.decode();
#else
    return imm.decode() == Imm16(*this).decode();
#endif
}

void
InstMovWT::extractDest(Register* dest)
{
#if defined(VARAN_THUMB2)
    *dest = Register::FromCode((this->encode() >> 24) & 0xf);   // T3 Rd = hw1 bits[11:8]
#else
    *dest = toRD(*this);
#endif
}
bool
InstMovWT::checkDest(Register dest)
{
#if defined(VARAN_THUMB2)
    Register mine; extractDest(&mine);
    return dest == mine;
#else
    return dest == toRD(*this);
#endif
}

bool
InstMovW::IsTHIS(const Instruction& i)
{
#if defined(VARAN_THUMB2)
    return ((i.encode() & 0xffff) & 0xfbf0) == 0xf240;   // T3 movw
#else
    return (i.encode() & IsWTMask) == IsW;
#endif
}

InstMovW*
InstMovW::AsTHIS(const Instruction& i)
{
    if (IsTHIS(i))
        return (InstMovW*)&i;
    return nullptr;
}
InstMovT*
InstMovT::AsTHIS(const Instruction& i)
{
    if (IsTHIS(i))
        return (InstMovT*)&i;
    return nullptr;
}

bool
InstMovT::IsTHIS(const Instruction& i)
{
#if defined(VARAN_THUMB2)
    return ((i.encode() & 0xffff) & 0xfbf0) == 0xf2c0;   // T3 movt
#else
    return (i.encode() & IsWTMask) == IsT;
#endif
}

InstALU*
InstALU::AsTHIS(const Instruction& i)
{
    if (IsTHIS(i))
        return (InstALU*)&i;
    return nullptr;
}
bool
InstALU::IsTHIS(const Instruction& i)
{
    return (i.encode() & ALUMask) == 0;
}
void
InstALU::extractOp(ALUOp* ret)
{
    *ret = ALUOp(encode() & (0xf << 21));
}
bool
InstALU::checkOp(ALUOp op)
{
    ALUOp mine;
    extractOp(&mine);
    return mine == op;
}
void
InstALU::extractDest(Register* ret)
{
    *ret = toRD(*this);
}
bool
InstALU::checkDest(Register rd)
{
    return rd == toRD(*this);
}
void
InstALU::extractOp1(Register* ret)
{
    *ret = toRN(*this);
}
bool
InstALU::checkOp1(Register rn)
{
    return rn == toRN(*this);
}
Operand2
InstALU::extractOp2()
{
    return Operand2(encode());
}

InstCMP*
InstCMP::AsTHIS(const Instruction& i)
{
    if (IsTHIS(i))
        return (InstCMP*)&i;
    return nullptr;
}

bool
InstCMP::IsTHIS(const Instruction& i)
{
    return InstALU::IsTHIS(i) && InstALU::AsTHIS(i)->checkDest(r0) && InstALU::AsTHIS(i)->checkOp(OpCmp);
}

InstMOV*
InstMOV::AsTHIS(const Instruction& i)
{
    if (IsTHIS(i))
        return (InstMOV*)&i;
    return nullptr;
}

bool
InstMOV::IsTHIS(const Instruction& i)
{
    return InstALU::IsTHIS(i) && InstALU::AsTHIS(i)->checkOp1(r0) && InstALU::AsTHIS(i)->checkOp(OpMov);
}

Op2Reg
Operand2::toOp2Reg() const {
    return *(Op2Reg*)this;
}

Imm16::Imm16(Instruction& inst)
  : lower_(inst.encode() & 0xfff),
    upper_(inst.encode() >> 16),
    invalid_(0xfff)
{ }

Imm16::Imm16(uint32_t imm)
  : lower_(imm & 0xfff), pad_(0),
    upper_((imm >> 12) & 0xf),
    invalid_(0)
{
    MOZ_ASSERT(decode() == imm);
}

Imm16::Imm16()
  : invalid_(0xfff)
{ }

#if defined(VARAN_THUMB2)
// Shared 2-slot jump writer (defined below near varanPatchCondBranch2); forward-declared for PatchJump.
static void VaranComputeJump2(intptr_t s0, intptr_t target, uint32_t condField, uint32_t* w0, uint32_t* w1);
#endif

void
jit::PatchJump(CodeLocationJump& jump_, CodeLocationLabel label, ReprotectCode reprotect)
{
#if defined(VARAN_THUMB2)
    // The patchable jump is the reserved 2-slot site [slot0, slot0+4] laid by jumpWithPatch. Recover the
    // condition from slot0 and rewrite the pair with VaranComputeJump2 (in-range B<c>.W+NOP.W, or the
    // overflow invert+B.W). Reprotect + flush BOTH slots (8 bytes) -- device FACT: FlushInstructionCache
    // is mandatory and the I-cache is not auto-coherent, so a 4-byte flush would leave slot1 stale. Do
    // NOT route through the pool/InstLDR far path (device-lethal A32); >+-16MB trips VaranComputeJump2.
    uint32_t* w0 = reinterpret_cast<uint32_t*>(jump_.raw());
    uint32_t* w1 = w0 + 1;
    uint32_t cf = 14u;
    VaranDecodeBranchCond(*w0, &cf);
    intptr_t s0 = reinterpret_cast<intptr_t>(w0);
    intptr_t target = reinterpret_cast<intptr_t>(label.raw());
    MaybeAutoWritableJitCode awjc(reinterpret_cast<void*>(w0), 2 * sizeof(uint32_t), reprotect);
    VaranComputeJump2(s0, target, cf, w0, w1);
    AutoFlushICache::flush(uintptr_t(w0), 2 * sizeof(uint32_t));
    return;
#else
    // We need to determine if this jump can fit into the standard 24+2 bit
    // address or if we need a larger branch (or just need to use our pool
    // entry).
    Instruction* jump = (Instruction*)jump_.raw();
    // jumpWithPatch() returns the offset of the jump and never a pool or nop.
    Assembler::Condition c = jump->extractCond();
    MOZ_ASSERT(jump->is<InstBranchImm>() || jump->is<InstLDR>());

    int jumpOffset = label.raw() - jump_.raw();
    if (BOffImm::IsInRange(jumpOffset)) {
        // This instruction started off as a branch, and will remain one.
        MaybeAutoWritableJitCode awjc(jump, sizeof(Instruction), reprotect);
        Assembler::RetargetNearBranch(jump, jumpOffset, c);
    } else {
        // This instruction started off as a branch, but now needs to be demoted
        // to an ldr.
        uint8_t** slot = reinterpret_cast<uint8_t**>(jump_.jumpTableEntry());

        // Ensure both the branch and the slot are writable.
        MOZ_ASSERT(uintptr_t(slot) > uintptr_t(jump));
        size_t size = uintptr_t(slot) - uintptr_t(jump) + sizeof(void*);
        MaybeAutoWritableJitCode awjc(jump, size, reprotect);

        Assembler::RetargetFarBranch(jump, slot, label.raw(), c);
    }
#endif
}

void
Assembler::finish()
{
    flush();
    MOZ_ASSERT(!isFinished);
    isFinished = true;
}

bool
Assembler::asmMergeWith(Assembler& other)
{
    flush();
    other.flush();
    if (other.oom())
        return false;
    if (!AssemblerShared::asmMergeWith(size(), other))
        return false;
    return m_buffer.appendBuffer(other.m_buffer);
}

void
Assembler::executableCopy(uint8_t* buffer)
{
    MOZ_ASSERT(isFinished);
    m_buffer.executableCopy(buffer);
    AutoFlushICache::setRange(uintptr_t(buffer), m_buffer.size());
}

uint32_t
Assembler::actualIndex(uint32_t idx_) const
{
#if defined(VARAN_THUMB2)
    // VARAN jumpWithPatch returns CodeOffsetJump(slot0, jumpTableIndex=0) with NO pool entry, so
    // poolEntryOffset(PoolEntry(0)) would assert with zero pools. The IC fixup/repoint callers
    // (IonCaches) only need the raw jump offset back; jumpTableEntry_ is unused by VARAN PatchJump.
    return idx_;
#else
    ARMBuffer::PoolEntry pe(idx_);
    return m_buffer.poolEntryOffset(pe);
#endif
}

uint8_t*
Assembler::PatchableJumpAddress(JitCode* code, uint32_t pe_)
{
    return code->raw() + pe_;
}

class RelocationIterator
{
    CompactBufferReader reader_;
    // Offset in bytes.
    uint32_t offset_;

  public:
    RelocationIterator(CompactBufferReader& reader)
      : reader_(reader)
    { }

    bool read() {
        if (!reader_.more())
            return false;
        offset_ = reader_.readUnsigned();
        return true;
    }

    uint32_t offset() const {
        return offset_;
    }
};

template<class Iter>
const uint32_t*
Assembler::GetCF32Target(Iter* iter)
{
    Instruction* inst1 = iter->cur();

    if (inst1->is<InstBranchImm>()) {
        // See if we have a simple case, b #offset.
        BOffImm imm;
        InstBranchImm* jumpB = inst1->as<InstBranchImm>();
        jumpB->extractImm(&imm);
        return imm.getDest(inst1)->raw();
    }

    if (inst1->is<InstMovW>())
    {
        // See if we have the complex case:
        //  movw r_temp, #imm1
        //  movt r_temp, #imm2
        //  bx r_temp
        // OR
        //  movw r_temp, #imm1
        //  movt r_temp, #imm2
        //  str pc, [sp]
        //  bx r_temp

        Imm16 targ_bot;
        Imm16 targ_top;
        Register temp;

        // Extract both the temp register and the bottom immediate.
        InstMovW* bottom = inst1->as<InstMovW>();
        bottom->extractImm(&targ_bot);
        bottom->extractDest(&temp);

        // Extract the top part of the immediate.
        Instruction* inst2 = iter->next();
        MOZ_ASSERT(inst2->is<InstMovT>());
        InstMovT* top = inst2->as<InstMovT>();
        top->extractImm(&targ_top);

        // Make sure they are being loaded into the same register.
        MOZ_ASSERT(top->checkDest(temp));

        // Make sure we're branching to the same register.
#ifdef DEBUG
        // A toggled call sometimes has a NOP instead of a branch for the third
        // instruction. No way to assert that it's valid in that situation.
        Instruction* inst3 = iter->next();
#if defined(VARAN_THUMB2)
        // B1: toggledCall now emits `movw / movt / orr scratch,#1 / (blx|nop)`. inst3 is the orr, so
        // the branch-or-nop is one further on. Checking `is<InstNOP>()` on the orr and then taking
        // the `else` arm would run `as<InstBranchReg>()` on a non-branch -- which returns nullptr,
        // not an assert -- and the following checkDest() would dereference null on every GC that
        // traces a debug-instrumented BaselineScript. Step to the real slot first.
        inst3 = inst3->next();
#endif
        if (!inst3->is<InstNOP>()) {
            InstBranchReg* realBranch = nullptr;
            if (inst3->is<InstBranchReg>()) {
                realBranch = inst3->as<InstBranchReg>();
            } else {
                Instruction* inst4 = iter->next();
                realBranch = inst4->as<InstBranchReg>();
            }
            MOZ_ASSERT(realBranch->checkDest(temp));
        }
#endif

        uint32_t* dest = (uint32_t*) (targ_bot.decode() | (targ_top.decode() << 16));
        return dest;
    }

    if (inst1->is<InstLDR>())
        return *(uint32_t**) inst1->as<InstLDR>()->dest();

    MOZ_CRASH("unsupported branch relocation");
}

uintptr_t
Assembler::GetPointer(uint8_t* instPtr)
{
    InstructionIterator iter((Instruction*)instPtr);
    uintptr_t ret = (uintptr_t)GetPtr32Target(&iter, nullptr, nullptr);
    return ret;
}

template<class Iter>
const uint32_t*
Assembler::GetPtr32Target(Iter* start, Register* dest, RelocStyle* style)
{
    Instruction* load1 = start->cur();
    Instruction* load2 = start->next();

    if (load1->is<InstMovW>() && load2->is<InstMovT>()) {
        if (style)
            *style = L_MOVWT;

        // See if we have the complex case:
        //  movw r_temp, #imm1
        //  movt r_temp, #imm2

        Imm16 targ_bot;
        Imm16 targ_top;
        Register temp;

        // Extract both the temp register and the bottom immediate.
        InstMovW* bottom = load1->as<InstMovW>();
        bottom->extractImm(&targ_bot);
        bottom->extractDest(&temp);

        // Extract the top part of the immediate.
        InstMovT* top = load2->as<InstMovT>();
        top->extractImm(&targ_top);

        // Make sure they are being loaded into the same register.
        MOZ_ASSERT(top->checkDest(temp));

        if (dest)
            *dest = temp;

        uint32_t* value = (uint32_t*) (targ_bot.decode() | (targ_top.decode() << 16));
        return value;
    }

    if (load1->is<InstLDR>()) {
        if (style)
            *style = L_LDR;
        if (dest)
            *dest = toRD(*load1);
        return *(uint32_t**) load1->as<InstLDR>()->dest();
    }

#if defined(VARAN_THUMB2) && defined(JS_SIMULATOR_ARM)
    // The blanket UDF makes relocation read-back impossible (branches/pointer-loads are UDFs).
    // Compilation dies here -- dump the emit-time census so this run still yields the harvest.
    // Simulator-only: the census exists to triage encoder gaps on the sim host, and a device
    // build has no UDF arm to census (emitUdf MOZ_CRASHes there).
    VaranDumpUdfCensus();
#endif
    MOZ_CRASH("unsupported relocation");
}

static JitCode*
CodeFromJump(InstructionIterator* jump)
{
    uint8_t* target = (uint8_t*)Assembler::GetCF32Target(jump);
    return JitCode::FromExecutable(target);
}

void
Assembler::TraceJumpRelocations(JSTracer* trc, JitCode* code, CompactBufferReader& reader)
{
    RelocationIterator iter(reader);
    while (iter.read()) {
        InstructionIterator institer((Instruction*) (code->raw() + iter.offset()));
        JitCode* child = CodeFromJump(&institer);
        TraceManuallyBarrieredEdge(trc, &child, "rel32");
    }
}

template <class Iter>
static void
TraceOneDataRelocation(JSTracer* trc, Iter* iter)
{
    Instruction* ins = iter->cur();
    Register dest;
    Assembler::RelocStyle rs;
    const void* prior = Assembler::GetPtr32Target(iter, &dest, &rs);
    void* ptr = const_cast<void*>(prior);

    // No barrier needed since these are constants.
    TraceManuallyBarrieredGenericPointerEdge(trc, reinterpret_cast<gc::Cell**>(&ptr),
                                             "ion-masm-ptr");

    if (ptr != prior) {
        MacroAssemblerARM::ma_mov_patch(Imm32(int32_t(ptr)), dest, Assembler::Always, rs, ins);

        // L_LDR won't cause any instructions to be updated.
        if (rs != Assembler::L_LDR) {
            AutoFlushICache::flush(uintptr_t(ins), 4);
            AutoFlushICache::flush(uintptr_t(ins->next()), 4);
        }
    }
}

static void
TraceDataRelocations(JSTracer* trc, uint8_t* buffer, CompactBufferReader& reader)
{
    while (reader.more()) {
        size_t offset = reader.readUnsigned();
        InstructionIterator iter((Instruction*)(buffer + offset));
        TraceOneDataRelocation(trc, &iter);
    }
}

static void
TraceDataRelocations(JSTracer* trc, ARMBuffer* buffer, CompactBufferReader& reader)
{
    while (reader.more()) {
        BufferOffset offset(reader.readUnsigned());
        ARMBuffer::AssemblerBufferInstIterator iter(offset, buffer);
        TraceOneDataRelocation(trc, &iter);
    }
}

void
Assembler::TraceDataRelocations(JSTracer* trc, JitCode* code, CompactBufferReader& reader)
{
    ::TraceDataRelocations(trc, code->raw(), reader);
}

void
Assembler::copyJumpRelocationTable(uint8_t* dest)
{
    if (jumpRelocations_.length())
        memcpy(dest, jumpRelocations_.buffer(), jumpRelocations_.length());
}

void
Assembler::copyDataRelocationTable(uint8_t* dest)
{
    if (dataRelocations_.length())
        memcpy(dest, dataRelocations_.buffer(), dataRelocations_.length());
}

void
Assembler::copyPreBarrierTable(uint8_t* dest)
{
    if (preBarriers_.length())
        memcpy(dest, preBarriers_.buffer(), preBarriers_.length());
}

void
Assembler::trace(JSTracer* trc)
{
    for (size_t i = 0; i < jumps_.length(); i++) {
        RelativePatch& rp = jumps_[i];
        if (rp.kind() == Relocation::JITCODE) {
            JitCode* code = JitCode::FromExecutable((uint8_t*)rp.target());
            TraceManuallyBarrieredEdge(trc, &code, "masmrel32");
            MOZ_ASSERT(code == JitCode::FromExecutable((uint8_t*)rp.target()));
        }
    }

    if (dataRelocations_.length()) {
        CompactBufferReader reader(dataRelocations_);
        ::TraceDataRelocations(trc, &m_buffer, reader);
    }
}

void
Assembler::processCodeLabels(uint8_t* rawCode)
{
    for (size_t i = 0; i < codeLabels_.length(); i++) {
        CodeLabel label = codeLabels_[i];
        Bind(rawCode, label.patchAt(), rawCode + label.target()->offset());
    }
}

void
Assembler::writeCodePointer(CodeOffset* label) {
#if defined(VARAN_THUMB2)
    // This emits a DATA word -- a jump-table entry placeholder that processCodeLabels()/Bind() later
    // overwrites with a real address -- NOT an instruction. It must therefore bypass writeInst()'s
    // blanket A32->UDF divert, whose auto-code assumes its argument is an A32 instruction word.
    // Routing data through it produced the single highest count in the UDF census (1045 phantom
    // "0x2ff" emits = 0x200 | ((LabelBase::INVALID_OFFSET >> 20) & 0xff), i.e. 0x200|0xff), which
    // read as an unidentified INSTRUCTION gap when it is nothing of the kind. Size and semantics are
    // unchanged (both paths are one 4-byte putInt, both overwritten by Bind).
    //
    // NB this does NOT make Ion table-switch work under Thumb-2, and is not claimed to:
    // CodeGeneratorARM::emitTableSwitchDispatch does `ldr pc, [pc, index<<2]` over these entries, so
    // each bound address needs bit0 set (F2) -- plus that ldr is conditional (NotSigned) and
    // pc-relative. Those belong to the Ion batch; see ION-FRONTIER-SYNTHESIS.md.
    BufferOffset off = m_buffer.putInt(LabelBase::INVALID_OFFSET);
#else
    BufferOffset off = writeInst(LabelBase::INVALID_OFFSET);
#endif
    label->bind(off.getOffset());
}

void
Assembler::Bind(uint8_t* rawCode, CodeOffset* label, const void* address)
{
#if defined(VARAN_THUMB2)
    // ---- D2/C7: a jump-table entry is BRANCHED THROUGH, so it must carry the Thumb bit ----
    //
    // Found by our own B12 interworking oracle the moment the table-switch reshape above made
    // the dispatch legal: `ldr.w pc, [table, index lsl 2]` takes bit0 of the loaded word as the
    // instruction-set selector, so an even entry switches the core to ARM state and executes
    // Thumb halfwords as A32 -- device-lethal, and silent on anything that only checks encodings.
    //
    // SET, not mask: this is the C7 direction (a synthesized code address about to be jumped to),
    // the opposite of D1/D3 where an address is compared or subtracted.
    //
    // Scoped correctly because on ARM a CodeLabel is ONLY ever a tableswitch entry -- the sole
    // writeCodePointer() caller is CodeGeneratorARM::visitTableSwitch, and Trampoline-arm.cpp
    // states the same invariant in-tree ("On ARM, CodeLabel is only used for tableswitch"). If a
    // future CodeLabel is introduced for DATA, this must become per-site.
    *reinterpret_cast<uintptr_t*>(rawCode + label->offset()) =
        reinterpret_cast<uintptr_t>(address) | 1;
#else
    *reinterpret_cast<const void**>(rawCode + label->offset()) = address;
#endif
}

Assembler::Condition
Assembler::InvertCondition(Condition cond)
{
    const uint32_t ConditionInversionBit = 0x10000000;
    return Condition(ConditionInversionBit ^ cond);
}

Assembler::Condition
Assembler::UnsignedCondition(Condition cond)
{
    switch (cond) {
      case Zero:
      case NonZero:
        return cond;
      case LessThan:
      case Below:
        return Below;
      case LessThanOrEqual:
      case BelowOrEqual:
        return BelowOrEqual;
      case GreaterThan:
      case Above:
        return Above;
      case AboveOrEqual:
      case GreaterThanOrEqual:
        return AboveOrEqual;
      default:
        MOZ_CRASH("unexpected condition");
    }
}

Assembler::Condition
Assembler::ConditionWithoutEqual(Condition cond)
{
    switch (cond) {
      case LessThan:
      case LessThanOrEqual:
          return LessThan;
      case Below:
      case BelowOrEqual:
        return Below;
      case GreaterThan:
      case GreaterThanOrEqual:
        return GreaterThan;
      case Above:
      case AboveOrEqual:
        return Above;
      default:
        MOZ_CRASH("unexpected condition");
    }
}
Imm8::TwoImm8mData
Imm8::EncodeTwoImms(uint32_t imm)
{
#if defined(VARAN_THUMB2)
    // The A32 two-imm8m decomposition can produce a half that is A32-legal but Thumb-2-ILLEGAL (a
    // wrap-around 8-bit run), which would misencode. Disable the 2-insn optimization: return invalid
    // so alu_dbl falls to ma_mov+reg-op (<=1 extra instruction, always correct). Re-enable only with a
    // T2-aware two-modimm-halves search if profiling shows the fallback hot. (ENCODING-TABLES Batch 1.)
    return TwoImm8mData();
#else
    // In the ideal case, we are looking for a number that (in binary) looks
    // like:
    //   0b((00)*)n_1((00)*)n_2((00)*)
    //      left  n1   mid  n2
    //   where both n_1 and n_2 fit into 8 bits.
    // Since this is being done with rotates, we also need to handle the case
    // that one of these numbers is in fact split between the left and right
    // sides, in which case the constant will look like:
    //   0bn_1a((00)*)n_2((00)*)n_1b
    //     n1a  mid  n2   rgh    n1b
    // Also remember, values are rotated by multiples of two, and left, mid or
    // right can have length zero.
    uint32_t imm1, imm2;
    int left = CountLeadingZeroes32(imm) & 0x1E;
    uint32_t no_n1 = imm & ~(0xff << (24 - left));

    // Not technically needed: this case only happens if we can encode as a
    // single imm8m. There is a perfectly reasonable encoding in this case, but
    // we shouldn't encourage people to do things like this.
    if (no_n1 == 0)
        return TwoImm8mData();

    int mid = CountLeadingZeroes32(no_n1) & 0x1E;
    uint32_t no_n2 = no_n1 & ~((0xff << ((24 - mid) & 0x1f)) | 0xff >> ((8 + mid) & 0x1f));

    if (no_n2 == 0) {
        // We hit the easy case, no wraparound.
        // Note: a single constant *may* look like this.
        int imm1shift = left + 8;
        int imm2shift = mid + 8;
        imm1 = (imm >> (32 - imm1shift)) & 0xff;
        if (imm2shift >= 32) {
            imm2shift = 0;
            // This assert does not always hold, in fact, this would lead to
            // some incredibly subtle bugs.
            // assert((imm & 0xff) == no_n1);
            imm2 = no_n1;
        } else {
            imm2 = ((imm >> (32 - imm2shift)) | (imm << imm2shift)) & 0xff;
            MOZ_ASSERT( ((no_n1 >> (32 - imm2shift)) | (no_n1 << imm2shift)) ==
                        imm2);
        }
        MOZ_ASSERT((imm1shift & 0x1) == 0);
        MOZ_ASSERT((imm2shift & 0x1) == 0);
        return TwoImm8mData(datastore::Imm8mData(imm1, imm1shift >> 1),
                            datastore::Imm8mData(imm2, imm2shift >> 1));
    }

    // Either it wraps, or it does not fit. If we initially chopped off more
    // than 8 bits, then it won't fit.
    if (left >= 8)
        return TwoImm8mData();

    int right = 32 - (CountLeadingZeroes32(no_n2) & 30);
    // All remaining set bits *must* fit into the lower 8 bits.
    // The right == 8 case should be handled by the previous case.
    if (right > 8)
        return TwoImm8mData();

    // Make sure the initial bits that we removed for no_n1 fit into the
    // 8-(32-right) leftmost bits.
    if (((imm & (0xff << (24 - left))) << (8 - right)) != 0) {
        // BUT we may have removed more bits than we needed to for no_n1
        // 0x04104001 e.g. we can encode 0x104 with a single op, then 0x04000001
        // with a second, but we try to encode 0x0410000 and find that we need a
        // second op for 0x4000, and 0x1 cannot be included in the encoding of
        // 0x04100000.
        no_n1 = imm & ~((0xff >> (8 - right)) | (0xff << (24 + right)));
        mid = CountLeadingZeroes32(no_n1) & 30;
        no_n2 = no_n1  & ~((0xff << ((24 - mid)&31)) | 0xff >> ((8 + mid)&31));
        if (no_n2 != 0)
            return TwoImm8mData();
    }

    // Now assemble all of this information into a two coherent constants it is
    // a rotate right from the lower 8 bits.
    int imm1shift = 8 - right;
    imm1 = 0xff & ((imm << imm1shift) | (imm >> (32 - imm1shift)));
    MOZ_ASSERT((imm1shift & ~0x1e) == 0);
    // left + 8 + mid is the position of the leftmost bit of n_2.
    // We needed to rotate 0x000000ab right by 8 in order to get 0xab000000,
    // then shift again by the leftmost bit in order to get the constant that we
    // care about.
    int imm2shift =  mid + 8;
    imm2 = ((imm >> (32 - imm2shift)) | (imm << imm2shift)) & 0xff;
    MOZ_ASSERT((imm1shift & 0x1) == 0);
    MOZ_ASSERT((imm2shift & 0x1) == 0);
    return TwoImm8mData(datastore::Imm8mData(imm1, imm1shift >> 1),
                        datastore::Imm8mData(imm2, imm2shift >> 1));
#endif
}

ALUOp
jit::ALUNeg(ALUOp op, Register dest, Register scratch, Imm32* imm, Register* negDest)
{
    // Find an alternate ALUOp to get the job done, and use a different imm.
    *negDest = dest;
    switch (op) {
      case OpMov:
        *imm = Imm32(~imm->value);
        return OpMvn;
      case OpMvn:
        *imm = Imm32(~imm->value);
        return OpMov;
      case OpAnd:
        *imm = Imm32(~imm->value);
        return OpBic;
      case OpBic:
        *imm = Imm32(~imm->value);
        return OpAnd;
      case OpAdd:
        *imm = Imm32(-imm->value);
        return OpSub;
      case OpSub:
        *imm = Imm32(-imm->value);
        return OpAdd;
      case OpCmp:
        *imm = Imm32(-imm->value);
        return OpCmn;
      case OpCmn:
        *imm = Imm32(-imm->value);
        return OpCmp;
      case OpTst:
        MOZ_ASSERT(dest == InvalidReg);
        *imm = Imm32(~imm->value);
        *negDest = scratch;
        return OpBic;
        // orr has orn on thumb2 only.
      default:
        return OpInvalid;
    }
}

bool
jit::can_dbl(ALUOp op)
{
    // Some instructions can't be processed as two separate instructions such as
    // and, and possibly add (when we're setting ccodes). There is also some
    // hilarity with *reading* condition codes. For example, adc dest, src1,
    // 0xfff; (add with carry) can be split up into adc dest, src1, 0xf00; add
    // dest, dest, 0xff, since "reading" the condition code increments the
    // result by one conditionally, that only needs to be done on one of the two
    // instructions.
    switch (op) {
      case OpBic:
      case OpAdd:
      case OpSub:
      case OpEor:
      case OpOrr:
        return true;
      default:
        return false;
    }
}

bool
jit::condsAreSafe(ALUOp op) {
    // Even when we are setting condition codes, sometimes we can get away with
    // splitting an operation into two. For example, if our immediate is
    // 0x00ff00ff, and the operation is eors we can split this in half, since x
    // ^ 0x00ff0000 ^ 0x000000ff should set all of its condition codes exactly
    // the same as x ^ 0x00ff00ff. However, if the operation were adds, we
    // cannot split this in half. If the source on the add is 0xfff00ff0, the
    // result sholud be 0xef10ef, but do we set the overflow bit or not?
    // Depending on which half is performed first (0x00ff0000 or 0x000000ff) the
    // V bit will be set differently, and *not* updating the V bit would be
    // wrong. Theoretically, the following should work:
    //  adds r0, r1, 0x00ff0000;
    //  addsvs r0, r1, 0x000000ff;
    //  addvc r0, r1, 0x000000ff;
    // But this is 3 instructions, and at that point, we might as well use
    // something else.
    switch(op) {
      case OpBic:
      case OpOrr:
      case OpEor:
        return true;
      default:
        return false;
    }
}

ALUOp
jit::getDestVariant(ALUOp op)
{
    // All of the compare operations are dest-less variants of a standard
    // operation. Given the dest-less variant, return the dest-ful variant.
    switch (op) {
      case OpCmp:
        return OpSub;
      case OpCmn:
        return OpAdd;
      case OpTst:
        return OpAnd;
      case OpTeq:
        return OpEor;
      default:
        return op;
    }
}

O2RegImmShift
jit::O2Reg(Register r) {
    return O2RegImmShift(r, LSL, 0);
}

O2RegImmShift
jit::lsl(Register r, int amt)
{
    MOZ_ASSERT(0 <= amt && amt <= 31);
    return O2RegImmShift(r, LSL, amt);
}

O2RegImmShift
jit::lsr(Register r, int amt)
{
    MOZ_ASSERT(1 <= amt && amt <= 32);
    return O2RegImmShift(r, LSR, amt);
}

O2RegImmShift
jit::ror(Register r, int amt)
{
    MOZ_ASSERT(1 <= amt && amt <= 31);
    return O2RegImmShift(r, ROR, amt);
}
O2RegImmShift
jit::rol(Register r, int amt)
{
    MOZ_ASSERT(1 <= amt && amt <= 31);
    return O2RegImmShift(r, ROR, 32 - amt);
}

O2RegImmShift
jit::asr(Register r, int amt)
{
    MOZ_ASSERT(1 <= amt && amt <= 32);
    return O2RegImmShift(r, ASR, amt);
}


O2RegRegShift
jit::lsl(Register r, Register amt)
{
    return O2RegRegShift(r, LSL, amt);
}

O2RegRegShift
jit::lsr(Register r, Register amt)
{
    return O2RegRegShift(r, LSR, amt);
}

O2RegRegShift
jit::ror(Register r, Register amt)
{
    return O2RegRegShift(r, ROR, amt);
}

O2RegRegShift
jit::asr(Register r, Register amt)
{
    return O2RegRegShift(r, ASR, amt);
}

static js::jit::DoubleEncoder doubleEncoder;

/* static */ const js::jit::VFPImm js::jit::VFPImm::One(0x3FF00000);

js::jit::VFPImm::VFPImm(uint32_t top)
{
    data_ = -1;
    datastore::Imm8VFPImmData tmp;
    if (doubleEncoder.lookup(top, &tmp))
        data_ = tmp.encode();
}

#if defined(VARAN_THUMB2)
// Decode the byte value stored in a Thumb-2 branch word (real distance / chain link / sentinel).
BOffImm::BOffImm(const Instruction& inst)
  : data_(VaranDecodeBranchByteVal(inst.encode()))
{
}
#else
BOffImm::BOffImm(const Instruction& inst)
  : data_(inst.encode() & 0x00ffffff)
{
}
#endif

Instruction*
BOffImm::getDest(Instruction* src) const
{
#if defined(VARAN_THUMB2)
    // Thumb-2: data_ is the RAW signed byte distance (target - branch); use byte-address arithmetic
    // (T2 instructions are 2- or 4-byte, so Instruction[]-indexing does not apply, and there is no
    // A32 PC+8 word-index bias). Reached from GetCF32Target when tracing a bound branch's target.
    return reinterpret_cast<Instruction*>(reinterpret_cast<uint8_t*>(src) + data_);
#else
    // TODO: It is probably worthwhile to verify that src is actually a branch.
    // NOTE: This does not explicitly shift the offset of the destination left by 2,
    // since it is indexing into an array of instruction sized objects.
    return &src[((int32_t(data_) << 8) >> 8) + 2];
#endif
}

const js::jit::DoubleEncoder::DoubleEntry js::jit::DoubleEncoder::table[256] = {
#include "jit/arm/DoubleEntryTable.tbl"
};

// VFPRegister implementation
VFPRegister
VFPRegister::doubleOverlay(unsigned int which) const
{
    MOZ_ASSERT(!_isInvalid);
    MOZ_ASSERT(which == 0);
    if (kind != Double)
        return VFPRegister(code_ >> 1, Double);
    return *this;
}
VFPRegister
VFPRegister::singleOverlay(unsigned int which) const
{
    MOZ_ASSERT(!_isInvalid);
    if (kind == Double) {
        // There are no corresponding float registers for d16-d31.
        MOZ_ASSERT(code_ < 16);
        MOZ_ASSERT(which < 2);
        return VFPRegister((code_ << 1) + which, Single);
    }
    MOZ_ASSERT(which == 0);
    return VFPRegister(code_, Single);
}

VFPRegister
VFPRegister::sintOverlay(unsigned int which) const
{
    MOZ_ASSERT(!_isInvalid);
    if (kind == Double) {
        // There are no corresponding float registers for d16-d31.
        MOZ_ASSERT(code_ < 16);
        MOZ_ASSERT(which < 2);
        return VFPRegister((code_ << 1) + which, Int);
    }
    MOZ_ASSERT(which == 0);
    return VFPRegister(code_, Int);
}
VFPRegister
VFPRegister::uintOverlay(unsigned int which) const
{
    MOZ_ASSERT(!_isInvalid);
    if (kind == Double) {
        // There are no corresponding float registers for d16-d31.
        MOZ_ASSERT(code_ < 16);
        MOZ_ASSERT(which < 2);
        return VFPRegister((code_ << 1) + which, UInt);
    }
    MOZ_ASSERT(which == 0);
    return VFPRegister(code_, UInt);
}

bool
VFPRegister::isInvalid() const
{
    return _isInvalid;
}

bool
VFPRegister::isMissing() const
{
    MOZ_ASSERT(!_isInvalid);
    return _isMissing;
}


bool
Assembler::oom() const
{
    return AssemblerShared::oom() ||
           m_buffer.oom() ||
           jumpRelocations_.oom() ||
           dataRelocations_.oom() ||
           preBarriers_.oom();
}

// Size of the instruction stream, in bytes. Including pools. This function
// expects all pools that need to be placed have been placed. If they haven't
// then we need to go an flush the pools :(
size_t
Assembler::size() const
{
    return m_buffer.size();
}
// Size of the relocation table, in bytes.
size_t
Assembler::jumpRelocationTableBytes() const
{
    return jumpRelocations_.length();
}
size_t
Assembler::dataRelocationTableBytes() const
{
    return dataRelocations_.length();
}

size_t
Assembler::preBarrierTableBytes() const
{
    return preBarriers_.length();
}

// Size of the data table, in bytes.
size_t
Assembler::bytesNeeded() const
{
    return size() +
        jumpRelocationTableBytes() +
        dataRelocationTableBytes() +
        preBarrierTableBytes();
}

#ifdef JS_DISASM_ARM

void
Assembler::spewInst(Instruction* i)
{
    disasm::NameConverter converter;
    disasm::Disassembler dasm(converter);
    disasm::EmbeddedVector<char, disasm::ReasonableBufferSize> buffer;
    uint8_t* loc = reinterpret_cast<uint8_t*>(const_cast<uint32_t*>(i->raw()));
    dasm.InstructionDecode(buffer, loc);
    printf("   %08x  %s\n", reinterpret_cast<uint32_t>(loc), buffer.start());
}

// Labels are named as they are encountered by adding names to a
// table, using the Label address as the key.  This is made tricky by
// the (memory for) Label objects being reused, but reused label
// objects are recognizable from being marked as not used or not
// bound.  See spewResolve().
//
// In a number of cases there is no information about the target, and
// we just end up printing "patchable constant load to PC".  This is
// true especially for jumps to bailout handlers (which have no
// names).  See spewData() and its callers.  In some cases (loop back
// edges) some information about the intended target may be propagated
// from higher levels, and if so it's printed here.

void
Assembler::spew(Instruction* i)
{
    if (spewDisabled() || !i)
        return;
    disasm::NameConverter converter;
    disasm::Disassembler dasm(converter);
    disasm::EmbeddedVector<char, disasm::ReasonableBufferSize> buffer;
    uint8_t* loc = reinterpret_cast<uint8_t*>(const_cast<uint32_t*>(i->raw()));
    dasm.InstructionDecode(buffer, loc);
    spew("   %08x  %s", reinterpret_cast<uint32_t>(loc), buffer.start());
}

void
Assembler::spewTarget(Label* target)
{
    if (spewDisabled())
        return;
    spew("                        -> %d%s", spewResolve(target), !target->bound() ? "f" : "");
}

// If a target label is known, always print that and do not attempt to
// disassemble the branch operands, as they will often be encoding
// metainformation (pointers for a chain of jump instructions), and
// not actual branch targets.

void
Assembler::spewBranch(Instruction* i, Label* target /* may be nullptr */)
{
    if (spewDisabled() || !i)
        return;
    disasm::NameConverter converter;
    disasm::Disassembler dasm(converter);
    disasm::EmbeddedVector<char, disasm::ReasonableBufferSize> buffer;
    uint8_t* loc = reinterpret_cast<uint8_t*>(const_cast<uint32_t*>(i->raw()));
    dasm.InstructionDecode(buffer, loc);
    char labelBuf[128];
    labelBuf[0] = 0;
    if (!target)
        snprintf(labelBuf, sizeof(labelBuf), "  -> (link-time target)");
    if (InstBranchImm::IsTHIS(*i)) {
        InstBranchImm* bimm = InstBranchImm::AsTHIS(*i);
        BOffImm destOff;
        bimm->extractImm(&destOff);
        if (destOff.isInvalid() || target) {
            // The target information in the instruction is likely garbage, so remove it.
            // The target label will in any case be printed if we have it.
            //
            // The format of the instruction disassembly is [0-9a-f]{8}\s+\S+\s+.*,
            // where the \S+ string is the opcode.  Strip everything after the opcode,
            // and attach the label if we have it.
            int i;
            for ( i=8 ; i < buffer.length() && buffer[i] == ' ' ; i++ )
                ;
            for ( ; i < buffer.length() && buffer[i] != ' ' ; i++ )
                ;
            buffer[i] = 0;
            if (target) {
                snprintf(labelBuf, sizeof(labelBuf), "  -> %d%s", spewResolve(target),
                         !target->bound() ? "f" : "");
                target = nullptr;
            }
        }
    }
    spew("   %08x  %s%s", reinterpret_cast<uint32_t>(loc), buffer.start(), labelBuf);
    if (target)
        spewTarget(target);
}

void
Assembler::spewLabel(Label* l)
{
    if (spewDisabled())
        return;
    spew("                        %d:", spewResolve(l));
}

void
Assembler::spewRetarget(Label* label, Label* target)
{
    if (spewDisabled())
        return;
    spew("                        %d: .retarget -> %d%s",
         spewResolve(label), spewResolve(target), !target->bound() ? "f" : "");
}

void
Assembler::spewData(BufferOffset addr, size_t numInstr, bool loadToPC)
{
    if (spewDisabled())
        return;
    Instruction* inst = m_buffer.getInstOrNull(addr);
    if (!inst)
        return;
    uint32_t *instr = reinterpret_cast<uint32_t*>(inst);
    for ( size_t k=0 ; k < numInstr ; k++ ) {
        spew("   %08x  %08x       (patchable constant load%s)",
             reinterpret_cast<uint32_t>(instr+k), *(instr+k), loadToPC ? " to PC" : "");
    }
}

bool
Assembler::spewDisabled()
{
    return !(JitSpewEnabled(JitSpew_Codegen) || printer_);
}

void
Assembler::spew(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    spew(fmt, args);
    va_end(args);
}

void
Assembler::spew(const char* fmt, va_list va)
{
    if (printer_) {
        printer_->vprintf(fmt, va);
        printer_->put("\n");
    }
    js::jit::JitSpewVA(js::jit::JitSpew_Codegen, fmt, va);
}

uint32_t
Assembler::spewResolve(Label* l)
{
    // Note, spewResolve will sometimes return 0 when it is triggered
    // by the profiler and not by a full disassembly, since in that
    // case a label can be used or bound but not previously have been
    // defined.
    return l->used() || l->bound() ? spewProbe(l) : spewDefine(l);
}

uint32_t
Assembler::spewProbe(Label* l)
{
    uint32_t key = reinterpret_cast<uint32_t>(l);
    uint32_t value = 0;
    spewNodes_.lookup(key, &value);
    return value;
}

uint32_t
Assembler::spewDefine(Label* l)
{
    uint32_t key = reinterpret_cast<uint32_t>(l);
    spewNodes_.remove(key);
    uint32_t value = spewNext_++;
    if (!spewNodes_.add(key, value))
        return 0;
    return value;
}

Assembler::SpewNodes::~SpewNodes()
{
    Node* p = nodes;
    while (p) {
        Node* victim = p;
        p = p->next;
        js_free(victim);
    }
}

bool
Assembler::SpewNodes::lookup(uint32_t key, uint32_t* value)
{
    for ( Node* p = nodes ; p ; p = p->next ) {
        if (p->key == key) {
            *value = p->value;
            return true;
        }
    }
    return false;
}

bool
Assembler::SpewNodes::add(uint32_t key, uint32_t value)
{
    Node* node = (Node*)js_malloc(sizeof(Node));
    if (!node)
        return false;
    node->key = key;
    node->value = value;
    node->next = nodes;
    nodes = node;
    return true;
}

bool
Assembler::SpewNodes::remove(uint32_t key)
{
    for ( Node* p = nodes, *pp = nullptr ; p ; pp = p, p = p->next ) {
        if (p->key == key) {
            if (pp)
                pp->next = p->next;
            else
                nodes = p->next;
            js_free(p);
            return true;
        }
    }
    return false;
}

#endif // JS_DISASM_ARM

#if defined(VARAN_THUMB2) && defined(JS_SIMULATOR_ARM)
// SIMULATOR-ONLY. The UDF encoder and its emit-time census are triage instruments for the sim
// host; on a device build emitUdf() MOZ_CRASHes instead of encoding, so neither is reachable.
static uint32_t EncodeUdfT2(uint32_t code);   // forward decl (defined near the ALU encoders)
// Emit-time UDF census (P1.2a Task B). Non-static so the simulator can dump it just before it
// MOZ_CRASHes on a UDF -- one run then captures every gap emitted during compilation, including
// trampoline generation (JIT init, before any script runs). code is 12 bits (imm4:imm12 room).
uint32_t gVaranUdfEmitCounts[4096] = { 0 };
void
VaranDumpUdfCensus()
{
    fprintf(stderr, "=== VARAN-UDF-CENSUS begin (emit-time, per code) ===\n");
    for (uint32_t c = 0; c < 4096; c++) {
        if (gVaranUdfEmitCounts[c])
            fprintf(stderr, "VARAN-UDF-CENSUS code=0x%03x count=%u\n", c, gVaranUdfEmitCounts[c]);
    }
    fprintf(stderr, "=== VARAN-UDF-CENSUS end ===\n");
}
#endif

// Write a blob of binary into the instruction stream.
BufferOffset
Assembler::writeInst(uint32_t x)
{
#if defined(VARAN_THUMB2)
    // BLANKET (P1.2a): every remaining caller passes an A32 word from an unconverted EncodeX.
    // NEVER emit A32 -- mixed-ISA buffers are undecodable (ISA = T bit / branch bit0, not bytes).
    // Emit a coded wide UDF instead. code = auto-category(0x2)<<8 | A32 bits[27:20] (the
    // condition-independent type+opcode region -> maps to the instruction class in the harvest).
    return emitUdf(0x200u | ((x >> 20) & 0xff));
#else
    BufferOffset offs = m_buffer.putInt(x);
# ifdef JS_DISASM_ARM
    spew(m_buffer.getInstOrNull(offs));
# endif
    return offs;
#endif
}

#if defined(VARAN_THUMB2)
BufferOffset
Assembler::writeInstT2(uint32_t x)
{
    // Raw emit of an already-encoded (halfword-swapped) Thumb-2 word. No spew: JS_DISASM_ARM
    // decodes A32 and would print garbage for Thumb-2 (use llvm-objdump, directive #3).
    return m_buffer.putInt(x);
}
#if defined(VARAN_THUMB2)
// ADR (T3) -- `addw <Rd>, pc, #imm12`, i.e. Rd = Align(PC,4) + imm12.
//   hw0 = 0xF20F | (i << 10)        hw1 = (imm3 << 12) | (Rd << 8) | imm8   [imm12 = i:imm3:imm8]
// Oracle-verified: `addw r0, pc, #9` = 0f f2 09 00.
//
// This is the ONLY legal way for this backend to materialise a PC-relative address in a wide
// instruction. Both A32-style alternatives are UNPREDICTABLE in T32 and LLVM's assembler REFUSES
// both: `mov.w rd, pc` (MOV.W cannot take PC as a source -- only the 16-bit T1 `mov rd, pc` may)
// and `add.w rd, pc, #imm` (the modified-immediate ADD cannot take Rn = PC; with Rn = PC the
// encoding IS this ADR).
BufferOffset
Assembler::as_adr(Register rd, uint32_t imm12)
{
    MOZ_ASSERT(imm12 <= 0xfff);
    uint32_t hw0 = 0xF20Fu | (((imm12 >> 11) & 1) << 10);
    uint32_t hw1 = (((imm12 >> 8) & 7) << 12) | (uint32_t(rd.code()) << 8) | (imm12 & 0xff);
    return writeInstT2((hw1 << 16) | hw0);
}
#endif

#if defined(VARAN_THUMB2)
// Failure primitive for the emit-contract guards (Batch E STEP 5). Deliberately split:
//
//  - SIMULATOR build: emit a coded UDF. That keeps the guards inside the existing census
//    (gVaranUdfEmitCounts / HARVEST) and lets a jit-test run continue and report rather than
//    aborting the shell, which is how every other gap in this port has been triaged.
//  - DEVICE build: MOZ_CRASH. EncodeUdfT2 carries `#error` for !JS_SIMULATOR_ARM, so a UDF-based
//    guard is STRUCTURALLY ABSENT from a device build -- i.e. absent from the only place an
//    UNPREDICTABLE instruction actually does damage. Crashing at emit time is strictly better than
//    shipping an instruction the architecture does not define.
//
// Codes 0x50x/0x51x are free: the in-use bands are 0x100-0x13f, the whole 0x2xx auto-band
// (0x200 | A32 bits[27:20]), 0x300/0x301 and 0x3F2.
BufferOffset
Assembler::varanRejectPcField(uint32_t code)
{
# if defined(JS_SIMULATOR_ARM)
    // KNOWN LIVE TRIP (FACT, pinned by emit-site census 2026-07-21): code 0x501 fires from
    // MacroAssembler::wasmEmitTrapOutOfLineCode -> MacroAssembler::farJumpWithPatch, whose
    // `ma_add(pc, scratch, pc)` writes PC from a wide data-proc -- illegal in T32. The guard is
    // RIGHT; the code is wrong. farJumpWithPatch is wasm-tier and deferred as a unit together with
    // patchFarJump/repatchFarJump and GenerateInterruptExit, and the debug-63 recon already
    // classified the handful of wasm-using tests under debug/ as that same deferral, so this trip
    // does not add failures -- it relabels an already-failing path with a more accurate cause.
    // Do NOT exempt the site to quiet it: the exemption would have to be removed again the moment
    // the wasm batch converts it.
    return emitUdf(code);
# else
    fprintf(stderr, "VARAN: emit-contract violation 0x%03x -- PC in a register field with no legal "
                    "T32 encoding\n", code);
    MOZ_CRASH("VARAN: illegal PC-in-register-field emission");
# endif
}
#endif

BufferOffset
Assembler::varanUnsupported(uint32_t code)
{
# if defined(JS_SIMULATOR_ARM)
    // Under the simulator keep the census behaviour: a coded UDF, logged once, which the gate
    // reads. On a real ARM target EncodeUdfT2 does not compile at all, so without this split the
    // guard would VANISH from the device build and the unconverted shape would fall through to
    // whatever followed it. Loud in both worlds or it is not a guard.
    return emitUdf(code);
# else
    fprintf(stderr, "VARAN: unsupported encoder shape 0x%03x -- no converted Thumb-2 form\n", code);
    MOZ_CRASH("VARAN: unsupported Thumb-2 encoder shape");
# endif
}

BufferOffset
Assembler::emitUdf(uint32_t code)
{
    // ★ emitUdf IS ITSELF THE SPLIT PRIMITIVE (device-enablement, 2026-07-22).
    //
    // EncodeUdfT2 used to carry `#error` for a non-simulator target, which made a device build
    // impossible while any encoder path could emit a UDF. The all-corpora census is now zero, so
    // that tripwire has done its job and is retired -- but retiring it must NOT let any guard
    // become a silent no-op on device, and THREE call sites reach here WITHOUT going through
    // varanUnsupported()/varanRejectPcField(): writeInst()'s blanket A32 divert,
    // writeBranchInst()'s, and as_alu()'s invalid-immediate fallthrough.
    //
    // Splitting HERE rather than at those three sites is deliberate: it makes every caller --
    // present and future, direct or indirect -- loud on device by construction, so there is no
    // way to add a UDF-based guard that silently vanishes from the only build where an
    // UNPREDICTABLE instruction actually does damage.
#if defined(JS_SIMULATOR_ARM)
    // Task B: emit-time census. Log each DISTINCT code the first time it is emitted, immediately
    // to stderr -- robust to any later crash (relocation read-back or execution), and captures
    // trampoline generation (JIT init, before any script). The count gives frequency at dump time.
    uint32_t& n = gVaranUdfEmitCounts[code & 0xfff];
    if (n == 0)
        fprintf(stderr, "VARAN-UDF-EMIT code=0x%03x\n", code & 0xfff);
    n++;
    return writeInstT2(EncodeUdfT2(code));
#else
    // DEVICE build: never emit an instruction the architecture does not define. Crash at EMIT
    // time, which is strictly better than shipping a UDF and discovering it when it executes.
    fprintf(stderr, "VARAN: encoder gap reached on a device build -- code=0x%03x\n", code & 0xfff);
    MOZ_CRASH("VARAN: unconverted Thumb-2 encoder path reached on an ARM target");
#endif
}
BufferOffset
Assembler::writeBranchInstT2(uint32_t word, Label* documentation)
{
    // A converted 1-slot branch (B.W/BL/in-range B<c>.W). Mark as a branch for the buffer's pool
    // bookkeeping (mirrors the A32 writeBranchInst markAsBranch=true). No spew (JS_DISASM_ARM is A32).
    return m_buffer.putInt(word, /* markAsBranch = */ true);
}
#endif

BufferOffset
Assembler::writeBranchInst(uint32_t x, Label* documentation)
{
#if defined(VARAN_THUMB2)
    // BLANKET (P1.2a): the second A32 emit path. Branches must not emit A32 either -> coded UDF.
    return emitUdf(0x200u | ((x >> 20) & 0xff));
#else
    BufferOffset offs = m_buffer.putInt(x, /* markAsBranch = */ true);
# ifdef JS_DISASM_ARM
    spewBranch(m_buffer.getInstOrNull(offs), documentation);
# endif
    return offs;
#endif
}

// Allocate memory for a branch instruction, it will be overwritten
// subsequently and should not be disassembled.

BufferOffset
Assembler::allocBranchInst()
{
#if defined(VARAN_THUMB2)
    // Placeholder must be a decodable Thumb-2 word (the buffer is pure T2). NOP.W = 0xF3AF8000 ->
    // stored 0x8000F3AF. Overwritten in place by as_b/as_bl/varanPatchCondBranch2 before execution.
    return m_buffer.putInt(0x8000F3AFu, /* markAsBranch = */ true);
#else
    return m_buffer.putInt(Always | InstNOP::NopInst, /* markAsBranch = */ true);
#endif
}

void
Assembler::WriteInstStatic(uint32_t x, uint32_t* dest)
{
    MOZ_ASSERT(dest != nullptr);
    *dest = x;
}

void
Assembler::haltingAlign(int alignment)
{
    // TODO: Implement a proper halting align.
    nopAlign(alignment);
}

void
Assembler::nopAlign(int alignment)
{
    m_buffer.align(alignment);
}

BufferOffset
Assembler::as_nop()
{
#if defined(VARAN_THUMB2)
    // NOP.W (0x8000F3AF) -- the SAME InstNOP word ToggleCall writes for the disabled toggled-call slot
    // (toggledCall's disabled path flows ma_nop -> as_nop); an A32 NOP here would be an illegal Thumb
    // word AND != ToggleCall's InstNOP, breaking the toggle round-trip.
    return writeInstT2(0x8000F3AFu);
#else
    return writeInst(0xe320f000);
#endif
}

static uint32_t
EncodeAlu(Register dest, Register src1, Operand2 op2, ALUOp op, SBit s, Assembler::Condition c)
{
    return (int)op | (int)s | (int)c | op2.encode() |
           ((dest == InvalidReg) ? 0 : RD(dest)) |
           ((src1 == InvalidReg) ? 0 : RN(src1));
}

#if defined(VARAN_THUMB2)
// VARAN Phase 1 (P1.1): Thumb-2 T3 data-processing (shifted register) encoding. Every value
// below was VERIFIED against llvm-mc via t2oracle.sh (2026-07-19), not read from the ARM ARM.
// NOT a flat table: six A32 data-processing ALUOps have no direct T32 opcode -- they are
// special forms of others (TST/TEQ/CMP/CMN = base op + S, Rd=1111; MOV = ORR, Rn=1111;
// MVN = ORN, Rn=1111). RSC has no T32 form at all (synthesise later, P1.2).
struct VaranT2Alu { uint32_t op4; bool forceS; bool rnIsPC; bool rdIsPC; bool valid; };
static VaranT2Alu
VaranAluOpToT3(ALUOp op)
{
    switch (op) {
      case OpAnd: return { 0x0, false, false, false, true };
      case OpBic: return { 0x1, false, false, false, true };
      case OpOrr: return { 0x2, false, false, false, true };
      case OpMov: return { 0x2, false, true,  false, true };  // ORR, Rn=1111
      case OpEor: return { 0x4, false, false, false, true };
      case OpAdd: return { 0x8, false, false, false, true };
      case OpAdc: return { 0xA, false, false, false, true };
      case OpSbc: return { 0xB, false, false, false, true };
      case OpSub: return { 0xD, false, false, false, true };
      case OpRsb: return { 0xE, false, false, false, true };
      case OpMvn: return { 0x3, false, true,  false, true };  // ORN, Rn=1111
      case OpTst: return { 0x0, true,  false, true,  true };  // AND, S, Rd=1111
      case OpTeq: return { 0x4, true,  false, true,  true };  // EOR, S, Rd=1111
      case OpCmp: return { 0xD, true,  false, true,  true };  // SUB, S, Rd=1111
      case OpCmn: return { 0x8, true,  false, true,  true };  // ADD, S, Rd=1111
      case OpRsc: default: return { 0, false, false, false, false };  // no T32 form
    }
}
// Wide T3, register operand, LSL #0. Returns the 32-bit value whose LITTLE-ENDIAN store by
// writeInst() yields the correct Thumb-2 byte order (hw0 first): hence (hw1<<16)|hw0.
static uint32_t
EncodeAluT2Reg(Register dest, Register src1, Register rm, ALUOp op, SBit s,
               uint32_t shType = 0, uint32_t shAmt = 0)
{
    VaranT2Alu p = VaranAluOpToT3(op);
    MOZ_ASSERT(p.valid);
    uint32_t setFlags = (p.forceS || s == SetCC) ? 1 : 0;
    uint32_t rn = p.rnIsPC ? 0xf : (src1 == InvalidReg ? 0 : src1.code());
    uint32_t rd = p.rdIsPC ? 0xf : (dest == InvalidReg ? 0 : dest.code());
    uint32_t imm3 = (shAmt >> 2) & 7, imm2 = shAmt & 3;                 // T3 shift = imm3:imm2 (5 bits)
    uint32_t hw0 = (0x75u << 9) | (p.op4 << 5) | (setFlags << 4) | rn;  // 1110101 op4 S Rn
    uint32_t hw1 = (imm3 << 12) | (rd << 8) | (imm2 << 6) | ((shType & 3) << 4) | rm.code();
    return (hw1 << 16) | hw0;
}
// Wide UDF (T2): hw0 = 0xF7F0|imm4, hw1 = 0xA000|imm12 (imm = imm4:imm12). Oracle-verified.
// Every not-yet-converted encoder shape emits one of these carrying an identifying `code`, so
// the buffer stays PURE Thumb-2 (never mixed A32) and each gap fails LOUD at a known point --
// the simulator decodes it to a diagnostic and jit-tests harvest which gaps are hot (P1.2 order).
#if defined(JS_SIMULATOR_ARM)
static uint32_t
EncodeUdfT2(uint32_t code)
{
    // ★ THE `#error` THAT LIVED HERE IS RETIRED (2026-07-22). It blocked any ARM-target build
    // while an encoder path could emit a UDF; the all-corpora census (Baseline + wasm + asm.js +
    // --ion-eager, 5440 tests) is now ZERO, so it has served its purpose. The safety it provided
    // did NOT disappear: emitUdf() is now the split primitive and MOZ_CRASHes on a device build,
    // so a gap still fails loud -- at emit time, in the build where it matters.
    //
    // This helper is now only reachable from emitUdf()'s simulator arm.
    uint32_t hw0 = 0xf7f0u | ((code >> 12) & 0xf);
    uint32_t hw1 = 0xa000u | (code & 0xfff);
    return (hw1 << 16) | hw0;
}
#endif // JS_SIMULATOR_ARM

// ---- Thumb-2 branch encode/decode (P1.2b Group 2) ------------------------------------------
// DIRECT PORT of varan-jit/sim-host/BRANCH-ENCODING.md: Task B 13/13 oracle byte-match at
// positive/negative/boundary offsets, Task A chain round-trip 12011 offsets / 0 failures. Do not
// re-derive. `off` = target - (branch+4), even (Thumb-2 PC = insn+4). Returns (hw1<<16)|hw0 so a
// little-endian store yields hw0 first. NOP.W = 0xF3AF8000 -> stored word 0x8000F3AF (oracle-verified).
static const uint32_t VARAN_NOPW_WORD = 0x8000F3AFu;
// Byte-value sentinel carried in an unbound forward-branch's immediate field, marking the chain
// TAIL. Chain links are previous-branch buffer offsets (small, >= 0, even); a real branch distance
// is never read for validity (only chain links are). 0x00800000 mirrors the A32 BOffImm sentinel;
// it fits the B.W (+-16MB) slot that always carries the conditional chain link (see nextLink/bind).
static const int32_t VARAN_BOFF_INVALID = 0x00800000;

static bool VaranBwInRange(int32_t off)  { return (off & 1) == 0 && off >= -16777216 && off <= 16777214; }
static bool VaranBccInRange(int32_t off) { return (off & 1) == 0 && off >= -1048576  && off <= 1048574;  }

// B.W (T4, uncond) / BL (T1): 24-bit split with the J-bit XOR (I1=NOT(J1)^S ...). isBL picks hw1 base.
static uint32_t
EncodeBranchImmT2(int32_t off, bool isBL)
{
    MOZ_ASSERT(VaranBwInRange(off));
    uint32_t v = (uint32_t(off) >> 1) & 0xffffff;
    uint32_t S = (v >> 23) & 1, I1 = (v >> 22) & 1, I2 = (v >> 21) & 1;
    uint32_t imm10 = (v >> 11) & 0x3ff, imm11 = v & 0x7ff;
    uint32_t J1 = (I1 ^ 1) ^ S, J2 = (I2 ^ 1) ^ S;
    uint32_t hw0 = 0xF000u | (S << 10) | imm10;
    uint32_t hw1 = (isBL ? 0xD000u : 0x9000u) | (J1 << 13) | (J2 << 11) | imm11;
    return (hw1 << 16) | hw0;
}
// B<c>.W (T3, cond): 20-bit DIRECT split (no XOR). cond = Assembler::Condition >> 28 (0..13; AL/14 uses T4).
static uint32_t
EncodeBccT2(int32_t off, uint32_t cond)
{
    // Range guard. Every caller now feeds this in range: jumpWithPatch reserves a 2-slot site, so
    // varanPatchCondBranch2/VaranComputeJump2 only invoke EncodeBccT2 in the in-range arm (or with a
    // fixed +4 skip); as_alu's conditional branch-over uses off=4; the placeholder uses off=-4. A
    // >+-1MB conditional target now takes the 2-slot invert+B.W fallback instead of reaching here, so
    // this is a debug assert, not a release crash.
    MOZ_ASSERT(VaranBccInRange(off));
    MOZ_ASSERT(cond < 14);
    uint32_t v = (uint32_t(off) >> 1) & 0xfffff;
    uint32_t imm11 = v & 0x7ff, imm6 = (v >> 11) & 0x3f;
    uint32_t J1 = (v >> 17) & 1, J2 = (v >> 18) & 1, S = (v >> 19) & 1;
    uint32_t hw0 = 0xF000u | (S << 10) | (cond << 6) | imm6;
    uint32_t hw1 = 0x8000u | (J1 << 13) | (J2 << 11) | imm11;
    return (hw1 << 16) | hw0;
}

// ---- THE CONDITIONAL BRANCH-OVER PRIMITIVE ----
//
// Predication choice for the whole backend: B<!c>.W over an unconditional body, NEVER IT (the
// wide-only invariant permanently rejects IT, and the simulator has no IT decoder). Every
// conditional encoder in this file follows the same three lines:
//
//     AutoForbidPools afp(this, <1 + worst-case body words>);   // load-bearing, see below
//     uint32_t inv = uint32_t(InvertCondition(c)) >> 28;
//     BufferOffset br = writeInstT2(EncodeBccT2(4, inv));       // placeholder skip
//     <emit the body with c == Always>
//     varanPatchCondSkip(br, inv);
//     return br;
//
// The skip is a PLACEHOLDER patched afterwards rather than a hard-coded 4, because several bodies
// are not one instruction (the register-offset load/store synth reaches three) and a stale constant
// would land the taken path in the MIDDLE of its own body. AutoForbidPools is not decoration: with-
// out it the buffer may dump a constant pool between the branch and the body, and the taken path
// falls through into pool DATA.
void
Assembler::varanPatchCondSkip(BufferOffset br, uint32_t inv)
{
    // OOM guard, not defensive padding: once the buffer is out of memory putInt stops advancing,
    // so the body contributes 0 bytes and `skip` goes <= 0. Patching (or asserting on) a dead
    // buffer is meaningless -- the caller checks oom() and throws the code away.
    if (oom())
        return;
    int32_t skip = int32_t(nextOffset().getOffset()) - int32_t(br.getOffset()) - 4;
    MOZ_ASSERT(skip > 0 && (skip & 1) == 0);
    editSrc(br)->varanSetRaw(EncodeBccT2(skip, inv));
}

// LSL/LSR/ASR/ROR (register), T2 -- the ONLY Thumb-2 form of a register-CONTROLLED shift. There is
// no register-controlled-shift variant of general data-processing in T32 (A32 has one for every
// data-proc op), which is why the 0x11d seam needs a synth rather than a translation.
//   hw0 = 0xFA00 | (type << 5) | (S << 4) | Rn      hw1 = 0xF000 | (Rd << 8) | Rm
//   type: 0 LSL / 1 LSR / 2 ASR / 3 ROR;  Rn = value shifted, Rm = shift AMOUNT.
// Oracle-verified: lsl.w r0,r1,r2 = 01 fa 02 f0; lsls.w r0,r1,r2 = 11 fa 02 f0;
//                  asrs.w r3,r4,r5 = 54 fa 05 f3.
static uint32_t
EncodeShiftRegT2(Register rd, Register rvalue, Register ramount, uint32_t type, bool setCC)
{
    MOZ_ASSERT(type < 4);
    uint32_t hw0 = 0xFA00u | (type << 5) | (setCC ? 0x10u : 0u) | uint32_t(rvalue.code());
    uint32_t hw1 = 0xF000u | (uint32_t(rd.code()) << 8) | uint32_t(ramount.code());
    return (hw1 << 16) | hw0;
}

// The only two words a VARAN toggled-jump slot0 may ever hold (see MacroAssemblerARMCompat::
// toggledJump, Assembler::ToggleToJmp/ToggleToCmp). Both are wide, so the 2-slot footprint is
// invariant under toggling.
//   NOP.W  -> fall into slot1 -> branch TAKEN  (enabled)
//   B.W +4 -> skip slot1      -> fall through  (disabled)
// EncodeBranchImmT2's offset is relative to PC (= slot + 4), so +4 lands at slot0+8, i.e. exactly
// past slot1 -- the same convention VaranComputeJump2 uses for its `EncodeBccT2(4, inv)` skip.
// Deriving the skip word from the encoder (rather than hard-coding a magic constant) keeps it
// covered by the branch oracle that already byte-matches EncodeBranchImmT2 against llvm; the
// enabled word reuses the existing oracle-verified VARAN_NOPW_WORD rather than restating it.
static uint32_t VaranToggleSkipW() { return EncodeBranchImmT2(4, /* isBL = */ false); }

// One decoder feeds IsTHIS / extractImm / extractCond / the simulator. Classifies ONLY the wide
// words this backend emits with hw1[15]=1 (branches, UDF, NOP.W); movw/movt/ALU-T3 have hw1[15]=0.
// UDF (cond field 15) and NOP.W (cond field 14) share the T3 mask and are excluded by the cond test.
struct VaranBranchDec { bool valid; bool isBL; bool isCond; uint32_t cond; int32_t off; };
static VaranBranchDec
DecodeBranchT2(uint32_t word)
{
    VaranBranchDec d = { false, false, false, 0, 0 };
    uint32_t hw0 = word & 0xffff, hw1 = (word >> 16) & 0xffff;
    if ((hw0 & 0xF800u) != 0xF000u) return d;   // not a 32-bit Thumb-2 prefix
    if ((hw1 & 0x8000u) == 0)       return d;   // hw1[15]=0 -> data-processing, not branch/misc
    uint32_t sel = hw1 & 0xD000u;               // bits 15,14,12
    if (sel == 0x9000u || sel == 0xD000u) {     // T4 B.W (0x9) / T1 BL (0xD)
        uint32_t S = (hw0 >> 10) & 1, imm10 = hw0 & 0x3ff;
        uint32_t J1 = (hw1 >> 13) & 1, J2 = (hw1 >> 11) & 1, imm11 = hw1 & 0x7ff;
        uint32_t I1 = (J1 ^ 1) ^ S, I2 = (J2 ^ 1) ^ S;
        uint32_t v = (S << 23) | (I1 << 22) | (I2 << 21) | (imm10 << 11) | imm11;
        d.valid = true; d.isBL = (sel == 0xD000u);
        d.off = int32_t(v << 8) >> 7;           // sign-extend 24-bit, then <<1
        return d;
    }
    if (sel == 0x8000u) {                        // T3 B<c>.W (0x8) -- but exclude UDF/NOP.W by cond
        uint32_t cond = (hw0 >> 6) & 0xf;
        if (cond >= 14) return d;                // 14=AL never a T3; 15=UDF/reserved
        uint32_t S = (hw0 >> 10) & 1, imm6 = hw0 & 0x3f;
        uint32_t J1 = (hw1 >> 13) & 1, J2 = (hw1 >> 11) & 1, imm11 = hw1 & 0x7ff;
        uint32_t v = (S << 19) | (J2 << 18) | (J1 << 17) | (imm6 << 11) | imm11;
        d.valid = true; d.isCond = true; d.cond = cond;
        d.off = int32_t(v << 12) >> 11;          // sign-extend 20-bit, then <<1
        return d;
    }
    return d;
}

// Bridge used by the InstBImm/InstBLImm constructors (declared in the header). BOffImm carries a
// BYTE value `v` (a real distance dest-branch, OR a chain-link buffer offset, OR the invalid
// sentinel). The emitted branch's execution displacement is v (target = branch+v), so the encoded
// field is off = v-4; DecodeBranchT2 recovers off, and BOffImm(Instruction) reconstructs v = off+4.
uint32_t
js::jit::VaranEncodeBranchInst(bool isBL, int32_t byteVal, uint32_t condField /*0..14, 14=AL*/)
{
    int32_t off = byteVal - 4;
    if (isBL)
        return EncodeBranchImmT2(off, /*isBL=*/true);
    // The invalid sentinel (chain tail) is only ever stored in a B.W/BL slot, so force B.W even if
    // a condition is nominally present -- it would not fit a B<c>.W (+-1MB) field.
    if (condField >= 14 || byteVal == VARAN_BOFF_INVALID)
        return EncodeBranchImmT2(off, /*isBL=*/false);   // unconditional B.W (T4)
    return EncodeBccT2(off, condField);          // conditional placeholder / in-range B<c>.W
}
// Reconstruct the BYTE value stored in a branch word (inverse of VaranEncodeBranchInst). Returns
// VARAN_BOFF_INVALID if `word` is not a branch this backend emits.
int32_t
js::jit::VaranDecodeBranchByteVal(uint32_t word)
{
    VaranBranchDec d = DecodeBranchT2(word);
    if (!d.valid) return VARAN_BOFF_INVALID;
    return d.off + 4;
}
// True iff `word` is a branch this backend emits; *condField = 0..13 for B<c>.W, 14 (AL) for B.W/BL.
bool
js::jit::VaranDecodeBranchCond(uint32_t word, uint32_t* condField)
{
    VaranBranchDec d = DecodeBranchT2(word);
    if (!d.valid) return false;
    *condField = d.isCond ? d.cond : 14u;
    return true;
}
int
js::jit::VaranBranchKind(uint32_t word)
{
    VaranBranchDec d = DecodeBranchT2(word);
    if (!d.valid) return 0;
    return d.isBL ? 2 : 1;
}
uint32_t
Assembler::varanPeekWord(int byteOffset)
{
    return editSrc(BufferOffset(byteOffset))->encode();
}
#endif

BufferOffset
Assembler::as_alu(Register dest, Register src1, Operand2 op2,
                  ALUOp op, SBit s, Condition c)
{
#if defined(VARAN_THUMB2)
    // VARAN Phase 1 GATE SEAM (encoder layer). Wide Thumb-2 emission.
    // CONDITIONAL ALU -> B<!c>.W branch-over the unconditional body (predication choice: branch-over,
    // never IT). The body is exactly one wide (4-byte) T2 instruction -- converted or UDF -- so the
    // skip distance is 4. When !c the B<!c>.W skips the body; when c it falls through and executes.
    if (c != Always) {
        // The body is NO LONGER always one instruction: the register-controlled-shift synth added
        // below emits `lsl.w ip, rm, rs` + the ALU for any op other than OpMov. A hardcoded skip of 4
        // would land in the MIDDLE of that body and execute the ALU unconditionally, so the skip is
        // emitted as a placeholder and patched once the real body size is known (same shape as
        // varanEmitDtr's conditional arm).
        //
        // AutoForbidPools is load-bearing, not decoration: without it the buffer may dump a constant
        // pool between the branch and the body, and the taken path would fall through into pool DATA.
        // 3 = branch + the 2-instruction worst-case body.
        VaranForbidPoolsIfOutermost afp(this, 3);
        uint32_t inv = uint32_t(Assembler::InvertCondition(c)) >> 28;
        BufferOffset ret = writeInstT2(EncodeBccT2(4, inv));   // placeholder skip, patched below
        as_alu(dest, src1, op2, op, s, Always);                // the body (skipped unless c holds)
        varanPatchCondSkip(ret, inv);
        return ret;
    }
    // ---- EMIT-CONTRACT GUARD (Batch E STEP 5): no PC in a wide data-processing register field ----
    //
    // This is the EMIT-side twin of the decode-side loud-else added in Batch B. Three of the four
    // illegal forms the PC audit found were emitted SILENTLY; a guard here turns "assembles, runs in
    // the simulator, UNPREDICTABLE on silicon" into a loud failure at the moment of emission.
    //
    // A blanket "reject Rd==15 or Rn==15" would BREAK WORKING CODE, so the test keys off the
    // SEMANTIC op, not the raw field: in T32 the six special forms encode a 15 legitimately, and
    // VaranAluOpToT3 already flags exactly which --
    //     rnIsPC : OpMov / OpMvn      (ORR / ORN with Rn=1111)
    //     rdIsPC : OpTst/OpTeq/OpCmp/OpCmn (base op + S with Rd=1111)
    // Those callers pass InvalidReg for the field they do not own (as_mov/as_mvn pass src1 =
    // InvalidReg; as_cmp/as_cmn/as_teq/as_tst pass dest = InvalidReg), and the 15 is FORCED by the
    // encoder below, never supplied by the caller. So any r15 arriving through `dest`, `src1` or the
    // Operand2 register is a caller bug with no legal T32 encoding.
    //
    // NB this lives in as_alu, NOT in an encoder helper: there is no single "EncodeAluT2" -- the
    // modified-immediate arm is written inline further down, so a guard in EncodeAluT2Reg would
    // silently miss every immediate emission.
    {
        VaranT2Alu vguard = VaranAluOpToT3(op);
        if (!vguard.rdIsPC && dest != InvalidReg && dest.code() == 15)
            return varanRejectPcField(0x501);   // Rd = PC
        if (!vguard.rnIsPC && src1 != InvalidReg && src1.code() == 15)
            return varanRejectPcField(0x502);   // Rn = PC
        if (op2.isO2Reg()) {
            uint32_t vo2 = op2.encode();
            // Rm(3:0) always; Rs(11:8) only for a register-CONTROLLED shift (bit4).
            if ((vo2 & 0xf) == 0xf || ((vo2 & 0x10) && ((vo2 >> 8) & 0xf) == 0xf))
                return varanRejectPcField(0x503);   // Rm / Rs = PC
        }
    }

    // Unconditional register-operand, immediate shift (Batch 4 extends P1.1 LSL#0 to any imm shift --
    // closes the residual 0x11x shifted-register). o2 = Rm(3:0) | rrs(bit4) | type(6:5) | shAmt(11:7).
    // Register-CONTROLLED shift (rrs bit4=1) has no T3 form -> falls through to UDF.
    if (op2.isO2Reg()) {
        uint32_t o2 = op2.encode();
        VaranT2Alu p = VaranAluOpToT3(op);
        if (p.valid && (o2 & 0x10) == 0)
            return writeInstT2(EncodeAluT2Reg(dest, src1, Register::FromCode(o2 & 0xf), op, s,
                                              (o2 >> 5) & 3, (o2 >> 7) & 0x1f));
    }
    // Unconditional modified-immediate (Batch 1). op2 carries (ThumbExpandImm control | IsImmOp2);
    // control = op2.encode() & 0xfff (IsImmOp2 = bit25). op4 map = VaranAluOpToT3, identical to the
    // register form. hw0 = 0xF000|(i<<10)|(op4<<5)|(S<<4)|Rn; hw1 = (imm3<<12)|(Rd<<8)|imm8.
    if (op2.isImm8() && !op2.invalid()) {
        VaranT2Alu p = VaranAluOpToT3(op);
        if (p.valid) {
            uint32_t control = op2.encode() & 0xfff;
            uint32_t i = (control >> 11) & 1, imm3 = (control >> 8) & 7, imm8 = control & 0xff;
            uint32_t setFlags = (p.forceS || s == SetCC) ? 1u : 0u;
            uint32_t rn = p.rnIsPC ? 0xfu : (src1 == InvalidReg ? 0u : src1.code());
            uint32_t rd = p.rdIsPC ? 0xfu : (dest == InvalidReg ? 0u : dest.code());
            uint32_t hw0 = 0xF000u | (i << 10) | (p.op4 << 5) | (setFlags << 4) | rn;
            uint32_t hw1 = (imm3 << 12) | (rd << 8) | imm8;
            return writeInstT2((hw1 << 16) | hw0);
        }
    }
    // Register-CONTROLLED shift (A32 op2 bit4 = 1: `rd = rn <op> rm, <shift> rs`). This is the 0x11d
    // seam -- the hottest gap in the gate -- and is DISTINCT from the immediate shifted-register form
    // closed just above. Thumb-2 has no register-controlled-shift form for general data-processing,
    // only the dedicated LSL/LSR/ASR/ROR (register) instructions, so it must be SYNTHESISED:
    //   op == OpMov (what 0x11d actually is: `mov rd, rm, lsl rs` from ma_lsl/ma_lsr/ma_asr/ma_ror)
    //       -> emit the dedicated shift directly. ONE instruction, no scratch, flags exact.
    //   any other op
    //       -> materialise the shifted value into ip, then run the ordinary register-form ALU on ip.
    // ip/r12 is safe as the scratch: it is non-allocatable, so the allocator never hands it out.
    // A32 field layout of op2: Rs(11:8), type(6:5), bit4 = 1, Rm(3:0).
    if (op2.isO2Reg() && (op2.encode() & 0x10)) {
        uint32_t o2 = op2.encode();
        Register rvalue  = Register::FromCode(o2 & 0xf);           // A32 Rm -> T2 Rn
        Register ramount = Register::FromCode((o2 >> 8) & 0xf);    // A32 Rs -> T2 Rm
        uint32_t type    = (o2 >> 5) & 3;
        if (op == OpMov)
            return writeInstT2(EncodeShiftRegT2(dest, rvalue, ramount, type, s == SetCC));
        writeInstT2(EncodeShiftRegT2(ScratchRegister, rvalue, ramount, type, /* setCC = */ false));
        return as_alu(dest, src1, O2Reg(ScratchRegister), op, s, Always);
    }

    // ---- RSC SYNTH (the last real encoder gap on the Baseline path) ----
    //
    // T32 has no RSC at all. By definition:
    //     rsc rd, rn, op2  ==  rd = op2 - rn - NOT(C)  ==  op2 + NOT(rn) + C
    // which gives two exact rewrites, both flag-faithful:
    //     op2 is a bare register Rm : SBC rd, Rm, rn      (SBC rd,Rn,Rm = Rn - Rm - NOT(C), so the
    //                                                      REVERSE subtract is just swapped operands)
    //     op2 is an immediate       : MVN rd, rn ; ADC rd, rd, #imm
    //
    // The MVN must NOT set flags -- ADC reads C, and MVN with S=0 leaves it alone, so the incoming
    // carry survives into the ADC. Only the ADC/SBC carries `s`, which makes SetCC exact too.
    // No scratch is needed even when dest != src1: the immediate form writes ~src1 into dest first
    // and then accumulates into dest.
    //
    // Sole live caller: MacroAssembler::neg64 -> as_rsb(lo,lo,#0,SetCC); as_rsc(hi,hi,#0) -- the
    // int64 negate. Statically reachable from Baseline JS, so this had to be a synth, not a guard.
    if (op == OpRsc) {
        if (op2.isO2Reg() && (op2.encode() & 0xff0) == 0) {
            // Bare register, no shift and no register-controlled shift: swap into SBC.
            return as_alu(dest, Register::FromCode(op2.encode() & 0xf), O2Reg(src1), OpSbc, s,
                          Always);
        }
        if (op2.isImm8() && !op2.invalid()) {
            as_alu(dest, InvalidReg, O2Reg(src1), OpMvn, LeaveCC, Always);   // rd = ~rn, C intact
            return as_alu(dest, dest, op2, OpAdc, s, Always);                // rd = rd + imm + C
        }
        // A shifted-register RSC would need the shifted value materialised first; no caller emits
        // one, so keep it loud rather than shipping an untested third path.
        return varanUnsupported(0x130u);
    }

    // Remaining unconverted shapes (an invalid immediate) -> coded UDF, NEVER A32.
    // code = ALU(0x1)<<8 | shape<<4 | op-nibble; shape 1=shifted-reg 3=rsc/invalid.
    {
        uint32_t shape = op2.isImm8() ? 0u : (VaranAluOpToT3(op).valid ? 1u : 3u);
        uint32_t code = 0x100u | (shape << 4) | ((uint32_t(op) >> 21) & 0xf);
        return emitUdf(code);
    }
#else
    return writeInst(EncodeAlu(dest, src1, op2, op, s, c));
#endif
}

BufferOffset
Assembler::as_mov(Register dest, Operand2 op2, SBit s, Condition c)
{
    return as_alu(dest, InvalidReg, op2, OpMov, s, c);
}

/* static */ void
Assembler::as_alu_patch(Register dest, Register src1, Operand2 op2, ALUOp op, SBit s,
                        Condition c, uint32_t* pos)
{
#if defined(VARAN_THUMB2)
    // ---- B1: a SILENT A32 emitter ----
    //
    // WriteInstStatic is a bare `*dest = x` -- unlike writeInst() it does NOT divert to a coded
    // UDF, so this would patch a raw A32 word into a Thumb-2 buffer with NO diagnostic at all.
    // That is strictly worse than a UDF and NO census can see it: the emit-time census only
    // records emitUdf() calls. Only the source sweep found this class.
    //
    // Dead today (no callers anywhere in the tree), which is exactly why it needs a guard rather
    // than a conversion -- the same treatment RetargetFarBranch got.
    MOZ_CRASH("VARAN: as_alu_patch is an unconverted A32 patcher (silent A32 emission)");
#else
    WriteInstStatic(EncodeAlu(dest, src1, op2, op, s, c), pos);
#endif
}

/* static */ void
Assembler::as_mov_patch(Register dest, Operand2 op2, SBit s, Condition c, uint32_t* pos)
{
    as_alu_patch(dest, InvalidReg, op2, OpMov, s, c, pos);
}

BufferOffset
Assembler::as_mvn(Register dest, Operand2 op2, SBit s, Condition c)
{
    return as_alu(dest, InvalidReg, op2, OpMvn, s, c);
}

// Logical operations.
BufferOffset
Assembler::as_and(Register dest, Register src1, Operand2 op2, SBit s, Condition c)
{
    return as_alu(dest, src1, op2, OpAnd, s, c);
}
BufferOffset
Assembler::as_bic(Register dest, Register src1, Operand2 op2, SBit s, Condition c)
{
    return as_alu(dest, src1, op2, OpBic, s, c);
}
BufferOffset
Assembler::as_eor(Register dest, Register src1, Operand2 op2, SBit s, Condition c)
{
    return as_alu(dest, src1, op2, OpEor, s, c);
}
BufferOffset
Assembler::as_orr(Register dest, Register src1, Operand2 op2, SBit s, Condition c)
{
    return as_alu(dest, src1, op2, OpOrr, s, c);
}

// Mathematical operations.
BufferOffset
Assembler::as_adc(Register dest, Register src1, Operand2 op2, SBit s, Condition c)
{
    return as_alu(dest, src1, op2, OpAdc, s, c);
}
BufferOffset
Assembler::as_add(Register dest, Register src1, Operand2 op2, SBit s, Condition c)
{
    return as_alu(dest, src1, op2, OpAdd, s, c);
}
BufferOffset
Assembler::as_sbc(Register dest, Register src1, Operand2 op2, SBit s, Condition c)
{
    return as_alu(dest, src1, op2, OpSbc, s, c);
}
BufferOffset
Assembler::as_sub(Register dest, Register src1, Operand2 op2, SBit s, Condition c)
{
    return as_alu(dest, src1, op2, OpSub, s, c);
}
BufferOffset
Assembler::as_rsb(Register dest, Register src1, Operand2 op2, SBit s, Condition c)
{
    return as_alu(dest, src1, op2, OpRsb, s, c);
}
BufferOffset
Assembler::as_rsc(Register dest, Register src1, Operand2 op2, SBit s, Condition c)
{
    return as_alu(dest, src1, op2, OpRsc, s, c);
}

// Test operations.
BufferOffset
Assembler::as_cmn(Register src1, Operand2 op2, Condition c)
{
    return as_alu(InvalidReg, src1, op2, OpCmn, SetCC, c);
}
BufferOffset
Assembler::as_cmp(Register src1, Operand2 op2, Condition c)
{
    return as_alu(InvalidReg, src1, op2, OpCmp, SetCC, c);
}
BufferOffset
Assembler::as_teq(Register src1, Operand2 op2, Condition c)
{
    return as_alu(InvalidReg, src1, op2, OpTeq, SetCC, c);
}
BufferOffset
Assembler::as_tst(Register src1, Operand2 op2, Condition c)
{
    return as_alu(InvalidReg, src1, op2, OpTst, SetCC, c);
}

#if defined(VARAN_THUMB2)
// Emit ONE already-encoded wide word, predicated on `c`.
//
// This is the whole of the "conditional forms" conversion: sxtb/sxth/uxtb/uxth, movw/movt,
// mrs/msr and the multiplies all have a body of exactly one wide instruction, so they need no
// per-site size logic -- only the standard B<!c>.W branch-over (see varanPatchCondSkip). Sites
// whose body can EXPAND (as_alu's register-controlled-shift synth, varanEmitDtr's register-offset
// synth, as_extdtr's LDRD synth) must NOT use this; they write the placeholder themselves so the
// patched skip covers the real body.
//
// The skip is a constant 4 here, but it still goes through varanPatchCondSkip so there is exactly
// one place that computes a skip -- and so the assert fires if a "one-word" body ever grows.
static BufferOffset
VaranEmitCond1(Assembler* a, uint32_t word, Assembler::Condition c)
{
    if (c == Assembler::Always)
        return a->writeInstT2(word);
    VaranForbidPoolsIfOutermost afp(a, 2);
    uint32_t inv = uint32_t(Assembler::InvertCondition(c)) >> 28;
    BufferOffset br = a->writeInstT2(EncodeBccT2(4, inv));
    a->writeInstT2(word);
    a->varanPatchCondSkip(br, inv);
    return br;
}
#endif

static constexpr Register NoAddend = { Registers::pc };

static const int SignExtend = 0x06000070;

enum SignExtend {
    SxSxtb = 10 << 20,
    SxSxth = 11 << 20,
    SxUxtb = 14 << 20,
    SxUxth = 15 << 20
};

// Sign extension operations.
BufferOffset
Assembler::as_sxtb(Register dest, Register src, int rotate, Condition c)
{
#if defined(VARAN_THUMB2)
    // T2 sxtb.w Rd,Rm,ror#(rot*8) -- WIDE form (the 16-bit form violates wide-only). Oracle:
    // sxtb.w r1,r3 = fa4f f183, sxtb.w r1,r3,ror#8 = fa4f f193.
    return VaranEmitCond1(this,
        ((0xf080u | (dest.code() << 8) | ((rotate & 3) << 4) | src.code()) << 16) | 0xfa4fu, c);
#else
    return writeInst((int)c | SignExtend | SxSxtb | RN(NoAddend) | RD(dest) | ((rotate & 3) << 10) | src.code());
#endif
}
BufferOffset
Assembler::as_sxth(Register dest, Register src, int rotate, Condition c)
{
#if defined(VARAN_THUMB2)
    // sxth.w r1,r3 = fa0f f183
    return VaranEmitCond1(this,
        ((0xf080u | (dest.code() << 8) | ((rotate & 3) << 4) | src.code()) << 16) | 0xfa0fu, c);
#else
    return writeInst((int)c | SignExtend | SxSxth | RN(NoAddend) | RD(dest) | ((rotate & 3) << 10) | src.code());
#endif
}
BufferOffset
Assembler::as_uxtb(Register dest, Register src, int rotate, Condition c)
{
#if defined(VARAN_THUMB2)
    // uxtb.w r1,r3 = fa5f f183
    return VaranEmitCond1(this,
        ((0xf080u | (dest.code() << 8) | ((rotate & 3) << 4) | src.code()) << 16) | 0xfa5fu, c);
#else
    return writeInst((int)c | SignExtend | SxUxtb | RN(NoAddend) | RD(dest) | ((rotate & 3) << 10) | src.code());
#endif
}
BufferOffset
Assembler::as_uxth(Register dest, Register src, int rotate, Condition c)
{
#if defined(VARAN_THUMB2)
    // uxth.w r1,r3 = fa1f f183
    return VaranEmitCond1(this,
        ((0xf080u | (dest.code() << 8) | ((rotate & 3) << 4) | src.code()) << 16) | 0xfa1fu, c);
#else
    return writeInst((int)c | SignExtend | SxUxth | RN(NoAddend) | RD(dest) | ((rotate & 3) << 10) | src.code());
#endif
}

#if !defined(VARAN_THUMB2)   // A32 movw/movt encoders (Thumb-2 uses EncodeMovWT2)
static uint32_t
EncodeMovW(Register dest, Imm16 imm, Assembler::Condition c)
{
    MOZ_ASSERT(HasMOVWT());
    return 0x03000000 | c | imm.encode() | RD(dest);
}

static uint32_t
EncodeMovT(Register dest, Imm16 imm, Assembler::Condition c)
{
    MOZ_ASSERT(HasMOVWT());
    return 0x03400000 | c | imm.encode() | RD(dest);
}
#endif

#if defined(VARAN_THUMB2)
// VARAN P1.2b: movw/movt (T3). imm16 = imm4:i:imm3:imm8; hw0 = base | (i<<10) | imm4
// (base 0xF240 movw / 0xF2C0 movt), hw1 = imm3<<12 | Rd<<8 | imm8. Oracle-verified 2026-07-19.
// Unconditional (T3 has no condition field; a conditional movw/movt is branch-over per W2).
static uint32_t
EncodeMovWT2(Register dest, uint32_t imm16, bool isMovt)
{
    uint32_t imm4 = (imm16 >> 12) & 0xf;
    uint32_t i    = (imm16 >> 11) & 1;
    uint32_t imm3 = (imm16 >> 8) & 0x7;
    uint32_t imm8 = imm16 & 0xff;
    uint32_t hw0 = (isMovt ? 0xf2c0u : 0xf240u) | (i << 10) | imm4;
    uint32_t hw1 = (imm3 << 12) | (uint32_t(dest.code()) << 8) | imm8;
    return (hw1 << 16) | hw0;
}
#endif

// Not quite ALU worthy, but these are useful none the less. These also have
// the isue of these being formatted completly differently from the standard ALU
// operations.
BufferOffset
Assembler::as_movw(Register dest, Imm16 imm, Condition c)
{
#if defined(VARAN_THUMB2)
    // ⚠ A conditional movw/movt must NEVER reach a PATCHABLE site. ma_movPatchable emits the
    // movw+movt pair at a fixed footprint that as_movw_patch / as_movt_patch later rewrite IN
    // PLACE (and PatchDataWithValueCheck reads back); interposing a branch-over would shift both
    // words and silently corrupt every later patch. Audited: all six ma_movPatchable call sites
    // pass Always, so the conditional arm is unreachable from the patchable path -- but that is a
    // property of the callers, not of this function, hence the assert below rather than a comment.
    // (This is the "footprint changes break in-place patchers" class; it has bitten this port.)
    return VaranEmitCond1(this, EncodeMovWT2(dest, imm.decode(), /*isMovt=*/false), c);
#else
    return writeInst(EncodeMovW(dest, imm, c));
#endif
}

/* static */ void
Assembler::as_movw_patch(Register dest, Imm16 imm, Condition c, Instruction* pos)
{
#if defined(VARAN_THUMB2)
    WriteInstStatic(EncodeMovWT2(dest, imm.decode(), /*isMovt=*/false), (uint32_t*)pos);
#else
    WriteInstStatic(EncodeMovW(dest, imm, c), (uint32_t*)pos);
#endif
}

BufferOffset
Assembler::as_movt(Register dest, Imm16 imm, Condition c)
{
#if defined(VARAN_THUMB2)
    // See as_movw: conditional movt is legal here, but never on a patchable pair.
    return VaranEmitCond1(this, EncodeMovWT2(dest, imm.decode(), /*isMovt=*/true), c);
#else
    return writeInst(EncodeMovT(dest, imm, c));
#endif
}

/* static */ void
Assembler::as_movt_patch(Register dest, Imm16 imm, Condition c, Instruction* pos)
{
#if defined(VARAN_THUMB2)
    WriteInstStatic(EncodeMovWT2(dest, imm.decode(), /*isMovt=*/true), (uint32_t*)pos);
#else
    WriteInstStatic(EncodeMovT(dest, imm, c), (uint32_t*)pos);
#endif
}

static const int mull_tag = 0x90;

BufferOffset
Assembler::as_genmul(Register dhi, Register dlo, Register rm, Register rn,
                     MULOp op, SBit s, Condition c)
{
#if defined(VARAN_THUMB2)
    // T2 data-proc long/short multiply (Batch 2 bulk, oracle byte-matched). Rd=dhi, Rn=rm (hw0),
    // Rm=rn (hw1 low), Ra/RdLo=dlo (hw1 high). mul: Ra=1111. LeaveCC only -- T2 has no flag-setting
    // multiply. The condition is handled by VaranEmitCond1's branch-over at the bottom.
    //
    // ⚠ RE-AUDIT FINDING (this batch). The previous conditional arm here was a hand-rolled
    // branch-over that hard-coded a skip of 4 and -- unlike its two siblings in as_alu and
    // varanEmitDtr -- had NO AutoForbidPools. A constant pool dumped between the branch and the
    // multiply would have sent the TAKEN path into pool DATA. Routing through VaranEmitCond1 fixes
    // that by construction. It was latent, not live (no conditional multiply caller exists), and it
    // is exactly the "close one gap, expose the next" case: it only became visible when this site
    // was re-read for conversion.
    if (s == LeaveCC) {
        uint32_t rd = dhi.code(), rnHw0 = rm.code(), rmHw1 = rn.code();
        uint32_t ra = (dlo == InvalidReg) ? 0xfu : dlo.code();
        uint32_t hw0 = 0, hw1 = 0;
        switch (op) {
          case OpmMul:   hw0 = 0xfb00u | rnHw0; hw1 = (0xfu << 12) | (rd << 8) | rmHw1;        break;
          case OpmMla:   hw0 = 0xfb00u | rnHw0; hw1 = (ra   << 12) | (rd << 8) | rmHw1;        break;
          case OpmMls:   hw0 = 0xfb00u | rnHw0; hw1 = (ra   << 12) | (rd << 8) | 0x10u | rmHw1; break;
          case OpmUmull: hw0 = 0xfba0u | rnHw0; hw1 = (ra   << 12) | (rd << 8) | rmHw1;        break;  // ra=RdLo,rd=RdHi
          case OpmSmull: hw0 = 0xfb80u | rnHw0; hw1 = (ra   << 12) | (rd << 8) | rmHw1;        break;
          // OpmMlas / OpmUmlal / OpmSmlal: no converted T2 form. UNREACHABLE -- as_umlal/as_smlal
          // have no callers anywhere in the tree and there is no as_mlas. Loud guard, not a synth.
          default:       return varanUnsupported(0x201);
        }
        return VaranEmitCond1(this, (hw1 << 16) | hw0, c);
    }
    // SetCC (muls/mlas): no T2 form at all. UNREACHABLE -- every as_mul/as_mla/ma_mul call site
    // uses the default LeaveCC, and ma_check_mul gets its overflow condition from as_smull, not
    // from a flag-setting multiply.
    return varanUnsupported(0x201);
#else
    return writeInst(RN(dhi) | maybeRD(dlo) | RM(rm) | rn.code() | op | s | c | mull_tag);
#endif
}
BufferOffset
Assembler::as_mul(Register dest, Register src1, Register src2, SBit s, Condition c)
{
    return as_genmul(dest, InvalidReg, src1, src2, OpmMul, s, c);
}
BufferOffset
Assembler::as_mla(Register dest, Register acc, Register src1, Register src2,
                  SBit s, Condition c)
{
    return as_genmul(dest, acc, src1, src2, OpmMla, s, c);
}
BufferOffset
Assembler::as_umaal(Register destHI, Register destLO, Register src1, Register src2, Condition c)
{
    return as_genmul(destHI, destLO, src1, src2, OpmUmaal, LeaveCC, c);
}
BufferOffset
Assembler::as_mls(Register dest, Register acc, Register src1, Register src2, Condition c)
{
    return as_genmul(dest, acc, src1, src2, OpmMls, LeaveCC, c);
}

BufferOffset
Assembler::as_umull(Register destHI, Register destLO, Register src1, Register src2,
                    SBit s, Condition c)
{
    return as_genmul(destHI, destLO, src1, src2, OpmUmull, s, c);
}

BufferOffset
Assembler::as_umlal(Register destHI, Register destLO, Register src1, Register src2,
                    SBit s, Condition c)
{
    return as_genmul(destHI, destLO, src1, src2, OpmUmlal, s, c);
}

BufferOffset
Assembler::as_smull(Register destHI, Register destLO, Register src1, Register src2,
                    SBit s, Condition c)
{
    return as_genmul(destHI, destLO, src1, src2, OpmSmull, s, c);
}

BufferOffset
Assembler::as_smlal(Register destHI, Register destLO, Register src1, Register src2,
                    SBit s, Condition c)
{
    return as_genmul(destHI, destLO, src1, src2, OpmSmlal, s, c);
}

BufferOffset
Assembler::as_sdiv(Register rd, Register rn, Register rm, Condition c)
{
    return writeInst(0x0710f010 | c | RN(rd) | RM(rm) | rn.code());
}

BufferOffset
Assembler::as_udiv(Register rd, Register rn, Register rm, Condition c)
{
    return writeInst(0x0730f010 | c | RN(rd) | RM(rm) | rn.code());
}

BufferOffset
Assembler::as_clz(Register dest, Register src, Condition c)
{
    MOZ_ASSERT(src != pc && dest != pc);
#if defined(VARAN_THUMB2)
    // T2 clz Rd,Rm (Rm in BOTH hw0[3:0] and hw1[3:0]). Oracle: clz r1,r3 = fab3 f183.
    if (c == Always) {
        uint32_t hw0 = 0xfab0u | src.code();
        uint32_t hw1 = 0xf080u | (dest.code() << 8) | src.code();
        return writeInstT2((hw1 << 16) | hw0);
    }
    // Conditional clz: no T2 predication -> B<!c>.W(4) over the one AL-forced wide clz (branch-over, as
    // as_alu/varanEmitVfp do). Skip = 4 (one wide instruction).
    // ★ A3 POOL GUARD. N=2: the B<!c>.W branch-over + the one wide AL-forced clz body word. Same
    // class as A2 -- the hard-coded skip of 4 is the contract. (0 splits observed on the corpus, but
    // guarded anyway: same-class-by-construction, and the window is identical to A2's.) The
    // recursive as_clz(...,Always) below hits the Always arm, which opens no region of its own; the
    // nesting-safe variant makes that a no-op even if it did.
    VaranForbidPoolsIfOutermost varanAfp(this, 2);
    BufferOffset ret = writeInstT2(EncodeBccT2(4, uint32_t(InvertCondition(c)) >> 28));
    as_clz(dest, src, Always);
    return ret;
#else
    return writeInst(RD(dest) | src.code() | c | 0x016f0f10);
#endif
}

// Data transfer instructions: ldr, str, ldrb, strb. Using an int to
// differentiate between 8 bits and 32 bits is overkill, but meh.

static uint32_t
EncodeDtr(LoadStore ls, int size, Index mode, Register rt, DTRAddr addr, Assembler::Condition c)
{
#if defined(VARAN_THUMB2)
    // Under VARAN this static encoder is reached ONLY via as_dtr_patch <- PatchConstantPoolLoad, i.e.
    // a pc-relative constant-pool LDR-literal (PoolDTR: ldr Rt,[pc,#imm]; PoolBranch: ldr pc,[pc,#imm]).
    // Non-pool loads/stores go through varanEmitDtr (as_dtr). Return the T2 word (hw1<<16)|hw0 so
    // WriteInstStatic stores hw0 first (little-endian).
    MOZ_ASSERT(c == Assembler::Always);          // single 4B T2 LDR-literal slot -- no cond field
    MOZ_ASSERT(mode == Offset);
    MOZ_ASSERT(size == 32);                      // pools are word loads
    uint32_t data = addr.encode();
    bool isReg = (data >> 25) & 1;
    bool up    = (data >> 23) & 1;               // IsUp = 1<<23
    uint32_t imm12 = data & 0xfff;
    uint32_t rtc = rt.code();
    Register base = Register::FromCode((data >> 16) & 0xf);
    MOZ_ASSERT(!isReg);
    MOZ_ASSERT(base == pc);
    MOZ_ASSERT(ls == IsLoad);
    // R1 ALIAS GUARD: a pc-base load with Rt==pc AND imm12==0xfff makes hw1 == 0xF000|0xfff == 0xffff,
    // whose top-16 equals the PoolHeader 0xffff0000 marker -- a stream walker would mis-read the load as
    // a header. AsmPoolMaxOffset==1024 keeps imm12 far under 0xfff, so this must never occur; trip loud.
    MOZ_RELEASE_ASSERT(!(rtc == pc.code() && imm12 == 0xfffu),
                       "VARAN: pc-base LDR literal aliases PoolHeader (Rt==pc, imm12==0xfff)");
    uint32_t hw0 = 0xF85Fu | (up ? 0x80u : 0u);  // ldr.w Rt,[pc,#imm]: 0xF85F(U=0)/0xF8DF(U=1)
    uint32_t hw1 = (rtc << 12) | imm12;
    return (hw1 << 16) | hw0;
#else
    MOZ_ASSERT(mode == Offset ||  (rt != addr.getBase() && pc != addr.getBase()));
    MOZ_ASSERT(size == 32 || size == 8);
    return 0x04000000 | ls | (size == 8 ? 0x00400000 : 0) | mode | c | RT(rt) | addr.encode();
#endif
}

#if defined(VARAN_THUMB2)
// Emit a T2 single load/store (Batch 4). t4base = family T4/reg hw0 base (T3 = |0x80). Addressing is
// decoded from the A32 DTRAddr: base=Rn(19:16), I=bit25 (0 imm / 1 reg), U=bit23 (+/-), imm12(11:0) or
// {Rm(3:0), rrs(4), type(6:5), shAmt(11:7)}. In-range -> single T2; out-of-range / awkward shift ->
// synth into r12 (scratch) then a base load. Conditional load/store -> UDF (rare; ma_ldr is uncond).
BufferOffset
Assembler::varanEmitDtr(uint32_t t4base, Index mode, Register rt, DTRAddr addr, Condition c)
{
    if (c != Always) {
        // Conditional load/store -> B<!c>.W branch-over an unconditional body. NEVER IT: the
        // wide-only invariant permanently rejects it, and the simulator has no IT decoder.
        //
        // This was deferred because "branch-over needs a known body size" and the body is NOT fixed
        // size -- the register-offset synth below expands to as many as three instructions. Rather
        // than duplicate that size logic (which would silently rot the moment the synth changes),
        // emit a PLACEHOLDER branch, emit the body, then patch in the real skip once it is known.
        //
        // AutoForbidPools is load-bearing, not decoration: without it the buffer may dump a constant
        // pool between the branch and the body, and the taken path would fall through into pool DATA.
        // 4 = the 3-instruction worst-case body + the branch itself.
        VaranForbidPoolsIfOutermost afp(this, 4);
        uint32_t inv = uint32_t(InvertCondition(c)) >> 28;
        BufferOffset br = writeInstT2(EncodeBccT2(4, inv));      // placeholder skip, patched below
        varanEmitDtr(t4base, mode, rt, addr, Always);
        varanPatchCondSkip(br, inv);
        return br;
    }
    uint32_t data = addr.encode();
    Register base = Register::FromCode((data >> 16) & 0xf);
    uint32_t rtc = rt.code();
    bool isReg = (data >> 25) & 1;
    bool up    = (data >> 23) & 1;

    // ---- EMIT-CONTRACT GUARDS (Batch E STEP 5) ----
    //
    // D1: Rt == 15 is legal ONLY for a 32-bit LOAD (`ldr.w pc,[...]` is a real interworking branch,
    // and ma_pop(pc) / ma_popn_pc / handleFailureWithHandlerTail all depend on it). It is illegal for
    // every STORE and every sub-word load. `0xf850` is the only 32-bit-load t4base routed here.
    //
    // ⚠ THE ORACLE IS NOT AUTHORITATIVE FOR THIS ONE. LLVM happily assembles `str.w pc,[r0,#4]`
    // (c0 f8 04 f0). STR with Rt==15 is UNPREDICTABLE in T32 and we target real Tegra 3 silicon, so
    // this guard rests on the architecture rule -- exactly like the existing as_dtm 0x28c guard.
    // Do not "verify" it away with the assembler.
    if (rtc == 15 && t4base != 0xf850u)
        return varanRejectPcField(0x511);

    // D2: register-offset form with Rn == 15 or Rm == 15 has NO T32 encoding. Our encoder would emit
    // hw0 = t4base|15 = 0xF85F/0xF8DF, which DECODES AS AN LDR-LITERAL and swallows hw1 as a bogus
    // imm12 -- silent garbage, not a fault. This is the shape behind the Ion table-switch defect.
    if (isReg && (base.code() == 15 || (data & 0xf) == 15))
        return varanRejectPcField(0x512);

    // D3: a pc-BASE immediate form outside the one shape that is a genuine LDR-literal
    // (mode == Offset && U == 1). A negative pc offset takes the T4 arm, whose hw1 bits 11/10 are
    // imm12 bits in the literal encoding, so the assembled instruction means something else entirely.
    // DELIBERATELY NOT GUARDED: mode == Offset && up && base == pc, which is farJumpWithPatch's
    // `ma_ldr(DTRAddr(pc, DtrOffImm(0)), scratch)`. That is a LEGAL ldr.w rt,[pc,#imm12]; it is
    // semantically wrong (wasm tier, deferred as a unit) but guarding it would abort wasm compilation
    // on an instruction LLVM accepts. DtrOffImm(0) yields IsUp, so this provably does not fire there.
    if (!isReg && base.code() == 15 && !(mode == Offset && up))
        return varanRejectPcField(0x513);

    const Register ip = Register::FromCode(12);   // synth scratch

    if (isReg) {
        uint32_t rm = data & 0xf, rrs = (data >> 4) & 1, shType = (data >> 5) & 3, shAmt = (data >> 7) & 0x1f;
        if (up && mode == Offset && rrs == 0 && shType == 0 /*LSL*/ && shAmt <= 3) {   // native T2 reg form
            uint32_t hw0 = t4base | base.code();
            uint32_t hw1 = (rtc << 12) | (shAmt << 4) | rm;
            return writeInstT2((hw1 << 16) | hw0);
        }
        // Synth: compute the (possibly-shifted, possibly-negated) index into ip, then [base, ip].
        MOZ_ASSERT(base != ip && rt != ip);
        Register rmr = Register::FromCode(rm);
        if (rrs == 0 && shAmt != 0)
            as_mov(ip, O2RegImmShift(rmr, ShiftType(shType), shAmt));   // ip = Rm shifted
        else
            as_mov(ip, O2Reg(rmr));                                     // ip = Rm
        if (!up)
            as_rsb(ip, ip, O2Reg(base));   // ip = base - ip   (negative reg offset)
        else
            as_add(ip, base, O2Reg(ip));   // ip = base + ip
        uint32_t hw0 = (t4base | 0x80u) | ip.code();                   // T3 [ip, #0]
        return writeInstT2((rtc << 12) << 16 | hw0);
    }

    int32_t off = up ? int32_t(data & 0xfff) : -int32_t(data & 0xfff);
    if (mode == Offset) {
        if (off >= 0 && off <= 4095) {                                 // T3 positive imm12
            uint32_t hw0 = (t4base | 0x80u) | base.code();
            uint32_t hw1 = (rtc << 12) | uint32_t(off);
            return writeInstT2((hw1 << 16) | hw0);
        }
        if (off < 0 && off >= -255) {                                  // T4 negative imm8 (P=1,U=0,W=0)
            uint32_t hw0 = t4base | base.code();
            uint32_t hw1 = (rtc << 12) | 0x800u | (1u << 10) | uint32_t(-off);
            return writeInstT2((hw1 << 16) | hw0);
        }
        // Out of range -> movw/movt ip = off ; add ip, base, ip ; [ip, #0].
        MOZ_ASSERT(base != ip && rt != ip);
        as_movw(ip, Imm16(uint32_t(off) & 0xffff));
        if ((uint32_t(off) >> 16) != 0)
            as_movt(ip, Imm16((uint32_t(off) >> 16) & 0xffff));
        as_add(ip, base, O2Reg(ip));
        uint32_t hw0 = (t4base | 0x80u) | ip.code();
        return writeInstT2((rtc << 12) << 16 | hw0);
    }

    // Pre/Post index: T4 with P/W. P=(mode==PreIndex), W=1. imm8 in [-255,255].
    uint32_t aoff = uint32_t(off < 0 ? -off : off);
    if (aoff <= 255) {
        uint32_t P = (mode == PreIndex) ? 1u : 0u;
        uint32_t U = up ? 1u : 0u;
        uint32_t hw0 = t4base | base.code();
        uint32_t hw1 = (rtc << 12) | 0x800u | (P << 10) | (U << 9) | (1u << 8) | aoff;   // W=1
        return writeInstT2((hw1 << 16) | hw0);
    }
    // T32 pre/post-indexed load/store has ONLY an 8-bit immediate -- there is no wider encoding to
    // convert to. A synth would have to materialise the offset AND reproduce the base write-back,
    // which is a third addressing path with no caller to justify it. CONFIRMED unreachable: every
    // PreIndex/PostIndex emitter in the tree uses a machine-word-sized step (+-4/+-8 for push/pop
    // and the double-word trampoline walks), and the hardfp census over 5227 Baseline-JS tests
    // emitted this code zero times. Loud guard in BOTH worlds, not a silent gap on device.
    return varanUnsupported(0x24f);
}
#endif

BufferOffset
Assembler::as_dtr(LoadStore ls, int size, Index mode, Register rt, DTRAddr addr, Condition c)
{
#if defined(VARAN_THUMB2)
    // ldr/str (word) / ldrb/strb (byte). Family T4/reg hw0 base: L(bit4)/size select.
    uint32_t t4base = (size == 8)
        ? (ls == IsLoad ? 0xf810u : 0xf800u)    // ldrb / strb
        : (ls == IsLoad ? 0xf850u : 0xf840u);   // ldr  / str
    return varanEmitDtr(t4base, mode, rt, addr, c);
#else
    return writeInst(EncodeDtr(ls, size, mode, rt, addr, c));
#endif
}

/* static */ void
Assembler::as_dtr_patch(LoadStore ls, int size, Index mode, Register rt, DTRAddr addr, Condition c,
                        uint32_t* dest)
{
    // NOT guarded like as_alu_patch: this one is LIVE and already CONVERTED. EncodeDtr has a
    // VARAN arm that returns a T2 LDR-literal word, and PatchConstantPoolLoad calls this for
    // every PoolDTR / PoolBranch patch.
    //
    // (I guarded it briefly and it fired on two tests. My "no callers" conclusion came from a
    // grep that EXCLUDED Assembler-arm.cpp while looking for callers *outside* the assembler --
    // and both callers live in that very file. RULE: when deciding a symbol is dead, grep the
    // defining file too.)
    WriteInstStatic(EncodeDtr(ls, size, mode, rt, addr, c), dest);
}

class PoolHintData
{
  public:
    // VARAN: fixed UNSIGNED underlying type — stored in the 2-bit `loadType_`
    // bitfield of PoolHintData; clang-cl types a plain unscoped enum as signed int,
    // so PoolBranch(2)->-2 and PoolVDTR(3)->-1 in the signed 2-bit field, corrupting
    // pool-type dispatch. (Same clang-cl signed-enum class as RegType/Condition.)
    enum LoadType : uint32_t {
        // Set 0 to bogus, since that is the value most likely to be
        // accidentally left somewhere.
        PoolBOGUS  = 0,
        PoolDTR    = 1,
        PoolBranch = 2,
        PoolVDTR   = 3
    };

  private:
    uint32_t   index_    : 16;
    uint32_t   cond_     : 4;
    LoadType   loadType_ : 2;
    uint32_t   destReg_  : 5;
    uint32_t   destType_ : 1;
    uint32_t   ONES     : 4;

    static const uint32_t ExpectedOnes = 0xfu;

  public:
    void init(uint32_t index, Assembler::Condition cond, LoadType lt, Register destReg) {
        index_ = index;
        MOZ_ASSERT(index_ == index);
        cond_ = uint32_t(cond) >> 28;  // VARAN: unsigned shift. clang-cl (MSVC-compat) types the Condition enum as signed int, so a high condition (e.g. Always=0xe0000000) sign-extends on `cond >> 28`; the 4-bit field stored the right nibble regardless, but keep the extraction unsigned so the assert is truthful.
        MOZ_ASSERT(cond_ == uint32_t(cond) >> 28);
        loadType_ = lt;
        ONES = ExpectedOnes;
        destReg_ = destReg.code();
        destType_ = 0;
    }
    void init(uint32_t index, Assembler::Condition cond, LoadType lt, const VFPRegister& destReg) {
        MOZ_ASSERT(destReg.isFloat());
        index_ = index;
        MOZ_ASSERT(index_ == index);
        cond_ = uint32_t(cond) >> 28;  // VARAN: unsigned shift. clang-cl (MSVC-compat) types the Condition enum as signed int, so a high condition (e.g. Always=0xe0000000) sign-extends on `cond >> 28`; the 4-bit field stored the right nibble regardless, but keep the extraction unsigned so the assert is truthful.
        MOZ_ASSERT(cond_ == uint32_t(cond) >> 28);
        loadType_ = lt;
        ONES = ExpectedOnes;
        destReg_ = destReg.id();
        destType_ = destReg.isDouble();
    }
    Assembler::Condition getCond() const {
        return Assembler::Condition(cond_ << 28);
    }

    Register getReg() const {
        return Register::FromCode(destReg_);
    }
    VFPRegister getVFPReg() const {
        VFPRegister r = VFPRegister(destReg_, destType_ ? VFPRegister::Double : VFPRegister::Single);
        return r;
    }

    int32_t getIndex() const {
        return index_;
    }
    void setIndex(uint32_t index) {
        MOZ_ASSERT(ONES == ExpectedOnes && loadType_ != PoolBOGUS);
        index_ = index;
        MOZ_ASSERT(index_ == index);
    }

    LoadType getLoadType() const {
        // If this *was* a PoolBranch, but the branch has already been bound
        // then this isn't going to look like a real poolhintdata, but we still
        // want to lie about it so everyone knows it *used* to be a branch.
        if (ONES != ExpectedOnes)
            return PoolHintData::PoolBranch;
        return loadType_;
    }

    bool isValidPoolHint() const {
        // Most instructions cannot have a condition that is 0xf. Notable
        // exceptions are blx and the entire NEON instruction set. For the
        // purposes of pool loads, and possibly patched branches, the possible
        // instructions are ldr and b, neither of which can have a condition
        // code of 0xf.
        return ONES == ExpectedOnes;
    }
};

union PoolHintPun
{
    PoolHintData phd;
    uint32_t raw;
};

// Handles all of the other integral data transferring functions: ldrsb, ldrsh,
// ldrd, etc. The size is given in bits.
BufferOffset
Assembler::as_extdtr(LoadStore ls, int size, bool IsSigned, Index mode,
                     Register rt, EDtrAddr addr, Condition c)
{
#if defined(VARAN_THUMB2)
    if (c != Always) {
        // Conditional extended load/store -> B<!c>.W branch-over an unconditional body. NOT
        // VaranEmitCond1: this body is NOT one instruction -- the size 8/16 arm delegates to
        // varanEmitDtr (up to three instructions on the register-offset synth) and the size-64
        // register-offset arm emits add.w + ldrd/strd. So the skip is a placeholder patched once
        // the real body size is known, same as as_alu and varanEmitDtr.
        // 6 = the 5-instruction worst-case body + the branch itself.
        VaranForbidPoolsIfOutermost afp(this, 6);
        uint32_t inv = uint32_t(InvertCondition(c)) >> 28;
        BufferOffset br = writeInstT2(EncodeBccT2(4, inv));
        as_extdtr(ls, size, IsSigned, mode, rt, addr, Always);
        varanPatchCondSkip(br, inv);
        return br;
    }

    uint32_t data = addr.encode();
    Register base = Register::FromCode((data >> 16) & 0xf);
    bool isImm = (data >> 22) & 1;   // IsImmEDTR = 1<<22
    bool up    = (data >> 23) & 1;   // IsUp      = 1<<23

    if (size == 8 || size == 16) {
        // LDRSB / LDRH / STRH / LDRSH share varanEmitDtr's exact T3/T4/register structure
        // (the only difference from ldr/str is the class base and the 8-bit imm range). Rebuild a
        // DTRAddr from the decoded EDtrAddr (imm8, or lsl#0 register) and delegate.
        uint32_t t4base;
        if (size == 8) {
            MOZ_ASSERT(IsSigned && ls == IsLoad);
            t4base = 0xf910u;                              // ldrsb
        } else if (IsSigned) {
            MOZ_ASSERT(ls == IsLoad);
            t4base = 0xf930u;                              // ldrsh
        } else {
            t4base = (ls == IsLoad) ? 0xf830u : 0xf820u;   // ldrh / strh
        }
        if (isImm) {
            // EDtr immediate is a raw byte value (Imm8Data split imm4L|imm4H<<8), never scaled,
            // always <=255 -> fits varanEmitDtr's T3(imm12)/T4(imm8) split. self-range-check: no &0xff.
            uint32_t rawImm = (data & 0xf) | (((data >> 8) & 0xf) << 4);
            int32_t soff = up ? int32_t(rawImm) : -int32_t(rawImm);
            return varanEmitDtr(t4base, mode, rt, DTRAddr(base, DtrOffImm(soff)), c);
        }
        Register rm = Register::FromCode(data & 0xf);
        return varanEmitDtr(t4base, mode, rt,
                            DTRAddr(base, DtrRegImmShift(rm, LSL, 0, up ? IsUp : IsDown)), c);
    }

    // size == 64: LDRD / STRD (T1, base 0xE840, imm8 scaled x4, range +-1020). No register form in
    // T2 -> hot register-offset synth (add.w ip,Rn,Rm; ldrd/strd [ip,#0]). Rt2 = Rt+1.
    MOZ_ASSERT(size == 64);
    Register rt2 = Register::FromCode(rt.code() + 1);
    uint32_t L = (ls == IsLoad) ? 1u : 0u;
    const Register ip = Register::FromCode(12);   // synth scratch

    if (!isImm) {
        Register rm = Register::FromCode(data & 0xf);
        MOZ_ASSERT(base != ip && rt != ip && rt2 != ip);
        MOZ_ASSERT(mode == Offset);   // BaseIndex loadValue/storeValue only ever use Offset here
        if (up)
            as_add(ip, base, O2Reg(rm));
        else
            as_sub(ip, base, O2Reg(rm));
        uint32_t hw0 = 0xe840u | (1u << 8) /*P*/ | (1u << 7) /*U*/ | (L << 4) | ip.code();
        uint32_t hw1 = (rt.code() << 12) | (rt2.code() << 8);   // imm8 = 0
        return writeInstT2((hw1 << 16) | hw0);
    }

    uint32_t rawImm = (data & 0xf) | (((data >> 8) & 0xf) << 4);   // raw byte offset, <=255
    if ((rawImm & 3u) != 0)
        // T32 LDRD/STRD scales its imm8 by 4, so a non-word-multiple offset is architecturally
        // unencodable -- nothing to convert to. CONFIRMED unreachable: every 64-bit access the
        // backend emits is doubleword-aligned by construction, and the hardfp census over 5227
        // Baseline-JS tests emitted this code zero times.
        return varanUnsupported(0x25b);
    uint32_t imm8 = rawImm >> 2;                                   // x4-scaled T2 field. never &0xff
    // Offset:P=1,W=0 ; PreIndex:P=1,W=1 ; PostIndex:P=0,W=1 (A32's Index W-bit differs -> map by mode).
    uint32_t P = (mode == PostIndex) ? 0u : 1u;
    uint32_t W = (mode == Offset)    ? 0u : 1u;
    uint32_t U = up ? 1u : 0u;
    uint32_t hw0 = 0xe840u | (P << 8) | (U << 7) | (W << 5) | (L << 4) | base.code();
    uint32_t hw1 = (rt.code() << 12) | (rt2.code() << 8) | imm8;
    return writeInstT2((hw1 << 16) | hw0);
#else
    int extra_bits2 = 0;
    int extra_bits1 = 0;
    switch(size) {
      case 8:
        MOZ_ASSERT(IsSigned);
        MOZ_ASSERT(ls != IsStore);
        extra_bits1 = 0x1;
        extra_bits2 = 0x2;
        break;
      case 16:
        // 'case 32' doesn't need to be handled, it is handled by the default
        // ldr/str.
        extra_bits2 = 0x01;
        extra_bits1 = (ls == IsStore) ? 0 : 1;
        if (IsSigned) {
            MOZ_ASSERT(ls != IsStore);
            extra_bits2 |= 0x2;
        }
        break;
      case 64:
        extra_bits2 = (ls == IsStore) ? 0x3 : 0x2;
        extra_bits1 = 0;
        break;
      default:
        MOZ_CRASH("unexpected size in as_extdtr");
    }
    return writeInst(extra_bits2 << 5 | extra_bits1 << 20 | 0x90 |
                     addr.encode() | RT(rt) | mode | c);
#endif
}

BufferOffset
Assembler::as_dtm(LoadStore ls, Register rn, uint32_t mask,
                DTMMode mode, DTMWriteBack wb, Condition c)
{
#if defined(VARAN_THUMB2)
    // T2 integer LDM/STM (Batch 3). Only IA and DB have a Thumb-2 form; DA/IB have NONE -> UDF
    // (the backend's DA/IB sites -- loadValue -4/+4, Trampoline stmib -- synthesize via ldr/str, which
    // land in the load/store batch). hw0 = (IA?0xE880:0xE900) | (W<<5) | (L<<4) | Rn; hw1 = mask.
    if (c == Always && (mode == IA || mode == DB)) {
        uint32_t L = (ls == IsLoad) ? 1u : 0u;
        uint32_t W = (wb == WriteBack) ? 1u : 0u;
        // T2 tightens the list rules (A32 silently accepted these): a STORE list may not hold sp/pc,
        // and a LOAD list may not hold both lr and pc. Those rare frame-save/restore patterns cannot be
        // a single T2 LDM/STM -> defer them to per-register ldr/str (synthesized in the load/store
        // batch). (Single-register wide LDM/STM is UNPREDICTABLE in T2; the size()>1 call-site guards
        // remain the backstop and are preserved.)
        bool badStore = (L == 0) && (mask & ((1u << 13) | (1u << 15)));
        bool badLoad  = (L == 1) && (mask & (1u << 14)) && (mask & (1u << 15));
        if (!badStore && !badLoad) {
            uint32_t hw0 = (mode == IA ? 0xe880u : 0xe900u) | (W << 5) | (L << 4) | rn.code();
            return writeInstT2(((mask & 0xffffu) << 16) | hw0);
        }
    }
    // SYNTH (Batch B item 3): everything T2 LDM/STM cannot express -- DA/IB (no T2 form at all), a
    // conditional list, or a list T2 rejects (store containing sp/pc, load containing both lr and pc)
    // -> per-register ldr/str.
    //
    // ARM transfers a register list LOWEST-REGISTER-TO-LOWEST-ADDRESS in EVERY mode; only the address
    // range differs. With n = popcount(mask), the lowest address touched is
    //     IA: rn      IB: rn+4      DA: rn-4n+4      DB: rn-4n
    // and the writeback value is rn+4n (IA/IB) or rn-4n (DA/DB). Offsets here are tiny (|off| <= 64
    // for a full 16-register list), well inside as_dtr's T3 imm12 / T4 imm8 range.
    {
        uint32_t m = mask & 0xffffu;

        // A STORE list containing pc has no honest T2 lowering. LLVM's assembler will happily encode
        // `str.w pc, [rn,#off]` (f8c0 f004), but STR with Rt == 15 is UNPREDICTABLE in T32 -- it is
        // not defined to store anything in particular, and we target real Tegra 3 silicon.
        //
        // ---- W3: the STORED VALUE IS DEAD, so the slot only has to EXIST ----
        //
        // Sole caller: wasm's GenerateInterruptExit, via PushRegsInMask over
        // Registers::AllMask & ~(1<<sp) -- and AllMask = (1<<16)-1 does include pc. Reading that
        // stub is what settles this: three instructions after the push it does
        //     masm.storePtr(IntArgReg1, Address(r6, 14 * sizeof(uint32_t*)));   // "Store resumePC"
        // and slot 14 IS the pc slot ({r0..r12, lr, pc} places pc at the top of the block). So the
        // value STM would have stored is overwritten before anything can read it. The push needs
        // the pc slot to EXIST, at the right offset -- it never needs it to hold the pc.
        //
        // Therefore: keep pc in the popcount `n`, so the block size, every other register's offset
        // and the write-back delta are BYTE-IDENTICAL to the A32 layout, and simply skip emitting
        // the store for r15. The slot is left uninitialised, which is exactly what the caller
        // wants. No layout change, no caller change, no UNPREDICTABLE instruction, no guard.
        //
        // (`skipPcStore` is computed from the ORIGINAL mask precisely so that dropping the store
        // cannot be mistaken for dropping the register from the list -- doing the latter would
        // shift every subsequent slot by 4 and silently corrupt the saved register image.)
        const bool skipPcStore = (ls == IsStore) && (m & (1u << 15));

        uint32_t n = 0;
        for (uint32_t r = 0; r < 16; r++)
            if (m & (1u << r))
                n++;
        MOZ_ASSERT(n > 0);
        int32_t base = (mode == IA) ?  0
                     : (mode == IB) ?  4
                     : (mode == DA) ? -int32_t(4 * n) + 4
                     :                -int32_t(4 * n);          // DB

        // If the BASE register is itself in a LOAD list it must be transferred LAST: every offset
        // below is relative to the ORIGINAL rn, so loading it early would corrupt the remaining
        // addresses. Transferring it last also matches the architectural "final base value is the
        // loaded one". (A32 calls base-in-list-with-writeback UNPREDICTABLE; assert that away.)
        bool baseInLoadList = (ls == IsLoad) && (m & (1u << rn.code()));
        MOZ_ASSERT_IF(baseInLoadList, wb != WriteBack);

        // A32 `ldmia sp!, {..., pc}` updates the base AND branches in ONE instruction. This synth
        // cannot: it emits the loads, then the write-back `add`. Loading pc BRANCHES AWAY, so the
        // write-back would never execute and the base would be left stale.
        //
        // NOT live today -- wasm's GenerateInterruptExit restores {r0-r12, lr} and resumes with a
        // separate ret(), and no other caller puts pc in a write-back load list -- but it is
        // silent-wrong rather than loud-wrong if one ever does, which is the failure mode this port
        // keeps paying for. Assert instead of discovering it on device.
        MOZ_ASSERT(!((ls == IsLoad) && (m & (1u << 15)) && wb == WriteBack),
                   "VARAN: LDM{pc} with write-back cannot be synthesised -- pc load branches away "
                   "before the base update");

        BufferOffset first;
        bool haveFirst = false;
        int32_t baseSlot = 0;
        int32_t idx = 0;
        for (uint32_t r = 0; r < 16; r++) {
            if (!(m & (1u << r)))
                continue;
            int32_t off = base + 4 * idx;
            idx++;                              // ALWAYS advances -- the slot exists either way
            if (skipPcStore && r == 15)
                continue;                       // W3: slot reserved, value dead (see above)
            if (baseInLoadList && r == rn.code()) {
                baseSlot = off;                 // deferred to after the loop
                continue;
            }
            BufferOffset bo = as_dtr(ls, 32, Offset, Register::FromCode(r),
                                     DTRAddr(rn, DtrOffImm(off)), c);
            if (!haveFirst) { first = bo; haveFirst = true; }
        }
        if (baseInLoadList) {
            BufferOffset bo = as_dtr(ls, 32, Offset, rn, DTRAddr(rn, DtrOffImm(baseSlot)), c);
            if (!haveFirst) { first = bo; haveFirst = true; }
        }
        if (wb == WriteBack) {
            int32_t delta = (mode == IA || mode == IB) ? int32_t(4 * n) : -int32_t(4 * n);
            if (delta >= 0)
                as_add(rn, rn, Imm8(uint32_t(delta)), LeaveCC, c);
            else
                as_sub(rn, rn, Imm8(uint32_t(-delta)), LeaveCC, c);
        }
        MOZ_ASSERT(haveFirst);
        return first;
    }
#else
    return writeInst(0x08000000 | RN(rn) | ls | mode | mask | c | wb);
#endif
}

// Note, it's possible for markAsBranch and loadToPC to disagree,
// because some loads to the PC are not necessarily encoding
// instructions that should be marked as branches: only patchable
// near branch instructions should be marked.

BufferOffset
Assembler::allocEntry(size_t numInst, unsigned numPoolEntries,
                      uint8_t* inst, uint8_t* data, ARMBuffer::PoolEntry* pe,
                      bool markAsBranch, bool loadToPC)
{
    BufferOffset offs = m_buffer.allocEntry(numInst, numPoolEntries, inst, data, pe, markAsBranch);
    propagateOOM(offs.assigned());
#ifdef JS_DISASM_ARM
    spewData(offs, numInst, loadToPC);
#endif
    return offs;
}

// This is also used for instructions that might be resolved into branches,
// or might not.  If dest==pc then it is effectively a branch.

BufferOffset
Assembler::as_Imm32Pool(Register dest, uint32_t value, Condition c)
{
    PoolHintPun php;
    php.phd.init(0, c, PoolHintData::PoolDTR, dest);
    BufferOffset offs = allocEntry(1, 1, (uint8_t*)&php.raw, (uint8_t*)&value, nullptr, false,
                                   dest == pc);
    return offs;
}

/* static */ void
Assembler::WritePoolEntry(Instruction* addr, Condition c, uint32_t data)
{
    MOZ_ASSERT(addr->is<InstLDR>());
    *addr->as<InstLDR>()->dest() = data;
    MOZ_ASSERT(addr->extractCond() == c);
}

BufferOffset
Assembler::as_BranchPool(uint32_t value, RepatchLabel* label, ARMBuffer::PoolEntry* pe, Condition c,
                         Label* documentation)
{
    PoolHintPun php;
    php.phd.init(0, c, PoolHintData::PoolBranch, pc);
    BufferOffset ret = allocEntry(1, 1, (uint8_t*)&php.raw, (uint8_t*)&value, pe,
                                  /* markAsBranch = */ true, /* loadToPC = */ true);
    // If this label is already bound, then immediately replace the stub load
    // with a correct branch.
    if (label->bound()) {
        BufferOffset dest(label);
        BOffImm offset = dest.diffB<BOffImm>(ret);
        if (offset.isInvalid()) {
            m_buffer.fail_bail();
            return ret;
        }
        as_b(offset, c, ret);
    } else if (!oom()) {
        label->use(ret.getOffset());
    }
#ifdef JS_DISASM_ARM
    if (documentation)
        spewTarget(documentation);
#endif
    return ret;
}

BufferOffset
Assembler::as_FImm64Pool(VFPRegister dest, wasm::RawF64 value, Condition c)
{
    MOZ_ASSERT(dest.isDouble());
    PoolHintPun php;
    php.phd.init(0, c, PoolHintData::PoolVDTR, dest);
    uint64_t d = value.bits();
    return allocEntry(1, 2, (uint8_t*)&php.raw, (uint8_t*)&d);
}

BufferOffset
Assembler::as_FImm32Pool(VFPRegister dest, wasm::RawF32 value, Condition c)
{
    // Insert floats into the double pool as they have the same limitations on
    // immediate offset. This wastes 4 bytes padding per float. An alternative
    // would be to have a separate pool for floats.
    MOZ_ASSERT(dest.isSingle());
    PoolHintPun php;
    php.phd.init(0, c, PoolHintData::PoolVDTR, dest);
    uint32_t f = value.bits();
    return allocEntry(1, 1, (uint8_t*)&php.raw, (uint8_t*)&f);
}

#if defined(VARAN_THUMB2)
// VARAN Batch 4 pool self-test: PoolHintData is a .cpp-private class, so exercise its index round-trip
// here (a la varanT2RetargetSplice -- non-vacuously, with values that would corrupt a signed/narrow
// bitfield) and confirm the emit->patch index plumbing is ISA-independent. Returns failed-check count.
/* static */ int
Assembler::varanPoolHintSelfTest()
{
    int fails = 0;
    auto chk = [&](bool ok) { if (!ok) fails++; };

    // Round-trip a PoolHintData through init + setIndex + the getters. index_ is a 16-bit UNSIGNED
    // field; a value with bit15 set (0x8xxx / 0xffff) is the sharp case -- a signed or <16-bit field
    // would truncate/sign-mangle it. The other fields (cond, loadType, destReg) must survive setIndex,
    // and setIndex itself asserts the ONES sentinel + non-BOGUS type, so this also checks those.
    PoolHintPun php;
    php.phd.init(0, Assembler::LessThan, PoolHintData::PoolDTR, r5);
    chk(php.phd.getIndex() == 0);
    chk(php.phd.getCond() == Assembler::LessThan);
    chk(php.phd.getLoadType() == PoolHintData::PoolDTR);
    chk(php.phd.getReg() == r5);
    for (uint32_t idx : { 1u, 0x1234u, 0x8001u, 0xABCDu, 0xFFFFu, 0u }) {
        php.phd.setIndex(idx);
        chk(uint32_t(php.phd.getIndex()) == idx);        // exact 16-bit round-trip
        chk(php.phd.getCond() == Assembler::LessThan);   // cond untouched by setIndex
        chk(php.phd.getLoadType() == PoolHintData::PoolDTR);
        chk(php.phd.getReg() == r5);
    }

    // The VFP hint carries a float dest + double flag; round-trip a big index through it too.
    PoolHintPun pv;
    pv.phd.init(7, Assembler::Always, PoolHintData::PoolVDTR, d3);
    chk(pv.phd.getIndex() == 7);
    chk(pv.phd.getLoadType() == PoolHintData::PoolVDTR);
    chk(pv.phd.getVFPReg().isDouble());
    pv.phd.setIndex(0xFEDC);
    chk(uint32_t(pv.phd.getIndex()) == 0xFEDCu);
    chk(pv.phd.getLoadType() == PoolHintData::PoolVDTR);

    // Alias-guard headroom: at the max pool offset the pc-base LDR-literal imm12 must stay well under
    // 0xfff (else Rt==pc would alias the 0xffff0000 PoolHeader). AsmPoolMaxOffset keeps it bounded.
    chk(uint32_t(GetPoolMaxOffset()) <= 0xfffu);
    chk(uint32_t(GetPoolMaxOffset()) < 0xfffu);   // strictly under the alias value

    return fails;
}
#endif

// Pool callbacks stuff:
void
Assembler::InsertIndexIntoTag(uint8_t* load_, uint32_t index)
{
    uint32_t* load = (uint32_t*)load_;
    PoolHintPun php;
    php.raw = *load;
    php.phd.setIndex(index);
    *load = php.raw;
}

// patchConstantPoolLoad takes the address of the instruction that wants to be
// patched, and the address of the start of the constant pool, and figures
// things out from there.
void
Assembler::PatchConstantPoolLoad(void* loadAddr, void* constPoolAddr)
{
    PoolHintData data = *(PoolHintData*)loadAddr;
    uint32_t* instAddr = (uint32_t*) loadAddr;
    int offset = (char*)constPoolAddr - (char*)loadAddr;
    switch(data.getLoadType()) {
      case PoolHintData::PoolBOGUS:
        MOZ_CRASH("bogus load type!");
      case PoolHintData::PoolDTR:
#if defined(VARAN_THUMB2)
        // T2 pc-bias is Align(instr+4,4)=instr+4 (vs A32's instr+8) -> -4 not -8. Single 4B slot has
        // no cond field, so a conditional pool load would silently drop predication; assert AL.
        MOZ_ASSERT(data.getCond() == Always);
        Assembler::as_dtr_patch(IsLoad, 32, Offset, data.getReg(),
                                DTRAddr(pc, DtrOffImm(offset+4*data.getIndex() - 4)),
                                data.getCond(), instAddr);
#else
        Assembler::as_dtr_patch(IsLoad, 32, Offset, data.getReg(),
                                DTRAddr(pc, DtrOffImm(offset+4*data.getIndex() - 8)),
                                data.getCond(), instAddr);
#endif
        break;
      case PoolHintData::PoolBranch:
        // Either this used to be a poolBranch, and the label was already bound,
        // so it was replaced with a real branch, or this may happen in the
        // future. If this is going to happen in the future, then the actual
        // bits that are written here don't matter (except the condition code,
        // since that is always preserved across patchings) but if it does not
        // get bound later, then we want to make sure this is a load from the
        // pool entry (and the pool entry should be nullptr so it will crash).
        if (data.isValidPoolHint()) {
#if defined(VARAN_THUMB2)
            MOZ_ASSERT(data.getCond() == Always);   // ldr pc,[pc,#imm] T2 -- single slot, no cond
            Assembler::as_dtr_patch(IsLoad, 32, Offset, pc,
                                    DTRAddr(pc, DtrOffImm(offset+4*data.getIndex() - 4)),
                                    data.getCond(), instAddr);
#else
            Assembler::as_dtr_patch(IsLoad, 32, Offset, pc,
                                    DTRAddr(pc, DtrOffImm(offset+4*data.getIndex() - 8)),
                                    data.getCond(), instAddr);
#endif
        }
        break;
      case PoolHintData::PoolVDTR: {
        VFPRegister dest = data.getVFPReg();
#if defined(VARAN_THUMB2)
        int32_t imm = offset + (data.getIndex() * 4) - 4;   // T2 pc-bias instr+4
        MOZ_ASSERT(data.getCond() == Always);               // single-slot T2 VLDR-literal, no cond
#else
        int32_t imm = offset + (data.getIndex() * 4) - 8;
#endif
        MOZ_ASSERT(-1024 < imm && imm < 1024);
        Assembler::as_vdtr_patch(IsLoad, dest, VFPAddr(pc, VFPOffImm(imm)), data.getCond(),
                                 instAddr);
        break;
      }
    }
}

// Atomic instruction stuff:

#if defined(VARAN_THUMB2)
// ---- A1: exclusive access (T32) ----
//
// All oracle-verified against LLVM (thumbv7-unknown-windows-msvc), Rn=r2, Rt=r1, Rd=r0:
//     ldrex  r1,[r2]      = e852 1f00      ldrex r1,[r2,#16] = e852 1f04   (imm8 is x4-scaled)
//     strex  r0,r1,[r2]   = e842 1000
//     ldrexb r1,[r2]      = e8d2 1f4f      ldrexh r1,[r2]    = e8d2 1f5f
//     strexb r0,r1,[r2]   = e8c2 1f40      strexh r0,r1,[r2] = e8c2 1f50
//
// The A32 signatures carry no offset, so imm8 is always 0. T32 has no condition field on
// these, so a conditional form goes through the standard branch-over (VaranEmitCond1);
// every caller today is unconditional.
static uint32_t
VaranLdrexT2(Register rt, Register rn)          // hw0 = 0xE850|Rn ; hw1 = Rt<<12 | 0x0F00
{
    return (((uint32_t(rt.code()) << 12) | 0x0F00u) << 16) | (0xE850u | uint32_t(rn.code()));
}
static uint32_t
VaranStrexT2(Register rd, Register rt, Register rn)   // hw0 = 0xE840|Rn ; hw1 = Rt<<12|Rd<<8
{
    return (((uint32_t(rt.code()) << 12) | (uint32_t(rd.code()) << 8)) << 16)
           | (0xE840u | uint32_t(rn.code()));
}
static uint32_t
VaranLdrexBHT2(Register rt, Register rn, uint32_t kind)   // kind: 0x4F byte, 0x5F halfword
{
    return (((uint32_t(rt.code()) << 12) | 0x0F00u | kind) << 16)
           | (0xE8D0u | uint32_t(rn.code()));
}
static uint32_t
VaranStrexBHT2(Register rd, Register rt, Register rn, uint32_t kind)  // kind: 0x40 / 0x50
{
    return (((uint32_t(rt.code()) << 12) | 0x0F00u | kind | uint32_t(rd.code())) << 16)
           | (0xE8C0u | uint32_t(rn.code()));
}
#endif

BufferOffset
Assembler::as_ldrex(Register rt, Register rn, Condition c)
{
#if defined(VARAN_THUMB2)
    return VaranEmitCond1(this, VaranLdrexT2(rt, rn), c);
#else
    return writeInst(0x01900f9f | (int)c | RT(rt) | RN(rn));
#endif
}

BufferOffset
Assembler::as_ldrexh(Register rt, Register rn, Condition c)
{
#if defined(VARAN_THUMB2)
    return VaranEmitCond1(this, VaranLdrexBHT2(rt, rn, 0x5Fu), c);
#else
    return writeInst(0x01f00f9f | (int)c | RT(rt) | RN(rn));
#endif
}

BufferOffset
Assembler::as_ldrexb(Register rt, Register rn, Condition c)
{
#if defined(VARAN_THUMB2)
    return VaranEmitCond1(this, VaranLdrexBHT2(rt, rn, 0x4Fu), c);
#else
    return writeInst(0x01d00f9f | (int)c | RT(rt) | RN(rn));
#endif
}

BufferOffset
Assembler::as_strex(Register rd, Register rt, Register rn, Condition c)
{
    MOZ_ASSERT(rd != rn && rd != rt); // True restriction on Cortex-A7 (RPi2)
#if defined(VARAN_THUMB2)
    return VaranEmitCond1(this, VaranStrexT2(rd, rt, rn), c);
#else
    return writeInst(0x01800f90 | (int)c | RD(rd) | RN(rn) | rt.code());
#endif
}

BufferOffset
Assembler::as_strexh(Register rd, Register rt, Register rn, Condition c)
{
    MOZ_ASSERT(rd != rn && rd != rt); // True restriction on Cortex-A7 (RPi2)
#if defined(VARAN_THUMB2)
    return VaranEmitCond1(this, VaranStrexBHT2(rd, rt, rn, 0x50u), c);
#else
    return writeInst(0x01e00f90 | (int)c | RD(rd) | RN(rn) | rt.code());
#endif
}

BufferOffset
Assembler::as_strexb(Register rd, Register rt, Register rn, Condition c)
{
    MOZ_ASSERT(rd != rn && rd != rt); // True restriction on Cortex-A7 (RPi2)
#if defined(VARAN_THUMB2)
    return VaranEmitCond1(this, VaranStrexBHT2(rd, rt, rn, 0x40u), c);
#else
    return writeInst(0x01c00f90 | (int)c | RD(rd) | RN(rn) | rt.code());
#endif
}

// Memory barrier stuff:

#if defined(VARAN_THUMB2)
// ---- A2: memory barriers (T32) ----
//
// hw0 = 0xF3BF for all three; hw1 = 0x8F40|option (DSB), 0x8F50|option (DMB), 0x8F60|option
// (ISB). The option nibble is identical to A32's, so the existing BarrierOption values
// (BarrierSY = 15, BarrierST = 14) carry over unchanged.
//
// Oracle-verified against LLVM (thumbv7-unknown-windows-msvc):
//     dmb sy  = f3bf 8f5f     dmb ish = f3bf 8f5b
//     dsb sy  = f3bf 8f4f     dsb ish = f3bf 8f4b
//     isb sy  = f3bf 8f6f
//
// ★ These are DEVICE-CRITICAL beyond the UDF census: the device cacheFlush needs real DMB/ISB
// instructions, so "present" is not enough -- they have to be right.
static uint32_t
VaranBarrierT2(uint32_t hw1Base, uint32_t option)
{
    return (((hw1Base | (option & 0xf)) & 0xffffu) << 16) | 0xF3BFu;
}
#endif

BufferOffset
Assembler::as_dmb(BarrierOption option)
{
#if defined(VARAN_THUMB2)
    return writeInstT2(VaranBarrierT2(0x8F50u, uint32_t(option)));
#else
    return writeInst(0xf57ff050U | (int)option);
#endif
}
BufferOffset
Assembler::as_dsb(BarrierOption option)
{
#if defined(VARAN_THUMB2)
    return writeInstT2(VaranBarrierT2(0x8F40u, uint32_t(option)));
#else
    return writeInst(0xf57ff040U | (int)option);
#endif
}
BufferOffset
Assembler::as_isb()
{
#if defined(VARAN_THUMB2)
    return writeInstT2(VaranBarrierT2(0x8F60u, 0xfu));   // option == SY
#else
    return writeInst(0xf57ff06fU); // option == SY
#endif
}
BufferOffset
Assembler::as_dsb_trap()
{
    // DSB is "mcr 15, 0, r0, c7, c10, 4".
    // See eg https://bugs.kde.org/show_bug.cgi?id=228060.
    // ARMv7 manual, "VMSA CP15 c7 register summary".
    // Flagged as "legacy" starting with ARMv8, may be disabled on chip, see
    // ARMv8 manual E2.7.3 and G3.18.16.
    return writeInst(0xee070f9a);
}
BufferOffset
Assembler::as_dmb_trap()
{
    // DMB is "mcr 15, 0, r0, c7, c10, 5".
    // ARMv7 manual, "VMSA CP15 c7 register summary".
    // Flagged as "legacy" starting with ARMv8, may be disabled on chip, see
    // ARMv8 manual E2.7.3 and G3.18.16.
    return writeInst(0xee070fba);
}
BufferOffset
Assembler::as_isb_trap()
{
    // ISB is "mcr 15, 0, r0, c7, c5, 4".
    // ARMv7 manual, "VMSA CP15 c7 register summary".
    // Flagged as "legacy" starting with ARMv8, may be disabled on chip, see
    // ARMv8 manual E2.7.3 and G3.18.16.
    return writeInst(0xee070f94);
}

// Control flow stuff:

// bx can *only* branch to a register, never to an immediate.
BufferOffset
Assembler::as_bx(Register r, Condition c)
{
#if defined(VARAN_THUMB2)
    // BX Rm is 16-bit-only (T1: 0x4700|(Rm<<3)); the buffer is a 4-byte unit, so pack it in the HIGH
    // halfword with a NOP16 (0xbf00) low -- keeping the same NOP16-low/branch-high layout as BLX (where
    // the packing is load-bearing for the return address). BX is unconditional in T2 (cond dropped).
    return writeBranchInstT2(((0x4700u | (r.code() << 3)) << 16) | 0xbf00u);
#else
    BufferOffset ret = writeInst(((int) c) | OpBx | r.code());
    return ret;
#endif
}

#if defined(VARAN_THUMB2)
// Absolute (non-patchable) branch for ma_b(void*): materialize a Thumb code pointer into ip via
// movw/movt then bx ip. Used for bailout-table / OOL entries -- a fixed absolute target, never
// repatched. Deliberately NOT a pool `ldr pc,[pc,#imm]` (that would depend on the sim's A32
// kPCReadOffset=8 pc-relative base, off by +4 for Thumb-2). The body is a FIXED 12 bytes
// (movw+movt+bx, all wide/4B) so a conditional caller can branch over it with a constant skip.
// ip (ScratchRegister) is clobbered -- these branch sites do not carry a live ip across (same
// assumption as varanEmitDtr's out-of-range synth).
void
Assembler::varanAbsBranch(uint32_t target, Condition c)
{
    uint32_t t = target | 1u;   // Thumb interworking bit: bx reads bit0 of ip (F2 -- even => ARM fault).
    // ★ A4 POOL GUARD -- BY FAR the worst of the four: 419 splits on the corpus pre-guard. N=4 =
    // B<!c>.W (1) + movw (1) + movt (1) + bx (1; the 16-bit BX is packed with a NOP into one 4-byte
    // slot). The hard-coded skip of 12 encodes the exact body size, so a pool inserted ANYWHERE in
    // the window makes the !c path branch into pool DATA. Guard covers the whole function so the
    // Always path (3 words, no branch) is protected too; over-counting by one there is benign --
    // maxInst only decides whether to flush BEFORE entering.
    VaranForbidPoolsIfOutermost varanAfp(this, 4);
    if (c != Always) {
        // When !c, skip the 12-byte body (B<!c>.W, off = body size). InvertCondition(real)<14, in range.
        writeInstT2(EncodeBccT2(12, uint32_t(InvertCondition(c)) >> 28));
    }
    as_movw(ip, Imm16(t & 0xffffu));
    as_movt(ip, Imm16((t >> 16) & 0xffffu));   // always emitted (fixed body size), even if hi==0
    as_bx(ip);
}
#endif

void
Assembler::WritePoolGuard(BufferOffset branch, Instruction* dest, BufferOffset afterPool)
{
    BOffImm off = afterPool.diffB<BOffImm>(branch);
    if (off.isInvalid())
        MOZ_CRASH("BOffImm invalid");
    *dest = InstBImm(off, Always);
}

// Branch can branch to an immediate *or* to a register.
// Branches to immediates are pc relative, branches to registers are absolute.
BufferOffset
Assembler::as_b(BOffImm off, Condition c, Label* documentation)
{
#if defined(VARAN_THUMB2)
    // 1-slot unconditional B.W emit (unbound-chain head / bound target). Conditional branches are
    // 2-slot and never routed here -- they go through varanAsBCond() from as_b(Label*, c).
    MOZ_ASSERT(c == Always);
    return writeBranchInstT2(VaranEncodeBranchInst(/*isBL=*/false, off.decode(), uint32_t(c) >> 28),
                             documentation);
#else
    BufferOffset ret = writeBranchInst(((int)c) | OpB | off.encode(), documentation);
    return ret;
#endif
}

BufferOffset
Assembler::as_b(Label* l, Condition c)
{
#if defined(VARAN_THUMB2)
    // Thumb-2 conditional B<c>.W is only +-1MB; the 2-slot invert+B.W fallback (NEW machinery --
    // A32 conditional B was +-32MB) lives in varanAsBCond, which reserves both slots at emit.
    if (c != Always)
        return varanAsBCond(l, c);
#endif
    if (l->bound()) {
        // Note only one instruction is emitted here, the NOP is overwritten.
        BufferOffset ret = allocBranchInst();
        if (oom())
            return BufferOffset();

        BOffImm off = BufferOffset(l).diffB<BOffImm>(ret);
        if (off.isInvalid()) {
            m_buffer.fail_bail();
            return BufferOffset();
        }
        as_b(off, c, ret);
#ifdef JS_DISASM_ARM
        spewBranch(m_buffer.getInstOrNull(ret), l);
#endif
        return ret;
    }

    if (oom())
        return BufferOffset();

    int32_t old;
    BufferOffset ret;
    if (l->used()) {
        old = l->offset();
        // This will currently throw an assertion if we couldn't actually
        // encode the offset of the branch.
        if (!BOffImm::IsInRange(old)) {
            m_buffer.fail_bail();
            return ret;
        }
        ret = as_b(BOffImm(old), c, l);
    } else {
        old = LabelBase::INVALID_OFFSET;
        BOffImm inv;
        ret = as_b(inv, c, l);
    }

    if (oom())
        return BufferOffset();

    DebugOnly<int32_t> check = l->use(ret.getOffset());
    MOZ_ASSERT(check == old);
    return ret;
}

BufferOffset
Assembler::as_b(wasm::TrapDesc target, Condition c)
{
    Label l;
    BufferOffset ret = as_b(&l, c);
    bindLater(&l, target);
    return ret;
}

BufferOffset
Assembler::as_b(BOffImm off, Condition c, BufferOffset inst)
{
    // JS_DISASM_ARM NOTE: Can't disassemble here, because numerous callers use this to
    // patchup old code.  Must disassemble in caller where it makes sense.  Not many callers.
    *editSrc(inst) = InstBImm(off, c);
    return inst;
}

// blx can go to either an immediate or a register.
// When blx'ing to a register, we change processor state depending on the low
// bit of the register when blx'ing to an immediate, we *always* change
// processor state.

BufferOffset
Assembler::as_blx(Register r, Condition c)
{
#if defined(VARAN_THUMB2)
    // BLX Rm is 16-bit-only (T1: 0x4780|(Rm<<3)). Pack it in the HIGH halfword with a NOP16 low: stored
    // (blx16<<16)|0xbf00 puts NOP16 at slot+0 and blx16 at slot+2, so hardware LR = (slot+2)+2 = slot+4
    // = slot-end = currentOffset() = the safepoint/near-call return address recorded by call(Register).
    // Packing it LOW would make LR=slot+2 -> a 2-byte safepoint skew (silent, GC-lethal). AL only.
    return writeBranchInstT2(((0x4780u | (r.code() << 3)) << 16) | 0xbf00u);
#else
    return writeInst(((int) c) | OpBlx | r.code());
#endif
}

// bl can only branch to an pc-relative immediate offset
// It cannot change the processor state.
BufferOffset
Assembler::as_bl(BOffImm off, Condition c, Label* documentation)
{
#if defined(VARAN_THUMB2)
    // BL (T1) is unconditional. 1-slot; the offset field carries the chain link while unbound.
    MOZ_ASSERT(c == Always);
    return writeBranchInstT2(VaranEncodeBranchInst(/*isBL=*/true, off.decode(), uint32_t(c) >> 28),
                             documentation);
#else
    return writeBranchInst(((int)c) | OpBl | off.encode(), documentation);
#endif
}

BufferOffset
Assembler::as_bl(Label* l, Condition c)
{
    if (l->bound()) {
        // Note only one instruction is emitted here, the NOP is overwritten.
        BufferOffset ret = allocBranchInst();
        if (oom())
            return BufferOffset();

        BOffImm offset = BufferOffset(l).diffB<BOffImm>(ret);
        if (offset.isInvalid()) {
            m_buffer.fail_bail();
            return BufferOffset();
        }

        as_bl(offset, c, ret);
#ifdef JS_DISASM_ARM
        spewBranch(m_buffer.getInstOrNull(ret), l);
#endif
        return ret;
    }

    if (oom())
        return BufferOffset();

    int32_t old;
    BufferOffset ret;
    // See if the list was empty :(
    if (l->used()) {
        // This will currently throw an assertion if we couldn't actually encode
        // the offset of the branch.
        old = l->offset();
        if (!BOffImm::IsInRange(old)) {
            m_buffer.fail_bail();
            return ret;
        }
        ret = as_bl(BOffImm(old), c, l);
    } else {
        old = LabelBase::INVALID_OFFSET;
        BOffImm inv;
        ret = as_bl(inv, c, l);
    }

    if (oom())
        return BufferOffset();

    DebugOnly<int32_t> check = l->use(ret.getOffset());
    MOZ_ASSERT(check == old);
    return ret;
}

BufferOffset
Assembler::as_bl(BOffImm off, Condition c, BufferOffset inst)
{
    *editSrc(inst) = InstBLImm(off, c);
    return inst;
}

#if defined(VARAN_THUMB2)
// P1.2b Group 2 -- the +-1MB conditional-branch fallback (NEW machinery: A32's conditional B was
// +-32MB and simply fail_bail'd past its range, so there was nothing to adapt). Patch the reserved
// 2-instruction site at [slot0, slot0+4] to land at buffer offset `target`:
//   in +-1MB : slot0 = B<c>.W(target)          slot1 = NOP.W
//   overflow : slot0 = B<!c>.W(+4, skip slot1) slot1 = B.W(target)          (invert + branch-over)
// Both slots are always written, so the layout is fixed and no chain offset already stored ever
// shifts (grow-at-bind would corrupt the whole chain -- see BRANCH-ENCODING.md).
// Shared 2-slot jump writer. Computes the two raw Thumb-2 words for a patchable branch at slot pair
// [s0, s0+4] targeting `target`. s0/target are in a CONSISTENT space -- buffer offsets (assembly-time,
// varanPatchCondBranch2) or code addresses (post-link, PatchJump) -- since a branch only encodes the
// relative distance. condField 14/15 => Always (1-slot B.W + NOP.W); else a conditional pair:
//   in-range (<=+-1MB): slot0 = B<c>.W(target), slot1 = NOP.W
//   overflow (>+-1MB):  slot0 = B<!c>.W(+4) [skip slot1], slot1 = B.W(target)  (reaches +-16MB)
static void
VaranComputeJump2(intptr_t s0, intptr_t target, uint32_t condField, uint32_t* w0, uint32_t* w1)
{
    if (condField >= 14) {   // Always: one wide B.W in slot0, NOP.W in slot1
        int32_t off = int32_t(target - (s0 + 4));
        MOZ_RELEASE_ASSERT(VaranBwInRange(off), "VARAN: 2-slot Always jump out of +-16MB reach");
        *w0 = EncodeBranchImmT2(off, /*isBL=*/false);
        *w1 = VARAN_NOPW_WORD;
        return;
    }
    int32_t offCc = int32_t(target - (s0 + 4));   // B<c>.W at slot0 reaches `target` directly
    if (VaranBccInRange(offCc)) {
        *w0 = EncodeBccT2(offCc, condField);
        *w1 = VARAN_NOPW_WORD;
    } else {
        uint32_t inv = uint32_t(Assembler::InvertCondition(Assembler::Condition(condField << 28))) >> 28;
        *w0 = EncodeBccT2(4, inv);                // when !c, skip past slot1 (to s0+8)
        int32_t offBw = int32_t(target - (s0 + 8));
        MOZ_RELEASE_ASSERT(VaranBwInRange(offBw), "VARAN: 2-slot cond-overflow jump out of +-16MB reach");
        *w1 = EncodeBranchImmT2(offBw, /*isBL=*/false);
    }
}

void
Assembler::varanPatchCondBranch2(BufferOffset slot0, int32_t target, Condition c)
{
    int32_t b0 = slot0.getOffset();
    uint32_t cond = uint32_t(c) >> 28;
    Instruction* i0 = editSrc(slot0);
    Instruction* i1 = editSrc(BufferOffset(b0 + 4));
    uint32_t w0, w1;
    VaranComputeJump2(b0, target, cond, &w0, &w1);   // offsets: the branch distance is position-independent
    i0->varanSetRaw(w0);
    i1->varanSetRaw(w1);
}

// Batch-4 self-test for the shared 2-slot jump writer (the PatchJump/jumpWithPatch core). Exercises
// VaranComputeJump2 across Always + conditional, in-range + the >+-1MB overflow fallback, and decodes
// the emitted pair back to confirm it LANDS at the requested distance (non-vacuous -- a truncated offset
// or a mis-picked slot lands elsewhere). Returns failed-check count. slot0 is modeled at address 0.
/* static */ int
Assembler::varanJumpPatch2SelfTest()
{
    int fails = 0;
    auto chk = [&](bool ok) { if (!ok) fails++; };
    struct Case { uint32_t cf; int32_t d; };
    // cf 14 = Always; else a condition nibble. d = target distance from slot0 (must be even).
    const Case cases[] = {
        {14,  100}, {14, -100}, {14,  4000000}, {14, -4000000},   // Always: 1-slot B.W (+-16MB)
        {0,   100}, {0,  -100},                                   // EQ in-range: B<c>.W + NOP.W
        {11,  200000}, {11, -200000},                             // LT in-range near the +-1MB edge
        {1,   4000000}, {6, -4000000},                            // NE/VS overflow: invert + B.W
    };
    for (const Case& c : cases) {
        uint32_t w0 = 0, w1 = 0;
        VaranComputeJump2(0, c.d, c.cf, &w0, &w1);
        VaranBranchDec d0 = DecodeBranchT2(w0);
        int32_t landing;
        if (c.cf >= 14) {                       // Always: B.W in slot0, NOP.W in slot1
            chk(d0.valid && d0.off != 0);
            chk(w1 == VARAN_NOPW_WORD);
            landing = 4 + d0.off;               // slot0 pc = 0 + 4
        } else if (VaranBccInRange(c.d - 4)) {  // in-range conditional: B<c>.W + NOP.W
            chk(d0.valid && (d0.cond == c.cf));
            chk(w1 == VARAN_NOPW_WORD);
            landing = 4 + d0.off;
        } else {                                // overflow: B<!c>.W(+4) in slot0, B.W(target) in slot1
            chk(d0.valid && d0.cond == (uint32_t(InvertCondition(Condition(c.cf << 28))) >> 28));
            chk(d0.off == 4);                   // skip past slot1
            VaranBranchDec d1 = DecodeBranchT2(w1);
            chk(d1.valid && !d1.isBL);
            landing = 8 + d1.off;               // slot1 pc = 4 + 4
        }
        chk(landing == c.d);                    // lands exactly at the requested distance
    }
    return fails;
}

// 2-slot conditional branch to label `l`. slot0 = B<c>.W condition carrier; slot1 = a B.W that
// (while forward-unbound) holds the chain link -- a B.W field is +-16MB, so a link/sentinel that
// would not fit slot0's +-1MB always fits. nextLink()/bind() read+patch the pair; the label chain
// threads slot0 offsets. Backward (bound) branches take the same 2-slot form (the fallback applies
// to them too -- a bound target can also be > +-1MB away).
BufferOffset
Assembler::varanAsBCond(Label* l, Condition c)
{
    if (l->bound()) {
        // F1: slot0 + slot1 MUST be adjacent -- varanPatchCondBranch2() patches slot0 and slot0+4.
        // Without a pool guard the buffer may dump a constant pool between the two writes, and the
        // pool's GUARD B.W would then occupy slot0+4, so we would patch the pool's own guard branch.
        BufferOffset slot0;
        {
            VaranForbidPoolsIfOutermost afp(this, 2);
            slot0 = allocBranchInst();                   // slot0 placeholder (NOP.W, markAsBranch)
            m_buffer.putInt(0x8000F3AFu);                // slot1 placeholder (NOP.W)
        }
        if (oom())
            return BufferOffset();
        int32_t target = l->offset();
        if (!BOffImm::IsInRange(target - slot0.getOffset())) {
            m_buffer.fail_bail();
            return BufferOffset();
        }
        varanPatchCondBranch2(slot0, target, c);
#ifdef JS_DISASM_ARM
        spewBranch(m_buffer.getInstOrNull(slot0), l);
#endif
        return slot0;
    }

    if (oom())
        return BufferOffset();

    // ★ F1 ROOT CAUSE FIX. slot0 + slot1 MUST be adjacent: nextLink()/bind() read the chain link at
    // slot0+4. Without a pool guard the buffer can dump a constant pool BETWEEN the two writes below;
    // the pool's GUARD B.W then occupies slot0+4, and nextLink() decodes that guard's jump-over
    // displacement as the chain link. Observed under --ion-eager: 308 asserts, every bogus link
    // decoding to the same small value (16) from several DISTINCT chain heads -- the signature of
    // reading pool guards rather than links. Ion-specific only because Ion spills far more
    // constants, so the flush window between the two writes is hit constantly.
    // Nesting-safe variant on purpose: varanAsBCond is reached from regions that already hold a
    // no-pool region (e.g. visitTableSwitch) and enterNoPool() is NOT re-entrant -- that was E1.
    VaranForbidPoolsIfOutermost afp(this, 2);

    // slot0 = B<c>.W carrier (byteVal 0 -> a harmless self-relative placeholder; overwritten at bind).
    BufferOffset slot0 =
        writeBranchInstT2(VaranEncodeBranchInst(/*isBL=*/false, /*byteVal=*/0, uint32_t(c) >> 28));

    int32_t old;
    if (l->used()) {
        old = l->offset();
        if (!BOffImm::IsInRange(old)) {
            m_buffer.fail_bail();
            return BufferOffset();
        }
        writeInstT2(VaranEncodeBranchInst(/*isBL=*/false, old, /*Always=*/14u));            // slot1 = B.W(link)
    } else {
        old = LabelBase::INVALID_OFFSET;
        writeInstT2(VaranEncodeBranchInst(/*isBL=*/false, VARAN_BOFF_INVALID, /*Always=*/14u)); // slot1 = B.W(sentinel)
    }

    if (oom())
        return BufferOffset();

    DebugOnly<int32_t> check = l->use(slot0.getOffset());
    MOZ_ASSERT(check == old);
    return slot0;
}

void
Assembler::varanWriteChainLink(BufferOffset slot0, int32_t linkVal)
{
    uint32_t w = editSrc(slot0)->encode();
    int kind = VaranBranchKind(w);
    MOZ_ASSERT(kind != 0);
    uint32_t cond = 14u;
    VaranDecodeBranchCond(w, &cond);
    if (kind == 1 && cond < 14u) {
        // 2-slot conditional: the chain link lives in the companion B.W at slot0+4 (where nextLink
        // reads it); slot0's B<c>.W condition carrier is left untouched.
        editSrc(BufferOffset(slot0.getOffset() + 4))->varanSetRaw(
            VaranEncodeBranchInst(/*isBL=*/false, linkVal, /*Always=*/14u));
    } else {
        // 1-slot B.W (kind 1, Always) or BL (kind 2): the branch itself carries the link.
        editSrc(slot0)->varanSetRaw(
            VaranEncodeBranchInst(/*isBL=*/kind == 2, linkVal, /*Always=*/14u));
    }
}
#endif

BufferOffset
Assembler::as_mrs(Register r, Condition c)
{
#if defined(VARAN_THUMB2)
    // MRS <Rd>, APSR (T1): hw0 = 0xF3EF ; hw1 = 0x8000 | (Rd << 8).
    // Oracle: `mrs r4, apsr` = ef f3 00 84. The only caller today is wasm::GenerateInterruptExit,
    // which uses Always; the conditional arm is now a plain branch-over rather than a UDF, so a
    // future conditional caller gets correct code instead of a crash.
    return VaranEmitCond1(this, ((0x8000u | (uint32_t(r.code()) << 8)) << 16) | 0xF3EFu, c);
#else
    return writeInst(0x010f0000 | int(c) | RD(r));
#endif
}

BufferOffset
Assembler::as_msr(Register r, Condition c)
{
    // Hardcode the 'mask' field to 0b11 for now. It is bits 18 and 19, which
    // are the two high bits of the 'c' in this constant.
    MOZ_ASSERT((r.code() & ~0xf) == 0);
#if defined(VARAN_THUMB2)
    // MSR APSR_nzcvq, <Rn> (T1): hw0 = 0xF380 | Rn ; hw1 = 0x8000 | (mask << 8), mask = 0b1000.
    // Oracle: `msr apsr_nzcvq, r4` = 84 f3 00 88. This is the exact counterpart of the as_mrs above:
    // wasm::GenerateInterruptExit saves APSR with MRS and restores it with MSR, so both must name the
    // same field set. (The A32 line above encodes mask 0b1100 = f+s; T32 spells the flag-write
    // `nzcvq` = 0b1000, which is what `mrs ..., apsr` reads back.)
    return VaranEmitCond1(this, (0x8800u << 16) | (0xF380u | uint32_t(r.code())), c);
#else
    return writeInst(0x012cf000 | int(c) | r.code());
#endif
}

// VFP instructions!
enum vfp_tags {
    VfpTag   = 0x0C000A00,
    VfpArith = 0x02000000
};

BufferOffset
Assembler::writeVFPInst(vfp_size sz, uint32_t blob)
{
    MOZ_ASSERT((sz & blob) == 0);
    MOZ_ASSERT((VfpTag & blob) == 0);
    uint32_t a32 = VfpTag | sz | blob;   // full A32 VFP word (cond in bits 31:28)
#if defined(VARAN_THUMB2)
    // VFP is a mechanical near-copy (oracle 67/67). This funnel converts arith/move/cvt/cmp/vimm and
    // now VLDR/VSTR (as_vdtr) + VFP block-transfer (as_vdtm) -- all standard VFP words that differ from
    // A32 only in the cond nibble and halfword order.
    return varanEmitVfp(a32);
#else
    return writeInst(a32);
#endif
}

#if defined(VARAN_THUMB2)
BufferOffset
Assembler::varanEmitVfp(uint32_t a32)
{
    // T2 VFP word == the A32 word with the cond nibble forced to AL(1110), then halfword-swapped for
    // storage (hw0 = the high halfword first). CONDITIONAL VFP (cond != AL) -> B<!c>.W branch-over the
    // AL-forced body (predication choice: branch-over, never IT); the body is one wide (4-byte) VFP
    // instruction, so the skip is 4.
    uint32_t cond = (a32 >> 28) & 0xf;
    uint32_t al = (a32 & 0x0fffffffu) | 0xe0000000u;
    uint32_t t2 = ((al & 0xffffu) << 16) | (al >> 16);
    if (cond != 0xe) {
        // ★ A2 POOL GUARD. N=2: the B<!c>.W branch-over + the one wide VFP body word. The skip
        // constant 4 IS the contract, so a pool landing between them makes the taken path jump into
        // pool DATA. Demonstrated: 8 splits on the corpus pre-guard (CodeGeneratorARM::visitPowHalfD
        // emits loadConstantDouble -- allocating a pool entry -- immediately before this conditional
        // ma_vneg, so the pool is non-empty at exactly this emit point).
        VaranForbidPoolsIfOutermost varanAfp(this, 2);
        BufferOffset ret = writeInstT2(EncodeBccT2(4, uint32_t(Assembler::InvertCondition(Condition(cond << 28))) >> 28));
        writeInstT2(t2);
        return ret;
    }
    return writeInstT2(t2);
}
#endif

/* static */ void
Assembler::WriteVFPInstStatic(vfp_size sz, uint32_t blob, uint32_t* dest)
{
    MOZ_ASSERT((sz & blob) == 0);
    MOZ_ASSERT((VfpTag & blob) == 0);
    WriteInstStatic(VfpTag | sz | blob, dest);
}

// Unityped variants: all registers hold the same (ieee754 single/double)
// notably not included are vcvt; vmov vd, #imm; vmov rt, vn.
BufferOffset
Assembler::as_vfp_float(VFPRegister vd, VFPRegister vn, VFPRegister vm,
                  VFPOp op, Condition c)
{
    // Make sure we believe that all of our operands are the same kind.
    MOZ_ASSERT_IF(!vn.isMissing(), vd.equiv(vn));
    MOZ_ASSERT_IF(!vm.isMissing(), vd.equiv(vm));
    vfp_size sz = vd.isDouble() ? IsDouble : IsSingle;
    return writeVFPInst(sz, VD(vd) | VN(vn) | VM(vm) | op | VfpArith | c);
}

BufferOffset
Assembler::as_vadd(VFPRegister vd, VFPRegister vn, VFPRegister vm, Condition c)
{
    return as_vfp_float(vd, vn, vm, OpvAdd, c);
}

BufferOffset
Assembler::as_vdiv(VFPRegister vd, VFPRegister vn, VFPRegister vm, Condition c)
{
    return as_vfp_float(vd, vn, vm, OpvDiv, c);
}

BufferOffset
Assembler::as_vmul(VFPRegister vd, VFPRegister vn, VFPRegister vm, Condition c)
{
    return as_vfp_float(vd, vn, vm, OpvMul, c);
}

BufferOffset
Assembler::as_vnmul(VFPRegister vd, VFPRegister vn, VFPRegister vm, Condition c)
{
#if defined(VARAN_THUMB2)
    // SOURCE FIX: VNMUL = VMUL with bit6 (the negate bit) set. The A32 emitter reused OpvMul verbatim
    // -- a latent defect that emits a plain VMUL. The VFP near-copy would faithfully propagate that
    // wrong opcode, so fix it here (golden vnmul.f64 = EE210B42 vs plain vmul EE210B02, diff = bit6).
    return as_vfp_float(vd, vn, vm, VFPOp(OpvMul | 0x40), c);
#else
    return as_vfp_float(vd, vn, vm, OpvMul, c);
#endif
}

BufferOffset
Assembler::as_vnmla(VFPRegister vd, VFPRegister vn, VFPRegister vm, Condition c)
{
    MOZ_CRASH("Feature NYI");
}

BufferOffset
Assembler::as_vnmls(VFPRegister vd, VFPRegister vn, VFPRegister vm, Condition c)
{
    MOZ_CRASH("Feature NYI");
}

BufferOffset
Assembler::as_vneg(VFPRegister vd, VFPRegister vm, Condition c)
{
    return as_vfp_float(vd, NoVFPRegister, vm, OpvNeg, c);
}

BufferOffset
Assembler::as_vsqrt(VFPRegister vd, VFPRegister vm, Condition c)
{
    return as_vfp_float(vd, NoVFPRegister, vm, OpvSqrt, c);
}

BufferOffset
Assembler::as_vabs(VFPRegister vd, VFPRegister vm, Condition c)
{
    return as_vfp_float(vd, NoVFPRegister, vm, OpvAbs, c);
}

BufferOffset
Assembler::as_vsub(VFPRegister vd, VFPRegister vn, VFPRegister vm, Condition c)
{
    return as_vfp_float(vd, vn, vm, OpvSub, c);
}

BufferOffset
Assembler::as_vcmp(VFPRegister vd, VFPRegister vm, Condition c)
{
    return as_vfp_float(vd, NoVFPRegister, vm, OpvCmp, c);
}

BufferOffset
Assembler::as_vcmpz(VFPRegister vd, Condition c)
{
    return as_vfp_float(vd, NoVFPRegister, NoVFPRegister, OpvCmpz, c);
}

// Specifically, a move between two same sized-registers.
BufferOffset
Assembler::as_vmov(VFPRegister vd, VFPRegister vsrc, Condition c)
{
    return as_vfp_float(vd, NoVFPRegister, vsrc, OpvMov, c);
}

// Transfer between Core and VFP.

// Unlike the next function, moving between the core registers and vfp registers
// can't be *that* properly typed. Namely, since I don't want to munge the type
// VFPRegister to also include core registers. Thus, the core and vfp registers
// are passed in based on their type, and src/dest is determined by the
// float2core.

BufferOffset
Assembler::as_vxfer(Register vt1, Register vt2, VFPRegister vm, FloatToCore_ f2c,
                    Condition c, int idx)
{
    vfp_size sz = IsSingle;
    if (vm.isDouble()) {
        // Technically, this can be done with a vmov à la ARM ARM under vmov
        // however, that requires at least an extra bit saying if the operation
        // should be performed on the lower or upper half of the double. Moving
        // a single to/from 2N/2N+1 isn't equivalent, since there are 32 single
        // registers, and 32 double registers so there is no way to encode the
        // last 16 double registers.
        sz = IsDouble;
        MOZ_ASSERT(idx == 0 || idx == 1);
        // If we are transferring a single half of the double then it must be
        // moving a VFP reg to a core reg.
        MOZ_ASSERT_IF(vt2 == InvalidReg, f2c == FloatToCore);
        idx = idx << 21;
    } else {
        MOZ_ASSERT(idx == 0);
    }

    if (vt2 == InvalidReg)
        return writeVFPInst(sz, WordTransfer | f2c | c | RT(vt1) | maybeRN(vt2) | VN(vm) | idx);

    // We are doing a 64 bit transfer.
    return writeVFPInst(sz, DoubleTransfer | f2c | c | RT(vt1) | maybeRN(vt2) | VM(vm) | idx);
}

enum vcvt_destFloatness {
    VcvtToInteger = 1 << 18,
    VcvtToFloat  = 0 << 18
};
enum vcvt_toZero {
    VcvtToZero = 1 << 7, // Use the default rounding mode, which rounds truncates.
    VcvtToFPSCR = 0 << 7 // Use whatever rounding mode the fpscr specifies.
};
enum vcvt_Signedness {
    VcvtToSigned   = 1 << 16,
    VcvtToUnsigned = 0 << 16,
    VcvtFromSigned   = 1 << 7,
    VcvtFromUnsigned = 0 << 7
};

// Our encoding actually allows just the src and the dest (and their types) to
// uniquely specify the encoding that we are going to use.
BufferOffset
Assembler::as_vcvt(VFPRegister vd, VFPRegister vm, bool useFPSCR,
                   Condition c)
{
    // Unlike other cases, the source and dest types cannot be the same.
    MOZ_ASSERT(!vd.equiv(vm));
    vfp_size sz = IsDouble;
    if (vd.isFloat() && vm.isFloat()) {
        // Doing a float -> float conversion.
        if (vm.isSingle())
            sz = IsSingle;
        return writeVFPInst(sz, c | 0x02B700C0 | VM(vm) | VD(vd));
    }

    // At least one of the registers should be a float.
    vcvt_destFloatness destFloat;
    vcvt_Signedness opSign;
    vcvt_toZero doToZero = VcvtToFPSCR;
    MOZ_ASSERT(vd.isFloat() || vm.isFloat());
    if (vd.isSingle() || vm.isSingle())
        sz = IsSingle;

    if (vd.isFloat()) {
        destFloat = VcvtToFloat;
        opSign = (vm.isSInt()) ? VcvtFromSigned : VcvtFromUnsigned;
    } else {
        destFloat = VcvtToInteger;
        opSign = (vd.isSInt()) ? VcvtToSigned : VcvtToUnsigned;
        doToZero = useFPSCR ? VcvtToFPSCR : VcvtToZero;
    }
    return writeVFPInst(sz, c | 0x02B80040 | VD(vd) | VM(vm) | destFloat | opSign | doToZero);
}

BufferOffset
Assembler::as_vcvtFixed(VFPRegister vd, bool isSigned, uint32_t fixedPoint, bool toFixed, Condition c)
{
    MOZ_ASSERT(vd.isFloat());
    uint32_t sx = 0x1;
    vfp_size sf = vd.isDouble() ? IsDouble : IsSingle;
    int32_t imm5 = fixedPoint;
    imm5 = (sx ? 32 : 16) - imm5;
    MOZ_ASSERT(imm5 >= 0);
    imm5 = imm5 >> 1 | (imm5 & 1) << 5;
    return writeVFPInst(sf, 0x02BA0040 | VD(vd) | toFixed << 18 | sx << 7 |
                        (!isSigned) << 16 | imm5 | c);
}

// Transfer between VFP and memory.
static uint32_t
EncodeVdtr(LoadStore ls, VFPRegister vd, VFPAddr addr, Assembler::Condition c)
{
    return ls | 0x01000000 | addr.encode() | VD(vd) | c;
}

BufferOffset
Assembler::as_vdtr(LoadStore ls, VFPRegister vd, VFPAddr addr,
                   Condition c /* vfp doesn't have a wb option */)
{
    // VLDR/VSTR is a standard VFP word: the T2 encoding is the A32 encoding with cond forced to AL
    // and halfword-swapped -- exactly what writeVFPInst -> varanEmitVfp does for every other VFP op.
    // The offset (VFPOffImm, imm8 x4, |imm| <= 1020) survives addr.encode() identically across A32/T2;
    // pool read-back (PatchConstantPoolLoad case PoolVDTR) uses the same shared EncodeVdtr.
    vfp_size sz = vd.isDouble() ? IsDouble : IsSingle;
    return writeVFPInst(sz, EncodeVdtr(ls, vd, addr, c));
}

/* static */ void
Assembler::as_vdtr_patch(LoadStore ls, VFPRegister vd, VFPAddr addr, Condition c, uint32_t* dest)
{
    vfp_size sz = vd.isDouble() ? IsDouble : IsSingle;
#if defined(VARAN_THUMB2)
    // T2 VLDR-literal: the A32 VFP word with cond forced AL then halfword-swapped (same transform as
    // varanEmitVfp). A single 4B slot cannot hold a conditional VLDR -> AL-only (pool loads are AL).
    MOZ_ASSERT(c == Always);
    uint32_t a32 = VfpTag | sz | EncodeVdtr(ls, vd, addr, c);
    uint32_t al  = (a32 & 0x0fffffffu) | 0xe0000000u;
    uint32_t t2  = ((al & 0xffffu) << 16) | (al >> 16);
    WriteInstStatic(t2, dest);
#else
    WriteVFPInstStatic(sz, EncodeVdtr(ls, vd, addr, c), dest);
#endif
}

// VFP's ldm/stm work differently from the standard arm ones. You can only
// transfer a range.

BufferOffset
Assembler::as_vdtm(LoadStore st, Register rn, VFPRegister vd, int length,
                   /* also has update conditions */ Condition c)
{
    MOZ_ASSERT(length <= 16 && length >= 0);
    vfp_size sz = vd.isDouble() ? IsDouble : IsSingle;

    if (vd.isDouble())
        length *= 2;

    // VFP block-transfer (VLDM/VSTM/VPUSH/VPOP) is another cond-1110 near-copy (Batch 3): the backend
    // only emits the IA (vldmia) / DB (vstmdb, vpush/vpop) forms, whose A32 word halfword-swaps to the
    // exact T2 encoding. Route through writeVFPInst -> varanEmitVfp (same as the rest of VFP).
    return writeVFPInst(sz, dtmLoadStore | RN(rn) | VD(vd) | length |
                        dtmMode | dtmUpdate | dtmCond);
}

BufferOffset
Assembler::as_vimm(VFPRegister vd, VFPImm imm, Condition c)
{
    MOZ_ASSERT(imm.isValid());
    vfp_size sz = vd.isDouble() ? IsDouble : IsSingle;
    return writeVFPInst(sz,  c | imm.encode() | VD(vd) | 0x02B00000);

}

BufferOffset
Assembler::as_vmrs(Register r, Condition c)
{
    // vmrs/vmsr are VFP MCR/MRC forms but emit via writeInst (not writeVFPInst), so convert them
    // through the same near-copy helper directly.
#if defined(VARAN_THUMB2)
    return varanEmitVfp(c | 0x0ef10a10 | RT(r));
#else
    return writeInst(c | 0x0ef10a10 | RT(r));
#endif
}

BufferOffset
Assembler::as_vmsr(Register r, Condition c)
{
#if defined(VARAN_THUMB2)
    return varanEmitVfp(c | 0x0ee10a10 | RT(r));
#else
    return writeInst(c | 0x0ee10a10 | RT(r));
#endif
}

bool
Assembler::nextLink(BufferOffset b, BufferOffset* next)
{
    Instruction branch = *editSrc(b);
#if defined(VARAN_THUMB2)
    // P1.2b Group 2: real Thumb-2 fixup chain. The tolerant "non-branch => chain end" bringup
    // scaffold is GONE -- a non-branch at a chain node is now a genuine bug (MOZ_ASSERT). The
    // chain link lives in the slot that carries a full B.W field: for a 2-slot conditional branch
    // (slot0 = B<c>.W) that is the companion slot1 (b+4); for a 1-slot B.W/BL it is b itself.
    MOZ_ASSERT(branch.is<InstBranchImm>());
    uint32_t condField = 14u;
    VaranDecodeBranchCond(branch.encode(), &condField);
    bool twoSlot = branch.is<InstBImm>() && condField < 14u;   // B<c>.W carrier
    BufferOffset linkAt = twoSlot ? BufferOffset(b.getOffset() + 4) : b;
    int32_t v = VaranDecodeBranchByteVal(editSrc(linkAt)->encode());
    if (v == VARAN_BOFF_INVALID)
        return false;
    new (next) BufferOffset(v);
    return true;
#else
    MOZ_ASSERT(branch.is<InstBranchImm>());

    BOffImm destOff;
    branch.as<InstBranchImm>()->extractImm(&destOff);
    if (destOff.isInvalid())
        return false;

    // Propagate the next link back to the caller, by constructing a new
    // BufferOffset into the space they provided.
    new (next) BufferOffset(destOff.decode());
    return true;
#endif
}

void
Assembler::bind(Label* label, BufferOffset boff)
{
#ifdef JS_DISASM_ARM
    spewLabel(label);
#endif
    if (oom()) {
        // Ensure we always bind the label. This matches what we do on
        // x86/x64 and silences the assert in ~Label.
        label->bind(0);
        return;
    }

    if (label->used()) {
        bool more;
        // If our caller didn't give us an explicit target to bind to then we
        // want to bind to the location of the next instruction.
        BufferOffset dest = boff.assigned() ? boff : nextOffset();
        BufferOffset b(label);
        do {
            BufferOffset next;
            more = nextLink(b, &next);
            Instruction branch = *editSrc(b);
            Condition c = branch.extractCond();
            BOffImm offset = dest.diffB<BOffImm>(b);
            if (offset.isInvalid()) {
                m_buffer.fail_bail();
                return;
            }
#if defined(VARAN_THUMB2)
            // BL / unconditional B.W -> 1-slot patch. Conditional B -> 2-slot patch (B<c>.W+NOP.W
            // or, past +-1MB, invert+branch-over B.W). extractCond() != Always identifies the
            // conditional 2-slot form; diffB above already range-checked the target (+-16MB).
            if (branch.is<InstBLImm>())
                as_bl(offset, Always, b);
            else if (c != Always)
                varanPatchCondBranch2(b, dest.getOffset(), c);
            else if (branch.is<InstBImm>())
                as_b(offset, Always, b);
            else
                MOZ_CRASH("crazy fixup!");
#else
            if (branch.is<InstBImm>())
                as_b(offset, c, b);
            else if (branch.is<InstBLImm>())
                as_bl(offset, c, b);
            else
                MOZ_CRASH("crazy fixup!");
#endif
            b = next;
        } while (more);
    }
    label->bind(nextOffset().getOffset());
    MOZ_ASSERT(!oom());
}

void
Assembler::bindLater(Label* label, wasm::TrapDesc target)
{
    if (label->used()) {
        BufferOffset b(label);
        do {
            append(wasm::TrapSite(target, b.getOffset()));
        } while (nextLink(b, &b));
    }
    label->reset();
}

void
Assembler::bind(RepatchLabel* label)
{
    // It does not seem to be useful to record this label for
    // disassembly, as the value that is bound to the label is often
    // effectively garbage and is replaced by something else later.
    BufferOffset dest = nextOffset();
#if defined(VARAN_THUMB2)
    // The jumpWithPatch use is a reserved 2-slot conditional branch (slot0 = B<c>.W carrier, slot1 =
    // NOP.W), NOT a PoolHintData word. Recover the condition from slot0 and patch the pair to fall
    // through to `dest` (Delta = 8 -> always in-range B<c>.W + NOP.W; the overflow B.W form only
    // triggers for a real >+-1MB PatchJump retarget). Then bind the label to dest.
    if (label->used() && !oom()) {
        BufferOffset slot0(label->offset());
        uint32_t cf = 14u;
        VaranDecodeBranchCond(editSrc(slot0)->encode(), &cf);
        if (cf >= 14) {
            // ---- W1: NOT every RepatchLabel site is a 2-slot pair ----
            // (Mutation-tested 2026-07-22: forcing this branch to `false` -- i.e. restoring the
            // pre-W1 behaviour -- makes varanT2Wasm1Slot fail exactly its survival assertion.)
            //
            // Unconditional site -> patch slot0 ONLY. Writing slot0+4 here was ACTIVE MEMORY
            // CORRUPTION of emitted code, and the loud assert some tests hit was the LUCKY case:
            // it only fired because the offending branch happened to be the LAST instruction in
            // the buffer, so slot0+4 was past the end. When such a site is NOT last, the old code
            // silently wrote NOP.W over THE FOLLOWING REAL INSTRUCTION.
            //
            // The violator is MacroAssembler::wasmEmitTrapOutOfLineCode, case TrapSite::Jump: it
            // synthesizes a RepatchLabel over a site it did NOT create (`jump.use(site.codeOffset)`).
            // Every other RepatchLabel in the tree comes from jumpWithPatch / branchPtrWithPatch /
            // backedgeJump (verified: IonCaches x4, CodeGenerator OutOfLineUpdateCache::entry_,
            // CodeGenerator-shared x2), which all reserve the pair AT EMIT.
            //
            // WHY THE CONDITION FIELD IS THE RIGHT DISCRIMINATOR, and not merely a correlated proxy:
            // it is the SAME predicate that chose the reservation. as_b(Label*, c) sends c != Always
            // to varanAsBCond, which reserves both slots at emit AND writes the real condition into
            // slot0's carrier; c == Always takes the 1-slot path and leaves an unconditional B.W,
            // which VaranDecodeBranchCond reports as cf == 14. So reading cf back IS reading back
            // the emit-time reservation decision. (wasm trap jumps genuinely come in both shapes --
            // `jump(TrapDesc)` is Always, while branch32(AboveOrEqual, ..., oobTrap) is not.)
            //
            // THIS FIX CANNOT MOVE THE CORRUPTION -- the specific worry with the branch group, where
            // growing a site at bind time shifted everything after it. Here bind() now writes FEWER
            // words, never more, so no following instruction can shift.
            //
            // Safe for the Always sites that ARE 2-slot (jumpWithPatch/backedgeJump): slot1 already
            // holds the NOP.W that jumpWithPatch wrote (0x8000F3AF), and VaranComputeJump2's Always
            // arm would write VARAN_NOPW_WORD -- the same 0x8000F3AF. Skipping it is byte-for-byte
            // a no-op there, so this is not a trade-off between the two cases.
            uint32_t w0, w1;
            VaranComputeJump2(slot0.getOffset(), dest.getOffset(), cf, &w0, &w1);
            editSrc(slot0)->varanSetRaw(w0);
            (void)w1;   // deliberately NOT written -- it may be someone else's instruction
        } else {
            varanPatchCondBranch2(slot0, dest.getOffset(), Condition(cf << 28));
        }
    }
    label->bind(dest.getOffset());
    return;
#else
    if (label->used() && !oom()) {
        // If the label has a use, then change this use to refer to the bound
        // label.
        BufferOffset branchOff(label->offset());
        // Since this was created with a RepatchLabel, the value written in the
        // instruction stream is not branch shaped, it is PoolHintData shaped.
        Instruction* branch = editSrc(branchOff);
        PoolHintPun p;
        p.raw = branch->encode();
        Condition cond;
        if (p.phd.isValidPoolHint())
            cond = p.phd.getCond();
        else
            cond = branch->extractCond();

        BOffImm offset = dest.diffB<BOffImm>(branchOff);
        if (offset.isInvalid()) {
            m_buffer.fail_bail();
            return;
        }
        as_b(offset, cond, branchOff);
    }
    label->bind(dest.getOffset());
#endif
}

void
Assembler::retarget(Label* label, Label* target)
{
#ifdef JS_DISASM_ARM
    spewRetarget(label, target);
#endif
    if (label->used() && !oom()) {
        if (target->bound()) {
            bind(label, BufferOffset(target));
        } else if (target->used()) {
            // The target is not bound but used. Prepend label's branch list
            // onto target's.
            BufferOffset labelBranchOffset(label);
            BufferOffset next;

            // Find the head of the use chain for label.
            while (nextLink(labelBranchOffset, &next))
                labelBranchOffset = next;

            // Then patch the head of label's use chain to the tail of target's
            // use chain, prepending the entire use chain of target.
            Instruction branch = *editSrc(labelBranchOffset);
            int32_t prev = target->use(label->offset());
#if defined(VARAN_THUMB2)
            // Write the link 2-slot-aware: a conditional branch keeps its link in slot0+4, so a
            // plain 1-slot as_b(...,slot0) would store it where nextLink can't read it and SILENTLY
            // drop the spliced chain (the P2/audit bug). varanWriteChainLink handles both layouts.
            if (branch.is<InstBImm>() || branch.is<InstBLImm>())
                varanWriteChainLink(labelBranchOffset, prev);
            else
                MOZ_CRASH("crazy fixup!");
#else
            Condition c = branch.extractCond();
            if (branch.is<InstBImm>())
                as_b(BOffImm(prev), c, labelBranchOffset);
            else if (branch.is<InstBLImm>())
                as_bl(BOffImm(prev), c, labelBranchOffset);
            else
                MOZ_CRASH("crazy fixup!");
#endif
        } else {
            // The target is unbound and unused. We can just take the head of
            // the list hanging off of label, and dump that into target.
            DebugOnly<uint32_t> prev = target->use(label->offset());
            MOZ_ASSERT((int32_t)prev == Label::INVALID_OFFSET);
        }
    }
    label->reset();

}

static int stopBKPT = -1;
void
Assembler::as_bkpt()
{
    // This is a count of how many times a breakpoint instruction has been
    // generated. It is embedded into the instruction for debugging
    // purposes. Gdb will print "bkpt xxx" when you attempt to dissassemble a
    // breakpoint with the number xxx embedded into it. If this breakpoint is
    // being hit, then you can run (in gdb):
    //  >b dbg_break
    //  >b main
    //  >commands
    //  >set stopBKPT = xxx
    //  >c
    //  >end
    // which will set a breakpoint on the function dbg_break above set a
    // scripted breakpoint on main that will set the (otherwise unmodified)
    // value to the number of the breakpoint, so dbg_break will actuall be
    // called and finally, when you run the executable, execution will halt when
    // that breakpoint is generated.
    static int hit = 0;
    if (stopBKPT == hit)
        dbg_break();
#if defined(VARAN_THUMB2)
    // BKPT is 16-bit-only (T1: 0xBE00|imm8). Pack it low with a NOP16 high (0xbf00): stored word
    // (0xbf00<<16)|(0xBE00|imm8). BKPT needs no return address so the pad order is free. imm is 8-bit
    // in T2 (the A32 12-bit split is dropped).
    writeInstT2((0xbf00u << 16) | (0xbe00u | (hit & 0xff)));
#else
    writeInst(0xe1200070 | (hit & 0xf) | ((hit & 0xfff0) << 4));
#endif
    hit++;
}

void
Assembler::flushBuffer()
{
    m_buffer.flushPool();
}

void
Assembler::enterNoPool(size_t maxInst)
{
    m_buffer.enterNoPool(maxInst);
}

void
Assembler::leaveNoPool()
{
    m_buffer.leaveNoPool();
}

ptrdiff_t
Assembler::GetBranchOffset(const Instruction* i_)
{
    MOZ_ASSERT(i_->is<InstBranchImm>());
    InstBranchImm* i = i_->as<InstBranchImm>();
    BOffImm dest;
    i->extractImm(&dest);
    return dest.decode();
}

void
Assembler::RetargetNearBranch(Instruction* i, int offset, bool final)
{
    Assembler::Condition c = i->extractCond();
    RetargetNearBranch(i, offset, c, final);
}

void
Assembler::RetargetNearBranch(Instruction* i, int offset, Condition cond, bool final)
{
    // Retargeting calls is totally unsupported!
    MOZ_ASSERT_IF(i->is<InstBranchImm>(), i->is<InstBImm>() || i->is<InstBLImm>());
    if (i->is<InstBLImm>())
        new (i) InstBLImm(BOffImm(offset), cond);
    else
        new (i) InstBImm(BOffImm(offset), cond);

    // Flush the cache, since an instruction was overwritten.
    if (final)
        AutoFlushICache::flush(uintptr_t(i), 4);
}

void
Assembler::RetargetFarBranch(Instruction* i, uint8_t** slot, uint8_t* dest, Condition cond)
{
#if defined(VARAN_THUMB2)
    // DEAD under VARAN (FACT): the sole caller is the #else (A32) arm of jit::PatchJump -- the T2 arm
    // returns early, handling every range through the 2-slot reservation (>+-16MB trips
    // VaranComputeJump2). The body below is UNCONVERTED A32 and is silently wrong for Thumb-2 twice
    // over: it builds an InstLDR with the A32 `offset - 8` pc bias, while the InstLDR ctor now yields
    // a T2 pc-literal whose base is Align(pc+4,4) (-4), and it forwards a Condition into a form that
    // has no 1-slot T2 conditional analogue. Silently patching a wrong address is the worst outcome,
    // so fail loudly instead. If this is ever un-deadened: convert the bias to -4 and assert
    // cond == Always (a conditional far branch needs the 2-slot invert+branch-over form).
    MOZ_CRASH("VARAN: RetargetFarBranch is A32-only and must never execute under Thumb-2");
#endif
    int32_t offset = reinterpret_cast<uint8_t*>(slot) - reinterpret_cast<uint8_t*>(i);
    if (!i->is<InstLDR>()) {
        new (i) InstLDR(Offset, pc, DTRAddr(pc, DtrOffImm(offset - 8)), cond);
        AutoFlushICache::flush(uintptr_t(i), 4);
    }
    *slot = dest;
}

struct PoolHeader : Instruction
{
    struct Header
    {
        // The size should take into account the pool header.
        // The size is in units of Instruction (4 bytes), not byte.
        uint32_t size : 15;
        uint32_t isNatural : 1;  // Varan M1: uint32_t (not bool) so clang-cl packs it into the 32-bit unit -> sizeof(Header)==4
        uint32_t ONES : 16;

        Header(int size_, bool isNatural_)
          : size(size_),
            isNatural(isNatural_),
            ONES(0xffff)
        { }

        Header(const Instruction* i) {
            JS_STATIC_ASSERT(sizeof(Header) == sizeof(uint32_t));
            memcpy(this, i, sizeof(Header));
            MOZ_ASSERT(ONES == 0xffff);
        }

        uint32_t raw() const {
            JS_STATIC_ASSERT(sizeof(Header) == sizeof(uint32_t));
            uint32_t dest;
            memcpy(&dest, this, sizeof(Header));
            return dest;
        }
    };

    PoolHeader(int size_, bool isNatural_)
      : Instruction(Header(size_, isNatural_).raw(), true)
    { }

    uint32_t size() const {
        Header tmp(this);
        return tmp.size;
    }
    uint32_t isNatural() const {
        Header tmp(this);
        return tmp.isNatural;
    }

    static bool IsTHIS(const Instruction& i) {
        return (*i.raw() & 0xffff0000) == 0xffff0000;
    }
    static const PoolHeader* AsTHIS(const Instruction& i) {
        if (!IsTHIS(i))
            return nullptr;
        return static_cast<const PoolHeader*>(&i);
    }
};

void
Assembler::WritePoolHeader(uint8_t* start, Pool* p, bool isNatural)
{
    static_assert(sizeof(PoolHeader) == 4, "PoolHandler must have the correct size.");
    uint8_t* pool = start + 4;
    // Go through the usual rigmarole to get the size of the pool.
    pool += p->getPoolSize();
    uint32_t size = pool - start;
    MOZ_ASSERT((size & 3) == 0);
    size = size >> 2;
    MOZ_ASSERT(size < (1 << 15));
    PoolHeader header(size, isNatural);
    *(PoolHeader*)start = header;
}

// The size of an arbitrary 32-bit call in the instruction stream. On ARM this
// sequence is |pc = ldr pc - 4; imm32| given that we never reach the imm32.
uint32_t
Assembler::PatchWrite_NearCallSize()
{
    return sizeof(uint32_t);
}

void
Assembler::PatchWrite_NearCall(CodeLocationLabel start, CodeLocationLabel toCall)
{
    Instruction* inst = (Instruction*) start.raw();
    // Overwrite whatever instruction used to be here with a call. Since the
    // destination is in the same function, it will be within range of the
    // 24 << 2 byte bl instruction.
    uint8_t* dest = toCall.raw();
    new (inst) InstBLImm(BOffImm(dest - (uint8_t*)inst) , Always);
    // Ensure everyone sees the code that was just written into memory.
    AutoFlushICache::flush(uintptr_t(inst), 4);
}

void
Assembler::PatchDataWithValueCheck(CodeLocationLabel label, PatchedImmPtr newValue,
                                   PatchedImmPtr expectedValue)
{
    Instruction* ptr = reinterpret_cast<Instruction*>(label.raw());
    InstructionIterator iter(ptr);
    Register dest;
    Assembler::RelocStyle rs;

    DebugOnly<const uint32_t*> val = GetPtr32Target(&iter, &dest, &rs);
    MOZ_ASSERT(uint32_t((const uint32_t*)val) == uint32_t(expectedValue.value));

    MacroAssembler::ma_mov_patch(Imm32(int32_t(newValue.value)), dest, Always, rs, ptr);

    // L_LDR won't cause any instructions to be updated.
    if (rs != L_LDR) {
        AutoFlushICache::flush(uintptr_t(ptr), 4);
        AutoFlushICache::flush(uintptr_t(ptr->next()), 4);
    }
}

void
Assembler::PatchDataWithValueCheck(CodeLocationLabel label, ImmPtr newValue, ImmPtr expectedValue)
{
    PatchDataWithValueCheck(label,
                            PatchedImmPtr(newValue.value),
                            PatchedImmPtr(expectedValue.value));
}

// Stomps 32 bits of raw DATA into the reserved uint32 immediately before an Ion return address,
// recording the delta used to recover the IonScript* during invalidation.
//
// ===== TASK #32: DOES THIS NEED AN I-CACHE FLUSH?  VERDICT: NO. =====================
// The previous comment here ("that instruction will never be executed again, an ICache flush should
// not be necessary") was an assertion with no enumeration behind it, and that vagueness is what left
// the question open for a year. The enumeration, done properly:
//
//   WRITER  : exactly one, tree-wide -- jit::InvalidateActivation (Ion.cpp:3150).
//   READER  : exactly one -- JitFrameIterator::checkInvalidation (JitFrames.cpp:162), which reads it
//             as DATA (int32 offset, then Assembler::GetPointer), never fetches it as an instruction.
//   EXECUTION: the word is NOT reachable as code afterwards -- the OSI point is redirected to the
//             invalidation epilogue by PatchWrite_NearCall (Ion.cpp:3157) in the same loop iteration,
//             so control never returns to this address.
//
// ⚠️ The enumeration deliberately includes THIS defining file and computed/indirect access, not just
// a caller-name grep: the as_dtr_patch precedent in this project declared a helper dead using a grep
// that excluded its own defining file, where both callers lived. Residual risk, stated rather than
// hidden: a GENERIC code-stream walker (the disassembler, Assembler::NextInstruction) would read this
// word as an instruction, but none is run over invalidated Ion code on this path.
//
// Since both accesses are DATA on the same core, D-cache coherency is all that is required, and no
// I-cache maintenance is needed. (Contrast every ARM code-PATCH primitive, which does flush.)
// ====================================================================================
void
Assembler::PatchWrite_Imm32(CodeLocationLabel label, Imm32 imm) {
    // Raw is going to be the return address.
    uint32_t* raw = (uint32_t*)label.raw();
#if defined(VARAN_THUMB2)
    raw = (uint32_t*)((uintptr_t)raw & ~(uintptr_t)1);
#endif
#if defined(VARAN_THUMB2)
    // ★ C6 (lands with C5 in JitFrames.cpp -- SYMMETRIC, see below). On Thumb-2 label.raw() is the
    // ODD runtime return address (lr = next|1 from `as_orr rN,rN,#1; as_blx rN`). Pointer arithmetic
    // on the odd value put this store at evenRA-3, covering [evenRA-3 .. evenRA]: it missed the
    // bottom byte of the reserved slot and clobbered the FIRST BYTE OF THE LIVE INSTRUCTION at the
    // return address, at an odd (misaligned) address. Measured pre-fix:
    //   RA=0x2e4c6239 write=[0x2e4c6235,0x2e4c6239) slot=[0x2e4c6234,0x2e4c6238) -> OUT-OF-SLOT.
    //
    // ⚠️ MASK ONLY THE SLOT ADDRESS. Do NOT "fix" the delta or the reader's `returnAddr + delta`:
    //   delta = epilogueDataOffset - (oddRA - codeRaw)   [Ion.cpp:3148]
    //   read  = oddRA + delta = codeRaw + epilogueDataOffset
    // the odd bit CANCELS algebraically, so that value is already correct and masking one side of it
    // would MANUFACTURE the bug. Only the storage location was ever wrong, and it must be corrected
    // on BOTH ends together (here and JitFrames.cpp) or they stop addressing the same word.
    //
    // PROVEN (sim corpus, 2026-07-23): pre-fix the write was [evenRA-3 .. evenRA] with wrStart&3==1;
    // post-fix it is exactly the reserved slot with wrStart&3==0 -- 174042 in-slot observations and
    // ZERO footprint violations corpus-wide, evenRA&3 observed as 0 in every single case (so the
    // return address really is 4-aligned, which the reserved-uint32 comment only implied). Fixing
    // this removed ~280 failing invocations: it was NOT the latent/dead-code issue the old comment
    // suggested -- clobbering one byte of the live instruction on every invalidation was breaking
    // real tests.
#endif
    // Overwrite the 4 bytes before the return address, which will end up being
    // the call instruction.
    *(raw - 1) = imm.value;
}

uint8_t*
Assembler::NextInstruction(uint8_t* inst_, uint32_t* count)
{
    Instruction* inst = reinterpret_cast<Instruction*>(inst_);
    if (count != nullptr)
        *count += sizeof(Instruction);
    return reinterpret_cast<uint8_t*>(inst->next());
}

static bool
InstIsGuard(Instruction* inst, const PoolHeader** ph)
{
    Assembler::Condition c = inst->extractCond();
    if (c != Assembler::Always)
        return false;
    if (!(inst->is<InstBXReg>() || inst->is<InstBImm>()))
        return false;
    // See if the next instruction is a pool header.
    *ph = (inst + 1)->as<const PoolHeader>();
    return *ph != nullptr;
}

static bool
InstIsBNop(Instruction* inst)
{
    // In some special situations, it is necessary to insert a NOP into the
    // instruction stream that nobody knows about, since nobody should know
    // about it, make sure it gets skipped when Instruction::next() is called.
    // this generates a very specific nop, namely a branch to the next
    // instruction.
    Assembler::Condition c = inst->extractCond();
    if (c != Assembler::Always)
        return false;
    if (!inst->is<InstBImm>())
        return false;
    InstBImm* b = inst->as<InstBImm>();
    BOffImm offset;
    b->extractImm(&offset);
    return offset.decode() == 4;
}

static bool
InstIsArtificialGuard(Instruction* inst, const PoolHeader** ph)
{
    if (!InstIsGuard(inst, ph))
        return false;
    return !(*ph)->isNatural();
}

// If the instruction points to a artificial pool guard then skip the pool.
Instruction*
Instruction::skipPool()
{
    const PoolHeader* ph;
    // If this is a guard, and the next instruction is a header, always work
    // around the pool. If it isn't a guard, then start looking ahead.
    if (InstIsGuard(this, &ph)) {
        // Don't skip a natural guard.
        if (ph->isNatural())
            return this;
        return (this + 1 + ph->size())->skipPool();
    }
    if (InstIsBNop(this))
        return (this + 1)->skipPool();
    return this;
}

// Cases to be handled:
// 1) no pools or branches in sight => return this+1
// 2) branch to next instruction => return this+2, because a nop needed to be inserted into the stream.
// 3) this+1 is an artificial guard for a pool => return first instruction after the pool
// 4) this+1 is a natural guard => return the branch
// 5) this is a branch, right before a pool => return first instruction after the pool
// in assembly form:
// 1) add r0, r0, r0 <= this
//    add r1, r1, r1 <= returned value
//    add r2, r2, r2
//
// 2) add r0, r0, r0 <= this
//    b foo
//    foo:
//    add r2, r2, r2 <= returned value
//
// 3) add r0, r0, r0 <= this
//    b after_pool;
//    .word 0xffff0002  # bit 15 being 0 indicates that the branch was not requested by the assembler
//    0xdeadbeef        # the 2 indicates that there is 1 pool entry, and the pool header
//    add r4, r4, r4 <= returned value
// 4) add r0, r0, r0 <= this
//    b after_pool  <= returned value
//    .word 0xffff8002  # bit 15 being 1 indicates that the branch was requested by the assembler
//    0xdeadbeef
//    add r4, r4, r4
// 5) b after_pool  <= this
//    .word 0xffff8002  # bit 15 has no bearing on the returned value
//    0xdeadbeef
//    add r4, r4, r4  <= returned value

Instruction*
Instruction::next()
{
    Instruction* ret = this+1;
    const PoolHeader* ph;
    // If this is a guard, and the next instruction is a header, always work
    // around the pool. If it isn't a guard, then start looking ahead.
    if (InstIsGuard(this, &ph))
        return (ret + ph->size())->skipPool();
    if (InstIsArtificialGuard(ret, &ph))
        return (ret + 1 + ph->size())->skipPool();
    return ret->skipPool();
}

void
Assembler::ToggleToJmp(CodeLocationLabel inst_)
{
    uint32_t* ptr = (uint32_t*)inst_.raw();

#if defined(VARAN_THUMB2)
    // ENABLE: slot0 becomes NOP.W, so control falls into slot1's untouched B.W. See
    // MacroAssemblerARMCompat::toggledJump for why this is a 2-slot redesign and not a bit-flip.
    // The A32 read-back (is<InstCMP>, toRD == r0, the (0xf<<20) offset-preservation mask) has no T2
    // meaning and is replaced by exact word compares -- a strictly tighter check, since only two
    // words are ever legal in slot0.
    MOZ_ASSERT(*ptr == VaranToggleSkipW());        // must currently be DISABLED (mirrors A32's is<InstCMP>)
    MOZ_ASSERT(DecodeBranchT2(ptr[1]).valid);      // slot1 is still the branch: footprint intact
    *ptr = VARAN_NOPW_WORD;
    // Only slot0 changed, so a 4-byte flush is exact -- slot1 is never written and so can never be
    // stale. (Contrast PatchJump, which rewrites both slots and must flush 8.) The flush itself is
    // mandatory, not decorative: VENICE's I-cache is device-proven NOT auto-coherent, and these
    // sites toggle at runtime while the surrounding code is already executing.
    AutoFlushICache::flush(uintptr_t(ptr), 4);
    return;
#else
    DebugOnly<Instruction*> inst = (Instruction*)inst_.raw();
    MOZ_ASSERT(inst->is<InstCMP>());

    // Zero bits 20-27, then set 24-27 to be correct for a branch.
    // 20-23 will be party of the B's immediate, and should be 0.
    *ptr = (*ptr & ~(0xff << 20)) | (0xa0 << 20);
    AutoFlushICache::flush(uintptr_t(ptr), 4);
#endif
}

void
Assembler::ToggleToCmp(CodeLocationLabel inst_)
{
    uint32_t* ptr = (uint32_t*)inst_.raw();

#if defined(VARAN_THUMB2)
    // DISABLE: slot0 becomes B.W +4, branching over slot1 to the fallthrough. slot1's branch word is
    // left intact -- that is precisely what makes the toggle reversible without preserving any
    // encoded offset bits.
    //
    // NB the A32 name is kept for the shared call sites, but under Thumb-2 nothing here compares:
    // there is no CMP, and unlike the A32 form this clobbers no condition flags.
    MOZ_ASSERT(*ptr == VARAN_NOPW_WORD);           // must currently be ENABLED (mirrors A32's is<InstBImm>)
    MOZ_ASSERT(DecodeBranchT2(ptr[1]).valid);      // slot1 is still the branch: footprint intact
    *ptr = VaranToggleSkipW();
    AutoFlushICache::flush(uintptr_t(ptr), 4);
    return;
#else
    DebugOnly<Instruction*> inst = (Instruction*)inst_.raw();
    MOZ_ASSERT(inst->is<InstBImm>());

    // Ensure that this masking operation doesn't affect the offset of the
    // branch instruction when it gets toggled back.
    MOZ_ASSERT((*ptr & (0xf << 20)) == 0);

    // Also make sure that the CMP is valid. Part of having a valid CMP is that
    // all of the bits describing the destination in most ALU instructions are
    // all unset (looks like it is encoding r0).
    MOZ_ASSERT(toRD(*inst) == r0);

    // Zero out bits 20-27, then set them to be correct for a compare.
    *ptr = (*ptr & ~(0xff << 20)) | (0x35 << 20);

    AutoFlushICache::flush(uintptr_t(ptr), 4);
#endif
}

void
Assembler::ToggleCall(CodeLocationLabel inst_, bool enabled)
{
    Instruction* inst = (Instruction*)inst_.raw();
    // Skip a pool with an artificial guard.
    inst = inst->skipPool();
    MOZ_ASSERT(inst->is<InstMovW>() || inst->is<InstLDR>());

    if (inst->is<InstMovW>()) {
        // If it looks like the start of a movw/movt sequence, then make sure we
        // have all of it (and advance the iterator past the full sequence).
        inst = inst->next();
        MOZ_ASSERT(inst->is<InstMovT>());
#if defined(VARAN_THUMB2)
        // B1: toggledCall emits an unconditional `orr scratch, scratch, #1` (the Thumb bit) between
        // the movw/movt pair and the toggled slot, in BOTH arms, so the footprint stays uniform at
        // 16 bytes and the toggled word stays last. Step over it.
        inst = inst->next();
#endif
    }

    inst = inst->next();
    MOZ_ASSERT(inst->is<InstNOP>() || inst->is<InstBLXReg>());

    if (enabled == inst->is<InstBLXReg>()) {
        // Nothing to do.
        return;
    }

    if (enabled)
        *inst = InstBLXReg(ScratchRegister, Always);
    else
        *inst = InstNOP();

    AutoFlushICache::flush(uintptr_t(inst), 4);
}

size_t
Assembler::ToggledCallSize(uint8_t* code)
{
    Instruction* inst = (Instruction*)code;
    // Skip a pool with an artificial guard.
    inst = inst->skipPool();
    MOZ_ASSERT(inst->is<InstMovW>() || inst->is<InstLDR>());

    if (inst->is<InstMovW>()) {
        // If it looks like the start of a movw/movt sequence, then make sure we
        // have all of it (and advance the iterator past the full sequence).
        inst = inst->next();
        MOZ_ASSERT(inst->is<InstMovT>());
#if defined(VARAN_THUMB2)
        // B1: toggledCall emits an unconditional `orr scratch, scratch, #1` (the Thumb bit) between
        // the movw/movt pair and the toggled slot, in BOTH arms, so the footprint stays uniform at
        // 16 bytes and the toggled word stays last. Step over it.
        inst = inst->next();
#endif
    }

    inst = inst->next();
    MOZ_ASSERT(inst->is<InstNOP>() || inst->is<InstBLXReg>());
    return uintptr_t(inst) + 4 - uintptr_t(code);
}

uint8_t*
Assembler::BailoutTableStart(uint8_t* code)
{
    Instruction* inst = (Instruction*)code;
    // Skip a pool with an artificial guard or NOP fill.
    inst = inst->skipPool();
    MOZ_ASSERT(inst->is<InstBLImm>());
    return (uint8_t*) inst;
}

InstructionIterator::InstructionIterator(Instruction* i_)
  : i(i_)
{
    // Work around pools with an artificial pool guard and around nop-fill.
    i = i->skipPool();
}

uint32_t Assembler::NopFill = 0;

uint32_t
Assembler::GetNopFill()
{
    static bool isSet = false;
    if (!isSet) {
        char* fillStr = getenv("ARM_ASM_NOP_FILL");
        uint32_t fill;
        if (fillStr && sscanf(fillStr, "%u", &fill) == 1)
            NopFill = fill;
        if (NopFill > 8)
            MOZ_CRASH("Nop fill > 8 is not supported");
        isSet = true;
    }
    return NopFill;
}

uint32_t Assembler::AsmPoolMaxOffset = 1024;

uint32_t
Assembler::GetPoolMaxOffset()
{
    static bool isSet = false;
    if (!isSet) {
        char* poolMaxOffsetStr = getenv("ASM_POOL_MAX_OFFSET");
        uint32_t poolMaxOffset;
        if (poolMaxOffsetStr && sscanf(poolMaxOffsetStr, "%u", &poolMaxOffset) == 1)
            AsmPoolMaxOffset = poolMaxOffset;
        isSet = true;
    }
    return AsmPoolMaxOffset;
}

SecondScratchRegisterScope::SecondScratchRegisterScope(MacroAssembler &masm)
  : AutoRegisterScope(masm, masm.getSecondScratchReg())
{
}
