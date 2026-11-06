#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <initializer_list>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "online/constants.h"
#include "online/relay_frame.h"
#include "online/udp_transport.h"

namespace eck::online {

// Sees every datagram the socket receives before the game does; true keeps it from the game. `reply` sends from the
// same socket and may be used during the call.
class DatagramTap {
 public:
  virtual ~DatagramTap() = default;
  virtual bool Take(const Endpoint& from, const uint8_t* data, size_t size, Transport& reply) = 0;
};

// Hands one encoded relay frame to the online service's connection; false when it cannot take it now.
using RelaySend = std::function<bool(std::vector<uint8_t>&&)>;

inline constexpr size_t kRelaySampledSizes = 4;

struct SocketTraffic {
  uint64_t udp_tx_packets = 0;
  uint64_t udp_rx_packets = 0;
  uint64_t relay_tx_packets = 0;
  uint64_t relay_tx_bytes = 0;
  uint64_t relay_rx_packets = 0;
  uint64_t relay_rx_bytes = 0;
  uint64_t relay_rejected = 0;
};

// The one UDP socket online play uses, bound early and reused by every room: STUN, punching and the game's own
// traffic share it because the NAT mapping the peers were told about belongs to this socket alone. A seat reached
// through the service's relay is addressed as RelayPeerEndpoint(seat): sends to it go out as relay frames to that
// seat, and frames from it come out of Receive with the same endpoint, so the game never knows the difference.
// Thread-safe.
class OnlineGameSocket final : public Transport {
 public:
  // Binds the first free port of `ports`. Once open it stays on that port: later calls never rebind.
  bool Open(const std::vector<uint16_t>& ports, bool loopback_only = false);
  bool is_open() const;
  uint16_t bound_port() const;

  void SetTap(DatagramTap* tap);
  // Runs `use` with the socket held, so the tap's own state and sends stay consistent with Take.
  void Use(const std::function<void(Transport&)>& use);
  // Lets the tap see what has arrived when the game is not reading; game datagrams wait for Receive.
  void Pump();

  // The room's relay: from now on its frames (those carrying `token`) are queued. Replaces any earlier relay.
  void OpenRelay(uint32_t token, RelaySend send);
  // Whether sends to RelayPeerEndpoint(seat) go through the relay.
  void SelectRelay(int seat, bool selected);
  bool relay_selected(int seat) const;
  // Forgets the relay, every seat's selection and everything it queued.
  void CloseRelay();
  bool relay_open() const;
  // One binary message from the online service; false when it is not a frame of the open relay.
  bool DeliverRelay(const uint8_t* data, size_t size);
  // Why the relay can no longer carry the room ("send_refused"); empty while it can.
  std::string relay_failure() const;

  void ResetTraffic();
  SocketTraffic traffic() const;

  bool Send(const Endpoint& to, const uint8_t* data, size_t size) override;
  size_t Receive(uint8_t* buffer, size_t capacity, Endpoint& from) override;

 private:
  bool TakenByTap(const Endpoint& from, const uint8_t* data, size_t size);
  bool SendRelayLocked(int seat, const uint8_t* data, size_t size);

  mutable std::mutex mutex_;
  UdpTransport socket_;
  DatagramTap* tap_ = nullptr;
  std::deque<std::pair<Endpoint, std::vector<uint8_t>>> waiting_;

  uint32_t relay_token_ = 0;
  std::array<bool, kMaxSeats> relay_seat_{};
  RelaySend relay_send_;
  uint32_t relay_seq_ = 0;
  std::deque<std::pair<int, std::vector<uint8_t>>> relay_in_;
  std::string relay_failure_;
  SocketTraffic traffic_;
};

}  // namespace eck::online
