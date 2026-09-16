/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/loongarch64/MoveEmitter-loongarch64.h"

#include "jit/MacroAssembler-inl.h"

using namespace js;
using namespace js::jit;

Address
MoveEmitterLoongArch64::cycleSlot(uint32_t slot, uint32_t subslot) const
{
    int32_t offset = masm.framePushed() - pushedAtCycle_;
    MOZ_ASSERT(Imm16::IsInSignedRange(offset));
    return Address(StackPointer, offset + slot * Simd128DataSize + subslot);
}

void
MoveEmitterLoongArch64::emit(const MoveResolver& moves)
{
    if (moves.numCycles()) {
        masm.reserveStack(moves.numCycles() * Simd128DataSize);
        pushedAtCycle_ = masm.framePushed();
    }

    for (size_t i = 0; i < moves.numMoves(); i++)
        emit(moves.getMove(i));
}

void
MoveEmitterLoongArch64::emit(const MoveOp& move)
{
    const MoveOperand& from = move.from();
    const MoveOperand& to = move.to();

    if (move.isCycleEnd() && move.isCycleBegin()) {
        breakCycle(from, to, move.endCycleType(), move.cycleBeginSlot());
        completeCycle(from, to, move.type(), move.cycleEndSlot());
        return;
    }

    if (move.isCycleEnd()) {
        MOZ_ASSERT(inCycle_);
        completeCycle(from, to, move.type(), move.cycleEndSlot());
        MOZ_ASSERT(inCycle_ > 0);
        inCycle_--;
        return;
    }

    if (move.isCycleBegin()) {
        breakCycle(from, to, move.endCycleType(), move.cycleBeginSlot());
        inCycle_++;
    }

    switch (move.type()) {
      case MoveOp::FLOAT32:
        emitFloat32Move(from, to);
        break;
      case MoveOp::DOUBLE:
        emitDoubleMove(from, to);
        break;
      case MoveOp::INT32:
        emitInt32Move(from, to);
        break;
      case MoveOp::GENERAL:
        emitMove(from, to);
        break;
      case MoveOp::SIMD128INT:
        emitSimd128IntMove(from, to);
        break;
      case MoveOp::SIMD128FLOAT:
        emitSimd128FloatMove(from, to);
        break;
      default:
        MOZ_CRASH("Unexpected move type");
    }
}

void
MoveEmitterLoongArch64::breakCycle(const MoveOperand& from, const MoveOperand& to,
                                   MoveOp::Type type, uint32_t slotId)
{
    switch (type) {
      case MoveOp::FLOAT32:
        if (to.isMemory()) {
            FloatRegister temp = ScratchFloat32Reg;
            masm.loadFloat32(getAdjustedAddress(to), temp);
            masm.storeFloat32(temp, cycleSlot(slotId));
        } else {
            masm.storeFloat32(to.floatReg(), cycleSlot(slotId));
        }
        break;
      case MoveOp::DOUBLE:
        if (to.isMemory()) {
            FloatRegister temp = ScratchDoubleReg;
            masm.loadDouble(getAdjustedAddress(to), temp);
            masm.storeDouble(temp, cycleSlot(slotId));
        } else {
            masm.storeDouble(to.floatReg(), cycleSlot(slotId));
        }
        break;
      case MoveOp::SIMD128INT:
        if (to.isMemory()) {
            FloatRegister temp = ScratchSimd128Reg;
            masm.loadAlignedSimd128Int(getAdjustedAddress(to), temp);
            masm.storeAlignedSimd128Int(temp, cycleSlot(slotId));
        } else {
            masm.storeAlignedSimd128Int(to.floatReg(), cycleSlot(slotId));
        }
        break;
      case MoveOp::SIMD128FLOAT:
        if (to.isMemory()) {
            FloatRegister temp = ScratchSimd128Reg;
            masm.loadAlignedSimd128Float(getAdjustedAddress(to), temp);
            masm.storeAlignedSimd128Float(temp, cycleSlot(slotId));
        } else {
            masm.storeAlignedSimd128Float(to.floatReg(), cycleSlot(slotId));
        }
        break;
      case MoveOp::INT32:
        if (to.isMemory()) {
            Register temp = tempReg();
            masm.load32(getAdjustedAddress(to), temp);
            masm.store32(temp, cycleSlot(0));
        } else {
            MOZ_ASSERT(to.reg() != spilledReg_);
            masm.store32(to.reg(), cycleSlot(0));
        }
        break;
      case MoveOp::GENERAL:
        if (to.isMemory()) {
            Register temp = tempReg();
            masm.loadPtr(getAdjustedAddress(to), temp);
            masm.storePtr(temp, cycleSlot(0));
        } else {
            MOZ_ASSERT(to.reg() != spilledReg_);
            masm.storePtr(to.reg(), cycleSlot(0));
        }
        break;
      default:
        MOZ_CRASH("Unexpected move type");
    }
}

void
MoveEmitterLoongArch64::completeCycle(const MoveOperand& from, const MoveOperand& to,
                                      MoveOp::Type type, uint32_t slotId)
{
    switch (type) {
      case MoveOp::FLOAT32:
        if (to.isMemory()) {
            FloatRegister temp = ScratchFloat32Reg;
            masm.loadFloat32(cycleSlot(slotId), temp);
            masm.storeFloat32(temp, getAdjustedAddress(to));
        } else {
            masm.loadFloat32(cycleSlot(slotId), to.floatReg());
        }
        break;
      case MoveOp::DOUBLE:
        if (to.isMemory()) {
            FloatRegister temp = ScratchDoubleReg;
            masm.loadDouble(cycleSlot(slotId), temp);
            masm.storeDouble(temp, getAdjustedAddress(to));
        } else {
            masm.loadDouble(cycleSlot(slotId), to.floatReg());
        }
        break;
      case MoveOp::SIMD128INT:
        if (to.isMemory()) {
            FloatRegister temp = ScratchSimd128Reg;
            masm.loadAlignedSimd128Int(cycleSlot(slotId), temp);
            masm.storeAlignedSimd128Int(temp, getAdjustedAddress(to));
        } else {
            masm.loadAlignedSimd128Int(cycleSlot(slotId), to.floatReg());
        }
        break;
      case MoveOp::SIMD128FLOAT:
        if (to.isMemory()) {
            FloatRegister temp = ScratchSimd128Reg;
            masm.loadAlignedSimd128Float(cycleSlot(slotId), temp);
            masm.storeAlignedSimd128Float(temp, getAdjustedAddress(to));
        } else {
            masm.loadAlignedSimd128Float(cycleSlot(slotId), to.floatReg());
        }
        break;
      case MoveOp::INT32:
        MOZ_ASSERT(slotId == 0);
        if (to.isMemory()) {
            Register temp = tempReg();
            masm.load32(cycleSlot(0), temp);
            masm.store32(temp, getAdjustedAddress(to));
        } else {
            MOZ_ASSERT(to.reg() != spilledReg_);
            masm.load32(cycleSlot(0), to.reg());
        }
        break;
      case MoveOp::GENERAL:
        MOZ_ASSERT(slotId == 0);
        if (to.isMemory()) {
            Register temp = tempReg();
            masm.loadPtr(cycleSlot(0), temp);
            masm.storePtr(temp, getAdjustedAddress(to));
        } else {
            MOZ_ASSERT(to.reg() != spilledReg_);
            masm.loadPtr(cycleSlot(0), to.reg());
        }
        break;
      default:
        MOZ_CRASH("Unexpected move type");
    }
}

void
MoveEmitterLoongArch64::emitSimd128IntMove(const MoveOperand& from, const MoveOperand& to)
{
    MOZ_ASSERT_IF(from.isFloatReg(), from.floatReg().isSimd128());
    MOZ_ASSERT_IF(to.isFloatReg(), to.floatReg().isSimd128());

    if (from.isFloatReg()) {
        if (to.isFloatReg()) {
            masm.moveSimd128Int(from.floatReg(), to.floatReg());
        } else {
            MOZ_ASSERT(to.isMemory());
            masm.storeAlignedSimd128Int(from.floatReg(), getAdjustedAddress(to));
        }
    } else if (to.isFloatReg()) {
        MOZ_ASSERT(from.isMemory());
        masm.loadAlignedSimd128Int(getAdjustedAddress(from), to.floatReg());
    } else {
        MOZ_ASSERT(from.isMemory());
        MOZ_ASSERT(to.isMemory());
        FloatRegister temp = ScratchSimd128Reg;
        masm.loadAlignedSimd128Int(getAdjustedAddress(from), temp);
        masm.storeAlignedSimd128Int(temp, getAdjustedAddress(to));
    }
}

void
MoveEmitterLoongArch64::emitSimd128FloatMove(const MoveOperand& from, const MoveOperand& to)
{
    MOZ_ASSERT_IF(from.isFloatReg(), from.floatReg().isSimd128());
    MOZ_ASSERT_IF(to.isFloatReg(), to.floatReg().isSimd128());

    if (from.isFloatReg()) {
        if (to.isFloatReg()) {
            masm.moveSimd128Float(from.floatReg(), to.floatReg());
        } else {
            MOZ_ASSERT(to.isMemory());
            masm.storeAlignedSimd128Float(from.floatReg(), getAdjustedAddress(to));
        }
    } else if (to.isFloatReg()) {
        MOZ_ASSERT(from.isMemory());
        masm.loadAlignedSimd128Float(getAdjustedAddress(from), to.floatReg());
    } else {
        MOZ_ASSERT(from.isMemory());
        MOZ_ASSERT(to.isMemory());
        FloatRegister temp = ScratchSimd128Reg;
        masm.loadAlignedSimd128Float(getAdjustedAddress(from), temp);
        masm.storeAlignedSimd128Float(temp, getAdjustedAddress(to));
    }
}

void
MoveEmitterLoongArch64::emitDoubleMove(const MoveOperand& from, const MoveOperand& to)
{
    MOZ_ASSERT_IF(from.isFloatReg(), from.floatReg() != ScratchDoubleReg);
    MOZ_ASSERT_IF(to.isFloatReg(), to.floatReg() != ScratchDoubleReg);

    if (from.isFloatReg()) {
        if (to.isFloatReg()) {
            masm.moveDouble(from.floatReg(), to.floatReg());
        } else if (to.isGeneralReg()) {
            masm.moveFromDouble(from.floatReg(), to.reg());
        } else {
            MOZ_ASSERT(to.isMemory());
            masm.storeDouble(from.floatReg(), getAdjustedAddress(to));
        }
    } else if (to.isFloatReg()) {
        if (from.isMemory())
            masm.loadDouble(getAdjustedAddress(from), to.floatReg());
        else
            masm.moveToDouble(from.reg(), to.floatReg());
    } else {
        MOZ_ASSERT(from.isMemory());
        MOZ_ASSERT(to.isMemory());
        masm.loadDouble(getAdjustedAddress(from), ScratchDoubleReg);
        masm.storeDouble(ScratchDoubleReg, getAdjustedAddress(to));
    }
}
