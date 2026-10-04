// SPDX-License-Identifier: MIT
#include "cpu/decoder.hpp"

namespace nanorv::cpu {
namespace {

constexpr unsigned bit(u32 value, unsigned index) noexcept {
    return (value >> index) & 1u;
}

constexpr u32 bits(u32 value, unsigned hi, unsigned lo) noexcept {
    return (value >> lo) & ((1u << (hi - lo + 1)) - 1u);
}

Decoded decode_base(u32 raw) noexcept {
    Decoded d{};
    d.raw = raw;
    d.length = 4;
    d.compressed = false;
    d.opcode = static_cast<u8>(bits(raw, 6, 0));
    d.rd = static_cast<u8>(bits(raw, 11, 7));
    d.rs1 = static_cast<u8>(bits(raw, 19, 15));
    d.rs2 = static_cast<u8>(bits(raw, 24, 20));
    d.rs3 = static_cast<u8>(bits(raw, 31, 27));
    d.funct3 = static_cast<u8>(bits(raw, 14, 12));
    d.funct7 = static_cast<u8>(bits(raw, 31, 25));

    switch (d.opcode) {
    case 0x37: // LUI
    case 0x17: // AUIPC
        d.format = InsnFormat::U;
        break;
    case 0x6f: // JAL
        d.format = InsnFormat::J;
        break;
    case 0x67: // JALR
    case 0x13: // OP-IMM
    case 0x1b: // OP-IMM-32
    case 0x03: // LOAD
        d.format = InsnFormat::I;
        break;
    case 0x63: // BRANCH
        d.format = InsnFormat::B;
        break;
    case 0x23: // STORE
        d.format = InsnFormat::S;
        break;
    case 0x33: // OP
    case 0x3b: // OP-32
    case 0x0f: // MISC-MEM (FENCE)
        d.format = InsnFormat::R;
        break;
    case 0x73: // SYSTEM
        d.format = InsnFormat::System;
        break;
    case 0x07: // LOAD-FP
    case 0x27: // STORE-FP
    case 0x43: // FMADD
    case 0x47: // FMSUB
    case 0x4b: // FNMSUB
    case 0x4f: // FNMADD
    case 0x53: // OP-FP
        d.format = InsnFormat::R4;
        break;
    default:
        d.format = InsnFormat::Unknown;
        break;
    }
    return d;
}

// Compressed (RVC) field extraction. The three quadrants alias rd'/rs1'/rs2'
// onto x8..x15.
Decoded decode_compressed(u32 raw) noexcept {
    Decoded d{};
    d.raw = raw;
    d.length = 2;
    d.compressed = true;
    const u32 quadrant = bits(raw, 1, 0);
    d.funct3 = static_cast<u8>(bits(raw, 15, 13));
    d.opcode = static_cast<u8>(quadrant); // quadrant in the opcode field
    d.funct7 = 0;

    const u32 rd_full = bits(raw, 11, 7);
    const u32 rs2_full = bits(raw, 6, 2);
    const u32 rd_short = 8u + bits(raw, 4, 2);
    const u32 rs1_short = 8u + bits(raw, 9, 7);
    const u32 rs2_short = 8u + bits(raw, 4, 2);

    switch (quadrant) {
    case 0:
        d.rd = static_cast<u8>(rd_short);
        d.rs1 = static_cast<u8>(rs1_short);
        d.rs2 = static_cast<u8>(rs2_short);
        d.format = (d.funct3 == 0) ? InsnFormat::U : InsnFormat::R; // C.ADDI4SPN vs loads/stores
        break;
    case 1:
        d.rd = static_cast<u8>(rd_full);
        d.rs1 = static_cast<u8>(rd_full);
        d.rs2 = static_cast<u8>(rs2_short);
        switch (d.funct3) {
        case 0:
        case 1:
        case 2:
        case 3:
            d.format = InsnFormat::I;
            break;
        case 4:
            // C.SRLI/C.SRAI/C.ANDI (rd' == rs1') or C.SUB/C.XOR/C.SUBW/C.ADDW
            d.rd = static_cast<u8>(rs1_short);
            d.rs1 = static_cast<u8>(rs1_short);
            d.funct7 = static_cast<u8>(bit(raw, 12) << 6);
            d.format = (bit(raw, 12) == 0) ? InsnFormat::I : InsnFormat::R;
            break;
        case 5:
            d.rd = 0;
            d.rs1 = 0;
            d.format = InsnFormat::J;
            break;
        default: // C.BEQZ / C.BNEZ
            d.rd = 0;
            d.rs1 = static_cast<u8>(rs1_short);
            d.format = InsnFormat::B;
            break;
        }
        break;
    default: // quadrant 2
        switch (d.funct3) {
        case 0: // C.SLLI
            d.rd = static_cast<u8>(rd_full);
            d.rs1 = static_cast<u8>(rd_full);
            d.format = InsnFormat::I;
            break;
        case 1: // C.FLDSP
        case 2: // C.LWSP
        case 3: // C.LDSP
            d.rd = static_cast<u8>(rd_full);
            d.rs1 = 0;
            d.format = InsnFormat::I;
            break;
        case 4: { // C.JR / C.MV / C.EBREAK / C.JALR / C.ADD
            if (bit(raw, 12) == 0) {
                if (rs2_full == 0) {
                    d.rd = 0;                  // C.JR rs1
                    d.rs1 = static_cast<u8>(rd_full);
                    d.rs2 = 0;
                    d.format = InsnFormat::I;
                } else {                      // C.MV rd, rs2
                    d.rd = static_cast<u8>(rd_full);
                    d.rs1 = static_cast<u8>(rd_full);
                    d.rs2 = static_cast<u8>(rs2_full);
                    d.format = InsnFormat::R;
                }
            } else if (rd_full == 0) {
                d.rd = 0;                      // C.EBREAK
                d.rs1 = 0;
                d.rs2 = 0;
                d.format = InsnFormat::System;
            } else if (rs2_full == 0) {
                d.rd = 0;                      // C.JALR rs1
                d.rs1 = static_cast<u8>(rd_full);
                d.rs2 = 0;
                d.format = InsnFormat::I;
            } else {                           // C.ADD rd, rs2
                d.rd = static_cast<u8>(rd_full);
                d.rs1 = static_cast<u8>(rd_full);
                d.rs2 = static_cast<u8>(rs2_full);
                d.format = InsnFormat::R;
            }
            break;
        }
        default: // C.FSDSP / C.SWSP / C.SDSP
            d.rs2 = static_cast<u8>(rs2_full);
            d.rs1 = 0;
            d.format = InsnFormat::S;
            break;
        }
        break;
    }
    return d;
}

} // namespace

u32 instruction_length(u16 low16) noexcept {
    // RV64GC contains no instruction longer than 32 bits: the 48-bit and longer
    // encodings only exist for FLQ/FSQ, which are RV32-only. Any bit pattern that
    // would decode as a longer instruction falls into the 32-bit path and is
    // rejected by the opcode dispatcher with an illegal-instruction trap, so the
    // fetcher never skips a byte it should not.
    if ((low16 & 0x3u) != 0x3u) {
        return 2; // 16-bit compressed
    }
    return 4;
}

Decoded decode(u32 raw) noexcept {
    if ((raw & 0x3u) != 0x3u) {
        return decode_compressed(raw);
    }
    return decode_base(raw);
}

const char* to_string(InsnFormat format) noexcept {
    switch (format) {
    case InsnFormat::R:      return "R";
    case InsnFormat::R4:     return "R4";
    case InsnFormat::I:      return "I";
    case InsnFormat::S:      return "S";
    case InsnFormat::B:      return "B";
    case InsnFormat::U:      return "U";
    case InsnFormat::J:      return "J";
    case InsnFormat::System: return "SYSTEM";
    case InsnFormat::Unknown: break;
    }
    return "?";
}

} // namespace nanorv::cpu