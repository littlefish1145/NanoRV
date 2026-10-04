// SPDX-License-Identifier: MIT
#include "machine/fdt.hpp"
#include <initializer_list>
#include <random>
namespace nanorv::machine {
namespace {
void word(std::vector<u8> &v, u32 n) {
  for (int i = 3; i >= 0; --i)
    v.push_back(static_cast<u8>(n >> (i * 8)));
}
void align(std::vector<u8> &v) {
  while (v.size() % 4)
    v.push_back(0);
}
struct Fdt {
  std::vector<u8> tree, strings;
  void begin(const std::string &name) {
    word(tree, 1);
    tree.insert(tree.end(), name.begin(), name.end());
    tree.push_back(0);
    align(tree);
  }
  void end() { word(tree, 2); }
  void prop(const std::string &name, const std::vector<u8> &data) {
    word(tree, 3);
    word(tree, static_cast<u32>(data.size()));
    word(tree, static_cast<u32>(strings.size()));
    strings.insert(strings.end(), name.begin(), name.end());
    strings.push_back(0);
    tree.insert(tree.end(), data.begin(), data.end());
    align(tree);
  }
  void cells(const std::string &name, std::initializer_list<u32> values) {
    std::vector<u8> data;
    for (auto v : values)
      word(data, v);
    prop(name, data);
  }
  void list(const std::string &name,const std::vector<u32>& values) {
    std::vector<u8> data;for(auto value:values)word(data,value);prop(name,data);
  }
  void str(const std::string &name, const std::string &value) {
    std::vector<u8> data(value.begin(), value.end());
    data.push_back(0);
    prop(name, data);
  }
  void reg(u64 base, u64 size) {
    cells("reg", {static_cast<u32>(base >> 32), static_cast<u32>(base),
                  static_cast<u32>(size >> 32), static_cast<u32>(size)});
  }
};
} // namespace
std::vector<u8> make_fdt(u64 ram_size, u64 start, u64 size,
                         const std::string &args, bool disk, bool net, u32 harts,bool graphics) {
  Fdt f;
  f.begin("");
  f.cells("#address-cells", {2});
  f.cells("#size-cells", {2});
  f.str("compatible", "riscv-virtio");
  f.str("model", "NanoRV virt");
  f.begin("chosen");
  f.str("stdout-path", "/soc/serial@10000000");
  f.str("bootargs", args);
  // Trusted host entropy prevents early userspace getrandom() stalls.
  std::random_device random;
  std::vector<u8> seed(64);
  for (auto& byte : seed) byte = static_cast<u8>(random());
  f.prop("rng-seed", seed);
  if (size) {
    f.cells("linux,initrd-start",
            {static_cast<u32>(start >> 32), static_cast<u32>(start)});
    f.cells("linux,initrd-end", {static_cast<u32>((start + size) >> 32),
                                 static_cast<u32>(start + size)});
  }
  f.end();
  f.begin("memory@80000000");
  f.str("device_type", "memory");
  f.reg(0x80000000, ram_size);
  f.end();
  f.begin("cpus");
  f.cells("#address-cells", {1});
  f.cells("#size-cells", {0});
  f.cells("timebase-frequency", {10000000});
  for(u32 n=0;n<harts;++n) {
  f.begin("cpu@"+std::to_string(n));
  f.str("device_type", "cpu");
  f.cells("reg", {n});
  f.str("status", "okay");
  f.str("compatible", "riscv");
  f.str("riscv,isa", "rv64imafdc_zicsr_zifencei_zba_zbb_zbc_zbs_zbkb_zbkc_zkne_zknd_zknh");
  f.str("mmu-type", "riscv,sv48");
  f.begin("interrupt-controller");
  f.cells("#interrupt-cells", {1});
  f.prop("interrupt-controller", {});
  f.str("compatible", "riscv,cpu-intc");
  f.cells("phandle", {0x100+n});
  f.end();
  f.end();
  }
  f.end();
  f.begin("soc");
  f.cells("#address-cells", {2});
  f.cells("#size-cells", {2});
  f.str("compatible", "simple-bus");
  f.prop("ranges", {});
  f.begin("clint@2000000");
  f.str("compatible", std::string("sifive,clint0\0riscv,clint0",
                                  sizeof("sifive,clint0\0riscv,clint0") - 1));
  f.reg(0x2000000, 0x10000);
  std::vector<u32> timer_irqs,external_irqs;
  for(u32 n=0;n<harts;++n) {
    timer_irqs.insert(timer_irqs.end(),{0x100+n,3,0x100+n,7});
    external_irqs.insert(external_irqs.end(),{0x100+n,11,0x100+n,9});
  }
  f.list("interrupts-extended", timer_irqs);
  f.end();
  f.begin("plic@c000000");
  f.str("compatible", "riscv,plic0");
  f.reg(0xc000000, 0x600000);
  f.cells("#interrupt-cells", {1});
  f.prop("interrupt-controller", {});
  f.list("interrupts-extended", external_irqs);
  f.cells("riscv,ndev", {95});
  f.cells("phandle", {2});
  f.end();
  f.begin("serial@10000000");
  f.str("compatible", "ns16550a");
  f.reg(0x10000000, 0x100);
  f.cells("clock-frequency", {3686400});
  f.cells("current-speed", {115200});
  f.cells("interrupt-parent", {2});
  f.cells("interrupts", {10});
  f.end();
  const char* virtio_nodes[]={"virtio_mmio@10001000","virtio_mmio@10002000","virtio_mmio@10003000","virtio_mmio@10004000"};
  for (unsigned n = 0; n < 4; ++n) {
    if (!(n == 0 ? disk : n==1?net:graphics)) continue;
    f.begin(virtio_nodes[n]);
    f.str("compatible", "virtio,mmio");
    f.reg(0x10001000 + n * 0x1000, 0x1000);
    f.cells("interrupt-parent", {2});
    f.cells("interrupts", {n + 1});
    f.end();
  }
  f.begin("rtc@101000");
  f.str("compatible", "google,goldfish-rtc");
  f.reg(0x101000, 0x1000);
  f.cells("interrupt-parent", {2});
  f.cells("interrupts", {11});
  f.end();
  f.begin("test@100000");
  f.str("compatible",
        std::string("sifive,test1\0sifive,test0\0syscon",
                    sizeof("sifive,test1\0sifive,test0\0syscon") - 1));
  f.reg(0x100000, 0x1000);
  f.cells("phandle", {3});
  f.end();
  f.end();
  f.begin("reboot");
  f.str("compatible", "syscon-reboot");
  f.cells("regmap", {3});
  f.cells("offset", {0});
  f.cells("value", {0x7777});
  f.end();
  f.begin("poweroff");
  f.str("compatible", "syscon-poweroff");
  f.cells("regmap", {3});
  f.cells("offset", {0});
  f.cells("value", {0x5555});
  f.end();
  f.end();
  word(f.tree, 9);
  std::vector<u8> out;
  const u32 ts = static_cast<u32>(f.tree.size()),
            ss = static_cast<u32>(f.strings.size());
  for (u32 n :
       {0xd00dfeedu, 56 + ts + ss, 56u, 56 + ts, 40u, 17u, 16u, 0u, ss, ts})
    word(out, n);
  for (int n = 0; n < 4; ++n)
    word(out, 0);
  out.insert(out.end(), f.tree.begin(), f.tree.end());
  out.insert(out.end(), f.strings.begin(), f.strings.end());
  return out;
}
} // namespace nanorv::machine
