// SPDX-License-Identifier: MIT
#include "cpu/block_engine.hpp"
#include "cpu/compressed.hpp"
#include "mmu/mmu.hpp"
#include "core/log.hpp"
#include "platform/platform.hpp"
#include <algorithm>
#include <cstring>
#include <cstdlib>
namespace nanorv::cpu {
bool BlockEngine::profiling_requested() noexcept {
  static const bool enabled=[] {const auto* setting=std::getenv("NanoRV_PROFILE");return setting && std::strcmp(setting,"1")==0;}();
  return enabled;
}
bool BlockEngine::direct_links_requested() noexcept {
  static const bool enabled=[] {const auto* setting=std::getenv("NanoRV_DIRECT_LINKS");return !setting || std::strcmp(setting,"0")!=0;}();
  return enabled;
}
bool BlockEngine::inline_loads_requested() noexcept {
  static const bool enabled=[] {const auto* setting=std::getenv("NanoRV_INLINE_LOADS");return !setting || std::strcmp(setting,"0")!=0;}();
  return enabled;
}
bool BlockEngine::inline_stores_requested() noexcept {
  static const bool enabled=[] {const auto* setting=std::getenv("NanoRV_INLINE_STORES");return !setting || std::strcmp(setting,"0")!=0;}();
  return enabled;
}
BlockEngine::RunSample::RunSample(BlockEngine& engine,bool enabled):owner(engine),active(enabled) {
  owner.profile_sample_=active;
  if(active) {
    if(!owner.statistics_.timer_call_ticks) {
      // Calibrate only the opt-in profile. A median rejects an occasional
      // scheduler interruption; the parser subtracts this measured timestamp
      // cost before interpreting nested resolver/execution sample fractions.
      std::array<u64,5> costs{};u64 sink=0;
      for(auto& cost:costs) {
        const auto begin=platform::profiling_ticks();
        for(unsigned n=0;n<20000;++n)sink^=platform::profiling_ticks();
        cost=(platform::profiling_ticks()-begin)/20000;
      }
      std::sort(costs.begin(),costs.end());owner.statistics_.timer_call_ticks=costs[2];
      if(!sink)std::atomic_signal_fence(std::memory_order_seq_cst);
    }
    started=platform::profiling_ticks();++owner.statistics_.timing_samples;
  }
}
void BlockEngine::RunSample::execute(bool native) {
  if(!active)return;
  const auto now=platform::profiling_ticks();owner.statistics_.lookup_ticks+=now-started;started=now;
  stage=native?1u:2u;
  if(native)++owner.statistics_.native_samples;else ++owner.statistics_.cold_samples;
}
BlockEngine::RunSample::~RunSample() {
  if(active) {
    const auto elapsed=platform::profiling_ticks()-started;
    if(stage==1)owner.statistics_.native_ticks+=elapsed;
    else if(stage==2)owner.statistics_.cold_ticks+=elapsed;
    else owner.statistics_.lookup_ticks+=elapsed;
  }
  owner.profile_sample_=false;
}
JitCompilerPool::JitCompilerPool(u32 workers) {
  for(u32 n=0;n<workers;++n)threads_.emplace_back([this] {
    for(;;) {
      std::vector<Job> batch;
      {std::unique_lock lock(mutex_);wake_.wait(lock,[&]{return stopping_ || !jobs_.empty();});
        if(stopping_)return;
        const auto count=std::min<std::size_t>(16,jobs_.size());batch.reserve(count);
        for(std::size_t n=0;n<count;++n) {batch.push_back(std::move(jobs_.front()));jobs_.pop_front();}}
      std::vector<std::span<const CachedInstruction>> spans;spans.reserve(batch.size());
      for(const auto& job:batch)spans.push_back(job.resident->load(std::memory_order_acquire)?job.instructions:std::span<const CachedInstruction>{});
      auto code=NativeBlock::compile_many(spans);
      // All code pages are RX before any completion becomes visible. Draining
      // existing work adds no timed batching delay to cold-code startup.
      for(std::size_t n=0;n<batch.size();++n)batch[n].complete(std::move(code[n]));
    }
  });
}
JitCompilerPool::~JitCompilerPool() {
  {std::lock_guard lock(mutex_);stopping_=true;}wake_.notify_all();
  for(auto& worker:threads_)worker.join();
}
bool JitCompilerPool::submit(std::span<const CachedInstruction> instructions,const std::atomic<bool>* resident,std::function<void(std::unique_ptr<NativeBlock>)> complete) {
  {std::lock_guard lock(mutex_);if(stopping_ || jobs_.size()>=8192)return false;jobs_.push_back({instructions,resident,std::move(complete)});}
  wake_.notify_one();return true;
}
BlockEngine::BlockEngine(u32 workers) {if(workers)compiler_pool_=std::make_shared<JitCompilerPool>(workers);}
BlockEngine::~BlockEngine() {for(auto& set:cache_)for(auto& block:set.ways)if(block)block->resident=false;}
void BlockEngine::schedule(const std::shared_ptr<Block>& block) {
  auto state=compiler_state_;
  if(compiler_pool_->submit({block->instructions.data(),block->count},&block->resident,[block,state](std::unique_ptr<NativeBlock> code) {
    {std::lock_guard lock(state->mutex);state->completed.push_back({block,std::move(code)});state->ready.store(true,std::memory_order_release);}
  })) {block->queued=true;++statistics_.queued;}
}
void BlockEngine::collect_compiled() {
  // No completion is the overwhelmingly common case. Avoid a locked exchange
  // on every dispatch; the worker's release store pairs with the acquire below.
  if(!compiler_state_->ready.load(std::memory_order_acquire))return;
  if(!compiler_state_->ready.exchange(false,std::memory_order_acquire))return;
  std::vector<Completed> completed;
  {std::lock_guard lock(compiler_state_->mutex);completed.swap(compiler_state_->completed);}
  for(auto& item:completed) {if(!item.block->resident.load(std::memory_order_acquire))continue;item.block->native=std::move(item.code);item.block->queued=false;if(item.block->native) {publish_native(*item.block);++native_compiled_;}}
}
void BlockEngine::publish_native(Block& block) {
  block.native_link.block=&block;
  block.native_link.entry=block.native->linked_entry();
  block.native_link.count=block.count;
  block.native_link.expected_generation=block.generation;
}
void BlockEngine::select_context(const Context& current) {
  if(current==fast_context_)return;
  fast_context_=current;context_token_=0;
  for(const auto& tag:context_tags_)if(tag.token && tag.context==current) {context_token_=tag.token;break;}
  if(!context_token_) {
    context_token_=++next_context_token_;
    context_tags_[context_replacement_++%context_tags_.size()]={current,context_token_};
  }
}
bool BlockEngine::validate_source(Block& block,const memory::AddressSpace& bus,u64 generation) {
  if(block.generation==generation)return true;
  // Writable code pages often contain unrelated GOT/data/other functions.
  // Keep decoded operands and native code when this block's exact bytes have
  // not changed. Read through the shared atomic RAM API, then verify a stable
  // generation before comparing the two immutable snapshots.
  std::array<u8,128> snapshot{};
  if(!bus.read_bytes(block.address,{snapshot.data(),block.bytes}) ||
      bus.code_generation(block.address)!=generation ||
      !platform::code_bytes_equal(block.source.data(),snapshot.data(),block.bytes))return false;
  block.generation=generation;block.native_link.expected_generation=generation;++statistics_.revalidated;return true;
}
BlockEngine::Result BlockEngine::run(Hart& hart, const memory::AddressSpace& bus,
                                    Trap& trap, u32 limit, bool jit) {
  if(jit)collect_compiled();
  Result result{};
  ++statistics_.dispatches;
  RunSample sample(*this,profiling_ && ((statistics_.dispatches+static_cast<u64>(hart.hart_id)*131)&4095)==0);
  if (!limit || (hart.pc & 1) || (hart.pc & 4095) > 4092) return result;
  const auto current_context=context(hart);
  if(jit)select_context(current_context);
  auto& dispatch=dispatch_[dispatch_index(hart.pc)];
  std::shared_ptr<Block>* cached_slot=nullptr;
  u64 physical=0;
  if(jit && hart.translation_cache_enabled && dispatch.slot && dispatch.pc==hart.pc &&
      dispatch.context==context_token_ && dispatch.epoch_counter &&
      dispatch.epoch==*dispatch.epoch_counter && dispatch.bus==&bus) {
    cached_slot=dispatch.slot;physical=(*cached_slot)->address;
  } else {
    if (!mmu::translate(hart,bus,hart.pc & ~4095ull,4096*8,memory::Access::Fetch,physical,trap))
      return result; // the reference fetch decides faults at the actual instruction
    if (!bus.direct(physical,4096,memory::Access::Fetch)) return result;
    physical += hart.pc & 4095;
  }
  const auto available = static_cast<u32>(4096-(hart.pc&4095));
  const auto generation=bus.code_generation(physical);
  auto& set = cache_[((physical >> 1) ^ (physical >> 13) ^ (physical >> 25)) & (cache_.size()-1)];
  unsigned way=4;
  if(cached_slot)way=static_cast<unsigned>(cached_slot-set.ways.data());
  else for(unsigned n=0;n<4;++n)if(set.ways[n] && set.ways[n]->address==physical) {way=n;break;}
  if(way==4) {
    for(unsigned n=0;n<4;++n)if(!set.ways[n]) {way=n;break;}
    if(way==4)way=set.replacement;
  }
  set.replacement=(way+1)%4;
  auto& slot=set.ways[way];
  if (!slot || slot->address != physical || slot->bytes > available ||
      !validate_source(*slot,bus,generation)) {
    if(slot) {
      if(slot->address==physical)++statistics_.invalidated;else ++statistics_.evicted;
      slot->resident.store(false,std::memory_order_release);++set.epoch;
    }
    ++statistics_.decoded;
    slot = std::make_shared<Block>();
    auto& block = *slot;
    block.address = physical;
    block.residency_epoch=&set.epoch;
    block.generation=generation;
    block.native_link.generation=bus.code_generation_counter(physical);
    if(!bus.read_bytes(physical,{block.source.data(),std::min<std::size_t>(available,block.source.size())}))return result;
    while (block.count < 32 && block.bytes+2 <= available && block.bytes+4 <= block.source.size()) {
      u16 low; std::memcpy(&low,block.source.data()+block.bytes,2);
      const auto length=instruction_length(low);
      if (block.bytes+length > available) break;
      u32 raw=low;
      if(length==4)std::memcpy(&raw,block.source.data()+block.bytes,4);
      auto decoded=decode(raw);
      if(decoded.compressed) {
        if(const auto expanded=expand_compressed(low)) {
          decoded=decode(expanded);decoded.length=2;
        }
      }
      block.executors[block.count]=NativeBlock::executor_for(decoded);
      block.instructions[block.count++] = {decoded,raw};
      block.bytes += length;
      if(decoded.opcode==0x63 || decoded.opcode==0x67 || decoded.opcode==0x6f ||
         decoded.opcode==0x73 || decoded.opcode==0x0f || decoded.format==InsnFormat::Unknown)break;
    }
    if (!block.count) return result;
    const auto last_opcode=block.instructions[block.count-1].decoded.opcode;
    block.barrier=last_opcode==0x73 || last_opcode==0x0f;
    block.direct_exit=last_opcode!=0x67 && !block.barrier &&
      block.instructions[block.count-1].decoded.format!=InsnFormat::Unknown;
    if (bus.code_generation(physical)!=generation) {slot->resident=false;++set.epoch;slot.reset();return result;}
  }
  result.handled = true;
  if(jit) {
    dispatch={hart.pc,context_token_,set.epoch,&slot,&bus,&set.epoch};
    if(last_block_ && last_block_epoch_counter_ && last_block_epoch_==*last_block_epoch_counter_ &&
        last_context_==current_context)record_link(*last_block_,hart,slot.get(),last_block_pc_);
    last_block_=slot.get();last_block_epoch_=set.epoch;last_block_epoch_counter_=&set.epoch;
    last_block_pc_=hart.pc;last_context_=current_context;
  }
  auto& block = *slot;
  result.barrier=block.barrier;
  // One-time startup code is cheaper to interpret than to allocate/protect a
  // native page. Compile only blocks that actually recur.
  ++block.visits;
  const unsigned hot_threshold=compiler_pool_?16u:4u;
  if (jit && block.visits >= hot_threshold && limit >= block.count) {
    if (!block.native) {
      if(!compiler_pool_) {block.native=NativeBlock::compile({block.instructions.data(),block.count});if(block.native) {publish_native(block);++native_compiled_;}}
      else if(!block.queued && (block.visits&15)==0)schedule(slot);
    }
    if (block.native) {
      sample.execute(true);
      ChainState state{this,chaining_enabled_?limit:block.count,0,false,false,slot.get()};NativeChain chain{resolve_chain,&state};
      chain.generation=bus.code_generation_counter(block.address);
      chain.expected_generation=block.generation;chain.remaining=state.limit;
      block.native_link.generation=chain.generation;
      chain.current=&block.native_link;chain.current_pc=hart.pc;chain.context=context_token_;chain.sampling=profile_sample_;
      chain.direct_enabled=direct_links_enabled_;
      chain.inline_loads=inline_loads_enabled_;
      chain.inline_stores=inline_stores_enabled_;
      block.native->run(hart,bus,trap,&chain);
      statistics_.link_hits+=chain.direct_links;linked_blocks_+=chain.direct_links;
      statistics_.direct_link_hits+=chain.direct_links;
      result.trapped=state.trapped;result.retired=state.retired;result.barrier=state.barrier;
      statistics_.native_instructions+=state.retired;
      last_block_=state.current;last_block_epoch_counter_=state.current->residency_epoch;
      last_block_epoch_=*last_block_epoch_counter_;
      last_block_pc_=chain.current_pc;
      last_context_=context(hart);
      hart.next_pc=hart.pc;return result;
    }
  }
  sample.execute(false);
  for (u32 n=0;n<std::min(block.count,limit);++n) {
    const auto& instruction = block.instructions[n];
    if (!block.executors[n](&hart,&bus,&instruction,&trap)) {
      if (trap.cause == TrapCause::IllegalInstruction) trap.tval = instruction.original;
      result.trapped = true; return result;
    }
    ++result.retired;
    ++statistics_.cold_instructions;
    if(block.native && limit<block.count)++statistics_.tail_instructions;
    else if(block.queued)++statistics_.pending_instructions;
    else ++statistics_.warming_instructions;
  }
  hart.next_pc = hart.pc;
  return result;
}
void BlockEngine::record_link(Block& source,const Hart& hart,Block* target,u64 source_pc) {
  unsigned index=2;
  for(unsigned n=0;n<2;++n)
    if(source.native_link.next[n].pc==hart.pc &&
       (source.native_link.next[n].context&~NativeLinkNode::kSamePageContext)==context_token_) {index=n;break;}
  if(index==2)index=source.replacement++&1;
  const auto* counter=target?target->residency_epoch:nullptr;
  const auto epoch=counter?*counter:0;
  // QEMU's goto_tb is restricted to direct, same-page successors. The source
  // page has already passed whole-page fetch/PMP validation for this chain.
  const bool same_page=target && source.direct_exit && (source.address>>12)==(target->address>>12) &&
    (source_pc>>12)==(hart.pc>>12);
  const auto context=context_token_|(same_page?NativeLinkNode::kSamePageContext:0);
  source.native_link.next[index]={hart.pc,epoch,context,target?&target->native_link:nullptr,counter};
}
BlockEngine::Block* BlockEngine::find_link(Block& source,Hart& hart,const memory::AddressSpace& bus,Trap& trap,u32 remaining,u64 source_pc) {
  // A chain exits at every SYSTEM/FENCE/trap boundary. Ordinary instructions
  // cannot change SATP, effective privilege or translation_epoch, so the
  // dispatcher's validated context remains valid throughout this chain.
  // Share the native descriptor table. The context token represents the full
  // dispatch context; do not copy/compare a second five-field table per exit.
  for(auto& link:source.native_link.next)if(link.pc==hart.pc &&
      ((link.context&~NativeLinkNode::kSamePageContext)==context_token_ ||
       ((link.context&NativeLinkNode::kSamePageContext) && (source_pc>>12)==(hart.pc>>12))) &&
      link.epoch_counter && link.epoch==*link.epoch_counter) {
    auto* target=link.target?static_cast<Block*>(link.target->block):nullptr;
    if(target && target->native && target->count<=remaining && validate_source(*target,bus,bus.code_generation(target->address))) {++statistics_.link_hits;return target;}
  }
  ++statistics_.link_misses;
  if((hart.pc&1) || (hart.pc&4095)>4092) {++statistics_.link_exits;return {};}
  const auto& dispatch=dispatch_[dispatch_index(hart.pc)];
  if(hart.translation_cache_enabled && dispatch.slot && dispatch.pc==hart.pc &&
      dispatch.context==context_token_ && dispatch.epoch_counter &&
      dispatch.epoch==*dispatch.epoch_counter && dispatch.bus==&bus) {
    auto* target=dispatch.slot->get();
    if(target->native && target->count<=remaining && validate_source(*target,bus,bus.code_generation(target->address))) {
      ++statistics_.slow_link_hits;record_link(source,hart,target,source_pc);return target;
    }
  }
  u64 physical;
  // Speculative successor lookup must never manufacture a fetch trap. A miss
  // returns to the ordinary dispatcher, which checks the exact instruction.
  Trap ignored{};
  if(!mmu::translate(hart,bus,hart.pc&~4095ull,4096*8,memory::Access::Fetch,physical,ignored) || !bus.direct(physical,4096,memory::Access::Fetch)) {++statistics_.link_exits;return {};}
  (void)trap;physical+=hart.pc&4095;
  auto& set=cache_[((physical>>1)^(physical>>13)^(physical>>25))&(cache_.size()-1)];
  for(const auto& target:set.ways)if(target && target->address==physical && target->native && target->count<=remaining && validate_source(*target,bus,bus.code_generation(physical))) {
    ++statistics_.slow_link_hits;record_link(source,hart,target.get(),source_pc);return target.get();
  }
  ++statistics_.link_exits;
  return {};
}
const void* BlockEngine::resolve_chain(NativeChain* chain,Hart* hart,const memory::AddressSpace* bus,Trap* trap,u32 count) {
  auto& state=*static_cast<ChainState*>(chain->state);
  struct ResolverSample {
    BlockEngine& owner;bool active;u64 started=0;
    ResolverSample(BlockEngine& engine):owner(engine),active(engine.profile_sample_) {if(active)started=platform::profiling_ticks();}
    ~ResolverSample() {if(active) {owner.statistics_.resolver_ticks+=platform::profiling_ticks()-started;++owner.statistics_.resolver_samples;}}
  } sample(*state.owner);
  if(chain->current)state.current=static_cast<Block*>(chain->current->block);
  state.retired=chain->retired+(count&0x7fffffffu);chain->retired=state.retired;state.trapped=(count&0x80000000u)!=0;
  if(sample.active) {
    state.owner->statistics_.sampled_native_instructions+=chain->sampled_instructions+(count&0x7fffffffu);
    state.owner->statistics_.sampled_helper_calls+=chain->sampled_helpers+state.current->native->helper_calls(count&0x7fffffffu,state.trapped);
    state.owner->statistics_.sampled_inline_loads+=chain->sampled_loads;
    chain->sampled_loads=0;
    chain->sampled_instructions=chain->sampled_helpers=0;
  }
  const auto retired_here=count&0x7fffffffu;
  // An ordinary block/fault exit needs no host division to update loop stats.
  // Only a native self-loop may retire more than one block per resolver call.
  const auto full_blocks=retired_here<state.current->count?0u:
    retired_here==state.current->count?1u:retired_here/state.current->count;
  if(full_blocks)state.owner->statistics_.self_loops+=full_blocks-(state.trapped?0u:1u);
  state.barrier=state.current->barrier;
  if(state.trapped || state.barrier || hart->waiting) {++state.owner->statistics_.boundary_exits;return nullptr;}
  if(state.retired>=state.limit) {++state.owner->statistics_.budget_exits;return nullptr;}
  auto next=state.owner->find_link(*state.current,*hart,*bus,*trap,state.limit-state.retired,chain->current_pc);
  if(!next)return nullptr;
  state.current=next;
  chain->generation=bus->code_generation_counter(next->address);
  next->native_link.generation=chain->generation;
  chain->current=&next->native_link;
  chain->current_pc=hart->pc;
  chain->expected_generation=next->generation;chain->remaining=state.limit-state.retired;
  ++state.owner->linked_blocks_;return state.current->native->entry();
}
BlockEngine::Result BlockEngine::run_batch(Hart& hart,const memory::AddressSpace& bus,
                                         Trap& trap,u32 limit,bool jit) {
  Result total{};
  const bool previous_chaining=chaining_enabled_;chaining_enabled_=true;
  while(total.retired<limit) {
    auto part=run(hart,bus,trap,limit-total.retired,jit);
    if(!part.handled)break;
    total.handled=true;total.retired+=part.retired;
    total.trapped=part.trapped;total.barrier=part.barrier;
    if(part.trapped || part.barrier || !part.retired || hart.waiting)break;
  }
  chaining_enabled_=previous_chaining;return total;
}
void BlockEngine::report_statistics(u32 hart_id) const {
  const auto& s=statistics_;
  const auto simd=platform::code_compare_backend();
  NanoRV_INFO("JIT snapshot compare: %.*s",static_cast<int>(simd.size()),simd.data());
  NanoRV_INFO("JIT profile hart %u: dispatch=%llu decode=%llu eviction=%llu invalidation=%llu revalidation=%llu native_insn=%llu cold_insn=%llu queued=%llu link_hit=%llu link_miss=%llu self_loop=%llu",
    hart_id,static_cast<unsigned long long>(s.dispatches),static_cast<unsigned long long>(s.decoded),
    static_cast<unsigned long long>(s.evicted),static_cast<unsigned long long>(s.invalidated),
    static_cast<unsigned long long>(s.revalidated),static_cast<unsigned long long>(s.native_instructions),
    static_cast<unsigned long long>(s.cold_instructions),static_cast<unsigned long long>(s.queued),
    static_cast<unsigned long long>(s.link_hits),static_cast<unsigned long long>(s.link_misses),
    static_cast<unsigned long long>(s.self_loops));
  NanoRV_INFO("JIT detail hart %u: direct_link_hit=%llu slow_link_hit=%llu link_exit=%llu budget_exit=%llu boundary_exit=%llu tail_insn=%llu pending_insn=%llu warming_insn=%llu",
    hart_id,static_cast<unsigned long long>(s.direct_link_hits),static_cast<unsigned long long>(s.slow_link_hits),static_cast<unsigned long long>(s.link_exits),
    static_cast<unsigned long long>(s.budget_exits),static_cast<unsigned long long>(s.boundary_exits),
    static_cast<unsigned long long>(s.tail_instructions),static_cast<unsigned long long>(s.pending_instructions),
    static_cast<unsigned long long>(s.warming_instructions));
  if(profiling_)NanoRV_INFO("JIT sample hart %u: samples=%llu native_samples=%llu cold_samples=%llu lookup_ticks=%llu native_ticks=%llu cold_ticks=%llu resolver_ticks=%llu resolver_samples=%llu sampled_native_insn=%llu sampled_helper_calls=%llu sampled_inline_loads=%llu timer_call_ticks=%llu",
    hart_id,static_cast<unsigned long long>(s.timing_samples),static_cast<unsigned long long>(s.native_samples),
    static_cast<unsigned long long>(s.cold_samples),static_cast<unsigned long long>(s.lookup_ticks),
    static_cast<unsigned long long>(s.native_ticks),static_cast<unsigned long long>(s.cold_ticks),
    static_cast<unsigned long long>(s.resolver_ticks),static_cast<unsigned long long>(s.resolver_samples),
    static_cast<unsigned long long>(s.sampled_native_instructions),static_cast<unsigned long long>(s.sampled_helper_calls),
    static_cast<unsigned long long>(s.sampled_inline_loads),static_cast<unsigned long long>(s.timer_call_ticks));
}
}
