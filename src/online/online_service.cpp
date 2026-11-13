#include "online/online_service.h"

#include <algorithm>
#include <cstdlib>
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
constexpr double kHostRefreshSeconds = 60.0;
constexpr double kHostRetrySeconds = 5.0;
constexpr double kJoinSeconds = 10.0;
constexpr double kInviteSeconds = 60.0;
constexpr double kInviteSendSeconds = 15.0;
// The service holds a seat this long; a session that needs the service does not outlive it.
constexpr double kHoldSeconds = 30.0;
// A NAT mapping found earlier may be gone by the time a later seat joins an idle session.
constexpr double kStunStaleSeconds = 20.0;
constexpr double kKeepAliveSeconds = 5.0;
// This side lost the service. The service's own "disconnect" means seat 0 stopped coming back.
constexpr char kConnectionLostReason[] = "connection_lost";

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
  sending_.reset();
  ClearRoom(false);
  held_room_.clear();
  friends_.clear();
  incoming_.clear();
  outgoing_.clear();
  invites_.clear();
  sending_.reset();
  hosting_.reset();
  accepted_invite_.reset();
  joining_ = false;
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
  if (room_ && step_ != Step::kOnline && now >= hold_deadline_ && !peers_.AllDirect()) {
    held_room_.clear();
    OnRoomClosed(kConnectionLostReason);
  }
  if (joining_ && now >= join_deadline_) {
    joining_ = false;
    DropAcceptedInvite("timeout");
    Notice(Tr("Unable to connect"));
  }
  if (sending_ && now >= sending_->deadline) FailInviteSend("timeout");
  invited_.erase(std::remove_if(invited_.begin(), invited_.end(), [&](const SentInvite& i) { return now >= i.expires; }),
                 invited_.end());
  if (step_ != Step::kOnline) return;
  if (now >= next_refresh_) Refresh();
  PumpHosting();
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
  // An answer about a room this player has since left says nothing about the one it is in now.
  if (RoomScoped(pending.purpose) && pending.room != (room_ ? room_->room : std::string())) return;
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
      if (!ok || !message.room) {
        next_host_refresh_ = now + kHostRetrySeconds;
        Notice(DescribeServiceError(ok ? "internal" : message.code));
        Log("online session host failed code=" + message.code);
        if (sending_) FailInviteSend("host_rejected_" + message.code);
        break;
      }
      if (!hosting_) {
        // Left before the service answered.
        const std::string id = message.room->room;
        Send(Purpose::kRoomLeave, [&id](int seq) { return EncodeRoomLeave(seq, id); });
      } else if (room_ && room_->room == message.room->room) {
        OnRoomView(*message.room);
      } else {
        OnSeated(message);
      }
      break;
    case Purpose::kInviteAccept:
      if (!accepted_invite_ || *accepted_invite_ != pending.player) {
        if (ok && message.room) {
          const std::string id = message.room->room;
          Send(Purpose::kRoomLeave, [&id](int seq) { return EncodeRoomLeave(seq, id); });
        }
        break;
      }
      joining_ = false;
      if (!ok || !message.room) {
        Notice(DescribeServiceError(ok ? "internal" : message.code));
        DropAcceptedInvite("join_rejected_" + message.code);
        break;
      }
      OnSeated(message);
      break;
    case Purpose::kResume:
      if (!ok || !message.room) {
        Log("online resume refused code=" + message.code);
        held_room_.clear();
        OnRoomClosed(kConnectionLostReason);
        break;
      }
      held_room_.clear();
      OnSeated(message);
      Log("online resumed seat=" + std::to_string(seat_));
      break;
    case Purpose::kRoomReopen:
      reopening_ = false;
      if (!ok) {
        Log("online room reopen refused code=" + message.code);
        LeaveLocked("reopen_refused");
      } else if (message.room) {
        OnRoomView(*message.room);
      }
      break;
    case Purpose::kRoomUpdate:
    case Purpose::kRoomStart:
      if (!ok) Notice(DescribeServiceError(message.code));
      else if (message.room) OnRoomView(*message.room);
      break;
    case Purpose::kInviteSend:
      if (!sending_ || sending_->player != pending.player) break;
      if (!ok) {
        Debug("invite failed friend=" + pending.player + " code=" + message.code);
        Notice(DescribeServiceError(message.code));
        FailInviteSend(message.code);
        break;
      }
      invited_.push_back({message.invite_id, sending_->player, sending_->name, now_ + kInviteSeconds});
      InviteLog("sent friend=" + sending_->player + " pending=" + std::to_string(invited_.size()));
      sending_.reset();
      break;
    case Purpose::kRoomLeave:
    case Purpose::kInviteDecline:
    case Purpose::kEndpoint:
    case Purpose::kReady:
    case Purpose::kPick:
      if (!ok) Log("online request failed code=" + message.code);
      break;
    case Purpose::kTransport:
      if (!ok) {
        Log("online transport report refused code=" + message.code);
        const int seat = std::atoi(pending.player.c_str());
        peers_.RelayRefused(seat);
        if (config_.game_socket) config_.game_socket->SelectRelay(seat, false);
      }
      break;
    case Purpose::kRace:
      if (!ok) Log("online race report refused code=" + message.code);
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
  if (!held_room_.empty()) {
    const std::string id = held_room_;
    Send(Purpose::kResume, [id](int seq) { return EncodeMatchResume(seq, id); });
  } else if (!message.resume.empty()) {
    // A seat from an earlier run of the game: nothing here knows it, and it would keep this player busy.
    const std::string id = message.resume;
    Send(Purpose::kRoomLeave, [&id](int seq) { return EncodeRoomLeave(seq, id); });
    SendHost();
  } else {
    SendHost();
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
      InviteRefused(message.player, false);
      break;
    case Kind::kRoomCreated:
      if (hosting_ && (!room_ || room_->room != message.room->room)) OnSeated(message);
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
      Notice(message.invite->host_name + Tr(" invited you to a race."));
      Debug("invite received from=" + message.invite->host);
      break;
    case Kind::kInviteEnded:
      invites_.erase(std::remove_if(invites_.begin(), invites_.end(),
                                    [&](const Invite& i) { return i.id == message.invite_id; }),
                     invites_.end());
      Debug("invite ended reason=" + message.reason);
      for (const SentInvite& sent : invited_) {
        if (sent.id != message.invite_id) continue;
        if (message.reason == "accepted" || message.reason == "expired") {
          const std::string id = sent.id;
          invited_.erase(std::remove_if(invited_.begin(), invited_.end(), [&](const SentInvite& i) { return i.id == id; }),
                         invited_.end());
        } else {
          const std::string player = sent.player;
          InviteRefused(player, message.reason == "declined");
        }
        break;
      }
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
  const bool resumed = peers_.active() && peers_.room() == message.room->room && peers_.my_seat() == message.seat;
  room_ = *message.room;
  seat_ = message.seat;
  relay_token_ = message.relay_token;
  closed_reason_.clear();
  if (seat_ != 0) hosting_.reset();
  joining_ = false;
  const bool accepted = accepted_invite_.has_value();
  if (accepted) {
    Log("invite transition consumed generation=" + std::to_string(accepted_generation_));
    accepted_invite_.reset();
  }
  const bool relay = relay_token_ != 0 && config_.relay && config_.game_socket && capabilities_.relay_supported;
  if (resumed) {
    peers_.SetRelayCoordinated(relay);
  } else {
    ++session_generation_;
    peers_.Begin(room_->room, seat_, relay);
    rtt_.clear();
    invited_.clear();
    establishment_.fill({});
    punching_.fill(false);
    ready_sent_ = false;
    pick_sent_.reset();
    udp_ = Udp::kNone;
    stun_started_ = false;
    mapped_ = {};
    if (config_.game_socket) {
      config_.game_socket->Use([&](Transport&) { rendezvous_.BeginRoom(PunchKey(room_->room), seat_); });
    }
  }
  if (config_.game_socket) {
    const std::weak_ptr<LobbyChannel> link = channel_;
    config_.game_socket->OpenRelay(relay_token_, [link](std::vector<uint8_t>&& frame) {
      const std::shared_ptr<LobbyChannel> channel = link.lock();
      return channel && channel->SendBinary(std::move(frame));
    });
  }
  endpoint_sent_ = false;
  OnRoomView(*room_);
  Log("online seated seat=" + std::to_string(seat_) + " capacity=" + std::to_string(room_->capacity) +
      " relay=" + (relay ? "true" : "false"));
  Debug("session joined seat=" + std::to_string(seat_));
  if (accepted) {
    InviteLog("accepted friend=" + accepted_host_ + " seat=" + std::to_string(seat_));
    if (replacing_role_) InviteLog("joined_inviter seat=" + std::to_string(seat_));
    replacing_role_ = false;
  }
  if (sending_ && seat_ == 0) {
    InviteLog("host_create_ready seat=0");
    DispatchInvite();
  }
}

void OnlineService::OnRoomView(const RoomView& room) {
  if (!room_ || room.room != room_->room) return;
  const RoomView before = *room_;
  room_ = room;
  peers_.UpdateSeats(room.seats);
  for (const Seat& seat : room.seats) {
    if (seat.seat >= int(before.seats.size())) continue;
    const Seat& was = before.seats[size_t(seat.seat)];
    if (seat.ready != was.ready) Debug("peer seat=" + std::to_string(seat.seat) + " ready=" + (seat.ready ? "yes" : "no"));
    if (seat.seat == seat_) continue;
    if (was.state != SeatState::kEmpty && seat.state == SeatState::kEmpty) {
      LifecycleLog("remote_leave seat=" + std::to_string(seat.seat));
      if (seat_ == 0) LifecycleLog("guest_left seat=" + std::to_string(seat.seat));
      Notice(Tr("Player left") + std::string(": ") + was.name);
      if (config_.game_socket) {
        config_.game_socket->SelectRelay(seat.seat, false);
        config_.game_socket->Use([&](Transport&) { rendezvous_.StopSeat(seat.seat); });
      }
      punching_[size_t(seat.seat)] = false;
      establishment_[size_t(seat.seat)].Reset();
      rtt_.erase(seat.seat);
    } else if (was.state == SeatState::kEmpty && seat.state != SeatState::kEmpty) {
      Notice(seat.name + Tr(" joined."));
      if (seat_ == 0) InviteLog("accepted friend=" + seat.player + " seat=" + std::to_string(seat.seat));
      invited_.erase(std::remove_if(invited_.begin(), invited_.end(),
                                    [&](const SentInvite& i) { return i.player == seat.player; }),
                     invited_.end());
    }
  }
  if (before.state != RoomState::kOpen && room.state == RoomState::kOpen) pick_sent_.reset();
  if (before.state == RoomState::kOpen && room.state != RoomState::kOpen) {
    ready_sent_ = false;
    pick_sent_.reset();
    invited_.clear();
  }
  if (room.players >= room.capacity) invited_.clear();
}

void OnlineService::OnRoomClosed(const std::string& reason) {
  std::string text;
  const bool host_gone = reason == "host_left" || reason == "cancelled" || reason == "disconnect";
  if (host_gone) text = Tr("The host left the race.");
  else if (reason == "expired") text = Tr("The race expired.");
  else text = Tr("Connection lost");
  Notice(text);
  Log("online room closed reason=" + reason);
  if (host_gone) LifecycleLog("host_left seat=0 reason=" + reason);
  hosting_.reset();
  ClearRoom(true);
  closed_reason_ = reason;
}

void OnlineService::ClearRoom(bool keep_reason) {
  if (sending_ && room_) FailInviteSend("room_closed");
  reopening_ = false;
  if (room_) {
    ++session_generation_;
    LifecycleLog("cleanup room=" + room_->room + " session_generation=" + std::to_string(session_generation_));
  }
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
  ready_sent_ = false;
  pick_sent_.reset();
  establishment_.fill({});
  invited_.clear();
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

void OnlineService::InviteRefused(const std::string& player, bool declined) {
  for (const SentInvite& sent : invited_) {
    if (sent.player != player) continue;
    invite_outcome_ = {invite_outcome_.generation + 1, sent.name, declined};
    break;
  }
  invited_.erase(std::remove_if(invited_.begin(), invited_.end(), [&](const SentInvite& i) { return i.player == player; }),
                 invited_.end());
}

void OnlineService::DropAcceptedInvite(const std::string& reason) {
  replacing_role_ = false;
  if (!accepted_invite_) return;
  Log("invite transition failed generation=" + std::to_string(accepted_generation_) + " reason=" + reason);
  accepted_invite_.reset();
}

void OnlineService::DispatchInvite() {
  const std::string player = sending_->player;
  Debug("invite send friend=" + player);
  Send(Purpose::kInviteSend, [&](int seq) { return EncodeInviteSend(seq, player); }, player);
}

void OnlineService::FailInviteSend(const std::string& reason) {
  InviteLog("send_failed friend=" + sending_->player + " reason=" + reason);
  invite_outcome_ = {invite_outcome_.generation + 1, sending_->name, false};
  sending_.reset();
  if (!room_) hosting_.reset();
}

// Everything of the old role goes before the join is even asked for, so the join starts from a player with no room.
void OnlineService::AbandonOwnSession() {
  sending_.reset();
  hosting_.reset();
  if (!room_) return;
  InviteLog("role_replace old_seat=" + std::to_string(seat_) + " new_role=guest");
  const std::string id = room_->room;
  Send(Purpose::kRoomLeave, [&](int seq) { return EncodeRoomLeave(seq, id); });
  held_room_.clear();
  ClearRoom(false);
  replacing_role_ = true;
  InviteLog("old_room_cleared");
}

bool OnlineService::RoomScoped(Purpose purpose) {
  switch (purpose) {
    case Purpose::kRoomUpdate:
    case Purpose::kRoomStart:
    case Purpose::kRoomReopen:
    case Purpose::kInviteSend:
    case Purpose::kEndpoint:
    case Purpose::kReady:
    case Purpose::kPick:
    case Purpose::kTransport: return true;
    default: return false;
  }
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
  joining_ = false;
  DropAcceptedInvite("service_disconnected");
  if (sending_) FailInviteSend("service_disconnected");
  for (Friend& f : friends_) {
    f.online = false;
    f.state = FriendState::kOffline;
  }
  if (room_) {
    // The service holds the seat for a while: the table keeps every direct path, and the relay is closed until the
    // seat is resumed.
    if (held_room_.empty()) hold_deadline_ = now + kHoldSeconds;
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

void OnlineService::HostSession(const RoomRules& rules) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (room_) return;
  const bool requested = hosting_.has_value();
  hosting_ = rules;
  Debug("session host requested");
  if (!requested) SendHost();
}

void OnlineService::SendHost() {
  if (!hosting_ || !Connected() || room_) return;
  next_host_refresh_ = now_ + kHostRefreshSeconds;
  const RoomRules rules = *hosting_;
  Send(Purpose::kRoomHost, [&rules](int seq) { return EncodeRoomHost(seq, kMaxSeats, rules); });
}

void OnlineService::PumpHosting() {
  if (!hosting_ || joining_ || now_ < next_host_refresh_) return;
  if (!room_) {
    SendHost();
  } else if (IsHost() && room_->state == RoomState::kOpen) {
    next_host_refresh_ = now_ + kHostRefreshSeconds;
    const RoomRules rules = *hosting_;
    Send(Purpose::kRoomHost, [&rules](int seq) { return EncodeRoomHost(seq, kMaxSeats, rules); });
  }
}

RaceTelemetryContext OnlineService::RaceContext() const {
  std::lock_guard<std::mutex> lock(mutex_);
  RaceTelemetryContext context;
  context.ready = Connected() && capabilities_.race_telemetry;
  if (room_ && room_->state != RoomState::kOpen) {
    context.room = room_->room;
    context.seat = seat_;
  }
  return context;
}

bool OnlineService::ReportRaceStart(const RaceStartFields& race) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!Connected() || !capabilities_.race_telemetry) return false;
  Send(Purpose::kRace, [&](int seq) { return EncodeRaceStart(seq, race); });
  return true;
}

bool OnlineService::ReportRaceComplete(const RaceCompleteFields& race) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!Connected() || !capabilities_.race_telemetry) return false;
  Send(Purpose::kRace, [&](int seq) { return EncodeRaceComplete(seq, race); });
  return true;
}

bool OnlineService::StartRace() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!Connected() || !IsHost() || room_->state != RoomState::kOpen || !peers_.AllEstablished()) return false;
  const std::string id = room_->room;
  Send(Purpose::kRoomStart, [&](int seq) { return EncodeRoomStart(seq, id); });
  return true;
}

bool OnlineService::PublishRace(const RoomRules& rules) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!Connected() || !IsHost() || room_->state != RoomState::kOpen || !peers_.AllEstablished()) return false;
  const std::string id = room_->room;
  hosting_ = rules;
  Send(Purpose::kRoomUpdate, [&](int seq) { return EncodeRoomUpdate(seq, id, rules); });
  Send(Purpose::kRoomStart, [&](int seq) { return EncodeRoomStart(seq, id); });
  return true;
}

bool OnlineService::ReopenRoom(const RoomRules& rules) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!Connected() || !IsHost() || room_->state != RoomState::kRacing || !capabilities_.room_reopen || reopening_) return false;
  reopening_ = true;
  hosting_ = rules;
  const std::string id = room_->room;
  Send(Purpose::kRoomReopen, [&](int seq) { return EncodeRoomReopen(seq, id, rules); });
  return true;
}

bool OnlineService::LeaveSession(const char* source) {
  std::lock_guard<std::mutex> lock(mutex_);
  return LeaveLocked(source);
}

bool OnlineService::LeaveLocked(const char* source) {
  hosting_.reset();
  if (!room_) return false;
  LifecycleLog("local_leave requested seat=" + std::to_string(seat_) + " source=" + source);
  const std::string id = room_->room;
  const bool told = Connected();
  Send(Purpose::kRoomLeave, [&](int seq) { return EncodeRoomLeave(seq, id); });
  held_room_.clear();
  ClearRoom(false);
  return told;
}

void OnlineService::AbandonSession(const std::string& reason) {
  std::lock_guard<std::mutex> lock(mutex_);
  LeaveLocked("watchdog");
  closed_reason_ = reason;
}

void OnlineService::SendPick(PickState pick, int character) {
  std::lock_guard<std::mutex> lock(mutex_);
  const std::pair<PickState, int> next(pick, character);
  if (!Connected() || !room_ || pick_sent_ == next) return;
  pick_sent_ = next;
  const std::string id = room_->room;
  Send(Purpose::kPick, [&](int seq) { return EncodeMatchPick(seq, id, pick, character); });
}

bool OnlineService::SendInvite(const std::string& player, const std::optional<RoomRules>& session_rules) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto known = [&](const SentInvite& sent) { return sent.player == player; };
  if (!Connected() || joining_ || sending_ || std::any_of(invited_.begin(), invited_.end(), known)) return false;
  const bool free_seat = room_ && IsHost() && room_->state == RoomState::kOpen &&
                         room_->players + int(invited_.size()) < room_->capacity;
  if (room_ ? !free_seat : !session_rules) return false;
  std::string name;
  for (const Friend& f : friends_) {
    if (f.player == player) name = f.name;
  }
  sending_ = InviteSend{player, name, now_ + kInviteSendSeconds};
  InviteLog("send_requested friend=" + player);
  if (room_) {
    DispatchInvite();
    return true;
  }
  InviteLog("host_create_begin");
  if (!hosting_) {
    hosting_ = *session_rules;
    Debug("session host requested");
    SendHost();
  }
  return true;
}

bool OnlineService::AcceptInvite(const std::string& invite) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = std::find_if(invites_.begin(), invites_.end(), [&](const Invite& i) { return i.id == invite; });
  if (found == invites_.end() || !Connected() || joining_ || (room_ && !SoloHost())) return false;
  accepted_invite_ = invite;
  accepted_host_ = found->host_name;
  ++accepted_generation_;
  joining_ = true;
  join_deadline_ = now_ + kJoinSeconds;
  invites_.erase(found);
  Debug("invite accepted generation=" + std::to_string(accepted_generation_));
  AbandonOwnSession();
  Send(Purpose::kInviteAccept, [&](int seq) { return EncodeInviteAccept(seq, invite); }, invite);
  return true;
}

void OnlineService::DeclineInvite(const std::string& invite) {
  std::lock_guard<std::mutex> lock(mutex_);
  invites_.erase(std::remove_if(invites_.begin(), invites_.end(), [&](const Invite& i) { return i.id == invite; }),
                 invites_.end());
  Debug("invite declined");
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
  pending_[seq] = {purpose, player, now_, room_ ? room_->room : std::string()};
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
  Debug("endpoint seat=" + std::to_string(seat_) + " public=" + (mapped ? "yes" : "no") +
        " lan=" + std::to_string(config_.lan.size()));
  Send(Purpose::kEndpoint, [&](int seq) { return EncodeMatchEndpoint(seq, id, fields); });
  endpoint_sent_ = true;
}

void OnlineService::StartPunchFor(const PeerState& peer, double now) {
  const std::vector<Candidate> candidates = PeerCandidates(peer.info, config_.lan, mapped_);
  peers_.PunchStarted(peer.seat, now);
  punching_[size_t(peer.seat)] = true;
  const int seat = peer.seat;
  config_.game_socket->Use([&](Transport& out) { rendezvous_.StartPunch(seat, candidates, net::NowMs(), out); });
}

void OnlineService::ApplyChoices(const std::vector<PathChoice>& choices) {
  for (const PathChoice& choice : choices) {
    Log(std::string("online path seat=") + std::to_string(choice.seat) + " path=" + PeerPathName(choice.path));
    Debug("peer seat=" + std::to_string(choice.seat) + " transport=" + PeerPathName(choice.path));
    if (config_.game_socket) config_.game_socket->SelectRelay(choice.seat, choice.path == PeerPath::kRelay);
    if (choice.path != PeerPath::kDirect && config_.game_socket && punching_[size_t(choice.seat)]) {
      const int seat = choice.seat;
      config_.game_socket->Use([&](Transport&) { rendezvous_.StopSeat(seat); });
    }
    if (choice.announce && room_) {
      const std::string id = room_->room;
      const int seat = choice.seat;
      const PathKind kind = choice.path == PeerPath::kRelay ? PathKind::kRelay : PathKind::kDirect;
      Send(Purpose::kTransport, [&](int seq) { return EncodeMatchTransport(seq, id, seat, kind); }, std::to_string(seat));
    }
  }
}

// A guest that cannot reach the host gives the seat back at once rather than holding it and the invite list.
void OnlineService::LeaveFailed(const std::string& reason) {
  Log("online paths failed reason=" + reason);
  const std::string id = room_->room;
  Send(Purpose::kRoomLeave, [&](int seq) { return EncodeRoomLeave(seq, id); });
  held_room_.clear();
  Notice(Tr("Unable to connect"));
  ClearRoom(false);
  closed_reason_ = "no_path";
}

void OnlineService::PumpPaths(double now) {
  if (!room_ || !config_.game_socket) return;
  OnlineGameSocket& socket = *config_.game_socket;
  if (socket.is_open()) PumpSocket(socket, now);
  if (Connected() && room_) PumpEstablishment(now);
}

void OnlineService::PumpSocket(OnlineGameSocket& socket, double now) {
  socket.Pump();
  const uint64_t now_ms = net::NowMs();
  const std::vector<PeerState>& peers = peers_.peers();
  const auto waiting = [](const PeerState& peer) {
    return peer.present && (peer.path == PeerPath::kPending || peer.path == PeerPath::kProbing);
  };

  // The mapping goes stale while a host waits alone, so it is found when there is somebody to reach.
  if (udp_ == Udp::kNone && Connected() && !peers.empty()) BeginStun();
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
      stun_done_at_ = now;
    }
  }
  if (udp_ == Udp::kReady && Connected()) {
    const bool newcomer = std::any_of(peers.begin(), peers.end(), [](const PeerState& peer) {
      return peer.present && peer.path == PeerPath::kPending;
    });
    if (endpoint_sent_ && newcomer && now - stun_done_at_ >= kStunStaleSeconds) {
      udp_ = Udp::kNone;
      endpoint_sent_ = false;
      return;
    }
    if (!endpoint_sent_) SendEndpoint();
  }

  if (udp_ == Udp::kReady && endpoint_sent_) {
    for (const PeerState& peer : peers) {
      if (!waiting(peer)) continue;
      if (peer.path == PeerPath::kPending) {
        peers_.BeginProbe(peer.seat, now);
        Debug("peer seat=" + std::to_string(peer.seat) + " probing");
      }
      if (peer.endpoint_known && !punching_[size_t(peer.seat)]) StartPunchFor(peer, now);
    }
  }
  socket.Use([&](Transport& out) { rendezvous_.Tick(now_ms, out); });
  for (const PeerState& peer : peers) {
    if (!punching_[size_t(peer.seat)]) continue;
    switch (rendezvous_.punch_stage(peer.seat)) {
      case Rendezvous::Stage::kDone:
        if (config_.force_relay) peers_.DirectFailed(peer.seat);
        else peers_.DirectProven(peer.seat, rendezvous_.selected(peer.seat).endpoint, now);
        break;
      case Rendezvous::Stage::kFailed: peers_.DirectFailed(peer.seat); break;
      default: break;
    }
  }
  // The host heard this side's answer, so this side heard the host: where its punches came from is proven too.
  if (const PeerState* host = peers_.Find(0); host && !config_.force_relay && !peers_.host() &&
                                              host->announced == PathKind::kDirect &&
                                              !host->direct_proven && !rendezvous_.heard(0).empty()) {
    peers_.DirectProven(0, rendezvous_.heard(0).front(), now);
  }
  ApplyChoices(peers_.Decide(now));

  if (const std::string failure = socket.relay_failure(); !failure.empty() && relay_token_) {
    Log("online relay failed reason=" + failure);
    peers_.FailRelayPaths();
    socket.CloseRelay();
  }
  if (now >= next_keepalive_) {
    next_keepalive_ = now + kKeepAliveSeconds;
    for (const PeerState& peer : peers) {
      if (peer.path != PeerPath::kDirect) continue;
      const Endpoint to = peer.direct;
      socket.Use([&](Transport& out) { rendezvous_.KeepAlive(to, out); });
    }
  }
  const bool ready = peers_.AllEstablished();
  if (Connected() && ready != ready_sent_) {
    ready_sent_ = ready;
    const std::string id = room_->room;
    Send(Purpose::kReady, [&](int seq) { return EncodeMatchReady(seq, id, ready); });
  }
}

// One budget per seat, from the seat appearing to its path being chosen, so no seat stays Connecting.
void OnlineService::PumpEstablishment(double now) {
  const std::vector<PeerState> peers = peers_.peers();
  for (const PeerState& peer : peers) {
    if (!peer.present) continue;
    Establishment& budget = establishment_[size_t(peer.seat)];
    const std::string seat = std::to_string(peer.seat);
    if (peer.path == PeerPath::kDirect || peer.path == PeerPath::kRelay) {
      if (budget.Select(now)) Log("network establishment ready seat=" + seat + " transport=" + PeerPathName(peer.path));
      continue;
    }
    if (peer.path != PeerPath::kFailed) {
      if (budget.phase() == Establishment::Phase::kIdle || budget.phase() == Establishment::Phase::kSelected) {
        budget.Begin(now);
        Log("network establishment begin seat=" + seat);
      }
      if (!budget.ConnectingExpired(now)) continue;
      peers_.Expire(peer.seat);
    }
    if (!budget.Fail(now, peer.path == PeerPath::kFailed ? "no_path" : "timeout")) continue;
    Log("network establishment failed seat=" + seat + " reason=" + budget.reason());
    if (!peers_.host()) {
      LeaveFailed(budget.reason());
      return;
    }
    Notice(Tr("Unable to connect") + std::string(": ") + peer.name);
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

void OnlineService::Debug(const std::string& line) { Log("[ONLINE] " + line); }

void OnlineService::InviteLog(const std::string& line) { Log("[ONLINE-INVITE] " + line); }

void OnlineService::LifecycleLog(const std::string& line) { Log("[ONLINE-LIFECYCLE] " + line); }

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
  s.invited = invited_;
  s.invite_outcome = invite_outcome_;
  s.hosting = hosting_.has_value();
  s.creating_session = sending_ && !room_;
  if (sending_) s.sending_invite = sending_->player;
  s.joining = joining_;
  s.accepted_generation = accepted_generation_;
  s.room = room_;
  s.seat = seat_;
  s.session_generation = session_generation_;
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
