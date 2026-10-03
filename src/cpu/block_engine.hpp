// SPDX-License-Identifier: MIT
#pragma once
#include "cpu/execute.hpp"
#include "cpu/jit_x64.hpp"
#include <array>
#include <memory>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <deque>
#include <atomic>
#include <functional>
namespace rvvm::cpu {
class JitCompilerPool {
public:
  explicit JitCompilerPool(u32 workers);
  ~JitCompilerPool();
  bool submit(std::span<const CachedInstruction>,const std::atomic<bool>*,std::function<void(std::unique_ptr<NativeBlock>)>);
private:
  std::mutex mutex_;std::condition_variable wake_;
  struct Job {std::span<const CachedInstruction> instructions;const std::atomic<bool>* resident;std::function<void(std::unique_ptr<NativeBlock>)> complete;};
  std::deque<Job> jobs_;std::vector<std::thread> threads_;bool stopping_=false;
};
class BlockEngine {
public:
  explicit BlockEngine(u32 workers=0);
  explicit BlockEngine(std::shared_ptr<JitCompilerPool> pool):compiler_pool_(std::move(pool)) {}
  ~BlockEngine();
  struct Result { bool handled = false, trapped = false; u32 retired = 0; bool barrier = false; };
  Result run(Hart&, const memory::AddressSpace&, Trap&, u32 limit = 32, bool jit = false);
  Result run_batch(Hart&, const memory::AddressSpace&, Trap&, u32 limit, bool jit);
  u64 native_compiled() const noexcept {return native_compiled_;}
  u64 linked_blocks() const noexcept {return linked_blocks_;}
  // These counters belong to the executing hart; compiler workers never touch
  // them, so profiling adds no shared atomics to instruction execution.
  struct Statistics {
    u64 dispatches=0,decoded=0,evicted=0,invalidated=0,revalidated=0;
    u64 native_instructions=0,cold_instructions=0,queued=0;
    u64 link_hits=0,link_misses=0,self_loops=0;
    u64 slow_link_hits=0,link_exits=0,budget_exits=0,boundary_exits=0;
    u64 direct_link_hits=0;
    u64 tail_instructions=0,pending_instructions=0,warming_instructions=0;
    u64 timing_samples=0,native_samples=0,cold_samples=0;
    u64 lookup_ticks=0,native_ticks=0,cold_ticks=0,resolver_ticks=0,resolver_samples=0;
    u64 timer_call_ticks=0;
    u64 sampled_native_instructions=0,sampled_helper_calls=0;
    u64 sampled_inline_loads=0;
  };
  const Statistics& statistics() const noexcept {return statistics_;}
  void report_statistics(u32 hart_id) const;
private:
  struct Context {
    const Hart* hart=nullptr;u64 satp=0,epoch=0,status=0;Privilege privilege=Privilege::Machine;
    bool operator==(const Context&) const=default;
  };
  static Context context(const Hart& h) {
    return {&h,h.csr.raw(kCsrSatp),h.translation_epoch,h.csr.raw(kCsrMstatus)&((1ull<<17)|(1ull<<18)|(1ull<<19)|(3ull<<11)),h.privilege};
  }
  struct Block {
    u64 address = 0;
    u64 generation = 0;
    // Hot scalar fields first: every C++->native transition reads them.
    u32 bytes = 0, count = 0, visits = 0;
    bool queued=false,barrier=false,direct_exit=false;
    std::atomic<bool> resident{true};
    std::unique_ptr<NativeBlock> native;
    NativeLinkNode native_link;
    const u64* residency_epoch=nullptr;
    std::array<u8,128> source{};
    std::array<CachedInstruction,32> instructions{};
    // Keep decoded operands dense for native helpers; cold execution alone
    // reads this separate table of preselected C++ functions.
    std::array<CachedInstruction::Executor,32> executors{};
    unsigned replacement=0;
  };
  // No eviction occurs inside a native chain. Each cache set has its own epoch,
  // checked before dereferencing a target pointer. Evicting one set therefore
  // cannot invalidate links to all the other sets in a large application.
  struct ChainState {BlockEngine* owner;u32 limit,retired=0;bool trapped=false,barrier=false;Block* current=nullptr;};
  static const void* resolve_chain(NativeChain*,Hart*,const memory::AddressSpace*,Trap*,u32);
  void record_link(Block&,const Hart&,Block*,u64 source_pc);
  Block* find_link(Block&,Hart&,const memory::AddressSpace&,Trap&,u32,u64 source_pc);
  Context fast_context_;u64 context_token_=0;
  struct ContextTag {Context context;u64 token=0;};
  std::array<ContextTag,16> context_tags_{};
  unsigned context_replacement_=0;u64 next_context_token_=0;
  static void publish_native(Block&);
  void select_context(const Context&);
  Block* last_block_=nullptr;u64 last_block_epoch_=0,last_block_pc_=0;const u64* last_block_epoch_counter_=nullptr;Context last_context_;
  // Virtual-PC hits avoid a repeated fetch translation and physical hash scan.
  // Slot and epoch-counter addresses are stable; the set epoch is checked
  // before dereferencing its potentially replaced shared_ptr.
  struct Dispatch {u64 pc=~0ull,context=0,epoch=0;std::shared_ptr<Block>* slot=nullptr;const memory::AddressSpace* bus=nullptr;const u64* epoch_counter=nullptr;};
  std::array<Dispatch,4096> dispatch_{};
  static std::size_t dispatch_index(u64 pc) noexcept {return static_cast<std::size_t>(((pc>>1)^(pc>>12))&4095);}
  // Linux has a much larger instruction working set than small bare-metal
  // loops. Heap storage keeps this cache out of the Windows thread stack.
  struct Set {std::array<std::shared_ptr<Block>,4> ways;unsigned replacement=0;u64 epoch=0;};
  std::vector<Set> cache_{16384};
  struct Completed {std::shared_ptr<Block> block;std::unique_ptr<NativeBlock> code;};
  struct CompilerState {std::mutex mutex;std::vector<Completed> completed;std::atomic<bool> ready{false};};
  std::shared_ptr<CompilerState> compiler_state_=std::make_shared<CompilerState>();
  std::shared_ptr<JitCompilerPool> compiler_pool_;
  u64 native_compiled_=0;
  u64 linked_blocks_=0;
  bool chaining_enabled_=false;
  Statistics statistics_{};
  static bool profiling_requested() noexcept;
  static bool direct_links_requested() noexcept;
  static bool inline_loads_requested() noexcept;
  static bool inline_stores_requested() noexcept;
  bool direct_links_enabled_=direct_links_requested();
  bool inline_loads_enabled_=inline_loads_requested();
  bool inline_stores_enabled_=inline_stores_requested();
  bool profiling_=profiling_requested(),profile_sample_=false;
  struct RunSample {
    BlockEngine& owner;bool active;u64 started=0;unsigned stage=0;
    RunSample(BlockEngine&,bool);
    void execute(bool native);
    ~RunSample();
  };
  void collect_compiled();
  void schedule(const std::shared_ptr<Block>&);
  bool validate_source(Block&,const memory::AddressSpace&,u64 generation);
};
}
