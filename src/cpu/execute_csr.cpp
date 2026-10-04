// SPDX-License-Identifier: MIT
#include "cpu/execute_csr.hpp"
#include "cpu/host_fp.hpp"

#include "cpu/csr.hpp"
#include "cpu/exec_util.hpp"

namespace nanorv::cpu {
namespace {

enum class CsrOp : u8 {
    ReadWrite = 1, // CSRRW  / CSRRWI
    ReadSet = 2,   // CSRRS  / CSRRSI
    ReadClear = 3, // CSRRC  / CSRRCI
};

bool immediate_form(u32 funct3) noexcept {
    return funct3 >= 5;
}

const char* mnemonic_for(u32 funct3) noexcept {
    switch (static_cast<CsrOp>(funct3)) {
    case CsrOp::ReadWrite: return "csrrw";
    case CsrOp::ReadSet: return "csrrs";
    case CsrOp::ReadClear: return "csrrc";
    }
    return "csr?";
}

} // namespace

bool is_csrinstr(const Decoded& insn) noexcept {
    return !insn.compressed && insn.opcode == 0x73 && insn.funct3 != 0;
}

bool execute_csr(Hart& hart, const Decoded& insn, Trap& trap) noexcept {
    if (!is_csrinstr(insn)) {
        return detail::illegal_insn(insn, trap);
    }

    // The immediate forms share the operation with their register counterparts:
    // funct3 = 0b101/0b110/0b111 behave like 0b001/0b010/0b011.
    const CsrOp op = static_cast<CsrOp>(insn.funct3 & 0x3u);
    if (insn.funct3 != 1 && insn.funct3 != 2 && insn.funct3 != 3 && insn.funct3 != 5 &&
        insn.funct3 != 6 && insn.funct3 != 7) {
        // funct3 == 0b100 is reserved; funct3 == 0 is ECALL/EBREAK and never
        // reaches this group.
        return detail::illegal_insn(insn, trap);
    }

    const u32 csr_address = detail::bits(insn.raw, 31, 20);
    if(csr_address>=1 && csr_address<=3 && !(hart.csr.raw(kCsrMstatus)&(3ull<<13)))return detail::illegal_insn(insn,trap);
    if((csr_address==kCsrSatp) && hart.privilege==Privilege::Supervisor && (hart.csr.raw(kCsrMstatus)&(1ull<<20)))return detail::illegal_insn(insn,trap);
    const bool immediate = immediate_form(insn.funct3);
    // The immediate is zero extended from 5 bits and always lives in the rs1
    // field, even when it encodes register x0.
    const u64 source = immediate ? detail::bits(insn.raw, 19, 15) : hart.get_reg(insn.rs1);

    // Side effect rules from the specification:
    //   * CSRRW/CSRRWI must not read the CSR when rd is x0, so that reading a
    //     CSR with side effects can be done as a write of x0;
    //   * CSRRS/CSRRC (and the immediate forms) must not write when the source
    //     operand is zero, so that they can be used to read without writing.
    const bool performs_write = (op == CsrOp::ReadWrite) || (insn.rs1 != 0);
    const bool needs_old_for_rd = (insn.rd != kRegZero);
    const bool needs_old_for_rmw = performs_write && (op != CsrOp::ReadWrite);

    u64 old_value = 0;
    if (needs_old_for_rd || needs_old_for_rmw || op != CsrOp::ReadWrite) {
        // One read serves both rd and the read-modify-write computation.
        if (!hart.csr.read(csr_address, hart.privilege, hart.retired, old_value)) {
            return detail::illegal_insn(insn, trap);
        }
    }

    if (performs_write) {
        const u64 new_value =
            (op == CsrOp::ReadWrite) ? source
                                     : ((op == CsrOp::ReadSet) ? (old_value | source) : (old_value & ~source));
        if (!hart.csr.write(csr_address, hart.privilege, new_value, hart.retired+1)) {
            return detail::illegal_insn(insn, trap);
        }
        // Guest writes may clear sticky exception flags inside an externally
        // supplied native instruction span. Mirror that boundary in MXCSR.
        if(csr_address==kCsrFflags || csr_address==kCsrFcsr)host_fp_clear_exceptions();
        if(csr_address<=3)hart.csr.set_raw(kCsrMstatus,hart.csr.raw(kCsrMstatus)|(3ull<<13)|(1ull<<63));
        if(csr_address==kCsrSatp || (csr_address>=0x3a0 && csr_address<=0x3bf))++hart.translation_epoch;
    }

    hart.set_reg(insn.rd, old_value);
    return true;
}

const char* csr_mnemonic(const Decoded& insn) noexcept {
    if (insn.funct3 == 0 || insn.funct3 == 4) {
        return "csr?";
    }
    const char* const base = mnemonic_for(insn.funct3 & 0x3u);
    if (!immediate_form(insn.funct3)) {
        return base;
    }
    switch (insn.funct3) {
    case 5: return "csrrwi";
    case 6: return "csrrsi";
    case 7: return "csrrci";
    default: break;
    }
    return base;
}

} // namespace nanorv::cpu
