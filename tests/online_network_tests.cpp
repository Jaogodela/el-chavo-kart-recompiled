// The sockets under the online code, on loopback only: UDP send and receive, the address forms the service may hand
// back, and the STUN codec against a local responder. Nothing here leaves the machine.

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "online/net_addr.h"
#include "online/stun.h"
#include "online/udp_transport.h"
#include "online_test_support.h"

using online_test::Check;
namespace online = eck::online;
namespace net = eck::online::net;

namespace {

constexpr uint32_t kLoopback = 0x7F000001;

using Bytes = std::vector<uint8_t>;

bool Receive(online::UdpTransport& transport, Bytes& data, online::Endpoint& from, int milliseconds = 1500) {
  uint8_t buffer[2048];
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
  while (std::chrono::steady_clock::now() < deadline) {
    if (const size_t n = transport.Receive(buffer, sizeof buffer, from)) {
      data.assign(buffer, buffer + n);
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return false;
}

void TestUdpLoopback() {
  online::UdpTransport a, b;
  Check(!a.is_open() && !a.Send({kLoopback, 9}, nullptr, 0), "a closed socket sends nothing");
  Check(a.Open(0, true) && a.is_open() && a.bound_port() != 0, "port 0 binds a free loopback port");
  Check(b.Open(0, true) && b.bound_port() != a.bound_port(), "two sockets get two ports");

  uint8_t scratch[16];
  online::Endpoint from;
  const auto before = std::chrono::steady_clock::now();
  Check(a.Receive(scratch, sizeof scratch, from) == 0, "Receive never blocks and returns 0 when nothing is waiting");
  Check(std::chrono::steady_clock::now() - before < std::chrono::milliseconds(500), "it returned at once");

  const Bytes hello = {'E', 'C', 'K', 1, 2, 3};
  Check(b.Send({kLoopback, a.bound_port()}, hello.data(), hello.size()), "send to a loopback port");
  Bytes got;
  Check(Receive(a, got, from) && got == hello, "the datagram arrives intact");
  Check(from.address == kLoopback && from.port == b.bound_port(), "from names the sender's address and port");

  Bytes full(1472);
  for (size_t i = 0; i < full.size(); ++i) full[i] = uint8_t(i * 7);
  Check(a.Send({kLoopback, b.bound_port()}, full.data(), full.size()), "a full relay-sized datagram is sent");
  Check(Receive(b, got, from) && got == full && from.port == a.bound_port(), "and arrives whole");

  for (uint8_t i = 0; i < 20; ++i) b.Send({kLoopback, a.bound_port()}, &i, 1);
  int received = 0;
  uint8_t last = 0;
  while (Receive(a, got, from, 300)) {
    ++received;
    last = got[0];
  }
  Check(received == 20 && last == 19, "datagrams of a burst arrive in order");

  const uint16_t port = a.bound_port();
  a.Close();
  Check(!a.is_open() && !a.Send({kLoopback, b.bound_port()}, hello.data(), hello.size()), "a closed socket stays closed");
  Check(b.Send({kLoopback, port}, hello.data(), hello.size()), "sending to a closed port is not an error for the sender");

  online::UdpTransport again;
  Check(again.Open(port, true) && again.bound_port() == port, "the closed port opens again");
  online::UdpTransport taken;
  Check(!taken.Open(port, true), "a port in use is refused, so the game tries the next one");
  again.Close();
  Check(a.Open(port, true) && a.bound_port() == port, "a closed transport opens again");
}

void TestPeerAddresses() {
  uint32_t address = 0;
  using Kind = online::PeerAddressKind;
  Check(online::ParsePeerAddress("127.0.0.1", address) == Kind::kIPv4 && address == kLoopback, "an IPv4 literal");
  Check(online::ParsePeerAddress("::ffff:203.0.113.7", address) == Kind::kIPv4MappedIPv6 && address == 0xCB007107,
        "an IPv4-mapped IPv6 literal");
  Check(online::ParsePeerAddress("203.0.113.7:7101", address) == Kind::kHostPort && address == 0xCB007107,
        "host:port keeps the host");
  address = 0;
  Check(online::ParsePeerAddress("2001:db8::1", address) == Kind::kIPv6 && address == 0 &&
            !online::UsableAddress(Kind::kIPv6),
        "native IPv6 is recognised and not usable: the transport is IPv4 only");
  Check(online::ParsePeerAddress("[2001:db8::1]:7101", address) == Kind::kIPv6, "bracketed IPv6");
  Check(online::ParsePeerAddress("", address) == Kind::kEmpty, "empty");
  Check(online::ParsePeerAddress("999.1.1.1", address) == Kind::kInvalid, "out of range");
  Check(online::ParsePeerAddress("not an address", address) == Kind::kInvalid, "garbage");
  Check(online::ParseIPv4("localhost", address) && address == kLoopback, "localhost");
  Check(net::IsLan(kLoopback) && net::IsLan(net::ParseIp("192.168.1.5")) && !net::IsLan(net::ParseIp("203.0.113.7")),
        "LAN classification");
  Check(net::Classify(net::ParseIp("100.64.0.1")) == net::AddressKind::kCarrierNat, "carrier-grade NAT range");
  Check(net::IpText(0xC0A80105) == "192.168.1.5" && net::ParseIp("1.2.3") == 0, "text forms");
}

Bytes StunResponse(const net::stun::TransactionId& id, uint16_t type, const Bytes& attributes) {
  Bytes out = {uint8_t(type >> 8), uint8_t(type), uint8_t(attributes.size() >> 8), uint8_t(attributes.size()),
               0x21, 0x12, 0xA4, 0x42};
  out.insert(out.end(), id.begin(), id.end());
  out.insert(out.end(), attributes.begin(), attributes.end());
  return out;
}

Bytes Attribute(uint16_t type, const Bytes& value) {
  Bytes out = {uint8_t(type >> 8), uint8_t(type), uint8_t(value.size() >> 8), uint8_t(value.size())};
  out.insert(out.end(), value.begin(), value.end());
  while (out.size() % 4) out.push_back(0);
  return out;
}

Bytes XorMapped(uint32_t ip, uint16_t port) {
  const uint16_t xport = port ^ 0x2112;
  const uint32_t xip = ip ^ 0x2112A442;
  return Attribute(0x0020, {0, 1, uint8_t(xport >> 8), uint8_t(xport), uint8_t(xip >> 24), uint8_t(xip >> 16),
                            uint8_t(xip >> 8), uint8_t(xip)});
}

Bytes PlainMapped(uint32_t ip, uint16_t port) {
  return Attribute(0x0001, {0, 1, uint8_t(port >> 8), uint8_t(port), uint8_t(ip >> 24), uint8_t(ip >> 16),
                            uint8_t(ip >> 8), uint8_t(ip)});
}

void TestStunCodec() {
  net::stun::TransactionId id{};
  for (size_t i = 0; i < id.size(); ++i) id[i] = uint8_t(0xA0 + i);
  uint8_t request[net::stun::kRequestSize];
  net::stun::MakeRequest(id, request);
  Check(request[0] == 0x00 && request[1] == 0x01 && request[2] == 0 && request[3] == 0, "a Binding request without attributes");
  Check(request[4] == 0x21 && request[5] == 0x12 && request[6] == 0xA4 && request[7] == 0x42, "with the magic cookie");
  Check(std::memcmp(request + 8, id.data(), id.size()) == 0, "and the transaction id");

  net::Endpoint mapped;
  const Bytes good = StunResponse(id, 0x0101, XorMapped(0xCB007107, 41000));
  Check(net::stun::ParseResponse(good.data(), good.size(), id, mapped) && mapped.ip == 0xCB007107 && mapped.port == 41000,
        "XOR-MAPPED-ADDRESS gives the public endpoint");
  mapped = {};
  const Bytes plain = StunResponse(id, 0x0101, PlainMapped(0xCB007108, 41001));
  Check(net::stun::ParseResponse(plain.data(), plain.size(), id, mapped) && mapped.ip == 0xCB007108 && mapped.port == 41001,
        "MAPPED-ADDRESS is the fallback");
  Bytes both = PlainMapped(0x01010101, 1);
  const Bytes xored = XorMapped(0xCB007107, 41000);
  both.insert(both.end(), xored.begin(), xored.end());
  const Bytes preferred = StunResponse(id, 0x0101, both);
  mapped = {};
  Check(net::stun::ParseResponse(preferred.data(), preferred.size(), id, mapped) && mapped.ip == 0xCB007107,
        "the XOR form wins when both are present");

  net::stun::TransactionId other = id;
  other[11] ^= 1;
  Check(!net::stun::ParseResponse(good.data(), good.size(), other, mapped), "another transaction's answer is ignored");
  const Bytes error = StunResponse(id, 0x0111, XorMapped(0xCB007107, 41000));
  Check(!net::stun::ParseResponse(error.data(), error.size(), id, mapped), "an error response is not a mapping");
  Check(!net::stun::ParseResponse(good.data(), 19, id, mapped), "a short packet");
  Bytes truncated = good;
  truncated.resize(good.size() - 4);
  Check(!net::stun::ParseResponse(truncated.data(), truncated.size(), id, mapped), "a packet shorter than its length field");
  Bytes bad_cookie = good;
  bad_cookie[5] ^= 0xFF;
  Check(!net::stun::ParseResponse(bad_cookie.data(), bad_cookie.size(), id, mapped), "a wrong magic cookie");
  const Bytes v6 = StunResponse(id, 0x0101, Attribute(0x0020, {0, 2, 0x11, 0x22, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}));
  Check(!net::stun::ParseResponse(v6.data(), v6.size(), id, mapped), "an IPv6 mapping gives no IPv4 endpoint");
  const Bytes empty_attributes = StunResponse(id, 0x0101, {});
  Check(!net::stun::ParseResponse(empty_attributes.data(), empty_attributes.size(), id, mapped), "no mapped address at all");
  const Bytes zero = StunResponse(id, 0x0101, XorMapped(0, 0));
  Check(!net::stun::ParseResponse(zero.data(), zero.size(), id, mapped) || mapped.valid(), "a zero endpoint is never valid");
  Bytes overrun = StunResponse(id, 0x0101, XorMapped(0xCB007107, 41000));
  overrun[22] = 0xFF;
  Check(!net::stun::ParseResponse(overrun.data(), overrun.size(), id, mapped), "an attribute longer than the packet");
  Check(net::StunServers("off").empty() && net::StunServers("").empty(), "off and empty mean no STUN");
  const auto servers = net::StunServers("a.example:3479,b.example");
  Check(servers.size() == 2 && servers[0].second == 3479 && servers[1].second == 3478, "server list with the default port");
}

class StunResponder {
 public:
  StunResponder() {
    udp_.Open(0, true);
    thread_ = std::thread([this] {
      uint8_t buffer[512];
      online::Endpoint from;
      while (!stop_) {
        const size_t n = udp_.Receive(buffer, sizeof buffer, from);
        if (!n) {
          std::this_thread::sleep_for(std::chrono::milliseconds(2));
          continue;
        }
        if (n != net::stun::kRequestSize) continue;
        ++requests_;
        if (silent_) continue;
        net::stun::TransactionId id;
        std::memcpy(id.data(), buffer + 8, id.size());
        const Bytes reply = StunResponse(id, 0x0101, XorMapped(from.address, from.port));
        udp_.Send(from, reply.data(), reply.size());
      }
    });
  }
  ~StunResponder() {
    stop_ = true;
    thread_.join();
  }
  uint16_t port() const { return udp_.bound_port(); }
  void Silence(bool silent) { silent_ = silent; }
  int requests() const { return requests_; }

 private:
  online::UdpTransport udp_;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> silent_{false};
  std::atomic<int> requests_{0};
};

void TestStunQuery() {
  Check(net::SocketsReady(), "sockets are ready");
  const net::SocketHandle client = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  const sockaddr_in any = net::ToSockaddr({kLoopback, 0});
  Check(client != net::kNoSocket && bind(client, reinterpret_cast<const sockaddr*>(&any), sizeof any) == 0, "a client socket");
  sockaddr_in bound{};
#ifdef _WIN32
  int length = sizeof bound;
#else
  socklen_t length = sizeof bound;
#endif
  getsockname(client, reinterpret_cast<sockaddr*>(&bound), &length);
  const net::Endpoint local = net::FromSockaddr(bound);

  StunResponder responder;
  net::Endpoint mapped;
  Check(net::stun::Query(client, {kLoopback, responder.port()}, 2000, mapped), "a Binding request is answered");
  Check(mapped.ip == kLoopback && mapped.port == local.port, "the answer names this socket's own endpoint");

  responder.Silence(true);
  const auto before = std::chrono::steady_clock::now();
  mapped = {};
  Check(!net::stun::Query(client, {kLoopback, responder.port()}, 400, mapped), "a silent server times out");
  const auto took = std::chrono::steady_clock::now() - before;
  Check(took >= std::chrono::milliseconds(350) && took < std::chrono::milliseconds(1500), "within the time asked for");
  Check(responder.requests() >= 2, "and the request was repeated meanwhile");
  net::CloseSocket(client);
}

}  // namespace

int main() {
  TestUdpLoopback();
  TestPeerAddresses();
  TestStunCodec();
  TestStunQuery();
  return online_test::Finish();
}
