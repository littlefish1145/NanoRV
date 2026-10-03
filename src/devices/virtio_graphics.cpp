// SPDX-License-Identifier: MIT
#include "devices/virtio_graphics.hpp"
#include <cstring>
#include <algorithm>
#include <bit>
#include "core/log.hpp"
namespace rvvm::devices {
namespace {
u32 get32(std::span<const u8> bytes,std::size_t at) {u32 v=0;if(at+4<=bytes.size())std::memcpy(&v,bytes.data()+at,4);return v;}
u64 get64(std::span<const u8> bytes,std::size_t at) {u64 v=0;if(at+8<=bytes.size())std::memcpy(&v,bytes.data()+at,8);return v;}
void put32(std::vector<u8>& bytes,std::size_t at,u32 v) {std::memcpy(bytes.data()+at,&v,4);}
u32 bgra(u32 value,u32 format) {
  if(format==3 || format==4)return value>>8;
  if(format==67 || format==134)return (value&0xff00ff00)|((value&255)<<16)|((value>>16)&255);
  if(format==68 || format==121)return std::byteswap(value);
  return value;
}
}
VirtioGpu::VirtioGpu(memory::AddressSpace& bus,platform::Display& display):VirtioMmio(bus,16,2,0),display_(display) {}
bool VirtioGpu::open_renderer(std::string& error,platform::HostGpu gpu) {
  if(!renderer_.open(error,gpu))return false;
  if(!renderer_.cap_info(1)[1] || !renderer_.cap_info(2)[1]) {error="host VirGL driver did not provide OpenGL capabilities";return false;}
  device_features(1);RVVM_INFO("VirGL host renderer: %s",renderer_.description().c_str());return true;
}
bool VirtioGpu::config_read(u64 o,unsigned width,u64& v) {
  const std::array<u32,4> config{0,0,1,renderer_.ready()?2u:0u};
  if((width!=8 && width!=16 && width!=32) || o+width/8>sizeof(config))return false;
  v=0;std::memcpy(&v,reinterpret_cast<const u8*>(config.data())+o,width/8);return true;
}
bool VirtioGpu::config_write(u64 o,unsigned width,u64) {return o==4 && width==32;}
void VirtioGpu::reset_device() {renderer_.reset();contexts_.clear();resources_.clear();scanout_=cursor_=0;scan_={};allocated_=0;}
bool VirtioGpu::rect_valid(const Resource& r,Rect a) const {
  return a.x<=r.width && a.y<=r.height && a.w<=r.width-a.x && a.h<=r.height-a.y;
}
bool VirtioGpu::backing_read(const Resource& r,u64 offset,std::span<u8> bytes) {
  if(offset>r.bytes || bytes.size()>r.bytes-offset)return false;
  for(const auto& b:r.backing) {
    if(offset>=b.length) {offset-=b.length;continue;}
    const auto n=std::min<std::size_t>(bytes.size(),b.length-static_cast<std::size_t>(offset));
    if(!bus_.read_bytes(b.address+offset,bytes.first(n)))return false;
    bytes=bytes.subspan(n);offset=0;if(bytes.empty())return true;
  }
  return bytes.empty();
}
bool VirtioGpu::sync_backing(Resource& r,bool to_guest,const std::vector<std::vector<u8>>* snapshot) {
  if(r.host_backing.empty()) {
    if(to_guest)for(auto entry:r.backing)bus_.notify_dma_write(entry.address,entry.length);
    return true;
  }
  for(std::size_t n=0;n<r.backing.size();++n) {
    if(to_guest && snapshot) {
      // A readback can cover only part of a shared staging allocation. Copy
      // changed bytes only; never overwrite unrelated CPU edits with a stale
      // shadow. The pre-transfer snapshot was refreshed from guest RAM.
      const auto& before=(*snapshot)[n];const auto& after=r.host_backing[n];
      for(std::size_t start=0;start<after.size();) {
        if(before[start]==after[start]) {++start;continue;}
        auto end=start+1;while(end<after.size() && before[end]!=after[end])++end;
        if(!bus_.write_bytes(r.backing[n].address+start,{after.data()+start,end-start}))return false;
        start=end;
      }
      bus_.notify_dma_write(r.backing[n].address,r.backing[n].length);
      continue;
    }
    const bool ok=to_guest?bus_.write_bytes(r.backing[n].address,r.host_backing[n]):
                           bus_.read_bytes(r.backing[n].address,r.host_backing[n]);
    if(!ok)return false;
  }
  return true;
}
bool VirtioGpu::submit_3d(u32 context,std::span<const u8> commands) {
  // VirGL's TRANSFER3D (43) and COPY_TRANSFER3D (45) also access attached
  // iovecs inside the command stream. Preserve order by splitting only at
  // transfers; ordinary draws/shaders still submit in batches.
  for(std::size_t offset=0;offset<commands.size();) {
    if(commands.size()-offset<4)return false;
    const auto header=get32(commands,offset),op=header&255,length=header>>16;
    const auto bytes=(static_cast<std::size_t>(length)+1)*4;
    if(bytes>commands.size()-offset || (op==43 && length<13) || (op==45 && length!=14))return false;
    offset+=bytes;
  }
  std::size_t begin=0;
  for(std::size_t offset=0;offset<commands.size();) {
    const auto header=get32(commands,offset),op=header&255;
    const auto bytes=(static_cast<std::size_t>(header>>16)+1)*4;
    if(op==43 || op==45) {
      if(offset>begin && renderer_.submit(context,commands.subspan(begin,offset-begin)))return false;
      const auto item=commands.subspan(offset,bytes);
      const bool download=op==43 && get32(item,52)==2;
      const auto handle=get32(item,op==45?48:4);
      auto resource=resources_.find(handle);
      std::vector<std::vector<u8>> before;
      if(resource!=resources_.end()) {
        if(!sync_backing(resource->second,false))return false;
        if(download)before=resource->second.host_backing;
      }
      if(renderer_.submit(context,item))return false;
      if(download && resource!=resources_.end()) {
        renderer_.finish();
        if(!sync_backing(resource->second,true,&before))return false;
      }
      begin=offset+bytes;
    }
    offset+=bytes;
  }
  return begin==commands.size() || !renderer_.submit(context,commands.subspan(begin));
}
void VirtioGpu::publish() {
  auto it=resources_.find(scanout_);if(it==resources_.end() || !scan_.w || !scan_.h)return;
  auto& r=it->second;
  if(r.rendered && !renderer_.pixels(scanout_,r.width,r.height,r.pixels))return;
  const bool direct_format=r.format==1 || r.format==2;
  if(direct_format && !resources_.contains(cursor_) && !scan_.x && !scan_.y && scan_.w==r.width && scan_.h==r.height) {
    display_.present(scan_.w,scan_.h,r.pixels);++frames_;return;
  }
  auto& pixels=scan_pixels_;pixels.resize(static_cast<std::size_t>(scan_.w)*scan_.h);
  for(u32 y=0;y<scan_.h;++y) {
    const auto* row=r.pixels.data()+static_cast<std::size_t>(y+scan_.y)*r.width+scan_.x;
    auto* destination=pixels.data()+static_cast<std::size_t>(y)*scan_.w;
    if(direct_format)std::copy_n(row,scan_.w,destination);
    else for(u32 x=0;x<scan_.w;++x)destination[x]=bgra(row[x],r.format);
  }
  if(auto c=resources_.find(cursor_);c!=resources_.end()) {
    if(c->second.rendered && !renderer_.pixels(cursor_,c->second.width,c->second.height,c->second.pixels))return;
    for(u32 y=0;y<c->second.height;++y)for(u32 x=0;x<c->second.width;++x) {
      const auto dx=static_cast<i64>(cursor_x_)+x-hot_x_,dy=static_cast<i64>(cursor_y_)+y-hot_y_;
      if(dx<0 || dy<0 || dx>=scan_.w || dy>=scan_.h)continue;
      const auto source=bgra(c->second.pixels[static_cast<std::size_t>(y)*c->second.width+x],c->second.format);
      const u32 alpha=source>>24;
      auto& target=pixels[static_cast<std::size_t>(dy)*scan_.w+static_cast<std::size_t>(dx)];
      u32 combined=0;
      for(unsigned n=0;n<3;++n)combined|=((((source>>(n*8))&255)*alpha+((target>>(n*8))&255)*(255-alpha))/255)<<(n*8);
      target=combined;
    }
  }
  display_.present(scan_.w,scan_.h,pixels);++frames_;
}
std::vector<u8> VirtioGpu::command(std::span<const u8> b) {
  std::vector<u8> response(24,0);
  u32 status=0x1100;
  if(b.size()<24) {put32(response,0,0x1205);return response;}
  // Commands execute synchronously: fences are complete when the response is published.
  put32(response,4,get32(b,4)&1);std::memcpy(response.data()+8,b.data()+8,8);
  put32(response,16,get32(b,16));
  const auto type=get32(b,0);
  const auto ctx=get32(b,16);
  const u32 id=get32(b,type==0x103?44:type==0x104?40:type==0x105?48:(type==0x205 || type==0x206)?56:24);
  auto it=resources_.find(id);
  const Rect rect{get32(b,24),get32(b,28),get32(b,32),get32(b,36)};
  switch(type) {
  case 0x100:
    response.resize(24+16*24,0);put32(response,32,800);put32(response,36,600);put32(response,40,1);status=0x1101;break;
  case 0x101: {
    const auto format=get32(b,28),width=get32(b,32),height=get32(b,36);
    const bool format_ok=format==1 || format==2 || format==3 || format==4 || format==67 || format==68 || format==121 || format==134;
    const u64 size=static_cast<u64>(width)*height*4;
    if(b.size()<40 || !id || it!=resources_.end() || !width || !height || width>4096 || height>4096 || !format_ok)status=0x1205;
    else if(size>64*1024*1024 || allocated_+size>256*1024*1024 || resources_.size()>=256)status=0x1201;
    else {
      if(renderer_.ready() && renderer_.create({id,2,format,(1u<<1)|(1u<<3)|(1u<<18),width,height,1,1,0,0,1})) {status=0x1201;break;}
      auto& resource=resources_.emplace(id,Resource{format,width,height,std::vector<u32>(static_cast<std::size_t>(size/4)),{},0}).first->second;
      resource.rendered=renderer_.ready();resource.allocation=size;allocated_+=size;
    }
    break;
  }
  case 0x102:
    if(b.size()<32)status=0x1205;
    else if(it==resources_.end())status=0x1203;
    else {if(it->second.rendered)renderer_.unref(id);allocated_-=it->second.allocation;resources_.erase(it);if(scanout_==id)scanout_=0;if(cursor_==id)cursor_=0;}
    break;
  case 0x103:
    if(b.size()<48)status=0x1205;
    else if(get32(b,40))status=0x1202;
    else if(!id) {scanout_=0;scan_={};}
    else if(it==resources_.end())status=0x1203;
    else if(!rect.w || !rect.h || it->second.width>4096 || it->second.height>4096 || !rect_valid(it->second,rect))status=0x1205;
    else {scanout_=id;scan_=rect;publish();}
    break;
  case 0x104:
    if(b.size()<48)status=0x1205;
    else if(it==resources_.end())status=0x1203;
    else if(!rect_valid(it->second,rect))status=0x1205;
    else if(id==scanout_)publish();
    break;
  case 0x105:
    if(b.size()<56)status=0x1205;
    else if(it==resources_.end())status=0x1203;
    else if(!rect_valid(it->second,rect))status=0x1205;
    else {
      auto& r=it->second;const u64 offset=get64(b,40),stride=static_cast<u64>(r.width)*4;
      if(r.rendered) {
        if(!sync_backing(r,false)) {status=0x1205;break;}
        if(renderer_.transfer(id,0,{rect.x,rect.y,0,rect.w,rect.h,1},offset,0,0,0,false))status=0x1205;break;
      }
      // Validate every row before mutating the resource.
      if(rect.h && (offset>r.bytes || (rect.h-1)*stride>r.bytes-offset || static_cast<u64>(rect.w)*4>r.bytes-offset-(rect.h-1)*stride))status=0x1205;
      else for(u32 y=0;y<rect.h;++y) {
        auto* destination=reinterpret_cast<u8*>(r.pixels.data()+static_cast<std::size_t>(rect.y+y)*r.width+rect.x);
        if(!backing_read(r,offset+y*stride,{destination,static_cast<std::size_t>(rect.w)*4})) {status=0x1205;break;}
      }
    }
    break;
  case 0x106:
    if(b.size()<32)status=0x1205;
    else if(it==resources_.end())status=0x1203;
    else {
      const auto count=get32(b,28);
      if(count>16384 || b.size()!=32ull+static_cast<u64>(count)*16) {status=0x1205;break;}
      std::vector<Backing> backing;u64 bytes=0;
      for(u32 n=0;n<count;++n) {
        const auto address=get64(b,32+n*16);const auto length=get32(b,40+n*16);
        if(!length || !bus_.direct(address,length,memory::Access::Read) || (it->second.rendered && !bus_.direct(address,length,memory::Access::Write)) || bytes+length>256*1024*1024) {status=0x1205;break;}
        backing.push_back({address,length});bytes+=length;
      }
      if(status==0x1100) {
        if(it->second.rendered) {
          std::vector<platform::RenderBacking> iov;
          std::vector<std::vector<u8>> bounce;
          if(bus_.concurrent()) {
            for(auto entry:backing) {
              bounce.emplace_back(entry.length);
              bus_.read_bytes(entry.address,bounce.back());iov.push_back({bounce.back().data(),entry.length});
            }
          } else for(auto entry:backing)iov.push_back({bus_.direct(entry.address,entry.length,memory::Access::Write),entry.length});
          if(renderer_.attach(id,iov)) {renderer_.detach(id);status=0x1205;break;}
          it->second.host_backing=std::move(bounce);
        }
        it->second.backing=std::move(backing);it->second.bytes=bytes;
      }
    }
    break;
  case 0x107:
    if(b.size()<32)status=0x1205;
    else if(it==resources_.end())status=0x1203;
    else {if(it->second.rendered)renderer_.detach(id);it->second.backing.clear();it->second.host_backing.clear();it->second.bytes=0;}
    break;
  case 0x108: {
    const auto index=get32(b,24);
    if(!renderer_.ready() || b.size()<32 || index>=2)status=0x1205;
    else {const auto cap=renderer_.cap_info(index+1);response.resize(40);put32(response,24,index+1);put32(response,28,cap[0]);put32(response,32,cap[1]);status=0x1102;}
    break;
  }
  case 0x109: {
    if(!renderer_.ready() || b.size()<32 || id<1 || id>2) {status=0x1205;break;}
    const auto caps=renderer_.caps(id,get32(b,28));
    if(caps.empty())status=0x1205;
    else {response.insert(response.end(),caps.begin(),caps.end());status=0x1103;}
    break;
  }
  case 0x200: {
    if(!renderer_.ready() || b.size()<96 || !ctx || ctx>0x7fffffffu || contexts_.contains(ctx) || get32(b,24)>64 || get32(b,28))status=0x1205;
    else if(contexts_.size()>=256 || renderer_.context(ctx,std::string(reinterpret_cast<const char*>(b.data()+32),get32(b,24))))status=0x1200;
    else contexts_.insert(ctx);
    break;
  }
  case 0x201:
    if(!contexts_.contains(ctx))status=0x1204;
    else {renderer_.destroy_context(ctx);contexts_.erase(ctx);}
    break;
  case 0x202:case 0x203:
    if(b.size()<32 || !contexts_.contains(ctx))status=0x1204;
    else if(it==resources_.end())status=0x1203;
    else renderer_.context_resource(ctx,id,type==0x202);
    break;
  case 0x204: {
    if(!renderer_.ready() || b.size()<72 || !id || id>0x7fffffffu || it!=resources_.end()) {status=0x1205;break;}
    std::array<u32,11> args{};for(unsigned n=0;n<11;++n)args[n]=get32(b,24+n*4);
    const auto width=args[4],height=args[5],depth=args[6],layers=args[7];
    // Buffers use byte-sized width. Textures reserve an upper bound for mipmaps/MSAA.
    u64 size=width;
    if(args[1]!=0)size=static_cast<u64>(width)*height*4;
    if(!width || width>(args[1]?16384u:64u*1024*1024) || !height || height>16384 || !depth || depth>2048 || !layers || layers>2048 || args[8]>14 || args[9]>16 || args[1]>8) {status=0x1205;break;}
    if(size>64*1024*1024 || depth>64*1024*1024/size || layers>64*1024*1024/(size*depth)) {status=0x1201;break;}
    size*=depth*layers;if(args[8])size=size*2;if(args[9]>1)size*=args[9];
    if(size>64*1024*1024 || allocated_+size>256*1024*1024 || resources_.size()>=4096) {status=0x1201;break;}
    if(renderer_.create(args)) {status=0x1201;break;}
    Resource r{args[2],width,height,{}, {},0,true,depth,layers,args[8],size,{}};resources_.emplace(id,std::move(r));allocated_+=size;
    break;
  }
  case 0x205:case 0x206: {
    if(!renderer_.ready() || b.size()<72) {status=0x1205;break;}
    if(ctx && !contexts_.contains(ctx)) {status=0x1204;break;}
    if(it==resources_.end() || !it->second.rendered) {status=0x1203;break;}
    std::array<u32,6> box{};for(unsigned n=0;n<6;++n)box[n]=get32(b,24+n*4);
    const auto level=get32(b,60);auto& r=it->second;
    if(level>r.last_level || box[0]>(r.width>>level) || box[3]>(r.width>>level)-box[0] || box[1]>std::max(1u,r.height>>level) || box[4]>std::max(1u,r.height>>level)-box[1] || box[2]>=r.depth*r.array_size || box[5]>r.depth*r.array_size-box[2]) {status=0x1205;break;}
    if(!sync_backing(r,false)) {status=0x1205;break;}
    std::vector<std::vector<u8>> before;
    if(type==0x206)before=r.host_backing;
    if(renderer_.transfer(id,ctx,box,get64(b,48),level,get32(b,64),get32(b,68),type==0x206)) {status=0x1205;break;}
    if(type==0x206) {
      if(!sync_backing(r,true,&before))status=0x1205;
    }
    break;
  }
  case 0x207:
    if(!renderer_.ready() || b.size()<32 || get32(b,24)%4 || b.size()!=32ull+get32(b,24))status=0x1205;
    else if(!contexts_.contains(ctx))status=0x1204;
    else if(!submit_3d(ctx,b.subspan(32)))status=0x1200;
    break;
  default:status=0x1205;break;
  }
  if(renderer_.ready() && (get32(b,4)&1))renderer_.finish();
  if(status>=0x1200)RVVM_WARN("VirtIO GPU rejected command 0x%x (resource %u, context %u, size %zu): 0x%x",type,id,ctx,b.size(),status);
  put32(response,0,status);return response;
}
void VirtioGpu::notify(unsigned index) {
  u16 head;std::vector<Descriptor> chain;
  while(take(index,head,chain)) {
    std::vector<u8> request;u64 capacity=0;bool output=false;
    for(const auto& d:chain) {
      if(d.flags&2) {output=true;capacity+=d.length;continue;}
      if(output || request.size()+d.length>1024*1024) {fail();return;}
      const auto offset=request.size();request.resize(offset+d.length);
      if(!bus_.read_bytes(d.address,{request.data()+offset,d.length})) {fail();return;}
    }
    if(index==1) {
      if(request.size()>=56 && !get32(request,24) && (get32(request,0)==0x300 || get32(request,0)==0x301)) {
        const auto id=get32(request,40);auto it=resources_.find(id);
        if(get32(request,0)==0x300 && (!id || (it!=resources_.end() && it->second.width==64 && it->second.height==64))) {
          cursor_=id;hot_x_=get32(request,44);hot_y_=get32(request,48);
        }
        cursor_x_=get32(request,28);cursor_y_=get32(request,32);publish();
      }
      complete(index,head,0);continue;
    }
    if(capacity<24) {fail();return;}
    auto response=command(request);
    if(capacity<response.size()) {response.resize(24);put32(response,0,0x1205);}
    std::size_t offset=0;
    for(const auto& d:chain)if(d.flags&2) {
      const auto n=std::min<std::size_t>(d.length,response.size()-offset);
      if(n && !bus_.write_bytes(d.address,{response.data()+offset,n})) {fail();return;}
      offset+=n;
    }
    complete(index,head,static_cast<u32>(response.size()));
  }
}
VirtioInput::VirtioInput(memory::AddressSpace& bus):VirtioMmio(bus,18,2,0) {}
bool VirtioInput::config_write(u64 o,unsigned width,u64 v) {
  if(width!=8 || o>1)return false;
  (o?subsel_:select_)=static_cast<u8>(v);return true;
}
bool VirtioInput::config_read(u64 o,unsigned width,u64& v) {
  std::array<u8,136> bytes{};bytes[0]=select_;bytes[1]=subsel_;
  if(select_==1 || select_==2) {
    const std::string name=select_==1?"RVVM keyboard and mouse":"rvvm-input-1";
    bytes[2]=static_cast<u8>(name.size());std::memcpy(bytes.data()+8,name.data(),name.size());
  } else if(select_==3) {bytes[2]=8;bytes[8]=6;bytes[10]=0x56;bytes[11]=0x52;bytes[12]=1;bytes[14]=1;}
  else if(select_==0x11) {
    auto bit=[&](unsigned n) {bytes[8+n/8]|=static_cast<u8>(1u<<(n&7));bytes[2]=static_cast<u8>(n/8+1);};
    if(!subsel_) {bit(0);bit(1);bit(2);}
    else if(subsel_==1) {for(unsigned n=1;n<=127;++n)bit(n);bit(272);bit(273);bit(274);}
    else if(subsel_==2) {bit(0);bit(1);bit(8);}
  }
  if((width!=8 && width!=16 && width!=32 && width!=64) || o+width/8>bytes.size())return false;
  v=0;std::memcpy(&v,bytes.data()+o,width/8);return true;
}
void VirtioInput::push(std::span<const platform::InputEvent> events) {
  if(!ready())return;
  for(auto event:events)if(events_.size()<4096)events_.push_back(event);
  deliver();
}
void VirtioInput::notify(unsigned index) {
  if(!index) {deliver();return;}
  u16 head;std::vector<Descriptor> chain;
  while(take(1,head,chain))complete(1,head,0); // LED/status events do not affect input delivery
}
void VirtioInput::deliver() {
  u16 head;std::vector<Descriptor> chain;
  while(!events_.empty() && take(0,head,chain)) {
    std::array<u8,8> bytes{};const auto event=events_.front();
    std::memcpy(bytes.data(),&event.type,2);std::memcpy(bytes.data()+2,&event.code,2);std::memcpy(bytes.data()+4,&event.value,4);
    std::size_t offset=0;
    for(const auto& d:chain) {
      if(!(d.flags&2)) {fail();return;}
      const auto n=std::min<std::size_t>(d.length,bytes.size()-offset);
      if(n)bus_.write_bytes(d.address,{bytes.data()+offset,n});offset+=n;
    }
    if(offset!=8) {fail();return;}
    events_.pop_front();complete(0,head,8);
  }
}
}
