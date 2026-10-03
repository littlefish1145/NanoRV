// SPDX-License-Identifier: MIT
#include "cpu/csr.hpp"

#include "platform/platform.hpp"

#include <cstdio>

namespace rvvm::cpu {
namespace {

// Stable descriptor storage, indexed by find_csr() for kernel CSR accesses.
constexpr CsrInfo kCsrs[] = {
    {kCsrFflags, "fflags", Privilege::User, true, 0x1f},
    {kCsrFrm, "frm", Privilege::User, true, 0x07},
    {kCsrFcsr, "fcsr", Privilege::User, true, 0xff},
    {0x100,"sstatus",Privilege::Supervisor,true,~0ull},
    {0x104,"sie",Privilege::Supervisor,true,0x222},
    {0x105,"stvec",Privilege::Supervisor,true,~0ull},
    {0x106,"scounteren",Privilege::Supervisor,true,7},
    {0x140,"sscratch",Privilege::Supervisor,true,~0ull},
    {0x141,"sepc",Privilege::Supervisor,true,~1ull},
    {0x142,"scause",Privilege::Supervisor,true,~0ull},
    {0x143,"stval",Privilege::Supervisor,true,~0ull},
    {0x144,"sip",Privilege::Supervisor,true,2},
    {0x180,"satp",Privilege::Supervisor,true,~0ull},
    {0x300,"mstatus",Privilege::Machine,true,~0ull},
    {0x301,"misa",Privilege::Machine,true,0},
    {0x302,"medeleg",Privilege::Machine,true,0xb3ff},
    {0x303,"mideleg",Privilege::Machine,true,0x222},
    {0x304,"mie",Privilege::Machine,true,0xaaa},
    {0x305,"mtvec",Privilege::Machine,true,~0ull},
    {0x306,"mcounteren",Privilege::Machine,true,7},
    {0x320,"mcountinhibit",Privilege::Machine,true,0}, // counters are not inhibitable
    {0x340,"mscratch",Privilege::Machine,true,~0ull},
    {0x341,"mepc",Privilege::Machine,true,~1ull},
    {0x342,"mcause",Privilege::Machine,true,~0ull},
    {0x343,"mtval",Privilege::Machine,true,~0ull},
    {0x344,"mip",Privilege::Machine,true,0x222},
    {0xb00,"mcycle",Privilege::Machine,true,~0ull},
    {0xb02,"minstret",Privilege::Machine,true,~0ull},
    {kCsrCycle, "cycle", Privilege::User, false, 0},
    {kCsrTime, "time", Privilege::User, false, 0},
    {kCsrInstret, "instret", Privilege::User, false, 0},
    {kCsrMvendorid, "mvendorid", Privilege::Machine, false, 0},
    {kCsrMarchid, "marchid", Privilege::Machine, false, 0},
    {kCsrMimpid, "mimpid", Privilege::Machine, false, 0},
    {kCsrMhartid, "mhartid", Privilege::Machine, false, 0},
};

// Only CSR numbers that actually exist may be accessed. Note that the
// floating point CSRs live at 0x001-0x003, i.e. their low address bits are not
// zero, so there is no blanket "csr[1:0] must be zero" rule; an unimplemented
// address is simply not in the table. Machine level registers are gated by the
// privilege check, which is how csr[11:10] == 0b11 is enforced.
bool privilege_allows(const CsrInfo& info, Privilege privilege) noexcept {
    return privilege_at_least(privilege, info.privilege);
}

} // namespace

const CsrInfo* find_csr(u32 address) noexcept {
    static const auto pmp = [] {
        std::array<CsrInfo,18> table{};
        table[0]={0x3a0,"pmpcfg0",Privilege::Machine,true,~0ull};
        table[1]={0x3a2,"pmpcfg2",Privilege::Machine,true,~0ull};
        for(u32 n=0;n<16;++n) table[n+2]={0x3b0+n,"pmpaddr",Privilege::Machine,true,(1ull<<54)-1};
        return table;
    }();
    static const auto index=[] {
        std::array<const CsrInfo*,4096> table{};
        for(const auto& info:pmp)table[info.address]=&info;
        for(const auto& info:kCsrs)table[info.address]=&info;
        return table;
    }();
    return address<index.size()?index[address]:nullptr;
}

const char* csr_name(u32 address) noexcept {
    const CsrInfo* const info = find_csr(address);
    return info != nullptr ? info->name : "unknown";
}

u64 read_timebase_ticks(u64 timebase_frequency) noexcept {
    const u64 nanoseconds = platform::monotonic_ns();
    // Split the multiply so that a large frequency cannot overflow.
    const u64 seconds = nanoseconds / 1'000'000'000ull;
    const u64 remainder = nanoseconds % 1'000'000'000ull;
    return seconds * timebase_frequency + (remainder * timebase_frequency) / 1'000'000'000ull;
}

bool CsrFile::read(u32 address, Privilege privilege, u64 instret, u64& value) const {
    const CsrInfo* const info = find_csr(address);
    if (info == nullptr || !privilege_allows(*info, privilege)) {
        return false;
    }

    if (address>=0xc00 && address<=0xc02 && privilege != Privilege::Machine) {
        const u64 mask=1ull<<(address-0xc00);
        if (!(raw(kCsrMcounteren)&mask) || (privilege==Privilege::User && !(raw(kCsrScounteren)&mask))) return false;
    }
    constexpr u64 smask=0x80000003000de762ull;
    switch (address) {
    case kCsrSstatus: value=raw(kCsrMstatus)&smask; return true;
    case kCsrSie: value=raw(kCsrMie)&raw(kCsrMideleg); return true;
    case kCsrSip: value=raw(kCsrMip)&raw(kCsrMideleg); return true;
    case 0xb00: value=instret+raw(0xb00); return true;
    case 0xb02: value=instret+raw(0xb02); return true;
    case kCsrFflags: value = fflags(); return true;
    case kCsrFrm: value = frm(); return true;
    case kCsrFcsr: value = fcsr_; return true;
    case kCsrCycle: value = instret+raw(0xb00); return true;
    case kCsrTime: value = virtual_clock ? ticks : read_timebase_ticks(timebase_frequency_); return true;
    case kCsrInstret: value = instret+raw(0xb02); return true;
    case kCsrMvendorid: value = kVendorId; return true;
    case kCsrMarchid: value = kArchId; return true;
    case kCsrMimpid: value = kImplId; return true;
    case kCsrMhartid: value = hart_id_; return true;
    default: value=raw(address); return true;
    }
}

bool CsrFile::write(u32 address, Privilege privilege, u64 value, u64 instret) {
    const CsrInfo* const info = find_csr(address);
    if (info == nullptr || !info->writable || !privilege_allows(*info, privilege)) {
        return false;
    }
    const u64 masked = value & info->write_mask;

    switch (address) {
    case 0xb00: case 0xb02: set_raw(address,value-instret); return true;
    case kCsrMisa: return true; // fixed supported ISA, WARL
    case kCsrMstatus: case kCsrSstatus: {
        const u64 mask=address==kCsrSstatus?0xde762ull:0x7fffeaull;
        u64 v=(raw(kCsrMstatus)&~mask)|(value&mask);
        if(((v>>11)&3)==2) v&=~(3ull<<11);
        v&=~(1ull<<63);
        if(((v>>13)&3)==3 || ((v>>15)&3)==3) v|=1ull<<63;
        set_raw(kCsrMstatus,v); return true;
    }
    case kCsrSie: set_raw(kCsrMie,(raw(kCsrMie)&~raw(kCsrMideleg))|(masked&raw(kCsrMideleg))); return true;
    case kCsrSip: set_raw(kCsrMip,(raw(kCsrMip)&~2ull)|(masked&raw(kCsrMideleg)&2)); return true;
    case kCsrMip: set_raw(address,(raw(address)&~0x222ull)|masked); return true;
    case kCsrMtvec: case kCsrStvec: set_raw(address,(masked&~3ull)|((masked&3)==1?1:0)); return true;
    case kCsrSatp: if((value>>60)==0 || (value>>60)==8 || (value>>60)==9) set_raw(address,masked); return true;
    case kCsrFflags: set_fflags(masked); return true;
    case kCsrFrm: set_frm(masked); return true;
    case kCsrFcsr: set_fcsr(masked); return true;
    default:
        if(address==0x3a0 || address==0x3a2) {
            u64 v=raw(address);
            for(unsigned n=0;n<8;++n) {
                const unsigned shift=n*8;
                if(v&(0x80ull<<shift)) continue;
                u64 byte=(value>>shift)&0x9f;
                if((byte&3)==2) byte&=~2ull;
                v=(v&~(255ull<<shift))|(byte<<shift);
            }
            set_raw(address,v); return true;
        }
        if(address>=0x3b0 && address<0x3c0) {
            u32 n=address-0x3b0;
            if(raw(n<8?0x3a0:0x3a2)&(0x80ull<<((n%8)*8))) return true;
            if(n<15) { u32 next=n+1; u64 c=(raw(next<8?0x3a0:0x3a2)>>((next%8)*8))&255; if((c&0x98)==0x88) return true; }
        }
        set_raw(address,masked); return true;
    }
}

void CsrFile::set_fflags(u64 value) noexcept {
    fcsr_ = (fcsr_ & ~static_cast<u64>(0x1fu)) | (value & 0x1fu);
}

void CsrFile::set_frm(u64 value) noexcept {
    fcsr_ = (fcsr_ & ~static_cast<u64>(0xe0u)) | ((value & 0x7u) << 5);
}

void CsrFile::set_fcsr(u64 value) noexcept {
    fcsr_ = value & 0xffu;
}

std::string format_csr_dump(const CsrFile& csr, u64 instret) {
    std::string out = "\ncsrs:\n";
    char line[96];
    for (const CsrInfo& info : kCsrs) {
        u64 value = 0;
        if (!csr.read(info.address, Privilege::Machine, instret, value)) {
            continue;
        }
        std::snprintf(line, sizeof(line), "  %-10s 0x%03x  0x%016llx\n", info.name, info.address,
                      static_cast<unsigned long long>(value));
        out += line;
    }
    return out;
}

} // namespace rvvm::cpu
