// SPDX-License-Identifier: MIT
//
// CLI options for the virtual machine. Stage 1 only accepts the options that
// are actually implemented; unknown or not-yet-implemented options fail loudly
// instead of being ignored.
#pragma once

#include "core/types.hpp"
#include "platform/network.hpp"
#include "platform/display.hpp"

#include <expected>
#include <optional>
#include <string>
#include <vector>

namespace nanorv::machine {

enum class EngineKind {
    Interpreter, // reference interpreter (always available)
    Block,       // stage 10
    Jit,         // stage 11
};

enum class ImageKind {
    Elf,
    Raw,
};

struct MachineConfig {
    // --- image -------------------------------------------------------------
    ImageKind image_kind = ImageKind::Elf;
    std::string image_path;
    bool linux_boot = false;
    std::string bios_path = "assets/fw_jump.bin", kernel_path, initrd_path, dtb_path;
    std::string append = "console=ttyS0 earlycon=sbi rdinit=/bin/sh TERM=dumb";
    std::string disk_path;
    bool network = false;
    std::vector<platform::PortForward> port_forwards;
    bool graphics = false, display_window = true;
    bool virgl = false;
    platform::DisplayBackend display_backend=platform::DisplayBackend::Auto;
    platform::HostGpu host_gpu=platform::HostGpu::Auto;
    bool host_vulkan_info=false,host_vulkan_test=false;
    bool realtime_clock = false;
    bool parallel_harts = true;
    std::string screenshot_path;
    u64 raw_load_address = nanorv::kDramBase;
    std::optional<u64> entry_override;

    // --- machine -----------------------------------------------------------
    u64 ram_size = 256ull * 1024 * 1024; // default 256 MiB, QEMU virt uses 128 MiB
    u32 hart_count = 1;
    // Frequency the `time` CSR counts in. 1 GHz makes it read in nanoseconds
    // until the CLINT defines a real timebase in stage 5.
    u64 timebase_frequency = 1'000'000'000ull;

    // --- execution ---------------------------------------------------------
    EngineKind engine = EngineKind::Interpreter;
    u32 jit_workers = 2;
    u64 max_instructions = 0; // 0 = unlimited
    bool trace = false;
    bool dump_registers = true;
    bool verbose_memory_map = false;

    // --- diagnostics -------------------------------------------------------
    bool show_help = false;
    bool show_version = false;
};

// Returns the parsed configuration, or an error message on failure.
std::expected<MachineConfig, std::string> parse_command_line(int argc, const char* const* argv);

std::string usage_text();
std::string version_text();

} // namespace nanorv::machine
