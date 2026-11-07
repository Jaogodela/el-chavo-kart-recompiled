// What the Connection test tells the player for every combination of what it found: the verdict, whether Play may
// treat the PC as ready, and the lines the Online page shows. Pure: no sockets, no service.

#include <string>
#include <vector>

#include "online/connection_test.h"
#include "online/net_addr.h"
#include "online_test_support.h"

using online_test::Check;
namespace online = eck::online;
using online::ConnectionFacts;
using online::ConnectionReport;
using Mark = ConnectionReport::Mark;
using Router = ConnectionFacts::Router;

namespace {

ConnectionFacts Good() {
  ConnectionFacts f;
  f.lobby_ok = true;
  f.lobby_latency_ms = 42;
  f.udp_ok = true;
  f.second_answer = true;
  f.same_mapping = true;
  f.relay_ok = true;
  f.udp_port = 7101;
  return f;
}

const ConnectionReport::Line* Find(const ConnectionReport& report, const std::string& label) {
  for (const ConnectionReport::Line& line : report.lines) {
    if (line.label == label) return &line;
  }
  return nullptr;
}

bool Says(const ConnectionReport& report, const std::string& label, const std::string& text) {
  const ConnectionReport::Line* line = Find(report, label);
  return line && line->text.find(text) != std::string::npos;
}

struct Case {
  const char* name;
  ConnectionFacts facts;
  bool ready;
  const char* verdict;
};

void TestVerdictMatrix() {
  std::vector<Case> cases;
  auto add = [&](const char* name, bool ready, const char* verdict, auto&& change) {
    ConnectionFacts f = Good();
    change(f);
    cases.push_back({name, f, ready, verdict});
  };
  add("everything works", true, "Ready for online play", [](ConnectionFacts&) {});
  add("Worker unavailable", false, "Not ready: cannot reach the online service", [](ConnectionFacts& f) { f.lobby_ok = false; });
  add("Worker refuses this game", false, "Not ready: the online service did not accept this game", [](ConnectionFacts& f) {
    f.lobby_ok = false;
    f.lobby_refusal = "The online service needs a different version of the game.";
  });
  add("no identity", false, "Not ready: cannot reach the online service", [](ConnectionFacts& f) { f.identity_ok = false; });
  add("STUN failed, no relay", false, "Limited: direct connections may be blocked by your network", [](ConnectionFacts& f) {
    f.udp_ok = f.second_answer = f.same_mapping = f.relay_ok = false;
  });
  add("STUN failed, relay available", true, "Ready for online play", [](ConnectionFacts& f) {
    f.udp_ok = f.second_answer = f.same_mapping = false;
  });
  add("one STUN answer, no relay", false, "Probably ready: your network's NAT type could not be fully tested",
      [](ConnectionFacts& f) { f.second_answer = f.same_mapping = f.relay_ok = false; });
  add("one STUN answer, relay available", true, "Ready for online play",
      [](ConnectionFacts& f) { f.second_answer = f.same_mapping = false; });
  add("symmetric NAT, no relay, no router", false, "Limited: some players may not be able to connect to you",
      [](ConnectionFacts& f) { f.same_mapping = f.relay_ok = false; });
  add("symmetric NAT, relay available", true, "Ready for online play", [](ConnectionFacts& f) { f.same_mapping = false; });
  add("symmetric NAT, router opened", true, "Ready for online play", [](ConnectionFacts& f) {
    f.same_mapping = f.relay_ok = false;
    f.router = Router::kMapped;
  });
  add("symmetric NAT, router behind another NAT", false, "Limited: some players may not be able to connect to you",
      [](ConnectionFacts& f) {
        f.same_mapping = f.relay_ok = false;
        f.router = Router::kMappedBehindNat;
      });
  add("the PC has a public address", true, "Ready for online play", [](ConnectionFacts& f) {
    f.same_mapping = f.relay_ok = false;
    f.public_address = true;
  });
  add("UDP port could not be opened, relay offered", false, "Limited: the multiplayer UDP port could not be opened",
      [](ConnectionFacts& f) {
        f.closed_ports = "7101-7110";
        f.udp_ok = f.second_answer = f.same_mapping = false;
      });
  add("STUN off, relay available", true, "Ready for online play", [](ConnectionFacts& f) {
    f.stun_off = true;
    f.udp_ok = f.second_answer = f.same_mapping = false;
  });
  add("STUN off, no relay", false, "Limited: some players may not be able to connect to you", [](ConnectionFacts& f) {
    f.stun_off = true;
    f.udp_ok = f.second_answer = f.same_mapping = f.relay_ok = false;
  });

  for (const Case& test : cases) {
    const ConnectionReport report = online::SummarizeConnection(test.facts);
    Check(report.verdict == test.verdict, std::string(test.name) + ": verdict '" + report.verdict + "'");
    Check(report.ready == test.ready, std::string(test.name) + ": ready is " + (test.ready ? "true" : "false"));
    Check(!report.lines.empty() && !report.details.empty(), std::string(test.name) + ": lines and log details exist");
    for (const ConnectionReport::Line& line : report.lines) {
      Check(line.text.find("203.0.113") == std::string::npos && line.text.find("127.0.0") == std::string::npos,
            std::string(test.name) + ": no address on screen");
    }
  }
}

void TestLines() {
  const ConnectionReport ready = online::SummarizeConnection(Good());
  Check(ready.latency_ms == 42 && Says(ready, "Online service", "42"), "the service line carries the sign-in time");
  Check(Find(ready, "Online service")->mark == Mark::kOk && Find(ready, "Direct connections")->mark == Mark::kOk,
        "service and direct connections are OK");
  Check(Says(ready, "Direct connections", "UDP port 7101"), "the opened port is named");
  Check(Find(ready, "Relay") && Find(ready, "Relay")->mark == Mark::kOk, "the relay is reported when offered");

  ConnectionFacts down = Good();
  down.lobby_ok = false;
  const ConnectionReport unavailable = online::SummarizeConnection(down);
  Check(Find(unavailable, "Online service")->mark == Mark::kProblem, "an unreachable service is a problem");

  ConnectionFacts relayed = Good();
  relayed.udp_ok = relayed.second_answer = relayed.same_mapping = false;
  const ConnectionReport via_relay = online::SummarizeConnection(relayed);
  Check(Find(via_relay, "Direct connections")->mark == Mark::kNote, "a blocked direct path is only a note when the relay stands in");

  ConnectionFacts closed = Good();
  closed.closed_ports = "7101-7110";
  closed.udp_ok = closed.second_answer = closed.same_mapping = false;
  const ConnectionReport no_port = online::SummarizeConnection(closed);
  Check(Find(no_port, "Direct connections")->mark == Mark::kProblem && !Find(no_port, "Relay"),
        "with no UDP port open the relay is not offered, it plays on the same socket");

  ConnectionFacts double_nat = Good();
  double_nat.same_mapping = double_nat.relay_ok = false;
  double_nat.router = Router::kMappedBehindNat;
  Check(Says(online::SummarizeConnection(double_nat), "Router configuration", "Another router is in front"),
        "a double NAT is explained");
  ConnectionFacts no_upnp = Good();
  no_upnp.same_mapping = no_upnp.relay_ok = false;
  Check(Says(online::SummarizeConnection(no_upnp), "Router configuration", "Automatic setup is not available"),
        "a router without UPnP is explained");
}

void TestRouterStatus() {
  using eck::online::net::ParseIp;
  Check(online::ClassifyRouter(ParseIp("203.0.113.7")) == Router::kMapped, "a public router address");
  Check(online::ClassifyRouter(ParseIp("177.18.65.229")) == Router::kMapped, "another public router address");
  Check(online::ClassifyRouter(ParseIp("192.168.0.1")) == Router::kMappedBehindNat, "a private router address");
  Check(online::ClassifyRouter(ParseIp("10.0.0.1")) == Router::kMappedBehindNat, "10/8");
  Check(online::ClassifyRouter(ParseIp("100.64.0.1")) == Router::kMappedBehindNat, "carrier-grade NAT");
  Check(online::ClassifyRouter(0) == Router::kMappedBehindNat, "an empty address from the router");
}

void TestRunWithoutAnyNetwork() {
  online::ConnectionTestRequest request;
  request.url = "ws://127.0.0.1:1/";
  request.identity.error = "no identity in this test";
  request.stun = "off";
  request.upnp = false;
  request.game_ports = {};
  bool probed = false;
  request.probe_upnp = [&](uint16_t) {
    probed = true;
    return eck::online::net::upnp::Status{};
  };
  const ConnectionReport report = online::RunConnectionTest(request);
  Check(!report.ready && report.verdict == "Not ready: cannot reach the online service", "without an identity it is not ready");
  Check(!probed, "UPnP is not tried when it is turned off");
  bool details_name_identity = false;
  for (const std::string& detail : report.details) details_name_identity |= detail.find("no online identity") != std::string::npos;
  Check(details_name_identity, "the log says why");
}

}  // namespace

int main() {
  TestVerdictMatrix();
  TestLines();
  TestRouterStatus();
  TestRunWithoutAnyNetwork();
  return online_test::Finish();
}
