# Online in the game's menus

Free Race / Multiplayer asks **Local** or **Online**. Nothing is written to the game files; the one movie change is the
in-memory main-menu patch of `docs/main-menu.md`.

## Flow

```
Free Race / Multiplayer (Exhibition row)
  Local  -> the original Exhibition screens (nothing else happens)
  Online -> Create Room (Players 2/3/4, Create)   -> Room
            Find Room   (Saulo 1/4 ... Join)       -> Room
Room: Player 1..4 (Host, You, Invited, Waiting for players, Room full)
      host: Invite Friends, Start Race (2+ players; the room need not be full)   all: Leave Room
Incoming invite "<Name> invited you to a Multiplayer race": Accept / Decline (Accept joins that room, never creates one)
```

Pages, rows and reactions are `src/online_menu.h` (pure, tested). `src/online_menu_overlay.cpp` draws them as an ImGui
modal (`ModalDialog`, the base of the Achievements overlay) and feeds them D-pad/stick, A, B, arrows, Enter, Escape and
the mouse. The service is only polled through `snapshot()` once per frame; rows change when the snapshot changes
(seat joined/left, host left, reconnecting, room started, search answered), and rooms are searched on entering Find
Room and on Refresh (at most every 2 s).

## Why an overlay and not native Scaleform screens

Judged not practical to land blind; the other cooked movies were not decoded for this decision.

- The main-menu movie (sprites 99 and 130, the only one the patch system rewrites) has no list, seat or toast screens.
  Every screen would be new AS2 assembled by hand in `main_menu_swf.cpp`: a list with selection and scrolling, a Players
  selector, per-seat lines, toasts and a modal, in raw bytecode that cannot be run while writing it.
- Text reaches a movie only through `RequestTextFieldUpdate(key)`, answered in `execGetFieldValueByName` into a text
  field the movie bound with `AddTextFieldToSyncText`. That is one string per key per request; a list that grows, an
  invite that arrives, a seat that changes would each need its own bound fields, a poll timer in the movie and a way
  to put new rows on the stage. None of that exists in the movie, and a wrong guess is a crash in the player.
- The Achievements row already opens an overlay of this kind, so the pattern, input blocking and release gate are
  proven in this port.

The overlay is therefore the first testable flow; the reserved-key protocol is kept as the seam a native screen can use
later.

## Reserved keys (`src/main_menu.h`)

Exact matches in the `sub_82A46B68` hook; every other key, including any other `ECK_*` text, is answered by the game.

| Key | Sent by | Effect |
| --- | --- | --- |
| `ECK_EXIT_GAME` | Exit Game row | hard exit |
| `ECK_ONLINE_ENTER` | Exhibition row confirmed (OnGamepadA) | `RequestOnlineMenu()`: with Online play on, the Local/Online choice opens over the game |
| `ECK_ONLINE_MENU` | main-menu movie `onLoad` | `NoteMainMenuShown()`: an invite may open the menu while the main menu is up |

## Movie patch

Same gate as the Exit row: size 176575 and SHA-256 `6ac9fb05fdac97a9070878a99000ac6bd217b1678c68cf8ae5784e92b4ae8d7e`
(`kMainMenuSwfFingerprint`), sprite 99 rows, class script `COMMAND_GOTO_ACHIEVEMENTS`; any mismatch leaves the movie
untouched and the menu is the original. Added to the wrapped `CKMainMenuRoot` methods of sprite 130:

- `onLoad`: after the original, sends `ECK_ONLINE_MENU`.
- `OnGamepadA`: `Exit` row sends `ECK_EXIT_GAME` and stops. Any other row: when the selected command is `Exhibition`,
  sends `ECK_ONLINE_ENTER`; then **always** calls the original handler. Local is therefore the original flow: the
  Exhibition screen opens as before and the choice merely sits over it. `tests/main_menu_tests.cpp` checks the original
  is saved once, called once, after the guarded request.

With Online play off the request is ignored and nothing opens. Local never calls the online service.

## Start Race (host)

`StartRace()` freezes the seats in the service. On the room leaving `open`, every PC (host and guests) builds the same
`online_start::Plan` (`src/online_start_race.h`): race id and seed from the room id, laps/items/track/karts from the room
rules (`laps` 3 and `items` 1 by default; `track` absent is recorded as unknown), characters and karts unknown, seats
mapped by `race::MapSeatsToKarts` (humans in seat order from slot 0, bots after). `Decide()` then:

- `ECK_ONLINE_DEBUG=1`: logs the dry run, with host seat, local seat, local player index, per seat kart slot and
  PlayerController slot, and the expected `?NumHumanKarts=?TotalKarts=?NumSelectedTrack=?PrimaryControllerID=`
  fields (`[DEBUG_SAULO][ONLINE-RACE] dry-run ...`). Nothing is injected.
- Always, until a hook proves the guest side: `[DEBUG_SAULO][ONLINE-RACE] start blocked reason=<reason>` and the room
  shows "Race not available yet". Without the variable the reason is
  `ECK_ONLINE_DEBUG is not 1 and the final guest hook is unproven`.

Seam for the race-entry probes: `online_start::SetStartRaceHook(std::function<std::optional<Verdict>(const Plan&)>)`.
A hook that returns `real_entry` takes over (the room shows "Race starting"); a verdict with a reason is logged as the
blocked line. Joiners and the host can always Leave Room and return to the menus.

## Texts

`src/online_text.h` plus `kMenuStrings` in `src/online_menu.h`, INT/ESM/ESN/PTB; the tests assert every key is present
in all four. The main-menu overrides (Free Race / Multiplayer, Exit Game) and the fall-through of unknown keys are
unchanged.

## What to look for at runtime

1. Log: `scaleform patch applied`, then on pressing Free Race / Multiplayer `[DEBUG_SAULO][MAIN-MENU] action=ECK_ONLINE_ENTER`
   and `[DEBUG_SAULO][ONLINE-MENU] opened`. If `ECK_ONLINE_ENTER` never appears, the confirm path used (mouse click?
   other handler than `OnGamepadA`) is not wrapped; the Exit row has the same limit.
2. The Exhibition screen loads behind the choice. Confirm B on every page steps back and the game gets no stray press
   when the overlay closes.
3. `ECK_ONLINE_MENU` after the menu loads (needed for an invite to open the menu on its own). It is cleared on Local,
   so an invite does not interrupt a race that was started from the menu; Cups/Challenges/Extras do not clear it.
4. Two instances (`tools/online-instance.ps1`, `ECK_ONLINE_DEBUG=1`): create, invite/join, Start Race, compare the
   two `dry-run` blocks (same race id and seed, opposite `local`/`remote`).
5. Not covered: a declined invite does not reach the host (the service does not tell it), so the Invited marker fades
   after 60 s; per-seat character and kart and the track come from the game's own screens and are unknown here.
