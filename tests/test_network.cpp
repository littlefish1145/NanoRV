// SPDX-License-Identifier: MIT
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include "platform/network.hpp"
#include "platform/platform.hpp"
#include "test_framework.hpp"
#include <array>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <functional>
using namespace rvvm;
namespace {
struct Winsock {
  Winsock(){WSADATA data{};CHECK_EQ(WSAStartup(MAKEWORD(2,2),&data),0);}
  ~Winsock(){WSACleanup();}
};
struct Socket {
  SOCKET value=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
  ~Socket(){if(value!=INVALID_SOCKET)closesocket(value);}
};
u16 get16(const std::vector<u8>& b,std::size_t n){return static_cast<u16>((b[n]<<8)|b[n+1]);}
u32 get32(const std::vector<u8>& b,std::size_t n){return (static_cast<u32>(get16(b,n))<<16)|get16(b,n+2);}
void put16(std::vector<u8>& b,std::size_t n,u16 v){b[n]=static_cast<u8>(v>>8);b[n+1]=static_cast<u8>(v);}
void put32(std::vector<u8>& b,std::size_t n,u32 v){put16(b,n,static_cast<u16>(v>>16));put16(b,n+2,static_cast<u16>(v));}
u32 sum(std::span<const u8> bytes) {
  u32 result=0;for(std::size_t n=0;n<bytes.size();n+=2)result+=(static_cast<u32>(bytes[n])<<8)|(n+1<bytes.size()?bytes[n+1]:0);
  return result;
}
u16 checksum(std::span<const u8> bytes,u32 seed=0) {
  auto result=sum(bytes)+seed;while(result>>16)result=(result&65535)+(result>>16);
  return static_cast<u16>(~result);
}
std::size_t tcp_offset(const std::vector<u8>& frame) {
  if(frame.size()<54 || get16(frame,12)!=0x800 || frame[23]!=6)return 0;
  const auto offset=14+static_cast<std::size_t>(frame[14]&15)*4;
  return offset+20<=frame.size()?offset:0;
}
void arp_reply(platform::NatNetwork& net,const std::vector<u8>& request) {
  const std::array<u8,6> mac{0x52,0x54,0,0x12,0x34,0x56};
  std::vector<u8> reply(42);
  std::copy_n(request.begin()+6,6,reply.begin());std::copy(mac.begin(),mac.end(),reply.begin()+6);
  put16(reply,12,0x806);std::copy_n(request.begin()+14,6,reply.begin()+14);put16(reply,20,2);
  std::copy(mac.begin(),mac.end(),reply.begin()+22);std::copy_n(request.begin()+38,4,reply.begin()+28);
  std::copy_n(request.begin()+22,6,reply.begin()+32);std::copy_n(request.begin()+28,4,reply.begin()+38);
  net.send(reply);
}
std::vector<u8> wait_frame(platform::NatNetwork& net,const std::function<bool(const std::vector<u8>&)>& accept,
                         std::chrono::milliseconds timeout=std::chrono::seconds(2)) {
  const auto until=std::chrono::steady_clock::now()+timeout;
  std::string observed;
  while(std::chrono::steady_clock::now()<until) {
    std::vector<u8> frame;
    if(net.receive(frame)) {
      if(observed.size()<600) {
        observed+=" type="+std::to_string(frame.size()>=14?get16(frame,12):0);
        const auto t=tcp_offset(frame);
        if(t)observed+=" tcp_flags="+std::to_string(frame[t+13])+" dst="+std::to_string(get16(frame,t+2));
        else if(frame.size()>34 && get16(frame,12)==0x800)observed+=" ip_proto="+std::to_string(frame[23])+" type/code="+std::to_string(frame[34])+"/"+std::to_string(frame[35]);
      }
      if(frame.size()>=42 && get16(frame,12)==0x806 && get16(frame,20)==1)arp_reply(net,frame);
      else if(accept(frame))return frame;
    } else platform::sleep_ms(1);
  }
  platform::write_out("NAT test timeout; observed:"+observed+"\n");
  return {};
}
std::vector<u8> tcp_reply(const std::vector<u8>& incoming,u32 seq,u32 ack,u8 flags,std::span<const u8> payload={}) {
  const auto old_tcp=tcp_offset(incoming);CHECK(old_tcp);
  const u16 length=static_cast<u16>(20+payload.size());
  std::vector<u8> reply(14+20+length);
  std::copy_n(incoming.begin()+6,6,reply.begin());std::copy_n(incoming.begin(),6,reply.begin()+6);
  put16(reply,12,0x800);reply[14]=0x45;put16(reply,16,static_cast<u16>(20+length));
  reply[22]=64;reply[23]=6;
  std::copy_n(incoming.begin()+30,4,reply.begin()+26);std::copy_n(incoming.begin()+26,4,reply.begin()+30);
  put16(reply,24,checksum({reply.data()+14,20}));
  constexpr std::size_t tcp=34;
  put16(reply,tcp,get16(incoming,old_tcp+2));put16(reply,tcp+2,get16(incoming,old_tcp));
  put32(reply,tcp+4,seq);put32(reply,tcp+8,ack);reply[tcp+12]=0x50;reply[tcp+13]=flags;
  put16(reply,tcp+14,65535);std::copy(payload.begin(),payload.end(),reply.begin()+tcp+20);
  put16(reply,tcp+16,checksum({reply.data()+tcp,length},sum({reply.data()+26,8})+6+length));
  return reply;
}
}
TEST(network, loopback_tcp_forward_accept_read_write_and_reopen) {
  Winsock winsock;platform::NatNetwork net;
  for(unsigned round=0;round<2;++round) {
    u16 port=0;
    {
      Socket reservation;CHECK_NE(reservation.value,INVALID_SOCKET);
      sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
      CHECK_EQ(bind(reservation.value,reinterpret_cast<sockaddr*>(&address),sizeof(address)),0);
      int size=sizeof(address);CHECK_EQ(getsockname(reservation.value,reinterpret_cast<sockaddr*>(&address),&size),0);
      port=ntohs(address.sin_port);
    }
    const std::array<platform::PortForward,1> forwards{{{port,12345}}};std::string error;
    CHECK(net.open(error,forwards));
    Socket client;CHECK_NE(client.value,INVALID_SOCKET);
    DWORD timeout=2000;CHECK_EQ(setsockopt(client.value,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout)),0);
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);address.sin_port=htons(port);
    CHECK_EQ(connect(client.value,reinterpret_cast<sockaddr*>(&address),sizeof(address)),0);
    const auto syn=wait_frame(net,[](const auto& f){const auto t=tcp_offset(f);return t && get16(f,t+2)==12345 && (f[t+13]&2);});
    CHECK(!syn.empty());const auto server_sequence=get32(syn,tcp_offset(syn)+4)+1;
    net.send(tcp_reply(syn,0x12345678,server_sequence,0x12));
    CHECK_EQ(send(client.value,"ping",4,0),4);
    const auto data=wait_frame(net,[](const auto& f){const auto t=tcp_offset(f);if(!t)return false;
      const auto start=t+static_cast<std::size_t>(f[t+12]>>4)*4;
      return start+4<=f.size() && std::memcmp(f.data()+start,"ping",4)==0;});
    CHECK(!data.empty());
    const std::array<u8,4> pong{'p','o','n','g'};
    net.send(tcp_reply(data,0x12345679,server_sequence+4,0x18,pong));
    std::array<char,4> received{};std::size_t done=0;
    while(done<received.size()) {const auto n=recv(client.value,received.data()+done,static_cast<int>(received.size()-done),0);CHECK(n>0);done+=static_cast<std::size_t>(n);}
    CHECK_EQ(std::string(received.data(),received.size()),std::string("pong"));
  }
}
TEST(network, refused_loopback_connection_reports_guest_tcp_reset) {
  Winsock winsock;Socket closed_port;CHECK_NE(closed_port.value,INVALID_SOCKET);
  sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
  CHECK_EQ(bind(closed_port.value,reinterpret_cast<sockaddr*>(&address),sizeof(address)),0);
  int size=sizeof(address);CHECK_EQ(getsockname(closed_port.value,reinterpret_cast<sockaddr*>(&address),&size),0);
  platform::NatNetwork net;std::string error;CHECK(net.open(error));
  // The socket reserves its port but never listens, so a real connect to the
  // host alias deterministically exercises the asynchronous error path.
  const std::array<u8,6> guest{0x52,0x54,0,0x12,0x34,0x56};
  const std::array<u8,4> host_ip{10,0,2,2},guest_ip{10,0,2,15};
  std::vector<u8> arp(42);std::fill_n(arp.begin(),6,u8{255});std::copy(guest.begin(),guest.end(),arp.begin()+6);
  put16(arp,12,0x806);put16(arp,14,1);put16(arp,16,0x800);arp[18]=6;arp[19]=4;put16(arp,20,1);
  std::copy(guest.begin(),guest.end(),arp.begin()+22);std::copy(guest_ip.begin(),guest_ip.end(),arp.begin()+28);
  std::copy(host_ip.begin(),host_ip.end(),arp.begin()+38);net.send(arp);
  const auto gateway=wait_frame(net,[](const auto& f){return f.size()>=42 && get16(f,12)==0x806 && get16(f,20)==2;});
  CHECK(!gateway.empty());
  std::vector<u8> incoming(54);
  std::copy(guest.begin(),guest.end(),incoming.begin());std::copy_n(gateway.begin()+6,6,incoming.begin()+6);
  put16(incoming,12,0x800);incoming[14]=0x45;incoming[23]=6;
  std::copy(host_ip.begin(),host_ip.end(),incoming.begin()+26);std::copy(guest_ip.begin(),guest_ip.end(),incoming.begin()+30);
  put16(incoming,34,ntohs(address.sin_port));put16(incoming,36,12345);
  net.send(tcp_reply(incoming,0x12345678,0,2));
  // Windows retries a localhost SYN for about two seconds before reporting
  // WSAECONNREFUSED. This is a correctness deadline, not a latency assertion.
  const auto reset=wait_frame(net,[](const auto& f){const auto t=tcp_offset(f);return t && get16(f,t+2)==12345 && (f[t+13]&4);},std::chrono::seconds(4));
  CHECK(!reset.empty());
}
