cmake_minimum_required(VERSION 3.24)

project(RVVM
    VERSION 0.9.0
    DESCRIPTION "Windows-native high performance RISC-V (RV64GC) virtual machine"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(RVVM_WERROR       "Treat compiler warnings as errors" OFF)
option(RVVM_BUILD_TESTS "Build the unit test suite"        ON)
option(RVVM_ASAN         "Enable address sanitizer (debug only)" OFF)
option(RVVM_NASM_HELPERS "Use optional Windows x64 SSE2/AVX2 assembly helpers" ON)

if(RVVM_NASM_HELPERS AND WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    find_program(RVVM_NASM_EXECUTABLE nasm HINTS "D:/NASM")
    if(RVVM_NASM_EXECUTABLE)
        set(CMAKE_ASM_NASM_COMPILER "${RVVM_NASM_EXECUTABLE}")
        set(CMAKE_ASM_NASM_OBJECT_FORMAT win64)
        enable_language(ASM_NASM)
        add_library(rvvm_native_helpers STATIC src/platform/windows/code_compare.asm)
        set_target_properties(rvvm_native_helpers PROPERTIES LINKER_LANGUAGE CXX)
    endif()
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# ---------------------------------------------------------------------------
# Warning configuration (MSVC and clang-cl)
# ---------------------------------------------------------------------------
add_library(rvvm_warnings INTERFACE)
target_compile_features(rvvm_warnings INTERFACE cxx_std_23)

if(MSVC)
    target_compile_options(rvvm_warnings INTERFACE
        /W4
        /permissive-
        /Zc:__cplusplus
        /utf-8
        /EHsc
        /D_CRT_SECURE_NO_WARNINGS
        /D_SILENCE_ALL_CXX17_DEPRECATION_WARNINGS
        /wd4996   # we implement <stdexcept>-free error paths on purpose
    )
    if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(rvvm_warnings INTERFACE /Zc:preprocessor /MP)
    else()
        # clang-cl ships its own preprocessor and build driver options.
        #
        # The MSVC STL that ships with VS2022 17.14 refuses to be used by older
        # clang-cl releases. The project only relies on stable, standard library
        # facilities, so the mismatch check is explicitly opted out of; drop this
        # once clang 19+ is the baseline.
        target_compile_definitions(rvvm_warnings INTERFACE _ALLOW_COMPILER_AND_STL_VERSION_MISMATCH)
    endif()
    if(RVVM_WERROR)
        target_compile_options(rvvm_warnings INTERFACE /WX)
    endif()
    if(RVVM_ASAN)
        target_compile_options(rvvm_warnings INTERFACE /fsanitize=address)
        target_link_options(rvvm_warnings INTERFACE /fsanitize=address)
    endif()
else()
    target_compile_options(rvvm_warnings INTERFACE
        -Wall -Wextra -Wpedantic
        -Wshadow -Wconversion -Wsign-conversion
        -Wno-unused-parameter
    )
    if(RVVM_WERROR)
        target_compile_options(rvvm_warnings INTERFACE -Werror)
    endif()
endif()

# ---------------------------------------------------------------------------
# rvvm_core : all machine logic, zero Win32 dependencies
# ---------------------------------------------------------------------------
add_library(rvvm_core STATIC
    src/core/log.cpp
    src/core/elf_loader.cpp
    src/cpu/csr.cpp
    src/cpu/compressed.cpp
    src/cpu/privileged.cpp
    src/cpu/execute_fp.cpp
    src/cpu/execute_bitmanip.cpp
    src/cpu/execute_crypto.cpp
    src/cpu/host_fp.cpp
    src/mmu/mmu.cpp
    src/devices/virt_devices.cpp
    src/devices/virtio.cpp
    src/devices/virtio_graphics.cpp
    src/cpu/decoder.cpp
    src/cpu/disassembler.cpp
    src/cpu/execute.cpp
    src/cpu/block_engine.cpp
    src/cpu/jit_x64.cpp
    src/cpu/ir.cpp
    src/cpu/execute_csr.cpp
    src/cpu/execute_atomic.cpp
    src/cpu/execute_muldiv.cpp
    src/cpu/trap.cpp
    src/debug/register_dump.cpp
    src/debug/trace.cpp
    src/machine/machine.cpp
    src/machine/parallel.cpp
    src/machine/fdt.cpp
    src/machine/machine_config.cpp
    src/memory/address_space.cpp
    src/memory/ram_region.cpp
    src/platform/windows/windows_platform.cpp
    src/platform/windows/windows_network.cpp
    src/platform/windows/windows_disk.cpp
    src/platform/windows/windows_display.cpp
    src/platform/windows/windows_renderer.cpp
    src/platform/windows/windows_vulkan.cpp
    src/platform/code_compare.cpp
)

if(TARGET rvvm_native_helpers)
    target_compile_definitions(rvvm_core PRIVATE RVVM_NASM_HELPERS=1)
    target_link_libraries(rvvm_core PRIVATE rvvm_native_helpers)
endif()
target_include_directories(rvvm_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(rvvm_core PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libslirp/ucrt64/include/slirp")
target_include_directories(rvvm_core PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/virgl/ucrt64/include/virgl")
target_include_directories(rvvm_core PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/vulkan-headers/include")
target_link_libraries(rvvm_core PRIVATE ws2_32 user32 gdi32 opengl32)
target_link_libraries(rvvm_core PUBLIC rvvm_warnings)
if(MSVC)
    set_source_files_properties(src/cpu/host_fp.cpp PROPERTIES COMPILE_OPTIONS "/fp:strict")
endif()
add_subdirectory(third_party/softfloat)
target_link_libraries(rvvm_core PUBLIC rvvm_softfloat)

# ---------------------------------------------------------------------------
# rvvm : the virtual machine executable
# ---------------------------------------------------------------------------
add_executable(rvvm src/main.cpp)
target_link_libraries(rvvm PRIVATE rvvm_core)
if(MSVC)
    # A linker map adds no instrumentation or runtime overhead. It allows the
    # short live sampler to identify C++ hotspots in the exact shipped binary.
    target_link_options(rvvm PRIVATE "/MAP:$<TARGET_FILE_DIR:rvvm>/rvvm.map")
endif()

# C++ whole-program optimization allows hot MMU/RAM helpers to inline across
# translation units. Debug/ASan stay local.
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    set_property(TARGET rvvm_core rvvm PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(RVVM_BUILD_TESTS)
    enable_testing()
    add_executable(rvvm_tests
        tests/test_main.cpp
        tests/test_log.cpp
        tests/test_address_space.cpp
        tests/test_ram.cpp
        tests/test_elf_loader.cpp
        tests/test_decoder.cpp
        tests/test_execute.cpp
        tests/test_muldiv.cpp
        tests/test_atomic.cpp
        tests/test_csr.cpp
        tests/test_differential.cpp
        tests/test_machine.cpp
        tests/test_linux_support.cpp
        tests/test_virtio.cpp
        tests/test_block_engine.cpp
        tests/test_jit.cpp
        tests/test_code_memory.cpp
        tests/test_network.cpp
        tests/test_smp.cpp
        tests/test_extensions.cpp
        tests/test_graphics.cpp
    )
    target_include_directories(rvvm_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_link_libraries(rvvm_tests PRIVATE rvvm_core)
    if(MSVC)
        # The assertion macros intentionally compare constant expressions.
        target_compile_options(rvvm_tests PRIVATE /wd4127)
    endif()
    add_test(NAME rvvm_tests COMMAND rvvm_tests)
    set_tests_properties(rvvm_tests PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    add_executable(rvvm_compare_benchmark scripts/benchmark-code-compare.cpp)
    target_link_libraries(rvvm_compare_benchmark PRIVATE rvvm_core)
    add_executable(rvvm_jit_inspect scripts/inspect-jit-code.cpp)
    target_link_libraries(rvvm_jit_inspect PRIVATE rvvm_core)
endif()

message(STATUS "RVVM ${PROJECT_VERSION}")
message(STATUS "  compiler      : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  build type    : ${CMAKE_BUILD_TYPE}")
message(STATUS "  tests         : ${RVVM_BUILD_TESTS}")
message(STATUS "  warnings as err: ${RVVM_WERROR}")
