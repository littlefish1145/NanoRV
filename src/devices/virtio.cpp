// SPDX-License-Identifier: MIT
#include "devices/virtio.hpp"
#include "core/log.hpp"
#include <cstring>
#include <limits>
#include <mutex>
#include <condition_variable>
#include <thread>
namespace rvvm::devices {
VirtioMmio::VirtioMmio(memory::AddressSpace& bus, u32 id, unsigned queues, u64 features)
  : bus_(bus), id_(id), queue_count_(queues), features_(features | (1ull << 32)) {}
void VirtioMmio::fail() {
  if (!(status_ & 64)) RVVM_WARN("VirtIO device %u: invalid queue or DMA; device requires reset",id_);
  status_ |= 64; interrupt_status_ |= 2;
}
bool VirtioMmio::load(u64 o, unsigned w, u64& v) noexcept {
  v = 0;
  if (o >= 0x100) return config_read(o - 0x100, w, v);
  if (w != 32 || (o & 3)) return false;
  const auto& q = queues_[queue_selector_ < queue_count_ ? queue_selector_ : 0];
  switch (o) {
  case 0: v = 0x74726976; break;
  case 4: v = 2; break;
  case 8: v = id_; break;
  case 12: v = 0x5256564d; break;
  case 0x10: v = device_selector_ < 2 ? static_cast<u32>(features_ >> (device_selector_ * 32)) : 0; break;
  case 0x34: v = queue_selector_ < queue_count_ ? 256 : 0; break;
  case 0x38: v = q.num; break;
  case 0x44: v = queue_selector_ < queue_count_ ? q.ready : 0; break;
  case 0x60: v = interrupt_status_; break;
  case 0x70: v = status_; break;
  case 0xb0: case 0xb4: v=0xffffffffu;break; // no shared-memory regions
  case 0xfc: v = 0; break;
  default: break;
  }
  return true;
}
bool VirtioMmio::store(u64 o, unsigned w, u64 v) noexcept {
  if(o>=0x100)return config_write(o-0x100,w,v);
  if (w != 32 || (o & 3) || o >= 0x100) return false;
  auto& q = queues_[queue_selector_ < queue_count_ ? queue_selector_ : 0];
  auto part = [&](u64& target, bool high) {
    unsigned shift = high ? 32 : 0;
    target = (target & ~(0xffffffffull << shift)) | ((v & 0xffffffff) << shift);
  };
  switch (o) {
  case 0x14: device_selector_ = static_cast<u32>(v); break;
  case 0x20: if (driver_selector_ < 2) part(driver_features_, driver_selector_ != 0); break;
  case 0x24: driver_selector_ = static_cast<u32>(v); break;
  case 0x30: queue_selector_ = static_cast<u32>(v); break;
  case 0x38:
    if (queue_selector_ >= queue_count_ || q.ready || !v || v > 256 || (v & (v - 1))) fail();
    else q.num = static_cast<u32>(v);
    break;
  case 0x44:
    if (queue_selector_ >= queue_count_) { fail(); break; }
    if (v && (!q.num || (q.desc & 15) || (q.avail & 1) || (q.used & 3) ||
        !bus_.direct(q.desc, q.num * 16, memory::Access::Read) ||
        !bus_.direct(q.avail, 4 + q.num * 2, memory::Access::Read) ||
        !bus_.direct(q.used, 4 + q.num * 8, memory::Access::Write))) fail();
    else {
      if(!v && q.ready)++q.generation;
      q.ready = static_cast<u32>(v) & 1;
    }
    break;
  case 0x50: if (v < queue_count_ && ready()) notify(static_cast<unsigned>(v)); break;
  case 0x64: interrupt_status_ &= ~static_cast<u32>(v); break;
  case 0x70:
    if (!v) { status_ = interrupt_status_ = 0; driver_features_ = 0; queues_ = {}; reset_device(); }
    else {
      status_ = static_cast<u32>(v);
      if ((status_ & 8) && ((driver_features_ & ~features_) || !(driver_features_ & (1ull << 32)))) status_ &= ~8u;
    }
    break;
  case 0x80: if (!q.ready) part(q.desc, false); break;
  case 0x84: if (!q.ready) part(q.desc, true); break;
  case 0x90: if (!q.ready) part(q.avail, false); break;
  case 0x94: if (!q.ready) part(q.avail, true); break;
  case 0xa0: if (!q.ready) part(q.used, false); break;
  case 0xa4: if (!q.ready) part(q.used, true); break;
  default: break;
  }
  return true;
}
bool VirtioMmio::take(unsigned index, u16& head, std::vector<Descriptor>& chain) {
  auto& q = queues_[index];
  if (!ready() || !q.ready) return false;
  u64 v = 0;
  if (!bus_.load(q.avail + 2, 16, v)) { fail(); return false; }
  const auto pending = static_cast<u16>(static_cast<u16>(v) - q.next_avail);
  if (!pending) return false;
  if (pending > q.num || !bus_.load(q.avail + 4 + (q.next_avail % q.num) * 2, 16, v)) { fail(); return false; }
  head = static_cast<u16>(v);
  unsigned n = head;
  std::array<bool, 256> seen{};
  chain.clear();
  u64 total = 0;
  for (;;) {
    if (n >= q.num || seen[n]) { fail(); return false; }
    seen[n] = true;
    Descriptor d{};
    if (!bus_.read_bytes(q.desc + n * 16, {reinterpret_cast<u8*>(&d), sizeof(d)}) ||
        (d.flags & ~3u) || !(bus_.direct(d.address, d.length, d.flags & 2 ? memory::Access::Write : memory::Access::Read)) ||
        (total += d.length) > 16 * 1024 * 1024) { fail(); return false; }
    chain.push_back(d);
    if (!(d.flags & 1)) break;
    n = d.next;
  }
  ++q.next_avail;
  return true;
}
void VirtioMmio::complete(unsigned index, u16 head, u32 length) {
  auto& q = queues_[index];
  const u64 slot = q.used + 4 + (q.used_index % q.num) * 8;
  if (!bus_.store(slot, 32, head) || !bus_.store(slot + 4, 32, length) ||
      !bus_.store(q.used + 2, 16, ++q.used_index)) { fail(); return; }
  u64 flags = 0;
  if (!bus_.load(q.avail, 16, flags)) { fail(); return; }
  if (!(flags & 1)) interrupt_status_ |= 1;
}
struct VirtioBlock::AsyncIo {
  std::mutex mutex;std::condition_variable wake;
  std::deque<Request> requests,completed;
  std::thread worker;std::function<void()> io_wake;
  std::atomic<bool> completed_ready{false};
  std::size_t pending=0,bytes=0;bool stopping=false,busy=false;
  ~AsyncIo() {
    {std::lock_guard lock(mutex);stopping=true;io_wake={};}
    wake.notify_one();if(worker.joinable())worker.join();
  }
};
VirtioBlock::VirtioBlock(memory::AddressSpace& bus) : VirtioMmio(bus, 2, 1, (1ull << 6) | (1ull << 9)) {}
VirtioBlock::~VirtioBlock()=default;
void VirtioBlock::set_async(bool enabled) {
  if(enabled==static_cast<bool>(async_))return;
  if(!enabled) {
    auto state=std::move(async_);
    {std::lock_guard lock(state->mutex);state->stopping=true;state->io_wake={};}
    state->wake.notify_one();state->worker.join();
    for(auto& request:state->completed)if(request.epoch==io_epoch_)finish(request);
    return;
  }
  auto state=std::make_unique<AsyncIo>();
  state->worker=std::thread([this,s=state.get()] {
    std::vector<Request> batch;batch.reserve(16);std::vector<u8> buffer;
    for(;;) {
      batch.clear();
      {std::unique_lock lock(s->mutex);s->wake.wait(lock,[&]{return s->stopping || !s->requests.empty();});
        if(s->requests.empty())return;
        while(batch.size()<16 && !s->requests.empty()) {batch.push_back(std::move(s->requests.front()));s->requests.pop_front();}
        s->busy=true;}
      perform_batch(batch,buffer);
      {std::lock_guard lock(s->mutex);
        for(auto& request:batch)s->completed.push_back(std::move(request));s->busy=false;
        s->completed_ready.store(true,std::memory_order_release);
        if(s->io_wake)s->io_wake();}
      s->wake.notify_all();
    }
  });
  async_=std::move(state);
}
void VirtioBlock::set_io_wake(std::function<void()> callback) {
  if(async_) {std::lock_guard lock(async_->mutex);async_->io_wake=std::move(callback);}
}
void VirtioBlock::reset_device() {
  ++io_epoch_;
  if(!async_)return;
  // Finish accepted writes/flushes before reset returns. The worker never
  // accesses guest memory, and old completion DMA is discarded after reset.
  std::unique_lock lock(async_->mutex);
  async_->wake.wait(lock,[&]{return async_->requests.empty() && !async_->busy;});
  async_->completed.clear();async_->pending=async_->bytes=0;
  async_->completed_ready.store(false,std::memory_order_relaxed);
}
void VirtioBlock::poll() {
  if(!async_ || !async_->completed_ready.exchange(false,std::memory_order_acquire))return;
  std::deque<Request> completed;
  {std::lock_guard lock(async_->mutex);completed.swap(async_->completed);
    for(const auto& request:completed) {--async_->pending;async_->bytes-=request.data.size();}}
  for(auto& request:completed)if(request.epoch==io_epoch_ && ready() && queues_[request.queue].ready)finish(request);
  if(!completed.empty() && ready())notify(0);
}
bool VirtioBlock::open(const std::string& path, std::string& error) {
  const bool asynchronous=static_cast<bool>(async_);
  set_async(false);
  if(!disk_.open(path,error))return false;
  std::array<u8,4> magic{};
  if(!disk_.read(0,magic)) {error="cannot read disk header";return false;}
  if (std::memcmp(magic.data(),"QFI\xfb",4)==0) { error="--disk expects raw format; convert qcow2 with qemu-img first"; return false; }
  if (disk_.size() % 512) { error = "raw disk must contain whole 512-byte sectors"; return false; }
  capacity_ = disk_.size() / 512;
  set_async(asynchronous);
  return true;
}
bool VirtioBlock::config_read(u64 o, unsigned w, u64& v) {
  std::array<u8, 64> data{};
  std::memcpy(data.data(), &capacity_, 8);
  u32 sector = 512; std::memcpy(data.data() + 20, &sector, 4);
  if ((w != 8 && w != 16 && w != 32 && w != 64) || o >= data.size() || w / 8 > data.size() - o) return false;
  v = 0; std::memcpy(&v, data.data() + o, w / 8); return true;
}
void VirtioBlock::notify(unsigned index) {
  u16 head;
  std::vector<Descriptor> chain;
  for (;;) {
    if(async_) {std::lock_guard lock(async_->mutex);
      if(async_->pending>=256 || async_->bytes>=32*1024*1024)return;}
    if(!take(index, head, chain))return;
    if (chain.size() < 2 || (chain.front().flags & 2) || chain.front().length < 16 ||
        !(chain.back().flags & 2) || chain.back().length < 1) { fail(); return; }
    u64 type = 0, sector = 0;
    bus_.load(chain.front().address, 32, type);
    bus_.load(chain.front().address + 8, 64, sector);
    Request request;request.queue=index;request.head=head;request.epoch=io_epoch_;request.type=type;
    request.queue_generation=queues_[index].generation;
    u64 bytes = 0;
    for (std::size_t n = 1; n + 1 < chain.size(); ++n) bytes += chain[n].length;
    if (type == 0 || type == 1) {
      if (sector > capacity_ || bytes % 512 || bytes / 512 > capacity_ - sector) request.status = 1;
      if (!request.status) {
        request.position=sector*512;request.data.resize(static_cast<std::size_t>(bytes));
        std::size_t offset=0;
        for (std::size_t n = 1; n + 1 < chain.size(); ++n) {
          const auto& d = chain[n];
          if (static_cast<bool>(d.flags & 2) != (type == 0) ||
              (type==1 && d.length && !bus_.read_bytes(d.address,{request.data.data()+offset,d.length}))) {request.status=1;break;}
          offset+=d.length;
        }
      }
    } else if (type == 4) {
      // Ordered with preceding writes on the same FIFO worker.
    } else if (type == 8) {
      constexpr char id[] = "RVVM native disk";
      if (chain.size() < 3 || !(chain[1].flags & 2)) request.status = 1;
      else {
        request.data.resize(std::min<u32>(chain[1].length,20));
        if(!request.data.empty())std::memcpy(request.data.data(),id,std::min<std::size_t>(request.data.size(),sizeof(id)-1));
      }
    } else request.status = 2;
    request.chain=std::move(chain);
    if(async_) {
      {std::lock_guard lock(async_->mutex);++async_->pending;async_->bytes+=request.data.size();
        async_->requests.push_back(std::move(request));}
      async_->wake.notify_one();
    } else {perform(request);finish(request);}
  }
}
void VirtioBlock::perform(Request& request) {
  if(request.status)return;
  const bool ok=request.type==0?disk_.read(request.position,request.data):
    request.type==1?disk_.write(request.position,request.data):request.type==4?disk_.flush():true;
  if(!ok)request.status=1;
}
void VirtioBlock::perform_batch(std::vector<Request>& batch,std::vector<u8>& buffer) {
  for(std::size_t first=0;first<batch.size();) {
    auto& request=batch[first];std::size_t end=first+1,bytes=request.data.size();
    if(!request.status && request.type<=1 && bytes) {
      // Merge only adjacent FIFO requests of the same direction. Never move a
      // request across a read/write dependency or a flush barrier.
      while(end<batch.size()) {
        const auto& next=batch[end];
        if(next.status || next.type!=request.type || !next.data.size() ||
            next.position!=request.position+bytes || next.data.size()>1024*1024 || bytes>1024*1024-next.data.size())break;
        bytes+=next.data.size();++end;
      }
    }
    if(end==first+1)perform(request);
    else {
      buffer.resize(bytes);std::size_t offset=0;
      if(request.type==1)for(auto n=first;n<end;++n) {
        const auto& data=batch[n].data;std::memcpy(buffer.data()+offset,data.data(),data.size());offset+=data.size();
      }
      const bool ok=request.type==0?disk_.read(request.position,buffer):disk_.write(request.position,buffer);
      offset=0;
      for(auto n=first;n<end;++n) {
        auto& item=batch[n];if(!ok)item.status=1;
        else if(request.type==0)std::memcpy(item.data.data(),buffer.data()+offset,item.data.size());
        offset+=item.data.size();
      }
    }
    first=end;
  }
}
void VirtioBlock::finish(Request& request) {
  if(request.epoch!=io_epoch_ || !ready() || !queues_[request.queue].ready ||
      request.queue_generation!=queues_[request.queue].generation)return;
  u32 written=1;
  if(!request.status && request.type==0) {
    std::size_t offset=0;
    for(std::size_t n=1;n+1<request.chain.size();++n) {
      const auto& d=request.chain[n];
      if(!d.length)continue;
      if(!bus_.write_bytes(d.address,{request.data.data()+offset,d.length})) {request.status=1;break;}
      offset+=d.length;written+=d.length;
    }
  } else if(!request.status && request.type==8) {
    if(!bus_.write_bytes(request.chain[1].address,request.data))request.status=1;
    else written+=static_cast<u32>(request.data.size());
  }
  if(!bus_.store(request.chain.back().address,8,request.status)) {fail();return;}
  complete(request.queue,request.head,written);
}
VirtioNet::VirtioNet(memory::AddressSpace& bus) : VirtioMmio(bus, 1, 2, 3 | (1ull << 5) | (1ull << 16)) {}
bool VirtioNet::config_read(u64 o, unsigned w, u64& v) {
  const std::array<u8, 12> data{0x52, 0x54, 0x00, 0x12, 0x34, 0x56, 1, 0};
  if ((w != 8 && w != 16 && w != 32 && w != 64) || o >= data.size() || w / 8 > data.size() - o) return false;
  v = 0; std::memcpy(&v, data.data() + o, w / 8); return true;
}
void VirtioNet::notify(unsigned index) {
  if (index == 0) { deliver(); return; }
  u16 head;
  std::vector<Descriptor> chain;
  while (take(1, head, chain)) {
    std::vector<u8> frame;
    for (const auto& d : chain) {
      if ((d.flags & 2) || frame.size() + d.length > 65548) { fail(); return; }
      const auto begin=frame.size();frame.resize(begin+d.length);
      if(!bus_.read_bytes(d.address,{frame.data()+begin,d.length})) {fail();return;}
    }
    if (frame.size() >= 12 + 14 && frame[1] == 0 && !(frame[0]&~1u)) {
      bool valid=true;
      if(frame[0]&1) {
        const std::size_t start=static_cast<unsigned>(frame[6])|(static_cast<unsigned>(frame[7])<<8);
        const std::size_t offset=static_cast<unsigned>(frame[8])|(static_cast<unsigned>(frame[9])<<8);
        const std::size_t size=frame.size()-12;
        if(!negotiated(1) || start>size || offset>size-start || size-start-offset<2) valid=false;
        else {
          u32 sum=0;
          for(std::size_t n=12+start;n<frame.size();n+=2) {
            sum+=static_cast<u32>(frame[n])<<8;
            if(n+1<frame.size())sum+=frame[n+1];
          }
          while(sum>>16)sum=(sum&65535)+(sum>>16);
          const auto checksum=static_cast<u16>(~sum);
          frame[12+start+offset]=static_cast<u8>(checksum>>8);
          frame[13+start+offset]=static_cast<u8>(checksum);
        }
      }
      if(valid)network_.send({frame.data() + 12, frame.size() - 12});
    }
    complete(1, head, 0);
  }
  poll();
}
void VirtioNet::poll() {
  network_.poll();
  std::vector<u8> frame;
  while (network_.receive(frame)) {
    if (packets_.size() < 4096) packets_.push_back(std::move(frame));
  }
  deliver();
}
void VirtioNet::deliver() {
  u16 head;
  std::vector<Descriptor> chain;
  while (!packets_.empty() && take(0, head, chain)) {
    std::vector<u8> bytes(12, 0);
    if(negotiated(2))bytes[0]=2; // all received frames are generated by the SLIRP stack
    bytes.insert(bytes.end(), packets_.front().begin(), packets_.front().end());
    packets_.pop_front();
    std::size_t done = 0;
    for (const auto& d : chain) {
      if (!(d.flags & 2)) { fail(); return; }
      const auto length = std::min<std::size_t>(d.length, bytes.size() - done);
      if (length) bus_.write_bytes(d.address, {bytes.data() + done, length});
      done += length;
    }
    if (done != bytes.size()) { complete(0, head, 0); continue; }
    complete(0, head, static_cast<u32>(done));
  }
}
} // namespace rvvm::devices
