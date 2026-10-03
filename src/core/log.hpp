// SPDX-License-Identifier: MIT
//
// Minimal levelled logger. Everything goes to stderr so that guest output on
// stdout stays clean, and a sink hook allows unit tests to capture records.
#pragma once

#include <string>
#include <string_view>

namespace rvvm::core {

enum class LogLevel : int {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
    Off = 5,
};

void set_log_level(LogLevel level) noexcept;
LogLevel log_level() noexcept;

const char* to_string(LogLevel level) noexcept;

using LogSink = void (*)(LogLevel level, std::string_view message, void* user);

// Installs a sink that receives every record that passes the level filter.
void set_log_sink(LogSink sink, void* user) noexcept;

// Emits one record. Prefer the RVVM_LOG macros.
void log_emit(LogLevel level, std::string_view message);

#ifdef __GNUC__
#define RVVM_PRINTF_FORMAT(fmt_index, first_arg) __attribute__((format(printf, fmt_index, first_arg)))
#else
#define RVVM_PRINTF_FORMAT(fmt_index, first_arg)
#endif

void log_printf(LogLevel level, const char* format, ...) RVVM_PRINTF_FORMAT(2, 3);

// Emits an Error record and terminates the process. Reserved for unrecoverable
// host resource failures (allocation, file mapping); never for guest behaviour.
[[noreturn]] void log_fatal(const char* format, ...) RVVM_PRINTF_FORMAT(1, 2);

} // namespace rvvm::core

#define RVVM_LOG(level, ...)                                            \
    do {                                                                \
        if (static_cast<int>(level) >= static_cast<int>(::rvvm::core::log_level())) { \
            ::rvvm::core::log_printf((level), __VA_ARGS__);              \
        }                                                               \
    } while (false)

#define RVVM_TRACE(...) RVVM_LOG(::rvvm::core::LogLevel::Trace, __VA_ARGS__)
#define RVVM_DEBUG(...) RVVM_LOG(::rvvm::core::LogLevel::Debug, __VA_ARGS__)
#define RVVM_INFO(...)  RVVM_LOG(::rvvm::core::LogLevel::Info, __VA_ARGS__)
#define RVVM_WARN(...)  RVVM_LOG(::rvvm::core::LogLevel::Warn, __VA_ARGS__)
#define RVVM_ERROR(...) RVVM_LOG(::rvvm::core::LogLevel::Error, __VA_ARGS__)
#define RVVM_FATAL(...) ::rvvm::core::log_fatal(__VA_ARGS__)