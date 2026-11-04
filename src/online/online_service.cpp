#include "online/online_service.h"

#include <algorithm>
#include <exception>
#include <thread>

#include "online/net_addr.h"
#include "online/stun.h"
#include "ui_text.h"

namespace eck::online {
namespace {

constexpr double kStepTimeoutSeconds = 15.0;
constexpr double kMaxRetrySeconds = 20.0;
constexpr double kFatalRetrySeconds = 60.0;
constexpr double kRefreshSeconds = 30.0;
constexpr double kNameSettleSeconds = 1.5;
constexpr double kLeaveFlushSeconds = 2.0;

bool Fatal(const std::string& code) {
  return code == "proto" || code == "bad_game" || code == "bad_image" || code == "unauthorized";
}

std::vector<Endpoint> ResolveStunServers(const std::string& stun_config) {
  std::vector<Endpoint> servers;
  if (!net::SocketsReady()) return servers;
  for (const auto& [host, port] : net::StunServers(stun_config)) {
    if (const uint32_t ip = net::ResolveIpv4(host)) servers.push_back({ip, port});
  }
  return servers;
}

}  // namespace

struct OnlineService::TestState {
  std::mutex mutex;
  bool done = false;
  std::optional<ConnectionReport> report;  // none when the test stopped without one
  std::string stopped;
};

struct OnlineService::StunLookup {
  std::mutex mutex;
  bool done = false;
  std::vector<Endpoint> servers;
};

OnlineService::OnlineService(ServiceConfig config)
    : config_(std::move(config)), rendezvous_([this](const std::string& line) { Log(line); }) {
  if (config_.game_socket) config_.game_socket->SetTap(&rendezvous_);
}

OnlineService::~OnlineService() {
  if (config_.game_socket) {
    config_.game_socket->SetTap(nullptr);
    config_.game_socket->CloseRelay();
  }
}

void OnlineService::Apply(const UserSettings& settings) {
  std::lock_guard<std::mutex> lock(mutex_);
  const bool was_enabled = settings_.enabled;
  const std::string before = settings_.name;
  settings_ = settings;
  settings_.name = CleanOnlineName(settings.name);
  if (settings_.name != before) name_since_ = now_;
  if (was_enabled && !settings.enabled) Disable();
}

void OnlineService::Disable() {
  if (Connected() && channel_) {
    if (room_) SendLocked(EncodeMatchLeave(next_seq_++, room_->room));
    leaving_ = std::move(channel_);
    leaving_deadline_ = now_ + kLeaveFlushSeconds;
  }
  if (channel_) channel_->Close();
  channel_.reset();
  step_ = Step::kIdle;
  pending_.clear();
  ClearRoom(false);
  held_room_.clear();
  friends_.clear();
  incoming_.clear();
  outgoing_.clear();
  invites_.clear();
  rooms_.clear();
  friend_code_.clear();
  fetched_ = false;
  connection_ = Connection::kOff;
  connection_text_.clear();
  notice_.clear();
  notices_.clear();
  test_.reset();
  testing_ = false;
  test_probe_.reset();
  retry_at_ = 0;
  retry_delay_ = 1.0;
}

void OnlineService::Update(double now) {
  std::lock_guard<std::mutex> lock(mutex_);
  now_ = now;
  if (leaving_) {
    ChannelEvent event;
    bool done = now >= leaving_deadline_;
    while (!done && leaving_->Poll(event)) done = event.kind != ChannelEvent::Kind::kOpen;
    if (done) {
      leaving_->Close();
      leaving_.reset();
    }
  }
  if (!settings_.enabled) return;
  CollectTest();
  if (config_.url.empty()) {
    connection_ = Connection::kError;
    connection_text_ = Tr("Unable to connect");
    return;
  }
  if (step_ == Step::kIdle && !channel_ && now >= retry_at_) OpenChannel(now);
  PumpChannel(now);
  PumpPaths(now);
  const bool opening = step_ == Step::kOpening || step_ == Step::kGreeting;
  if (opening && now > step_deadline_) OnLinkLost("timeout", now);
  if (step_ != Step::kOnline) return;
  if (now >= next_refresh_) Refresh();
  if (SessionName() != greeted_name_ && !room_ && now - name_since_ >= kNameSettleSeconds) {
    Log("online name changed, signing in again");
    if (channel_) channel_->Close();
    channel_.reset();
    step_ = Step::kIdle;
    pending_.clear();
    OpenChannel(now);
  }
}

void OnlineService::OpenChannel(double now) {
  if (!EnsureIdentity()) {
    connection_ = Connection::kError;
    connection_text_ = Tr("Online identity is unavailable.");
    retry_at_ = now + kFatalRetrySeconds;
    return;
  }
  channel_ = config_.open_channel(config_.url);
  step_ = Step::kOpening;
  step_deadline_ = now + kStepTimeoutSeconds;
  if (connection_ != Connection::kChecking) {
    connection_ = Connection::kChecking;
    connection_text_ = Tr("Connecting");
  }
}

void OnlineService::PumpChannel(double now) {
  ChannelEvent event;
  while (channel_ && channel_->Poll(event)) OnEvent(event, now);
}

void OnlineService::OnEvent(const ChannelEvent& event, double now) {
  switch (event.kind) {
    case ChannelEvent::Kind::kOpen:
      Log("online service connected");
      step_ = Step::kGreeting;
      step_deadline_ = now + kStepTimeoutSeconds;
      Send(Purpose::kHello, [this](int seq) {
        HelloFields hello;
        hello.image = config_.image;
        hello.build = config_.build;
        hello.player = identity_.player_id;
        hello.token = identity_.token;
        hello.name = SessionName();
        hello.version = config_.version;
        hello.platform = config_.platform;
        hello.presence = true;
        hello.relay = config_.relay && config_.game_socket;
        return EncodeHello(seq, hello);
      });
      break;
    case ChannelEvent::Kind::kText:
      OnText(event.text, now);
      break;
    case ChannelEvent::Kind::kBinary:
      if (config_.game_socket) {
        config_.game_socket->DeliverRelay(reinterpret_cast<const uint8_t*>(event.text.data()), event.text.size());
      }
      break;
    case ChannelEvent::Kind::kClosed:
      OnLinkLost(event.text.empty() ? "closed" : event.text, now);
      break;
  }
}

void OnlineService::OnText(const std::string& text, double now) {
  const std::optional<ServerMessage> message = DecodeServerMessage(text);
  if (!message) {
    Log("online error malformed server message");
    return;
  }
  if (message->kind == ServerMessage::Kind::kResponse) OnResponse(*message, now);
  else OnServerEvent(*message, now);
}

void OnlineService::OnResponse(const ServerMessage& message, double now) {
  const auto found = pending_.find(message.seq);
  if (found == pending_.end()) return;
  const Pending pending = found->second;
  pending_.erase(found);
  const bool ok = message.code == "ok";

  switch (pending.purpose) {
    case Purpose::kHello:
      if (!ok) {
        Log("online sign-in refused code=" + message.code);
        connection_ = Fatal(message.code) ? Connection::kError : Connection::kOffline;
        connection_text_ = DescribeServiceError(message.code);
        if (channel_) channel_->Close();
        channel_.reset();
        step_ = Step::kIdle;
        retry_at_ = now + (Fatal(message.code) ? kFatalRetrySeconds : retry_delay_);
        return;
      }
      OnGreeted(message, now);
      break;
    case Purpose::kFriends:
      if (ok && message.friends) {
        friends_ = *message.friends;
        fetched_ = true;
      }
      break;
    case Purpose::kRequests:
      if (ok) {
        if (message.incoming) incoming_ = *message.incoming;
        if (message.outgoing) outgoing_ = *message.outgoing;
      }
      break;
    case Purpose::kAdd:
      if (!ok) {
        Notice(DescribeServiceError(message.code));
      } else {
        Notice(message.state == "friends" ? Tr("You are now friends.") : Tr("Friend request sent."));
        Refresh();
      }
      break;
    case Purpose::kAccept:
    case Purpose::kDecline:
    case Purpose::kRemove:
      if (!ok) Notice(DescribeServiceError(message.code));
      Refresh();
      break;
    case Purpose::kRoomHost:
    case Purpose::kRoomJoin:
    case Purpose::kInviteAccept:
      if (!ok || !message.room) {
        Notice(DescribeServiceError(ok ? "internal" : message.code));
        Log("online room request failed code=" + message.code);
        break;
      }
      OnSeated(message);
      break;
    case Purpose::kResume:
      if (!ok || !message.room) {
        Log("online resume refused code=" + message.code);
        held_room_.clear();
        OnRoomClosed("disconnect");
        break;
      }
      held_room_.clear();
      OnSeated(message);
      Log("online resumed seat=" + std::to_string(seat_));
      break;
    case Purpose::kRoomUpdate:
    case Purpose::kRoomStart:
      if (!ok) Notice(DescribeServiceError(message.code));
      else if (message.room) OnRoomView(*message.room);
      break;
    case Purpose::kRoomSearch:
      if (ok && message.rooms) {
        rooms_ = *message.rooms;
        ++search_generation_;
      }
      break;
    case Purpose::kInviteSend:
      if (!ok) Notice(DescribeServiceError(message.code));
      break;
    case Purpose::kRoomLeave:
    case Purpose::kInviteDecline:
    case Purpose::kEndpoint:
    case Purpose::kReady:
      if (!ok) Log("online request failed code=" + message.code);
      break;
    case Purpose::kTransport:
      if (!ok) Log("online transport report refused code=" + message.code);
      break;
  }
}

void OnlineService::OnGreeted(const ServerMessage& message, double now) {
  const bool was_reconnecting = connection_ == Connection::kChecking && !friend_code_.empty();
  step_ = Step::kOnline;
  connection_ = Connection::kOnline;
  connection_text_ = Tr("Online");
  retry_delay_ = 1.0;
  friend_code_ = message.friend_code.empty() ? identity_.player_id : message.friend_code;
  greeted_name_ = SessionName();
  capabilities_ = message.capabilities.value_or(ServiceCapabilities{});
  Log(std::string("online signed in") + (was_reconnecting ? " (reconnected)" : "") + " name=" + greeted_name_ +
      " relay_supported=" + (capabilities_.relay_supported ? "true" : "false") +
      " seat_routing=" + (capabilities_.seat_routing ? "true" : "false") +
      " worker_build=" + (capabilities_.worker_build.empty() ? std::string("-") : capabilities_.worker_build));
  Refresh();
  if (held_room_.empty() && !message.resume.empty()) held_room_ = message.resume;
  if (!held_room_.empty()) {
    const std::string id = held_room_;
    Send(Purpose::kResume, [id](int seq) { return EncodeMatchResume(seq, id); });
  }
  (void)now;
}

void OnlineService::OnServerEvent(const ServerMessage& message, double now) {
  using Kind = ServerMessage::Kind;
  (void)now;
  switch (message.kind) {
    case Kind::kResponse:
      break;
    case Kind::kFriendRequest:
      Notice(message.request.name + Tr(" wants to be your friend."));
      Refresh();
      break;
    case Kind::kFriendAdded:
      Notice(message.friend_view.name + Tr(" is now your friend."));
      UpdateFriend(message.friend_view);
      Refresh();
      break;
    case Kind::kFriendRemoved:
      friends_.erase(std::remove_if(friends_.begin(), friends_.end(),
                                    [&](const Friend& f) { return f.player == message.player; }),
                     friends_.end());
      Refresh();
      break;
    case Kind::kFriendStatus:
      UpdateFriend(message.friend_view);
      break;
    case Kind::kFriendOffline:
      for (Friend& f : friends_) {
        if (f.player == message.player) {
          f.online = false;
          f.state = FriendState::kOffline;
        }
      }
      break;
    case Kind::kRoomCreated:
      if (!room_ || room_->room != message.room->room) OnSeated(message);
      break;
    case Kind::kRoomUpdated:
      if (room_ && room_->room == message.room->room) OnRoomView(*message.room);
      break;
    case Kind::kRoomClosed:
      if (room_ && room_->room == message.room_id) OnRoomClosed(message.reason);
      break;
    case Kind::kInviteReceived:
      invites_.erase(std::remove_if(invites_.begin(), invites_.end(),
                                    [&](const Invite& i) { return i.id == message.invite->id; }),
                     invites_.end());
      invites_.push_back(*message.invite);
      Notice(message.invite->host_name + Tr(" invited you to a room."));
      break;
    case Kind::kInviteEnded:
      invites_.erase(std::remove_if(invites_.begin(), invites_.end(),
                                    [&](const Invite& i) { return i.id == message.invite_id; }),
                     invites_.end());
      break;
    case Kind::kMatchPeer:
      OnPeer(message.peer);
      break;
    case Kind::kMatchTransport:
      if (room_ && room_->room == message.room_id && message.transport_seat == 0) {
        peers_.HostAnnounced(message.transport);
        Log(std::string("host transport=") + PathKindName(message.transport));
      }
      break;
    case Kind::kRelayClosed:
      if (room_ && room_->room == message.room_id) {
        Log("relay closed reason=" + message.reason);
        peers_.FailRelayPaths();
        if (config_.game_socket) config_.game_socket->CloseRelay();
      }
      break;
  }
}

void OnlineService::OnSeated(const ServerMessage& message) {
  room_ =*message.room;
  seat_ = message.seat;
  relay_token_ = message.relay_token;
  closed_reason_.clear();
  const bool relay = relay_token_ != 0 && config_.relay && config_.game_socket && capabilities_.relay_supported;
  const bool resumed = peers_.active() && peers_.room() == room_->room && peers_.my_seat() == seat_;
  if (resumed) {
    peers_.SetRelayCoordinated(relay);
  } else {
    peers_.Begin(room_->room, seat_, relay);
    rtt_.clear();
    race_started_ = false;
    establishment_.Reset();
    punching_.fill(false);
    if (config_.game_socket) {
      config_.game_socket->Use([&](Transport&) { rendezvous_.BeginRoom(PunchKey(room_->room), seat_); });
    }
  }
  if (config_.game_socket) {
    config_.game_socket->OpenRelay(relay_token_, [ch = channel_](std::vector<uint8_t>&& frame) {
      return ch && ch->SendBinary(std::move(frame));
    });
  }
  endpoint_sent_ = false;
  OnRoomView(*room_);
  Log("online seated seat=" + std::to_string(seat_) + " capacity=" + std::to_string(room_->capacity) +
      " relay=" + (relay ? "true" : "false"));
}

void OnlineService::OnRoomView(const RoomView& room) {
  if (!room_) return;
  const RoomView before = *room_;
  room_ = room;
  peers_.UpdateSeats(room.seats);
  for (const Seat& seat : room.seats) {
    if (seat.seat == seat_ || seat.seat >= int(before.seats.size())) continue;
    const Seat& was = before.seats[size_t(seat.seat)];
    if (was.state != SeatState::kEmpty && seat.state == SeatState::kEmpty) {
      Notice(Tr("Player left") + std::string(": ") + was.name);
      if (config_.game_socket) {
        config_.game_socket->SelectRelay(seat.seat, false);
        config_.game_socket->Use([&](Transport&) { rendezvous_.StopSeat(seat.seat); });
      }
      punching_[size_t(seat.seat)] = false;
      rtt_.erase(seat.seat);
    } else if (was.state == SeatState::kEmpty && seat.state != SeatState::kEmpty) {
      Notice(seat.name + Tr(" joined."));
    }
  }
  if (room.state != RoomState::kOpen && !race_started_) {
    race_started_ = true;
    establishment_.Begin(now_);
  }
}

void OnlineService::OnRoomClosed(const std::string& reason) {
  std::string text;
  if (reason == "host_left" || reason == "cancelled") text = Tr("The host closed the room.");
  else if (reason == "expired") text = Tr("The room expired.");
  else text = Tr("Connection lost");
  Notice(text);
  Log("online room closed reason=" + reason);
  ClearRoom(true);
  closed_reason_ = reason;
}

void OnlineService::ClearRoom(bool keep_reason) {
  if (!keep_reason) closed_reason_.clear();
  if (config_.game_socket) {
    config_.game_socket->CloseRelay();
    config_.game_socket->Use([&](Transport&) { rendezvous_.Stop(); });
  }
  room_.reset();
  seat_ = -1;
  relay_token_ = 0;
  peers_.Clear();
  rtt_.clear();
  punching_.fill(false);
  endpoint_sent_ = false;
  race_started_ = false;
  establishment_.Reset();
  udp_ = Udp::kNone;
  stun_started_ = false;
  mapped_ = {};
}

void OnlineService::OnPeer(const PeerInfo& peer) {
  if (!room_ || peer.room != room_->room) return;
  peers_.SetPeer(peer);
  const PeerState* state = peers_.Find(peer.seat);
  if (state && state->path == PeerPath::kPending && punching_[size_t(peer.seat)]) {
    if (config_.game_socket) config_.game_socket->Use([&](Transport&) { rendezvous_.StopSeat(peer.seat); });
    punching_[size_t(peer.seat)] = false;
  }
  Log("online peer seat=" + std::to_string(peer.seat) + " same_network=" + (peer.same_network ? "true" : "false") +
      " relay=" + (peer.relay ? "true" : "false") + " candidates=" + std::to_string(peer.candidates.size()));
}

void OnlineService::UpdateFriend(const Friend& entry) {
  for (Friend& f : friends_) {
    if (f.player == entry.player) {
      f = entry;
      return;
    }
  }
  friends_.push_back(entry);
}

void OnlineService::Refresh() {
  if (!Connected()) return;
  next_refresh_ = now_ + kRefreshSeconds;
  Send(Purpose::kFriends, [](int seq) { return EncodeFriendsList(seq); });
  Send(Purpose::kRequests, [](int seq) { return EncodeFriendsRequests(seq); });
}

void OnlineService::OnLinkLost(const std::string& why, double now) {
  Log("online service disconnected reason=" + why);
  if (channel_) channel_->Close();
  channel_.reset();
  pending_.clear();
  const bool was_online = step_ == Step::kOnline;
  step_ = Step::kIdle;
  invites_.clear();
  for (Friend& f : friends_) {
    f.online = false;
    f.state = FriendState::kOffline;
  }
  if (room_) {
    // The service holds the seat for a while: the table keeps every direct path, and the relay is closed until the
    // seat is resumed.
    held_room_ = room_->room;
    if (config_.game_socket) config_.game_socket->CloseRelay();
    peers_.PauseRelay();
    endpoint_sent_ = false;
  }
  if (was_online) Notice(Tr("Connection lost"));
  const bool known = !friend_code_.empty();
  connection_ = known ? Connection::kChecking : Connection::kOffline;
  connection_text_ = known ? Tr("Reconnecting") : DescribeServiceError("unreachable");
  retry_at_ = now + retry_delay_;
  retry_delay_ = std::min(retry_delay_ * 2, kMaxRetrySeconds);
}

FriendRequestResult OnlineService::AddFriend(const std::string& text) {
  std::lock_guard<std::mutex> lock(mutex_);
  const std::string code = NormalizeFriendCode(text);
  const auto refuse = [&](FriendRequestResult result, const std::string& notice) {
    Notice(notice);
    return result;
  };
  if (!Connected()) return refuse(FriendRequestResult::kNotConnected, Tr("Not connected to the online service."));
  if (code.empty()) return refuse(FriendRequestResult::kInvalid, DescribeServiceError("bad_request"));
  if (code == friend_code_) return refuse(FriendRequestResult::kOwnCode, Tr("That is your own friend code."));
  const auto is_code = [&code](const auto& entry) { return entry.player == code; };
  if (std::any_of(friends_.begin(), friends_.end(), is_code)) {
    return refuse(FriendRequestResult::kAlreadyFriends, Tr("Already friends."));
  }
  if (std::any_of(outgoing_.begin(), outgoing_.end(), is_code)) {
    return refuse(FriendRequestResult::kAlreadyPending, Tr("Friend request already pending."));
  }
  Send(Purpose::kAdd, [&code](int seq) { return EncodeFriendsAdd(seq, code); }, code);
  return FriendRequestResult::kSent;
}

void OnlineService::AcceptFriend(const std::string& player) {
  std::lock_guard<std::mutex> lock(mutex_);
  incoming_.erase(std::remove_if(incoming_.begin(), incoming_.end(), [&](const Request& r) { return r.player == player; }),
                  incoming_.end());
  Send(Purpose::kAccept, [&player](int seq) { return EncodeFriendsAccept(seq, player); });
}

void OnlineService::DeclineFriend(const std::string& player) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto drop = [&](std::vector<Request>& list) {
    list.erase(std::remove_if(list.begin(), list.end(), [&](const Request& r) { return r.player == player; }), list.end());
  };
  drop(incoming_);
  drop(outgoing_);
  Send(Purpose::kDecline, [&player](int seq) { return EncodeFriendsDecline(seq, player); });
}

void OnlineService::RemoveFriend(const std::string& player) {
  std::lock_guard<std::mutex> lock(mutex_);
  friends_.erase(std::remove_if(friends_.begin(), friends_.end(), [&](const Friend& f) { return f.player == player; }),
                 friends_.end());
  Send(Purpose::kRemove, [&player](int seq) { return EncodeFriendsRemove(seq, player); });
}

bool OnlineService::CreateRoom(int capacity, const RoomRules& rules) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!Connected() || InRoom() || capacity < kMinSeats || capacity > kMaxSeats) return false;
  Send(Purpose::kRoomHost, [&](int seq) { return EncodeRoomHost(seq, capacity, rules); });
  return true;
}

bool OnlineService::UpdateRoom(int capacity, const RoomRules* rules) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!Connected() || !IsHost() || room_->state != RoomState::kOpen) return false;
  const std::string id = room_->room;
  Send(Purpose::kRoomUpdate, [&](int seq) { return EncodeRoomUpdate(seq, id, capacity, rules); });
  return true;
}

void OnlineService::SearchRooms() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!Connected() || InRoom()) return;
  Send(Purpose::kRoomSearch, [](int seq) { return EncodeRoomSearch(seq); });
}

bool OnlineService::JoinRoom(const std::string& handle) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!Connected() || InRoom()) return false;
  Send(Purpose::kRoomJoin, [&](int seq) { return EncodeRoomJoin(seq, handle); });
  return true;
}

bool OnlineService::StartRace() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!Connected() || !IsHost() || room_->state != RoomState::kOpen) return false;
  const std::string id = room_->room;
  Send(Purpose::kRoomStart, [&](int seq) { return EncodeRoomStart(seq, id); });
  return true;
}

bool OnlineService::SetReady(bool ready) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!Connected() || !InRoom()) return false;
  const std::string id = room_->room;
  Send(Purpose::kReady, [&](int seq) { return EncodeMatchReady(seq, id, ready); });
  return true;
}

void OnlineService::LeaveRoom() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!InRoom()) return;
  const std::string id = room_->room;
  Send(Purpose::kRoomLeave, [&](int seq) { return EncodeRoomLeave(seq, id); });
  held_room_.clear();
  ClearRoom(false);
}

void OnlineService::CancelRoom() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!IsHost()) return;
  const std::string id = room_->room;
  Send(Purpose::kRoomLeave, [&](int seq) { return EncodeRoomCancel(seq, id); });
  held_room_.clear();
  ClearRoom(false);
}

bool OnlineService::SendInvite(const std::string& player) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!Connected() || !IsHost() || room_->state != RoomState::kOpen) return false;
  Send(Purpose::kInviteSend, [&](int seq) { return EncodeInviteSend(seq, player); }, player);
  return true;
}

bool OnlineService::AcceptInvite(const std::string& invite) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!Connected() || InRoom()) return false;
  invites_.erase(std::remove_if(invites_.begin(), invites_.end(), [&](const Invite& i) { return i.id == invite; }),
                 invites_.end());
  Send(Purpose::kInviteAccept, [&](int seq) { return EncodeInviteAccept(seq, invite); });
  return true;
}

void OnlineService::DeclineInvite(const std::string& invite) {
  std::lock_guard<std::mutex> lock(mutex_);
  invites_.erase(std::remove_if(invites_.begin(), invites_.end(), [&](const Invite& i) { return i.id == invite; }),
                 invites_.end());
  Send(Purpose::kInviteDecline, [&](int seq) { return EncodeInviteDecline(seq, invite); });
}

bool OnlineService::SignedIn() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return step_ == Step::kOnline;
}

ServiceCapabilities OnlineService::Capabilities() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return capabilities_;
}

OnlineProfile OnlineService::Profile() const {
  std::lock_guard<std::mutex> lock(mutex_);
  OnlineProfile profile;
  profile.signed_in = step_ == Step::kOnline;
  if (profile.signed_in) {
    profile.friend_code = friend_code_;
    profile.name = greeted_name_;
  }
  return profile;
}

void OnlineService::Send(Purpose purpose, const std::function<std::string(int)>& encode, const std::string& player) {
  if (!channel_ || step_ == Step::kIdle || step_ == Step::kOpening) return;
  const int seq = next_seq_++;
  pending_[seq] = {purpose, player, now_};
  channel_->Send(encode(seq));
}

void OnlineService::SendLocked(const std::string& text) {
  if (channel_) channel_->Send(text);
}

void OnlineService::TestConnection() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!settings_.enabled || testing_) return;
  test_.reset();
  EnsureIdentity();
  testing_ = true;
  auto state = std::make_shared<TestState>();
  test_probe_ = state;
  ConnectionTestRequest request;
  request.url = config_.url;
  request.identity = identity_;
  request.name = SessionName();
  request.image = config_.image;
  request.build = config_.build;
  request.stun = config_.stun;
  request.upnp = config_.upnp;
  request.game_ports = config_.game_ports;
  auto work = [state, run = config_.test, request = std::move(request)] {
    std::optional<ConnectionReport> report;
    std::string stopped;
    try {
      report = run(request);
    } catch (const std::exception& e) {
      stopped = e.what();
    } catch (...) {
      stopped = "unknown error";
    }
    std::lock_guard<std::mutex> lock(state->mutex);
    state->report = std::move(report);
    state->stopped = std::move(stopped);
    state->done = true;
  };
  if (config_.synchronous_probes) work();
  else std::thread(work).detach();
}

void OnlineService::CollectTest() {
  if (!test_probe_) return;
  std::optional<ConnectionReport> finished;
  std::string stopped;
  {
    std::lock_guard<std::mutex> lock(test_probe_->mutex);
    if (!test_probe_->done) return;
    finished = test_probe_->report;
    stopped = test_probe_->stopped;
  }
  test_probe_.reset();
  testing_ = false;
  if (!finished) {
    Log("online connection test stopped without a report: " + stopped);
    return;
  }
  test_ = finished;
  ++tests_finished_;
  for (const std::string& detail : finished->details) Log("online connection test " + detail);
  Log("online connection test " + finished->verdict);
}

void OnlineService::SetPeerRtt(int seat, std::optional<int> rtt_ms) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (rtt_ms) rtt_[seat] = *rtt_ms;
  else rtt_.erase(seat);
}

std::vector<GamePeer> OnlineService::GamePeers() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<GamePeer> out;
  for (const PeerState& peer : peers_.peers()) {
    if (!peer.present || (peer.path != PeerPath::kDirect && peer.path != PeerPath::kRelay)) continue;
    const bool relay = peer.path == PeerPath::kRelay;
    out.push_back({peer.seat, peer.name, relay ? RelayPeerEndpoint(peer.seat) : peer.direct, relay});
  }
  return out;
}

int OnlineService::SeatOfEndpoint(const Endpoint& from) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (const int seat = RelaySeatOf(from); seat >= 0) return seat;
  return peers_.SeatOf(from);
}

// -- gameplay paths ---------------------------------------------------------------------------------------------

void OnlineService::BeginStun() {
  udp_ = Udp::kDiscovering;
  stun_started_ = false;
  mapped_ = {};
  if (stun_servers_ || stun_lookup_) return;
  auto lookup = std::make_shared<StunLookup>();
  stun_lookup_ = lookup;
  const auto resolve = config_.resolve_stun ? config_.resolve_stun : &ResolveStunServers;
  auto work = [lookup, resolve, stun = config_.stun] {
    std::vector<Endpoint> servers = resolve(stun);
    std::lock_guard<std::mutex> lock(lookup->mutex);
    lookup->servers = std::move(servers);
    lookup->done = true;
  };
  if (config_.synchronous_probes) work();
  else std::thread(work).detach();
}

std::optional<std::vector<Endpoint>> OnlineService::StunServers() {
  if (stun_servers_) return stun_servers_;
  if (!stun_lookup_) return std::vector<Endpoint>();
  std::vector<Endpoint> servers;
  {
    std::lock_guard<std::mutex> lock(stun_lookup_->mutex);
    if (!stun_lookup_->done) return std::nullopt;
    servers = stun_lookup_->servers;
  }
  stun_lookup_.reset();
  // Kept for later rooms only once found: a lookup that found nothing (DNS not ready yet) is tried again.
  if (!servers.empty()) stun_servers_ = servers;
  return servers;
}

void OnlineService::SendEndpoint() {
  if (!room_) return;
  EndpointFields fields;
  fields.port = config_.game_socket->bound_port();
  fields.lan = config_.lan;
  // The service refuses a mapping that is not a public address.
  const bool mapped = mapped_.port && net::Classify(mapped_.address) == net::AddressKind::kPublic;
  if (mapped) {
    fields.public_ip = net::IpText(mapped_.address);
    fields.public_port = mapped_.port;
    fields.candidates.push_back({fields.public_ip, fields.public_port, "srflx"});
  }
  for (const std::string& ip : config_.lan) fields.candidates.push_back({ip, fields.port, "host"});
  if (fields.candidates.size() > 6) fields.candidates.resize(6);
  fields.relay = relay_token_ != 0;
  const std::string id = room_->room;
  Log("online endpoint advertised public=" + std::string(mapped ? "true" : "false") +
      " port=" + std::to_string(fields.port));
  Send(Purpose::kEndpoint, [&](int seq) { return EncodeMatchEndpoint(seq, id, fields); });
  endpoint_sent_ = true;
}

void OnlineService::StartPunchFor(const PeerState& peer, double now) {
  const std::vector<Candidate> candidates = PeerCandidates(peer.info, config_.lan, mapped_);
  peers_.BeginProbe(peer.seat, now);
  peers_.PunchStarted(peer.seat, now);
  punching_[size_t(peer.seat)] = true;
  const int seat = peer.seat;
  config_.game_socket->Use([&](Transport& out) { rendezvous_.StartPunch(seat, candidates, net::NowMs(), out); });
}

void OnlineService::ApplyChoices(const std::vector<PathChoice>& choices) {
  for (const PathChoice& choice : choices) {
    Log(std::string("online path seat=") + std::to_string(choice.seat) + " path=" + PeerPathName(choice.path));
    if (config_.game_socket) config_.game_socket->SelectRelay(choice.seat, choice.path == PeerPath::kRelay);
    if (choice.announce && room_) {
      const std::string id = room_->room;
      const int seat = choice.seat;
      const PathKind kind = choice.path == PeerPath::kRelay ? PathKind::kRelay : PathKind::kDirect;
      Send(Purpose::kTransport, [&](int seq) { return EncodeMatchTransport(seq, id, seat, kind); });
    }
  }
}

void OnlineService::FailPaths(const std::string& reason) {
  Log("online paths failed reason=" + reason);
  Notice(Tr("Unable to connect"));
}

void OnlineService::PumpPaths(double now) {
  if (!room_ || !config_.game_socket) return;
  OnlineGameSocket& socket = *config_.game_socket;
  if (!socket.is_open()) return;
  socket.Pump();
  const uint64_t now_ms = net::NowMs();

  if (udp_ == Udp::kNone && Connected()) BeginStun();
  if (udp_ == Udp::kDiscovering) {
    if (!stun_started_) {
      if (const std::optional<std::vector<Endpoint>> servers = StunServers()) {
        stun_started_ = true;
        const uint16_t port = socket.bound_port();
        socket.Use([&](Transport& out) { rendezvous_.StartStun(*servers, port, now_ms, out); });
      }
    }
    if (stun_started_ && rendezvous_.stun_stage() != Rendezvous::Stage::kRunning) {
      mapped_ = rendezvous_.mapped();
      udp_ = Udp::kReady;
    }
  }
  if (udp_ == Udp::kReady && Connected() && !endpoint_sent_) SendEndpoint();

  if (udp_ == Udp::kReady) {
    for (const PeerState& peer : peers_.peers()) {
      if (peer.present && peer.endpoint_known && peer.path == PeerPath::kPending && !punching_[size_t(peer.seat)]) {
        StartPunchFor(peer, now);
      }
    }
  }
  socket.Use([&](Transport& out) { rendezvous_.Tick(now_ms, out); });
  for (const PeerState& peer : peers_.peers()) {
    if (!punching_[size_t(peer.seat)]) continue;
    switch (rendezvous_.punch_stage(peer.seat)) {
      case Rendezvous::Stage::kDone: peers_.DirectProven(peer.seat, rendezvous_.selected(peer.seat).endpoint, now); break;
      case Rendezvous::Stage::kFailed: peers_.DirectFailed(peer.seat); break;
      default: break;
    }
  }
  ApplyChoices(peers_.Decide(now));

  if (const std::string failure = socket.relay_failure(); !failure.empty() && relay_token_) {
    Log("online relay failed reason=" + failure);
    peers_.FailRelayPaths();
    socket.CloseRelay();
  }
  if (establishment_.phase() == Establishment::Phase::kConnecting) {
    if (peers_.AllEstablished()) {
      establishment_.Select(now);
    } else if (peers_.AnyFailed()) {
      if (establishment_.Fail(now, "path_failed")) FailPaths("no_path");
    } else if (establishment_.ConnectingExpired(now)) {
      if (establishment_.Fail(now, "timeout")) FailPaths("timeout");
    }
  }
}

// -- shared -----------------------------------------------------------------------------------------------------

bool OnlineService::EnsureIdentity() {
  if (!identity_loaded_) {
    identity_loaded_ = true;
    identity_ = config_.identity ? config_.identity() : Identity{};
    if (!identity_.ok()) Log("online identity unavailable " + identity_.error);
  }
  return identity_.ok();
}

std::string OnlineService::SessionName() const {
  std::string name = CleanOnlineName(settings_.name);
  if (!identity_.ephemeral) return name;
  return name.substr(0, kMaxOnlineNameChars - 2) + "-2";
}

void OnlineService::Notice(const std::string& text) {
  notices_.push_back(text);
  notice_ = text;
}

void OnlineService::Log(const std::string& line) {
  if (config_.log) config_.log(line);
}

Snapshot OnlineService::snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  Snapshot s;
  s.connection = connection_;
  s.connection_text = connection_text_;
  s.friend_code = friend_code_;
  s.fetched = fetched_;
  s.friends = friends_;
  s.incoming = incoming_;
  s.outgoing = outgoing_;
  s.invites = invites_;
  s.rooms = rooms_;
  s.search_generation = search_generation_;
  s.room = room_;
  s.seat = seat_;
  s.reconnecting = room_ && step_ != Step::kOnline;
  for (const PeerState& peer : peers_.peers()) {
    PeerSnapshot entry;
    entry.seat = peer.seat;
    entry.name = peer.name;
    entry.connected = peer.present && !peer.lost;
    entry.path = peer.path;
    if (const auto it = rtt_.find(peer.seat); it != rtt_.end()) entry.rtt_ms = it->second;
    s.peers.push_back(std::move(entry));
  }
  s.paths_ready = peers_.AllEstablished();
  s.closed_reason = closed_reason_;
  s.notice = notice_;
  s.testing = testing_;
  s.test = test_;
  s.tests_finished = tests_finished_;
  return s;
}

std::vector<std::string> OnlineService::TakeNotices() {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> out;
  out.swap(notices_);
  return out;
}

}  // namespace eck::online
