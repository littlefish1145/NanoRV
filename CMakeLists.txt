cmake_minimum_required(VERSION 3.24)

project(NanoRV
    VERSION 0.9.0
    DESCRIPTION "Windows-native high performance RISC-V (RV64GC) virtual machine"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(NanoRV_WERROR       "Treat compiler warnings as errors" OFF)
option(NanoRV_BUILD_TESTS "Build the unit test suite"        ON)
option(NanoRV_ASAN         "Enable address sanitizer (debug only)" OFF)
option(NanoRV_NASM_HELPERS "Use optional Windows x64 SSE2/AVX2 assembly helpers" ON)

if(NanoRV_NASM_HELPERS AND WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    find_program(NanoRV_NASM_EXECUTABLE nasm HINTS "D:/NASM")
    if(NanoRV_NASM_EXECUTABLE)
        set(CMAKE_ASM_NASM_COMPILER "${NanoRV_NASM_EXECUTABLE}")
        set(CMAKE_ASM_NASM_OBJECT_FORMAT win64)
        enable_language(ASM_NASM)
        add_library(nanorv_native_helpers STATIC src/platform/windows/code_compare.asm)
        set_target_properties(nanorv_native_helpers PROPERTIES LINKER_LANGUAGE CXX)
    endif()
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# ---------------------------------------------------------------------------
# Warning configuration (MSVC and clang-cl)
# ---------------------------------------------------------------------------
add_library(nanorv_warnings INTERFACE)
target_compile_features(nanorv_warnings INTERFACE cxx_std_23)

if(MSVC)
    target_compile_options(nanorv_warnings INTERFACE
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
        target_compile_options(nanorv_warnings INTERFACE /Zc:preprocessor /MP)
    else()
        # clang-cl ships its own preprocessor and build driver options.
        #
        # The MSVC STL that ships with VS2022 17.14 refuses to be used by older
        # clang-cl releases. The project only relies on stable, standard library
        # facilities, so the mismatch check is explicitly opted out of; drop this
        # once clang 19+ is the baseline.
        target_compile_definitions(nanorv_warnings INTERFACE _ALLOW_COMPILER_AND_STL_VERSION_MISMATCH)
    endif()
    if(NanoRV_WERROR)
        target_compile_options(nanorv_warnings INTERFACE /WX)
    endif()
    if(NanoRV_ASAN)
        target_compile_options(nanorv_warnings INTERFACE /fsanitize=address)
        target_link_options(nanorv_warnings INTERFACE /fsanitize=address)
    endif()
else()
    target_compile_options(nanorv_warnings INTERFACE
        -Wall -Wextra -Wpedantic
        -Wshadow -Wconversion -Wsign-conversion
        -Wno-unused-parameter
    )
    if(NanoRV_WERROR)
        target_compile_options(nanorv_warnings INTERFACE -Werror)
    endif()
endif()

# ---------------------------------------------------------------------------
# nanorv_core : all machine logic, zero Win32 dependencies
# ---------------------------------------------------------------------------
add_library(nanorv_core STATIC
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

if(TARGET nanorv_native_helpers)
    target_compile_definitions(nanorv_core PRIVATE NanoRV_NASM_HELPERS=1)
    target_link_libraries(nanorv_core PRIVATE nanorv_native_helpers)
endif()
target_include_directories(nanorv_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(nanorv_core PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libslirp/ucrt64/include/slirp")
target_include_directories(nanorv_core PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/virgl/ucrt64/include/virgl")
target_include_directories(nanorv_core PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/vulkan-headers/include")
target_link_libraries(nanorv_core PRIVATE ws2_32 user32 gdi32 opengl32)
target_link_libraries(nanorv_core PUBLIC nanorv_warnings)
if(MSVC)
    set_source_files_properties(src/cpu/host_fp.cpp PROPERTIES COMPILE_OPTIONS "/fp:strict")
endif()
add_subdirectory(third_party/softfloat)
target_link_libraries(nanorv_core PUBLIC nanorv_softfloat)

# ---------------------------------------------------------------------------
# nanorv : the virtual machine executable
# ---------------------------------------------------------------------------
add_executable(nanorv src/main.cpp)
target_link_libraries(nanorv PRIVATE nanorv_core)
if(MSVC)
    # A linker map adds no instrumentation or runtime overhead. It allows the
    # short live sampler to identify C++ hotspots in the exact shipped binary.
    target_link_options(nanorv PRIVATE "/MAP:$<TARGET_FILE_DIR:nanorv>/nanorv.map")
endif()

# C++ whole-program optimization allows hot MMU/RAM helpers to inline across
# translation units. Debug/ASan stay local.
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    set_property(TARGET nanorv_core nanorv PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(NanoRV_BUILD_TESTS)
    enable_testing()
    add_executable(nanorv_tests
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
    target_include_directories(nanorv_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_link_libraries(nanorv_tests PRIVATE nanorv_core)
    if(MSVC)
        # The assertion macros intentionally compare constant expressions.
        target_compile_options(nanorv_tests PRIVATE /wd4127)
    endif()
    add_test(NAME nanorv_tests COMMAND nanorv_tests)
    set_tests_properties(nanorv_tests PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    add_executable(nanorv_compare_benchmark scripts/benchmark-code-compare.cpp)
    target_link_libraries(nanorv_compare_benchmark PRIVATE nanorv_core)
    add_executable(nanorv_jit_inspect scripts/inspect-jit-code.cpp)
    target_link_libraries(nanorv_jit_inspect PRIVATE nanorv_core)
endif()

message(STATUS "NanoRV ${PROJECT_VERSION}")
message(STATUS "  compiler      : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  build type    : ${CMAKE_BUILD_TYPE}")
message(STATUS "  tests         : ${NanoRV_BUILD_TESTS}")
message(STATUS "  warnings as err: ${NanoRV_WERROR}")
