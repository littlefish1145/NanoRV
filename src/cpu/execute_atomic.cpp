// SPDX-License-Identifier: MIT
#include "cpu/execute_atomic.hpp"

#include "cpu/exec_util.hpp"
#include "cpu/softint.hpp"
#include "core/log.hpp"
#include <atomic>

namespace rvvm::cpu {
namespace {

using detail::illegal_insn;

// funct5 values, i.e. funct7[6:2].
enum class AmoOp : u8 {
    Add = 0x00,
    Swap = 0x01,
    Lr = 0x02,
    Sc = 0x03,
    Xor = 0x04,
    Or = 0x08,
    And = 0x0c,
    Min = 0x10,
    Max = 0x14,
    Minu = 0x18,
    Maxu = 0x1c,
};

const char* amo_name(u32 funct5, bool word) noexcept {
    switch (static_cast<AmoOp>(funct5)) {
    case AmoOp::Add: return word ? "amoadd.w" : "amoadd.d";
    case AmoOp::Swap: return word ? "amoswap.w" : "amoswap.d";
    case AmoOp::Lr: return word ? "lr.w" : "lr.d";
    case AmoOp::Sc: return word ? "sc.w" : "sc.d";
    case AmoOp::Xor: return word ? "amoxor.w" : "amoxor.d";
    case AmoOp::Or: return word ? "amoor.w" : "amoor.d";
    case AmoOp::And: return word ? "amoand.w" : "amoand.d";
    case AmoOp::Min: return word ? "amomin.w" : "amomin.d";
    case AmoOp::Max: return word ? "amomax.w" : "amomax.d";
    case AmoOp::Minu: return word ? "amominu.w" : "amominu.d";
    case AmoOp::Maxu: return word ? "amomaxu.w" : "amomaxu.d";
    }
    return "?";
}

// Applies one of the read-modify-write operations. `old_value` and `operand` are
// already truncated to the access width, so the same code serves .W and .D.
u64 apply_amo(u32 funct5, u64 old_value, u64 operand, bool word) noexcept {
    const u64 mask = word ? 0xffff'ffffull : ~static_cast<u64>(0);
    const u64 a = old_value & mask;
    const u64 b = operand & mask;
    switch (static_cast<AmoOp>(funct5)) {
    case AmoOp::Swap: return b;
    case AmoOp::Xor: return a ^ b;
    case AmoOp::Or: return a | b;
    case AmoOp::And: return a & b;
    case AmoOp::Add: return a + b;
    case AmoOp::Min:
    case AmoOp::Max: {
        const bool a_less = word ? (static_cast<i32>(static_cast<u32>(a)) < static_cast<i32>(static_cast<u32>(b)))
                                 : (static_cast<i64>(a) < static_cast<i64>(b));
        const bool take_a = (static_cast<AmoOp>(funct5) == AmoOp::Min) ? a_less : !a_less;
        return take_a ? a : b;
    }
    case AmoOp::Minu:
    case AmoOp::Maxu: {
        const bool a_less = a < b;
        const bool take_a = (static_cast<AmoOp>(funct5) == AmoOp::Minu) ? a_less : !a_less;
        return take_a ? a : b;
    }
    case AmoOp::Lr:
    case AmoOp::Sc:
        break; // handled separately
    }
    return a;
}

bool execute_lr(Hart& hart, const memory::AddressSpace& memory, const Decoded& insn, unsigned width,
                Trap& trap) noexcept {
    const u64 address = hart.get_reg(insn.rs1);
    if (!detail::check_natural_alignment(address, width, /*is_load=*/true, trap)) {
        return false;
    }
    u64 physical;
    if(!mmu::translate(hart,memory,address,width,memory::Access::Read,physical,trap))return false;
    // This machine's atomic PMA is RAM; device registers are not reservation
    // storage and must not be read speculatively by an unsupported LR.
    if(!memory.direct(physical,width/8,memory::Access::Read)) {
        trap={TrapCause::LoadAccessFault,address,true};return false;
    }
    u64 value = 0;
    auto guard=memory.atomic_guard(physical);
    if (!memory.load(physical,width,value)) {
        trap={TrapCause::LoadAccessFault,address,true};
        return false;
    }
    hart.set_reservation(physical,width);
    hart.reservation_version=memory.reservation_version(physical);
    hart.set_reg(insn.rd, width == 32 ? sext32_to_64(value) : value);
    return true;
}

bool execute_sc(Hart& hart, const memory::AddressSpace& memory, const Decoded& insn, unsigned width,
                Trap& trap) noexcept {
    const u64 address = hart.get_reg(insn.rs1);
    const u64 operand = hart.get_reg(insn.rs2);
    // An SC always clears the reservation, whether it succeeds or not.
    u64 physical;
    if(!mmu::translate(hart,memory,address,width,memory::Access::Write,physical,trap)) {hart.clear_reservation();return false;}
    auto guard=memory.atomic_guard(physical);
    const bool reserved = hart.reservation_matches(physical) && hart.reservation_width>=width &&
        (!memory.concurrent() || hart.reservation_version==memory.reservation_version(physical));
    hart.clear_reservation();

    if (!detail::check_natural_alignment(address, width, /*is_load=*/false, trap)) {
        return false;
    }
    // Failed SC still performs protection checks, without performing a store.
    if(!memory.direct(physical,width/8,memory::Access::Write)) {
        trap={TrapCause::StoreAccessFault,address,true};return false;
    }
    if (!reserved) {
        // The store must not be performed; rd reports the failure.
        hart.set_reg(insn.rd, 1);
        return true;
    }
    const u64 value = width == 32 ? static_cast<u32>(operand) : operand;
    if (!memory.store_locked(physical,width,value,guard)) {
        trap={TrapCause::StoreAccessFault,address,true};
        return false;
    }
    hart.set_reg(insn.rd, 0);
    return true;
}

bool execute_amo(Hart& hart, const memory::AddressSpace& memory, const Decoded& insn, unsigned width,
                 u32 funct5, Trap& trap) noexcept {
    const u64 address = hart.get_reg(insn.rs1);
    const u64 operand = hart.get_reg(insn.rs2);
    if (!detail::check_natural_alignment(address, width, /*is_load=*/false, trap)) {
        return false;
    }

    u64 physical;
    if(!mmu::translate(hart,memory,address,width,memory::Access::Write,physical,trap))return false;
    if(!memory.direct(physical,width/8,memory::Access::Write)) {
        trap={TrapCause::StoreAccessFault,address,true};return false;
    }
    u64 old_value = 0;
    u64 read_physical=0;
    if (!mmu::translate(hart,memory,address,width,memory::Access::Read,read_physical,trap)) {
        // AMO faults are store/AMO faults, including the read half.
        if(trap.cause==TrapCause::LoadPageFault)trap.cause=TrapCause::StorePageFault;
        if(trap.cause==TrapCause::LoadAccessFault)trap.cause=TrapCause::StoreAccessFault;
        return false;
    }
    auto guard=memory.atomic_guard(physical);
    if(!memory.load(read_physical,width,old_value)) {trap={TrapCause::StoreAccessFault,address,true};return false;}
    const bool word = (width == 32);
    const u64 new_value = apply_amo(funct5, old_value, operand, word);
    if (!memory.store_locked(physical,width,new_value,guard)) {
        trap={TrapCause::StoreAccessFault,address,true};
        return false;
    }
    // AMO*.W sign-extends the loaded value into rd.
    hart.set_reg(insn.rd, word ? sext32_to_64(old_value) : old_value);
    return true;
}

} // namespace

bool is_atomic(const Decoded& insn) noexcept {
    return !insn.compressed && insn.opcode == 0x2f;
}

static bool execute_atomic_operation(Hart& hart, const memory::AddressSpace& memory, const Decoded& insn,
                    Trap& trap) noexcept {
    if (!is_atomic(insn)) {
        return illegal_insn(insn, trap);
    }

    // funct3 selects the width: 0b010 = .W, 0b011 = .D.
    unsigned width = 0;
    switch (insn.funct3) {
    case 2: width = 32; break;
    case 3: width = 64; break;
    default: return illegal_insn(insn, trap);
    }

    const u32 funct5 = insn.funct7 >> 2;
    switch (static_cast<AmoOp>(funct5)) {
    case AmoOp::Lr: {
        // LR encodes the full register number of rd in the rd field and
        // requires rs2 == 0.
        if (insn.rs2 != 0) {
            return illegal_insn(insn, trap);
        }
        return execute_lr(hart, memory, insn, width, trap);
    }
    case AmoOp::Sc:
        return execute_sc(hart, memory, insn, width, trap);
    case AmoOp::Add:
    case AmoOp::Swap:
    case AmoOp::Xor:
    case AmoOp::Or:
    case AmoOp::And:
    case AmoOp::Min:
    case AmoOp::Max:
    case AmoOp::Minu:
    case AmoOp::Maxu:
        return execute_amo(hart, memory, insn, width, funct5, trap);
    }
    return illegal_insn(insn, trap);
}

bool execute_atomic(Hart& hart,const memory::AddressSpace& memory,const Decoded& insn,Trap& trap) noexcept {
    // Conservative RCsc ordering for aq/rl, independent of ordinary RAM store
    // ordering. Keep unannotated operations atomic through the stripe lock.
    const auto ordering=insn.funct7&3;
    // Release orders earlier accesses before the operation; acquire orders the
    // operation before later accesses. On our x64 backend the stripe's locked
    // acquisition also orders a preceding release against a subsequent acquire.
    if(ordering&1)std::atomic_thread_fence(std::memory_order_seq_cst);
    const bool result=execute_atomic_operation(hart,memory,insn,trap);
    if(ordering&2)std::atomic_thread_fence(std::memory_order_seq_cst);
    return result;
}

const char* atomic_mnemonic(const Decoded& insn) noexcept {
    if (insn.funct3 > 3) {
        return "?";
    }
    return amo_name(insn.funct7 >> 2, /*word=*/insn.funct3 == 2);
}

} // namespace rvvm::cpu
