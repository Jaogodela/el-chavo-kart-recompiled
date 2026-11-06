# Local save, progression and achievements

Everything the player generates lives under the user data root (`build\user_data` in development, resolved by ReXGlue
and the launcher elsewhere). Nothing is written to `game\extracted`, and nothing is pre-populated: a missing user data
root is a new profile.

## Storage model (static, from `default.xex`)

| System | Used | How |
| --- | --- | --- |
| Profile settings | yes | `XamUserReadProfileSettings` / `XamUserWriteProfileSettings`, three settings per call (UE3 online profile settings), written through `XamUserGetXUID` for user 0 |
| XamContent / SavedData | imported | `XamContentCreateEx`, `XamContentClose`, `XamContentDelete`, `XamContentCreateEnumerator`, `XamContentGetCreator`, `XamShowDeviceSelectorUI`; served by ReXGlue's content manager |
| Achievements | yes | `XMsgStartIORequest` (XGI `XUserWriteAchievements`), handled by ReXGlue |
| Stats / leaderboards | imported | `XamUserCreateStatsEnumerator`, `XamEnumerate`; left to ReXGlue, no persistence depends on them |

The profile settings are the three title-specific blobs `0x63E83FFF`, `0x63E83FFE`, `0x63E83FFD`, which ReXGlue's
default profile already defines, loads on read and saves on write. No hook is needed on the profile path. A first run
reads them as not set (`from` = 0) and the game builds its defaults; the first write creates the files. A development
profile from a previous session showed all three files created by the game, one with ~1 KB of data and two empty
(patch 0024 keeps the zero-size writes safe).

Script progress is one JSON string: `KartManager_Data.SavePlayerProfile` writes `KartScoreDao.GetJSONObjectAsString()` as
profile setting 111, and `CreateJSONObject` rebuilds that string from the live `KartScoreDao` on every save. Which setting ends
up in which of the three blobs could not be proven without running the game.

## User data layout (relative to the user data root)

- `475807D7\profile\User\63E83FFF`, `63E83FFE`, `63E83FFD`: the game's profile settings (written by ReXGlue).
- `475807D7\<content>`: only if the game creates a SavedData container (written by ReXGlue's content manager).
- `achievements\475807D7.toml`: unlocked achievements, the only unlock state.
- `settings.toml`: the launcher's player settings.

## Achievements

- 46 achievements, IDs 1 to 46 (no ID 0, no alias), 1000 gamerscore, read from the XDBF resource inside `default.xex`.
  Icons and strings come from that XDBF through ReXGlue (`AchievementIconCache`); nothing is shipped separately.
- Unlock: the game's XGI write reaches `KernelState::UnlockAchievement`, which saves the TOML and shows ReXGlue's
  standard Xbox-style notification.
- F7 opens the overlay (`src/achievements_overlay.cpp`); F7, Escape and controller B close it. The overlay is about
  70% of the viewport with a size cap, scrolls with the D-pad or left stick, and lists icon, name, description,
  gamerscore, state, the unlocked count and total gamerscore.
- While the overlay is open and until the closing press is released (1 s timeout), the game reads neither keyboard nor
  controller, so B never also backs out of the menu underneath (`src/modal_dialog.cpp`, `src/overlay_rules.h`).
- The game's own Achievements menu calls the wrapper at `0x830D8E58` around `XamShowAchievementsUI`. The project hook in
  `src/achievements_menu.cpp` opens the same overlay and returns success (0); ReXGlue's stub no longer runs on that
  path. If no overlay is available the original import runs.
- The launcher Achievements page reads the same store through a temporary tool-mode runtime that loads `default.xex`
  (`src/launcher_achievements.cpp`). It is read-only and is destroyed before Play builds the game's runtime.

## Live and network

No Live compatibility was added. Sign-in state, XNet and sessions are untouched, and online features stay unsupported.

## Checks

`.\tools\test.ps1 -Filter achievements` runs the headless list, summary, window-size, close-gate and scroll rules.
