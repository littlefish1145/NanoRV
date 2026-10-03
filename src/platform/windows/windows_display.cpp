// SPDX-License-Identifier: MIT
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "platform/display.hpp"
#include "platform/platform.hpp"
#include "core/log.hpp"
#include <mutex>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <array>
#include <cstring>
#include <algorithm>
namespace rvvm::platform {
struct Display::Impl {
  std::mutex mutex;std::condition_variable started;
  std::thread thread;std::atomic<HWND> window{nullptr};
  bool ready=false,success=false;
  struct Frame {unsigned width=0,height=0;std::vector<std::uint32_t> pixels;};
  std::array<std::shared_ptr<Frame>,3> buffers;
  std::shared_ptr<const Frame> frame;
  std::vector<InputEvent> input;
  std::array<bool,128> pressed{};bool mouse_valid=false;int mouse_x=0,mouse_y=0;
  bool raw_mouse=false;
  std::atomic<bool> paint_pending{false};
  std::unique_ptr<VulkanPresenter> vulkan;
  std::string backend_name="headless";
  ~Impl() {if(auto hwnd=window.load())PostMessageW(hwnd,WM_CLOSE,0,0);if(thread.joinable())thread.join();}
  void event(std::uint16_t type,std::uint16_t code,std::int32_t value) {
    std::lock_guard lock(mutex);if(input.size()<4094) {input.push_back({type,code,value});input.push_back({0,0,0});}
  }
  static LRESULT CALLBACK procedure(HWND hwnd,UINT msg,WPARAM w,LPARAM l) {
    auto* s=reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
    if(msg==WM_NCCREATE) {s=static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(s));}
    if(!s)return DefWindowProcW(hwnd,msg,w,l);
    switch(msg) {
    case WM_PAINT: {
      PAINTSTRUCT paint{};HDC dc=BeginPaint(hwnd,&paint);RECT rect{};GetClientRect(hwnd,&rect);
      std::shared_ptr<const Frame> frame;
      {
        std::lock_guard lock(s->mutex);frame=s->frame;
        // A newer producer can enqueue another repaint while the GPU presents
        // this immutable frame. Never hold the input/device mutex over a fence.
        s->paint_pending=false;
      }
      if(s->vulkan && frame) {
        std::string error;
        if(!s->vulkan->present(frame->width,frame->height,frame->pixels,error)) {
          RVVM_ERROR("Vulkan presentation failed: %s; switching to GDI",error.c_str());
          s->vulkan.reset();std::lock_guard lock(s->mutex);s->backend_name="GDI (Vulkan runtime failure)";
        }
      }
      if(!s->vulkan && frame) {
        BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth=static_cast<LONG>(frame->width);info.bmiHeader.biHeight=-static_cast<LONG>(frame->height);
        info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
        SetStretchBltMode(dc,COLORONCOLOR);
        StretchDIBits(dc,0,0,rect.right,rect.bottom,0,0,static_cast<int>(frame->width),static_cast<int>(frame->height),frame->pixels.data(),&info,DIB_RGB_COLORS,SRCCOPY);
      } else if(!frame)FillRect(dc,&rect,static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
      EndPaint(hwnd,&paint);return 0;
    }
    case WM_APP:InvalidateRect(hwnd,nullptr,FALSE);return 0;
    case WM_SIZE:InvalidateRect(hwnd,nullptr,FALSE);return 0;
    case WM_KEYDOWN:case WM_KEYUP:case WM_SYSKEYDOWN:case WM_SYSKEYUP: {
      unsigned code=(static_cast<unsigned long long>(l)>>16)&127;
      if((static_cast<unsigned long long>(l)>>24)&1) {
        switch(code) {case 28:code=96;break;case 29:code=97;break;case 56:code=100;break;
        case 71:code=102;break;case 72:code=103;break;case 73:code=104;break;case 75:code=105;break;
        case 77:code=106;break;case 79:code=107;break;case 80:code=108;break;case 81:code=109;break;
        case 82:code=110;break;case 83:code=111;break;case 53:code=98;break;case 91:code=125;break;case 92:code=126;break;default:break;}
      }
      if(code && code<128) {
        const bool down=msg==WM_KEYDOWN || msg==WM_SYSKEYDOWN;
        s->event(1,static_cast<std::uint16_t>(code),down?(s->pressed[code]?2:1):0);s->pressed[code]=down;
      }
      return 0;
    }
    case WM_KILLFOCUS:
      for(unsigned n=1;n<128;++n)if(s->pressed[n]) {s->event(1,static_cast<std::uint16_t>(n),0);s->pressed[n]=false;}
      for(std::uint16_t n=272;n<=274;++n)s->event(1,n,0);
      s->mouse_valid=false;return 0;
    case WM_MOUSEMOVE: {
      if(s->raw_mouse)return 0;
      const int x=static_cast<short>(LOWORD(l)),y=static_cast<short>(HIWORD(l));
      if(s->mouse_valid) {if(x!=s->mouse_x)s->event(2,0,x-s->mouse_x);if(y!=s->mouse_y)s->event(2,1,y-s->mouse_y);}
      s->mouse_x=x;s->mouse_y=y;s->mouse_valid=true;return 0;
    }
    case WM_INPUT: {
      RAWINPUT raw{};UINT size=sizeof(raw);
      if(GetRawInputData(reinterpret_cast<HRAWINPUT>(l),RID_INPUT,&raw,&size,sizeof(RAWINPUTHEADER))!=static_cast<UINT>(-1) && raw.header.dwType==RIM_TYPEMOUSE && !(raw.data.mouse.usFlags&MOUSE_MOVE_ABSOLUTE)) {
        if(raw.data.mouse.lLastX)s->event(2,0,raw.data.mouse.lLastX);
        if(raw.data.mouse.lLastY)s->event(2,1,raw.data.mouse.lLastY);
      }
      return DefWindowProcW(hwnd,msg,w,l);
    }
    case WM_LBUTTONDOWN:case WM_RBUTTONDOWN:case WM_MBUTTONDOWN:
      SetFocus(hwnd);SetCapture(hwnd);s->event(1,msg==WM_LBUTTONDOWN?272:msg==WM_RBUTTONDOWN?273:274,1);return 0;
    case WM_LBUTTONUP:case WM_RBUTTONUP:case WM_MBUTTONUP:
      ReleaseCapture();s->event(1,msg==WM_LBUTTONUP?272:msg==WM_RBUTTONUP?273:274,0);return 0;
    case WM_MOUSEWHEEL:s->event(2,8,static_cast<short>(HIWORD(w))/WHEEL_DELTA);return 0;
    case WM_DESTROY:s->vulkan.reset();s->window=nullptr;PostQuitMessage(0);return 0;
    default:return DefWindowProcW(hwnd,msg,w,l);
    }
  }
};
Display::Display():impl_(std::make_unique<Impl>()) {}
Display::~Display()=default;
bool Display::open(bool visible,std::string& error,DisplayBackend backend,HostGpu gpu) {
  if(!visible) {
    if(backend==DisplayBackend::Vulkan) {error="--display-backend vulkan requires --display window";return false;}
    return true;
  }
  if(impl_->thread.joinable())return true;
  auto& s=*impl_;
  std::string startup_error;
  s.thread=std::thread([&s,backend,gpu,&startup_error] {
    const auto instance=GetModuleHandleW(nullptr);
    WNDCLASSW wc{};wc.lpfnWndProc=Impl::procedure;wc.hInstance=instance;wc.lpszClassName=L"RVVMDisplay";wc.hCursor=LoadCursorW(nullptr,MAKEINTRESOURCEW(32512));
    RegisterClassW(&wc);
    HWND hwnd=CreateWindowExW(0,wc.lpszClassName,L"RVVM Debian — keyboard/mouse",WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,816,639,nullptr,nullptr,instance,&s);
    s.window=hwnd;
    bool success=hwnd!=nullptr;
    if(success && backend!=DisplayBackend::Gdi) {
      s.vulkan=std::make_unique<VulkanPresenter>();
      if(!s.vulkan->open(hwnd,gpu,startup_error)) {
        s.vulkan.reset();
        if(backend==DisplayBackend::Vulkan)success=false;
        else RVVM_WARN("Vulkan unavailable: %s; using GDI",startup_error.c_str());
      }
    }
    s.backend_name=s.vulkan?"Vulkan / "+s.vulkan->description():"GDI";
    {std::lock_guard lock(s.mutex);s.ready=true;s.success=success;}s.started.notify_one();
    if(!success) {if(hwnd)DestroyWindow(hwnd);return;}
    const RAWINPUTDEVICE mouse{1,2,0,hwnd};s.raw_mouse=RegisterRawInputDevices(&mouse,1,sizeof(mouse))!=FALSE;
    ShowWindow(hwnd,SW_SHOW);UpdateWindow(hwnd);MSG message{};
    while(GetMessageW(&message,nullptr,0,0)>0) {TranslateMessage(&message);DispatchMessageW(&message);}
  });
  std::unique_lock lock(s.mutex);s.started.wait(lock,[&]{return s.ready;});
  if(!s.success)error=startup_error.empty()?"cannot create Win32 display window":startup_error;return s.success;
}
std::string Display::description() const {std::lock_guard lock(impl_->mutex);return impl_->backend_name;}
void Display::present(unsigned width,unsigned height,std::span<const std::uint32_t> pixels) {
  if(!width || !height || pixels.size()!=static_cast<std::size_t>(width)*height)return;
  {
    std::lock_guard lock(impl_->mutex);
    auto free=std::find_if(impl_->buffers.begin(),impl_->buffers.end(),[](const auto& buffer){return !buffer || buffer.use_count()==1;});
    // Screenshots may retain additional immutable snapshots temporarily.
    auto buffer=free==impl_->buffers.end()?std::make_shared<Impl::Frame>():*free;
    if(!buffer)buffer=std::make_shared<Impl::Frame>();
    buffer->width=width;buffer->height=height;buffer->pixels.assign(pixels.begin(),pixels.end());
    if(free!=impl_->buffers.end())*free=buffer;
    impl_->frame=std::move(buffer);
  }
  if(auto hwnd=impl_->window.load();hwnd && !impl_->paint_pending.exchange(true))PostMessageW(hwnd,WM_APP,0,0);
}
std::vector<InputEvent> Display::events() {std::lock_guard lock(impl_->mutex);std::vector<InputEvent> result;result.swap(impl_->input);return result;}
bool Display::save(const std::string& path,std::string& error) {
  std::shared_ptr<const Impl::Frame> frame;
  {std::lock_guard lock(impl_->mutex);frame=impl_->frame;}
  if(!frame) {error="guest has not produced a display frame";return false;}
  const auto bytes=static_cast<DWORD>(frame->pixels.size()*4);
  constexpr DWORD offset=sizeof(BITMAPFILEHEADER)+sizeof(BITMAPINFOHEADER);
  BITMAPFILEHEADER file{0x4d42,offset+bytes,0,0,offset};
  BITMAPINFOHEADER header{};header.biSize=sizeof(header);header.biWidth=static_cast<LONG>(frame->width);header.biHeight=-static_cast<LONG>(frame->height);header.biPlanes=1;header.biBitCount=32;header.biSizeImage=bytes;
  std::vector<std::uint8_t> output(file.bfSize);
  std::memcpy(output.data(),&file,sizeof(file));std::memcpy(output.data()+sizeof(file),&header,sizeof(header));std::memcpy(output.data()+file.bfOffBits,frame->pixels.data(),bytes);
  return write_file(path,output,&error);
}
}
