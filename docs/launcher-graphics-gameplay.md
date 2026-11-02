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

## Local players
The game polls and range-checks 4 users (`cmplwi 4`). `XamUserGetName` is called from `sub_82A0E268` and `sub_82A0D328` with a
32-byte buffer, converted as ANSI; the SDK limits to 15 bytes. Patch 0012 provides the 4 name cvars. Patch 0040 (from SSF2T's
guest sign-in) makes users 1-3 signed in (offline, local only) with a derived XUID and their name while a pad is assigned to
them; user 0 is unchanged. Names are separate from any future online identity.
