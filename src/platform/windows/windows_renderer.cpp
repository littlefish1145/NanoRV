// SPDX-License-Identifier: MIT
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <GL/gl.h>
#include "platform/renderer.hpp"
#include <filesystem>
#include <unordered_map>
#include <cstring>
#include <climits>
// Optimus and AMD switchable-graphics hints are exported by the final EXE.
// Vulkan uses explicit physical-device selection independently of these hints.
extern "C" {
__declspec(dllexport) DWORD NvOptimusEnablement=1;
__declspec(dllexport) DWORD AmdPowerXpressRequestHighPerformance=1;
}
// The Unix iovec ABI used by the MinGW renderer is pointer + size_t.
struct iovec {void* iov_base;std::size_t iov_len;};
struct virgl_box {std::uint32_t x,y,z,w,h,d;};
#include <virglrenderer.h>
namespace nanorv::platform {
struct Renderer::Impl {
  HMODULE library=nullptr;HWND window=nullptr;HDC dc=nullptr;HGLRC bootstrap=nullptr,shared=nullptr;
  using CreateContext=HGLRC(WINAPI*)(HDC,HGLRC,const int*);
  CreateContext create_context=nullptr;bool initialized=false;std::string renderer;
  virgl_renderer_callbacks callbacks{};
  std::unordered_map<std::uint32_t,std::vector<iovec>> backing;
  std::vector<std::uint32_t> command_buffer;
#define API(name) decltype(&virgl_renderer_##name) name=nullptr
  API(init);API(cleanup);API(reset);API(get_cap_set);API(fill_caps);API(resource_create);API(resource_unref);
  API(resource_attach_iov);API(resource_detach_iov);API(context_create);API(context_destroy);
  API(ctx_attach_resource);API(ctx_detach_resource);API(submit_cmd);API(transfer_read_iov);API(transfer_write_iov);
  API(force_ctx_0);API(get_rect);API(resource_get_info);
#undef API
  static void fence(void*,std::uint32_t) {}
  static void* create(void* cookie,int,virgl_renderer_gl_ctx_param* p) {
    auto& s=*static_cast<Impl*>(cookie);
    const int attributes[]{0x2091,p->major_ver,0x2092,p->minor_ver,0x9126,p->compat_ctx?2:1,0};
    auto ctx=s.create_context(s.dc,p->shared?s.shared:nullptr,attributes);
    if(ctx && !s.shared)s.shared=ctx;
    return ctx;
  }
  static void destroy(void* cookie,void* context) {
    auto& s=*static_cast<Impl*>(cookie);auto ctx=static_cast<HGLRC>(context);
    if(wglGetCurrentContext()==ctx)wglMakeCurrent(nullptr,nullptr);
    if(s.shared==ctx)s.shared=nullptr;
    wglDeleteContext(ctx);
  }
  static int current(void* cookie,int,void* context) {
    auto& s=*static_cast<Impl*>(cookie);return wglMakeCurrent(s.dc,static_cast<HGLRC>(context))?0:-1;
  }
  ~Impl() {
    if(initialized)cleanup(this);
    wglMakeCurrent(nullptr,nullptr);
    if(bootstrap)wglDeleteContext(bootstrap);
    if(dc)ReleaseDC(window,dc);
    if(window)DestroyWindow(window);
    if(library)FreeLibrary(library);
  }
};
Renderer::Renderer():impl_(std::make_unique<Impl>()) {} Renderer::~Renderer()=default;
bool Renderer::open(std::string& error,HostGpu gpu) {
  auto& s=*impl_;if(s.initialized)return true;
  const auto path=std::filesystem::absolute("third_party/virgl/ucrt64/bin/libvirglrenderer-1.dll");
  s.library=LoadLibraryExW(path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  if(!s.library) {error="cannot load bundled VirGL runtime (Windows error "+std::to_string(GetLastError())+")";return false;}
#define LOAD(name) s.name=reinterpret_cast<decltype(s.name)>(GetProcAddress(s.library,"virgl_renderer_" #name));if(!s.name) {error="VirGL missing API " #name;return false;}
  LOAD(init);LOAD(cleanup);LOAD(reset);LOAD(get_cap_set);LOAD(fill_caps);LOAD(resource_create);LOAD(resource_unref);
  LOAD(resource_attach_iov);LOAD(resource_detach_iov);LOAD(context_create);LOAD(context_destroy);LOAD(ctx_attach_resource);
  LOAD(ctx_detach_resource);LOAD(submit_cmd);LOAD(transfer_read_iov);LOAD(transfer_write_iov);LOAD(force_ctx_0);LOAD(get_rect);LOAD(resource_get_info);
#undef LOAD
  WNDCLASSW wc{};wc.style=CS_OWNDC;wc.lpfnWndProc=DefWindowProcW;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"NanoRV VirGL context";
  if(!RegisterClassW(&wc) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS) {error="cannot register OpenGL window class";return false;}
  s.window=CreateWindowW(wc.lpszClassName,L"NanoRV renderer",WS_POPUP,0,0,1,1,nullptr,nullptr,wc.hInstance,nullptr);
  if(!s.window || !(s.dc=GetDC(s.window))) {error="cannot create OpenGL surface";return false;}
  PIXELFORMATDESCRIPTOR pfd{};pfd.nSize=sizeof(pfd);pfd.nVersion=1;pfd.dwFlags=PFD_DRAW_TO_WINDOW|PFD_SUPPORT_OPENGL|PFD_DOUBLEBUFFER;pfd.iPixelType=PFD_TYPE_RGBA;pfd.cColorBits=32;pfd.cDepthBits=24;
  const int format=ChoosePixelFormat(s.dc,&pfd);
  if(!format || !SetPixelFormat(s.dc,format,&pfd) || !(s.bootstrap=wglCreateContext(s.dc)) || !wglMakeCurrent(s.dc,s.bootstrap)) {error="cannot initialize Windows OpenGL";return false;}
  s.create_context=reinterpret_cast<Impl::CreateContext>(wglGetProcAddress("wglCreateContextAttribsARB"));
  if(!s.create_context) {error="host driver needs OpenGL 3.3 context support";return false;}
  const auto* name=glGetString(GL_RENDERER);s.renderer=name?reinterpret_cast<const char*>(name):"unknown OpenGL driver";
  const auto* vendor_text=glGetString(GL_VENDOR);
  const std::string vendor=vendor_text?reinterpret_cast<const char*>(vendor_text):"unknown";
  if((gpu==HostGpu::Nvidia && vendor.find("NVIDIA")==std::string::npos) ||
      (gpu==HostGpu::Intel && vendor.find("Intel")==std::string::npos) ||
      (gpu==HostGpu::Amd && vendor.find("ATI")==std::string::npos && vendor.find("AMD")==std::string::npos)) {
    error="Windows selected OpenGL vendor "+vendor+"; select the requested GPU for nanorv.exe in Windows Graphics settings";return false;
  }
  s.callbacks.version=1;s.callbacks.write_fence=Impl::fence;s.callbacks.create_gl_context=Impl::create;s.callbacks.destroy_gl_context=Impl::destroy;s.callbacks.make_current=Impl::current;
  const int result=s.init(&s,0,&s.callbacks);
  if(result) {error="VirGL initialization failed: "+std::to_string(result);return false;}
  s.initialized=true;return true;
}
bool Renderer::ready() const {return impl_->initialized;}
std::string Renderer::description() const {return impl_->renderer;}
void Renderer::reset() {if(ready()) {impl_->reset();impl_->backing.clear();}}
std::array<std::uint32_t,2> Renderer::cap_info(std::uint32_t id) {std::array<std::uint32_t,2> r{};if(ready())impl_->get_cap_set(id,&r[0],&r[1]);return r;}
std::vector<std::uint8_t> Renderer::caps(std::uint32_t id,std::uint32_t version) {auto info=cap_info(id);if(!info[1] || version>info[0] || info[1]>1024*1024)return {};std::vector<std::uint8_t> out(info[1]);impl_->fill_caps(id,version,out.data());return out;}
int Renderer::create(std::array<std::uint32_t,11> args) {virgl_renderer_resource_create_args a{};static_assert(sizeof(a)==sizeof(args));std::memcpy(&a,args.data(),sizeof(a));return impl_->resource_create(&a,nullptr,0);}
void Renderer::unref(std::uint32_t id) {impl_->resource_unref(id);impl_->backing.erase(id);}
int Renderer::attach(std::uint32_t id,std::span<const RenderBacking> data) {
  // virglrenderer retains the iovec array; preserve it until detach or unref.
  std::vector<iovec> entries;for(auto b:data)entries.push_back({b.data,b.size});
  detach(id);auto& iov=impl_->backing[id];iov=std::move(entries);
  return impl_->resource_attach_iov(static_cast<int>(id),iov.data(),static_cast<int>(iov.size()));
}
void Renderer::detach(std::uint32_t id) {iovec* iov=nullptr;int count=0;impl_->resource_detach_iov(static_cast<int>(id),&iov,&count);impl_->backing.erase(id);}
int Renderer::context(std::uint32_t id,std::string name) {return impl_->context_create(id,static_cast<std::uint32_t>(name.size()),name.data());}
void Renderer::destroy_context(std::uint32_t id) {impl_->context_destroy(id);}
void Renderer::context_resource(std::uint32_t ctx,std::uint32_t id,bool attach) {if(attach)impl_->ctx_attach_resource(static_cast<int>(ctx),static_cast<int>(id));else impl_->ctx_detach_resource(static_cast<int>(ctx),static_cast<int>(id));}
int Renderer::submit(std::uint32_t ctx,std::span<const std::uint8_t> data) {
  if(data.size()%4 || data.size()/4>INT_MAX)return -1;
  auto& aligned=impl_->command_buffer;aligned.resize(data.size()/4);std::memcpy(aligned.data(),data.data(),data.size());return impl_->submit_cmd(aligned.data(),static_cast<int>(ctx),static_cast<int>(aligned.size()));
}
int Renderer::transfer(std::uint32_t id,std::uint32_t ctx,std::array<std::uint32_t,6> box,std::uint64_t offset,std::uint32_t level,std::uint32_t stride,std::uint32_t layer,bool read) {
  virgl_box b{};std::memcpy(&b,box.data(),sizeof(b));
  if(read)return impl_->transfer_read_iov(id,ctx,level,stride,layer,&b,offset,nullptr,0);
  return impl_->transfer_write_iov(id,ctx,static_cast<int>(level),stride,layer,&b,offset,nullptr,0);
}
bool Renderer::pixels(std::uint32_t id,unsigned width,unsigned height,std::vector<std::uint32_t>& output) {
  impl_->force_ctx_0();virgl_renderer_resource_info info{};
  if(impl_->resource_get_info(static_cast<int>(id),&info) || info.width!=width || info.height!=height)return false;
  output.resize(static_cast<std::size_t>(width)*height);iovec iov{output.data(),output.size()*4};
  impl_->get_rect(static_cast<int>(id),&iov,1,0,0,0,static_cast<int>(width),static_cast<int>(height));return true;
}
void Renderer::finish() {glFinish();}
}
