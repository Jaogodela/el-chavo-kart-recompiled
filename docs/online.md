# Online play

Rooms of 2 to 4 players over the internet. Players meet through a small online service (`server/eck-lobby`); the race
itself runs directly between the PCs over UDP, and only a player pair that cannot reach each other is relayed through the
service. The wire format is [`server/eck-lobby/PROTOCOL.md`](../server/eck-lobby/PROTOCOL.md) (protocol version 1, game id
`el-chavo-kart-pc`).

## Status

Implemented and unit-tested here: the client stack in `src/online` (identity, protocol, service connection, friends,
rooms, invites, per-seat UDP path setup, relay frames, reconnect with seat resume), the launcher **Online** page and the
connection test. Not done yet: the game-side hooks that put rooms into the game's own menus (Create Room, Find Room,
Invite Friends, Start Race, Leave Room) and carry the race traffic over the gameplay socket. Until those land, the
launcher can sign in, manage friends and run the connection test, but a race cannot be started online. No public service is
deployed: there is no default service address.

## Playing (launcher)

1. Launcher > **Online** > **Online play** On. The rest of the page appears only then.
2. **Online name** is what other players see: up to 16 letters, digits, `-` or `_` (the field accepts nothing else). The
   local player names under Gameplay are separate and unchanged.
3. Share **Your friend code** (`XXXX-XXXX`; **Copy** puts exactly that on the clipboard). Paste or type a friend's code
   under **Friends** and press **Send request**; they **Accept** or **Decline** it under **Friend requests**.
4. **Connection test** signs in to the service, asks two STUN servers which public address and port the gameplay UDP socket
   gets, and asks the router to open the port by UPnP (automatic, no settings). The verdict is `Ready for online play`,
   `Limited: ...` or `Not ready: ...`, with the lines Online service, Direct connections, Router configuration and, when
   the service offers its relay, Relay. With Online play on, **Play** waits until one test has finished (whatever its
   result); the test is remembered while Online play stays on and forgotten when it is switched off.
5. The in-game ping panel reads `Ping: 38 / 51 / 44 ms`: one value per connected peer, lowest seat first, `--` until a
   round trip is measured. It has no hotkey: F3 is reserved by the game's own bindings.

Both players must run the same game image (`eck-usa-1db4e96b`) and build number; a friend on another build shows
`Version mismatch` and cannot be joined or invited.

Settings: `eck_online_enabled`, `eck_online_name` and `eck_online_connection_test` (written by the launcher) in
`settings.toml`.

## Service address

The game connects to the production service, `wss://eck-lobby.saulobenigno.workers.dev` (`kDefaultServiceUrl`), with nothing to
set up: launch the game and turn Online play on. The service and how it is deployed are in
[`server/eck-lobby/README.md`](../server/eck-lobby/README.md).

Developer overrides: `ECK_LOBBY` (another service, `ws://` or `wss://`), `ECK_STUN` (`host:port,host:port` or `off`), `ECK_UPNP`
(`0` to turn UPnP off) and `ECK_ONLINE_PORT` (one fixed gameplay UDP port).

## Identity

The first time Online play connects, 32 random bytes are written as a 64-character hex token to
`eck_online_identity.ini` in the user data folder. The token is private: it is only ever sent to the service, never
shown, logged or sent to other players. The public friend code is derived from it
(`sha256("eck-player-id:" + token)`, 8 characters of an alphabet without I, O, 0 and 1); the service stores only
`sha256("eck-token:" + token)`. Anyone with the file can play as you; deleting it makes a new identity. A second process on
the same user folder cannot share the identity: it plays under a temporary one with a `-2` name suffix.

## Architecture

- `online_service`: one WebSocket to the service. Signs in, keeps friends and presence, hosts, searches, joins, leaves and
  starts rooms, sends and answers invites, publishes this client's endpoint, reports ready and transport, and after a lost
  connection signs in again and resumes the held seat (the service holds it for 30 s).
- Seats are assigned by the service: the host is seat 0, a join takes the lowest free seat, and the seat number is the
  in-game player slot. The client never picks one.
- `game_socket`: one UDP socket for the whole process, bound early (the first free port of 7101 to 7110, chosen so it
  does not collide with other recompiled titles that use 7001 and 7002) and reused by STUN, hole punching and the race, so
  the NAT mapping the peers were told about stays valid. A relayed seat is addressed as a loopback endpoint that carries
  its seat number, so the game talks to it like any other peer.
- `rendezvous`: per room, per peer seat. It learns the socket's public endpoint from STUN, then punches each peer's
  candidates (LAN only when both are on one network, then STUN/UPnP, then the address the service observed). Only a punch
  acknowledgement selects an address for a seat.
- `peer_table`: the per-seat path choice, pure and unit-tested. The host decides its path to each peer: direct as soon as a
  punch is acknowledged, otherwise the relay once the probe window (2.5 s, at least 1.5 s of punching) is over and both
  ends offer the relay; it reports the choice with `match.transport` and the peer follows. Two non-host seats decide
  between themselves. Without a relay a seat that cannot be reached fails. A room has 15 s to establish every path once the
  host presses Start Race.
- Relay: binary frames of a 16-byte header (`E`, `K`, version, type, room token, sequence, destination seat, source seat,
  length) and up to 1472 payload bytes, routed by destination seat; the service stamps the real source seat.
- Logs carry no private token and no full addresses (only the first octet and the port).

Public API for the game side: `Online()` (`src/online_runtime.h`) is the process-wide `eck::online::OnlineService`;
`GameSocket()` is the shared socket; `PumpOnlineService()` runs once per presented frame while the game runs.
`OnlineService` offers `CreateRoom`, `UpdateRoom`, `SearchRooms`, `JoinRoom`, `StartRace`, `SetReady`, `LeaveRoom`,
`CancelRoom`, `SendInvite`, `AcceptInvite`, `DeclineInvite`, `snapshot()`, `GamePeers()`, `SeatOfEndpoint()` and
`SetPeerRtt()`.

## Several instances on one PC

`tools\online-instance.ps1` starts one isolated instance: its own user folder (settings, identity, saves), log file and
gameplay UDP port, under `build\online-instances\<name>`. It starts nothing unless you run it; `-PrepareOnly` only creates
the folders and prints where everything is.

```
tools\online-instance.ps1 -Name a    # port 7101
tools\online-instance.ps1 -Name b    # port 7102
tools\online-instance.ps1 -Name c    # port 7103, a third player
tools\online-instance.ps1 -Name d    # port 7104, a fourth player
```

The instance letters a to j map to ports 7101 to 7110; any other name needs `-Port`. Switch Online play on from each
instance's own Online page. Every instance makes its own friend code, so add the instances to each other as friends.

## Tests

`tools\test.ps1 -Filter online` builds and runs `eck_online_identity_tests`, `eck_online_protocol_tests`,
`eck_online_peer_tests`, `eck_online_service_tests` and `eck_online_launcher_tests`: no network, no window, no game files.
The service tests drive `OnlineService` against a scripted connection; the service's own tests are in
`server/eck-lobby/test` (`npm test`).
