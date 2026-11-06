#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "online/constants.h"

namespace eck::online {

enum class FriendState { kOffline, kOnline, kHosting, kConnecting, kInGame };

struct Friend {
  std::string player;  // the friend code, XXXX-XXXX
  std::string name;
  bool online = false;
  FriendState state = FriendState::kOffline;
  std::string platform;
  std::string version;
  std::string image;
  int build = 0;
  bool compatible = true;
};

struct Request {
  std::string player;
  std::string name;
};

// Opaque to the service: up to 16 small named integers (mode, track, laps...).
using RoomRules = std::vector<std::pair<std::string, int>>;

enum class SeatState { kEmpty, kConnected, kReconnecting };
enum class PickState { kNone, kSelecting, kConfirmed };  // same values as the game's EKartPlayerSelectIndividualState
inline constexpr int kCharacters = 12;
enum class RoomState { kOpen, kStarting, kRacing, kClosed };
enum class PathKind { kUnknown, kDirect, kRelay };

struct Seat {
  int seat = 0;
  std::string player;
  std::string name;
  SeatState state = SeatState::kEmpty;
  bool ready = false;
  std::string platform;
  std::string version;
  PathKind transport = PathKind::kUnknown;  // the host's reported path to this seat
  PickState pick = PickState::kNone;
  int character = 0;
};

struct RoomView {
  std::string room;
  std::string host;
  std::string host_name;
  int capacity = kMinSeats;
  int players = 0;
  RoomState state = RoomState::kOpen;
  std::string image;
  int build = 0;
  RoomRules rules;
  std::vector<Seat> seats;  // exactly `capacity` entries, index = seat
  int64_t created = 0;
  int64_t updated = 0;
};

struct Invite {
  std::string id;
  std::string host;
  std::string host_name;
  int players = 0;
  int capacity = kMinSeats;
  RoomRules rules;
  std::string image;
  int build = 0;
  std::string platform;
  std::string version;
  int64_t expires = 0;
};

struct ServiceCapabilities {
  bool announced = false;
  int protocol_version = 0;
  std::string worker_build;
  bool public_endpoints = false;
  bool relay_supported = false;
  int relay_max_payload = 0;
  int min_players = kMinSeats;
  int max_players = kMaxSeats;
  bool four_player_rooms = false;
  bool seat_routing = false;
  bool race_telemetry = false;
  bool room_reopen = false;
};

// One race, as the service counts it. `room` is empty for a race of this PC alone.
struct RaceStartFields {
  int race = 0;
  std::string room;
  std::string competition;
  int players = 1;
  std::optional<int> track;
  std::vector<int> characters;
};

struct RaceCompleteFields {
  int race = 0;
  std::string room;
  int duration_ms = 0;
  std::vector<int> places;  // same order as the characters of the start
};

struct EndpointCandidate {
  std::string ip;
  uint16_t port = 0;
  std::string kind;  // host, srflx or upnp
};

// Another seat's gameplay endpoint (match.peer). `ip` is the address the service saw.
struct PeerInfo {
  std::string room;
  int seat = -1;
  std::string player;
  std::string name;
  std::string ip;
  uint16_t port = 0;
  std::vector<std::string> lan;
  std::string public_ip;
  uint16_t public_port = 0;
  std::vector<EndpointCandidate> candidates;
  bool same_network = false;
  bool relay = false;
};

struct ServerMessage {
  enum class Kind {
    kResponse,
    kFriendRequest,
    kFriendAdded,
    kFriendRemoved,
    kFriendStatus,
    kFriendOffline,
    kRoomCreated,
    kRoomUpdated,
    kRoomClosed,
    kInviteReceived,
    kInviteEnded,
    kMatchPeer,
    kMatchTransport,
    kRelayClosed,
  };
  Kind kind = Kind::kResponse;

  // Responses
  int seq = 0;
  std::string code;  // "ok" or an error code
  std::string friend_code;
  std::optional<std::vector<Friend>> friends;
  std::optional<std::vector<Request>> incoming;
  std::optional<std::vector<Request>> outgoing;
  std::string state;  // friends.add: "pending" or "friends"
  std::optional<RoomView> room;  // room.host, invite.accept, room.start, match.resume
  int seat = -1;                 // own seat, with `room`
  uint32_t relay_token = 0;      // nonzero when the room has a relay for this client
  std::string invite_id;         // invite.send
  int64_t invite_expires = 0;
  std::string resume;  // hello: the room id of a seat still held
  std::optional<ServiceCapabilities> capabilities;
  int64_t server_time = 0;

  // Events
  Friend friend_view;
  Request request;
  std::string player;   // friend.removed, friend.offline
  std::string room_id;  // room.closed, invite.ended, match.transport, relay.closed
  std::string reason;
  std::optional<Invite> invite;  // invite.received
  PeerInfo peer;                 // match.peer
  PathKind transport = PathKind::kUnknown;  // match.transport: the host's path to this client
  int transport_seat = -1;                  // match.transport: the host's seat
};

std::optional<ServerMessage> DecodeServerMessage(std::string_view text);

struct HelloFields {
  std::string image = kImageId;
  int build = kBuildNumber;
  std::string player;
  std::string token;
  std::string name;
  std::string version;
  std::string platform;
  bool presence = true;  // false: a probe that is not shown to friends (the connection test)
  bool relay = false;
};

std::string EncodeHello(int seq, const HelloFields& hello);
std::string EncodePing(int seq);
std::string EncodeFriendsList(int seq);
std::string EncodeFriendsRequests(int seq);
std::string EncodeFriendsAdd(int seq, const std::string& code);
std::string EncodeFriendsAccept(int seq, const std::string& player);
std::string EncodeFriendsDecline(int seq, const std::string& player);
std::string EncodeFriendsRemove(int seq, const std::string& player);
std::string EncodeRoomHost(int seq, int capacity, const RoomRules& rules);
std::string EncodeRoomUpdate(int seq, const std::string& room, const RoomRules& rules);
std::string EncodeRoomLeave(int seq, const std::string& room);
std::string EncodeRoomStart(int seq, const std::string& room);
std::string EncodeRoomReopen(int seq, const std::string& room, const RoomRules& rules);
std::string EncodeInviteSend(int seq, const std::string& player);
std::string EncodeInviteAccept(int seq, const std::string& invite);
std::string EncodeInviteDecline(int seq, const std::string& invite);

struct EndpointFields {
  uint16_t port = 0;
  std::vector<std::string> lan;
  std::string public_ip;  // with public_port, the gameplay socket's STUN-mapped endpoint
  uint16_t public_port = 0;
  std::vector<EndpointCandidate> candidates;
  bool relay = false;
};
std::string EncodeMatchEndpoint(int seq, const std::string& room, const EndpointFields& endpoint);
std::string EncodeMatchReady(int seq, const std::string& room, bool ready);
std::string EncodeMatchPick(int seq, const std::string& room, PickState pick, int character);
std::string EncodeMatchLeave(int seq, const std::string& room);
// Host only: its path to one peer seat.
std::string EncodeMatchTransport(int seq, const std::string& room, int seat, PathKind transport);
std::string EncodeMatchResume(int seq, const std::string& room);
std::string EncodeRaceStart(int seq, const RaceStartFields& race);
std::string EncodeRaceComplete(int seq, const RaceCompleteFields& race);

// "abcd 2345" and "ABCD-2345" both give "ABCD-2345"; empty when the text is not a friend code.
std::string NormalizeFriendCode(std::string_view text);

// User-facing sentence for a service error code.
std::string DescribeServiceError(const std::string& code);

// Same image and build, the service's own compatibility rule.
bool ImagesCompatible(const std::string& image_a, int build_a, const std::string& image_b, int build_b);

// "Online · Windows · v0.1.0" (or "Hosting" / "Connecting" / "In game" first) for a friend that is online.
std::string FriendStatusLine(const Friend& entry);

const char* PathKindName(PathKind kind);
const char* RoomStateName(RoomState state);

}  // namespace eck::online
