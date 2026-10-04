// SPDX-License-Identifier: MIT
#include "machine/machine.hpp"

#include "core/elf_loader.hpp"
#include "core/log.hpp"
#include "debug/register_dump.hpp"
#include "debug/trace.hpp"
#include "platform/platform.hpp"
#include "machine/fdt.hpp"
#include "cpu/privileged.hpp"

#include <cstdio>
#include <algorithm>
#include <cstring>

namespace nanorv::machine {
namespace {

std::string format_bytes(u64 bytes) {
    char buffer[64];
    if (bytes >= 1024ull * 1024 * 1024) {
        std::snprintf(buffer, sizeof(buffer), "%.2f GiB", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= 1024 * 1024) {
        std::snprintf(buffer, sizeof(buffer), "%.2f MiB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else {
        std::snprintf(buffer, sizeof(buffer), "%llu bytes", static_cast<unsigned long long>(bytes));
    }
    return buffer;
}

} // namespace

Machine::Machine(const MachineConfig& config) : config_(config), block_(memory_), net_(memory_), gpu_(memory_,display_),input_(memory_),engine_(config.engine==EngineKind::Jit && !(config.parallel_harts && config.hart_count>1)?config.jit_workers:0) {
    ram_ = std::make_unique<memory::RamRegion>("dram", config_.ram_size, nanorv::kDramBase);
    memory::Permissions permissions{};
    permissions.read = true;
    permissions.write = true;
    permissions.execute = true;
    if (!memory_.add(nanorv::kDramBase, config_.ram_size, ram_.get(), "dram", permissions)) {
        NanoRV_FATAL("machine: failed to map DRAM at 0x%llx", static_cast<unsigned long long>(nanorv::kDramBase));
    }

    // Devices arrive in stage 5 (CLINT/PLIC/UART); the map is printed now so the
    // layout is visible from the very first run.
    NanoRV_INFO("NanoRV: RAM %s at 0x%016llx (%s)", format_bytes(config_.ram_size).c_str(),
              static_cast<unsigned long long>(nanorv::kDramBase), ram_->name());
    memory::Permissions mmio{true,true,false};
    memory_.add(0x10000000,uart_.size(),&uart_,"uart",mmio);
    memory_.add(0x2000000,clint_.size(),&clint_,"clint",mmio);
    memory_.add(0xc000000,plic_.size(),&plic_,"plic",mmio);
    memory_.add(0x100000,test_.size(),&test_,"test",mmio);
    if (!config_.disk_path.empty()) memory_.add(0x10001000,block_.size(),&block_,"virtio-blk",mmio);
    if (config_.network) memory_.add(0x10002000,net_.size(),&net_,"virtio-net",mmio);
    if(config_.graphics) {
      memory_.add(0x10003000,gpu_.size(),&gpu_,"virtio-gpu",mmio);
      memory_.add(0x10004000,input_.size(),&input_,"virtio-input",mmio);
    }
    memory_.add(0x101000,rtc_.size(),&rtc_,"rtc",mmio);

    harts_.resize(config_.hart_count);
    if(harts_.size()>1) { // A single hart has no other hart to clear; per-store note_store covers its own reservation.
    memory_.set_write_observer(this,[](void* state,u64 address,u64 bytes) {
        for(auto& h:static_cast<Machine*>(state)->harts_) {
            if(h.reservation_valid && address<h.reservation_address+h.reservation_width/8 &&
               h.reservation_address<address+bytes)h.clear_reservation();
        }
    });
    }
    u32 index = 0;
    for (auto& h : harts_) {
        h.reset(nanorv::kDramBase);
        h.hart_id = index++;
        h.csr = cpu::CsrFile(h.hart_id, config_.timebase_frequency);
        h.translation_cache_enabled=config_.engine!=EngineKind::Interpreter;
    }
    print_memory_map();
}

Machine::~Machine() = default;

void Machine::print_memory_map() const {
    if (!config_.verbose_memory_map) {
        return;
    }
    NanoRV_INFO("address space:");
    for (const auto& entry : memory_.regions()) {
        NanoRV_INFO("  [%016llx, %016llx) %-8s %s", static_cast<unsigned long long>(entry.base),
                  static_cast<unsigned long long>(entry.base + entry.size), entry.name.c_str(),
                  format_bytes(entry.size).c_str());
    }
}

std::expected<core::LoadedImage, std::string> Machine::load_image() {
    if(config_.linux_boot) {
        std::string error;
        if (!config_.disk_path.empty() && !block_.open(config_.disk_path, error)) return std::unexpected(error);
        if (config_.network && !net_.open(error,config_.port_forwards)) return std::unexpected(error);
        if(config_.graphics && !display_.open(config_.display_window,error,config_.display_backend,config_.host_gpu))return std::unexpected(error);
        if(config_.graphics)NanoRV_INFO("Host display: %s",display_.description().c_str());
        if(config_.virgl && !gpu_.open_renderer(error,config_.host_gpu))return std::unexpected(error);
        auto firmware=platform::read_file(config_.bios_path,&error);if(!firmware)return std::unexpected(error);
        auto kernel=platform::read_file(config_.kernel_path,&error);if(!kernel)return std::unexpected(error);
        if(kernel->size()<64 || std::memcmp(kernel->data()+56,"RSC\x05",4)!=0)return std::unexpected("invalid RISC-V Linux Image header");
        platform::ByteBuffer initrd;
        if(!config_.initrd_path.empty()) {auto b=platform::read_file(config_.initrd_path,&error);if(!b)return std::unexpected(error);initrd=std::move(*b);}
        constexpr u64 kernel_addr=0x80200000,dtb_addr=0x84000000,initrd_addr=0x88000000;
        auto dtb=make_fdt(config_.ram_size,initrd_addr,initrd.size(),config_.append,!config_.disk_path.empty(),config_.network,config_.hart_count,config_.graphics);
        if(!config_.dtb_path.empty()) {auto b=platform::read_file(config_.dtb_path,&error);if(!b)return std::unexpected(error);dtb=std::move(*b);}
        if(firmware->size()>kernel_addr-kDramBase || kernel->size()>dtb_addr-kernel_addr || dtb.size()>initrd_addr-dtb_addr || initrd.size()>config_.ram_size-(initrd_addr-kDramBase))return std::unexpected("boot images do not fit the non-overlapping RAM layout");
        std::memcpy(ram_->host_data(),firmware->data(),firmware->size());
        std::memcpy(ram_->host_data()+kernel_addr-kDramBase,kernel->data(),kernel->size());
        std::memcpy(ram_->host_data()+dtb_addr-kDramBase,dtb.data(),dtb.size());
        if(!initrd.empty())std::memcpy(ram_->host_data()+initrd_addr-kDramBase,initrd.data(),initrd.size());
        memory_.invalidate_code();
        reset(kDramBase);
        clock_origin_ns_=platform::monotonic_ns();clock_poll_at_=0;
        for(auto& h:harts_) {h.set_reg(10,h.hart_id);h.set_reg(11,dtb_addr);h.csr.virtual_clock=true;h.translation_cache_enabled=true;}
        loaded_=true;
        NanoRV_INFO("Linux boot: OpenSBI=0x80000000 kernel=0x80200000 DTB=0x84000000 initrd=0x88000000 (%zu bytes)",initrd.size());
        core::LoadedImage image{};image.entry=kDramBase;image.segment_count=4;return image;
    }
    std::string error;
    auto bytes = platform::read_file(config_.image_path, &error);
    if (!bytes) {
        return std::unexpected<std::string>(error);
    }
    NanoRV_INFO("NanoRV: '%s' is %zu bytes", config_.image_path.c_str(), bytes->size());

    core::LoadResult loaded =
        (config_.image_kind == ImageKind::Elf)
            ? core::load_elf(memory_, *bytes)
            : core::load_raw(memory_, *bytes, config_.raw_load_address, config_.entry_override);
    if (!loaded) {
        return std::unexpected<std::string>(loaded.error());
    }

    if (config_.entry_override) {
        NanoRV_INFO("NanoRV: entry point overridden: 0x%016llx -> 0x%016llx",
                  static_cast<unsigned long long>(loaded->entry),
                  static_cast<unsigned long long>(*config_.entry_override));
        loaded->entry = *config_.entry_override;
    }

    entry_ = loaded->entry;
    reset(entry_);
    loaded_ = true;
    NanoRV_INFO("NanoRV: loaded %u segment(s), %llu bytes, image range [0x%016llx, 0x%016llx), entry 0x%016llx",
              loaded->segment_count, static_cast<unsigned long long>(loaded->bytes_written),
              static_cast<unsigned long long>(loaded->lowest_address),
              static_cast<unsigned long long>(loaded->highest_address),
              static_cast<unsigned long long>(loaded->entry));
    return loaded;
}

void Machine::reset(u64 entry) {
    for (auto& h : harts_) {
        h.reset(entry);
    }
    entry_ = entry;
}

RunResult Machine::step(u32 hart_index) {
    RunResult result{};
    cpu::Hart& hart = harts_[hart_index];

    cpu::Decoded insn{};
    cpu::Trap trap{};
    if(config_.linux_boot) {
        block_.poll();
        ++attempts_;
        if(!config_.realtime_clock)clint_.time=attempts_/10;
        else if(attempts_>=clock_poll_at_ || hart.waiting) {
            if(!clock_origin_ns_)clock_origin_ns_=platform::monotonic_ns();
            clint_.time=(platform::monotonic_ns()-clock_origin_ns_)/100;
            clock_poll_at_=attempts_+4096;
        }
        if(attempts_>=uart_poll_at_) {uart_.poll();uart_poll_at_=attempts_+1024;}
        clint_.update(hart);
        if (attempts_>=net_poll_at_ || hart.waiting) {
            if (config_.network) net_.poll();
            if(config_.graphics) {auto events=display_.events();input_.push(events);}
            net_poll_at_=attempts_+16384;
        }
        if (attempts_>=plic_poll_at_ || hart.waiting) {
            plic_.set_irq(1,block_.interrupt());plic_.set_irq(2,net_.interrupt());
            plic_.set_irq(11,rtc_.interrupt());
            if(config_.graphics) {plic_.set_irq(3,gpu_.interrupt());plic_.set_irq(4,input_.interrupt());}
            plic_poll_at_=attempts_+256;
        }
        plic_.update(hart,uart_.interrupt());
        if(cpu::pending_interrupt(hart,trap)) {cpu::enter_trap(hart,trap);return result;}
        if(hart.waiting && (hart.csr.raw(cpu::kCsrMip)&hart.csr.raw(cpu::kCsrMie)))hart.waiting=false;
        if(hart.waiting) {
            const bool all_waiting=std::all_of(harts_.begin(),harts_.end(),[](const cpu::Hart& h){return h.waiting;});
            if(all_waiting) {
                if(!config_.realtime_clock) {clint_.time+=10000;attempts_=clint_.time*10;}
                uart_.poll();platform::sleep_ms(1);
            }
            return result;
        }
    }
    if (config_.engine != EngineKind::Interpreter && !config_.trace) {
        const u32 batch_limit=config_.engine==EngineKind::Jit?8192u:256u;
        u32 limit = batch_limit;
        if (config_.max_instructions) limit = static_cast<u32>(std::min<u64>(batch_limit,config_.max_instructions-run_instructions_));
        auto block = engine_.run_batch(hart,memory_,trap,limit,config_.engine==EngineKind::Jit);
        if (block.handled) {
            result.instructions = block.retired;
            if (config_.linux_boot && block.retired > 1) attempts_ += block.retired-1;
            if (block.trapped) {
                if (config_.linux_boot) cpu::enter_trap(hart,trap);
                else { result.reason=StopReason::Trap;result.trapped=true;result.trap=trap; }
            }
            return result;
        }
    }
    if (!cpu::fetch(hart, memory_, insn, trap)) {
        if(config_.linux_boot) {cpu::enter_trap(hart,trap);return result;}
        result.reason = StopReason::Trap;
        result.trapped = true;
        result.trap = trap;
        return result;
    }

    const u64 pc = hart.pc;
    if (config_.trace) {
        debug::trace_instruction(hart, insn);
    }

    hart.next_pc = pc + insn.length;
    if (!cpu::execute(hart, memory_, insn, trap)) {
        if(config_.linux_boot) {
            NanoRV_DEBUG("trap pc=%llx cause=%llu tval=%llx",hart.pc,static_cast<u64>(trap.cause),trap.tval);
            cpu::enter_trap(hart,trap);return result;
        }
        result.reason = StopReason::Trap;
        result.trapped = true;
        result.trap = trap;
        return result;
    }

hart.pc = hart.next_pc;
        hart.next_pc = hart.pc;
        result.instructions = 1;
        return result;
    }

RunResult Machine::run() {
    block_.set_async(config_.linux_boot && !config_.disk_path.empty());
    struct DrainDisk {
        devices::VirtioBlock& block;
        ~DrainDisk() {block.set_async(false);}
    } drain_disk{block_};
    const bool parallel=config_.parallel_harts && hart_count()>1 && !config_.trace;
    platform::prefer_performance_core(parallel?hart_count():0);
    if(parallel)return run_parallel();
    RunResult result{};
    const u64 start = platform::monotonic_ns();
    u64 retired = 0;
    run_instructions_=0;
    u32 current=0,quantum=0;

    while (true) {
        if(test_.stopped) {
            if ((test_.code&0xffff)==0x7777 && config_.linux_boot) {
                NanoRV_INFO("NanoRV: guest requested reboot");
                uart_.reset();clint_=devices::Clint{};plic_=devices::Plic{};
                rtc_=devices::GoldfishRtc{};test_=devices::TestDevice{};
                block_.store(0x70,32,0);net_.store(0x70,32,0);
                gpu_.store(0x70,32,0);input_.store(0x70,32,0);
                attempts_=uart_poll_at_=plic_poll_at_=net_poll_at_=0;
                std::memset(ram_->host_data(),0,static_cast<std::size_t>(config_.ram_size));
                auto reloaded=load_image();
                if (!reloaded) {NanoRV_ERROR("NanoRV: reboot failed: %s",reloaded.error().c_str());result.exit_code=1;break;}
                current=quantum=0;
                continue;
            }
            if ((test_.code&0xffff)==0x3333) result.exit_code=1;
            break;
        }
        const RunResult step_result = step(current);
        retired += step_result.instructions;
        run_instructions_=retired;
        if (step_result.trapped) {
            const cpu::Hart& hart = harts_[current];
            NanoRV_ERROR("NanoRV: trap at pc=0x%016llx: %s (cause %llu, tval 0x%llx)",
                       static_cast<unsigned long long>(hart.pc),
                       cpu::to_string(step_result.trap.cause),
                       static_cast<unsigned long long>(step_result.trap.cause),
                       static_cast<unsigned long long>(step_result.trap.tval));
            result.reason = StopReason::Trap;
            result.trapped = true;
            result.trap = step_result.trap;
            break;
        }
        if (config_.max_instructions != 0 && retired >= config_.max_instructions) {
            NanoRV_INFO("NanoRV: instruction limit of %llu reached",
                      static_cast<unsigned long long>(config_.max_instructions));
            result.reason = StopReason::InstructionLimit;
            break;
        }
        quantum+=static_cast<u32>(std::max<u64>(1,step_result.instructions));
        if(quantum>=8192 || harts_[current].waiting) {
            // Do not spend scheduling quanta on parked secondary harts. Check
            // their local interrupts before skipping, including SBI IPIs.
            for(u32 n=0;n<hart_count();++n) {
                current=(current+1)%hart_count();auto& next=harts_[current];
                if(!next.waiting)break;
                clint_.update(next);plic_.update(next,uart_.interrupt());
                if(next.csr.raw(cpu::kCsrMip)&next.csr.raw(cpu::kCsrMie)) {next.waiting=false;break;}
            }
            quantum=0;
        }
    }

    result.instructions = retired;
    result.native_blocks=engine_.native_compiled();result.linked_blocks=engine_.linked_blocks();
    if(config_.engine==EngineKind::Jit)engine_.report_statistics(0);
    if(config_.engine==EngineKind::Jit)NanoRV_INFO("JIT: %llu native blocks, %llu native block links",static_cast<unsigned long long>(result.native_blocks),static_cast<unsigned long long>(result.linked_blocks));
    if(config_.graphics) {
      NanoRV_INFO("VirtIO GPU: %llu presented frames",static_cast<unsigned long long>(gpu_.frames()));
      if(!config_.screenshot_path.empty()) {std::string error;if(!display_.save(config_.screenshot_path,error)) {NanoRV_ERROR("screenshot: %s",error.c_str());result.exit_code=1;}}
    }
    block_.set_async(false);
    uart_.flush();
    result.elapsed_ns = platform::monotonic_ns() - start;
    return result;
}

} // namespace nanorv::machine
