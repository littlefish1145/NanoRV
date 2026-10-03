// SPDX-License-Identifier: MIT
#include "cpu/execute.hpp"

#include "cpu/exec_util.hpp"
#include "cpu/execute_atomic.hpp"
#include "cpu/execute_csr.hpp"
#include "cpu/execute_muldiv.hpp"
#include "cpu/compressed.hpp"
#include "cpu/privileged.hpp"
#include "cpu/execute_fp.hpp"
#include "cpu/execute_bitmanip.hpp"
#include "cpu/execute_crypto.hpp"
#include "core/log.hpp"
#include <atomic>

namespace rvvm::cpu {
namespace {

using detail::bits;
using detail::do_load;
using detail::do_store;
using detail::illegal_insn;

constexpr i64 imm_i(u32 raw) noexcept {
    return static_cast<i64>(static_cast<i32>(raw)) >> 20;
}

constexpr i64 imm_s(u32 raw) noexcept {
    const u32 value = bits(raw, 31, 25) << 5 | bits(raw, 11, 7);
    return sign_extend(value, 12);
}

constexpr i64 imm_b(u32 raw) noexcept {
    const u32 value = (bits(raw, 31, 31) << 12) | (bits(raw, 7, 7) << 11) | bits(raw, 30, 25) << 5 |
                      bits(raw, 11, 8) << 1;
    return sign_extend(value, 13);
}

constexpr i64 imm_j(u32 raw) noexcept {
    const u32 value = (bits(raw, 31, 31) << 20) | (bits(raw, 19, 12) << 12) | bits(raw, 20, 20) << 11 |
                      bits(raw, 30, 21) << 1;
    return sign_extend(value, 21);
}

constexpr i64 imm_u(u32 raw) noexcept {
    return sign_extend(raw & 0xfffff000u, 32);
}

// ---------------------------------------------------------------------------
// Instruction groups
// ---------------------------------------------------------------------------
// Instruction groups
// ---------------------------------------------------------------------------

bool exec_lui(Hart& hart, const Decoded& insn) noexcept {
    hart.set_reg(insn.rd, static_cast<u64>(imm_u(insn.raw)));
    return true;
}

bool exec_auipc(Hart& hart, const Decoded& insn) noexcept {
    hart.set_reg(insn.rd, hart.pc + static_cast<u64>(imm_u(insn.raw)));
    return true;
}

bool exec_jal(Hart& hart, const Decoded& insn, Trap& trap) noexcept {
    const u64 target = hart.pc + static_cast<u64>(imm_j(insn.raw));
    if ((target & 1u) != 0) {
        trap = Trap{TrapCause::InstructionAddressMisaligned, target, true};
        return false;
    }
    hart.set_reg(insn.rd, hart.pc + insn.length);
    hart.next_pc = target;
    return true;
}

bool exec_jalr(Hart& hart, const Decoded& insn, Trap& trap) noexcept {
    if (insn.funct3 != 0) {
        return illegal_insn(insn, trap);
    }
    const u64 unaligned = hart.get_reg(insn.rs1) + static_cast<u64>(imm_i(insn.raw));
    const u64 target = unaligned & ~static_cast<u64>(1);
    hart.set_reg(insn.rd, hart.pc + insn.length);
    hart.next_pc = target;
    return true;
}

bool exec_branch(Hart& hart, const Decoded& insn, Trap& trap) noexcept {
    const u64 a = hart.get_reg(insn.rs1);
    const u64 b = hart.get_reg(insn.rs2);
    bool taken = false;
    switch (insn.funct3) {
    case 0: taken = a == b; break;                        // BEQ
    case 1: taken = a != b; break;                        // BNE
    case 4: taken = static_cast<i64>(a) < static_cast<i64>(b); break;  // BLT
    case 5: taken = static_cast<i64>(a) >= static_cast<i64>(b); break; // BGE
    case 6: taken = a < b; break;                         // BLTU
    case 7: taken = a >= b; break;                        // BGEU
    default: return illegal_insn(insn, trap);
    }
    if (!taken) {
        return true;
    }
    const u64 target = hart.pc + static_cast<u64>(imm_b(insn.raw));
    if ((target & 1u) != 0) {
        trap = Trap{TrapCause::InstructionAddressMisaligned, target, true};
        return false;
    }
    hart.next_pc = target;
    return true;
}

bool exec_load(Hart& hart, const memory::AddressSpace& memory, const Decoded& insn, Trap& trap) noexcept {
    const u64 address = hart.get_reg(insn.rs1) + static_cast<u64>(imm_i(insn.raw));
    u64 raw_value = 0;
    unsigned width = 0;
    bool sign = false;
    switch (insn.funct3) {
    case 0: width = 8; sign = true; break;  // LB
    case 1: width = 16; sign = true; break; // LH
    case 2: width = 32; sign = true; break; // LW
    case 3: width = 64; sign = false; break;// LD
    case 4: width = 8; sign = false; break; // LBU
    case 5: width = 16; sign = false; break;// LHU
    case 6: width = 32; sign = false; break;// LWU
    default: return illegal_insn(insn, trap);
    }
    if (!do_load(hart, memory, address, width, raw_value, trap)) {
        return false;
    }
    hart.set_reg(insn.rd, sign ? static_cast<u64>(sign_extend(raw_value, width)) : raw_value);
    return true;
}

bool exec_store(Hart& hart, const memory::AddressSpace& memory, const Decoded& insn, Trap& trap) noexcept {
    unsigned width = 0;
    switch (insn.funct3) {
    case 0: width = 8; break;  // SB
    case 1: width = 16; break; // SH
    case 2: width = 32; break; // SW
    case 3: width = 64; break; // SD
    default: return illegal_insn(insn, trap);
    }
    const u64 address = hart.get_reg(insn.rs1) + static_cast<u64>(imm_s(insn.raw));
    return do_store(hart, memory, address, width, hart.get_reg(insn.rs2), trap);
}

bool exec_op_imm(Hart& hart, const Decoded& insn, Trap& trap) noexcept {
    const u64 a = hart.get_reg(insn.rs1);
    const i64 imm = imm_i(insn.raw);
    u64 result = 0;
    switch (insn.funct3) {
    case 0: result = a + static_cast<u64>(imm); break;                    // ADDI
    case 2: result = static_cast<i64>(a) < imm ? 1u : 0u; break;          // SLTI
    case 3: result = a < static_cast<u64>(imm) ? 1u : 0u; break;          // SLTIU
    case 4: result = a ^ static_cast<u64>(imm); break;                     // XORI
    case 6: result = a | static_cast<u64>(imm); break;                     // ORI
    case 7: result = a & static_cast<u64>(imm); break;                     // ANDI
    case 1: {                                                                // SLLI
        // RV64 shift immediates encode a 6-bit shamt, so bits [31:26] must be zero
        // while bit [25] is shamt[5].
        if (bits(insn.raw, 31, 26) != 0) {
            return illegal_insn(insn, trap);
        }
        result = a << bits(insn.raw, 25, 20);
        break;
    }
    case 5: {                                                                // SRLI / SRAI
        const unsigned shift = bits(insn.raw, 25, 20);
        const u32 top = bits(insn.raw, 31, 26);
        if (top == 0x00) {
            result = a >> shift;
        } else if (top == 0x10) {
            result = static_cast<u64>(static_cast<i64>(a) >> shift);
        } else {
            return illegal_insn(insn, trap);
        }
        break;
    }
    default: return illegal_insn(insn, trap);
    }
    hart.set_reg(insn.rd, result);
    return true;
}

bool exec_op_imm_32(Hart& hart, const Decoded& insn,
                    Trap& trap) noexcept {
    const u32 a = static_cast<u32>(hart.get_reg(insn.rs1));
    u32 result = 0;
    switch (insn.funct3) {
    case 0: result = a + static_cast<u32>(imm_i(insn.raw)); break;          // ADDIW
    case 1: {                                                               // SLLIW
        if (insn.funct7 != 0) {
            return illegal_insn(insn, trap);
        }
        result = a << bits(insn.raw, 24, 20);
        break;
    }
    case 5: {                                                               // SRLIW / SRAIW
        const unsigned shift = bits(insn.raw, 24, 20);
        if (insn.funct7 == 0x00) {
            result = a >> shift;
        } else if (insn.funct7 == 0x20) {
            result = static_cast<u32>(static_cast<i32>(a) >> shift);
        } else {
            return illegal_insn(insn, trap);
        }
        break;
    }
    default: return illegal_insn(insn, trap);
    }
    hart.set_reg(insn.rd, static_cast<u64>(static_cast<i64>(static_cast<i32>(result))));
    return true;
}

bool exec_op(Hart& hart, const Decoded& insn, Trap& trap) noexcept {
    const u64 a = hart.get_reg(insn.rs1);
    const u64 b = hart.get_reg(insn.rs2);
    u64 result = 0;
    if (insn.funct7 == 0x00) {
        switch (insn.funct3) {
        case 0: result = a + b; break;                              // ADD
        case 1: result = a << (b & 0x3fu); break;                  // SLL
        case 2: result = static_cast<i64>(a) < static_cast<i64>(b) ? 1u : 0u; break; // SLT
        case 3: result = a < b ? 1u : 0u; break;                   // SLTU
        case 4: result = a ^ b; break;                              // XOR
        case 5: result = a >> (b & 0x3fu); break;                  // SRL
        case 6: result = a | b; break;                              // OR
        case 7: result = a & b; break;                              // AND
        default: return illegal_insn(insn, trap);
        }
    } else if (insn.funct7 == 0x20) {
        switch (insn.funct3) {
        case 0: result = a - b; break;                                          // SUB
        case 5: result = static_cast<u64>(static_cast<i64>(a) >> (b & 0x3fu)); break; // SRA
        default: return illegal_insn(insn, trap);
        }
    } else {
        return illegal_insn(insn, trap);
    }
    hart.set_reg(insn.rd, result);
    return true;
}

bool exec_op_32(Hart& hart, const Decoded& insn, Trap& trap) noexcept {
    const u32 a = static_cast<u32>(hart.get_reg(insn.rs1));
    const u32 b = static_cast<u32>(hart.get_reg(insn.rs2));
    u32 result = 0;
    if (insn.funct7 == 0x00) {
        switch (insn.funct3) {
        case 0: result = a + b; break;             // ADDW
        case 1: result = a << (b & 0x1fu); break; // SLLW
        case 5: result = a >> (b & 0x1fu); break; // SRLW
        default: return illegal_insn(insn, trap);
        }
    } else if (insn.funct7 == 0x20) {
        switch (insn.funct3) {
        case 0: result = a - b; break;                                       // SUBW
        case 5: result = static_cast<u32>(static_cast<i32>(a) >> (b & 0x1fu)); break; // SRAW
        default: return illegal_insn(insn, trap);
        }
    } else {
        return illegal_insn(insn, trap);
    }
    hart.set_reg(insn.rd, static_cast<u64>(static_cast<i64>(static_cast<i32>(result))));
    return true;
}

bool exec_misc_mem(const Decoded& insn, Trap& trap) noexcept {
    switch (insn.funct3) {
    case 0: // Stronger host ordering is permitted by RVWMO, including aq/rl.
    case 1: // Code-page generations are acquired by every linked block entry.
        std::atomic_thread_fence(std::memory_order_seq_cst);
        return true;
    default:
        return illegal_insn(insn, trap);
    }
}

Trap ecall_trap(Privilege privilege) noexcept {
    switch (privilege) {
    case Privilege::User:
        return Trap{TrapCause::EnvironmentCallUser, 0, false};
    case Privilege::Supervisor:
        return Trap{TrapCause::EnvironmentCallSupervisor, 0, false};
    case Privilege::Machine:
        return Trap{TrapCause::EnvironmentCallMachine, 0, false};
    }
    return Trap{TrapCause::EnvironmentCallMachine, 0, false};
}

bool exec_system(Hart& hart, const Decoded& insn, Trap& trap) noexcept {
    if (insn.raw != 0x00000073 && insn.raw != 0x00100073) return execute_privileged(hart,insn,trap);
    if (insn.funct3 != 0) {
        // Zicsr lives in its own instruction group; see execute_csr.cpp.
        return illegal_insn(insn, trap);
    }
    const u32 imm = bits(insn.raw, 31, 20);
    if (insn.rd != 0 || insn.rs1 != 0) {
        // ECALL/EBREAK/MRET/SRET/WFI/SFENCE.VMA all have rd == 0, rs1 == 0.
        return illegal_insn(insn, trap);
    }
    switch (imm) {
    case 0x000:
        trap = ecall_trap(hart.privilege);
        return false;
    case 0x001:
        trap = Trap{TrapCause::Breakpoint, hart.pc, true};
        return false;
    default:
        // MRET (0x302), SRET (0x102), WFI (0x105) and SFENCE.VMA (0x120) are
        // implemented in stage 4 together with the CSR file they require.
        return illegal_insn(insn, trap);
    }
}

} // namespace

bool fetch(const memory::AddressSpace& memory, u64 pc, Decoded& out, Trap& trap) noexcept {
    if ((pc & 1u) != 0) {
        trap = Trap{TrapCause::InstructionAddressMisaligned, pc, true};
        return false;
    }
    u64 low = 0;
    if (!memory.load(pc, 16, low, /*for_exec=*/true)) {
        trap = Trap{TrapCause::InstructionAccessFault, pc, true};
        return false;
    }
    const u32 length = instruction_length(static_cast<u16>(low));
    u32 raw = static_cast<u32>(low);
    if (length == 4) {
        u64 high = 0;
        if (!memory.load(pc + 2, 16, high, /*for_exec=*/true)) {
            trap = Trap{TrapCause::InstructionAccessFault, pc, true};
            return false;
        }
        raw |= static_cast<u32>(high & 0xffffu) << 16;
    }
    out = decode(raw);
    return true;
}

bool fetch(Hart& hart, const memory::AddressSpace& memory, Decoded& out, Trap& trap) noexcept {
    if(hart.pc&1) {trap={TrapCause::InstructionAddressMisaligned,hart.pc,true};return false;}
    u64 lo,hi;
    if(!mmu::load(hart,memory,hart.pc,16,lo,trap,true))return false;
    u32 raw=static_cast<u32>(lo);
    if(instruction_length(static_cast<u16>(lo))==4) {
        if(!mmu::load(hart,memory,hart.pc+2,16,hi,trap,true))return false;
        raw|=static_cast<u32>(hi)<<16;
    }
    out=decode(raw);return true;
}
bool execute(Hart& hart, const memory::AddressSpace& memory, const Decoded& insn, Trap& trap) noexcept {
    if (insn.compressed) {
        struct CachedCompressed { u32 raw = ~0u; Decoded decoded{}; bool valid = false; };
        static thread_local CachedCompressed cache[1024];
        auto& entry = cache[(insn.raw >> 2) & 1023];
        if (entry.raw != insn.raw) {
            entry.raw = insn.raw;
            const u32 expanded = expand_compressed(static_cast<u16>(insn.raw));
            entry.valid = expanded != 0;
            entry.decoded = decode(expanded);
            entry.decoded.length = 2;
        }
        if (!entry.valid) return illegal_insn(insn, trap);
        const bool ok = execute(hart, memory, entry.decoded, trap);
        if (!ok && trap.cause == TrapCause::IllegalInstruction) trap.tval = insn.raw;
        return ok;
    }

    if(execute_bitmanip(hart,insn)) { ++hart.retired;return true; }
    if(execute_crypto(hart,insn)) { ++hart.retired;return true; }
    bool ok = false;
    switch (insn.opcode) {
    case 0x37: ok = exec_lui(hart, insn); break;
    case 0x17: ok = exec_auipc(hart, insn); break;
    case 0x6f: ok = exec_jal(hart, insn, trap); break;
    case 0x67: ok = exec_jalr(hart, insn, trap); break;
    case 0x63: ok = exec_branch(hart, insn, trap); break;
    case 0x03: ok = exec_load(hart, memory, insn, trap); break;
    case 0x23: ok = exec_store(hart, memory, insn, trap); break;
    case 0x13: ok = exec_op_imm(hart, insn, trap); break;
    case 0x1b: ok = exec_op_imm_32(hart, insn, trap); break;
    case 0x33:
        // funct7 == 0x01 selects RV64M, everything else is base RV64I.
        ok = (insn.funct7 == 0x01) ? execute_muldiv(hart, memory, insn, trap)
                                  : exec_op(hart, insn, trap);
        break;
    case 0x3b:
        ok = (insn.funct7 == 0x01) ? execute_muldiv(hart, memory, insn, trap)
                                  : exec_op_32(hart, insn, trap);
        break;
    case 0x2f: ok = execute_atomic(hart, memory, insn, trap); break;
    case 0x07: case 0x27: case 0x43: case 0x47: case 0x4b: case 0x4f: case 0x53:
        ok=execute_fp(hart,memory,insn,trap);break;
    case 0x0f: ok = exec_misc_mem(insn, trap); break;
    case 0x73:
        ok = (insn.funct3 == 0) ? exec_system(hart, insn, trap) : execute_csr(hart, insn, trap);
        break;
    default:
        ok = illegal_insn(insn, trap);
        break;
    }

    if (ok) {
        ++hart.retired;
    }
    return ok;
}

} // namespace rvvm::cpu
