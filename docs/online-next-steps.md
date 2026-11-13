# Online: what is left, in order

State when this was written: branch `feat/online-race-state-sync`, 19 commits, clean tree, nothing pushed. Every suite green
(lobby 999, session 1271, sync 218, race 684, probe 145, channel 32), the game builds.

Repository under work: `C:\Users\joaop\el-chavo-kart-recompiled` (build + game files). A second copy that is only source and
tests: `C:\Users\joaop\.t3\scratch\<session>\repo`. Patches are applied to the second and copied to the first.

## Verified working (live, two and three instances)

- Sign-in, friends, invite, accept, seats 0..2 in one session, `transport=direct`.
- Character pick synchronized: the guest's choice shows on the host's panel.
- The host publishes a race with `NumHumanKarts=N`; every guest travels into that same race.
- Three players, 2 minutes: each completed a lap, no step backwards on any kart, matching items and places.
- Two players, 5 minutes: same. Host alone: 2 laps.

## Task A - every PC must show the same players on the same panels

Symptom: the character screen arranges the players differently on each PC (Mr. Raymond on panel 1 on one window, on panel 2 on
another). The race itself is consistent because the URL's `CharArray` is per slot; the screens are not.

Cause: `Panels` is indexed by pad (`src/online_lobby.h`, `using Panels = std::array<PanelView, kPanels>`), and the character
screen maps a seat to a panel by identity:

```cpp
inline constexpr std::array<int, kPanels> kPanelOfSeat = {0, 1, 2, 3};
inline constexpr std::array<int, kPanels> kSeatOfPanel = {0, 1, 2, 3};
```

while the race URL maps a seat to a pad per PC (`src/online_race_start.h`):

```cpp
inline int PadOfSeat(const CanonicalRace& race, int seat, int local_seat, int own_pad)
```

which puts the local player on its own pad and the others on the remaining pads in seat order. Each PC has one physical pad,
so the local player always lands on pad 0 and the others shift: two different views of the same room.

Fix: the character screen uses the same mapping as the race URL. Concretely:

- `LobbyPads::Step` resolves a seat to a panel through `PadOfSeat(race, seat, local_seat, physical_pad)` instead of
  `kPanelOfSeat`; the same for `kSeatOfPanel` when a panel has to be named back.
- `CanonicalPadSeats` (`src/online_lobby.h`) is the function that already exists for the race, so the screen and the race share
  one table instead of two.
- `kPanelOfSeat`/`kSeatOfPanel` stay only for "no session" (the game's own layout).
- Tests in `tests/online_lobby_tests.cpp`: the panel a seat drives is the same on the host and on a guest, and it matches the
  pad the race URL would give that seat. `TestCanonicalPanels`, `TestPadRules` and `TestPlan` cover the current behaviour and
  will need to say the new one.

Acceptance: three windows side by side show the same character on the same panel, and the race still starts with every seat
in its slot.

## Task B - measure the start

`127d901` changed `StartGate::NoteRaceState` to fire when the countdown **starts** (`state < kRaceCinematic` returns false),
so the guests press START at the countdown start instead of after the host has already gone. Not measured live yet.

Run `spread.ps1` (in the session scratchpad; recreate it if gone): it presses START on the host, holds throttle on all three,
then samples every player's own `distance` once a second and prints the spread between them. Aligned start = small spread in
the first second and all three moving in the same sample; the old behaviour showed the host hundreds of thousands of units
ahead after a few seconds.

## Task C - a shared start instant, and the OK panel

Design agreed with the user:

1. Pressing A on a corner panel marks that player OK (`match.ready`, which already exists in the protocol; the service moves
   the room `starting` -> `racing` when every seat is ready). The panel lists every seat and its state.
2. The host publishes `start_at = host clock + 2 s` in the room rules (`RoomRules`, `src/online/lobby` helper style, see
   `AfterRaceRules`/`SetupRules` in `src/online_lobby.h`), so no new wire field is needed.
3. Every PC converts `start_at` to its own clock with the existing `HostClock` (slew limited, never backwards) and presses the
   gate then, host included. The game's own 3-2-1 then runs at the same instant everywhere, which is what makes the karts
   leave together.
4. If not everyone is OK, the host publishes the same `start_at` when a 45 s deadline expires, so nobody is blocked.

Order: 2 and 3 first (that is the desync), then 1 and 4. The panel is a `ModalDialog`-free overlay draw in
`src/online_menu_overlay.cpp`, which already draws the multiplayer menu.

## How to test (the whole harness)

```
# one lobby for the run; its database is in memory, so friends are lost when it restarts
node server/eck-lobby/local/server.js --port 8791      # or Start-Process with -RedirectStandardOutput to see room closes

# three instances, each its own user folder, udp port and control port
tools\online-instance.ps1 -Name a -Port 7101 -Lobby ws://127.0.0.1:8791/    # control 8799
tools\online-instance.ps1 -Name b -Port 7102 -Lobby ws://127.0.0.1:8791/    # control 8800
tools\online-instance.ps1 -Name c -Port 7103 -Lobby ws://127.0.0.1:8791/    # control 8801
```

`race3p.ps1` does the whole setup by itself: settings (no launcher, no intro, online on, connection test satisfied), launch,
wait for sign-in, friend codes from the identity files, friends both ways, the three character screens, invite/accept,
picks, host confirms and presses START, track, publish, then prints the three states. `drive.ps1 -Samples N` presses START on
the host, holds throttle on all three and samples distance, reporting per player: samples, distance range, laps, place and
backward steps.

Channel verbs (`tools/test-client.py`, one port per instance via `ECK_TEST_CONTROL_PORT`): `state`, `console`, `race <map>`,
`pad <buttons> <lx> <ly> <rt> <lt>`, `shot <file.bmp>`, `match host/accept/start/leave`, `friend add <code>`,
`pick <0|1|2> [character]`, `reset`, `help`. Button values: 0x10 Start, 0x1000 A, 0x2000 B, 0x2 down.

## Lessons that cost time (do not repeat)

- A guest's START opens the port's multiplayer menu, where the next A is "Leave Multiplayer". Never press START on a guest; the
  host's race releases it.
- `distance` resets when a lap completes: a backwards step that coincides with a lap increase is not a teleport.
- The room closes for a reason that the local service can name: log it in `hub.js` `closeRoom` (one console.log was used) and
  run the lobby with output redirected.
- The port used to give a session up under a race (`WatchSession` -> `AbandonSession`); `8bdd436` stops that while a race runs.
- Editing these files: they are CRLF. Read, normalise, patch, restore the end-of-line, or half the replacements fail. Prefer
  `git checkout -- <file>` before re-patching.
- The game executable cannot be rebuilt while an instance runs (`permission denied` on the exe).
- Diagnostics in the tree: `[ONLINE-RACE] guest holding`, `[ONLINE] race kept`, `[ONLINE-RACE] race travel abandoned`.

## Done means

Three instances reach the finish of a full race together, none teleporting, with the character screen showing the same
arrangement on all three and every kart leaving the gate in the same sample.
