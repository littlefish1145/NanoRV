// SPDX-License-Identifier: MIT
#include "core/log.hpp"
#include "test_framework.hpp"

#include <string>
#include <vector>

namespace {

std::vector<std::string> g_records;

void sink(rvvm::core::LogLevel level, std::string_view message, void* user) {
    (void)level;
    (void)user;
    g_records.emplace_back(message);
}

} // namespace

TEST(log, level_filtering) {
    g_records.clear();
    rvvm::core::set_log_sink(&sink, nullptr);

    rvvm::core::set_log_level(rvvm::core::LogLevel::Warn);
    RVVM_INFO("info message");
    RVVM_WARN("warn message");
    CHECK_EQ(g_records.size(), std::size_t{1});
    CHECK_EQ(g_records[0], std::string("warn message"));

    g_records.clear();
    rvvm::core::set_log_level(rvvm::core::LogLevel::Off);
    RVVM_ERROR("error message");
    CHECK_EQ(g_records.size(), std::size_t{0});

    rvvm::core::set_log_level(rvvm::core::LogLevel::Off);
    rvvm::core::set_log_sink(nullptr, nullptr);
}

TEST(log, formats_arguments) {
    g_records.clear();
    rvvm::core::set_log_sink(&sink, nullptr);
    rvvm::core::set_log_level(rvvm::core::LogLevel::Debug);

    RVVM_DEBUG("pc=0x%llx name=%s count=%d", static_cast<unsigned long long>(0xdeadbeefull), "hart0",
               42);
    CHECK_EQ(g_records.size(), std::size_t{1});
    CHECK_EQ(g_records[0], std::string("pc=0xdeadbeef name=hart0 count=42"));

    g_records.clear();
    RVVM_DEBUG("long %s", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    CHECK_EQ(g_records.size(), std::size_t{1});
    CHECK_EQ(g_records[0].size(), std::size_t{5 + 74});

    // Restore the state the rest of the suite expects.
    rvvm::core::set_log_level(rvvm::core::LogLevel::Off);
    rvvm::core::set_log_sink(nullptr, nullptr);
}