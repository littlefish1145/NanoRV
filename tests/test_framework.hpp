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

namespace rvvm::test {

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
const char* trap_cause_name(rvvm::cpu::TrapCause cause) noexcept;

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

} // namespace rvvm::test

#define RVVM_TEST_CONCAT_INNER(a, b) a##b
#define RVVM_TEST_CONCAT(a, b) RVVM_TEST_CONCAT_INNER(a, b)

#define TEST(suite, name)                                                              \
    static void RVVM_TEST_CONCAT(rvvm_test_, __LINE__)();                             \
    static const ::rvvm::test::Registrar RVVM_TEST_CONCAT(rvvm_registrar_, __LINE__)( \
        #suite, #name, &RVVM_TEST_CONCAT(rvvm_test_, __LINE__));                     \
    static void RVVM_TEST_CONCAT(rvvm_test_, __LINE__)()

#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            ::rvvm::test::fail(__FILE__, __LINE__, "CHECK(" #condition ")"); \
        }                                                                   \
    } while (false)

#define CHECK_EQ(lhs, rhs)                                                                             \
    do {                                                                                               \
        const auto rvvm_lhs = (lhs);                                                                    \
        const auto rvvm_rhs = (rhs);                                                                    \
        if (!(rvvm_lhs == rvvm_rhs)) {                                                                  \
            ::rvvm::test::fail(__FILE__, __LINE__,                                                      \
                               std::string("CHECK_EQ(" #lhs ", " #rhs ") failed: ") +                    \
                                   ::rvvm::test::to_text(rvvm_lhs) + " != " + ::rvvm::test::to_text(rvvm_rhs)); \
        }                                                                                              \
    } while (false)

#define CHECK_NE(lhs, rhs)                                                                            \
    do {                                                                                              \
        const auto rvvm_lhs = (lhs);                                                                   \
        const auto rvvm_rhs = (rhs);                                                                   \
        if (rvvm_lhs == rvvm_rhs) {                                                                    \
            ::rvvm::test::fail(__FILE__, __LINE__,                                                     \
                               std::string("CHECK_NE(" #lhs ", " #rhs ") failed: both are ") +        \
                                   ::rvvm::test::to_text(rvvm_lhs));                                  \
        }                                                                                             \
    } while (false)