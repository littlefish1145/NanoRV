// SPDX-License-Identifier: MIT
#include "devices/virtio.hpp"
#include "platform/platform.hpp"
#include <fstream>
#include "devices/virt_devices.hpp"
#include "test_framework.hpp"
#include "test_helpers.hpp"
#include <filesystem>
using namespace rvvm;
using namespace rvvm::test;
namespace {
struct DiskFixture : CpuFixture {
  std::filesystem::path path = std::filesystem::temp_directory_path() /
      ("rvvm-disk-test-" + std::to_string(platform::monotonic_ns()) + ".raw");
  std::unique_ptr<devices::VirtioBlock> device;
  static constexpr u64 desc = kDramBase + 0x1000, avail = kDramBase + 0x2000,
      used = kDramBase + 0x3000, header = kDramBase + 0x4000,
      buffer = kDramBase + 0x5000, status = kDramBase + 0x6000;
  DiskFixture() {
    { std::ofstream file(path, std::ios::binary); std::array<char,4096> zero{}; file.write(zero.data(), zero.size()); }
    device = std::make_unique<devices::VirtioBlock>(space);
    std::string error; CHECK(device->open(path.string(), error));
    reg(0x24, 1); reg(0x20, 1); reg(0x70, 15);
    reg(0x38, 8);
    reg(0x80, desc); reg(0x90, avail); reg(0xa0, used); reg(0x44, 1);
  }
  ~DiskFixture() { device.reset(); std::filesystem::remove(path); }
  void reg(u64 offset, u64 value) { CHECK(device->store(offset,32,value)); }
  u64 read(u64 offset, unsigned width=32) { u64 v=0; CHECK(device->load(offset,width,v)); return v; }
  void descriptor(unsigned index, u64 address, u32 length, u16 flags, u16 next) {
    devices::VirtioMmio::Descriptor d{address,length,flags,next};
    CHECK(space.write_bytes(desc+16*index,{reinterpret_cast<const u8*>(&d),sizeof(d)}));
  }
  void request(u32 type, u64 sector, u16 index, bool writable) {
    space.store(header,32,type); space.store(header+8,64,sector);
    descriptor(0,header,16,1,1);
    descriptor(1,buffer,512,static_cast<u16>(1 | (writable?2:0)),2);
    descriptor(2,status,1,2,0);
    space.store(status,8,255); space.store(avail+4+((index-1)%8)*2,16,0);
    space.store(avail+2,16,index); reg(0x50,0);
  }
};
}
TEST(virtio, raw_disk_write_read_flush_and_irq_ack) {
  DiskFixture c;
  CHECK_EQ(c.read(0),0x74726976ull); CHECK_EQ(c.read(4),2ull);
  CHECK_EQ(c.read(0x100,64),8ull);
  std::array<u8,512> data{};
  for (unsigned n=0;n<data.size();++n) data[n]=static_cast<u8>(n);
  c.space.write_bytes(c.buffer,data);
  c.request(1,2,1,false);
  CHECK_EQ(c.space.load_u8(c.status),0); CHECK_EQ(c.space.load_u16(c.used+2),1);
  CHECK(c.device->interrupt()); c.reg(0x64,1); CHECK(!c.device->interrupt());
  c.space.fill_zero(c.buffer,512);
  c.request(0,2,2,true);
  std::array<u8,512> actual{}; c.space.read_bytes(c.buffer,actual);
  CHECK(data == actual); CHECK_EQ(c.space.load_u32(c.used+16),513);
  c.request(4,0,3,false); CHECK_EQ(c.space.load_u8(c.status),0);
  c.request(0,8,4,true); CHECK_EQ(c.space.load_u8(c.status),1);
  c.reg(0x70,0); CHECK_EQ(c.read(0x44),0ull); CHECK(!c.device->interrupt());
}
TEST(virtio, cyclic_descriptor_chain_requires_reset_without_dma) {
  DiskFixture c;
  c.descriptor(0,c.header,16,1,0);
  c.space.store(c.avail+4,16,0); c.space.store(c.avail+2,16,1); c.reg(0x50,0);
  CHECK((c.read(0x70)&64)!=0); CHECK(c.device->interrupt());
  CHECK_EQ(c.space.load_u16(c.used+2),0);
}
TEST(devices, plic_multiple_sources_priority_claim_and_reassert) {
  devices::Plic p; cpu::Hart h; u64 v;
  CHECK(p.store(4,32,3)); CHECK(p.store(8,32,7)); CHECK(p.store(0x2080,32,6));
  p.set_irq(1,true); p.set_irq(2,true); p.update(h,false);
  CHECK(h.csr.raw(cpu::kCsrMip)&0x200);
  CHECK(p.load(0x201004,32,v)); CHECK_EQ(v,2ull);
  CHECK(p.load(0x201004,32,v)); CHECK_EQ(v,1ull);
  p.set_irq(2,false); CHECK(p.store(0x201004,32,2));
  CHECK(p.load(0x201004,32,v)); CHECK_EQ(v,0ull);
  CHECK(p.store(0x201004,32,1));
  CHECK(p.load(0x201004,32,v)); CHECK_EQ(v,1ull);
}
TEST(virtio, native_nat_arp_reply_traverses_tx_rx_queues_and_irq) {
  CpuFixture c;devices::VirtioNet net(c.space);std::string error;
  CHECK(net.open(error));
  auto reg=[&](u64 o,u64 v){CHECK(net.store(o,32,v));};
  reg(0x24,1);reg(0x20,1);reg(0x24,0);reg(0x20,3);reg(0x70,15);
  const u64 rx=kDramBase+0x4000,tx=kDramBase+0xa000;
  for(unsigned queue=0;queue<2;++queue) {
    const u64 base=kDramBase+0x1000+queue*0x6000;
    reg(0x30,queue);reg(0x38,8);reg(0x80,base);reg(0x90,base+0x1000);reg(0xa0,base+0x2000);
    const devices::VirtioMmio::Descriptor descriptor{queue?tx:rx,queue?54u:1536u,static_cast<u16>(queue?0:2),0};
    CHECK(c.space.write_bytes(base,{reinterpret_cast<const u8*>(&descriptor),sizeof(descriptor)}));
    c.space.store(base+0x1004,16,0);c.space.store(base+0x1002,16,1);reg(0x44,1);
  }
  std::array<u8,54> packet{};
  const u8 arp[]={255,255,255,255,255,255,0x52,0x54,0,0x12,0x34,0x56,8,6,
      0,1,8,0,6,4,0,1,0x52,0x54,0,0x12,0x34,0x56,10,0,2,15,0,0,0,0,0,0,10,0,2,2};
  std::copy(std::begin(arp),std::end(arp),packet.begin()+12);
  c.space.write_bytes(tx,packet);reg(0x50,1);
  for(unsigned n=0;n<1000 && c.space.load_u16(kDramBase+0x3002)==0;++n) {net.poll();platform::sleep_ms(1);}
  CHECK_EQ(c.space.load_u16(kDramBase+0x3002),1);
  CHECK_EQ(c.space.load_u16(kDramBase+0x9002),1);
  CHECK(c.space.load_u32(kDramBase+0x3008)>=54u); // Ethernet padding is allowed
  CHECK(c.space.load_u32(kDramBase+0x3008)<=1536u);
  CHECK_EQ(c.space.load_u8(rx),2); // checksum validated by the backend
  CHECK_EQ(c.space.load_u8(rx+12+21),2); // ARP reply opcode
  CHECK_EQ(c.space.load_u8(rx+12),0x52);CHECK_EQ(c.space.load_u8(rx+13),0x54);
  CHECK(net.interrupt());reg(0x64,1);CHECK(!net.interrupt());
}
TEST(devices, uart_divisor_latches_do_not_alias_scratch_or_interrupt_enable) {
  devices::Uart uart;u64 value;
  CHECK(uart.store(7,8,0x53));CHECK(uart.store(1,8,1));
  CHECK(uart.store(3,8,0x80));CHECK(uart.store(0,8,0x12));CHECK(uart.store(1,8,0x34));
  CHECK(uart.load(0,8,value));CHECK_EQ(value,0x12ull);
  CHECK(uart.load(1,8,value));CHECK_EQ(value,0x34ull);
  CHECK(uart.store(3,8,3));
  CHECK(uart.load(7,8,value));CHECK_EQ(value,0x53ull);
  CHECK(uart.load(1,8,value));CHECK_EQ(value,1ull);
}
