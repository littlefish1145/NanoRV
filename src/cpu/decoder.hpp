// SPDX-License-Identifier: MIT
//
// Instruction length + field extraction for RV64GC. No execution semantics here.
//
// RVC encodings are decoded so that instruction boundaries are correct and so
// that the disassembler and the trace can be written against one field set.
#pragma once

#include "core/types.hpp"

namespace rvvm::cpu {

enum class InsnFormat : u8 {
    Unknown = 0,
    R,   // register-register
    R4,  // FP register-register-register-register (F/D, stage 3)
    I,   // register-immediate
    S,   // store
    B,   // conditional branch
    U,   // upper immediate
    J,   // jump
    System,
};

const char* to_string(InsnFormat format) noexcept;

struct Decoded {
    u32 raw = 0;          // instruction bits, right aligned
    u8 length = 4;        // 2 or 4 bytes
    bool compressed = false;
    InsnFormat format = InsnFormat::Unknown;
    u8 opcode = 0;        // bits [6:0] (for compressed: the quadrant/funct pair)
    u8 rd = 0;
    u8 rs1 = 0;
    u8 rs2 = 0;
    u8 rs3 = 0;           // FP encodings only
    u8 funct3 = 0;
    u8 funct7 = 0;
};

// Decodes the already-fetched instruction bits. The caller guarantees that
// `raw` holds a full 16 or 32 bit instruction (length determined first).
Decoded decode(u32 raw) noexcept;

// Instruction length in bytes from the low 16 bits.
// RV64GC has no encoding longer than 32 bits, so the result is always 2 or 4.
u32 instruction_length(u16 low16) noexcept;

// Sign extension helpers used by both the decoder and the executor.
constexpr i64 sign_extend(u64 value, unsigned bits) noexcept {
    if (bits == 0 || bits >= 64) {
        return static_cast<i64>(value);
    }
    const u64 mask = (static_cast<u64>(1) << bits) - 1;
    const u64 sign = static_cast<u64>(1) << (bits - 1);
    return static_cast<i64>((value & mask) ^ sign) - static_cast<i64>(sign);
}

constexpr i32 sign_extend_32(u32 value, unsigned bits) noexcept {
    return static_cast<i32>(static_cast<i64>(sign_extend(value, bits)));
}

} // namespace rvvm::cpu