# eck-lobby

Online service for El Chavo Kart (PC recompilation): player identity, friends, presence, rooms of 2 to 4 players with
server-assigned seats, invites, endpoint exchange and a per-room binary relay. Races run directly between the PCs over UDP
whenever they can; a seat that cannot be reached directly is relayed through this service, between the sockets of that one
room only.

Production address: `wss://eck-lobby.saulobenigno.workers.dev`. The game uses it by default: players launch the game, turn
Online play on, and need no Node, no server, no environment variable and no Cloudflare tool.

This service is separate from every other game's. It has its own Worker name, Durable Object class, SQL data, dashboard secret,
rate-limit namespace and protocol (`game: "el-chavo-kart-pc"`, protocol 1). The wire format is [PROTOCOL.md](PROTOCOL.md).

## Design

- `src/worker.js`: the Worker. `GET /` and `GET /health` return `service`, `game`, `protocol`, `status`, `worker_build`,
  `relay_supported`, `relay_max_payload`. Every WebSocket goes to the single `ECKLobby` Durable Object (SQLite-backed, WebSocket
  Hibernation, state in socket attachments) after the edge gate in `src/edge.js`.
- `src/hub.js`: the whole service, independent of Cloudflare. A **room is rows, not an object**: any number run side by side in
  one Durable Object. Tables: `players`, `friends`, `friend_requests`, `registrations`, `rooms`, `room_seats`, `invites`, `handles`,
  `meta`, plus the metrics tables below.
- `src/protocol.js`: versions, limits, validation, friend code derivation, the relay frame codec. `SUPPORTED_IMAGES` is the allowlist
  of game images; two peers are compatible when `image` and `build` are equal.
- `src/game_data.js`: the fixed ids race telemetry accepts, in the order of the game's script.
- `src/abuse.js`: the in-memory limits. `src/metrics.js`: aggregate metrics. `src/admin.js`, `src/admin_page.js`: the dashboard.
- `local/`: the same hub on Node, used by the automated tests. It is not part of the service and no player or normal test needs it.

A player is identified by a private 32-byte token that never leaves their PC except to this service. The public id
(`XXXX-XXXX`) is derived from it and is also the friend code. The hub checks the id against the token, stores only a hash of the
token (`eck-token:`, `eck-player-id:` derivation) and refuses a known id with another token.

### Rooms

The host is seat 0; each join takes the lowest free seat; a client never chooses. Capacity is 2, 3 or 4. `room.host` creates a
room (from the host of an open room it is a refresh), `room.search` lists friends' compatible rooms, `room.join` and `invite.accept`
seat a player, `room.leave` frees a seat (the host leaving closes the room), `room.cancel` closes it, `room.start` freezes the seats:
no new joins, no reassignment, all invites end. A room is `open`, then `starting`, then `racing` when every seat is ready.

A lost connection holds its seat for 30 s and only that player can resume it. If the hold runs out a joiner's seat is freed, or left
empty after the start, and the others are told; a host's closes the room.

### Friends and invites

`friends.list`, `.requests`, `.add`, `.accept`, `.decline`, `.remove`. A friend's state is `offline`, `online`, `hosting`,
`connecting` or `in_game`, with platform, version, image and build, and whether they are compatible with the viewer.

The host of an open room can invite up to one friend per free seat at once (`invite.send`; `invite.accept` joins that room, never a
new one; `invite.decline`). Invites end as `declined`, `accepted`, `expired` (60 s), `room_full`, `host_left`, `room_started`,
`room_closed` or `cancelled`.

### Direct connections

Direct UDP is always preferred; this service only signals. Each client publishes its gameplay socket with `match.endpoint` (local
port, LAN addresses, STUN-mapped public address, UPnP mapping) and receives every other seat's with `match.peer`, where `ip` is the
address the service itself saw. LAN candidates reach only a seat on the same public network (same IPv4 address, or same IPv6 /64). A
public address that is not the connection's own is dropped on IPv4; on IPv6 it cannot be compared and passes as a claim. Either way
a client only uses an address after a punch acknowledgement keyed by the room.

The race is host-authoritative, a star around seat 0. For each peer the host probes direct first and chooses with `match.transport`
(`direct`, or `relay` after the probe window); the peer gets the same event and follows it, so both ends always agree. Each seat has
its own path, so a room can be mixed (P2 direct, P3 relay, P4 direct) and one seat needing the relay moves nobody else to it.

### Relay

`relay: true` in `hello` advertises the capability; a room whose host has it gets a relay token, given only to clients that have it
too. There is no user-facing switch. Binary frames (`PROTOCOL.md`, section 7): `E`, `K`, version, type, room token, sequence, destination
seat or `0xFF` for all, source seat, length, payload of 1 to 1472 bytes.

The sender's room and seat come from its socket, never from the frame. The service overwrites the source seat, delivers only inside
that room, and rejects bad magic, version, type, token, length, size or destination. Payloads are never parsed, logged or stored.

### Metrics

Stored per day (America/Sao_Paulo), in `metrics_daily`: `sessions`, `rooms_created`, `rooms_started`, `joins`, `invites`,
`incompatible_rejects`, `direct_selected`, `relay_selected`, `rooms_closed`, `rooms_with_guest`, `rooms_direct`, `rooms_relay`,
`rooms_pending`, `peak_online`, `peak_in_rooms`, `peak_rooms`, `races_started`, `races_completed`, `races_abandoned`,
`rooms_with_race`, `rooms_rematched` (2 or more races), `race_duration_ms` (sum, completed races only) and `races_in_rooms` (sum).
In `metrics_daily_dimensions`: `platform`, `version`, `country`, `reject`, `transport`, `room_capacity`, `room_occupancy`
(`2/2`, `2/3`, `3/3`, `2/4`, `3/4`, `4/4`), `room_lifetime`, `abuse`, `competition`, `player_count` (`1` to `4`), `race_transport`
(completed races only), `race_duration_bucket` (`under_2m`, `2_4m`, `4_6m`, `6_10m`, `over_10m`), `races_per_room` (`1`, `2`, `3`,
`4+`, written once a room closes with at least one race). In `metrics_samples`: one row per 15 minutes (online, in rooms, rooms),
kept 90 days. In `recent_rooms`: the latest 50 closed rooms that had a guest, 30 days at most, with end time, duration, capacity, peak
occupancy, transport, platform categories and final state.

A room counts as direct when it used only direct paths, relay when any seat used the relay, pending when it had a guest and no path
was chosen. A session is a presence connection; a reconnect within 10 minutes is not a new one. Platform, version and country collapse
to `Other` or `Unknown` when unknown, a dimension takes at most 64 values a day and the rest count as `Other`. Country is Cloudflare's,
set at the edge; a client's copy of the header is replaced.

Never stored in any of these: name, friend code, player id, address, room id, relay token, private token, relay payload.

### Race telemetry

Wire format: PROTOCOL.md section 8. The game reports its own races with no player action, on the connection it already has; with
Online play off, or the service not signed in, a race is simply not reported, and telemetry never opens a connection.

The client (`src/online_race_telemetry.*`) arms on the race URL `KartProgressManager_Base.LoadRace` hands to `ConsoleCommand`, sends
`race.start` when `RaceState` reaches InProgress in a world it saw opening, and `race.complete` when it reaches Finished. It leaves out
what it cannot prove: `track` comes from the map name, because `NumSelectedTrack` is the global track in Exhibition only; `places` are
sent only when every human's kart was found by `ControllerId`, carries the character the URL gave it, and has a place of its own.
Capture the Flag has team places only, so it sends none. The cup podium and the tutorials are not races.

In a started room only the host reports, and the service checks the seats. Without a room the race is this PC's own, solo or
split-screen, and a player count is the client's word: nothing else exists to check it against.

A race ends as abandoned when its room closes, its connection ends, or a new race starts first; each path counts it once. Duration is
clamped to what the service measured. Transport is what the seats ride on as the race ends.

Stored per day, all fixed ids and counts: `metrics_daily_player_counts` (races, completed, abandoned, duration, direct, relay, mixed
by player count), `metrics_daily_race_outcomes` (the same for each competition and track), `metrics_daily_characters` (picks, races,
completed), `metrics_daily_finish_positions`, `metrics_daily_character_pairs`, `metrics_daily_compositions` and
`metrics_daily_race_cross` (competition by players, track by players, track by competition, character by competition, character by
track). Bounds: 12 characters, 20 tracks, 9 competitions, 1 to 4 players.

Definitions:

- A pick is a seat; a character's races count each race once.
- A pair counts once per race for each two distinct seats' characters. Two seats on one character make a pair with itself. The game's
  local character screen refuses a character another panel holds, so that needs a room with its own selection.
- A composition keeps every seat, sorted by id: `0+0+1` is not `0+1`.
- A finish position is the rank among the humans of a completed race, from `places`. A solo race has none. A win is rank 1, a podium is
  any rank but last and at most third, last place is the worst rank of that race. These compare humans, so a bot may have finished ahead.

## Abuse protection

None of this changes what a normal player sees. Refusals are counted in memory and cost no write.

- **Edge**: `WS_UPGRADE_LIMITER` allows 30 WebSocket upgrades per address group per 60 s (IPv4 by address, IPv6 by /64). A refused
  upgrade is an HTTP 429 with `Retry-After: 60` and never reaches the Durable Object. Without the binding, or if it fails, upgrades
  pass and the limits below remain.
- **Connections**: 16 sockets per address group; 4 KB text frames; 60 messages burst then 20/s per socket, and 10 burst then 1/s for
  `friends.add`, `room.host`, `room.join`, `invite.send`, `invite.accept`, `match.resume`.
- **Sign-in**: 12 `hello` per player burst, then one per 15 s, counted only after the token matched, so guessing cannot lock anyone out.
  20 new players per address per day.
- **Rooms**: creating a room allows 6 per player burst, one per 20 s, and 20 per address burst, one per 6 s. `room.join` and
  `invite.accept` 20 burst, one per 5 s per player. `match.resume` 12 burst, one per 10 s.
- **Invites**: 20 per player burst, one per 3 s, and 60 per address burst, one per 2 s; at most one per free seat. Limits apply after
  validation, so a failed join, host or invite spends nothing.
- **Race telemetry**: 30 per player burst, one per 3 s, and 40 per room burst, one per 2 s (per player again, for a solo race). A
  normal room's rematches stay well inside this; it only refuses the kind of spam no player triggers by racing.
- **Bounds**: 16-character names, versions and platforms, 200 friends, 50 pending requests each way, 20 search results, 4 LAN addresses,
  6 candidates, 16 rule fields. Rate-limit maps keep 4096 keys.
- **Relay, per connection**: 400 frames/s (burst 800) and 128 KB/s (burst 256 KB), charged once per frame received.
- **Relay, per room**: the connection budget times the room's capacity, charged once per delivery, so fan-out costs what it costs the
  object. 2 seats: 800 deliveries/s, 256 KB/s (bursts 1600, 512 KB). 3 seats: 1200/s, 384 KB/s (2400, 768 KB). 4 seats: 1600/s, 512 KB/s
  (3200, 1 MB). Budgets survive a reconnect. 256 dropped frames end the room's relay.
- **Invalid relay frames**: a sender is allowed 200, then 5 per second, then closed with 1008. A seat that is away (grace, resuming), or a
  relay that closed while one is, is not a fault.
- **Backpressure**: a seat with more than 256 KB queued loses its frames while the others keep theirs (`relay_backpressure_drop`); 1024
  drops in a row end the relay. This relies on the runtime reporting `bufferedAmount`; where it does not, the rate limits are the bound.
- **Writes**: unchanged player rows, room refreshes (30 s window), room settings, endpoints, ready flags and transport choices are not
  written; a repeated search keeps its handles; relay frames and presence never write. Rejections are written once per reason and
  address per 10 minutes.
- **Alarms**: an alarm already set for the same time or earlier is not set again, and a woken object reads its stored alarm. Alarms expire
  rooms, invites and grace, flush diagnostics, sample usage, and sweep old requests, registrations, handles, samples and recent rooms.
- **Diagnostics**: `ws_upgrade_accepted`, `ws_upgrade_limited`, `ws_upgrade_ip_cap`, `hello_accepted`, `hello_reconnect_deduped`,
  `hello_limited`, `room_host_accepted`, `room_host_limited`, `room_join_limited`, `invite_limited`, `room_resume_limited`,
  `relay_abuse_closed`, `relay_backpressure_drop`, `race_limited` are kept in memory and written at the 15-minute alarm. They hold counter names only, and a
  counter not yet written is lost if the object is evicted.

All rooms share the one Durable Object, so relayed traffic counts against its CPU and request budget.

## Admin dashboard

`GET /admin` serves a static, read-only page; `/admin/app.css` and `/admin/app.js` are its assets. Its data comes from `/admin/api/summary`
and `/admin/api/history?days=N&recent=M` (days 1 to 365, recent 1 to 50), which need `Authorization: Bearer <ECK_DASHBOARD_TOKEN>`. The token
is a Worker secret of 16 or more characters, compared by digest; without it the API answers 503. Nothing can be changed from the dashboard.
Answers carry `no-store`, `nosniff`, `no-referrer`, `DENY` framing, same-origin isolation and a strict Content-Security-Policy.

It shows, now: players online, in rooms, rooms advertised, active rooms, Direct, Relay and Pending seats, rooms by size, occupied and maximum
seats, average occupancy. Then today's and the selected range's totals, a comparison with the previous period of the same length, current
rooms (short id only, read live), last rooms, usage and rooms over time, breakdowns by room size, occupancy, lifetime, platform, version,
country and transport, daily totals, and the rejection and protection counters.

## Deploy (manual; nothing in this repository deploys)

```
cd server/eck-lobby
npx wrangler whoami                            # must be the account that owns saulobenigno.workers.dev
npx wrangler secret put ECK_DASHBOARD_TOKEN    # any secret of 16+ characters, used by nothing else
npx wrangler deploy
curl https://eck-lobby.saulobenigno.workers.dev/health
```

`wrangler.toml`: Worker `eck-lobby`, Durable Object class `ECKLobby` bound as `LOBBY`, migration `v1` (`new_sqlite_classes`), rate limit
`WS_UPGRADE_LIMITER`. Rate-limit `namespace_id` values are per account, so `4004` must stay unique among this account's Workers: another
service with the same id would share its counters. Never edit a migration that has been applied; never reuse another service's Worker
name, class, namespace or secret. The tables are created by the hub on first use.

## Tests and development

```
cd server/eck-lobby
npm test            # node --test "test/*.test.js"; no dependencies, no Cloudflare account, Node 22.15 or newer
```

`test/worker.test.js` runs the real Worker and Durable Object class against a stand-in runtime; the rest drive the hub in process with a
movable clock, and `test/server.test.js` over real WebSockets on `local/server.js`.

`ECK_LOBBY=ws://127.0.0.1:8791/` in a game's environment overrides the service for development, against `node local/server.js`
(loopback clients are exempt from the per-address limits) or `npm run dev` (`wrangler dev --local`).
