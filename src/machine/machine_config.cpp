// SPDX-License-Identifier: MIT
#include "machine/machine_config.hpp"

#include "core/log.hpp"

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace rvvm::machine {
namespace {

using R = std::unexpected<std::string>;

std::string to_lower(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return text;
}

R fail(const std::string& message) { return R(message); }

std::expected<u64, std::string> parse_u64(const std::string& text) {
    if (text.empty()) {
        return fail("expected a number but got an empty argument");
    }
    errno = 0;
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text.c_str(), &end, 0);
    if (errno != 0 || end == nullptr || *end != '\0') {
        return fail("'" + text + "' is not a valid number");
    }
    return static_cast<u64>(value);
}

std::expected<std::string, std::string> next_argument(int argc, const char* const* argv, int& index) {
    if (index + 1 >= argc) {
        return fail(std::string("option '") + argv[index] + "' requires a value");
    }
    ++index;
    return std::string(argv[index]);
}

} // namespace

std::string usage_text() {
    return
        "RVVM - Windows-native RISC-V (RV64GC) virtual machine\n"
        "\n"
        "Usage:\n"
        "  rvvm run <image.elf|image.bin> [options]\n"
        "  rvvm <image.elf|image.bin> [options]\n"
        "\n"
        "Image:\n"
        "      --bios FILE        OpenSBI firmware (default assets/fw_jump.bin)\n"
        "      --kernel FILE      boot an RV64 Linux Image\n"
        "      --initrd FILE      initramfs\n"
        "      --disk FILE        writable raw disk on VirtIO block\n"
        "      --net user         NAT networking with DHCP and DNS\n"
        "      --forward H:G      loopback TCP host port H to guest port G\n"
        "      --display MODE     window|headless; enable graphics and input\n"
        "      --gpu TYPE         2d|virgl; VirGL uses host OpenGL\n"
        "      --display-backend  auto|gdi|vulkan (host presentation only)\n"
        "      --host-gpu TYPE    auto|nvidia|intel|amd; require selected vendor\n"
        "      --host-vulkan-info enumerate host Vulkan devices without booting\n"
        "      --host-vulkan-test verify host Vulkan GPU upload/readback\n"
        "      --screenshot FILE  save final framebuffer as BMP\n"
        "      --dtb FILE         use an external device tree\n"
        "      --append TEXT      Linux command line\n"
        "      --elf              treat the image as an ELF64 RISC-V executable (default)\n"
        "      --raw              treat the image as a flat binary\n"
        "      --load-addr ADDR   load address for --raw (default 0x80000000)\n"
        "      --entry ADDR       override the entry point\n"
        "\n"
        "Machine:\n"
        "  -m, --memory SIZE      RAM size in bytes; accepts K/M/G suffixes (default 256M)\n"
        "  -smp, --smp N          number of harts (1..8)\n"
        "      --smp-mode MODE    parallel|cooperative (default parallel)\n"
        "\n"
        "Execution:\n"
        "      --engine NAME      interpreter|block|jit\n"
        "      --jit-workers N    background compiler threads (0..8, default 2)\n"
        "      --clock MODE       virtual|realtime (default virtual)\n"
        "  -d, --trace            print an execution trace\n"
        "      --max-steps N      stop after N retired instructions\n"
        "      --no-reg-dump      do not print the register dump on exit\n"
        "      --log-level LEVEL  trace|debug|info|warn|error (default info)\n"
        "\n"
        "Other:\n"
        "  -h, --help             show this help\n"
        "  -V, --version          show the version\n";
}

std::string version_text() {
    return "rvvm 0.9.0 (RV64GC+Zba/Zbb/Zbs / Sv39/Sv48 / VirtIO+VirGL / parallel SMP / IR x64 JIT, Windows native)";
}

std::expected<MachineConfig, std::string> parse_command_line(int argc, const char* const* argv) {
    MachineConfig config{};
    bool image_seen = false;

    int i = 1;
    if (i < argc) {
        const std::string_view first(argv[i]);
        if (first == "-h" || first == "--help") {
            config.show_help = true;
            return config;
        }
        if (first == "-V" || first == "--version") {
            config.show_version = true;
            return config;
        }
    }

    for (; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if(arg=="--host-vulkan-info") {config.host_vulkan_info=true;continue;}
        if(arg=="--host-vulkan-test") {config.host_vulkan_test=true;continue;}
        if(arg=="--display-backend" || arg=="--host-gpu") {
            const auto value=next_argument(argc,argv,i);if(!value)return fail(value.error());
            if(arg=="--display-backend") {
                if(*value=="auto")config.display_backend=platform::DisplayBackend::Auto;
                else if(*value=="gdi")config.display_backend=platform::DisplayBackend::Gdi;
                else if(*value=="vulkan")config.display_backend=platform::DisplayBackend::Vulkan;
                else return fail("--display-backend expects auto, gdi or vulkan");
            } else {
                if(*value=="auto")config.host_gpu=platform::HostGpu::Auto;
                else if(*value=="nvidia")config.host_gpu=platform::HostGpu::Nvidia;
                else if(*value=="intel")config.host_gpu=platform::HostGpu::Intel;
                else if(*value=="amd")config.host_gpu=platform::HostGpu::Amd;
                else return fail("--host-gpu expects auto, nvidia, intel or amd");
            }
            continue;
        }
        if(arg=="--smp-mode") {
            const auto value=next_argument(argc,argv,i);if(!value)return fail(value.error());
            if(*value!="parallel" && *value!="cooperative")return fail("--smp-mode expects parallel or cooperative");
            config.parallel_harts=*value=="parallel";continue;
        }

        if (arg == "-h" || arg == "--help") {
            config.show_help = true;
            return config;
        }
        if (arg == "-V" || arg == "--version") {
            config.show_version = true;
            return config;
        }
        // The 'run' sub-command is optional: it is only treated as one when it is
        // the very first argument and is not the name of the image.
        if (!image_seen && i == 1 && arg == "run") {
            continue;
        }
        if (arg == "--elf") {
            config.image_kind = ImageKind::Elf;
            continue;
        }
        if (arg == "--raw") {
            config.image_kind = ImageKind::Raw;
            continue;
        }
        if (arg == "-d" || arg == "--trace") {
            config.trace = true;
            continue;
        }
        if (arg == "--no-reg-dump") {
            config.dump_registers = false;
            continue;
        }
        if (arg == "--memory-map") {
            config.verbose_memory_map = true;
            continue;
        }

        if(arg=="--forward") {
            const auto v=next_argument(argc,argv,i);if(!v)return fail(v.error());
            const auto split=v->find(':');if(split==std::string::npos)return fail("--forward expects host-port:guest-port");
            const auto host=parse_u64(v->substr(0,split)),guest=parse_u64(v->substr(split+1));
            if(!host || !guest || !*host || !*guest || *host>65535 || *guest>65535)return fail("--forward ports must be 1..65535");
            for(const auto& port:config.port_forwards)if(port.host==*host)return fail("duplicate --forward host port");
            config.port_forwards.push_back({static_cast<u16>(*host),static_cast<u16>(*guest)});continue;
        }
        if (arg == "--bios" || arg == "--kernel" || arg == "--initrd" || arg == "--dtb" || arg == "--append" || arg == "--disk" || arg == "--net" || arg=="--display" || arg=="--screenshot" || arg=="--clock" || arg=="--gpu") {
            const auto v=next_argument(argc,argv,i);if(!v)return fail(v.error());
            if(arg=="--bios")config.bios_path=*v;
            if(arg=="--kernel") {config.kernel_path=*v;config.linux_boot=true;}
            if(arg=="--initrd")config.initrd_path=*v;
            if(arg=="--dtb")config.dtb_path=*v;
            if(arg=="--append")config.append=*v;
            if(arg=="--disk")config.disk_path=*v;
            if(arg=="--net") { if (*v != "user") return fail("--net expects user"); config.network=true; }
            if(arg=="--display") {if(*v!="window" && *v!="headless")return fail("--display expects window or headless");config.graphics=true;config.display_window=*v=="window";}
            if(arg=="--screenshot")config.screenshot_path=*v;
            if(arg=="--clock") {if(*v!="realtime" && *v!="virtual")return fail("--clock expects realtime or virtual");config.realtime_clock=*v=="realtime";}
            if(arg=="--gpu") {if(*v!="2d" && *v!="virgl")return fail("--gpu expects 2d or virgl");config.graphics=true;config.virgl=*v=="virgl";}
            continue;
        }
        if (arg == "--load-addr" || arg == "--entry" || arg == "-m" || arg == "--memory" || arg == "-smp" ||
            arg == "--smp" || arg == "--max-steps" || arg == "--engine" || arg == "--log-level" || arg=="--jit-workers") {
            const auto value = next_argument(argc, argv, i);
            if (!value) {
                return fail(value.error());
            }
            const std::string v = *value;
            if(arg=="--jit-workers") {const auto n=parse_u64(v);if(!n || *n>8)return fail("--jit-workers expects 0..8");config.jit_workers=static_cast<u32>(*n);continue;}

            if (arg == "--load-addr" || arg == "--entry") {
                const auto parsed = parse_u64(v);
                if (!parsed) {
                    return fail(std::string(arg) + ": " + parsed.error());
                }
                if (arg == "--entry") {
                    config.entry_override = *parsed;
                } else {
                    config.raw_load_address = *parsed;
                }
                continue;
            }
            if (arg == "-m" || arg == "--memory") {
                u64 multiplier = 1;
                std::string digits = v;
                if (!digits.empty()) {
                    const char suffix = static_cast<char>(
                        std::tolower(static_cast<unsigned char>(digits.back())));
                    if (suffix == 'k') {
                        multiplier = 1024;
                    } else if (suffix == 'm') {
                        multiplier = 1024 * 1024;
                    } else if (suffix == 'g') {
                        multiplier = 1024ull * 1024 * 1024;
                    }
                    if (multiplier != 1) {
                        digits.pop_back();
                    }
                }
                const auto parsed = parse_u64(digits);
                if (!parsed) {
                    return fail(std::string(arg) + ": " + parsed.error());
                }
                config.ram_size = *parsed * multiplier;
                continue;
            }
            if (arg == "-smp" || arg == "--smp") {
                const auto parsed = parse_u64(v);
                if (!parsed) {
                    return fail(std::string(arg) + ": " + parsed.error());
                }
                if (*parsed < 1 || *parsed > 8) {
                    return fail("--smp must be between 1 and 8");
                }
                config.hart_count = static_cast<u32>(*parsed);
                continue;
            }
            if (arg == "--max-steps") {
                const auto parsed = parse_u64(v);
                if (!parsed) {
                    return fail(std::string(arg) + ": " + parsed.error());
                }
                config.max_instructions = *parsed;
                continue;
            }
            if (arg == "--engine") {
                const std::string name = to_lower(v);
                if (name == "interpreter") {
                    config.engine = EngineKind::Interpreter;
                } else if (name == "block") {
                    config.engine = EngineKind::Block;
                } else if (name == "jit" || name == "dynarec") {
                    config.engine = EngineKind::Jit;
                } else {
                    return fail("unknown engine '" + v + "' (expected interpreter, block or jit)");
                }
                continue;
            }
            if (arg == "--log-level") {
                const std::string name = to_lower(v);
                if (name == "trace") {
                    core::set_log_level(core::LogLevel::Trace);
                } else if (name == "debug") {
                    core::set_log_level(core::LogLevel::Debug);
                } else if (name == "info") {
                    core::set_log_level(core::LogLevel::Info);
                } else if (name == "warn" || name == "warning") {
                    core::set_log_level(core::LogLevel::Warn);
                } else if (name == "error") {
                    core::set_log_level(core::LogLevel::Error);
                } else {
                    return fail("unknown log level '" + v + "'");
                }
                continue;
            }
        }

        if (!arg.empty() && arg[0] == '-') {
            return fail("unknown option '" + std::string(arg) + "'\n\n" + usage_text());
        }

        if (image_seen) {
            return fail("more than one image given ('" + config.image_path + "' and '" +
                        std::string(arg) + "')");
        }
        config.image_path = std::string(arg);
        image_seen = true;
    }

    if (!image_seen && !config.linux_boot && !config.host_vulkan_info && !config.host_vulkan_test) {
        return fail("no image given\n\n" + usage_text());
    }
    if (config.ram_size < 1024 * 1024) {
        return fail("--memory must be at least 1 MiB");
    }
    if(config.parallel_harts && config.hart_count>1 && (config.ram_size&4095))return fail("parallel SMP RAM must be a multiple of 4K");
    if(config.linux_boot && image_seen)return fail("use --kernel or a positional image, not both");
    if(config.linux_boot && config.ram_size<256ull*1024*1024)return fail("Linux boot requires at least 256M RAM");
    if(!config.port_forwards.empty() && !config.network)return fail("--forward requires --net user");
    if(config.display_backend==platform::DisplayBackend::Vulkan && (!config.graphics || !config.display_window))
        return fail("--display-backend vulkan requires --display window");
    return config;
}

} // namespace rvvm::machine
