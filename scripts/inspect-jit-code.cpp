// SPDX-License-Identifier: MIT
// Write non-executable COFF snapshots for llvm-objdump, plus body boundaries.
#include "cpu/jit_x64.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
using namespace rvvm;
namespace {
u32 op_i(i32 imm,u32 a,u32 f,u32 dst) {return ((static_cast<u32>(imm)&4095)<<20)|(a<<15)|(f<<12)|(dst<<7)|0x13;}
u32 op_r(u32 a,u32 b,u32 f,u32 dst) {return (b<<20)|(a<<15)|(f<<12)|(dst<<7)|0x33;}
u32 branch(i32 offset,u32 a) {
  const auto bits=static_cast<u32>(offset)&8191;
  return ((bits>>12)<<31)|(((bits>>5)&63)<<25)|(a<<15)|(1u<<12)|
    (((bits>>1)&15)<<8)|(((bits>>11)&1)<<7)|0x63;
}
void number(std::ostream& out,u32 value,unsigned bytes) {
  for(unsigned n=0;n<bytes;++n)out.put(static_cast<char>(value>>(n*8)));
}
void snapshot(const std::filesystem::path& path,std::span<const u8> code) {
  std::ofstream out(path,std::ios::binary);
  number(out,0x8664,2);number(out,1,2);number(out,0,4);number(out,0,4);number(out,0,4);
  number(out,0,2);number(out,0,2);
  out.write(".text\0\0\0",8);number(out,0,4);number(out,0,4);
  number(out,static_cast<u32>(code.size()),4);number(out,60,4);
  number(out,0,4);number(out,0,4);number(out,0,2);number(out,0,2);number(out,0x60500020,4);
  out.write(reinterpret_cast<const char*>(code.data()),static_cast<std::streamsize>(code.size()));
  if(!out)throw std::runtime_error("cannot write COFF snapshot");
}
}
int main(int argc,char** argv) {
  try {
    if(argc!=2) {std::cerr<<"Usage: rvvm_jit_inspect OUTPUT_DIRECTORY\n";return 1;}
    const std::filesystem::path directory=argv[1];std::filesystem::create_directories(directory);
    for(const std::string name:{"three-operand-add","three-operand-bitwise","memory","store","self-loop-fusible","register-pressure"}) {
      std::vector<cpu::CachedInstruction> instructions;
      if(name=="store") {
        for(unsigned n=0;n<16;++n) {
          const u32 first=(2u<<20)|(1u<<15)|(2u<<12)|0x23;
          const u32 second=op_i(8,1,0,1);
          for(const auto raw:{first,second})instructions.push_back({cpu::decode(raw),raw});
        }
      } else if(name=="self-loop-fusible") {
        for(unsigned n=0;n<30;++n) {const auto raw=op_i(1,1,0,1);instructions.push_back({cpu::decode(raw),raw});}
        for(const auto raw:{op_i(-1,30,0,30),branch(-124,30)})instructions.push_back({cpu::decode(raw),raw});
      } else if(name=="register-pressure") {
        for(unsigned n=0;n<32;++n) {
          const auto raw=op_r(n%6+1,(n+1)%6+1,n&1?0:4,(n+2)%6+1);
          instructions.push_back({cpu::decode(raw),raw});
        }
      } else for(unsigned n=0;n<16;++n) {
        const u32 first=name=="three-operand-add"?op_r(1,2,0,3):
          name=="three-operand-bitwise"?op_r(1,2,4,3):(1u<<15)|(3u<<12)|(3u<<7)|3;
        const u32 second=op_i(17,3,0,1);
        for(const auto raw:{first,second})instructions.push_back({cpu::decode(raw),raw});
      }
      auto native=cpu::NativeBlock::compile(instructions);
      if(!native)throw std::runtime_error("native compilation failed");
      snapshot(directory/(name+".obj"),native->code_bytes());
      std::cout<<"{\"case\":\""<<name<<"\",\"guest_instructions\":"<<instructions.size()
        <<",\"code_bytes\":"<<native->code_bytes().size()<<",\"body_offset\":"<<native->body_offset()
        <<",\"body_size\":"<<native->body_size()<<"}\n";
    }
    return 0;
  } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
