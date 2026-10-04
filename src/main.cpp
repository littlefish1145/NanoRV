// SPDX-License-Identifier: MIT
//
// nanorv entry point. This file is the only place that touches the console and
// the process exit code; everything else lives in the machine core.
#include "core/log.hpp"
#include "debug/register_dump.hpp"
#include "machine/machine.hpp"
#include "machine/machine_config.hpp"
#include "platform/platform.hpp"

#include <cstdio>
#include <string>

namespace {

int run(const nanorv::machine::MachineConfig& config) {
    nanorv::machine::Machine machine(config);

    auto loaded = machine.load_image();
    if (!loaded) {
        NanoRV_ERROR("nanorv: %s", loaded.error().c_str());
        return 1;
    }

    const nanorv::machine::RunResult result = machine.run();

    if (config.dump_registers) {
        nanorv::debug::dump_registers(machine.hart(0));
    }

    const nanorv::u64 retired = result.instructions;
    const double seconds = static_cast<double>(result.elapsed_ns) / 1e9;
    NanoRV_INFO("nanorv: %llu instructions in %.3f s (%.0f ins/s)", static_cast<unsigned long long>(retired),
              seconds, seconds > 0.0 ? static_cast<double>(retired) / seconds : 0.0);

    switch (result.reason) {
    case nanorv::machine::StopReason::InstructionLimit:
        return 0;
    case nanorv::machine::StopReason::Trap: {
        std::string tval_text;
        if (result.trap.has_tval) {
            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), "0x%llx",
                          static_cast<unsigned long long>(result.trap.tval));
            tval_text = std::string(" (tval ") + buffer + ")";
        }
        NanoRV_ERROR("nanorv: stopped on trap: %s%s", nanorv::cpu::to_string(result.trap.cause),
                   tval_text.c_str());
        return 2;
    }
    case nanorv::machine::StopReason::None:
        break;
    }
    return result.exit_code;
}

} // namespace

int main(int argc, char** argv) {
    nanorv::platform::initialize_console();
    const auto parsed = nanorv::machine::parse_command_line(argc, argv);
    if (!parsed) {
        nanorv::platform::write_err(parsed.error() + "\n");
        return 1;
    }
    if (parsed->show_help) {
        nanorv::platform::write_out(nanorv::machine::usage_text());
        return 0;
    }
    if (parsed->show_version) {
        nanorv::platform::write_out(nanorv::machine::version_text() + "\n");
        return 0;
    }
    if(parsed->host_vulkan_info || parsed->host_vulkan_test) {
        std::string error;
        if(parsed->host_vulkan_info) {
            std::vector<nanorv::platform::VulkanDevice> devices;
            if(!nanorv::platform::VulkanPresenter::enumerate(devices,error)) {NanoRV_ERROR("%s",error.c_str());return 1;}
            for(const auto& device:devices)std::printf("%s | vendor=0x%04x device=0x%04x Vulkan=%u.%u.%u %s\n",device.name.c_str(),
                device.vendor,device.device,device.api>>22,(device.api>>12)&1023,device.api&4095,device.software?"software":device.discrete?"discrete":"integrated");
        }
        if(parsed->host_vulkan_test) {
            std::string device;
            if(!nanorv::platform::VulkanPresenter::self_test(parsed->host_gpu,device,error)) {NanoRV_ERROR("%s",error.c_str());return 1;}
            std::printf("Vulkan GPU upload/readback PASS: %s (three sizes, exact pixel comparison)\n",device.c_str());
        }
        return 0;
    }
    return run(*parsed);
}
