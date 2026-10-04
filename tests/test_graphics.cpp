// SPDX-License-Identifier: MIT
#include "devices/virtio_graphics.hpp"
#include "platform/platform.hpp"
#include "test_framework.hpp"
#include "test_helpers.hpp"
#include <cstring>
#include <algorithm>
#include <thread>
using namespace nanorv;using namespace nanorv::test;
#include <cstdlib>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
namespace {
void put(std::vector<u8>& b,unsigned n,u32 v) {std::memcpy(b.data()+n,&v,4);}
struct GpuFixture:CpuFixture {
  platform::Display display;devices::VirtioGpu gpu{space,display};u16 index=0;
  GpuFixture() {
    reg(0x24,1);reg(0x20,1);reg(0x70,15);reg(0x38,8);
    reg(0x80,kDramBase+0x1000);reg(0x90,kDramBase+0x2000);reg(0xa0,kDramBase+0x3000);reg(0x44,1);
  }
  void reg(u64 o,u64 v) {CHECK(gpu.store(o,32,v));}
  u32 submit(std::vector<u8> bytes) {
    CHECK(space.write_bytes(kDramBase+0x6000,bytes));
    const devices::VirtioMmio::Descriptor in{kDramBase+0x6000,static_cast<u32>(bytes.size()),1,1},out{kDramBase+0x8000,512,2,0};
    CHECK(space.write_bytes(kDramBase+0x1000,{reinterpret_cast<const u8*>(&in),16}));CHECK(space.write_bytes(kDramBase+0x1010,{reinterpret_cast<const u8*>(&out),16}));
    CHECK(space.store(kDramBase+0x2004+(index%8)*2,16,0));CHECK(space.store(kDramBase+0x2002,16,++index));reg(0x50,0);
    CHECK_EQ(space.load_u16(kDramBase+0x3002),index);return space.load_u32(kDramBase+0x8000);
  }
  std::vector<u8> command(u32 type,unsigned size=32,u32 id=1) {std::vector<u8> bytes(size);put(bytes,0,type);if(size>=28)put(bytes,24,id);return bytes;}
  void resource() {auto b=command(0x101,40);put(b,28,2);put(b,32,2);put(b,36,2);CHECK_EQ(submit(b),0x1100u);}
};
}
TEST(graphics, gpu_display_dma_rectangles_fences_and_framebuffer) {
  GpuFixture c;auto b=c.command(0x100,24);put(b,4,1);put(b,8,123);CHECK_EQ(c.submit(b),0x1101u);CHECK_EQ(c.space.load_u32(kDramBase+0x8008),123u);
  c.resource();b=c.command(0x106,48);put(b,28,1);put(b,32,static_cast<u32>(kDramBase+0x5000));put(b,40,16);CHECK_EQ(c.submit(b),0x1100u);
  const std::array<u32,4> colors{0xff0000,0xff00,0xff,0xffffff};CHECK(c.space.write_bytes(kDramBase+0x5000,{reinterpret_cast<const u8*>(colors.data()),16}));
  b=c.command(0x105,56);put(b,24,0);put(b,32,2);put(b,36,2);put(b,48,1);CHECK_EQ(c.submit(b),0x1100u);
  b=c.command(0x103,48);put(b,24,0);put(b,32,2);put(b,36,2);put(b,44,1);CHECK_EQ(c.submit(b),0x1100u);CHECK_EQ(c.gpu.frames(),1ull);
  std::string error;CHECK(c.display.save("build/gpu-unit.bmp",error));auto saved=platform::read_file("build/gpu-unit.bmp",&error);CHECK(saved.has_value());CHECK_EQ(saved->size(),70u);
  u32 pixel;std::memcpy(&pixel,saved->data()+54,4);CHECK_EQ(pixel,colors[0]);std::memcpy(&pixel,saved->data()+66,4);CHECK_EQ(pixel,colors[3]);
}
TEST(graphics, gpu_rejects_invalid_backing_bounds_and_resource_exhaustion) {
  GpuFixture c;c.resource();auto b=c.command(0x106,48);put(b,28,1);put(b,32,0x10000000);put(b,40,16);CHECK_EQ(c.submit(b),0x1205u);
  b=c.command(0x105,56);put(b,24,0);put(b,32,2);put(b,36,2);put(b,48,1);CHECK_EQ(c.submit(b),0x1205u);
  b=c.command(0x103,48);put(b,24,0);put(b,32,3);put(b,36,2);put(b,44,1);CHECK_EQ(c.submit(b),0x1205u);
  b=c.command(0x101,40,2);put(b,28,2);put(b,32,0xffffffff);put(b,36,2);CHECK_EQ(c.submit(b),0x1205u);
  b=c.command(0x102,32,55);CHECK_EQ(c.submit(b),0x1203u);
  c.reg(0x70,0);c.reg(0x24,1);c.reg(0x20,1);c.reg(0x70,15);c.reg(0x38,8);c.reg(0x80,kDramBase+0x1000);c.reg(0x90,kDramBase+0x2000);c.reg(0xa0,kDramBase+0x3000);
  c.space.store(kDramBase+0x2002,16,0);c.index=0;c.reg(0x44,1);CHECK_EQ(c.submit(c.command(0x102)),0x1203u);
}
TEST(graphics, input_capabilities_and_key_events_use_standard_evdev_layout) {
  CpuFixture c;devices::VirtioInput input(c.space);u64 value=0;
  CHECK(input.store(0x100,8,0x11));CHECK(input.store(0x101,8,1));CHECK(input.load(0x102,8,value));CHECK_EQ(value,35ull);
  CHECK(input.load(0x10b,8,value));CHECK(value&(1u<<4)); // KEY_ENTER=28
  CHECK(input.store(0x24,32,1));CHECK(input.store(0x20,32,1));CHECK(input.store(0x70,32,15));
  CHECK(input.store(0x38,32,8));CHECK(input.store(0x80,32,kDramBase+0x1000));CHECK(input.store(0x90,32,kDramBase+0x2000));CHECK(input.store(0xa0,32,kDramBase+0x3000));
  const devices::VirtioMmio::Descriptor desc{kDramBase+0x4000,8,2,0};CHECK(c.space.write_bytes(kDramBase+0x1000,{reinterpret_cast<const u8*>(&desc),16}));
  CHECK(c.space.store(kDramBase+0x2004,16,0));CHECK(c.space.store(kDramBase+0x2002,16,1));CHECK(input.store(0x44,32,1));
  const platform::InputEvent event{1,28,1};input.push({&event,1});CHECK_EQ(c.space.load_u16(kDramBase+0x3002),1);CHECK_EQ(c.space.load_u16(kDramBase+0x4000),1);CHECK_EQ(c.space.load_u16(kDramBase+0x4002),28);CHECK_EQ(c.space.load_u32(kDramBase+0x4004),1u);CHECK(input.interrupt());
}
TEST(graphics, absent_shared_memory_and_unadvertised_3d_are_rejected) {
  GpuFixture c;u64 value=0;CHECK(c.gpu.load(0xb0,32,value));CHECK_EQ(value,0xffffffffull);
  CHECK(c.gpu.load(0xb4,32,value));CHECK_EQ(value,0xffffffffull);
  CHECK_EQ(c.submit(c.command(0x108)),0x1205u);
  CHECK_EQ(c.submit(c.command(0x204,72)),0x1205u);
  CHECK_EQ(c.submit(c.command(0x207,32)),0x1205u);
}
TEST(graphics, optional_host_virgl_resources_and_transfers) {
  if(!std::getenv("NanoRV_TEST_VIRGL"))return;
  platform::Renderer renderer;std::string error;CHECK(renderer.open(error));
  std::printf("VirGL host: %s\n",renderer.description().c_str());
  std::printf("Host renderer: %s\n",renderer.description().c_str());
  CHECK(renderer.cap_info(1)[1]);CHECK(renderer.cap_info(2)[1]);CHECK(!renderer.caps(2,renderer.cap_info(2)[0]).empty());
  CHECK_EQ(renderer.create({1,2,2,(1u<<1)|(1u<<3),2,2,1,1,0,0,1}),0);
  std::array<u32,4> source{0xff102030,0xff405060,0xff708090,0xffa0b0c0};
  platform::RenderBacking backing{source.data(),sizeof(source)};CHECK_EQ(renderer.attach(1,{&backing,1}),0);
  CHECK_EQ(renderer.transfer(1,0,{0,0,0,2,2,1},0,0,0,0,false),0);
  std::vector<u32> pixels;CHECK(renderer.pixels(1,2,2,pixels));
  CHECK_EQ(pixels.size(),4u);for(unsigned n=0;n<4;++n)CHECK_EQ(pixels[n]&0xffffffu,source[n]&0xffffffu);
  CHECK_EQ(renderer.context(5,"NanoRV host integration"),0);renderer.context_resource(5,1,true);renderer.context_resource(5,1,false);renderer.destroy_context(5);
  renderer.unref(1);renderer.reset();
}
TEST(graphics, optional_parallel_virgl_stream_transfers_sync_changed_guest_pixels) {
  if(!std::getenv("NanoRV_TEST_VIRGL"))return;
  GpuFixture c;c.space.set_concurrent(true);std::string error;CHECK(c.gpu.open_renderer(error));
  c.resource();auto b=c.command(0x106,48);put(b,28,1);put(b,32,static_cast<u32>(kDramBase+0x5000));put(b,40,16);CHECK_EQ(c.submit(b),0x1100u);
  b=c.command(0x200,96,0);put(b,16,5);CHECK_EQ(c.submit(b),0x1100u);
  b=c.command(0x202);put(b,16,5);CHECK_EQ(c.submit(b),0x1100u);
  const std::array<u32,4> colors{0xff102030,0xff405060,0xff708090,0xffa0b0c0};
  CHECK(c.space.write_bytes(kDramBase+0x5000,{reinterpret_cast<const u8*>(colors.data()),16}));
  b=c.command(0x207,88,56);put(b,16,5);put(b,32,43|(13u<<16));put(b,36,1);
  put(b,68,2);put(b,72,2);put(b,76,1);put(b,84,1);CHECK_EQ(c.submit(b),0x1100u);
  auto scan=c.command(0x103,48);put(scan,24,0);put(scan,32,2);put(scan,36,2);put(scan,44,1);CHECK_EQ(c.submit(scan),0x1100u);
  CHECK(c.display.save("build/gpu-parallel-unit.bmp",error));auto pixels=platform::read_file("build/gpu-parallel-unit.bmp",&error);CHECK(pixels);
  for(unsigned n=0;n<4;++n) {u32 pixel;std::memcpy(&pixel,pixels->data()+54+n*4,4);CHECK_EQ(pixel&0xffffff,colors[n]&0xffffff);}
  CHECK(c.space.fill_zero(kDramBase+0x5000,16));put(b,84,2);CHECK_EQ(c.submit(b),0x1100u);
  for(unsigned n=0;n<4;++n)CHECK_EQ(c.space.load_u32(kDramBase+0x5000+n*4)&0xffffff,colors[n]&0xffffff);
  CHECK(c.space.store(kDramBase+0x5000,32,0x556677));CHECK(c.space.store(kDramBase+0x500c,32,0));
  put(b,56,1);put(b,60,1);put(b,68,1);put(b,72,1);put(b,80,12);
  CHECK_EQ(c.submit(b),0x1100u);CHECK_EQ(c.space.load_u32(kDramBase+0x5000),0x556677u);
  CHECK_EQ(c.space.load_u32(kDramBase+0x500c)&0xffffff,colors[3]&0xffffff);
  const auto version=c.space.reservation_version(kDramBase+0x500c);
  CHECK_EQ(c.submit(b),0x1100u);CHECK(c.space.reservation_version(kDramBase+0x500c)>version);
  put(b,32,43|(14u<<16));CHECK_EQ(c.submit(b),0x1200u); // reject truncated embedded command
}
TEST(graphics, vulkan_rejects_invalid_frames_and_headless_presentation) {
  platform::VulkanPresenter presenter;std::string error;
  CHECK(!presenter.open(nullptr,platform::HostGpu::Auto,error));CHECK(!error.empty());
  const std::array<u32,4> pixels{};
  CHECK(!presenter.present(2,2,pixels,error));
  platform::Display display;
  CHECK(!display.open(false,error,platform::DisplayBackend::Vulkan));
}
TEST(graphics, display_snapshots_remain_coherent_during_concurrent_present_and_save) {
  platform::Display display;std::string error;
  std::vector<u32> initial(17*13,0x112233);display.present(17,13,initial);
  std::jthread writer([&] {
    for(unsigned n=0;n<256;++n) {
      const unsigned width=17+n%3,height=13+n%5;
      std::vector<u32> pixels(static_cast<std::size_t>(width)*height,n*7919);
      display.present(width,height,pixels);
    }
  });
  for(unsigned n=0;n<16;++n) {
    CHECK(display.save("build/display-snapshot-unit.bmp",error));
    auto file=platform::read_file("build/display-snapshot-unit.bmp",&error);CHECK(file);
    u32 width=0,height=0,pixel=0;
    std::memcpy(&width,file->data()+18,4);std::memcpy(&height,file->data()+22,4);
    height=0u-height;CHECK_EQ(file->size(),54ull+static_cast<u64>(width)*height*4);
    std::memcpy(&pixel,file->data()+54,4);
    for(std::size_t offset=54;offset<file->size();offset+=4) {u32 actual;std::memcpy(&actual,file->data()+offset,4);CHECK_EQ(actual,pixel);}
  }
}
TEST(graphics, optional_host_vulkan_upload_readback_and_vendor_selection) {
  if(!std::getenv("NanoRV_TEST_VULKAN"))return;
  std::vector<platform::VulkanDevice> devices;std::string error,name;
  CHECK(platform::VulkanPresenter::enumerate(devices,error));CHECK(!devices.empty());
  CHECK(platform::VulkanPresenter::self_test(platform::HostGpu::Auto,name,error));
  std::printf("Vulkan GPU exact readback: %s\n",name.c_str());
  for(const auto pair:{std::pair{platform::HostGpu::Nvidia,0x10deu},std::pair{platform::HostGpu::Intel,0x8086u},std::pair{platform::HostGpu::Amd,0x1002u}}) {
    const bool exists=std::any_of(devices.begin(),devices.end(),[&](const auto& d){return d.vendor==pair.second && !d.software;});
    const bool success=platform::VulkanPresenter::self_test(pair.first,name,error);
    CHECK_EQ(success,exists);
  }
}
TEST(graphics, optional_host_vulkan_swapchain_resize_minimize_and_cleanup) {
  if(!std::getenv("NanoRV_TEST_VULKAN"))return;
  struct Window {
    HWND handle=CreateWindowExW(0,L"STATIC",L"NanoRV Vulkan test",WS_OVERLAPPEDWINDOW,40,40,257,193,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    ~Window() {if(handle)DestroyWindow(handle);}
  } window;
  CHECK(window.handle);ShowWindow(window.handle,SW_SHOWNOACTIVATE);
  platform::VulkanPresenter presenter;std::string error;
  CHECK(presenter.open(window.handle,platform::HostGpu::Auto,error));
  std::printf("Vulkan WSI: %s\n",presenter.description().c_str());
  for(unsigned frame=0;frame<48;++frame) {
    if(frame%8==0)CHECK(SetWindowPos(window.handle,nullptr,0,0,257+frame*3,193+frame,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE));
    const unsigned width=17+frame%3,height=13+frame%5;
    std::vector<u32> pixels(static_cast<std::size_t>(width)*height,0xff000000u|(frame*7919));
    CHECK(presenter.present(width,height,pixels,error));
    if(frame==24) {ShowWindow(window.handle,SW_MINIMIZE);CHECK(presenter.present(width,height,pixels,error));ShowWindow(window.handle,SW_SHOWNOACTIVATE);}
    MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {TranslateMessage(&message);DispatchMessageW(&message);}
  }
  const std::array<u32,4> invalid{};CHECK(!presenter.present(3,3,invalid,error));
}
