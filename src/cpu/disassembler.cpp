// SPDX-License-Identifier: MIT
#include "cpu/disassembler.hpp"

#include "cpu/csr.hpp"
#include "cpu/execute_atomic.hpp"
#include "cpu/execute_csr.hpp"
#include "cpu/execute_muldiv.hpp"

#include <cstdarg>
#include <cstdio>
#include <string>
#include <string_view>

namespace rvvm::cpu {
namespace {

constexpr std::string_view kRegNames[32] = {
    "zero", "ra", "sp", "gp", "tp",  "t0",  "t1", "t2", "s0", "s1", "a0", "a1",
    "a2",   "a3", "a4", "a5", "a6",  "a7",  "s2", "s3", "s4", "s5", "s6", "s7",
    "s8",   "s9", "s10", "s11", "t3", "t4", "t5", "t6",
};

constexpr std::string_view kFRegNames[32] = {
    "ft0", "ft1", "ft2",  "ft3",  "ft4", "ft5", "ft6",  "ft7",  "fs0", "fs1",
    "fa0", "fa1", "fa2",  "fa3",  "fa4", "fa5", "fa6",  "fa7",  "fs2", "fs3",
    "fs4", "fs5", "fs6",  "fs7",  "fs8", "fs9", "fs10", "fs11", "ft8", "ft9",
    "ft10", "ft11",
};

std::string_view reg(u32 index) noexcept {
    return index < 32 ? kRegNames[index] : std::string_view("x?");
}

std::string_view freg(u32 index) noexcept {
    return index < 32 ? kFRegNames[index] : std::string_view("f?");
}

// Appends a formatted operand list to an instruction mnemonic.
std::string appendf(const char* mnemonic, const char* format, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

std::string appendf(const char* mnemonic, const char* format, ...) {
    char buffer[192];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    std::string out(mnemonic);
    out += buffer;
    return out;
}

u32 bits(u32 value, unsigned hi, unsigned lo) noexcept {
    return (value >> lo) & ((1u << (hi - lo + 1)) - 1u);
}

i64 imm_i(u32 raw) noexcept { return sign_extend(raw >> 20, 12); }
i64 imm_s(u32 raw) noexcept { return sign_extend((bits(raw, 31, 25) << 5) | bits(raw, 11, 7), 12); }
i64 imm_b(u32 raw) noexcept {
    return sign_extend((bits(raw, 31, 31) << 12) | (bits(raw, 7, 7) << 11) | (bits(raw, 30, 25) << 5) |
                           (bits(raw, 11, 8) << 1),
                       13);
}
i64 imm_j(u32 raw) noexcept {
    return sign_extend((bits(raw, 31, 31) << 20) | (bits(raw, 19, 12) << 12) | (bits(raw, 20, 20) << 11) |
                           (bits(raw, 30, 21) << 1),
                       21);
}

// "a0" / "0(a0)" / "-8(a0)" style memory operand; the displacement is always
// printed so that traces are easy to diff.
std::string mem_operand(const Decoded& insn, i64 offset) {
    char buffer[32];
    if (offset < 0) {
        std::snprintf(buffer, sizeof(buffer), "-%lld(%s)", static_cast<long long>(-offset),
                      std::string(reg(insn.rs1)).c_str());
    } else {
        std::snprintf(buffer, sizeof(buffer), "%lld(%s)", static_cast<long long>(offset),
                      std::string(reg(insn.rs1)).c_str());
    }
    return buffer;
}

std::string disassemble_compressed(const Decoded& insn) {
    const u32 raw = insn.raw;
    const u32 quadrant = bits(raw, 1, 0);
    const u32 funct3 = bits(raw, 15, 13);
    const u32 rd_full = bits(raw, 11, 7);
    const u32 rs2_full = bits(raw, 6, 2);
    const std::string_view rd_short = reg(8u + bits(raw, 4, 2));
    const std::string_view rs1_short = reg(8u + bits(raw, 9, 7));

    switch (quadrant) {
    case 0:
        switch (funct3) {
        case 0:
            return appendf("c.addi4spn", " %s, sp, %u", std::string(rd_short).c_str(),
                           bits(raw, 10, 7) << 2 | bits(raw, 12, 11) << 4);
        case 1:
            return appendf("c.fld", " %s, %u(%s)", std::string(freg(8u + bits(raw, 4, 2))).c_str(),
                           bits(raw, 6, 5) << 3 | bits(raw, 12, 10) << 6, std::string(rs1_short).c_str());
        case 2:
            return appendf("c.lw", " %s, %u(%s)", std::string(rd_short).c_str(),
                           bits(raw, 5, 5) << 3 | bits(raw, 12, 10) << 6 | bits(raw, 6, 6) << 2,
                           std::string(rs1_short).c_str());
        case 3:
            return appendf("c.ld", " %s, %u(%s)", std::string(rd_short).c_str(),
                           bits(raw, 6, 5) << 3 | bits(raw, 12, 10) << 6, std::string(rs1_short).c_str());
        case 5:
            return appendf("c.fsd", " %s, %u(%s)", std::string(freg(8u + bits(raw, 4, 2))).c_str(),
                           bits(raw, 6, 5) << 3 | bits(raw, 12, 10) << 6, std::string(rs1_short).c_str());
        case 6:
            return appendf("c.sw", " %s, %u(%s)", std::string(reg(8u + bits(raw, 4, 2))).c_str(),
                           bits(raw, 5, 5) << 3 | bits(raw, 12, 10) << 6 | bits(raw, 6, 6) << 2,
                           std::string(rs1_short).c_str());
        case 7:
            return appendf("c.sd", " %s, %u(%s)", std::string(reg(8u + bits(raw, 4, 2))).c_str(),
                           bits(raw, 6, 5) << 3 | bits(raw, 12, 10) << 6, std::string(rs1_short).c_str());
        default:
            return appendf(".hword", " 0x%04x", bits(raw, 15, 0));
        }
    case 1:
        switch (funct3) {
        case 0:
            return appendf("c.addi", " %s, %lld", std::string(reg(rd_full)).c_str(),
                           static_cast<long long>(
                               sign_extend(bits(raw, 12, 12) << 5 | bits(raw, 6, 2), 6)));
        case 1:
            return appendf("c.addiw", " %s, %lld", std::string(reg(rd_full)).c_str(),
                           static_cast<long long>(
                               sign_extend(bits(raw, 12, 12) << 5 | bits(raw, 6, 2), 6)));
        case 2:
            return appendf("c.li", " %s, %lld", std::string(reg(rd_full)).c_str(),
                           static_cast<long long>(
                               sign_extend(bits(raw, 12, 12) << 5 | bits(raw, 6, 2), 6)));
        case 3:
            if (rd_full == 2) {
                return appendf("c.addi16sp", " sp, %lld",
                               static_cast<long long>(sign_extend(
                                   bits(raw, 12, 12) << 9 | bits(raw, 4, 3) << 7 | bits(raw, 5, 5) << 6 |
                                       bits(raw, 2, 2) << 4 | bits(raw, 6, 6) << 2,
                                   10)));
            }
            return appendf("c.lui", " %s, 0x%llx", std::string(reg(rd_full)).c_str(),
                           static_cast<unsigned long long>(
                               sign_extend(bits(raw, 12, 12) << 5 | bits(raw, 6, 2), 6) << 12));
        case 4: {
            if (bits(raw, 11, 10) == 3) {
                const char* const name = (bits(raw, 12, 12) != 0)
                                             ? (bits(raw, 6, 5) == 0 ? "c.subw" : "c.addw")
                                             : (bits(raw, 6, 5) == 0 ? "c.sub" : "c.xor");
                return appendf(name, " %s, %s", std::string(rd_short).c_str(),
                               std::string(reg(8u + bits(raw, 4, 2))).c_str());
            }
            static const char* const names[3] = {"c.srli", "c.srai", "c.andi"};
            return appendf(names[bits(raw, 11, 10)], " %s, %lld", std::string(rd_short).c_str(),
                           static_cast<long long>(sign_extend(bits(raw, 12, 12) << 5 | bits(raw, 6, 2), 6)));
        }
        case 5:
            return appendf("c.j", " %+lld",
                           static_cast<long long>(sign_extend(
                               bits(raw, 12, 12) << 11 | bits(raw, 11, 11) << 4 | bits(raw, 10, 9) << 8 |
                                   bits(raw, 8, 8) << 10 | bits(raw, 7, 7) << 6 | bits(raw, 6, 6) << 7,
                               12)));
        default:
            return appendf(funct3 == 6 ? "c.beqz" : "c.bnez", " %s, %+lld",
                           std::string(rs1_short).c_str(),
                           static_cast<long long>(sign_extend(
                               bits(raw, 12, 12) << 8 | bits(raw, 6, 5) << 6 | bits(raw, 4, 3) << 1 |
                                   bits(raw, 2, 2) << 5,
                               9)));
        }
    default:
        switch (funct3) {
        case 0:
            return appendf("c.slli", " %s, %lld", std::string(reg(rd_full)).c_str(),
                           static_cast<long long>(
                               sign_extend(bits(raw, 12, 12) << 5 | bits(raw, 6, 2), 6)));
        case 1:
            return appendf("c.fldsp", " %s, %u(sp)", std::string(freg(rd_full)).c_str(),
                           bits(raw, 4, 2) << 3 | bits(raw, 12, 12) << 5 | bits(raw, 6, 5) << 6);
        case 2:
            return appendf("c.lwsp", " %s, %u(sp)", std::string(reg(rd_full)).c_str(),
                           bits(raw, 3, 2) << 2 | bits(raw, 12, 12) << 5 | bits(raw, 6, 4) << 6);
        case 3:
            return appendf("c.ldsp", " %s, %u(sp)", std::string(reg(rd_full)).c_str(),
                           bits(raw, 4, 2) << 3 | bits(raw, 12, 12) << 5 | bits(raw, 6, 5) << 6);
        case 4:
            if (bits(raw, 12, 12) == 0 && rs2_full == 0) {
                return appendf("c.jr", " %s", std::string(reg(rd_full)).c_str());
            }
            if (bits(raw, 12, 12) == 0) {
                return appendf("c.mv", " %s, %s", std::string(reg(rd_full)).c_str(),
                               std::string(reg(rs2_full)).c_str());
            }
            if (rd_full == 0) {
                return "c.ebreak";
            }
            if (rs2_full == 0) {
                return appendf("c.jalr", " %s", std::string(reg(rd_full)).c_str());
            }
            return appendf("c.add", " %s, %s", std::string(reg(rd_full)).c_str(),
                           std::string(reg(rs2_full)).c_str());
        default:
            return appendf(funct3 == 7 ? "c.sdsp" : (funct3 == 5 ? "c.fsdsp" : "c.swsp"),
                           " %s, %u(sp)", std::string(reg(rs2_full)).c_str(),
                           bits(raw, 12, 12) << 5 | bits(raw, 6, 2));
        }
    }
}

} // namespace

std::string disassemble(const Decoded& insn) {
    if (insn.compressed) {
        return disassemble_compressed(insn);
    }

    const u32 raw = insn.raw;
    const u32 funct3 = insn.funct3;
    switch (insn.opcode) {
    case 0x37:
        return appendf("lui", " %s, 0x%llx", std::string(reg(insn.rd)).c_str(),
                       static_cast<unsigned long long>(bits(raw, 31, 12)));
    case 0x17:
        return appendf("auipc", " %s, 0x%llx", std::string(reg(insn.rd)).c_str(),
                       static_cast<unsigned long long>(bits(raw, 31, 12)));
    case 0x6f:
        return appendf("jal", " %s, %+lld", std::string(reg(insn.rd)).c_str(),
                       static_cast<long long>(imm_j(raw)));
    case 0x67:
        return appendf("jalr", " %s, %lld(%s)", std::string(reg(insn.rd)).c_str(),
                       static_cast<long long>(imm_i(raw)), std::string(reg(insn.rs1)).c_str());
    case 0x63: {
        static const char* const names[8] = {"beq", "bne", "?", "?", "blt", "bge", "bltu", "bgeu"};
        return appendf(names[funct3], " %s, %s, %+lld", std::string(reg(insn.rs1)).c_str(),
                       std::string(reg(insn.rs2)).c_str(), static_cast<long long>(imm_b(raw)));
    }
    case 0x03: {
        static const char* const names[8] = {"lb", "lh", "lw", "ld", "lbu", "lhu", "lwu", "?"};
        return appendf(names[funct3], " %s, %s", std::string(reg(insn.rd)).c_str(),
                       mem_operand(insn, imm_i(raw)).c_str());
    }
    case 0x23: {
        static const char* const names[8] = {"sb", "sh", "sw", "sd", "?", "?", "?", "?"};
        return appendf(names[funct3], " %s, %s", std::string(reg(insn.rs2)).c_str(),
                       mem_operand(insn, imm_s(raw)).c_str());
    }
    case 0x13:
        switch (funct3) {
        case 0:
            return appendf("addi", " %s, %s, %lld", std::string(reg(insn.rd)).c_str(),
                           std::string(reg(insn.rs1)).c_str(), static_cast<long long>(imm_i(raw)));
        case 2:
            return appendf("slti", " %s, %s, %lld", std::string(reg(insn.rd)).c_str(),
                           std::string(reg(insn.rs1)).c_str(), static_cast<long long>(imm_i(raw)));
        case 3:
            return appendf("sltiu", " %s, %s, %lld", std::string(reg(insn.rd)).c_str(),
                           std::string(reg(insn.rs1)).c_str(), static_cast<long long>(imm_i(raw)));
        case 4:
            return appendf("xori", " %s, %s, %lld", std::string(reg(insn.rd)).c_str(),
                           std::string(reg(insn.rs1)).c_str(), static_cast<long long>(imm_i(raw)));
        case 6:
            return appendf("ori", " %s, %s, %lld", std::string(reg(insn.rd)).c_str(),
                           std::string(reg(insn.rs1)).c_str(), static_cast<long long>(imm_i(raw)));
        case 7:
            return appendf("andi", " %s, %s, %lld", std::string(reg(insn.rd)).c_str(),
                           std::string(reg(insn.rs1)).c_str(), static_cast<long long>(imm_i(raw)));
        case 1:
            return appendf("slli", " %s, %s, %u", std::string(reg(insn.rd)).c_str(),
                           std::string(reg(insn.rs1)).c_str(), bits(raw, 25, 20));
        default:
            return appendf(bits(raw, 31, 26) == 0x10 ? "srai" : "srli", " %s, %s, %u",
                           std::string(reg(insn.rd)).c_str(), std::string(reg(insn.rs1)).c_str(),
                           bits(raw, 25, 20));
        }
    case 0x1b:
        switch (funct3) {
        case 0:
            return appendf("addiw", " %s, %s, %lld", std::string(reg(insn.rd)).c_str(),
                           std::string(reg(insn.rs1)).c_str(), static_cast<long long>(imm_i(raw)));
        case 1:
            return appendf("slliw", " %s, %s, %u", std::string(reg(insn.rd)).c_str(),
                           std::string(reg(insn.rs1)).c_str(), bits(raw, 24, 20));
        default:
            return appendf(insn.funct7 == 0x20 ? "sraiw" : "srliw", " %s, %s, %u",
                           std::string(reg(insn.rd)).c_str(), std::string(reg(insn.rs1)).c_str(),
                           bits(raw, 24, 20));
        }
    case 0x33: {
        if (insn.funct7 == 0x01) {
            return appendf(muldiv_mnemonic(insn), " %s, %s, %s", std::string(reg(insn.rd)).c_str(),
                           std::string(reg(insn.rs1)).c_str(), std::string(reg(insn.rs2)).c_str());
        }
        const char* name = "?";
        if (insn.funct7 == 0x00) {
            static const char* const names[8] = {"add", "sll", "slt", "sltu", "xor", "srl", "or", "and"};
            name = names[funct3];
        } else if (insn.funct7 == 0x20) {
            name = (funct3 == 0) ? "sub" : (funct3 == 5 ? "sra" : "?");
        }
        return appendf(name, " %s, %s, %s", std::string(reg(insn.rd)).c_str(),
                       std::string(reg(insn.rs1)).c_str(), std::string(reg(insn.rs2)).c_str());
    }
    case 0x3b: {
        if (insn.funct7 == 0x01) {
            return appendf(muldiv_mnemonic(insn), " %s, %s, %s", std::string(reg(insn.rd)).c_str(),
                           std::string(reg(insn.rs1)).c_str(), std::string(reg(insn.rs2)).c_str());
        }
        const char* name = "?";
        if (insn.funct7 == 0x00) {
            static const char* const names[8] = {"addw", "sllw", "?", "?", "?", "srlw", "?", "?"};
            name = names[funct3];
        } else if (insn.funct7 == 0x20) {
            name = (funct3 == 0) ? "subw" : (funct3 == 5 ? "sraw" : "?");
        }
        return appendf(name, " %s, %s, %s", std::string(reg(insn.rd)).c_str(),
                       std::string(reg(insn.rs1)).c_str(), std::string(reg(insn.rs2)).c_str());
    }
    case 0x0f:
        if (funct3 == 1) {
            return "fence.i";
        }
        return appendf("fence", " 0x%llx, 0x%llx", static_cast<unsigned long long>(bits(raw, 23, 20)),
                       static_cast<unsigned long long>(bits(raw, 27, 24)));
    case 0x73:
        if (funct3 == 0) {
            switch (bits(raw, 31, 20)) {
            case 0x000: return "ecall";
            case 0x001: return "ebreak";
            case 0x102: return "sret";
            case 0x105: return "wfi";
            case 0x120: return "sfence.vma";
            case 0x302: return "mret";
            default: break;
            }
            return appendf("system", " 0x%llx", static_cast<unsigned long long>(bits(raw, 31, 20)));
        }
        {
            // Zicsr. The *I forms take a zero extended 5 bit immediate that
            // lives in the rs1 field.
            const u32 csr_address = bits(raw, 31, 20);
            const char* const name = csr_name(csr_address);
            const bool immediate = funct3 >= 5;
            if (immediate) {
                return appendf(csr_mnemonic(insn), " %s, %s, %u", std::string(reg(insn.rd)).c_str(), name,
                               bits(raw, 19, 15));
            }
            return appendf(csr_mnemonic(insn), " %s, %s, %s", std::string(reg(insn.rd)).c_str(), name,
                           std::string(reg(insn.rs1)).c_str());
        }
    case 0x2f: {
        // Atomics: the mnemonic comes from the A extension table, then aq/rl.
        // funct7[1] = aq, funct7[0] = rl, funct5 = funct7 >> 2.
        const char* const base = atomic_mnemonic(insn);
        const u32 funct5 = insn.funct7 >> 2;
        const bool aq = (insn.funct7 & 0x2) != 0;
        const bool rl = (insn.funct7 & 0x1) != 0;
        char mnemonic[32];
        if (base[0] == '?') {
            std::snprintf(mnemonic, sizeof(mnemonic), "amo.?");
        } else if (aq && rl) {
            std::snprintf(mnemonic, sizeof(mnemonic), "%s.aqrl", base);
        } else {
            std::snprintf(mnemonic, sizeof(mnemonic), "%s%s%s", base, aq ? ".aq" : "", rl ? ".rl" : "");
        }
        if (funct5 == 0x02) {
            // LR carries no rs2 operand.
            return appendf(mnemonic, " %s, (%s)", std::string(reg(insn.rd)).c_str(),
                           std::string(reg(insn.rs1)).c_str());
        }
        return appendf(mnemonic, " %s, %s, (%s)", std::string(reg(insn.rd)).c_str(),
                       std::string(reg(insn.rs2)).c_str(), std::string(reg(insn.rs1)).c_str());
    }
    case 0x07:
    case 0x27:
    case 0x43:
    case 0x47:
    case 0x4b:
    case 0x4f:
    case 0x53:
        return appendf("fp", " 0x%llx", static_cast<unsigned long long>(insn.funct7));
    default:
        break;
    }
    return appendf(".word", " 0x%08llx", static_cast<unsigned long long>(raw));
}

std::string disassemble(u32 raw) { return disassemble(decode(raw)); }

} // namespace rvvm::cpu