// SPDX-License-Identifier: MIT
#include "memory/address_space.hpp"

#include "core/log.hpp"

#include <algorithm>
#include <cstring>

namespace nanorv::memory {

bool AddressSpace::add(u64 base, u64 size, MemoryRegion* region, std::string name,
                       Permissions permissions) {
    if (region == nullptr || size == 0) {
        NanoRV_ERROR("address space: refusing to add region '%s' (size=%llu, region=%p)", name.c_str(),
                   static_cast<unsigned long long>(size), static_cast<const void*>(region));
        return false;
    }
    if (base + size < base) { // wraps the address space
        NanoRV_ERROR("address space: region '%s' wraps the address space", name.c_str());
        return false;
    }
    for (const auto& existing : regions_) {
        if (base < existing.base + existing.size && existing.base < base + size) {
            NanoRV_ERROR("address space: region '%s' [%016llx, %016llx) overlaps '%s' [%016llx, %016llx)",
                       name.c_str(), static_cast<unsigned long long>(base),
                       static_cast<unsigned long long>(base + size), existing.name.c_str(),
                       static_cast<unsigned long long>(existing.base),
                       static_cast<unsigned long long>(existing.base + existing.size));
            return false;
        }
    }

    RegionEntry entry{};
    entry.base = base;
    entry.size = size;
    entry.name = std::move(name);
    entry.region = region;
    entry.permissions = permissions;
    regions_.push_back(std::move(entry));
    if (auto* bytes = region->direct_data()) {
        ram_base_ = base; ram_size_ = size; ram_data_ = bytes;
        ram_permissions_ = permissions;
        page_count_=(size+4095)/4096;
        code_pages_=std::make_unique<std::atomic<u64>[]>(static_cast<std::size_t>(page_count_));
        code_summary_=std::make_unique<std::atomic<u64>[]>(static_cast<std::size_t>((page_count_+63)/64));
    }
    std::sort(regions_.begin(), regions_.end(),
              [](const RegionEntry& a, const RegionEntry& b) { return a.base < b.base; });
    mmio_pages_.fill({});
    for(const auto& mapped:regions_)if(!mapped.region->direct_data()) {
        const u64 first=mapped.base>>12,last=(mapped.base+mapped.size-1)>>12;
        if(last-first>=4096)continue;
        for(u64 page=first;page<=last;++page)mmio_pages_[(page^(page>>12))&4095]={page,&mapped};
    }
    NanoRV_DEBUG("address space: mapped '%s' at [%016llx, %016llx)", entry.name.c_str(),
               static_cast<unsigned long long>(entry.base),
               static_cast<unsigned long long>(entry.base + entry.size));
    return true;
}

const RegionEntry* AddressSpace::entry_at(std::size_t index) const noexcept {
    return index < regions_.size() ? &regions_[index] : nullptr;
}

const RegionEntry* AddressSpace::find_region_named(std::string_view name) const noexcept {
    for (const auto& entry : regions_) {
        if (entry.name == name) {
            return &entry;
        }
    }
    return nullptr;
}

const RegionEntry* AddressSpace::find_entry(u64 phys, u64& offset, u64 required_bytes) const noexcept {
    const auto page=phys>>12;
    const auto& cached=mmio_pages_[(page^(page>>12))&4095];
    if(cached.tag==page && cached.entry && phys>=cached.entry->base) {
        const auto relative=phys-cached.entry->base;
        if(relative<cached.entry->size && required_bytes<=cached.entry->size-relative) {offset=relative;return cached.entry;}
    }
    for (const auto& entry : regions_) {
        if (phys < entry.base) {
            break; // sorted by base: nothing further can contain phys
        }
        if (phys >= entry.base && phys < entry.base + entry.size) {
            const u64 region_offset = phys - entry.base;
            if (required_bytes != 0 && region_offset + required_bytes > entry.size) {
                return nullptr; // the access straddles the end of the region
            }
            offset = region_offset;
            return &entry;
        }
    }
    return nullptr;
}

MemoryRegion* AddressSpace::find(u64 phys, u64& offset, u64 required_bytes) const noexcept {
    const RegionEntry* const entry = find_entry(phys, offset, required_bytes);
    return entry != nullptr ? entry->region : nullptr;
}

bool AddressSpace::load(u64 phys, unsigned width_bits, u64& value, bool for_exec) const noexcept {
    if (width_bits != 8 && width_bits != 16 && width_bits != 32 && width_bits != 64) {
        NanoRV_ERROR("address space: unsupported load width %u at 0x%016llx", width_bits,
                   static_cast<unsigned long long>(phys));
        return false;
    }
    if (auto* bytes = direct(phys, width_bits / 8, for_exec ? Access::Fetch : Access::Read)) {
        switch(width_bits) {
        case 8:value=read_fast<8>(bytes);break;
        case 16:value=read_fast<16>(bytes);break;
        case 32:value=read_fast<32>(bytes);break;
        case 64:value=read_fast<64>(bytes);break;
        }
        return true;
    }
    u64 offset = 0;
    const RegionEntry* const entry = find_entry(phys, offset, width_bits / 8);
    if (entry == nullptr) {
        NanoRV_ERROR("address space: unmapped %u-bit load at 0x%016llx", width_bits,
                   static_cast<unsigned long long>(phys));
        return false;
    }
    if (!entry->permissions.read || (for_exec && !entry->permissions.execute)) {
        NanoRV_ERROR("address space: %u-bit load at 0x%016llx denied by '%s' permissions", width_bits,
                   static_cast<unsigned long long>(phys), entry->name.c_str());
        return false;
    }
    if(mmio_dispatch_)return mmio_dispatch_(mmio_state_,entry->region,offset,width_bits,value,false);
    return entry->region->load(offset, width_bits, value);
}

bool AddressSpace::store(u64 phys, unsigned width_bits, u64 value) const noexcept {
    if (width_bits != 8 && width_bits != 16 && width_bits != 32 && width_bits != 64) {
        NanoRV_ERROR("address space: unsupported store width %u at 0x%016llx", width_bits,
                   static_cast<unsigned long long>(phys));
        return false;
    }
    if (auto* bytes = direct(phys, width_bits / 8, Access::Write)) {
        switch(width_bits) {
        case 8:write_fast<8>(bytes,value);break;
        case 16:write_fast<16>(bytes,value);break;
        case 32:write_fast<32>(bytes,value);break;
        case 64:write_fast<64>(bytes,value);break;
        }
        return true;
    }
    u64 offset = 0;
    const RegionEntry* const entry = find_entry(phys, offset, width_bits / 8);
    if (entry == nullptr) {
        NanoRV_ERROR("address space: unmapped %u-bit store at 0x%016llx", width_bits,
                   static_cast<unsigned long long>(phys));
        return false;
    }
    if (!entry->permissions.write) {
        NanoRV_ERROR("address space: %u-bit store at 0x%016llx denied by '%s' permissions", width_bits,
                   static_cast<unsigned long long>(phys), entry->name.c_str());
        return false;
    }
    const bool stored=mmio_dispatch_?mmio_dispatch_(mmio_state_,entry->region,offset,width_bits,value,true):entry->region->store(offset, width_bits, value);
    if(stored)notify_write(phys,width_bits/8);
    return stored;
}

bool AddressSpace::read_bytes(u64 phys, std::span<std::uint8_t> out) const noexcept {
    if (auto* bytes = direct(phys, out.size(), Access::Read)) {
        if(!concurrent_)std::memcpy(out.data(), bytes, out.size());
        else for(std::size_t n=0;n<out.size();) {
            const auto count=std::min<std::size_t>(8-((phys+n)&7),out.size()-n);
            if(count==8) {
                const auto value=read_fast<64>(bytes+n);
                std::memcpy(out.data()+n,&value,sizeof(value));
            } else {
                const auto value=read_host(bytes+n,static_cast<unsigned>(count*8));
                std::memcpy(out.data()+n,&value,count);
            }
            n+=count;
        }
        return true;
    }
    for (std::size_t i = 0; i < out.size(); ++i) {
        u64 value = 0;
        if (!load(phys + i, 8, value)) {
            return false;
        }
        out[i] = static_cast<std::uint8_t>(value);
    }
    return true;
}

bool AddressSpace::write_bytes(u64 phys, std::span<const std::uint8_t> data) const noexcept {
    if (auto* bytes = direct(phys, data.size(), Access::Write)) {
        if(!concurrent_) {std::memcpy(bytes, data.data(), data.size());notify_write(phys,data.size());}
        else for(std::size_t n=0;n<data.size();) {
            const u64 address=phys+n;
            const auto count=std::min<std::size_t>(64-(address&63),data.size()-n);
            auto guard=atomic_guard(address);
            // DMA writes one cache-line segment under a single reservation
            // lock instead of reacquiring it for every eight-byte word.
            for(std::size_t part=0;part<count;) {
                const auto offset=static_cast<unsigned>((address+part)&7);
                const auto length=std::min<std::size_t>(8-offset,count-part);
                auto& word=*reinterpret_cast<u64*>(ram_data_+((address+part-ram_base_)&~7ull));
                std::atomic_ref<u64> atomic(word);
                if(length==8) {
                    u64 value;std::memcpy(&value,data.data()+n+part,sizeof(value));
                    atomic.store(value,std::memory_order_release);
                }
                else {
                    u64 value=0;std::memcpy(&value,data.data()+n+part,length);
                    const u64 mask=((1ull<<(length*8))-1)<<(offset*8);
                    atomic.store((atomic.load(std::memory_order_relaxed)&~mask)|(value<<(offset*8)),std::memory_order_release);
                }
                part+=length;
            }
            ++(*stripes_)[stripe_index(address)].version;
            notify_write(address,count);n+=count;
        }
        return true;
    }
    for (std::size_t i = 0; i < data.size(); ++i) {
        if (!store(phys + i, 8, data[i])) {
            return false;
        }
    }
    return true;
}

bool AddressSpace::fill_zero(u64 phys, u64 length) const noexcept {
    if (auto* bytes = direct(phys, length, Access::Write)) {
        if(!concurrent_) {std::memset(bytes, 0, static_cast<std::size_t>(length));notify_write(phys,length);}
        else {
            const std::array<u8,4096> zeros{};
            for(u64 n=0;n<length;) {const auto count=std::min<u64>(zeros.size(),length-n);write_bytes(phys+n,{zeros.data(),static_cast<std::size_t>(count)});n+=count;}
        }
        return true;
    }
    constexpr u64 kChunk = 512;
    u64 done = 0;
    while (done < length) {
        const u64 remaining = length - done;
        const u64 chunk = remaining < kChunk ? remaining : kChunk;
        for (u64 i = 0; i < chunk; ++i) {
            if (!store(phys + done + i, 8, 0)) {
                return false;
            }
        }
        done += chunk;
    }
    return true;
}

u8* AddressSpace::direct(u64 phys, u64 count, Access access) const noexcept {
    const u64 offset = phys - ram_base_;
    if (!ram_data_ || offset >= ram_size_ || count > ram_size_ - offset) return nullptr;
    if (access == Access::Write ? !ram_permissions_.write :
        (!ram_permissions_.read || (access == Access::Fetch && !ram_permissions_.execute))) return nullptr;
    return ram_data_ + offset;
}

u64 AddressSpace::read_host(const u8* ptr,unsigned width) const noexcept {
    if(!concurrent_) {u64 value=0;std::memcpy(&value,ptr,width/8);return value;}
    u64 value=0;
    for(unsigned n=0;n<width/8;) {
        const auto address=reinterpret_cast<std::uintptr_t>(ptr+n);
        const auto offset=static_cast<unsigned>(address&7);
        const auto count=std::min(8-offset,width/8-n);
        auto& word=*reinterpret_cast<u64*>(address&~std::uintptr_t{7});
        const u64 loaded=std::atomic_ref<u64>(word).load(std::memory_order_acquire);
        const u64 mask=count==8?~0ull:(1ull<<(count*8))-1;
        value|=((loaded>>(offset*8))&mask)<<(n*8);n+=count;
    }
    return value;
}
void AddressSpace::write_host(u8* ptr,unsigned width,u64 value) const noexcept {
    const u64 physical=ram_base_+static_cast<u64>(ptr-ram_data_);
    if(!concurrent_) {std::memcpy(ptr,&value,width/8);notify_write(physical,width/8);return;}
    for(unsigned n=0;n<width/8;) {
        const u64 address=physical+n;
        const auto offset=static_cast<unsigned>((address-ram_base_)&7),count=std::min(8-offset,width/8-n);
        auto guard=atomic_guard(address);
        auto& word=*reinterpret_cast<u64*>(ram_data_+((address-ram_base_)&~7ull));
        std::atomic_ref<u64> atomic(word);
        const u64 mask=count==8?~0ull:((1ull<<(count*8))-1)<<(offset*8);
        if(count==8)atomic.store(value>>(n*8),std::memory_order_release);
        else atomic.store((atomic.load(std::memory_order_relaxed)&~mask)|((value>>(n*8)<<(offset*8))&mask),std::memory_order_release);
        ++(*stripes_)[stripe_index(address)].version;
        n+=count;
    }
    notify_write(physical,width/8);
}
bool AddressSpace::compare_exchange(u64 address,unsigned width,u64& expected,u64 desired) const noexcept {
    auto guard=atomic_guard(address);u64 value=0;
    if(!load(address,width,value))return false;
    if(value!=expected) {expected=value;return true;}
    return store_locked(address,width,desired,guard);
}
bool AddressSpace::store_locked(u64 address,unsigned width,u64 value,const AtomicGuard& guard) const noexcept {
    if((width!=8 && width!=16 && width!=32 && width!=64) ||
       (address&(width/8-1)) || !guard.owns_lock() ||
       guard.mutex()!=&(*stripes_)[stripe_index(address)].mutex)return false;
    auto* ptr=direct(address,width/8,Access::Write);
    if(!ptr)return false;
    if(!concurrent_) {std::memcpy(ptr,&value,width/8);notify_write(address,width/8);return true;}
    switch(width) {
    case 8:write_locked<8>(ptr,value,address);break;
    case 16:write_locked<16>(ptr,value,address);break;
    case 32:write_locked<32>(ptr,value,address);break;
    case 64:write_locked<64>(ptr,value,address);break;
    }
    return true;
}
u64 AddressSpace::code_generation(u64 address) const noexcept {
    const auto offset=address-ram_base_;
    if(offset>=ram_size_ || !code_pages_)return 0;
    const u64 page_index=offset>>12;
    auto& page=code_pages_[page_index];
    auto generation=page.load(std::memory_order_acquire);
    if(!(generation&(1ull<<63))) {
        // Publish the summary before marking the fine table. A concurrent
        // writer must not skip invalidation of an already marked code page.
        code_summary_[page_index>>6].fetch_or(1ull<<(page_index&63),std::memory_order_seq_cst);
        generation=page.fetch_or(1ull<<63,std::memory_order_acq_rel)|(1ull<<63);
    }
    return generation;
}
void AddressSpace::notify_write(u64 address,u64 bytes) const noexcept {
    const auto offset=address-ram_base_;
    if(code_pages_ && offset<ram_size_ && bytes && bytes<=ram_size_-offset) {
        const auto end=(offset+bytes-1)>>12;
        for(u64 page=offset>>12;page<=end;++page) {
            // The summary table is 64x smaller than the fine table, so this
            // test stays in cache for data-heavy workloads.
            if(!(code_summary_[page>>6].load(std::memory_order_seq_cst)&(1ull<<(page&63))))continue;
            if(code_pages_[page].load(std::memory_order_relaxed)&(1ull<<63))code_pages_[page].fetch_add(1,std::memory_order_release);
        }
    }
    if(observer_ && !concurrent_)observer_(observer_state_,address,bytes);
}
void AddressSpace::invalidate_code() const noexcept {
    for(u64 n=0;n<page_count_;++n)if(code_pages_[n].load(std::memory_order_relaxed)&(1ull<<63))code_pages_[n].fetch_add(1,std::memory_order_release);
}
void AddressSpace::notify_dma_write(u64 address,u64 bytes) const noexcept {
    const auto offset=address-ram_base_;
    if(!bytes || offset>=ram_size_ || bytes>ram_size_-offset)return;
    if(concurrent_) {
        const auto lines=1+((bytes-1+(address&63))>>6);
        for(u64 n=0;n<std::min<u64>(4096,lines);++n) {
            // A hashed interval longer than the table need not cover all slots
            // in its first 4096 lines. Conservatively invalidate every stripe.
            const auto index=lines>=4096?static_cast<std::size_t>(n):stripe_index((address&~63ull)+n*64);
            auto& stripe=(*stripes_)[index];std::lock_guard lock(stripe.mutex);++stripe.version;
        }
    }
    notify_write(address,bytes);
}
} // namespace nanorv::memory
