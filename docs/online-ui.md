# Online in the game's menus

The original game has local multiplayer for 1 to 4 players and no online mode, so there is no original online menu to
reproduce. Online adds friend invitations to the original Free Race flow and nothing else. The one movie change is the
in-memory main-menu patch of `docs/main-menu.md`.

## Flow

```
Online play Off: the original game. No shortcut, no session, no network call, no overlay.

Online play On:
  Main Menu -> Free Race -> competition -> difficulty -> character screen   (all original)
  Character screen (ChavoKart_PlayerSelectManager): the small hint "X  Invite Player"
    X -> friend picker: only a list, nothing is created and B leaves nothing behind
    Select a friend -> the session is created if there is none, seat 0 is confirmed, the invite goes out, and the picker closes
    once the service took it. A failure stays on the picker with the reason, and the friend can be tried again
    Repeat while seats + invites the service took < 4
Incoming invite "<Name> invited you to a Multiplayer race": Accept / Decline
  Accept joins that host's hidden session in the seat the service gives, then the game travels to the same character screen.
  A session of their own with nobody in it ends first
  Decline leaves the player where they were
```

The character screen is the lobby and the game's four panels are the players, so there is no list of seats: the hint panel adds
only `Carlos - Invited`, `Connecting`, `Direct` or `Relay`, one line per real friend. The
hidden session (a `room` on the wire) is created by the first invite sent, always with 4 seats.

`OnlineService::SendInvite` is one transaction at a time. Without a room it asks for one and sends the invite when seat 0 is
confirmed, so the invite never outruns its session. Only the service's answer holds a seat: `SentInvite` exists from then on, and
goes on a refusal, a decline, an expiry or the friend disconnecting. Anything that fails on the way reaches `invite_outcome`.

Accepting while alone in an own session is a role change, not an update. `AbandonOwnSession` sends `room.leave` and clears the room,
its invites, peers and relay before `invite.accept` goes out, on the same socket, so the join starts from a player with no room. The
service's `enter` would close that room too; doing it here means a refused join leaves nothing half-replaced.
`session_generation` grows with every room entered or left. The game side resets its pads and drops the local pick when it moves,
and the overlay drops any race or Waiting for host state. An old room's events are refused by room id, and the answers to its
requests by the room they were sent from. Joining always travels, so the guest's panels are the game's own fresh ones.

Three systems, each usable without the next: the screen (`src/online_character_select.h`), Invite Player
(`src/online_menu.h`), and remote panels (`src/online_lobby.h`, `src/online_panel_bridge.h`). With Online play on, the hint needs
only the screen, and is hidden only for a guest or a full session. Before sign-in X says `Connecting to online service`. If the
panels cannot be found, invites and the network still work and the log says `remote_panel_bridge=unavailable`.

`src/online_menu_overlay.cpp` draws it and follows the session on the UI thread, and `src/online_game_hook.cpp` touches the game on
the game thread. The picker and
the prompt are `ModalDialog`s, the base of the Achievements overlay, adapted from SSF2T's `invite_ui.cpp`. A row is activated by
`Input()` or by the mouse, never by ImGui's own navigation as well.

## The character screen and X

`sub_829B1990` is `UGFxMoviePlayer::execActionScriptVoid`, registered at `0x83BE46F0`. Every screen goes through it, and the
method is a literal in the bytecode at `FFrame.Code` (`EX_StringConst`), so reading it consumes nothing:

- `KartScreen_Base.ShowScreen` and `HideScreen` give the screen on top. The object's class says which: a class is tied to a
  screen at the first `ShowScreen` after its movie opens, and a class lasts the whole run.
- `XboxTypeS_X` is bound to `OnPressXButton`, which `KartPC_Base` routes to the top screen only. X therefore takes the
  game's own input path and focus rule, and only reaches the hook on the instance that has the input. Keyboard X and Y do
  the same through ImGui, which only the focused window feeds.

With Online play off the hook does nothing.

## The host's game

`SetupCapture` (`src/online_setup_capture.h`) records the game as it is chosen, from the calls the menu movies make through
`ExternalInterface.call`, all of which reach `sub_829A1FA0`, the game's handler:

| Movie call | Effect |
| --- | --- |
| `SelectMenu("Exhibition")` | a new Free Race, a new generation, nothing chosen yet |
| `SelectMenu(anything else)`, the main menu showing again | the setup is gone, so a Cup or a Challenge cannot reuse it |
| `SelectCompetitionType(n)` | the mode; the difficulty has to be chosen again |
| `SetAIDifficulty(n)` | the difficulty |

CompetitionSelect shows again when the difficulty popup closes, so only coming back to it from the character screen clears the
choices. The first X publishes the setup in the session's `rules`: `setup` (version), `game_type`, `mode`, `difficulty`. The track
does not exist yet. An incomplete setup logs the event that is missing, and the picker opens with `Game setup unavailable` and
cannot send, because a guest would have nothing to enter. That includes the character screen reached by coming back from a local
race, where the game walks its menus by itself; going back to CompetitionSelect and choosing again completes it.

## A guest entering

`LobbyFlow` runs one path. After the join it validates the rules, then runs this on the game thread, after the engine tick, with
`AActor::ConsoleCommand` on `WorldInfo.Game`:

```
start StartMenu?GoBackType=2?NumGameMode=<mode>?KartAIDiffculty=<difficulty>?GameType=<type>?PrimaryControllerID=<pad>
```

That is the URL of the game's own `KartScreen_EndRace.BackToCharacterSelect`. `KartGame_Menu` reloads the menu map and walks
main menu, competition and character screens by itself (`OnMainMenuInitComplete`, `OnGameModeInitComplete`,
`SelectCompetitionType`), so the objects are the ones the game built. `start` is the game's default (`bUseOpenInsteadOfStart`
is off), and the original's loading screen, `StopAllSounds` and `ClearDelegates` are not part of this path. A guest in a race is
not prompted until the menus are back, so live gameplay is never torn down. A session without a valid setup is left, not entered.

## Panels

`KartGame_Menu.SetupLocalPlayers` always makes four `LocalPlayer`s, and `KartPC_Base` sends a pad's buttons to
`KartScreen_PlayerSelectManager`, which gives them to the panel with that `ControlledById`. So a remote seat is a pad: the
`XamInputGetState` stub (`sub_830D9E00`) answers for that controller index, focused or not, and `SetKartURLData` sees an ordinary
confirmed panel. `PanelPad` reads the panel and presses what moves it toward the seat's pick. Nothing is called in the game and no
UI is faked.

The panels come from the screen hook: the manager shows each one, in pad order, so `ScreenTracker` has the four objects.
`ScanPanels` confirms the layout on them: the word where each panel holds a different pad next to a pointer to `WorldInfo.Game`,
then `ScreenParent`, `playerSelectState` and `selectedPlayer` at their distances from it.

| | |
| --- | --- |
| Panels | seat N is panel N on every PC: Player 1 (top left) is the host, then top right, bottom left and bottom right |
| Own panel | the seat's: `XamInputGetState` answers that index from the physical pad, whatever number that pad has |
| Other indices | virtual pads of the other seats, and a neutral one for an empty seat, so a session takes no other local pad |
| Primary controller | never changed: profile, saves and achievements hang off `PrimaryControllerId`, so a guest's game still opens panel 0 itself, which is the host's. The guest's own panel opens with one injected Start |
| No session | every pad and panel is the original game's |

The game shows no character twice, so a panel waits while a local panel or a settled remote panel holds its target, and the
game's `IsReadyToGo` keeps the host on the screen until every panel is confirmed or closed. Which characters a player can pick
comes from that player's own save and Unlock secret characters option. A remote panel is never locked on the PC showing it, and
the black preview paint is skipped during a session: see `docs/launcher-graphics-gameplay.md`.

Picks are `match.pick` (`server/eck-lobby/PROTOCOL.md`): the service writes only the sender's seat, and every `room.updated`
carries all of them. They need no UDP path, so Direct and Relay are untouched.

A guest's pad rules: A and Start are masked on a confirmed panel, because the track is the host's. `match.pick` carries no seat, as
the service writes the sender's, so a pick shows on that seat's panel everywhere.

The race reads the physical pad as itself: only the character screen is by seat. So the URL's pads are panels on the host, which
`CanonicalPadSeats` reads as seats, and a host whose pad is not pad 0 releases `RaceCommand` instead of the game's own URL.

## Leaving

B on a closed own panel means leaving, because the game cannot leave the screen while another panel is open.

- Guest: leaves the session and travels with `GoBackType=0`. Same when the session ends under them.
- Host: leaves the session, the remote panels close by themselves, and the last one closing with the host's own panel closed
  makes the game go back to CompetitionSelect. CompetitionSelect or the main menu also end the session.

In a race and past it, Start on a guest and Back on the host open the multiplayer menu (Resume, Leave Multiplayer). Past the finish
Start does on the host too, as it pauses nothing there. It is a `ModalDialog`, so
the game reads no local input while it is open and until what was pressed is released, and nothing pauses. Start stays the
host's pause. A guest's Start also opens it at the race gate and when confirmed on the character screen.

| What happened | How this PC learns | What it does |
| --- | --- | --- |
| Host leaves | `room.closed` reason `host_left` | Guests go to the main menu: Host left |
| Host's game goes back to its menus | `LobbyFlow`: raced, in the menus, and no step of `room.reopen` | Leaves, which is the line above for the guests |
| Host's window closes | `room.leave` sent at exit, 200 ms to flush | The same |
| Host's process dies | `room.updated` seat 0 `reconnecting` and no snapshot for 15 s, or `room.closed` reason `disconnect` at 30 s | Host left |
| Guest leaves or dies | `room.updated` with the seat `empty` | Host and other guests stay. The kart's pad goes neutral after 0.25 s |
| This PC loses the service | `reconnecting` and no snapshot for 15 s, or the 30 s hold | Connection lost |
| Host seated, service up, no snapshot | `WatchRace`, 45 s | Connection lost |

The service holds a closed socket's seat for 30 s, and only `room.closed` or a seat going `empty` are final. `ready` and short
silence say nothing, as both change in a healthy session. `closed_reason` is `connection_lost` for this side's own loss, and
empty for a leave of its own, which tells the player nothing. `kMainMenu` is the one travel retried until a game exists, so a
session that ends during a load still gets home. The note waits for the main menu before it starts counting.

## After the race

The room stays as it is through the finish: nothing about `RaceState` 4 ends it. Each PC has one `KartScreen_HUDIndividual` per
human, and `KartScreen_HUDManager` moves on only when every one of them is in `FinishHudState` 3. Continue is A (2 to 3), Back is B
(3 to 2, inside the same screen), and each takes only its own pad. So nobody could finish the remote seat's HUD: not the host, whose
guest may be gone, and not a guest, whose host's pad is neutral.

- **Host:** remote seats' pads repeat the host's A and B on the finish HUD, up to the moment `ChavoKart_ResultsScreen` or a later
  movie opens. From there it is `KartScreen_RaceResults` and `KartScreen_EndRace`, as in a local game. A remote guest's A reaches none
  of this. Start and Back open the multiplayer menu.
- **Guest:** post-race when its own game or the host's snapshot says so. A, B and Back are closed, Waiting for host shows, and Start
  opens the multiplayer menu. It never walks the result screens: it waits on its finish HUD for the host's step.
- **The step:** the host's own game names the destination, in the `start StartMenu?...GoBackType=` of `LoadTheMainMenu`, for the
  pause menu as well. `room.reopen` puts the room back to `open` with `after_race` (a generation, so a repeat or an old step does
  nothing) and `after_dest`. Back to the character screen is GoBackType 2, the track screen 3, each followed by the guests with the
  same type. Restart and Next Race are held as any race start, and the guests stay on their results until it is published.
  GoBackType 0 and 1 leave this game, so they end the session and the guests are told the host left.

With a service that has no `capabilities.room_reopen`, or a refused step, the host leaves: nobody is left waiting.

## Starting the race

Only seat 0 starts, the same ownership as SSF2T, and nothing about it ends the session: only a real leave or a lost connection
does. `src/online_race_start.h` holds the pure part.

1. The game's `start <map>?...` reaches the `ConsoleCommand` hook. With a guest seated it is held; alone, it runs as a local race.
2. `CanonicalOfUrl` turns that final URL into validated numbers, and `RaceRules` adds them to the room's `rules` (`room.update`).
3. `room.start` freezes the seats (`starting`), every seat's `match.ready` moves the room to `racing`, and every PC travels on
   `racing`: the host runs its held command, a guest runs `RaceCommand`. The room is the shared clock, so guests have no timer.
4. A failure names itself and never becomes a local race: `race_payload_invalid`, `race_transition_timeout` (15 s, as SSF2T's
   establishment budget), `peer_lost`. The session ends and the host goes back to the character screen. The service has one
   close reason for a host leaving, so a guest sees that one.

Kart slots are the same on every PC, humans first in seat order. What differs is which pad holds a slot:
`SetupLocalPlayers` gives `GamePlayers[slot]` the id in `PlayersArray[slot]`, and `SpawnDefaultPawnFor` finds the character by
that id. So a guest loads the same race with `PlayersArray=1-0-99-...`: slot 0 is a pad nobody holds and slot 1 is the guest's own.
`CharArray` does not change, and `PrimaryControllerID` is the guest's own pad.

Each PC therefore has one local player per human, so the game draws its split screen.

## Racing

Seat 0 simulates the race; `src/online_race_sync.h` is the pure part and `src/online_race_sync_guest.cpp` touches the game. Kart
physics integrate a variable `DeltaTime` and read `rand()`, and the two PCs can run at different frame rates, so nothing here
assumes the same simulation runs twice. Everything travels on the session's Direct or Relay path, never as a Worker message.

- **Guest to host, 30 Hz:** the raw pad (`InputMsg`). All driving reaches the game through `XamInputGetState`, so the host answers
  that call for the seat's pad with the guest's buttons, triggers and sticks, and the kart drives as a local one does. Start and
  Back stay on the guest. After 0.25 s without input the pad goes neutral.
- **Host to guests, 20 Hz:** every kart by slot (`SnapshotMsg`): position, `KartMovementQuat`, velocity, `KartSpeed`, and that
  kart's race — `CompletedLaps`, `DistanceInRace`, `RacePosition`, `RaceFinishPosition`, `racingTime` and `bIsRaceFinished`.
  `idPawn` is the URL slot on every PC. The guest writes them onto the kart, so laps, the position list, the finish and the results
  are the host's on every PC; the game's own ordering agrees because the distance it sorts by is carried too. A kart whose own
  fields do not look like this layout is not written at all (`src/online_kart_write.h`), so a wrong offset cannot spread.
- **Remote karts on a guest:** drawn 100 ms in the host's past between two snapshots, by time and not by frames, moved with
  `UWorld::FarMoveActor` (`0x825D4E10`, noCheck), which is how UE3 places a replicated actor. Heading and speed are written too.
- **The guest's own kart:** drives at once. A snapshot says which input it had, and is compared with where this kart was when that
  input was sent. A small difference is ignored, a medium one blends away, and past 900 units the host's state replaces the kart.
  Before the green light the kart stands where the host has it: each game fills its own grid.
- **The start:** the gate is the host's. `RaceState` leaving 0 on the host is the begin event; `begun` repeats in every snapshot, so
  it needs no retransmission and no Worker change. Until then a guest's Start and A are closed and "Waiting for host" is on screen.
  On the begin, the same press reaches its game through its own pad until its `RaceState` moves. The host's Start waits for every
  guest's karts, 20 s at most.

Not synchronized: items and their effects, and the return from the race. Steering and drift animation of a remote kart are not
carried either. A host pause stops the race for everyone; a guest cannot pause, and has the multiplayer menu
instead (see "Leaving").

## Reserved keys (`src/main_menu.h`)

Exact matches in the `sub_82A46B68` hook; every other key, including any other `ECK_*` text, is answered by the game.

| Key | Sent by | Effect |
| --- | --- | --- |
| `ECK_EXIT_GAME` | Exit Game row | hard exit |
| `ECK_ONLINE_MENU` | main-menu movie `onLoad` | `NoteMainMenuShown()`: an invite can prompt from here on |

Same gate as the Exit row: size 176575 and SHA-256 `6ac9fb05fdac97a9070878a99000ac6bd217b1678c68cf8ae5784e92b4ae8d7e`
(`kMainMenuSwfFingerprint`); any mismatch leaves the movie untouched. `OnGamepadA` handles only the Exit row and otherwise
calls the original handler, so Free Race goes straight to CompetitionSelect.

## Texts

`src/online_text.h` plus `kMenuStrings` in `src/online_menu.h`, INT/ESM/ESN/PTB; the tests assert every key is present in all
four.

## What to look for at runtime

`[ONLINE-RACE]` carries one line per milestone (`race_begin host_confirmed`, `race_begin received`, `gameplay input_rx`,
`gameplay snapshot_rx`, `remote seat=0 state_applied`), and `[ONLINE-NET]` one summary per second with packets, bytes,
drops and each peer's transport, last sequences, age and round trip.

```
[ONLINE] character_select entered online_play=on
[ONLINE] invite shortcut available remaining=3
[ONLINE] invite picker opened
[ONLINE-SETUP] free_race generation=<n>
[ONLINE-SETUP] mode committed value=<n>
[ONLINE-SETUP] difficulty committed value=<n>
[ONLINE-SETUP] ready generation=<n> game_type=<n> mode=<n> difficulty=<n>
[ONLINE-SETUP] incomplete missing=<free_race|mode|difficulty>
[ONLINE] invite picker waiting reason=service_not_ready
[ONLINE] session host requested
[ONLINE] invite send friend=<code>
[ONLINE] invite accepted generation=<n>
[ONLINE] session joined seat=1
[ONLINE] guest setup received mode=<n> difficulty=<n>
[ONLINE] travel command=start StartMenu?GoBackType=2...
[ONLINE] guest entered character select
[ONLINE] peer seat=1 transport=direct
[ONLINE] pick seat=<n> state=<n> character=<n>
[ONLINE-GAME] canonical_panels seat0=panel0 seat1=panel1 seat2=panel2 seat3=panel3 local_seat=<n>
[ONLINE-GAME] panel0 seat=0 owner=remote virtual=0 character=<n> state=<n>
[ONLINE-GAME] panel1 seat=1 owner=local physical=0 character=<n> state=<n>
[ONLINE-GAME] panel2 empty
[ONLINE-INVITE] picker_open room=none seat=-1
[ONLINE-INVITE] send_requested friend=<code>
[ONLINE-INVITE] host_create_begin
[ONLINE-INVITE] host_create_ready seat=0
[ONLINE-INVITE] sent friend=<code> pending=<n>
[ONLINE-INVITE] send_failed friend=<code> reason=<reason>
[ONLINE-INVITE] accepted friend=<code or name> seat=<n>
[ONLINE-INVITE] role_replace old_seat=0 new_role=guest
[ONLINE-INVITE] old_room_cleared
[ONLINE-INVITE] joined_inviter seat=<n>
[ONLINE-LIFECYCLE] local_leave requested seat=<n> source=<guest_menu|host_menu|race_return|window_close|watchdog|menu>
[ONLINE-LIFECYCLE] remote_leave seat=<n>
[ONLINE-LIFECYCLE] guest_left seat=<n>
[ONLINE-LIFECYCLE] host_left seat=0 reason=<reason>
[ONLINE-LIFECYCLE] host_left seat=0 action=return_to_menu
[ONLINE-LIFECYCLE] cleanup room=<id> session_generation=<n>
[ONLINE-LIFECYCLE] host_return command=start StartMenu?...GoBackType=<n>
[ONLINE-LIFECYCLE] after_race host destination=<2|3|9> generation=<n> sent=<0|1>
[ONLINE] after_race follow destination=<2|3> generation=<n>
[ONLINE-RACE] post_race host|guest
[ONLINE-LIFECYCLE] return_to_menu reason=<reason>
[ONLINE-RACE] race held command=start <map>?...
[ONLINE-RACE] race canonical generation=<n> start <map>?...
[ONLINE-RACE] race published generation=<n>
[ONLINE] race start generation=<n> seat=<n>
[ONLINE-RACE] race released
[ONLINE-RACE] race refused field=<field>
[ONLINE-RACE] race aborted reason=<reason>
[ONLINE-RACE] slot=<n> seat=<n> user=<pad> ... role=local|remote_designated
[OPEN-FILE] package=<url> file=0x<address> operation=<pass|track_screen|patch_main_menu>
[ANALYTICS] race.complete race=<n> duration_ms=<n> places=<n>
[ANALYTICS] race left before the end race=<n> last_state=<n> seconds=<n> reason=<world_gone|new_race|service_lost>
```

`after_race` and `host_return` come from any host menu command, the pause menu included, so neither means a race finished.
Only `race.complete`, at `RaceState` Finished, does.

If `character_select entered` never appears, `ShowScreen` was not seen for the screen: check `fingerprint group=` in the log.
Without the hint, `invite shortcut hidden reason=` says why. For panels, one line each when the outcome changes:

```
[ONLINE-GAME] panel_scan manager=<addr> base=<offset> panel0=<addr> readable=yes shift=<n> pad_hits=<offsets> game_hits=<offsets>
[ONLINE-GAME] panel_candidate pad_at=<offset> game_at=<offset> parent_at=<offset> parent=<addr> state_at=<offset> state=<n> character_at=<offset> character=<n> failed=<none|screen_parent|state_or_character|no_candidate>
[ONLINE-GAME] remote_panel_bridge=available|unavailable reason=<panels_not_seen|panel_unreadable|game_unreadable|layout_not_found>
```
