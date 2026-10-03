// SPDX-License-Identifier: MIT
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
using socklen_t = int;
#include "libslirp.h"
#include "platform/network.hpp"
#include "platform/platform.hpp"
#include <algorithm>
#include <deque>
#include <filesystem>
#include <limits>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <unordered_map>
namespace rvvm::platform {
struct NatNetwork::Impl {
  HMODULE module = nullptr;
  DLL_DIRECTORY_COOKIE directory = nullptr;
  Slirp* slirp = nullptr;
  bool winsock = false;
  decltype(&slirp_new) create = nullptr;
  decltype(&slirp_cleanup) cleanup = nullptr;
  decltype(&slirp_input) input = nullptr;
  decltype(&slirp_pollfds_fill_socket) fill = nullptr;
  decltype(&slirp_pollfds_poll) pollfds = nullptr;
  struct Timer { SlirpTimerCb cb; void* data; std::int64_t expires = INT64_MAX; };
  std::vector<Timer*> timers;
  std::vector<WSAPOLLFD> sockets;
  WSAEVENT socket_event=WSA_INVALID_EVENT;
  std::unordered_map<SOCKET,long> selected_sockets;
  std::deque<std::vector<std::uint8_t>> incoming;
  SlirpCb callbacks{};
  std::mutex frames_mutex;
  WakeEvent wake;
  std::function<void()> receive_wake;
  std::deque<std::vector<std::uint8_t>> outgoing;
  std::atomic<bool> stop{false};
  std::thread worker;
  decltype(&slirp_add_hostfwd) forward=nullptr;
  void poll_once();
  bool select_sockets();
  int poll_ready();
  void consume_socket_events() {
    for(const auto& [fd,mask]:selected_sockets) {
      (void)mask;WSANETWORKEVENTS events{};WSAEnumNetworkEvents(fd,nullptr,&events);
    }
  }
  void forget_socket(SOCKET fd) {
    if(selected_sockets.erase(fd))WSAEventSelect(fd,nullptr,0);
  }
  ~Impl() {
    stop=true;wake.notify();
    if(worker.joinable())worker.join();
    if (slirp) cleanup(slirp);
    if(socket_event!=WSA_INVALID_EVENT)WSACloseEvent(socket_event);
    for (auto* timer : timers) delete timer;
    if (module) FreeLibrary(module);
    if (directory) RemoveDllDirectory(directory);
    if (winsock) WSACleanup();
  }
};
NatNetwork::NatNetwork() : impl_(std::make_unique<Impl>()) {}
NatNetwork::~NatNetwork() = default;
bool NatNetwork::open(std::string& error,std::span<const PortForward> forwards) {
  // Reboot closes outstanding host connections and discards old guest frames.
  impl_=std::make_unique<Impl>();
  auto& s = *impl_;
  WSADATA data{};
  if (WSAStartup(MAKEWORD(2, 2), &data)) { error = "WSAStartup failed"; return false; }
  s.winsock = true;
  s.socket_event=WSACreateEvent(); // failure retains the bounded poll fallback
  const auto dependencies = std::filesystem::absolute("third_party/network-runtime").wstring();
  s.directory = AddDllDirectory(dependencies.c_str());
  const auto dll = std::filesystem::absolute("third_party/network-runtime/libslirp-0.dll").wstring();
  s.module = LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  if (!s.module) { error = "cannot load libslirp network runtime: " + last_error(); return false; }
#define SYMBOL(field, name) s.field = reinterpret_cast<decltype(s.field)>(GetProcAddress(s.module, #name)); if (!s.field) { error = "missing libslirp symbol " #name; return false; }
  SYMBOL(create, slirp_new)
  SYMBOL(cleanup, slirp_cleanup)
  SYMBOL(input, slirp_input)
  SYMBOL(fill, slirp_pollfds_fill_socket)
  SYMBOL(pollfds, slirp_pollfds_poll)
  SYMBOL(forward, slirp_add_hostfwd)
#undef SYMBOL
  auto& cb = s.callbacks;
  cb.send_packet = [](const void* buf, size_t length, void* opaque) -> slirp_ssize_t {
    auto& state = *static_cast<Impl*>(opaque);
    std::lock_guard lock(state.frames_mutex);
    if (state.incoming.size() < 4096 && length <= 65536) {
      auto* bytes = static_cast<const std::uint8_t*>(buf);
      state.incoming.emplace_back(bytes, bytes + length);
      if(state.receive_wake)state.receive_wake();
    }
    return static_cast<slirp_ssize_t>(length);
  };
  cb.guest_error = [](const char*, void*) {};
  cb.clock_get_ns = [](void*) -> int64_t { return static_cast<int64_t>(monotonic_ns()); };
  cb.timer_new = [](SlirpTimerCb fn, void* data, void* opaque) -> void* {
    auto* timer = new Impl::Timer{fn, data};
    static_cast<Impl*>(opaque)->timers.push_back(timer); return timer;
  };
  cb.timer_free = [](void* ptr, void* opaque) {
    auto& timers = static_cast<Impl*>(opaque)->timers;
    auto* timer = static_cast<Impl::Timer*>(ptr);
    std::erase(timers, timer); delete timer;
  };
  cb.timer_mod = [](void* timer, int64_t expiry, void* opaque) {
    static_cast<Impl::Timer*>(timer)->expires = expiry;static_cast<Impl*>(opaque)->wake.notify();
  };
  cb.register_poll_socket = [](slirp_os_socket fd, void* opaque) {
    // Invalidate the mask even if Windows reuses a recently closed SOCKET.
    static_cast<Impl*>(opaque)->selected_sockets[fd]=0;
  };
  cb.unregister_poll_socket = [](slirp_os_socket fd, void* opaque) {static_cast<Impl*>(opaque)->forget_socket(fd);};
  cb.notify = [](void* opaque) {static_cast<Impl*>(opaque)->wake.notify();};
  SlirpConfig cfg{};
  cfg.version = 6; cfg.in_enabled = true;
  inet_pton(AF_INET, "10.0.2.0", &cfg.vnetwork);
  inet_pton(AF_INET, "255.255.255.0", &cfg.vnetmask);
  inet_pton(AF_INET, "10.0.2.2", &cfg.vhost);
  inet_pton(AF_INET, "10.0.2.15", &cfg.vdhcp_start);
  inet_pton(AF_INET, "10.0.2.3", &cfg.vnameserver);
  cfg.vhostname = "rvvm"; cfg.if_mtu = cfg.if_mru = 1500;
  s.slirp = s.create(&cfg, &cb, &s);
  if (!s.slirp) { error = "libslirp initialization failed"; return false; }
  for(const auto& port:forwards) {
    in_addr host{},guest{};inet_pton(AF_INET,"127.0.0.1",&host);inet_pton(AF_INET,"10.0.2.15",&guest);
    if(!port.host || !port.guest || s.forward(s.slirp,0,host,port.host,guest,port.guest)<0) {
      error="cannot bind loopback TCP forward "+std::to_string(port.host)+":"+std::to_string(port.guest);return false;
    }
  }
  s.worker=std::thread([state=&s] {
    while(!state->stop.load(std::memory_order_relaxed)) {
      std::deque<std::vector<std::uint8_t>> outgoing;
      {std::lock_guard lock(state->frames_mutex);outgoing.swap(state->outgoing);}
      for(const auto& frame:outgoing)state->input(state->slirp,frame.data(),static_cast<int>(frame.size()));
      state->poll_once();
    }
  });
  return true;
}
void NatNetwork::send(std::span<const std::uint8_t> frame) {
  if (!impl_->slirp || frame.size()>65536)return;
  {std::lock_guard lock(impl_->frames_mutex);
    if(impl_->outgoing.size()>=4096)return;
    impl_->outgoing.emplace_back(frame.begin(),frame.end());}
  impl_->wake.notify();
}
void NatNetwork::poll() {} // host sockets are serviced independently of guest CPU speed
bool NatNetwork::Impl::select_sockets() {
  if(socket_event==WSA_INVALID_EVENT)return false;
  std::unordered_map<SOCKET,long> wanted;
  for(const auto& socket:sockets) {
    long mask=FD_CLOSE;
    if(socket.events&POLLRDNORM)mask|=FD_READ|FD_ACCEPT;
    if(socket.events&POLLWRNORM)mask|=FD_WRITE|FD_CONNECT;
    if(socket.events&POLLRDBAND)mask|=FD_OOB;
    wanted[socket.fd]|=mask;
  }
  for(auto it=selected_sockets.begin();it!=selected_sockets.end();) {
    if(!wanted.contains(it->first)) {WSAEventSelect(it->first,nullptr,0);it=selected_sockets.erase(it);}
    else ++it;
  }
  bool ready=true;
  for(const auto [fd,mask]:wanted) {
    const auto it=selected_sockets.find(fd);
    if(it!=selected_sockets.end() && it->second==mask)continue;
    if(WSAEventSelect(fd,socket_event,mask)==0)selected_sockets[fd]=mask;
    else ready=false;
  }
  return ready;
}
int NatNetwork::Impl::poll_ready() {
  int result=0;
  // Match QEMU's Windows AIO readiness probe. Windows select reports failed
  // nonblocking connects in exceptfds; WSAPoll is unreliable for that case.
  // Do not consume Winsock event/error records until libslirp has dispatched
  // them and inspected SO_ERROR. Chunking avoids the fixed fd_set capacity.
  for(std::size_t first=0;first<sockets.size();first+=FD_SETSIZE) {
    fd_set read{},write{},except{};FD_ZERO(&read);FD_ZERO(&write);FD_ZERO(&except);
    const auto end=std::min(first+FD_SETSIZE,sockets.size());
    for(auto n=first;n<end;++n) {
      auto& socket=sockets[n];socket.revents=0;
      if(socket.events&POLLRDNORM)FD_SET(socket.fd,&read);
      if(socket.events&POLLWRNORM)FD_SET(socket.fd,&write);
      FD_SET(socket.fd,&except);
    }
    timeval immediate{};
    if(select(0,&read,&write,&except,&immediate)==SOCKET_ERROR)return SOCKET_ERROR;
    for(auto n=first;n<end;++n) {
      auto& socket=sockets[n];
      if(FD_ISSET(socket.fd,&read))socket.revents|=POLLRDNORM;
      if(FD_ISSET(socket.fd,&write))socket.revents|=POLLWRNORM;
      if(FD_ISSET(socket.fd,&except)) {
        socket.revents|=POLLERR;
        if(socket.events&POLLWRNORM)socket.revents|=POLLWRNORM;
        if(socket.events&POLLRDBAND)socket.revents|=POLLRDBAND;
      }
      if(socket.revents)++result;
    }
  }
  return result;
}
void NatNetwork::Impl::poll_once() {
  auto& s=*this;
  s.sockets.clear();
  // Reset before examining socket records/level readiness. Events arriving
  // during this pass remain signalled, so there is no reset-after-poll race.
  if(s.socket_event!=WSA_INVALID_EVENT)WSAResetEvent(s.socket_event);
  uint32_t timeout = 100;
  s.fill(s.slirp, &timeout, [](slirp_os_socket fd, int events, void* opaque) {
    auto& sockets = static_cast<Impl*>(opaque)->sockets;
    short mask = 0;
    if (events & SLIRP_POLL_IN) mask |= POLLRDNORM;
    if (events & SLIRP_POLL_OUT) mask |= POLLWRNORM;
    if (events & SLIRP_POLL_PRI) mask |= POLLRDBAND;
    sockets.push_back({fd, mask, 0}); return static_cast<int>(sockets.size() - 1);
  }, &s);
  const bool events_ready=s.select_sockets();
  int result = s.poll_ready();
  if(result==0) {
    timeout=std::min(timeout,100u);
    const auto now_ms=static_cast<int64_t>(monotonic_ns()/1000000);
    for(const auto* timer:s.timers)if(timer->expires!=INT64_MAX) {
      const auto remaining=timer->expires>now_ms?static_cast<std::uint64_t>(timer->expires-now_ms):0ull;
      timeout=static_cast<uint32_t>(std::min<std::uint64_t>(timeout,remaining));
    }
    if(!events_ready)timeout=std::min(timeout,1u);
    s.wake.wait_ms(timeout,events_ready?s.socket_event:nullptr);
    result=s.poll_ready();
  }
  s.pollfds(s.slirp, result == SOCKET_ERROR, [](int index, void* opaque) {
    const short e = static_cast<Impl*>(opaque)->sockets[static_cast<std::size_t>(index)].revents;
    int flags = 0;
    if (e & POLLRDNORM) flags |= SLIRP_POLL_IN;
    if (e & POLLWRNORM) flags |= SLIRP_POLL_OUT;
    if (e & POLLRDBAND) flags |= SLIRP_POLL_PRI;
    if (e & (POLLERR|POLLNVAL)) flags |= SLIRP_POLL_ERR;
    if (e & POLLHUP) flags |= SLIRP_POLL_HUP;
    return flags;
  }, &s);
  s.consume_socket_events();
  const auto now = static_cast<int64_t>(monotonic_ns() / 1000000);
  const auto due_timers = s.timers;
  for (auto* timer : due_timers) if (std::find(s.timers.begin(),s.timers.end(),timer)!=s.timers.end() && timer->expires <= now) {
    timer->expires = INT64_MAX; timer->cb(timer->data);
  }
}
bool NatNetwork::receive(std::vector<std::uint8_t>& frame) {
  std::lock_guard lock(impl_->frames_mutex);
  if (impl_->incoming.empty()) return false;
  frame = std::move(impl_->incoming.front()); impl_->incoming.pop_front(); return true;
}
void NatNetwork::set_receive_wake(std::function<void()> callback) {
  std::lock_guard lock(impl_->frames_mutex);
  impl_->receive_wake=std::move(callback);
}
} // namespace rvvm::platform
