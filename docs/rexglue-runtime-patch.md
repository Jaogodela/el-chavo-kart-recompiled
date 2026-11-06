# Patched ReXGlue runtime

`tools/build.ps1` builds `rexruntime` and `rexgpu-xenos` from ReXGlue v0.10.0 (commit in `patches/rexglue-sdk/baseline.env`)
with the patches in `patches/rexglue-sdk`, applied in name order, and records the result in
`tools/rexglue/win-amd64/rexruntime-patch.txt`. The check and rebuild scripts are `tools/rexruntime-patch.ps1` and
`tools/build-patched-rexruntime.ps1`, taken from Super Street Fighter II Turbo HD Remix Recompiled. Debug builds use the
stock DLLs. The whole series was checked with `git apply` in order against a clean v0.10.0 tree.

| Patch | From | Why |
| --- | --- | --- |
| 0001 physical-release-order | Hard Corps 0001 | Allocator race corrupting page state |
| 0002 cpu-timers-and-waits | Hard Corps 0006 | Timer and wait CPU cost |
| 0003 renderdoc-detect-loaded-only | Hard Corps 0010 | Detection no longer loads RenderDoc |
| 0004 windows-high-resolution-sleep | Hard Corps 0015 | Sleep precision on Windows |
| 0005 vulkan-device-preference | Hard Corps 0026 | Graphics card selection |
| 0006 d3d12-adapter-preference | Hard Corps 0027 | Graphics card selection |
| 0007 msaa-2x-host-log | Hard Corps 0028 | Logs the host MSAA mode |
| 0008 ui-repaint-with-guest-frames | Hard Corps 0029 | One present per guest frame with ImGui dialogs open |
| 0009 stop-title-threads | Hard Corps 0033 | Clean teardown with guest threads running |
| 0010 imgui-mouse-cursors | Hard Corps 0045 | Launcher cursors |
| 0011 input-activity-callback | Hard Corps 0047 | Cursor follows the last input device |
| 0012 local-user-display-names | Hard Corps 0057 | Player names; saves keyed by the fixed profile |
| 0013 xam-enumerate-zero-items-on-error | Marvel Ultimate Alliance 0053 | Enumeration results on errors |
| 0014 xma-context-word-updates | Marvel Ultimate Alliance 0058 | XMA context correctness |
| 0015 xma-context-loop-layout-acquire-loads | Marvel Ultimate Alliance 0059 | XMA loop data race |
| 0016 sdl-audio-return-credit-on-failed-write | Marvel Ultimate Alliance 0060 | Audio queue stall after a failed write |
| 0017 xam-content-result-checks | Marvel Ultimate Alliance 0063 (upstream backport) | Content create/enumerate results |
| 0018 vfs-overwrite-in-place | Marvel Ultimate Alliance 0064 (upstream backport) | Save rewrites truncate in place |
| 0019 xma-context-stall-guards | Marvel Ultimate Alliance 0066 (Unowa001/crackdown-recomp) | Voices no longer stall after a hitch |
| 0020 excluded-audio-channels | Marvel Ultimate Alliance 0069 | Excluded channels do not spread NaN |
| 0021 audio-callback-pacing | Marvel Ultimate Alliance 0102 (Crackdown 0051) | Spaced audio callbacks |
| 0022 audio-thread-priority | Marvel Ultimate Alliance 0115 | Audio and XMA threads not starved |
| 0023 object-lookup-already-locked | Marvel Ultimate Alliance 0116 | Object lookup without re-locking |
| 0024 xam-zero-size-profile-settings | Marvel Ultimate Alliance 0142 | Empty profile setting writes |
| 0025 disable-debug-and-settings-overlay-binds | Marvel Ultimate Alliance 0146 | F3 and F4 open no SDK overlay |

The console bind (Backtick, the key that types an apostrophe on ABNT2 keyboards) is not patched out: the app removes it
with `UnregisterBind` in `OnCreateDialogs`, together with the two binds above, so Debug builds on the stock DLLs lose
them too.

Not carried: Marvel Ultimate Alliance 0137 (XMA stream packet continuity). It depends on that project's audio
diagnostics patch, and is the first candidate to evaluate if the log shows FFmpeg XMA errors such as "would have to skip
N bits".
