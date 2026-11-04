#include "online/protocol.h"

#include <cctype>

#include "online/json.h"
#include "ui_text.h"

namespace eck::online {
namespace {

std::string Quote(std::string_view text) {
  std::string out = "\"";
  for (const char c : text) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          static constexpr char kHex[] = "0123456789abcdef";
          out += "\\u00";
          out += kHex[(c >> 4) & 0x0F];
          out += kHex[c & 0x0F];
        } else {
          out += c;
        }
    }
  }
  return out + "\"";
}

std::string Envelope(const char* op, int seq, const std::string& fields = {}) {
  return std::string("{\"op\":") + Quote(op) + ",\"seq\":" + std::to_string(seq) + fields + "}";
}

std::string WithPlayer(const char* op, int seq, const std::string& player) {
  return Envelope(op, seq, ",\"player\":" + Quote(player));
}

std::string WithRoom(const char* op, int seq, const std::string& room, const std::string& more = {}) {
  return Envelope(op, seq, ",\"room\":" + Quote(room) + more);
}

std::string RulesJson(const RoomRules& rules) {
  std::string object = "{";
  for (size_t i = 0; i < rules.size(); ++i) {
    object += (i ? "," : "") + Quote(rules[i].first) + ":" + std::to_string(rules[i].second);
  }
  return object + "}";
}

std::string StringOf(const json::Value* v) { return v && v->IsString() ? v->AsString() : std::string(); }
bool BoolOf(const json::Value* v, bool fallback) { return v && v->IsBool() ? v->AsBool() : fallback; }
int64_t Int64Of(const json::Value* v) {
  const auto n = v && v->IsNumber() ? v->AsInt64() : std::nullopt;
  return n ? *n : 0;
}
int IntOf(const json::Value* v) { return int(Int64Of(v)); }

uint16_t PortOf(const json::Value* v) {
  const int64_t n = Int64Of(v);
  return n >= 1 && n <= 65535 ? uint16_t(n) : 0;
}

uint32_t RelayTokenOf(const json::Value* v) {
  const int64_t n = Int64Of(v);
  return n > 0 && n <= 0xFFFFFFFFll ? uint32_t(n) : 0;
}

FriendState StateOf(const std::string& text) {
  if (text == "online") return FriendState::kOnline;
  if (text == "hosting") return FriendState::kHosting;
  if (text == "connecting") return FriendState::kConnecting;
  if (text == "in_game") return FriendState::kInGame;
  return FriendState::kOffline;
}

PathKind PathOf(const std::string& text) {
  if (text == "direct") return PathKind::kDirect;
  if (text == "relay") return PathKind::kRelay;
  return PathKind::kUnknown;
}

SeatState SeatStateOf(const std::string& text) {
  if (text == "connected") return SeatState::kConnected;
  if (text == "reconnecting") return SeatState::kReconnecting;
  return SeatState::kEmpty;
}

std::optional<RoomState> RoomStateOf(const std::string& text) {
  if (text == "open") return RoomState::kOpen;
  if (text == "starting") return RoomState::kStarting;
  if (text == "racing") return RoomState::kRacing;
  if (text == "closed") return RoomState::kClosed;
  return std::nullopt;
}

RoomRules ReadRules(const json::Value* v) {
  RoomRules rules;
  if (!v || !v->IsObject()) return rules;
  for (const auto& [key, value] : v->AsObject()) {
    if (value.IsNumber()) rules.emplace_back(key, IntOf(&value));
  }
  return rules;
}

std::optional<Friend> ReadFriend(const json::Value* v) {
  if (!v || !v->IsObject()) return std::nullopt;
  Friend f;
  f.player = StringOf(v->Find("player"));
  f.name = StringOf(v->Find("name"));
  if (f.player.empty()) return std::nullopt;
  f.online = BoolOf(v->Find("online"), false);
  f.state = f.online ? StateOf(StringOf(v->Find("state"))) : FriendState::kOffline;
  f.platform = StringOf(v->Find("platform"));
  f.version = StringOf(v->Find("version"));
  f.image = StringOf(v->Find("image"));
  f.build = IntOf(v->Find("build"));
  f.compatible = BoolOf(v->Find("compatible"), true);
  return f;
}

std::optional<std::vector<Friend>> ReadFriends(const json::Value* v) {
  if (!v || !v->IsArray()) return std::nullopt;
  std::vector<Friend> out;
  for (const json::Value& row : v->AsArray()) {
    std::optional<Friend> f = ReadFriend(&row);
    if (!f) return std::nullopt;
    out.push_back(std::move(*f));
  }
  return out;
}

std::optional<std::vector<Request>> ReadRequests(const json::Value* v) {
  if (!v || !v->IsArray()) return std::nullopt;
  std::vector<Request> out;
  for (const json::Value& row : v->AsArray()) {
    if (!row.IsObject()) return std::nullopt;
    Request r{StringOf(row.Find("player")), StringOf(row.Find("name"))};
    if (r.player.empty()) return std::nullopt;
    out.push_back(std::move(r));
  }
  return out;
}

std::optional<RoomView> ReadRoom(const json::Value* v) {
  if (!v || !v->IsObject()) return std::nullopt;
  RoomView room;
  room.room = StringOf(v->Find("room"));
  room.host = StringOf(v->Find("host"));
  room.host_name = StringOf(v->Find("host_name"));
  room.capacity = IntOf(v->Find("capacity"));
  room.players = IntOf(v->Find("players"));
  const std::optional<RoomState> state = RoomStateOf(StringOf(v->Find("state")));
  room.image = StringOf(v->Find("image"));
  room.build = IntOf(v->Find("build"));
  room.rules = ReadRules(v->Find("rules"));
  room.created = Int64Of(v->Find("created"));
  room.updated = Int64Of(v->Find("updated"));
  const json::Value* seats = v->Find("seats");
  if (room.room.empty() || !state || room.capacity < kMinSeats || room.capacity > kMaxSeats || !seats ||
      !seats->IsArray() || seats->AsArray().size() != size_t(room.capacity)) {
    return std::nullopt;
  }
  room.state = *state;
  int index = 0;
  for (const json::Value& row : seats->AsArray()) {
    if (!row.IsObject() || IntOf(row.Find("seat")) != index) return std::nullopt;
    Seat seat;
    seat.seat = index++;
    seat.player = StringOf(row.Find("player"));
    seat.name = StringOf(row.Find("name"));
    seat.state = SeatStateOf(StringOf(row.Find("state")));
    seat.ready = BoolOf(row.Find("ready"), false);
    seat.platform = StringOf(row.Find("platform"));
    seat.version = StringOf(row.Find("version"));
    seat.transport = PathOf(StringOf(row.Find("transport")));
    room.seats.push_back(std::move(seat));
  }
  return room;
}

std::optional<RoomAd> ReadRoomAd(const json::Value& row) {
  if (!row.IsObject()) return std::nullopt;
  RoomAd ad;
  ad.handle = StringOf(row.Find("handle"));
  ad.host = StringOf(row.Find("host"));
  ad.host_name = StringOf(row.Find("host_name"));
  ad.players = IntOf(row.Find("players"));
  ad.capacity = IntOf(row.Find("capacity"));
  ad.rules = ReadRules(row.Find("rules"));
  ad.platform = StringOf(row.Find("platform"));
  ad.version = StringOf(row.Find("version"));
  if (ad.handle.empty() || ad.host.empty()) return std::nullopt;
  return ad;
}

std::optional<std::vector<RoomAd>> ReadRoomAds(const json::Value* v) {
  if (!v || !v->IsArray()) return std::nullopt;
  std::vector<RoomAd> out;
  for (const json::Value& row : v->AsArray()) {
    std::optional<RoomAd> ad = ReadRoomAd(row);
    if (!ad) return std::nullopt;
    out.push_back(std::move(*ad));
  }
  return out;
}

std::optional<Invite> ReadInvite(const json::Value* v) {
  if (!v || !v->IsObject()) return std::nullopt;
  Invite invite;
  invite.id = StringOf(v->Find("id"));
  invite.host = StringOf(v->Find("host"));
  invite.host_name = StringOf(v->Find("host_name"));
  invite.players = IntOf(v->Find("players"));
  invite.capacity = IntOf(v->Find("capacity"));
  invite.rules = ReadRules(v->Find("rules"));
  invite.image = StringOf(v->Find("image"));
  invite.build = IntOf(v->Find("build"));
  invite.platform = StringOf(v->Find("platform"));
  invite.version = StringOf(v->Find("version"));
  invite.expires = Int64Of(v->Find("expires"));
  if (invite.id.empty() || invite.host.empty()) return std::nullopt;
  return invite;
}

ServiceCapabilities ReadCapabilities(const json::Value& v) {
  ServiceCapabilities caps;
  caps.announced = true;
  caps.protocol_version = IntOf(v.Find("protocol_version"));
  caps.worker_build = StringOf(v.Find("worker_build"));
  caps.public_endpoints = BoolOf(v.Find("public_endpoints"), false);
  caps.relay_supported = BoolOf(v.Find("relay_supported"), false);
  caps.relay_max_payload = IntOf(v.Find("relay_max_payload"));
  if (const int n = IntOf(v.Find("min_players")); n > 0) caps.min_players = n;
  if (const int n = IntOf(v.Find("max_players")); n > 0) caps.max_players = n;
  caps.four_player_rooms = BoolOf(v.Find("four_player_rooms"), false);
  caps.seat_routing = BoolOf(v.Find("seat_routing"), false);
  caps.race_telemetry = BoolOf(v.Find("race_telemetry"), false);
  return caps;
}

std::vector<std::string> ReadStrings(const json::Value* v) {
  std::vector<std::string> out;
  if (!v || !v->IsArray()) return out;
  for (const json::Value& item : v->AsArray()) {
    if (item.IsString()) out.push_back(item.AsString());
  }
  return out;
}

}  // namespace

std::optional<ServerMessage> DecodeServerMessage(std::string_view text) {
  const std::optional<json::Value> root = json::Parse(text);
  if (!root || !root->IsObject()) return std::nullopt;
  ServerMessage message;
  if (root->Find("re")) {
    message.kind = ServerMessage::Kind::kResponse;
    message.seq = IntOf(root->Find("re"));
    message.code = StringOf(root->Find("code"));
    if (message.code.empty()) return std::nullopt;
    message.friend_code = StringOf(root->Find("friend_code"));
    message.state = StringOf(root->Find("state"));
    message.invite_id = StringOf(root->Find("invite"));
    message.invite_expires = Int64Of(root->Find("expires"));
    message.server_time = Int64Of(root->Find("time"));
    message.resume = StringOf(root->Find("resume"));
    message.rooms = ReadRoomAds(root->Find("rooms"));
    message.friends = ReadFriends(root->Find("friends"));
    message.incoming = ReadRequests(root->Find("incoming"));
    message.outgoing = ReadRequests(root->Find("outgoing"));
    if (const json::Value* caps = root->Find("capabilities"); caps && caps->IsObject()) {
      message.capabilities = ReadCapabilities(*caps);
    }
    if (const json::Value* room = root->Find("room"); room && room->IsObject()) {
      message.room = ReadRoom(room);
      if (!message.room) return std::nullopt;
      message.seat = root->Find("seat") ? IntOf(root->Find("seat")) : -1;
      message.relay_token = RelayTokenOf(root->Find("relay_token"));
    }
    return message;
  }
  const std::string event = StringOf(root->Find("ev"));
  using Kind = ServerMessage::Kind;
  if (event == "friend.status" || event == "friend.added") {
    std::optional<Friend> f = ReadFriend(root->Find("friend"));
    if (!f) return std::nullopt;
    message.kind = event == "friend.status" ? Kind::kFriendStatus : Kind::kFriendAdded;
    message.friend_view = std::move(*f);
    return message;
  }
  if (event == "friend.request") {
    message.kind = Kind::kFriendRequest;
    message.request = {StringOf(root->Find("player")), StringOf(root->Find("name"))};
    return message.request.player.empty() ? std::nullopt : std::optional<ServerMessage>(message);
  }
  if (event == "friend.removed" || event == "friend.offline") {
    message.kind = event == "friend.removed" ? Kind::kFriendRemoved : Kind::kFriendOffline;
    message.player = StringOf(root->Find("player"));
    return message.player.empty() ? std::nullopt : std::optional<ServerMessage>(message);
  }
  if (event == "room.created" || event == "room.updated") {
    message.kind = event == "room.created" ? Kind::kRoomCreated : Kind::kRoomUpdated;
    message.room = ReadRoom(root->Find("room"));
    if (!message.room) return std::nullopt;
    message.seat = root->Find("seat") ? IntOf(root->Find("seat")) : -1;
    message.relay_token = RelayTokenOf(root->Find("relay_token"));
    return message;
  }
  if (event == "room.closed" || event == "relay.closed") {
    message.kind = event == "room.closed" ? Kind::kRoomClosed : Kind::kRelayClosed;
    message.room_id = StringOf(root->Find("room"));
    message.reason = StringOf(root->Find("reason"));
    return message.room_id.empty() ? std::nullopt : std::optional<ServerMessage>(message);
  }
  if (event == "invite.received") {
    message.invite = ReadInvite(root->Find("invite"));
    if (!message.invite) return std::nullopt;
    message.kind = Kind::kInviteReceived;
    return message;
  }
  if (event == "invite.ended") {
    message.kind = Kind::kInviteEnded;
    message.invite_id = StringOf(root->Find("invite"));
    message.room_id = StringOf(root->Find("room"));
    message.reason = StringOf(root->Find("reason"));
    return message.invite_id.empty() ? std::nullopt : std::optional<ServerMessage>(message);
  }
  if (event == "match.transport") {
    message.kind = Kind::kMatchTransport;
    message.room_id = StringOf(root->Find("room"));
    message.transport = PathOf(StringOf(root->Find("transport")));
    message.transport_seat = root->Find("seat") ? IntOf(root->Find("seat")) : -1;
    return message.room_id.empty() || message.transport == PathKind::kUnknown ? std::nullopt
                                                                              : std::optional<ServerMessage>(message);
  }
  if (event == "match.peer") {
    message.kind = Kind::kMatchPeer;
    PeerInfo& peer = message.peer;
    peer.room = StringOf(root->Find("room"));
    peer.seat = root->Find("seat") ? IntOf(root->Find("seat")) : -1;
    peer.player = StringOf(root->Find("player"));
    peer.name = StringOf(root->Find("name"));
    peer.ip = StringOf(root->Find("ip"));
    peer.port = PortOf(root->Find("port"));
    if (peer.room.empty() || peer.seat < 0 || peer.seat >= kMaxSeats || peer.port == 0) return std::nullopt;
    peer.same_network = BoolOf(root->Find("same_network"), false);
    peer.relay = BoolOf(root->Find("relay"), false);
    peer.lan = ReadStrings(root->Find("lan"));
    peer.public_ip = StringOf(root->Find("public_ip"));
    peer.public_port = PortOf(root->Find("public_port"));
    if (peer.public_ip.empty()) peer.public_port = 0;
    if (const json::Value* list = root->Find("candidates"); list && list->IsArray()) {
      for (const json::Value& item : list->AsArray()) {
        if (!item.IsObject()) continue;
        EndpointCandidate candidate{StringOf(item.Find("ip")), PortOf(item.Find("port")), StringOf(item.Find("kind"))};
        if (!candidate.ip.empty() && candidate.port) peer.candidates.push_back(std::move(candidate));
      }
    }
    return message;
  }
  return std::nullopt;
}

std::string EncodeHello(int seq, const HelloFields& hello) {
  return Envelope("hello", seq,
                  ",\"proto\":" + std::to_string(kProtocolVersion) + ",\"game\":" + Quote(kGameId) +
                      ",\"image\":" + Quote(hello.image) + ",\"build\":" + std::to_string(hello.build) +
                      ",\"player\":" + Quote(hello.player) + ",\"token\":" + Quote(hello.token) +
                      ",\"name\":" + Quote(hello.name) + ",\"version\":" + Quote(hello.version) +
                      ",\"platform\":" + Quote(hello.platform) + ",\"presence\":" + (hello.presence ? "true" : "false") +
                      ",\"relay\":" + (hello.relay ? "true" : "false"));
}

std::string EncodePing(int seq) { return Envelope("ping", seq); }
std::string EncodeFriendsList(int seq) { return Envelope("friends.list", seq); }
std::string EncodeFriendsRequests(int seq) { return Envelope("friends.requests", seq); }
std::string EncodeFriendsAdd(int seq, const std::string& code) {
  return Envelope("friends.add", seq, ",\"code\":" + Quote(code));
}
std::string EncodeFriendsAccept(int seq, const std::string& player) { return WithPlayer("friends.accept", seq, player); }
std::string EncodeFriendsDecline(int seq, const std::string& player) {
  return WithPlayer("friends.decline", seq, player);
}
std::string EncodeFriendsRemove(int seq, const std::string& player) { return WithPlayer("friends.remove", seq, player); }

std::string EncodeRoomHost(int seq, int capacity, const RoomRules& rules) {
  return Envelope("room.host", seq, ",\"capacity\":" + std::to_string(capacity) + ",\"rules\":" + RulesJson(rules));
}

std::string EncodeRoomUpdate(int seq, const std::string& room, int capacity, const RoomRules* rules) {
  std::string more;
  if (capacity >= 0) more += ",\"capacity\":" + std::to_string(capacity);
  if (rules) more += ",\"rules\":" + RulesJson(*rules);
  return WithRoom("room.update", seq, room, more);
}

std::string EncodeRoomCancel(int seq, const std::string& room) { return WithRoom("room.cancel", seq, room); }
std::string EncodeRoomLeave(int seq, const std::string& room) { return WithRoom("room.leave", seq, room); }
std::string EncodeRoomSearch(int seq) { return Envelope("room.search", seq); }
std::string EncodeRoomJoin(int seq, const std::string& handle) {
  return Envelope("room.join", seq, ",\"handle\":" + Quote(handle));
}
std::string EncodeRoomStart(int seq, const std::string& room) { return WithRoom("room.start", seq, room); }

std::string EncodeInviteSend(int seq, const std::string& player) { return WithPlayer("invite.send", seq, player); }
std::string EncodeInviteAccept(int seq, const std::string& invite) {
  return Envelope("invite.accept", seq, ",\"invite\":" + Quote(invite));
}
std::string EncodeInviteDecline(int seq, const std::string& invite) {
  return Envelope("invite.decline", seq, ",\"invite\":" + Quote(invite));
}

std::string EncodeMatchEndpoint(int seq, const std::string& room, const EndpointFields& e) {
  std::string lan = "[";
  for (size_t i = 0; i < e.lan.size(); ++i) lan += (i ? "," : "") + Quote(e.lan[i]);
  lan += "]";
  std::string candidates = "[";
  for (size_t i = 0; i < e.candidates.size(); ++i) {
    candidates += std::string(i ? "," : "") + "{\"ip\":" + Quote(e.candidates[i].ip) +
                  ",\"port\":" + std::to_string(e.candidates[i].port) + ",\"kind\":" + Quote(e.candidates[i].kind) + "}";
  }
  candidates += "]";
  std::string mapped;
  if (e.public_port && !e.public_ip.empty()) {
    mapped = ",\"public_ip\":" + Quote(e.public_ip) + ",\"public_port\":" + std::to_string(e.public_port);
  }
  return WithRoom("match.endpoint", seq, room,
                  ",\"port\":" + std::to_string(e.port) + ",\"lan\":" + lan + mapped + ",\"candidates\":" + candidates +
                      ",\"relay\":" + (e.relay ? "true" : "false"));
}

std::string EncodeMatchReady(int seq, const std::string& room, bool ready) {
  return WithRoom("match.ready", seq, room, std::string(",\"ready\":") + (ready ? "true" : "false"));
}

std::string EncodeMatchLeave(int seq, const std::string& room) { return WithRoom("match.leave", seq, room); }

std::string EncodeMatchTransport(int seq, const std::string& room, int seat, PathKind transport) {
  return WithRoom("match.transport", seq, room,
                  ",\"seat\":" + std::to_string(seat) + ",\"transport\":" + Quote(PathKindName(transport)));
}

std::string EncodeMatchResume(int seq, const std::string& room) { return WithRoom("match.resume", seq, room); }

namespace {

std::string IntList(const std::vector<int>& values) {
  std::string list = "[";
  for (size_t i = 0; i < values.size(); ++i) list += (i ? "," : "") + std::to_string(values[i]);
  return list + "]";
}

std::string RaceRoom(const std::string& room) { return room.empty() ? std::string() : ",\"room\":" + Quote(room); }

}  // namespace

std::string EncodeRaceStart(int seq, const RaceStartFields& race) {
  std::string more = RaceRoom(race.room) + ",\"race\":" + std::to_string(race.race) + ",\"competition\":" + Quote(race.competition) +
                     ",\"players\":" + std::to_string(race.players);
  if (race.track) more += ",\"track\":" + std::to_string(*race.track);
  if (!race.characters.empty()) more += ",\"characters\":" + IntList(race.characters);
  return Envelope("race.start", seq, more);
}

std::string EncodeRaceComplete(int seq, const RaceCompleteFields& race) {
  std::string more = RaceRoom(race.room) + ",\"race\":" + std::to_string(race.race) + ",\"duration_ms\":" + std::to_string(race.duration_ms);
  if (!race.places.empty()) more += ",\"places\":" + IntList(race.places);
  return Envelope("race.complete", seq, more);
}

std::string NormalizeFriendCode(std::string_view text) {
  std::string compact;
  for (const char c : text) {
    if (c == '-' || std::isspace(static_cast<unsigned char>(c))) continue;
    compact += char(std::toupper(static_cast<unsigned char>(c)));
  }
  if (compact.size() != 8) return {};
  for (const char c : compact) {
    const bool letter = c >= 'A' && c <= 'Z' && c != 'I' && c != 'O';
    const bool digit = c >= '2' && c <= '9';
    if (!letter && !digit) return {};
  }
  return compact.substr(0, 4) + "-" + compact.substr(4);
}

std::string DescribeServiceError(const std::string& code) {
  if (code == "version_mismatch") return Tr("Version mismatch");
  if (code == "not_found") return Tr("No player has that friend code.");
  if (code == "not_friends") return Tr("That player is not your friend.");
  if (code == "offline") return Tr("Your friend is not online.");
  if (code == "busy") return Tr("One of you is already in a room.");
  if (code == "room_full") return Tr("Room full");
  if (code == "room_not_open") return Tr("The race has already started.");
  if (code == "need_players") return Tr("Waiting for players");
  if (code == "bad_request") return Tr("That is not a valid friend code.");
  if (code == "limit") return Tr("Too many requests. Try again in a moment.");
  if (code == "proto") return Tr("The online service needs a different version of the game.");
  if (code == "unauthorized" || code == "bad_game" || code == "bad_image") {
    return Tr("The online service did not accept this game.");
  }
  if (code == "unreachable" || code == "link_lost" || code == "timeout") return Tr("Unable to connect");
  if (code == "relay_unavailable") return Tr("The connection through the online service is not available.");
  return std::string(Tr("The online service refused the request (")) + code + ").";
}

bool ImagesCompatible(const std::string& image_a, int build_a, const std::string& image_b, int build_b) {
  return image_a == image_b && build_a == build_b;
}

std::string FriendStatusLine(const Friend& entry) {
  if (!entry.online) return Tr("Offline");
  std::string line;
  switch (entry.state) {
    case FriendState::kHosting: line = Tr("Hosting"); break;
    case FriendState::kConnecting: line = Tr("Connecting"); break;
    case FriendState::kInGame: line = Tr("In game"); break;
    default: line = Tr("Online"); break;
  }
  if (!entry.platform.empty()) line += Tr(" \xC2\xB7 ") + entry.platform;
  if (!entry.version.empty()) line += Tr(" \xC2\xB7 v") + entry.version;
  return line;
}

const char* PathKindName(PathKind kind) {
  switch (kind) {
    case PathKind::kDirect: return "direct";
    case PathKind::kRelay: return "relay";
    default: return "";
  }
}

const char* RoomStateName(RoomState state) {
  switch (state) {
    case RoomState::kOpen: return "open";
    case RoomState::kStarting: return "starting";
    case RoomState::kRacing: return "racing";
    default: return "closed";
  }
}

}  // namespace eck::online
