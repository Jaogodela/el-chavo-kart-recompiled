# Game analysis: El Chavo Kart (USA), Xbox 360

Facts established statically from the disc image and `default.xex`. Addresses are guest addresses in the decrypted
image (`python -I tools/xex_image.py`).

## Build

| | |
| --- | --- |
| Title ID / Media ID | 475807D7 / 5C2ECDD6 |
| Version | 0.0.0.3 (base 0.0.0.3), disc 1 of 1, built 2013-11-28 17:24:27 UTC |
| Executable | `default.xex`, 30,003,200 bytes, SHA-256 `1DB4E96B...5CE120F`; original PE `ChavoKartGame-Xbox360-Shipping.pe` |
| Image | 0x1DB0000 bytes at 0x82000000, entry point 0x830DB568, basic (uncompressed-block) XEX format |
| Engine | Unreal Engine 3 (cooked `.xxx` packages), FMOD (`FMODAudio.xxx`, `*.fsbcache`), Bink 2 movies statically linked (`BINK`, `RADCODE` sections), Scaleform GFx |
| Imports | `xboxkrnl.exe` (365 records) and `xam.xex` (236 records) only; no extra modules |
| Disc | XGD2, game partition at 0xFD90000; 401 files, 1.91 GB; `$SystemUpdate` is not game data |
| Title update / DLC | none on the disc; the title calls `XamContentCreateEnumerator` but no DLC is known |

## Languages

The disc ships four language sets: `INT` (English), `ESM` (Spanish, Latin America), `ESN` (Spanish, Spain) and `PTB`
(Brazilian Portuguese), as `Coalesced_<LANG>.bin`, `*_LOC_<LANG>.xxx`, `Xbox360TOC_<LANG>.txt` and a localized
`DISCLAIMER01_<LANG>.BIK`. The XDBF string tables (achievements, title name) are English, Spanish and Portuguese.

The UE3 language selector at 0x829E58B0 maps `XGetLanguage()` (0x830D8F80, XConfig user setting 9) to the package
suffix: 1 INT, 5 Spanish, 9 PTB (plus JPN/DEU/FRA/ITA/KOR/CHN/POL/RUS that this disc does not ship). Spanish is `ESN`
only when `XGetLocale()` (0x830D9830) returns 0x1F (Spain), else `ESM`. `XGetLocale()` reads XConfig user setting 0x0E,
the country byte, and translates it through the table at 0x83BC4168: country 31 gives Spain, 71 Mexico, 13 Brazil, 103
United States. The port sets ReXGlue's `user_language` and `user_country` accordingly (`src/language_setting.h`).

## Frame rate

The runtime configuration (`Coalesced_INT.bin`) has `UseVsync=False`, `bSmoothFrameRate=TRUE` and
`MaxSmoothedFrameRate=62`, at 1280x720. UE3 runs a variable time step. The title's default present interval is two vblanks,
which is the 30 FPS console cadence; see `docs/launcher-graphics-gameplay.md` for the hooks (`src/frame_limiter.cpp`) that
replace it with an immediate flip and a selectable 30/60 limiter.

## Recompilation

- 73,941 registered functions, of which 294 are imports. 4,709,501 of the 4,710,640 non-zero code words in `.text`,
  `BINK` and `RADCODE` are translated; the rest are in-code jump tables, padding and unreferenced fragments.
- `config/overrides.toml`: 16 entries. 15 are the targets of `b` from tail thunks, vtable forwarders and this-adjustors
  placed directly after another function, without which codegen Validate fails; one is a vtable-only this-adjustor.
- `config/discovered-functions.toml`: 430 functions reached only through vtables, data pointers or `lis`/`addi` pairs,
  written by `tools/find-missing-functions.py --apply-high`. The 672 LOW candidates are addresses inside other functions'
  `.pdata` extents (switch labels) and stay out; 117 are exception-handler funclets the SDK registers itself.
- CRT `setjmp` 0x830BDFB0 and `longjmp` 0x830BDC90 (7 call sites) are set in the manifest.

## Saves, achievements, license

- Saves go through `XamShowDeviceSelectorUI`, `XamContentCreateEx`, `XamContentClose`, `XamContentGetDeviceData` and
  profile settings through `XamUserReadProfileSettings`/`XamUserWriteProfileSettings`, all served by ReXGlue's content
  manager and profile under the user data root.
- Achievements: 46 (IDs 1 to 46, 1000G) in the XDBF resource. The title unlocks them through `XMsgStartIORequest`
  (XGI), which ReXGlue's achievement store handles; no ID 0, so no alias is needed.
- `XamContentGetLicenseMask` is called once, from the UE3 HDD cache setup at 0x829E5CE0, not from trial logic; the
  SDK default `license_mask` is kept.

## Input

From `Xbox360-ChavoKartInput.ini` (the cooked input config): left stick steers, analog triggers (`aRightAnalogTrigger`,
`aLeftAnalogTrigger`), A `SmartJump`, B boost (`BoostRelease` on release), X `UseItem`, Y `LookBack`, right shoulder
`ChangeTrigger`, Back `SafeRelocation`, Start pause, D-pad menu navigation.
