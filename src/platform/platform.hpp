// SPDX-License-Identifier: MIT
//
// Platform abstraction interface.
//
// RULE: nothing in core/ isa/ memory/ devices/ machine/ jit/ debug/ may include
// a platform header other than this one. The only backend currently shipping is
// src/platform/windows. All Win32 types stay inside that translation unit.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nanorv::platform {

// Page protection requested from the host OS.
enum class Protect : std::uint32_t {
    ReadWrite = 0,      // PAGE_READWRITE
    ReadExecute = 1,    // PAGE_EXECUTE_READ
    ReadWriteExecute = 2, // PAGE_EXECUTE_READWRITE (transient JIT use only, never permanent)
    NoAccess = 3,       // PAGE_NOACCESS, also used for boundary regression tests
};

inline constexpr std::size_t kPageSize = 4096;

// Round `bytes` up to a whole number of pages.
constexpr std::size_t align_up_pages(std::size_t bytes) noexcept {
    return (bytes + kPageSize - 1) & ~(kPageSize - 1);
}

// Host virtual memory. `ptr` is always page aligned when returned non-null.
void* allocate(std::size_t bytes, Protect protect);
void release(void* ptr, std::size_t bytes) noexcept;
bool protect(void* ptr, std::size_t bytes, Protect protect) noexcept;

// Shared backing with separate RW/non-executable and RX/read-only views.
// Generated code is copied through writable_data(), then published before its
// executable address becomes visible to any CPU thread. No view is RWX.
class CodeMemory {
public:
    explicit CodeMemory(std::size_t bytes);
    ~CodeMemory();
    CodeMemory(const CodeMemory&)=delete;
    CodeMemory& operator=(const CodeMemory&)=delete;
    void* writable_data() const noexcept;
    const void* executable_data() const noexcept;
    std::size_t size() const noexcept;
    bool publish(std::size_t offset,std::size_t bytes) const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// File IO. `read_file` returns the whole file; error string is filled on failure.
using ByteBuffer = std::vector<std::uint8_t>;
std::optional<ByteBuffer> read_file(const std::string& path, std::string* error);
bool write_file(const std::string& path, std::span<const std::uint8_t> data, std::string* error);

// Console / stderr output (unbuffered, no re-entrancy guarantees).
void write_out(std::string_view text);
void write_err(std::string_view text);
void initialize_console();
std::optional<std::uint8_t> read_input_byte() noexcept;

// Monotonic clock in nanoseconds; never goes backwards, unaffected by NTP.
std::uint64_t monotonic_ns() noexcept;
// Low-overhead ordered timestamp for relative profiling. On x64 these are
// invariant-TSC ticks, not turbo core cycles or nanoseconds; compare fractions.
std::uint64_t profiling_ticks() noexcept;
std::uint64_t thread_cpu_ns() noexcept;
std::uint64_t thread_id() noexcept;
std::uint64_t multiply_high_unsigned(std::uint64_t,std::uint64_t) noexcept;
// Compare immutable snapshots, reading exactly bytes from each input. The SSE2
// helper is optional; guest RAM must first be read with AddressSpace atomics.
bool code_bytes_equal(const void*,const void*,std::size_t bytes) noexcept;
bool avx2_available() noexcept;
std::string_view code_compare_backend() noexcept;
// Scheduler hint only; preserves the thread's allowed processor affinity.
void prefer_performance_core(std::uint32_t index) noexcept;
void sleep_ms(std::uint32_t milliseconds) noexcept;

// Auto-reset notification with an interruptible high-resolution deadline.
// Notifications before wait() are retained; concurrent notifications coalesce.
class WakeEvent {
public:
    WakeEvent();
    ~WakeEvent();
    WakeEvent(const WakeEvent&) = delete;
    WakeEvent& operator=(const WakeEvent&) = delete;
    void notify() noexcept;
    // An optional backend event can wake the same wait (e.g. Winsock). The
    // owner of a manual-reset event is responsible for consuming/resetting it.
    bool wait_ms(std::uint32_t milliseconds,void* additional_event=nullptr) noexcept;
private:
    void* event_ = nullptr;
    void* timer_ = nullptr;
};

// Number of hardware threads on the host.
std::uint32_t host_cpu_count() noexcept;

// Last platform level error description for the calling thread.
std::string last_error();

} // namespace nanorv::platform
