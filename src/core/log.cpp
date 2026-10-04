// SPDX-License-Identifier: MIT
#include "core/log.hpp"

#include "platform/platform.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace nanorv::core {
namespace {

LogLevel g_level = LogLevel::Info;
LogSink g_sink = nullptr;
void* g_sink_user = nullptr;

const char* level_tag(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::Trace: return "trace";
    case LogLevel::Debug: return "debug";
    case LogLevel::Info:  return "info ";
    case LogLevel::Warn:  return "warn ";
    case LogLevel::Error: return "error";
    case LogLevel::Off:   return "off  ";
    }
    return "?????";
}

} // namespace

void set_log_level(LogLevel level) noexcept { g_level = level; }
LogLevel log_level() noexcept { return g_level; }

const char* to_string(LogLevel level) noexcept { return level_tag(level); }

void set_log_sink(LogSink sink, void* user) noexcept {
    g_sink = sink;
    g_sink_user = user;
}

void log_emit(LogLevel level, std::string_view message) {
    if (static_cast<int>(level) < static_cast<int>(g_level)) {
        return;
    }
    if (g_sink != nullptr) {
        g_sink(level, message, g_sink_user);
        return;
    }
    std::string line;
    line.reserve(message.size() + 16);
    line += "[";
    line += level_tag(level);
    line += "] ";
    line += message;
    line += '\n';
    platform::write_err(line);
}

void log_printf(LogLevel level, const char* format, ...) {
    if (static_cast<int>(level) < static_cast<int>(g_level)) {
        return;
    }
    va_list args;
    va_start(args, format);
    va_list probe;
    va_copy(probe, args);
    const int needed = std::vsnprintf(nullptr, 0, format, probe);
    va_end(probe);
    if (needed < 0) {
        va_end(args);
        return;
    }
    std::vector<char> buffer(static_cast<std::size_t>(needed) + 1);
    std::vsnprintf(buffer.data(), buffer.size(), format, args);
    va_end(args);
    log_emit(level, std::string_view(buffer.data(), static_cast<std::size_t>(needed)));
}

void log_fatal(const char* format, ...) {
    va_list args;
    va_start(args, format);
    va_list probe;
    va_copy(probe, args);
    const int needed = std::vsnprintf(nullptr, 0, format, probe);
    va_end(probe);
    if (needed < 0) {
        va_end(args);
        platform::write_err("[fatal] log formatting failure\n");
        std::abort();
    }
    std::vector<char> buffer(static_cast<std::size_t>(needed) + 1);
    std::vsnprintf(buffer.data(), buffer.size(), format, args);
    va_end(args);
    log_emit(LogLevel::Error, std::string_view(buffer.data(), static_cast<std::size_t>(needed)));
    std::abort();
}

} // namespace nanorv::core