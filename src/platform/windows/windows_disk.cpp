// SPDX-License-Identifier: MIT
#include "platform/disk.hpp"
#include "platform/platform.hpp"
#include <windows.h>
#include <algorithm>
namespace rvvm::platform {
struct DiskFile::Impl {
  HANDLE file=INVALID_HANDLE_VALUE;
  std::uint64_t bytes=0;
  ~Impl() {if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);}
  bool seek(std::uint64_t position) {
    LARGE_INTEGER offset{};offset.QuadPart=static_cast<LONGLONG>(position);
    return SetFilePointerEx(file,offset,nullptr,FILE_BEGIN)!=0;
  }
};
DiskFile::DiskFile():impl_(std::make_unique<Impl>()) {}
DiskFile::~DiskFile()=default;
bool DiskFile::open(const std::string& path,std::string& error) {
  impl_=std::make_unique<Impl>();
  impl_->file=CreateFileA(path.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ,nullptr,
                         OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
  if(impl_->file==INVALID_HANDLE_VALUE) {error="cannot open writable raw disk '"+path+"': "+last_error();return false;}
  LARGE_INTEGER bytes{};
  if(!GetFileSizeEx(impl_->file,&bytes) || bytes.QuadPart<=0) {error="cannot determine disk size: "+last_error();return false;}
  impl_->bytes=static_cast<std::uint64_t>(bytes.QuadPart);return true;
}
std::uint64_t DiskFile::size() const noexcept {return impl_->bytes;}
bool DiskFile::read(std::uint64_t position,std::span<std::uint8_t> data) {
  if(position>impl_->bytes || data.size()>impl_->bytes-position || !impl_->seek(position))return false;
  while(!data.empty()) {
    const auto amount=static_cast<DWORD>(std::min<std::size_t>(data.size(),0x40000000));
    DWORD done=0;
    if(!ReadFile(impl_->file,data.data(),amount,&done,nullptr) || done!=amount)return false;
    data=data.subspan(done);
  }
  return true;
}
bool DiskFile::write(std::uint64_t position,std::span<const std::uint8_t> data) {
  if(position>impl_->bytes || data.size()>impl_->bytes-position || !impl_->seek(position))return false;
  while(!data.empty()) {
    const auto amount=static_cast<DWORD>(std::min<std::size_t>(data.size(),0x40000000));
    DWORD done=0;
    if(!WriteFile(impl_->file,data.data(),amount,&done,nullptr) || done!=amount)return false;
    data=data.subspan(done);
  }
  return true;
}
bool DiskFile::flush() {return FlushFileBuffers(impl_->file)!=0;}
}
