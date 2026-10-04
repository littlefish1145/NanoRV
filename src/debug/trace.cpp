// SPDX-License-Identifier: MIT
#include "debug/trace.hpp"

#include "core/log.hpp"
#include "cpu/disassembler.hpp"

namespace nanorv::debug {

void trace_instruction(const cpu::Hart& hart, const cpu::Decoded& insn) {
    const u32 raw = insn.raw;
    if (insn.length == 2) {
        NanoRV_INFO("0x%08llx: %04x        %s", static_cast<unsigned long long>(hart.pc),
                  static_cast<unsigned>(raw & 0xffffu), cpu::disassemble(insn).c_str());
    } else {
        NanoRV_INFO("0x%08llx: %08x    %s", static_cast<unsigned long long>(hart.pc),
                  static_cast<unsigned>(raw), cpu::disassemble(insn).c_str());
    }
}

} // namespace nanorv::debug