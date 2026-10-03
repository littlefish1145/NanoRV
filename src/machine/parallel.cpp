// SPDX-License-Identifier: MIT
#include "machine/machine.hpp"
#include "machine/parallel_clock.hpp"
#include "cpu/privileged.hpp"
#include "core/log.hpp"
#include "platform/platform.hpp"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <algorithm>
#include <cstring>
#include <barrier>

namespace rvvm::machine {
namespace {
// Devices (including WGL/VirGL) remain owned by the calling I/O thread. CPU
// workers synchronously enqueue MMIO; unrelated CPU execution is never locked.
class DeviceQueue {
  struct Request {
    memory::MemoryRegion* region;u64 offset,value;unsigned width;bool store,done=false,result=false;
    std::condition_variable wake;
  };
  std::mutex mutex_;platform::WakeEvent wake_;std::deque<Request*> requests_;
  const std::thread::id owner_=std::this_thread::get_id();
  devices::Uart* uart_;
public:
  explicit DeviceQueue(devices::Uart& uart):uart_(&uart) {}
  static bool dispatch(void* state,memory::MemoryRegion* region,u64 offset,unsigned width,u64& value,bool store) {
    auto& q=*static_cast<DeviceQueue*>(state);
    // The UART owns its lock and buffers TX. Register accesses need no
    // synchronous trip to the I/O thread for each character or LSR read.
    if(region==q.uart_)return store?region->store(offset,width,value):region->load(offset,width,value);
    if(std::this_thread::get_id()==q.owner_)return store?region->store(offset,width,value):region->load(offset,width,value);
    Request request{region,offset,value,width,store};
    std::unique_lock lock(q.mutex_);q.requests_.push_back(&request);q.wake_.notify();
    request.wake.wait(lock,[&]{return request.done;});value=request.value;return request.result;
  }
  bool drain() {
    // A producer can refill the queue continuously. Yield to DMA completions,
    // network delivery and IRQ publication after a bounded amount of MMIO.
    for(unsigned count=0;count<64;++count) {
      Request* request;
      {std::lock_guard lock(mutex_);if(requests_.empty())return false;request=requests_.front();requests_.pop_front();}
      const bool result=request->store?request->region->store(request->offset,request->width,request->value):request->region->load(request->offset,request->width,request->value);
      {std::lock_guard lock(mutex_);request->result=result;request->done=true;request->wake.notify_one();}
    }
    std::lock_guard lock(mutex_);return !requests_.empty();
  }
  void notify() {wake_.notify();}
  void wait() {wake_.wait_ms(1);}
};
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable:4324) // Per-vCPU progress deliberately occupies a cache line.
#endif
struct alignas(64) CpuProgress {
  std::atomic<u64> instructions{0},attempts{0},pending{0};std::atomic<bool> idle{false};
};
#ifdef _MSC_VER
#pragma warning(pop)
#endif
}
RunResult Machine::run_parallel() {
  RunResult result;result.hart_cpu_ns.resize(hart_count());result.hart_host_ids.resize(hart_count());
  const u64 started=platform::monotonic_ns();
  RVVM_INFO("SMP: %u independent vCPU threads, %u shared JIT compiler threads",hart_count(),config_.engine==EngineKind::Jit?config_.jit_workers:0);
  for(;;) {
    DeviceQueue devices(uart_);
    if(config_.network)net_.set_receive_wake([&devices]{devices.notify();});
    block_.set_io_wake([&devices]{devices.notify();});
    struct DetachDeviceWake {
      devices::VirtioNet& net;
      devices::VirtioBlock& block;
      ~DetachDeviceWake() {net.set_receive_wake({});block.set_io_wake({});}
    } detach_device_wake{net_,block_};
    memory_.set_concurrent(true);memory_.set_mmio_dispatch(&devices,DeviceQueue::dispatch);
    auto progress=std::make_unique<CpuProgress[]>(hart_count());
    auto cpu_wake=std::make_unique<platform::WakeEvent[]>(hart_count());
    std::vector<u64> compiled(hart_count()),linked(hart_count());
    std::atomic<bool> stop{false};std::atomic<u32> finished{0};std::atomic<u64> ticks{clint_.time};
    std::atomic<u64> remaining{config_.max_instructions?config_.max_instructions-result.instructions:~0ull};
    std::mutex failure_mutex;std::vector<std::thread> workers;
    std::barrier start_harts(static_cast<std::ptrdiff_t>(hart_count()));
    std::shared_ptr<cpu::JitCompilerPool> compilers;
    if(config_.engine==EngineKind::Jit && config_.jit_workers)compilers=std::make_shared<cpu::JitCompilerPool>(config_.jit_workers);
    for(u32 index=0;index<hart_count();++index)workers.emplace_back([&,index] {
      platform::prefer_performance_core(index);
      const auto cpu_started=platform::thread_cpu_ns();
      result.hart_host_ids[index]=platform::thread_id();
      bool joined_start=false;
      try {
        cpu::BlockEngine engine(compilers);
        start_harts.arrive_and_wait();joined_start=true;
        auto& h=harts_[index];u64 count=0,attempts=0;
        while(!stop.load(std::memory_order_acquire)) {
          cpu::Trap trap{};
          if(config_.linux_boot) {
            h.csr.ticks=ticks.load(std::memory_order_acquire);
            h.csr.set_raw(cpu::kCsrMip,(h.csr.raw(cpu::kCsrMip)&~0xa88ull)|progress[index].pending.load(std::memory_order_acquire));
            if(cpu::pending_interrupt(h,trap)) {cpu::enter_trap(h,trap);++attempts;continue;}
            if(h.waiting && (h.csr.raw(cpu::kCsrMip)&h.csr.raw(cpu::kCsrMie)))h.waiting=false;
          }
          progress[index].idle.store(h.waiting,std::memory_order_release);
          if(h.waiting) {
            progress[index].attempts.store(attempts,std::memory_order_relaxed);
            // IRQ publication signals this hart immediately. A bounded timeout
            // covers shutdown and hosts lacking precise waitable timers.
            cpu_wake[index].wait_ms(10);continue;
          }
          const u32 batch_limit=config_.engine==EngineKind::Jit?2048u:256u;
          u32 budget=batch_limit;
          if(config_.max_instructions) {
            auto available=remaining.load(std::memory_order_relaxed);
            do {budget=static_cast<u32>(std::min<u64>(batch_limit,available));if(!budget)break;}
            while(!remaining.compare_exchange_weak(available,available-budget,std::memory_order_acq_rel));
            if(!budget)break;
          }
          cpu::BlockEngine::Result block{};
          if(config_.engine!=EngineKind::Interpreter)block=engine.run_batch(h,memory_,trap,budget,config_.engine==EngineKind::Jit);
          if(!block.handled) {
            cpu::Decoded instruction;
            block.handled=true;
            if(!cpu::fetch(h,memory_,instruction,trap))block.trapped=true;
            else {
              h.next_pc=h.pc+instruction.length;
              if(!cpu::execute(h,memory_,instruction,trap))block.trapped=true;
              else {h.pc=h.next_pc;block.retired=1;}
            }
          }
          count+=block.retired;attempts+=std::max(1u,block.retired);
          progress[index].instructions.store(count,std::memory_order_relaxed);
          progress[index].attempts.store(attempts,std::memory_order_relaxed);
          if(config_.max_instructions && block.retired<budget)remaining.fetch_add(budget-block.retired,std::memory_order_release);
          if(block.trapped) {
            if(config_.linux_boot)cpu::enter_trap(h,trap);
            else {std::lock_guard lock(failure_mutex);result.reason=StopReason::Trap;result.trapped=true;result.trap=trap;stop.store(true,std::memory_order_release);}
          }
        }
        compiled[index]=engine.native_compiled();linked[index]=engine.linked_blocks();
        if(config_.engine==EngineKind::Jit)engine.report_statistics(index);
      } catch(const std::exception& e) {
        if(!joined_start)start_harts.arrive_and_drop();
        std::lock_guard lock(failure_mutex);RVVM_ERROR("vCPU %u failed: %s",index,e.what());result.exit_code=1;stop.store(true,std::memory_order_release);
      }
      result.hart_cpu_ns[index]+=platform::thread_cpu_ns()-cpu_started;
      finished.fetch_add(1,std::memory_order_release);
    });
    ParallelClock clock(hart_count(),clint_.time);
    std::vector<u64> sampled(hart_count());
    while(finished.load(std::memory_order_acquire)!=hart_count()) {
      const bool mmio_pending=devices.drain();block_.poll();uart_.poll();
      if(config_.network)net_.poll();
      if(config_.graphics) {auto events=display_.events();input_.push(events);}
      if(config_.linux_boot) {
        bool all_idle=true;
        for(u32 n=0;n<hart_count();++n) {sampled[n]=progress[n].attempts.load(std::memory_order_relaxed);all_idle&=progress[n].idle.load(std::memory_order_acquire);}
        if(config_.realtime_clock)clint_.time=(platform::monotonic_ns()-clock_origin_ns_)/100;
        else {
          clint_.time=clock.advance(sampled,all_idle);
        }
        plic_.set_irq(1,block_.interrupt());plic_.set_irq(2,net_.interrupt());plic_.set_irq(10,uart_.interrupt());plic_.set_irq(11,rtc_.interrupt());
        if(config_.graphics) {plic_.set_irq(3,gpu_.interrupt());plic_.set_irq(4,input_.interrupt());}
        ticks.store(clint_.time,std::memory_order_release);
        for(u32 n=0;n<hart_count();++n) {
          const auto pending=clint_.pending(n)|plic_.pending(n);
          const auto previous=progress[n].pending.exchange(pending,std::memory_order_acq_rel);
          if(pending && (pending!=previous || progress[n].idle.load(std::memory_order_acquire)))cpu_wake[n].notify();
        }
      }
      if(test_.stopped)stop.store(true,std::memory_order_release);
      if(stop.load(std::memory_order_acquire))for(u32 n=0;n<hart_count();++n)cpu_wake[n].notify();
      if(!mmio_pending)devices.wait();
    }
    while(devices.drain()) {}for(auto& worker:workers)worker.join();
    memory_.set_mmio_dispatch(nullptr,nullptr);memory_.set_concurrent(false);
    for(u32 n=0;n<hart_count();++n) {result.instructions+=progress[n].instructions.load();result.native_blocks+=compiled[n];result.linked_blocks+=linked[n];}
    if(test_.stopped && (test_.code&0xffff)==0x7777 && config_.linux_boot && !result.exit_code) {
      RVVM_INFO("RVVM: parallel guest requested reboot");
      uart_.reset();clint_=devices::Clint{};plic_=devices::Plic{};rtc_=devices::GoldfishRtc{};test_=devices::TestDevice{};
      block_.store(0x70,32,0);net_.store(0x70,32,0);gpu_.store(0x70,32,0);input_.store(0x70,32,0);
      attempts_=uart_poll_at_=plic_poll_at_=net_poll_at_=0;
      std::memset(ram_->host_data(),0,static_cast<std::size_t>(config_.ram_size));
      auto reloaded=load_image();if(!reloaded) {RVVM_ERROR("reboot: %s",reloaded.error().c_str());result.exit_code=1;break;}
      continue;
    }
    if(test_.stopped && (test_.code&0xffff)==0x3333)result.exit_code=1;
    if(config_.max_instructions && result.instructions>=config_.max_instructions)result.reason=StopReason::InstructionLimit;
    break;
  }
  if(config_.graphics && !config_.screenshot_path.empty()) {std::string error;if(!display_.save(config_.screenshot_path,error)) {RVVM_ERROR("screenshot: %s",error.c_str());result.exit_code=1;}}
  block_.set_async(false);
  uart_.flush();
  result.elapsed_ns=platform::monotonic_ns()-started;
  u64 cpu_total=0;
  for(u32 n=0;n<hart_count();++n) {cpu_total+=result.hart_cpu_ns[n];RVVM_INFO("vCPU %u: %.3f host CPU seconds",n,result.hart_cpu_ns[n]/1e9);}
  RVVM_INFO("vCPU concurrency: %.2f host CPU cores (excludes compiler and I/O threads)",result.elapsed_ns?static_cast<double>(cpu_total)/result.elapsed_ns:0.0);
  RVVM_INFO("JIT: %llu native blocks, %llu native block links",static_cast<unsigned long long>(result.native_blocks),static_cast<unsigned long long>(result.linked_blocks));
  return result;
}
}
