// SPDX-License-Identifier: MIT
// A local SIMD microbenchmark, not a guest or QEMU performance measurement.
#include "platform/platform.hpp"
#include <array>
#include <cstdio>
int main() {
    rvvm::platform::prefer_performance_core(0);
    std::array<unsigned char,192> a{},b{};
    for(unsigned n=0;n<a.size();++n)a[n]=b[n]=static_cast<unsigned char>(n*71);
    const auto backend=rvvm::platform::code_compare_backend();
    std::printf("backend=%.*s avx2_available=%d\n",static_cast<int>(backend.size()),backend.data(),
        rvvm::platform::avx2_available());
    constexpr unsigned iterations=4000000;
    for(unsigned bytes:{8u,16u,32u,64u,96u,128u}) {
        unsigned checksum=0;
        const auto start=rvvm::platform::monotonic_ns();
        for(unsigned n=0;n<iterations;++n)checksum+=rvvm::platform::code_bytes_equal(a.data()+(n&31),b.data()+(n&31),bytes);
        const auto elapsed=rvvm::platform::monotonic_ns()-start;
        if(checksum!=iterations)return 1;
        std::printf("bytes=%u ns_per_compare=%.3f checksum=%u\n",bytes,
            static_cast<double>(elapsed)/iterations,checksum);
    }
    return 0;
}
