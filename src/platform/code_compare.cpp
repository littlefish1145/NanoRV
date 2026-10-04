// SPDX-License-Identifier: MIT
#include "platform/platform.hpp"
#include <cstring>
#include <cstdlib>
#ifdef NanoRV_NASM_HELPERS
extern "C" int nanorv_code_bytes_equal(const void*,const void*,std::size_t) noexcept;
extern "C" int nanorv_code_bytes_equal_avx2(const void*,const void*,std::size_t) noexcept;
#endif
namespace nanorv::platform {
namespace {
int portable_equal(const void* a,const void* b,std::size_t bytes) noexcept {
    return bytes==0 || std::memcmp(a,b,bytes)==0;
}
struct Dispatch {int(*equal)(const void*,const void*,std::size_t) noexcept;std::string_view name;};
const Dispatch& dispatch() noexcept {
    static const Dispatch selected=[]() noexcept -> Dispatch {
#ifdef NanoRV_NASM_HELPERS
        const char* setting=std::getenv("NanoRV_SIMD");
        const std::string_view mode=setting?setting:"auto";
        if(mode=="portable")return {portable_equal,"portable"};
        if(mode!="sse2" && avx2_available())return {nanorv_code_bytes_equal_avx2,"avx2"};
        return {nanorv_code_bytes_equal,"sse2"};
#else
        return {portable_equal,"portable"};
#endif
    }();
    return selected;
}
}
bool code_bytes_equal(const void* a,const void* b,std::size_t bytes) noexcept {
    return dispatch().equal(a,b,bytes)!=0;
}
std::string_view code_compare_backend() noexcept {return dispatch().name;}
}
