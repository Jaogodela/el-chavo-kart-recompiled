# Launcher graphics and gameplay options

Static findings behind the Display, Graphics, Gameplay and Controls options. Nothing here was run-tested.

## Internal render resolution
`eck_internal_render_scale` (100/75/50) needs `render_target_path_d3d12 = rtv` when reduced (`ApplyInternalRenderPath`). The host
render targets are `resolution_scale * pct / 100` of the guest picture; EDRAM, resolves and textures keep the whole render
resolution. SDK patches 0038/0039 implement it. They are layered on the generic resolve and render-target changes, so the
series keeps their dependencies (0026-0037, each generic patch with its D3D12 counterpart); these were found by tracing the
lines and symbols each patch builds on, and the Hard Corps patches that nothing in the chain uses were left out.

## Skip intro
`sub_823BF3E8` starts the startup movie list (DISCLAIMER01, logos, INTRO, VIDEODEMO); returning early is the engine's own
`-nostartupmovies` path. The end-of-init stop-movie wait returns at once because no movie is active.

## Frame rate
Offered: 30 (the console rate), 60, 90, 120, 144, 165, 240 and Unlimited, default 60 (`eck_frame_rate`). The title requests
`PresentationInterval = TWO` unless `-novsync` is given (`sub_829EA5E0`), so its flip scheduler `sub_82B583B0` holds each
frame for two vblanks; with the SDK's 60 Hz guest vblank that is 30 FPS. The hook on `sub_82B583B0` clears the interval bits
(bits 8-11 of r3) so flips are immediate, and `PaceGuestFrame` (high-resolution wait on `VdSwap`) holds the selected rate.
The engine's own cap is disabled by hooking `GetMaxTickRate` (`sub_825A4D70`, 62 by default).

Simulation: `sub_82A40CB0` measures `GDeltaTime` from real time each loop iteration and `UGameEngine::Tick` (`sub_825A4D98`)
passes it to `UWorld::Tick`, which scales it by the time dilations, clamps it to 0.0005..0.4 s and accumulates `TimeSeconds`.
Actors tick with dt-proportional deltas, and the PhysX scene (`sub_82794FA8`) takes dt as sub-steps of at least 2.5 ms. The
engine is a variable-delta design, so the Hard Corps fixed 60 Hz step schedule and interpolation are not used. Unlimited
is held to 300 FPS so the delta never falls under the engine's floors. Not examined: frame-counted gameplay logic and
fixed per-frame smoothing, which is what the manual test at 90 and above has to confirm.

## Unlock secret characters
`eck_unlock_secret_characters`, off by default. From the decoded `ChavoKartGame.xxx` and `TitleMap.xxx`:

| Id | Character | Available |
| --- | --- | --- |
| 0, 1, 2 | Chaves, Quico, Seu Madruga | from the start |
| 3, 4 | Dona Florinda, Professor Girafales | cup 1, Copa Jirafales |
| 5, 6, 7 | Popis, Nhonho, Senhor Barriga | from the start |
| 8 | Dona Clotilde | cup 3, Copa Cleotilde |
| 9 | Paty | cup 5, Copa Sr. Barriga |
| 10 | Rufino Malfeitor | cup 7, Copa Rufino |
| 11 | Godines | from the start |

`EKartCharacterType` has these 12 plus `Count` and `MAX`, which are not characters. There are no DLC, test or placeholder slots:
12 `PlayerCharacterClassNames`, 12 `KartPawn*Arch_SF` packages, 12 preview actors, and `DoPlayerSelection` wraps at 0 and 11.

The defaults are `KartDefaultData_Cups.UnlockedCharacters` and each cup's `UnlocksCharacters`. A cup pays out the first time the
last unlocked cup ends with `BestPlace` 1 to 3 (`KartProgressManager_Cups.OnCupEnded`, then `KartScreen_WinnersPodium.CheckForUnlockables`
and `KartScoreDao.AddUnlockedCharacter`). The save is read into `KartScoreDao.UnlockedCharacters`, and
`KartScreen_PlayerSelectManager.GetListOfLockedCharacters` is 0 to 11 minus that array.

`isCharacterLocked` is the only reader that matters: `KartScreen_PlayerSelectIndividual.ShowPlayer` feeds it to the movie's
`SetLockVisible`, the movie's `OnGamepadA` refuses a confirm while `lockedMC` is visible, and `OnPressAButton` and
`OnPressStartButton` check it again.

The array cannot be changed: `CreateCupsJSON` saves it whole on every save, `HasUnblockedAllCharacters` (`Length == 12`) grants
achievement 34, and `AddUnlockedCharacter` would refuse to record a real cup reward. So `src/character_unlock_hook.cpp` hooks
`UObject::CallFunction` (`sub_82313048`), finds the function through `GNames`, runs it, and clears the caller's result for that one
call. `FFrame.Object` is the calling panel, so the answer is per panel: local panels follow the option, and a remote panel in a
session is always open, so a peer never refuses a pick because of its own save. Peers do not need the option to see a secret
character, and the protocol and Worker already accept all 12.

`KartSceneCapture2DActor.SetLockVisible(true)` paints the preview black and nothing restores it until the menu map reloads, and the
four panels share the twelve actors. In a session the hook returns from that function at once. A character the host passed over
before the room existed stays black on the host until the next map load; this only affects the preview.

The launcher runs before the game, so changing the option means restarting the game. The hook reads the cvar on every check.
A cup played with a secret character stores it in `CupData.SelectedCharacter` for resuming, as it does for the bots' characters.
That is not an unlock and resuming does not check it.

## Local players
The game polls and range-checks 4 users (`cmplwi 4`). `XamUserGetName` is called from `sub_82A0E268` and `sub_82A0D328` with a
32-byte buffer, converted as ANSI; the SDK limits to 15 bytes. Patch 0012 provides the 4 name cvars. Patch 0040 (from SSF2T's
guest sign-in) makes users 1-3 signed in (offline, local only) with a derived XUID and their name while a pad is assigned to
them; user 0 is unchanged. Names are separate from any future online identity.
