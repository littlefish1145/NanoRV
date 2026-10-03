// SPDX-License-Identifier: MIT
//
// Windows backend for the platform abstraction. This is the ONLY translation
// unit in the project that includes <windows.h>.
#include "platform/platform.hpp"

#include <windows.h>

#include <cstring>
#include <stdexcept>
#include <intrin.h>
#include <array>
#include <deque>

namespace rvvm::platform {
std::uint64_t profiling_ticks() noexcept {
#if defined(_M_X64)
    _mm_lfence();const auto timestamp=__rdtsc();_mm_lfence();return timestamp;
#else
    return monotonic_ns();
#endif
}
bool avx2_available() noexcept {
    static const bool available=[] {
#if defined(_M_X64)
        int values[4]{};__cpuid(values,0);
        if(values[0]<7)return false;
        __cpuidex(values,1,0);
        constexpr unsigned required=(1u<<26)|(1u<<27)|(1u<<28); // XSAVE/OSXSAVE/AVX
        if((static_cast<unsigned>(values[2])&required)!=required)return false;
        // XGETBV is legal only after OSXSAVE is set. Both XMM and YMM state
        // must be enabled by the OS, regardless of the hardware's AVX2 bit.
        if((_xgetbv(0)&6)!=6)return false;
        __cpuidex(values,7,0);
        return (static_cast<unsigned>(values[1])&(1u<<5))!=0;
#else
        return false;
#endif
    }();
    return available;
}
void prefer_performance_core(std::uint32_t index) noexcept {
    struct Core {WORD group;BYTE number,core,efficiency;};
    static const auto cores=[]() noexcept {
        std::vector<Core> result;
        try {
            const auto query=reinterpret_cast<decltype(&GetSystemCpuSetInformation)>(
                GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"GetSystemCpuSetInformation"));
            if(!query)return result;
            ULONG size=0;query(nullptr,0,&size,GetCurrentProcess(),0);
            if(!size)return result;
            std::vector<std::uint8_t> buffer(size);
            if(!query(reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buffer.data()),size,&size,GetCurrentProcess(),0))return result;
            for(std::size_t offset=0;offset+8<=size;) {
                const auto* set=reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(buffer.data()+offset);
                if(set->Size<8 || set->Size>size-offset)break;
                if(set->Type==CpuSetInformation && set->Size>=sizeof(SYSTEM_CPU_SET_INFORMATION) &&
                    !(set->CpuSet.Allocated && !set->CpuSet.AllocatedToTargetProcess)) {
                    result.push_back({set->CpuSet.Group,set->CpuSet.LogicalProcessorIndex,set->CpuSet.CoreIndex,set->CpuSet.EfficiencyClass});
                }
                offset+=set->Size;
            }
        } catch(...) {result.clear();}
        return result;
    }();
    GROUP_AFFINITY allowed{};
    if(!GetThreadGroupAffinity(GetCurrentThread(),&allowed))return;
    const auto available=[&](const Core& core) {
        return core.group==allowed.Group && core.number<sizeof(KAFFINITY)*8 &&
            (allowed.Mask&(KAFFINITY{1}<<core.number));
    };
    BYTE maximum=0;for(const auto& core:cores)if(available(core) && core.efficiency>maximum)maximum=core.efficiency;
    std::array<bool,256> used{};std::array<PROCESSOR_NUMBER,256> selected{};unsigned count=0;
    for(const auto& core:cores)if(available(core) && core.efficiency==maximum && !used[core.core]) {
        used[core.core]=true;selected[count++]={core.group,core.number,0};
    }
    if(count)SetThreadIdealProcessorEx(GetCurrentThread(),&selected[index%count],nullptr);
}
std::uint64_t multiply_high_unsigned(std::uint64_t left,std::uint64_t right) noexcept {
#if defined(_M_X64)
    std::uint64_t high=0;
    (void)_umul128(left,right,&high);
    return high;
#else
    const auto a=left&0xffffffffu,b=left>>32,c=right&0xffffffffu,d=right>>32;
    const auto low=a*c,mid1=a*d,mid2=b*c;
    return b*d+(mid1>>32)+(mid2>>32)+(((low>>32)+(mid1&0xffffffffu)+(mid2&0xffffffffu))>>32);
#endif
}
std::uint64_t thread_id() noexcept {return GetCurrentThreadId();}
std::uint64_t thread_cpu_ns() noexcept {
    FILETIME created{},exited{},kernel{},user{};
    if(!GetThreadTimes(GetCurrentThread(),&created,&exited,&kernel,&user))return 0;
    const auto ticks=[](FILETIME v) {return (static_cast<std::uint64_t>(v.dwHighDateTime)<<32)|v.dwLowDateTime;};
    return (ticks(kernel)+ticks(user))*100;
}
namespace {

DWORD to_page_protect(Protect p) noexcept {
    switch (p) {
    case Protect::ReadWrite:       return PAGE_READWRITE;
    case Protect::ReadExecute:     return PAGE_EXECUTE_READ;
    case Protect::ReadWriteExecute: return PAGE_EXECUTE_READWRITE;
    case Protect::NoAccess: return PAGE_NOACCESS;
    }
    return PAGE_READWRITE;
}

std::string format_error(DWORD code) {
    char* buffer = nullptr;
    const DWORD len = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<char*>(&buffer), 0, nullptr);
    std::string message;
    if (len != 0 && buffer != nullptr) {
        message.assign(buffer, len);
        while (!message.empty() && (message.back() == '\n' || message.back() == '\r')) {
            message.pop_back();
        }
    } else {
        message = "unknown error (" + std::to_string(code) + ")";
    }
    if (buffer != nullptr) {
        LocalFree(buffer);
    }
    return message;
}

LARGE_INTEGER qpc_frequency() {
    static const LARGE_INTEGER frequency = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return f;
    }();
    return frequency;
}

} // namespace

void* allocate(std::size_t bytes, Protect protect) {
    if (bytes == 0) {
        return nullptr;
    }
    const std::size_t size = align_up_pages(bytes);
    void* const ptr = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, to_page_protect(protect));
    return ptr;
}

void release(void* ptr, std::size_t bytes) noexcept {
    if (ptr == nullptr || bytes == 0) {
        return;
    }
    VirtualFree(ptr, 0, MEM_RELEASE);
}

bool protect(void* ptr, std::size_t bytes, Protect p) noexcept {
    if (ptr == nullptr || bytes == 0) {
        return false;
    }
    DWORD old = 0;
    if (!VirtualProtect(ptr, align_up_pages(bytes), to_page_protect(p), &old)) return false;
    if (p == Protect::ReadExecute) return FlushInstructionCache(GetCurrentProcess(),ptr,bytes) != 0;
    return true;
}

struct CodeMemory::Impl {
    HANDLE section=nullptr;
    void* writable=nullptr;
    void* executable=nullptr;
    std::size_t bytes=0;
    ~Impl() {
        if(executable)UnmapViewOfFile(executable);
        if(writable)UnmapViewOfFile(writable);
        if(section)CloseHandle(section);
    }
};
CodeMemory::CodeMemory(std::size_t bytes):impl_(std::make_unique<Impl>()) {
    if(!bytes || bytes>SIZE_MAX-(kPageSize-1))return;
    const auto aligned=align_up_pages(bytes);
    const auto size=static_cast<std::uint64_t>(aligned);
    // PAGE_EXECUTE_READWRITE is the section's maximum permitted protection,
    // not a mapped view. Both actual views have disjoint W and X permissions.
    impl_->section=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_EXECUTE_READWRITE,
        static_cast<DWORD>(size>>32),static_cast<DWORD>(size),nullptr);
    if(!impl_->section)return;
    impl_->writable=MapViewOfFile(impl_->section,FILE_MAP_READ|FILE_MAP_WRITE,0,0,aligned);
    if(!impl_->writable)return;
    impl_->executable=MapViewOfFile(impl_->section,FILE_MAP_READ|FILE_MAP_EXECUTE,0,0,aligned);
    if(!impl_->executable)return;
    impl_->bytes=aligned;
}
CodeMemory::~CodeMemory()=default;
void* CodeMemory::writable_data() const noexcept {return impl_->bytes?impl_->writable:nullptr;}
const void* CodeMemory::executable_data() const noexcept {return impl_->bytes?impl_->executable:nullptr;}
std::size_t CodeMemory::size() const noexcept {return impl_->bytes;}
bool CodeMemory::publish(std::size_t offset,std::size_t bytes) const noexcept {
    if(!impl_->bytes || offset>impl_->bytes || bytes>impl_->bytes-offset)return false;
    return FlushInstructionCache(GetCurrentProcess(),static_cast<const std::uint8_t*>(impl_->executable)+offset,bytes)!=0;
}

std::optional<ByteBuffer> read_file(const std::string& path, std::string* error) {
    HANDLE file = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (error != nullptr) {
            *error = "cannot open '" + path + "': " + format_error(GetLastError());
        }
        return std::nullopt;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size)) {
        if (error != nullptr) {
            *error = "cannot size '" + path + "': " + format_error(GetLastError());
        }
        CloseHandle(file);
        return std::nullopt;
    }
    if (size.QuadPart < 0 || static_cast<std::uint64_t>(size.QuadPart) > (1ull << 32)) {
        if (error != nullptr) {
            *error = "'" + path + "' is larger than 4 GiB, refusing to load";
        }
        CloseHandle(file);
        return std::nullopt;
    }

    ByteBuffer buffer(static_cast<std::size_t>(size.QuadPart));
    std::size_t done = 0;
    while (done < buffer.size()) {
        const DWORD chunk =
            static_cast<DWORD>((buffer.size() - done) > 0x10000000u ? 0x10000000u : (buffer.size() - done));
        DWORD read = 0;
        if (!ReadFile(file, buffer.data() + done, chunk, &read, nullptr) || read == 0) {
            if (error != nullptr) {
                *error = "short read on '" + path + "' after " + std::to_string(done) + " bytes";
            }
            CloseHandle(file);
            return std::nullopt;
        }
        done += read;
    }
    CloseHandle(file);
    return buffer;
}

bool write_file(const std::string& path, std::span<const std::uint8_t> data, std::string* error) {
    HANDLE file = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (error != nullptr) {
            *error = "cannot create '" + path + "': " + format_error(GetLastError());
        }
        return false;
    }
    std::size_t done = 0;
    bool ok = true;
    while (done < data.size()) {
        const DWORD chunk = static_cast<DWORD>(
            (data.size() - done) > 0x10000000u ? 0x10000000u : (data.size() - done));
        DWORD written = 0;
        if (!WriteFile(file, data.data() + done, chunk, &written, nullptr) || written != chunk) {
            if (error != nullptr) {
                *error = "short write on '" + path + "' after " + std::to_string(done) + " bytes";
            }
            ok = false;
            break;
        }
        done += written;
    }
    CloseHandle(file);
    return ok;
}

void initialize_console() {
    struct ConsoleSession {
        HANDLE input=GetStdHandle(STD_INPUT_HANDLE),output=GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD input_mode=0,output_mode=0;
        UINT input_cp=GetConsoleCP(),output_cp=GetConsoleOutputCP();
        bool has_input=GetConsoleMode(input,&input_mode)!=0;
        bool has_output=GetConsoleMode(output,&output_mode)!=0;
        ConsoleSession() {
            if(has_input) {
                SetConsoleCP(CP_UTF8);
                SetConsoleMode(input,input_mode&~(ENABLE_LINE_INPUT|ENABLE_ECHO_INPUT|ENABLE_PROCESSED_INPUT));
            }
            if(has_output) {
                SetConsoleOutputCP(CP_UTF8);
                SetConsoleMode(output,output_mode|ENABLE_VIRTUAL_TERMINAL_PROCESSING);
            }
        }
        ~ConsoleSession() {
            if(has_input) {SetConsoleMode(input,input_mode);SetConsoleCP(input_cp);}
            if(has_output) {SetConsoleMode(output,output_mode);SetConsoleOutputCP(output_cp);}
        }
    };
    static ConsoleSession session;
}
void write_out(std::string_view text) {
    const auto output=GetStdHandle(STD_OUTPUT_HANDLE);DWORD mode=0;
    if(GetConsoleMode(output,&mode)) {
        // UART batches can end in the middle of a UTF-8 character. Retain that
        // suffix and use the Unicode console API instead of a legacy code page.
        static std::string pending;
        pending.append(text);
        auto complete=pending.size();
        if(complete) {
            auto start=complete-1;
            while(start && (static_cast<unsigned char>(pending[start])&0xc0)==0x80 && complete-start<4)--start;
            const auto lead=static_cast<unsigned char>(pending[start]);
            const std::size_t expected=(lead&0xe0)==0xc0?2:(lead&0xf0)==0xe0?3:(lead&0xf8)==0xf0?4:1;
            if(complete-start<expected)complete=start;
        }
        if(!complete)return;
        const auto count=MultiByteToWideChar(CP_UTF8,0,pending.data(),static_cast<int>(complete),nullptr,0);
        if(count>0) {
            std::wstring wide(static_cast<std::size_t>(count),L'\0');
            MultiByteToWideChar(CP_UTF8,0,pending.data(),static_cast<int>(complete),wide.data(),count);
            for(std::size_t offset=0;offset<wide.size();) {
                DWORD written=0;
                if(!WriteConsoleW(output,wide.data()+offset,static_cast<DWORD>(wide.size()-offset),&written,nullptr) || !written)break;
                offset+=written;
            }
        }
        pending.erase(0,complete);return;
    }
    std::size_t done = 0;
    while (done < text.size()) {
        const DWORD chunk = static_cast<DWORD>(
            (text.size() - done) > 0x10000000u ? 0x10000000u : (text.size() - done));
        DWORD written = 0;
        if (!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text.data() + done, chunk, &written, nullptr)) {
            return;
        }
        if (written == 0) {
            return;
        }
        done += written;
    }
}

void write_err(std::string_view text) {
    std::size_t done = 0;
    while (done < text.size()) {
        const DWORD chunk = static_cast<DWORD>(
            (text.size() - done) > 0x10000000u ? 0x10000000u : (text.size() - done));
        DWORD written = 0;
        if (!WriteFile(GetStdHandle(STD_ERROR_HANDLE), text.data() + done, chunk, &written, nullptr)) {
            return;
        }
        if (written == 0) {
            return;
        }
        done += written;
    }
}

std::uint64_t monotonic_ns() noexcept {
    LARGE_INTEGER counter{};
    if (!QueryPerformanceCounter(&counter)) {
        return 0;
    }
    const LARGE_INTEGER freq = qpc_frequency();
    if (freq.QuadPart == 0) {
        return 0;
    }
    // Split to keep full precision for large counter values.
    const std::int64_t seconds = counter.QuadPart / freq.QuadPart;
    const std::int64_t remainder = counter.QuadPart % freq.QuadPart;
    return static_cast<std::uint64_t>(seconds) * 1'000'000'000ull +
           static_cast<std::uint64_t>(remainder) * 1'000'000'000ull /
               static_cast<std::uint64_t>(freq.QuadPart);
}

WakeEvent::WakeEvent() {
    event_=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    timer_=CreateWaitableTimerExW(nullptr,nullptr,
        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_MODIFY_STATE|SYNCHRONIZE);
    if(!event_) {
        if(timer_)CloseHandle(timer_);
        throw std::runtime_error("cannot create wake event");
    }
}
WakeEvent::~WakeEvent() {
    if(timer_)CloseHandle(timer_);
    if(event_)CloseHandle(event_);
}
void WakeEvent::notify() noexcept {SetEvent(event_);}
bool WakeEvent::wait_ms(std::uint32_t milliseconds,void* additional_event) noexcept {
    HANDLE signals[]{event_,additional_event};
    const DWORD signal_count=additional_event?2u:1u;
    if(!milliseconds)return WaitForMultipleObjects(signal_count,signals,FALSE,0)<WAIT_OBJECT_0+signal_count;
    LARGE_INTEGER due{};due.QuadPart=-static_cast<LONGLONG>(milliseconds)*10000;
    if(timer_ && SetWaitableTimer(timer_,&due,0,nullptr,nullptr,FALSE)) {
        HANDLE handles[]{event_,additional_event,timer_};
        if(!additional_event)handles[1]=timer_;
        const auto result=WaitForMultipleObjects(signal_count+1,handles,FALSE,INFINITE);
        CancelWaitableTimer(timer_);
        return result<WAIT_OBJECT_0+signal_count;
    }
    const auto result=WaitForMultipleObjects(signal_count,signals,FALSE,milliseconds);
    return result<WAIT_OBJECT_0+signal_count;
}
void sleep_ms(std::uint32_t milliseconds) noexcept {
    if (!milliseconds) {Sleep(0);return;}
    struct Timer {
        HANDLE handle=CreateWaitableTimerExW(nullptr,nullptr,
            CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_MODIFY_STATE|SYNCHRONIZE);
        ~Timer() {if(handle)CloseHandle(handle);}
    };
    thread_local Timer timer;
    LARGE_INTEGER due{};due.QuadPart=-static_cast<LONGLONG>(milliseconds)*10000;
    if(timer.handle && SetWaitableTimer(timer.handle,&due,0,nullptr,nullptr,FALSE)) {
        if(WaitForSingleObject(timer.handle,INFINITE)==WAIT_OBJECT_0)return;
    }
    Sleep(milliseconds); // older Windows builds may lack high-resolution timers
}
std::optional<std::uint8_t> read_input_byte() noexcept {
    initialize_console();
    HANDLE input=GetStdHandle(STD_INPUT_HANDLE); DWORD mode=0;
    if(GetConsoleMode(input,&mode)) {
        static std::deque<std::uint8_t> bytes;
        static WCHAR high_surrogate=0;
        if(!bytes.empty()) {const auto byte=bytes.front();bytes.pop_front();return byte;}
        DWORD count=0; GetNumberOfConsoleInputEvents(input,&count);
        while(count--) {
            INPUT_RECORD rec{};DWORD n=0;if(!ReadConsoleInputW(input,&rec,1,&n) || !n)break;
            if(rec.EventType!=KEY_EVENT || !rec.Event.KeyEvent.bKeyDown)continue;
            const WCHAR ch=rec.Event.KeyEvent.uChar.UnicodeChar;
            if(!ch)continue;
            if(ch>=0xd800 && ch<=0xdbff) {high_surrogate=ch;continue;}
            WCHAR wide[2]{ch,0};int length=1;
            if(ch>=0xdc00 && ch<=0xdfff && high_surrogate) {wide[0]=high_surrogate;wide[1]=ch;length=2;}
            high_surrogate=0;
            char encoded[8]{};const auto size=WideCharToMultiByte(CP_UTF8,0,wide,length,encoded,sizeof(encoded),nullptr,nullptr);
            for(unsigned repeat=0;repeat<rec.Event.KeyEvent.wRepeatCount;++repeat)
                for(int k=0;k<size;++k)bytes.push_back(static_cast<std::uint8_t>(encoded[k]));
            if(!bytes.empty()) {const auto byte=bytes.front();bytes.pop_front();return byte;}
        }
        return std::nullopt;
    }
    DWORD available=0;
    if(GetFileType(input)==FILE_TYPE_PIPE && !PeekNamedPipe(input,nullptr,0,nullptr,&available,nullptr))return std::nullopt;
    if(GetFileType(input)==FILE_TYPE_PIPE && !available)return std::nullopt;
    if(GetFileType(input)!=FILE_TYPE_PIPE && GetFileType(input)!=FILE_TYPE_DISK)return std::nullopt;
    std::uint8_t byte;DWORD n=0; if(ReadFile(input,&byte,1,&n,nullptr)&&n==1)return byte;return std::nullopt;
}

std::uint32_t host_cpu_count() noexcept {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const auto count = static_cast<std::uint32_t>(info.dwNumberOfProcessors);
    return count == 0 ? 1u : count;
}

std::string last_error() {
    return format_error(GetLastError());
}

} // namespace rvvm::platform
