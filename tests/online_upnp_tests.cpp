// UPnP port mapping against a router that is a scripted HTTP server on loopback. The real description parsing, SOAP
// requests and error handling run; only the multicast discovery is skipped, and no real router is needed.

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "online/net_addr.h"
#include "online/upnp.h"
#include "online_test_support.h"

using online_test::Check;
namespace net = eck::online::net;

namespace {

constexpr uint32_t kLoopback = 0x7F000001;
constexpr uint32_t kLan = 0xC0A80105;  // 192.168.1.5

struct Reply {
  int status = 200;
  std::string body;
  bool hang_up = false;
};

class MockRouter {
 public:
  using Handler = std::function<Reply(const std::string& request)>;

  explicit MockRouter(Handler handler) : handler_(std::move(handler)) {
    net::SocketsReady();
    listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address = net::ToSockaddr({kLoopback, 0});
    bind(listener_, reinterpret_cast<const sockaddr*>(&address), sizeof address);
    listen(listener_, 8);
#ifdef _WIN32
    int length = sizeof address;
#else
    socklen_t length = sizeof address;
#endif
    getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length);
    port_ = net::FromSockaddr(address).port;
    thread_ = std::thread([this] { Serve(); });
  }

  ~MockRouter() {
    stop_ = true;
    const net::SocketHandle wake = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    const sockaddr_in to = net::ToSockaddr({kLoopback, port_});
    connect(wake, reinterpret_cast<const sockaddr*>(&to), sizeof to);
    net::CloseSocket(wake);
    thread_.join();
    net::CloseSocket(listener_);
  }

  std::string Url(const std::string& path = "/rootDesc.xml") const {
    return "http://127.0.0.1:" + std::to_string(port_) + path;
  }

  std::vector<std::string> requests() {
    std::lock_guard lock(mutex_);
    return requests_;
  }

  int Count(const std::string& needle) {
    int n = 0;
    for (const std::string& request : requests()) n += request.find(needle) != std::string::npos;
    return n;
  }

 private:
  void Serve() {
    while (!stop_) {
      const net::SocketHandle client = accept(listener_, nullptr, nullptr);
      if (client == net::kNoSocket) continue;
      if (stop_) {
        net::CloseSocket(client);
        break;
      }
      std::string request;
      char chunk[4096];
      size_t need = std::string::npos;
      while (request.size() < (need == std::string::npos ? 1u << 20 : need)) {
        const int n = int(recv(client, chunk, sizeof chunk, 0));
        if (n <= 0) break;
        request.append(chunk, size_t(n));
        const size_t end = request.find("\r\n\r\n");
        if (end != std::string::npos && need == std::string::npos) {
          const size_t at = request.find("Content-Length: ");
          const size_t body = at == std::string::npos ? 0 : size_t(std::strtoul(request.c_str() + at + 16, nullptr, 10));
          need = end + 4 + body;
        }
      }
      {
        std::lock_guard lock(mutex_);
        requests_.push_back(request);
      }
      const Reply reply = handler_(request);
      if (!reply.hang_up) {
        const std::string text = "HTTP/1.1 " + std::to_string(reply.status) + " X\r\nContent-Length: " +
                                 std::to_string(reply.body.size()) + "\r\nConnection: close\r\n\r\n" + reply.body;
        send(client, text.data(), int(text.size()), 0);
      }
      net::CloseSocket(client);
    }
  }

  Handler handler_;
  net::SocketHandle listener_ = net::kNoSocket;
  uint16_t port_ = 0;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::mutex mutex_;
  std::vector<std::string> requests_;
};

std::string Description(const std::string& service, const std::string& control) {
  return "<root><device><serviceList><service><serviceType>urn:schemas-upnp-org:service:Layer3Forwarding:1</serviceType>"
         "<controlURL>/ignored</controlURL></service><service><serviceType>" + service + "</serviceType><controlURL>" +
         control + "</controlURL></service></serviceList></device></root>";
}

constexpr char kWanIp[] = "urn:schemas-upnp-org:service:WANIPConnection:1";

std::string Soap(const std::string& request, const std::string& action) {
  return request.find("SOAPAction: \"" + std::string(kWanIp) + "#" + action + "\"") != std::string::npos ? action : "";
}

std::string ErrorBody(int code) {
  return "<s:Envelope><s:Body><s:Fault><detail><UPnPError><errorCode>" + std::to_string(code) +
         "</errorCode></UPnPError></detail></s:Fault></s:Body></s:Envelope>";
}

Reply Router(const std::string& request, const std::function<Reply(const std::string& action)>& action) {
  if (request.rfind("GET ", 0) == 0) return {200, Description(kWanIp, "/ctl/IPConn")};
  for (const char* name : {"AddPortMapping", "DeletePortMapping", "GetExternalIPAddress"}) {
    if (!Soap(request, name).empty()) return action(name);
  }
  return {404, ""};
}

void TestAccepted() {
  MockRouter router([](const std::string& request) {
    return Router(request, [](const std::string& action) -> Reply {
      if (action == "GetExternalIPAddress") {
        return {200, "<s:Envelope><s:Body><u:R><NewExternalIPAddress>203.0.113.7</NewExternalIPAddress></u:R></s:Body></s:Envelope>"};
      }
      return {200, "<s:Envelope/>"};
    });
  });
  const net::upnp::Status status = net::upnp::ProbeGateway(router.Url(), kLan, 7101);
  Check(status.state == net::upnp::State::kMapped, "a router that accepts the mapping: mapped");
  Check(status.external.ip == net::ParseIp("203.0.113.7") && status.external.port == 7101, "the router's WAN address and port");
  Check(router.Count("AddPortMapping") == 1 && router.Count("DeletePortMapping") == 1,
        "the mapping is added once and removed again");
  const std::vector<std::string> requests = router.requests();
  const std::string add = requests.size() > 1 ? requests[1] : "";
  Check(add.rfind("POST /ctl/IPConn ", 0) == 0, "SOAP goes to the control URL of the WAN service, not another service");
  Check(add.find("<NewProtocol>UDP</NewProtocol>") != std::string::npos &&
            add.find("<NewInternalClient>192.168.1.5</NewInternalClient>") != std::string::npos &&
            add.find("<NewExternalPort>7101</NewExternalPort>") != std::string::npos &&
            add.find("<NewInternalPort>7101</NewInternalPort>") != std::string::npos &&
            add.find("<NewLeaseDuration>60</NewLeaseDuration>") != std::string::npos,
        "the mapping asks for UDP, this PC, the port and a one minute lease");
  Check(add.find("El Chavo Kart") != std::string::npos, "and names the game");
}

void TestPortConflicts() {
  std::atomic<int> taken{0};
  MockRouter router([&](const std::string& request) {
    return Router(request, [&](const std::string& action) -> Reply {
      if (action == "AddPortMapping" && taken++ < 2) return {500, ErrorBody(718)};
      return {200, "<NewExternalIPAddress>198.51.100.9</NewExternalIPAddress>"};
    });
  });
  const net::upnp::Status status = net::upnp::ProbeGateway(router.Url(), kLan, 7101);
  Check(status.state == net::upnp::State::kMapped && status.external.port == 7103, "two conflicts: the third external port is used");
  Check(router.Count("AddPortMapping") == 3, "one request per attempt");

  MockRouter always([](const std::string& request) {
    return Router(request, [](const std::string&) -> Reply { return {500, ErrorBody(718)}; });
  });
  const net::upnp::Status exhausted = net::upnp::ProbeGateway(always.Url(), kLan, 7101);
  Check(exhausted.state == net::upnp::State::kFailed && exhausted.detail.find("taken by other mappings") != std::string::npos,
        "every port taken: failed, and the player is told why");
  Check(always.Count("AddPortMapping") == 5, "five ports are tried, no more");
}

void TestLeaseRefusal() {
  std::atomic<int> adds{0};
  MockRouter router([&](const std::string& request) {
    return Router(request, [&](const std::string& action) -> Reply {
      if (action == "AddPortMapping" && adds++ == 0) return {500, ErrorBody(725)};
      return {200, "<NewExternalIPAddress>198.51.100.9</NewExternalIPAddress>"};
    });
  });
  const net::upnp::Status status = net::upnp::ProbeGateway(router.Url(), kLan, 7101);
  Check(status.state == net::upnp::State::kMapped, "a router that wants a permanent lease is asked again");
  const std::vector<std::string> requests = router.requests();
  Check(requests.size() > 2 && requests[2].find("<NewLeaseDuration>0</NewLeaseDuration>") != std::string::npos,
        "the second request has lease 0");
}

void TestRefusals() {
  MockRouter refuses([](const std::string& request) {
    return Router(request, [](const std::string&) -> Reply { return {500, ErrorBody(402)}; });
  });
  const net::upnp::Status refused = net::upnp::ProbeGateway(refuses.Url(), kLan, 7101);
  Check(refused.state == net::upnp::State::kFailed && refused.detail == "the router refused the mapping (error 402)",
        "a refusal carries the router's error code");
  Check(refuses.Count("AddPortMapping") == 1, "and is not retried");

  MockRouter silent([](const std::string& request) {
    if (request.rfind("GET ", 0) == 0) return Reply{200, Description(kWanIp, "/ctl/IPConn")};
    return Reply{200, "", true};
  });
  const net::upnp::Status no_answer = net::upnp::ProbeGateway(silent.Url(), kLan, 7101);
  Check(no_answer.state == net::upnp::State::kFailed && no_answer.detail == "the router did not answer",
        "a router that drops the connection did not answer");

  MockRouter broken([](const std::string&) { return Reply{500, "oops"}; });
  const net::upnp::Status bad = net::upnp::ProbeGateway(broken.Url(), kLan, 7101);
  Check(bad.state == net::upnp::State::kFailed && bad.detail == "no UPnP router answered", "an unusable description");

  MockRouter other_service([](const std::string&) {
    return Reply{200, Description("urn:schemas-upnp-org:service:Layer3Forwarding:1", "/x")};
  });
  Check(net::upnp::ProbeGateway(other_service.Url(), kLan, 7101).state == net::upnp::State::kFailed,
        "a device without a WAN connection service");

  Check(net::upnp::ProbeGateway("http://127.0.0.1:1/x.xml", kLan, 7101).state == net::upnp::State::kFailed,
        "nothing listening at the description URL");
  Check(net::upnp::ProbeGateway("https://router/desc.xml", kLan, 7101).state == net::upnp::State::kFailed,
        "only plain http descriptions are used");
  Check(net::upnp::ProbeGateway("", kLan, 7101).state == net::upnp::State::kFailed, "an empty URL");
}

void TestControlUrlForms() {
  auto probe = [](const std::string& control) {
    std::string seen;
    MockRouter router([&](const std::string& request) {
      if (request.rfind("GET ", 0) == 0) return Reply{200, Description(kWanIp, control)};
      if (request.rfind("POST ", 0) == 0) seen = request.substr(0, request.find(' ', 5));
      return Reply{200, "<NewExternalIPAddress>198.51.100.9</NewExternalIPAddress>"};
    });
    const net::upnp::Status status = net::upnp::ProbeGateway(router.Url(), kLan, 7101);
    return std::make_pair(status.state, seen);
  };
  Check(probe("ctl/IPConn").second == "POST /ctl/IPConn", "a control URL without a slash gets one");
  Check(probe("/ctl/IPConn").first == net::upnp::State::kMapped, "an absolute path");
  Check(probe("http://203.0.113.50:80/ctl").first == net::upnp::State::kFailed,
        "a control URL on another host is refused: the router controls only itself");
}

void TestDescriptionParsing() {
  MockRouter router([](const std::string& request) {
    if (request.rfind("GET ", 0) == 0) {
      return Reply{200, "<root><service><serviceType>urn:schemas-upnp-org:service:WANPPPConnection:1</serviceType>"
                        "<controlURL>\n  /ppp  \n</controlURL></service></root>"};
    }
    const bool ppp = request.find("WANPPPConnection:1#") != std::string::npos;
    return Reply{ppp ? 200 : 500, "<NewExternalIPAddress>198.51.100.9</NewExternalIPAddress>"};
  });
  const net::upnp::Status status = net::upnp::ProbeGateway(router.Url(), kLan, 7101);
  Check(status.state == net::upnp::State::kMapped, "a PPP WAN service with padded XML is understood");
}

}  // namespace

int main() {
  TestAccepted();
  TestPortConflicts();
  TestLeaseRefusal();
  TestRefusals();
  TestControlUrlForms();
  TestDescriptionParsing();
  return online_test::Finish();
}
