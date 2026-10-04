// SPDX-License-Identifier: MIT
//
// The machine: RAM, address space and the harts that run on it.
// One hart in stage 1; the multi-hart structure arrives with stage 9.
#pragma once

#include "core/elf_loader.hpp"
#include "cpu/execute.hpp"
#include "cpu/hart.hpp"
#include "machine/machine_config.hpp"
#include "memory/address_space.hpp"
#include "memory/ram_region.hpp"
#include "devices/virt_devices.hpp"
#include "devices/virtio.hpp"
#include "devices/virtio_graphics.hpp"
#include "cpu/block_engine.hpp"

#include <expected>
#include <memory>
#include <string>
#include <vector>

namespace nanorv::machine {

enum class StopReason {
    None,
    InstructionLimit,
    Trap,
};

struct RunResult {
    StopReason reason = StopReason::None;
    bool trapped = false;
    cpu::Trap trap{};
    u64 instructions = 0;
    u64 elapsed_ns = 0;
    int exit_code = 0;
    std::vector<u64> hart_cpu_ns;
    std::vector<u64> hart_host_ids;
    u64 native_blocks=0,linked_blocks=0;
};

class Machine {
public:
    explicit Machine(const MachineConfig& config);
    ~Machine();

    Machine(const Machine&) = delete;
    Machine& operator=(const Machine&) = delete;

    // Loads the configured image and resets the harts. Returns the error text.
    std::expected<core::LoadedImage, std::string> load_image();

    void reset(u64 entry);

    // Runs until a trap, an instruction limit, or forever when no limit is set.
    RunResult run();

    // Single step, used by the debugger and the tests.
    RunResult step(u32 hart_index = 0);

    memory::AddressSpace& memory() noexcept { return memory_; }
    const memory::AddressSpace& memory() const noexcept { return memory_; }
    cpu::Hart& hart(u32 index) noexcept { return harts_[index]; }
    const cpu::Hart& hart(u32 index) const noexcept { return harts_[index]; }
    u32 hart_count() const noexcept { return static_cast<u32>(harts_.size()); }
    const MachineConfig& config() const noexcept { return config_; }

    void print_memory_map() const;

private:
    RunResult run_parallel();
    MachineConfig config_;
    memory::AddressSpace memory_;
    std::unique_ptr<memory::RamRegion> ram_;
    std::vector<cpu::Hart> harts_;
    u64 entry_ = nanorv::kDramBase;
    bool loaded_ = false;
    devices::Uart uart_;
    devices::Clint clint_;
    devices::Plic plic_;
    devices::TestDevice test_;
    devices::VirtioBlock block_;
    devices::VirtioNet net_;
    platform::Display display_;
    devices::VirtioGpu gpu_;
    devices::VirtioInput input_;
    devices::GoldfishRtc rtc_;
    u64 attempts_ = 0;
    u64 run_instructions_ = 0;
    u64 uart_poll_at_ = 0, plic_poll_at_ = 0, net_poll_at_ = 0;
    u64 clock_origin_ns_=0,clock_poll_at_=0;
    cpu::BlockEngine engine_;
};

} // namespace nanorv::machine
