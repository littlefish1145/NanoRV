// SPDX-License-Identifier: MIT
//
// rvvm entry point. This file is the only place that touches the console and
// the process exit code; everything else lives in the machine core.
#include "core/log.hpp"
#include "debug/register_dump.hpp"
#include "machine/machine.hpp"
#include "machine/machine_config.hpp"
#include "platform/platform.hpp"

#include <cstdio>
#include <string>

namespace {

int run(const rvvm::machine::MachineConfig& config) {
    rvvm::machine::Machine machine(config);

    auto loaded = machine.load_image();
    if (!loaded) {
        RVVM_ERROR("rvvm: %s", loaded.error().c_str());
        return 1;
    }

    const rvvm::machine::RunResult result = machine.run();

    if (config.dump_registers) {
        rvvm::debug::dump_registers(machine.hart(0));
    }

    const rvvm::u64 retired = result.instructions;
    const double seconds = static_cast<double>(result.elapsed_ns) / 1e9;
    RVVM_INFO("rvvm: %llu instructions in %.3f s (%.0f ins/s)", static_cast<unsigned long long>(retired),
              seconds, seconds > 0.0 ? static_cast<double>(retired) / seconds : 0.0);

    switch (result.reason) {
    case rvvm::machine::StopReason::InstructionLimit:
        return 0;
    case rvvm::machine::StopReason::Trap: {
        std::string tval_text;
        if (result.trap.has_tval) {
            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), "0x%llx",
                          static_cast<unsigned long long>(result.trap.tval));
            tval_text = std::string(" (tval ") + buffer + ")";
        }
        RVVM_ERROR("rvvm: stopped on trap: %s%s", rvvm::cpu::to_string(result.trap.cause),
                   tval_text.c_str());
        return 2;
    }
    case rvvm::machine::StopReason::None:
        break;
    }
    return result.exit_code;
}

} // namespace

int main(int argc, char** argv) {
    rvvm::platform::initialize_console();
    const auto parsed = rvvm::machine::parse_command_line(argc, argv);
    if (!parsed) {
        rvvm::platform::write_err(parsed.error() + "\n");
        return 1;
    }
    if (parsed->show_help) {
        rvvm::platform::write_out(rvvm::machine::usage_text());
        return 0;
    }
    if (parsed->show_version) {
        rvvm::platform::write_out(rvvm::machine::version_text() + "\n");
        return 0;
    }
    if(parsed->host_vulkan_info || parsed->host_vulkan_test) {
        std::string error;
        if(parsed->host_vulkan_info) {
            std::vector<rvvm::platform::VulkanDevice> devices;
            if(!rvvm::platform::VulkanPresenter::enumerate(devices,error)) {RVVM_ERROR("%s",error.c_str());return 1;}
            for(const auto& device:devices)std::printf("%s | vendor=0x%04x device=0x%04x Vulkan=%u.%u.%u %s\n",device.name.c_str(),
                device.vendor,device.device,device.api>>22,(device.api>>12)&1023,device.api&4095,device.software?"software":device.discrete?"discrete":"integrated");
        }
        if(parsed->host_vulkan_test) {
            std::string device;
            if(!rvvm::platform::VulkanPresenter::self_test(parsed->host_gpu,device,error)) {RVVM_ERROR("%s",error.c_str());return 1;}
            std::printf("Vulkan GPU upload/readback PASS: %s (three sizes, exact pixel comparison)\n",device.c_str());
        }
        return 0;
    }
    return run(*parsed);
}
