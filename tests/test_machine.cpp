// SPDX-License-Identifier: MIT
#include "machine/machine.hpp"
#include "machine/machine_config.hpp"
#include "platform/platform.hpp"
#include "test_framework.hpp"

#include <filesystem>
#include <vector>
#include <thread>

using namespace nanorv;

TEST(platform, wake_event_retains_and_consumes_notification) {
    platform::WakeEvent event;
    event.notify();
    CHECK(event.wait_ms(0));
    CHECK(!event.wait_ms(0));
    CHECK(!event.wait_ms(1));
    event.notify();event.notify();
    CHECK(event.wait_ms(0));
    CHECK(!event.wait_ms(0));
}
TEST(platform, wake_event_interrupts_deadline) {
    platform::WakeEvent event;
    std::jthread sender([&]{platform::sleep_ms(5);event.notify();});
    CHECK(event.wait_ms(2000));
}

namespace {

// Scratch files live next to the test binary so that they are easy to inspect
// when a test fails.
std::filesystem::path scratch_path(const char* name) {
    std::error_code error;
    std::filesystem::path directory = std::filesystem::temp_directory_path(error);
    if (error) {
        directory = std::filesystem::current_path();
    }
    return directory / (std::string("nanorv_test_") + name);
}

void put16(std::vector<std::uint8_t>& image, std::size_t offset, u16 value) {
    image[offset + 0] = static_cast<std::uint8_t>(value);
    image[offset + 1] = static_cast<std::uint8_t>(value >> 8);
}

void put32(std::vector<std::uint8_t>& image, std::size_t offset, u32 value) {
    for (int i = 0; i < 4; ++i) {
        image[offset + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(value >> (i * 8));
    }
}

void put64(std::vector<std::uint8_t>& image, std::size_t offset, u64 value) {
    for (int i = 0; i < 8; ++i) {
        image[offset + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(value >> (i * 8));
    }
}

// a0 = 1 ; a0 = a0 + 41 ; <last_instruction>
std::vector<std::uint8_t> build_test_elf(u32 last_instruction) {
    constexpr std::size_t kEhdrSize = 64;
    constexpr std::size_t kPhdrSize = 56;
    constexpr std::size_t kContentOffset = 0x200;
    const std::vector<u32> program = {0x00100513u, 0x02950513u, last_instruction};

    std::vector<std::uint8_t> image(kContentOffset + program.size() * 4, 0);
    image[0] = 0x7f;
    image[1] = 'E';
    image[2] = 'L';
    image[3] = 'F';
    image[4] = 2; // ELFCLASS64
    image[5] = 1; // ELFDATA2LSB
    image[6] = 1; // EV_CURRENT
    put16(image, 16, 2);    // e_type = ET_EXEC
    put16(image, 18, 243);  // e_machine = EM_RISCV
    put32(image, 20, 1);    // e_version
    put64(image, 24, kDramBase);
    put64(image, 32, kEhdrSize); // e_phoff
    put64(image, 40, 0);         // e_shoff
    put32(image, 48, 0);         // e_flags
    put16(image, 52, static_cast<u16>(kEhdrSize));
    put16(image, 54, static_cast<u16>(kPhdrSize));
    put16(image, 56, 1); // e_phnum

    put32(image, kEhdrSize + 0, 1);   // PT_LOAD
    put32(image, kEhdrSize + 4, 0x7); // PF_R | PF_W | PF_X
    put64(image, kEhdrSize + 8, kContentOffset);
    put64(image, kEhdrSize + 16, kDramBase); // p_vaddr
    put64(image, kEhdrSize + 24, kDramBase); // p_paddr
    put64(image, kEhdrSize + 32, program.size() * 4);
    put64(image, kEhdrSize + 40, program.size() * 4);
    put64(image, kEhdrSize + 48, 0x1000);

    for (std::size_t i = 0; i < program.size(); ++i) {
        put32(image, kContentOffset + i * 4, program[i]);
    }
    return image;
}

} // namespace

TEST(machine, runs_an_elf_image_until_it_traps) {
    const auto path = scratch_path("run.elf");
    std::string error;
    CHECK(platform::write_file(path.string(), build_test_elf(0x00000073u /* ecall */), &error));

    machine::MachineConfig config{};
    config.image_path = path.string();
    config.image_kind = machine::ImageKind::Elf;
    config.ram_size = 4u * 1024 * 1024;
    config.hart_count = 1;
    config.dump_registers = false;
    config.trace = false;

    machine::Machine machine(config);
    const auto loaded = machine.load_image();
    CHECK(loaded.has_value());
    CHECK_EQ(loaded->entry, kDramBase);
    CHECK_EQ(machine.hart(0).pc, kDramBase);

    const machine::RunResult result = machine.run();
    CHECK(result.trapped);
    CHECK(result.reason == machine::StopReason::Trap);
    CHECK_EQ(machine.hart(0).get_reg(10), std::uint64_t{42});
    CHECK_EQ(result.instructions, std::uint64_t{2}); // the ecall does not retire

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

TEST(machine, honours_the_instruction_limit) {
    const auto path = scratch_path("limit.elf");
    // Last instruction is a backward branch so the program would loop forever.
    const auto image = build_test_elf(0xffdff06fu /* jal zero, -4 */);
    std::string error;
    CHECK(platform::write_file(path.string(), image, &error));

    machine::MachineConfig config{};
    config.image_path = path.string();
    config.ram_size = 4u * 1024 * 1024;
    config.max_instructions = 1000;
    config.dump_registers = false;

    machine::Machine machine(config);
    CHECK(machine.load_image().has_value());
    const machine::RunResult result = machine.run();
    CHECK(!result.trapped);
    CHECK(result.reason == machine::StopReason::InstructionLimit);
    CHECK_EQ(result.instructions, std::uint64_t{1000});

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

TEST(machine, reports_a_missing_image) {
    machine::MachineConfig config{};
    config.image_path = scratch_path("does_not_exist.elf").string();
    config.ram_size = 4u * 1024 * 1024;

    machine::Machine machine(config);
    const auto loaded = machine.load_image();
    CHECK(!loaded.has_value());
    CHECK(loaded.error().find("cannot open") != std::string::npos);
}

TEST(machine, entry_override_is_applied) {
    const auto path = scratch_path("entry.elf");
    std::string error;
    CHECK(platform::write_file(path.string(), build_test_elf(0x00000073u), &error));

    machine::MachineConfig config{};
    config.image_path = path.string();
    config.ram_size = 4u * 1024 * 1024;
    config.entry_override = kDramBase + 8;
    config.max_instructions = 2;
    config.dump_registers = false;

    machine::Machine machine(config);
    const auto loaded = machine.load_image();
    CHECK(loaded.has_value());
    CHECK_EQ(machine.hart(0).pc, kDramBase + 8);

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

TEST(machine_config, parses_the_command_line) {
    const char* argv[] = {"nanorv", "run", "program.elf", "-m", "512M", "-d", "--max-steps", "1000"};
    const auto parsed = machine::parse_command_line(8, argv);
    CHECK(parsed.has_value());
    CHECK_EQ(parsed->image_path, std::string("program.elf"));
    CHECK_EQ(parsed->ram_size, std::uint64_t{512ull * 1024 * 1024});
    CHECK(parsed->trace);
    CHECK_EQ(parsed->max_instructions, std::uint64_t{1000});
    CHECK(parsed->image_kind == machine::ImageKind::Elf);
}

TEST(machine_config, loopback_forward_requires_valid_unique_ports_and_network) {
    const char* valid[]={"nanorv","--kernel","assets/linux","--net","user","--forward","46001:46000"};
    auto config=machine::parse_command_line(static_cast<int>(std::size(valid)),valid);
    CHECK(config.has_value());CHECK_EQ(config->port_forwards.size(),1ull);
    CHECK_EQ(config->port_forwards[0].host,46001u);CHECK_EQ(config->port_forwards[0].guest,46000u);
    for(const char* value:{"0:22","22:0","65536:22","22:65536","abc:22","22:3:4","22"}) {
        const char* invalid[]={"nanorv","program.elf","--net","user","--forward",value};
        CHECK(!machine::parse_command_line(static_cast<int>(std::size(invalid)),invalid));
    }
    const char* missing[]={"nanorv","program.elf","--forward","46001:22"};
    CHECK(!machine::parse_command_line(static_cast<int>(std::size(missing)),missing));
    const char* duplicate[]={"nanorv","program.elf","--net","user","--forward","46001:22","--forward","46001:80"};
    CHECK(!machine::parse_command_line(static_cast<int>(std::size(duplicate)),duplicate));
}

TEST(machine_config, rejects_unimplemented_and_invalid_options) {
    {
        const char* argv[] = {"nanorv", "run", "program.elf", "--engine", "jit"};
        const auto parsed = machine::parse_command_line(5, argv);
        CHECK(parsed.has_value());
        CHECK(parsed->engine == machine::EngineKind::Jit);
    }
    {
        const char* argv[] = {"nanorv", "run", "program.elf", "--engine", "block"};
        const auto parsed = machine::parse_command_line(5, argv);
        CHECK(parsed.has_value());
        CHECK(parsed->engine == machine::EngineKind::Block);
    }
    {
        const char* argv[] = {"nanorv", "run", "program.elf", "-smp", "4"};
        const auto parsed = machine::parse_command_line(5, argv);
        CHECK(parsed.has_value());
        CHECK_EQ(parsed->hart_count,4u);
    }
    {
        const char* argv[] = {"nanorv", "run", "program.elf", "--nonsense"};
        const auto parsed = machine::parse_command_line(5, argv);
        CHECK(!parsed.has_value());
        CHECK(parsed.error().find("unknown option") != std::string::npos);
    }
    {
        const char* argv[] = {"nanorv", "run"};
        const auto parsed = machine::parse_command_line(2, argv);
        CHECK(!parsed.has_value());
        CHECK(parsed.error().find("no image") != std::string::npos);
    }
    {
        const char* argv[] = {"nanorv", "run", "a.elf", "b.elf"};
        const auto parsed = machine::parse_command_line(5, argv);
        CHECK(!parsed.has_value());
        CHECK(parsed.error().find("more than one image") != std::string::npos);
    }
    {
        const char* argv[] = {"nanorv", "run", "a.elf", "-m", "notanumber"};
        const auto parsed = machine::parse_command_line(5, argv);
        CHECK(!parsed.has_value());
    }
}

TEST(machine_config, accepts_the_documented_forms) {
    const char* help[] = {"nanorv", "--help"};
    const auto help_config = machine::parse_command_line(2, help);
    CHECK(help_config.has_value());
    CHECK(help_config->show_help);

    const char* raw[] = {"nanorv", "run", "flat.bin", "--raw", "--load-addr", "0x80000000", "--entry",
                         "0x80000100"};
    const auto parsed = machine::parse_command_line(8, raw);
    CHECK(parsed.has_value());
    CHECK(parsed->image_kind == machine::ImageKind::Raw);
    CHECK_EQ(parsed->raw_load_address, kDramBase);
    CHECK_EQ(*parsed->entry_override, kDramBase + 0x100);

    const char* no_subcommand[] = {"nanorv", "kernel.elf"};
    const auto parsed2 = machine::parse_command_line(2, no_subcommand);
    CHECK(parsed2.has_value());
    CHECK_EQ(parsed2->image_path, std::string("kernel.elf"));
}
TEST(machine, linux_syscon_reboot_reloads_firmware_and_resets_devices) {
    const auto firmware=scratch_path("reboot_firmware.bin"),kernel=scratch_path("reboot_linux.bin");
    std::vector<u8> bios(4),image(64);
    put32(bios,0,0x00108093); // addi ra,ra,1
    put32(image,56,0x05435352);
    CHECK(platform::write_file(firmware.string(),bios,nullptr));
    CHECK(platform::write_file(kernel.string(),image,nullptr));
    machine::MachineConfig config;config.linux_boot=true;
    config.bios_path=firmware.string();config.kernel_path=kernel.string();config.max_instructions=1;
    machine::Machine vm(config);CHECK(vm.load_image().has_value());
    vm.hart(0).set_reg(1,100);
    CHECK(vm.memory().store(0x100000,32,0x7777));
    auto result=vm.run();
    CHECK(result.reason==machine::StopReason::InstructionLimit);
    CHECK_EQ(result.instructions,1ull);CHECK_EQ(vm.hart(0).get_reg(1),1ull);
    CHECK_EQ(result.exit_code,0);
    CHECK(vm.memory().store(0x100000,32,0x3333));
    result=vm.run();CHECK_EQ(result.exit_code,1);
    std::error_code ignored;std::filesystem::remove(firmware,ignored);std::filesystem::remove(kernel,ignored);
}
TEST(machine_config, host_vulkan_diagnostics_and_strict_presentation_options) {
  const char* info[]{"nanorv","--host-vulkan-info","--host-vulkan-test","--host-gpu","nvidia"};
  auto parsed=machine::parse_command_line(5,info);CHECK(parsed);
  CHECK(parsed->host_vulkan_info && parsed->host_vulkan_test);CHECK(parsed->host_gpu==platform::HostGpu::Nvidia);
  const char* window[]{"nanorv","program.bin","--display","window","--display-backend","vulkan","--host-gpu","intel"};
  parsed=machine::parse_command_line(8,window);CHECK(parsed);CHECK(parsed->display_backend==platform::DisplayBackend::Vulkan);
  const char* headless[]{"nanorv","program.bin","--display","headless","--display-backend","vulkan"};
  CHECK(!machine::parse_command_line(6,headless));
  const char* invalid[]{"nanorv","--host-vulkan-info","--host-gpu","software"};
  CHECK(!machine::parse_command_line(4,invalid));
}
