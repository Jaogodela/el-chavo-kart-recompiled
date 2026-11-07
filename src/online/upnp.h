#pragma once

#include <cstdint>
#include <string>

#include "online/net_addr.h"

namespace eck::online::net::upnp {

enum class State { kOff, kMapped, kFailed };

struct Status {
  State state = State::kOff;
  Endpoint external;   // the router's WAN address and the mapped port
  std::string detail;  // what happened, for the log and the connection test
};

// The connection test: maps UDP `port` on the home router (Internet Gateway Device) for a minute and removes it again.
Status Probe(uint16_t port);

// The same mapping through the router whose device description is at `description_url`, skipping discovery.
Status ProbeGateway(const std::string& description_url, uint32_t lan_ip, uint16_t port);

}  // namespace eck::online::net::upnp
