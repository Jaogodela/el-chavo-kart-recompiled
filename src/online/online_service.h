#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "online/connection_test.h"
#include "online/constants.h"
#include "online/establishment.h"
#include "online/game_socket.h"
#include "online/identity.h"
#include "online/lobby_link.h"
#include "online/peer_table.h"
#include "online/protocol.h"
#include "online/rendezvous.h"

namespace eck::online {

struct ServiceConfig {
  std::string url = kDefaultServiceUrl;
  std::string image = kImageId;
  int build = kBuildNumber;
  std::string version;   // the port version friends see
  std::string platform;  // Windows or Linux
  std::vector<std::string> lan;  // this PC's own addresses, offered to a peer on the same network
  std::function<Identity()> identity;
  std::function<void(const std::string&)> log;
  ChannelFactory open_channel = OpenLobbyLink;
  std::string stun = net::kDefaultStunServers;
  bool upnp = true;
  std::function<ConnectionReport(const ConnectionTestRequest&)> test = RunConnectionTest;
  // The early-bound gameplay socket. Every room learns its public endpoint from it and proves each peer's endpoint over
  // it. Null means signaling only: the room works but no gameplay path is set up.
  OnlineGameSocket* game_socket = nullptr;
  // The STUN servers of `stun` as addresses; the default resolves them through DNS.
  std::function<std::vector<Endpoint>(const std::string& stun)> resolve_stun;
  // Tests run the connection test and the STUN lookup inline instead of on a thread.
  bool synchronous_probes = false;
  // Tells the service this client can play through its relay (with a gameplay socket only).
  bool relay = true;
  // The gameplay UDP ports in the order the socket and the connection test try them.
  std::vector<uint16_t> game_ports = GamePortRange();
};

struct UserSettings {
  bool enabled = false;
  std::string name = kDefaultOnlineName;
};

// One other seat of the current room, as the game and the launcher see it.
struct PeerSnapshot {
  int seat = -1;
  std::string name;
  bool connected = false;  // false while reconnecting
  PeerPath path = PeerPath::kPending;
  std::optional<int> rtt_ms;  // set by the game side through SetPeerRtt
};

struct Snapshot {
  Connection connection = Connection::kOff;
  std::string connection_text;
  std::string friend_code;
  bool fetched = false;
  std::vector<Friend> friends;
  std::vector<Request> incoming;  // friend requests to answer
  std::vector<Request> outgoing;  // friend requests still waiting
  std::vector<Invite> invites;    // friends' invitations to their room, oldest first
  std::vector<RoomAd> rooms;      // the last room.search answer
  int search_generation = 0;      // grows with every answered search
  std::optional<RoomView> room;   // the room this player is seated in
  int seat = -1;                  // this player's seat in `room`
  bool reconnecting = false;      // signed out of the service with a seat still held
  std::vector<PeerSnapshot> peers;
  bool paths_ready = false;       // every connected peer has a direct or relay path
  std::string closed_reason;      // why the last room ended, until a new one begins
  std::string notice;             // the last thing worth telling the player; stays until replaced
  bool testing = false;
  std::optional<ConnectionReport> test;
  int tests_finished = 0;  // connection tests that produced a report
};

// Who this player is on the service right now, for the game's profile. Never the token.
struct OnlineProfile {
  bool signed_in = false;
  std::string friend_code;
  std::string name;
};

// What AddFriend did with the text it was given; only kSent reaches the service.
enum class FriendRequestResult { kSent, kNotConnected, kInvalid, kOwnCode, kAlreadyFriends, kAlreadyPending };

// Where the game sends one peer's datagrams: a proven UDP endpoint, or RelayPeerEndpoint(seat) through the relay.
struct GamePeer {
  int seat = -1;
  std::string name;
  Endpoint endpoint;
  bool relay = false;
};

// The player's connection to the online service: identity, friends, presence, rooms of 2 to 4 seats, invites and the
// per-seat UDP paths between the seated players. It signals and proves each path on the gameplay socket; the race
// itself never passes through it unless a seat is relayed. Thread-safe.
class OnlineService {
 public:
  explicit OnlineService(ServiceConfig config);
  ~OnlineService();
  OnlineService(const OnlineService&) = delete;
  OnlineService& operator=(const OnlineService&) = delete;

  void Apply(const UserSettings& settings);
  // Every frame, from whichever thread is presenting.
  void Update(double now);

  FriendRequestResult AddFriend(const std::string& code);
  void AcceptFriend(const std::string& player);
  void DeclineFriend(const std::string& player);  // also withdraws an outgoing request
  void RemoveFriend(const std::string& player);

  // Rooms. The service assigns seats; the host is seat 0.
  bool CreateRoom(int capacity, const RoomRules& rules);
  bool UpdateRoom(int capacity, const RoomRules* rules);  // host, room open; capacity < 0 keeps it
  void SearchRooms();
  bool JoinRoom(const std::string& handle);  // a handle of the last search
  bool StartRace();                          // host: freezes the seats
  bool SetReady(bool ready);
  void LeaveRoom();  // the host leaving closes the room for everyone
  void CancelRoom();
  bool SendInvite(const std::string& player);
  bool AcceptInvite(const std::string& invite);
  void DeclineInvite(const std::string& invite);

  bool SignedIn() const;
  OnlineProfile Profile() const;
  ServiceCapabilities Capabilities() const;
  void TestConnection();
  // A line from the launcher page, in the same log.
  void LogLine(const std::string& line) { Log(line); }

  // Round trip to one peer, measured by the game's own traffic.
  void SetPeerRtt(int seat, std::optional<int> rtt_ms);
  // The peers whose path is decided, with where to send their datagrams.
  std::vector<GamePeer> GamePeers() const;
  // The seat a datagram from `from` belongs to, direct or relayed; -1 when none.
  int SeatOfEndpoint(const Endpoint& from) const;

  Snapshot snapshot() const;
  std::vector<std::string> TakeNotices();
  const std::string& url() const { return config_.url; }

 private:
  enum class Step { kIdle, kOpening, kGreeting, kOnline };
  enum class Purpose {
    kHello, kFriends, kRequests, kAdd, kAccept, kDecline, kRemove, kRoomHost, kRoomUpdate, kRoomLeave, kRoomSearch,
    kRoomJoin, kRoomStart, kInviteSend, kInviteAccept, kInviteDecline, kEndpoint, kReady, kResume, kTransport
  };
  struct Pending {
    Purpose purpose = Purpose::kHello;
    std::string player;
    double sent_at = 0;
  };

  void OpenChannel(double now);
  void PumpChannel(double now);
  void OnEvent(const ChannelEvent& event, double now);
  void OnText(const std::string& text, double now);
  void OnResponse(const ServerMessage& message, double now);
  void OnServerEvent(const ServerMessage& message, double now);
  void OnGreeted(const ServerMessage& message, double now);
  void OnLinkLost(const std::string& why, double now);
  void OnSeated(const ServerMessage& message);
  void OnRoomView(const RoomView& room);
  void OnRoomClosed(const std::string& reason);
  void OnPeer(const PeerInfo& peer);
  void ClearRoom(bool keep_reason);
  void UpdateFriend(const Friend& entry);
  void Refresh();
  void Send(Purpose purpose, const std::function<std::string(int)>& encode, const std::string& player = {});
  void SendLocked(const std::string& text);
  bool Connected() const { return step_ == Step::kOnline; }
  void Notice(const std::string& text);
  void Log(const std::string& line);
  void Disable();
  void CollectTest();
  bool EnsureIdentity();
  std::string SessionName() const;
  bool InRoom() const { return room_.has_value(); }
  bool IsHost() const { return room_ && seat_ == 0; }

  // Gameplay path setup on the socket.
  void PumpPaths(double now);
  void SendEndpoint();
  void BeginStun();
  std::optional<std::vector<Endpoint>> StunServers();
  void StartPunchFor(const PeerState& peer, double now);
  void ApplyChoices(const std::vector<PathChoice>& choices);
  void FailPaths(const std::string& reason);

  ServiceConfig config_;
  mutable std::mutex mutex_;
  UserSettings settings_;
  Identity identity_;
  bool identity_loaded_ = false;

  Connection connection_ = Connection::kOff;
  std::string connection_text_;
  std::shared_ptr<LobbyChannel> channel_;
  std::shared_ptr<LobbyChannel> leaving_;
  ServiceCapabilities capabilities_;
  double leaving_deadline_ = 0;
  double now_ = 0;
  double step_deadline_ = 0;
  Step step_ = Step::kIdle;
  double retry_at_ = 0;
  double retry_delay_ = 1.0;
  double next_refresh_ = 0;
  double name_since_ = 0;
  std::string greeted_name_;
  int next_seq_ = 1;
  std::map<int, Pending> pending_;

  std::string friend_code_;
  bool fetched_ = false;
  std::vector<Friend> friends_;
  std::vector<Request> incoming_, outgoing_;
  std::vector<Invite> invites_;
  std::vector<RoomAd> rooms_;
  int search_generation_ = 0;
  std::string notice_;
  std::vector<std::string> notices_;
  std::string closed_reason_;

  std::optional<RoomView> room_;
  int seat_ = -1;
  uint32_t relay_token_ = 0;
  std::string held_room_;  // a seat held across a lost connection, to resume
  std::map<int, int> rtt_;

  struct TestState;
  std::shared_ptr<TestState> test_probe_;
  std::optional<ConnectionReport> test_;
  bool testing_ = false;
  int tests_finished_ = 0;

  enum class Udp { kNone, kDiscovering, kReady };
  struct StunLookup;
  std::shared_ptr<StunLookup> stun_lookup_;
  std::optional<std::vector<Endpoint>> stun_servers_;
  Rendezvous rendezvous_;
  PeerTable peers_;
  Udp udp_ = Udp::kNone;
  bool stun_started_ = false;
  Endpoint mapped_;
  bool endpoint_sent_ = false;
  std::array<bool, kMaxSeats> punching_{};
  Establishment establishment_;
  bool race_started_ = false;
};

}  // namespace eck::online
