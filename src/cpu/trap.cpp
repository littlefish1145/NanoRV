// SPDX-License-Identifier: MIT
#include "cpu/trap.hpp"

namespace nanorv::cpu {

const char* to_string(TrapCause cause) noexcept {
    switch (cause) {
    case TrapCause::InstructionAddressMisaligned: return "instruction address misaligned";
    case TrapCause::InstructionAccessFault:       return "instruction access fault";
    case TrapCause::IllegalInstruction:           return "illegal instruction";
    case TrapCause::Breakpoint:                   return "breakpoint";
    case TrapCause::LoadAddressMisaligned:        return "load address misaligned";
    case TrapCause::LoadAccessFault:              return "load access fault";
    case TrapCause::StoreAddressMisaligned:       return "store address misaligned";
    case TrapCause::StoreAccessFault:             return "store access fault";
    case TrapCause::EnvironmentCallUser:          return "environment call from U";
    case TrapCause::EnvironmentCallSupervisor:     return "environment call from S";
    case TrapCause::EnvironmentCallMachine:        return "environment call from M";
    case TrapCause::InstructionPageFault:         return "instruction page fault";
    case TrapCause::LoadPageFault:                return "load page fault";
    case TrapCause::StorePageFault:               return "store page fault";
    }
    return "unknown cause";
}

} // namespace nanorv::cpu