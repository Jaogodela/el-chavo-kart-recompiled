# ECK lobby protocol, version 1

Wire contract between the El Chavo Kart PC client and `eck-lobby`. Everything is a WebSocket to the service root
(`wss://eck-lobby.saulobenigno.workers.dev`; `ECK_LOBBY` overrides it for development).

- Text frames (UTF-8 JSON, at most 4096 bytes) are signalling. A text frame that is exactly `ping` is answered with the
  text `pong` (no sign-in needed); use it as the keep-alive.
- Binary frames are relay frames (section 7).
- `PROTOCOL_VERSION = 1`, game id `"el-chavo-kart-pc"`.

## 1. Envelope

Request: `{"op": "<name>", "seq": <int 0..2147483647>, ...fields}`.
Response, always exactly one per request: `{"re": <seq>, "code": "ok" | "<error code>", ...fields}`.
Event, pushed any time after sign-in: `{"ev": "<name>", ...fields}`. Events have no `re`.
Unknown fields are ignored. Unknown ops answer `bad_request`. Every op except `hello` answers `no_session` before `hello`.

Common error codes (any op): `bad_request` (shape/type), `too_large` (text frame over 4096 bytes), `limit` (a rate or
count limit), `no_session`, `internal`.

## 2. Identity and sign-in

The client keeps a private 64-char lowercase hex `token`. Both values below are SHA-256 hex digests of UTF-8 text:

- stored hash: `sha256hex("eck-token:" + token)` (server side only)
- `player` / friend code: `d = sha256hex("eck-player-id:" + token)`; for i in 0..7 take byte i of the digest
  (`parseInt(d.slice(2*i, 2*i+2), 16)`), index `"ABCDEFGHJKLMNPQRSTUVWXYZ23456789"` with `byte % 32`; insert `-` after the 4th
  char. Result `XXXX-XXXX` (letters except I and O, digits 2-9). Friend codes are typed case-insensitively, with or without
  `-` or spaces.

(The domain prefixes are specific to this game, so identities are not shared with other games' lobbies.)

### hello

```
{"op":"hello","seq":1,"proto":1,"game":"el-chavo-kart-pc","player":"ABCD-2345","token":"<64 hex>",
 "name":"Saulo","image":"eck-usa-1db4e96b","build":1,"version":"0.1.0","platform":"Windows",
 "presence":true,"relay":true}
```
- `image`: string, the game image id; the service only accepts ids in its `SUPPORTED_IMAGES` allowlist
  (currently `eck-usa-1db4e96b`, the title's default.xex SHA-256 starts 1DB4E96B).
- `build`: integer 0..65535, the online/netplay build number.
- `name`: stripped to `[A-Za-z0-9_-]`, max 16 chars, empty becomes `Player`. `version`, `platform`: stripped to
  `[A-Za-z0-9 ._+-]`, max 16 chars, optional.
- `presence` (default true): false is a probe: the player is not shown online and cannot host/join/invite.
- `relay` (default false): this client can send/receive relay frames.
- A new `player` registers on first hello; a known `player` with another token is `unauthorized`.

Reply: `{"re":1,"code":"ok","player":"ABCD-2345","name":"Saulo","friend_code":"ABCD-2345","time":<ms>,
"capabilities":{"protocol_version":1,"worker_build":"...","public_endpoints":true,"relay_supported":true,
"relay_max_payload":1472,"min_players":2,"max_players":4,"four_player_rooms":true,"seat_routing":true},"resume":null}`.
`resume` is the room id of a seat this player still holds after a lost connection (section 5, `match.resume`), else null.

Errors: `proto` (proto != 1), `bad_game`, `bad_image`, `bad_request` (build/token/shape), `unauthorized` (id does not
match token, or token does not match the stored one), `limit` (sign-in flood, or too many new players per address),
`bad_request` if already signed in on this socket.

Two peers are *compatible* (can share a room) when `image` and `build` are both equal.

### ping
`{"op":"ping","seq":n}` -> `{"re":n,"code":"ok","time":<ms>}`.

## 3. Friends and presence

Friend view: online `{"player","name","online":true,"state","platform","version","image","build","compatible"}`;
offline `{"player","name","online":false,"state":"offline"}`. `compatible` is relative to the viewer.
`state`: `offline | online | hosting | connecting | in_game`:
`hosting` = host of an `open` room; `connecting` = seated in someone else's `open` room; `in_game` = in a `starting` or
`racing` room (any seat, host included); else `online`.

- `friends.list` -> `{friends:[view]}`
- `friends.requests` -> `{incoming:[{player,name,ms}], outgoing:[{player,name,ms}]}`
- `friends.add {code}` -> `{player,name,state:"pending"|"friends"}`; errors `not_found`, `bad_request` (own/malformed code), `limit`.
  Adding someone who already asked you makes you friends at once.
- `friends.accept {player}` -> `{friend:view}`; `not_found` if no request.
- `friends.decline {player}` -> `{}`; `not_found`.
- `friends.remove {player}` -> `{}`.

Events: `friend.request {player,name}`, `friend.added {friend:view}`, `friend.removed {player}`,
`friend.status {friend:view}` (any presence/state change of a friend), `friend.offline {player}`.

## 4. Rooms

Room view (used everywhere a room is returned or pushed):
```
{"room":"<16 hex id>","host":"ABCD-2345","host_name":"Saulo","capacity":4,"players":2,"state":"open",
 "image":"eck-usa-1db4e96b","build":1,"rules":{"mode":1,"track":3,"laps":3},
 "seats":[
  {"seat":0,"player":"ABCD-2345","name":"Saulo","state":"connected","ready":false,"platform":"Windows","version":"0.1.0","transport":""},
  {"seat":1,"player":"WXYZ-9876","name":"Ana","state":"reconnecting","ready":false,"platform":"Windows","version":"0.1.0","transport":"relay"},
  {"seat":2,"player":"","name":"","state":"empty","ready":false,"platform":"","version":"","transport":""},
  {"seat":3,"player":"","name":"","state":"empty","ready":false,"platform":"","version":"","transport":""}],
 "created":<ms>,"updated":<ms>}
```
- `seats` always has exactly `capacity` entries, index = seat number = in-game player slot (seat 0 = Player 1).
- Seat state: `connected | reconnecting | empty`. `ready` is the seat's `match.ready` flag. `transport` is `""` until the
  host reports it (`direct | relay`, `match.transport`).
- Room state: `open` (joinable) -> `starting` (host pressed start; seats frozen) -> `racing` -> `closed`. `closed` is only
  ever seen inside `room.closed`.
- `rules`: object of at most 16 keys, each key `[a-z_]{1,16}`, each value an integer 0..65535. Opaque to the service.

Seat assignment is server-authoritative. The host is seat 0. Every join takes the lowest free seat index. Seats are
never accepted from the client. A player who leaves an `open` room frees the seat; the next join takes the lowest free
one. Once the state leaves `open`, seats are frozen. A player has at most one room at a time (`busy` otherwise).
Replies to `room.host`, `room.join`, `invite.accept` and `match.resume` carry `seat` (your own seat) and, when the
room has a relay and this client signed in with `relay:true`, `relay_token` (u32, nonzero).

### room.host
`{"op":"room.host","seq":n,"capacity":2|3|4,"rules":{...}}` (`capacity` default 2, `rules` default `{}`)
-> `{"re":n,"code":"ok","room":view,"seat":0,"relay_token":123}`; then event `room.created {room:view,seat:0,relay_token?}` to
the host's socket.
Errors: `bad_request` (capacity/rules; `presence` false), `busy` (seated in someone else's room, or your own has started),
`limit` (creating rooms too fast). Requires `presence:true`. The room image/build are the host's.
Sent by the host of an `open` room it is a refresh, not a new room: the same reply, no event, no creation budget spent. Omitted
fields keep their value, changed ones apply as in `room.update`. An open room lives 1 h from its last write; an unchanged
refresh within 30 s of the last one is not written.

### room.update (host only, state `open`)
`{"op":"room.update","seq":n,"room":"<id>","capacity":3,"rules":{...}}` (either field optional) -> `{room:view}`.
Errors: `not_found`, `not_host`, `room_not_open`, `bad_request`, `capacity_too_small` (an occupied seat index would not fit).
Outstanding invites beyond the remaining free seats end with `room_full`. Everyone gets `room.updated`, unless nothing changed.

### room.cancel (host only) / room.leave / match.leave
- `room.cancel {room}` -> `{}`; closes the room for everyone (`room.closed` reason `cancelled`).
- `room.leave {room}` and `match.leave {room}` are the same op: the host leaving closes the room for everyone (reason
  `host_left`); another player leaving frees the seat (`open`) or empties it (`starting`/`racing`) and everyone left gets
  `room.updated`. Works for a held (`reconnecting`) seat from any socket of that player. Leaving a room you are not in is `{}`.

### room.search
`{"op":"room.search","seq":n}` -> `{"rooms":[{"handle":"<8 hex>","host":"ABCD-2345","host_name":"Saulo","players":2,
"capacity":4,"rules":{...},"platform":"Windows","version":"0.1.0"}]}` (at most 20).
Only `open` rooms whose host is a friend of the caller, is connected, and is compatible (image+build). Full rooms are
listed (`players == capacity`). `handle` is opaque, bound to the caller and valid for at least 60 s after any search that
returns it; a search returns the same handle until less than 60 s of its 120 s remain. The room id and every address stay hidden.

### room.join
`{"op":"room.join","seq":n,"handle":"<from room.search>"}` -> `{room:view, seat, relay_token?}`; everyone else gets
`room.updated`.
Errors: `not_found` (unknown/expired/other player's handle, or room gone), `not_friends`, `offline` (host not connected),
`version_mismatch` (image or build differs), `busy` (you are in a room), `room_not_open` (starting/racing), `room_full`,
`bad_request` (own room, presence false), `limit`.

### room.start (host only)
`{"op":"room.start","seq":n,"room":"<id>"}` -> `{room:view}` with state `starting`. Requires state `open` and at least 2
occupied seats (`need_players`). Freezes seats, clears every `ready` flag, ends all invites with `room_started`, hides the
room from search (a handle obtained earlier then answers `room_not_open` to `room.join`). Errors: `not_found`, `not_host`, `room_not_open`, `need_players`.
When every occupied seat has `match.ready` true while `starting`, the state becomes `racing` (event `room.updated`).

### Room events
- `room.created {room, seat, relay_token?}` (host, after `room.host`)
- `room.updated {room:view}`: full snapshot after any change (join, leave, ready, state, rules, capacity, seat
  connected/reconnecting).
- `room.closed {room:"<id>", reason}`: reasons `host_left | cancelled | disconnect | expired`. After it your
  seat is gone; `disconnect` = the host did not come back within 30 s.

## 5. Invites

Only the host of an `open` room can invite. Up to the number of free seats (never more than 3) invites are outstanding
at once; each expires after 60 s.

- `invite.send {player}` -> `{invite:"<16 hex>",expires:<ms>}`. A second invite to the same player replaces the first.
  Errors: `not_found` (you host no open room), `not_friends`, `offline`, `version_mismatch`, `busy` (target is in another
  room), `already_in_room` (target is in this one), `room_full`, `invite_limit` (outstanding invites already cover every
  free seat), `limit`, `bad_request` (presence false, own code).
- `invite.accept {invite}` -> same reply as `room.join` (`room`, `seat`, `relay_token?`). It joins the invite's room and never
  creates one. Errors: `not_found` (unknown/ended/not yours), `room_full`, `room_not_open`, `busy`, `version_mismatch`,
  `offline`, `limit`.
- `invite.decline {invite}` -> `{}` (unknown invite is also `{}`).

Events:
- `invite.received {invite:{"id","host","host_name","players","capacity","rules","image","build","platform","version","expires"}}` (to the target)
- `invite.ended {invite:"<id>",room:"<id>"?,reason}`. Reasons: `declined`, `accepted`, `expired`, `room_closed`, `room_full`,
  `host_left`, `room_started`, `cancelled`. Targets are told for everything except what they did themselves (`declined`,
  `accepted`); the sender is told only of `declined`, `accepted` and `expired`. `room` is only present for the sender.
  A target going offline silently drops its invites.

## 6. Match setup (between seated players)

All take `room` (the id from the room view) and require that the caller holds a seat in it (`not_found` otherwise).

### match.endpoint
`{"op":"match.endpoint","seq":n,"room":"<id>","port":7001,"lan":["192.168.1.5"],"public_ip":"203.0.113.9",
"public_port":50001,"candidates":[{"ip":"203.0.113.9","port":50001,"kind":"srflx"}],"relay":true}` -> `{}`.
- `port`: 1024..65535 (the game's UDP port). `lan`: up to 4 loopback/private IPv4. `public_ip`+`public_port`: both or neither,
  the client's own claim (the service never sees UDP). `candidates`: up to 6, `kind` in `host|srflx|upnp`, IPv4 any scope,
  port 1..65535. `relay` (optional bool): only meaningful with `hello.relay`.
Server stores it for the seat and pushes to every other connected seat of the room
`match.peer {room,seat,player,name,ip,port,lan,public_ip?,public_port?,candidates,same_network,relay}` where `ip` is the
address the service saw and `same_network` is true when it is in the receiver's address group (the same IPv4 address, or the
same IPv6 /64). `lan` and non-public `candidates` are only sent to a seat on the same network. It also pushes the publisher the
stored endpoints of the other seats, so the order of publishing does not matter. Republishing replaces and re-pushes; an
unchanged one is not written.
A public address can only be the sender's own: on an IPv4 connection a `public_ip` or public candidate other than the
connection's address is dropped, not forwarded. On an IPv6 connection the claim cannot be compared, so it passes as a claim;
only a punch acknowledgement, keyed by the room, makes a client use an address.

### match.ready
`{"op":"match.ready","seq":n,"room":"<id>","ready":true}` -> `{}`; everyone gets `room.updated` if the flag changed. See `room.start` for the
`starting` -> `racing` transition. In `open` it is a plain lobby ready flag.

### match.transport (host only)
`{"op":"match.transport","seq":n,"room":"<id>","seat":2,"transport":"direct"|"relay"}` -> `{}`. The host's choice for its
path to one peer. `relay` needs the room's relay, relay-capable host and peer, and the peer connected (`relay_unavailable`).
Errors: `not_host`, `bad_request`, `not_found`. Peer gets event `match.transport {room,seat:<host seat 0>,transport}`; seat view
`transport` is updated (visible in `room.updated`).

### match.resume
After a socket loss the seat is held 30 s (`state:"reconnecting"`; no one else can take it). On a new socket: `hello`,
then `{"op":"match.resume","seq":n,"room":"<id>"}` (`room.resume` is the same op) -> `{room:view, seat, relay_token?}` with only
the caller's own seat.
The old socket, if still bound, is closed with code 4001. Others get `room.updated` and a fresh `match.peer` for the
resumed seat once it republishes. Errors: `not_found` (no such held seat for this player, or hold expired), `bad_request`
(already in a room on this socket), `limit`. If the hold expires: in an `open` room the seat is freed (host: room closes
`disconnect`); in `starting`/`racing` the seat becomes `empty` (host: room closes `disconnect`).

### Events
`match.peer`, `match.transport`, `relay.closed {room,reason}` (reasons: `rate_limit`, `peer_backpressure`, `peer_unavailable`,
`unavailable`). A closed relay clears the `relay` transport of its seats (`room.updated`).

## 7. Relay frames (binary WebSocket frames)

Used when UDP paths fail. Scoped to the sender's own room and routed by seat.

```
offset size  field
0      1     0x45 ('E')
1      1     0x4B ('K')
2      1     version = 1
3      1     type = 1 (data)
4      4     token   u32 big-endian, the room's relay_token
8      4     seq     u32 big-endian, opaque to the service
12     1     dest    0..3 = that seat only, 0xFF = every other seat
13     1     src     client sends 0; the service overwrites it with the sender's real seat
14     2     length  u16 big-endian, payload bytes, 1..1472
16     N     payload opaque
```
Total length must be exactly 16 + length. The sender seat is always taken from the socket; `src` is stamped by the server and
never trusted. Frames never leave the sender's room, are not delivered back to the sender, are not stored or logged
(only counters and the first few frame sizes). A frame to an occupied seat that is reconnecting is dropped silently. Invalid
frames (bad magic/version/type/length, a destination that is empty, outside the room or the sender, wrong token) are dropped;
wrong/old token and no relay send `relay.closed {reason:"unavailable"}` once per room. A sender that keeps producing invalid
frames (200, then 5 per s) is closed with 1008; a seat that is away, or a relay that closed while one is, is not its fault.

Limits: a sender is charged once per frame received, 800 burst, 400/s packets and 256 KiB burst, 128 KiB/s; the room is charged
once per delivery, each seat of its capacity adding one sender's budget:

| capacity | packets burst, per s | bytes burst, per s |
|---|---|---|
| 2 | 1600, 800 | 512 KiB, 256 KiB |
| 3 | 2400, 1200 | 768 KiB, 384 KiB |
| 4 | 3200, 1600 | 1 MiB, 512 KiB |

256 dropped frames close the room's relay (`relay.closed` reason `rate_limit`). A recipient with more than 256 KiB queued loses
its frames while the other seats keep theirs; 1024 such drops in a row close the relay (`peer_backpressure`).

## 8. Race telemetry

Anonymous race statistics (README.md, Race telemetry). The game sends these by itself, on the connection it already has, and only
when `capabilities.race_telemetry` is true. A rejection is that call's own error code and never touches the room or the race.

With `room`, the race belongs to a started room: only its host reports it, and the service reads the player count from the seats.
Without `room`, it is this PC's own race, solo or split-screen, tied to this connection.

### race.start
`{"op":"race.start","seq":n,"room":"<id>","race":0,"competition":"time_attack","players":2,"track":4,"characters":[0,1]}` -> `{}`.
- `race`: integer 0..65535. Room: the room's next race from 0, at most 64 (`limit`); an earlier number is a harmless repeat, a later
  one is `race_order`. No room: any number above the last one on this connection; a number already used is a harmless repeat.
- `competition`: `normal_race`, `time_attack`, `survival`, `endurance`, `coins`, `follow_the_leader`, `ctf`, `challenges`,
  `challenges_ctf`.
- `players`: optional, 1..4. Room: must equal the occupied seats. No room: defaults to the number of `characters`, else 1.
- `track`: optional, `EKartTracks` id 0..19. `characters`: optional, one `EKartCharacterType` id 0..11 per human, in seat order; the same
  id may repeat. The client leaves out what it could not prove. Ids are in `src/game_data.js`.
- A race still pending when a new one starts counts as abandoned.
- Errors: `bad_request`, `not_found`, `not_host`, `race_not_ready` (room not `racing`), `race_order`, `limit`.

### race.complete
`{"op":"race.complete","seq":n,"room":"<id>","race":0,"duration_ms":125000,"places":[3,1]}` -> `{}`.
- `race`: the pending race; a repeat of one already finished is `{}`, anything else `race_mismatch`.
- `duration_ms`: 1000 to 3600000, clamped to what the service measured since `race.start` plus 5 s.
- `places`: optional, one distinct `RaceFinishPosition` 1..16 per human, in the order of `characters`.
- Errors: `bad_request`, `not_found`, `not_host`, `race_mismatch`, `limit`.

There is no leave message: a pending race ends as abandoned when its room closes, its connection ends or a new race starts.

## 9. Presence and limits summary

Limits: 4096-byte text frames, 60 msgs burst / 20 per s per socket, 10 burst / 1 per s for friends.add, room.host,
room.join, invite.send, invite.accept and match.resume, 200 friends, 50 pending requests each way, 16 sockets per address,
20 new players per address per day, seat hold 30 s, invite 60 s, open room 1 h, any room 4 h (`expired`), search results 20.
Race telemetry: 30 burst / 1 per 3 s per account, 40 burst / 1 per 2 s per room (or per account, for a solo race).

## 10. Error code index
`bad_request too_large limit no_session internal proto bad_game bad_image unauthorized not_found not_friends offline
version_mismatch busy already_in_room room_full room_not_open not_host need_players capacity_too_small invite_limit
relay_unavailable race_not_ready race_order race_mismatch`.
