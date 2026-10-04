// SPDX-License-Identifier: MIT
//
// Tiny self-contained test framework. Registration is static, so adding a test
// only requires adding a TEST() block; no external dependency, no network
// access, and it works identically under MSVC and clang-cl.
#pragma once

#include "cpu/trap.hpp"

#include <cstdio>
#include <sstream>
#include <string>
#include <string_view>

namespace nanorv::test {

struct TestCase {
    const char* suite;
    const char* name;
    void (*body)();
};

// Registry (defined in test_main.cpp).
void register_test(const TestCase& test_case);
int run_all();

// Failure reporting. Thrown by the CHECK macros so a test aborts early.
struct AssertionFailure {
    std::string message;
};

// Name of a trap cause, for failure messages.
const char* trap_cause_name(nanorv::cpu::TrapCause cause) noexcept;

[[noreturn]] void fail(const char* file, int line, const std::string& message);

template <typename T>
std::string to_text(const T& value) {
    std::ostringstream stream;
    stream << value;
    return stream.str();
}

struct Registrar {
    Registrar(const char* suite, const char* name, void (*body)()) {
        register_test(TestCase{suite, name, body});
    }
};

} // namespace nanorv::test

#define NanoRV_TEST_CONCAT_INNER(a, b) a##b
#define NanoRV_TEST_CONCAT(a, b) NanoRV_TEST_CONCAT_INNER(a, b)

#define TEST(suite, name)                                                              \
    static void NanoRV_TEST_CONCAT(nanorv_test_, __LINE__)();                             \
    static const ::nanorv::test::Registrar NanoRV_TEST_CONCAT(nanorv_registrar_, __LINE__)( \
        #suite, #name, &NanoRV_TEST_CONCAT(nanorv_test_, __LINE__));                     \
    static void NanoRV_TEST_CONCAT(nanorv_test_, __LINE__)()

#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            ::nanorv::test::fail(__FILE__, __LINE__, "CHECK(" #condition ")"); \
        }                                                                   \
    } while (false)

#define CHECK_EQ(lhs, rhs)                                                                             \
    do {                                                                                               \
        const auto nanorv_lhs = (lhs);                                                                    \
        const auto nanorv_rhs = (rhs);                                                                    \
        if (!(nanorv_lhs == nanorv_rhs)) {                                                                  \
            ::nanorv::test::fail(__FILE__, __LINE__,                                                      \
                               std::string("CHECK_EQ(" #lhs ", " #rhs ") failed: ") +                    \
                                   ::nanorv::test::to_text(nanorv_lhs) + " != " + ::nanorv::test::to_text(nanorv_rhs)); \
        }                                                                                              \
    } while (false)

#define CHECK_NE(lhs, rhs)                                                                            \
    do {                                                                                              \
        const auto nanorv_lhs = (lhs);                                                                   \
        const auto nanorv_rhs = (rhs);                                                                   \
        if (nanorv_lhs == nanorv_rhs) {                                                                    \
            ::nanorv::test::fail(__FILE__, __LINE__,                                                     \
                               std::string("CHECK_NE(" #lhs ", " #rhs ") failed: both are ") +        \
                                   ::nanorv::test::to_text(nanorv_lhs));                                  \
        }                                                                                             \
    } while (false)