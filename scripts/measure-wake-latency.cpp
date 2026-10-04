// Compare scheduler-rounded waits with the VM's interruptible precise wait.
#include "platform/platform.hpp"
#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <vector>
int main() {
    nanorv::platform::WakeEvent event;
    std::mutex mutex;std::condition_variable cv;
    for(int mode=0;mode!=2;++mode) {
        std::vector<double> samples;
        for(int n=0;n!=100;++n) {
            const auto start=nanorv::platform::monotonic_ns();
            if(mode)event.wait_ms(1);
            else {std::unique_lock lock(mutex);cv.wait_for(lock,std::chrono::milliseconds(1),[]{return false;});}
            samples.push_back((nanorv::platform::monotonic_ns()-start)/1e6);
        }
        std::sort(samples.begin(),samples.end());
        std::printf("%s median_ms=%.3f p95_ms=%.3f\n",mode?"WakeEvent":"condition_variable",samples[50],samples[94]);
    }
}
