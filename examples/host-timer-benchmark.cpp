#include "platform/platform.hpp"
#include <cstdio>
int main() {
    const auto start=rvvm::platform::monotonic_ns();
    for(unsigned n=0;n<100;++n)rvvm::platform::sleep_ms(1);
    std::printf("platform::sleep_ms(1): %.3f ms average (100 waits)\n",
        static_cast<double>(rvvm::platform::monotonic_ns()-start)/100000000.0);
}
