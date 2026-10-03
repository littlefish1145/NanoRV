// SPDX-License-Identifier: MIT
#include "test_framework.hpp"

#include "core/log.hpp"

#include <exception>
#include <vector>

namespace rvvm::test {
namespace {

std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

struct Collector {
    std::vector<std::string> failures;
};

Collector& collector() {
    static Collector state;
    return state;
}

} // namespace

const char* trap_cause_name(rvvm::cpu::TrapCause cause) noexcept {
    return rvvm::cpu::to_string(cause);
}

void register_test(const TestCase& test_case) { registry().push_back(test_case); }

void fail(const char* file, int line, const std::string& message) {
    char location[512];
    std::snprintf(location, sizeof(location), "%s:%d: %s", file, line, message.c_str());
    throw AssertionFailure{location};
}

int run_all() {
    int passed = 0;
    int failed = 0;
    std::vector<std::string> failed_names;

    for (const TestCase& test_case : registry()) {
        collector().failures.clear();
        std::string error;
        try {
            test_case.body();
        } catch (const AssertionFailure& failure) {
            error = failure.message;
        } catch (const std::exception& exception) {
            error = std::string("unexpected exception: ") + exception.what();
        } catch (...) {
            error = "unexpected non-standard exception";
        }

        if (error.empty()) {
            ++passed;
            std::printf("[  PASS  ] %s.%s\n", test_case.suite, test_case.name);
        } else {
            ++failed;
            const std::string name = std::string(test_case.suite) + "." + test_case.name;
            failed_names.push_back(name);
            std::printf("[  FAIL  ] %s\n         %s\n", name.c_str(), error.c_str());
        }
        // Flush eagerly so that a crash points at the offending test.
        std::fflush(stdout);
    }

    std::printf("\n%zu tests: %d passed, %d failed\n", registry().size(), passed, failed);
    std::fflush(stdout);
    for (const std::string& name : failed_names) {
        std::printf("  failed: %s\n", name.c_str());
    }
    std::fflush(stdout);
    return failed == 0 ? 0 : 1;
}

} // namespace rvvm::test

int main() {
    // Tests drive the logger through a sink so that expected warnings do not
    // pollute the test output.
    rvvm::core::set_log_level(rvvm::core::LogLevel::Off);
    return rvvm::test::run_all();
}