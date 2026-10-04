// SPDX-License-Identifier: MIT
//
// RV64M: integer multiplication and division (ISA manual, "M Standard
// Extension"). Reached only when funct7 == 0x01, so plain RV64I OP/OP-32
// encodings are untouched.
#include "cpu/execute_muldiv.hpp"

#include "cpu/exec_util.hpp"
#include "cpu/softint.hpp"
#include "core/log.hpp"

namespace nanorv::cpu {
namespace {

// funct3 -> mnemonic, shared by the disassembler and the log messages.
const char* muldiv_name(u32 funct3, bool word) noexcept {
    static const char* const names_xlen[8] = {"mul",  "mulh",   "mulhsu", "mulhu",
                                              "div",  "divu",   "rem",    "remu"};
    static const char* const names_word[8] = {"mulw", "?",      "?",      "?",
                                              "divw", "divuw",  "remw",   "remuw"};
    return word ? names_word[funct3] : names_xlen[funct3];
}

bool execute_muldiv_xlen(Hart& hart, const Decoded& insn, Trap& trap) noexcept {
    const u64 a = hart.get_reg(insn.rs1);
    const u64 b = hart.get_reg(insn.rs2);
    u64 result = 0;
    switch (insn.funct3) {
    case 0: result = a * b; break;                                                 // MUL
    case 1: result = mul_high_signed(static_cast<i64>(a), static_cast<i64>(b)); break; // MULH
    case 2: result = mul_high_signed_unsigned(static_cast<i64>(a), b); break;          // MULHSU
    case 3: result = mul_high_unsigned(a, b); break;                                   // MULHU
    case 4: result = div_signed(static_cast<i64>(a), static_cast<i64>(b)); break;      // DIV
    case 5: result = div_unsigned(a, b); break;                                        // DIVU
    case 6: result = rem_signed(static_cast<i64>(a), static_cast<i64>(b)); break;      // REM
    case 7: result = rem_unsigned(a, b); break;                                        // REMU
    default: return detail::illegal_insn(insn, trap);
    }
    hart.set_reg(insn.rd, result);
    return true;
}

// The *W forms compute on 32-bit operands and sign-extend the result to 64 bits.
// Doing the arithmetic on the sign/zero extended operands and truncating back to
// 32 bits reproduces every specified edge case, including INT32_MIN / -1.
bool execute_muldiv_word(Hart& hart, const Decoded& insn, Trap& trap) noexcept {
    const u64 raw_a = hart.get_reg(insn.rs1);
    const u64 raw_b = hart.get_reg(insn.rs2);
    u32 result = 0;
    switch (insn.funct3) {
    case 0: { // MULW
        const i64 a = static_cast<i32>(raw_a);
        const i64 b = static_cast<i32>(raw_b);
        result = static_cast<u32>(static_cast<u64>(a) * static_cast<u64>(b));
        break;
    }
    case 4: { // DIVW
        const i64 a = static_cast<i32>(raw_a);
        const i64 b = static_cast<i32>(raw_b);
        result = static_cast<u32>(div_signed(a, b));
        break;
    }
    case 5: { // DIVUW
        const u64 a = zext32(raw_a);
        const u64 b = zext32(raw_b);
        result = static_cast<u32>(div_unsigned(a, b));
        break;
    }
    case 6: { // REMW
        const i64 a = static_cast<i32>(raw_a);
        const i64 b = static_cast<i32>(raw_b);
        result = static_cast<u32>(rem_signed(a, b));
        break;
    }
    case 7: { // REMUW
        const u64 a = zext32(raw_a);
        const u64 b = zext32(raw_b);
        result = static_cast<u32>(rem_unsigned(a, b));
        break;
    }
    default:
        // MULH/MULHSU/MULHU have no 32-bit counterpart.
        return detail::illegal_insn(insn, trap);
    }
    hart.set_reg(insn.rd, sext32_to_64(result));
    return true;
}

} // namespace

bool is_muldiv(const Decoded& insn) noexcept {
    if (insn.compressed) {
        return false;
    }
    return (insn.opcode == 0x33 || insn.opcode == 0x3b) && insn.funct7 == 0x01;
}

bool execute_muldiv(Hart& hart, const memory::AddressSpace&, const Decoded& insn, Trap& trap) noexcept {
    if (!is_muldiv(insn)) {
        return detail::illegal_insn(insn, trap);
    }
    if (insn.opcode == 0x3b) {
        return execute_muldiv_word(hart, insn, trap);
    }
    return execute_muldiv_xlen(hart, insn, trap);
}

const char* muldiv_mnemonic(const Decoded& insn) noexcept {
    if (insn.funct3 > 7) {
        return "?";
    }
    return muldiv_name(insn.funct3, insn.opcode == 0x3b);
}

} // namespace nanorv::cpu