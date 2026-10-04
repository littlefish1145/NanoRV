// SPDX-License-Identifier: MIT
#pragma once
#include "cpu/jit_x64.hpp"
#include <vector>
namespace nanorv::cpu {
enum class IROp : u8 {Nop,Const,Move,Add,Sub,And,Or,Xor,Shl,Shr,Sar,Mul,Div,Rem,RotateLeft,RotateRight,AndNot,OrNot,Xnor,ByteSwap,SignByte,SignHalf,ZeroHalf,Compare,Select,PCRelative,Load,Store,FloatLoad,FloatStore,Branch,BranchCond,CallHelper,Exit};
struct IRInstruction {
  IROp op=IROp::CallHelper;
  u32 dst=0,a=0,b=0,c=0;
  u64 immediate=0;
  bool immediate_rhs=false,word=false,unsigned_compare=false,indirect=false;
  u8 condition=0;
  const CachedInstruction* source=nullptr;
};
std::vector<IRInstruction> lower_ir(std::span<const CachedInstruction>);
void optimize_ir(std::span<IRInstruction>);
bool pure_ir(IROp) noexcept;
}
