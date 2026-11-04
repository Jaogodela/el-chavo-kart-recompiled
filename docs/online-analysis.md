# Online analysis: networking and multiplayer in El Chavo Kart (Xbox 360)

Static audit only. Nothing here was observed at runtime. Every row is either read from the decrypted image
(`game/analysis/default.image.bin`, base `0x82000000`), the generated C++ (`generated/default/*.cpp`), the cooked
packages, `Coalesced_INT.bin`, or the SDK source (`out/rexglue-sdk-src`, patches in `patches/rexglue-sdk`). Where a
statement is an inference, it is in the Unknowns section, not in a table of facts.

Method notes (so the results can be redone):

- Native exec tables: every `UClassexecName\0` string in `.rdata` that is the first word of an 8-byte `{name, function}`
  entry. 2,492 entries found. The registration functions were found by the `lis`/`addi` pair that loads each table.
- Callers: all `// bl 0x...` and `// b 0x...` comment lines of the generated functions. Indirect calls (vtables, function
  pointers) are invisible to this, which is why some functions below list no caller.
- `.pdata` trap: pointers to a function start also appear in `.pdata` as `{begin, 0x40000000 | words << 8 | flags}`.
  A "reference" followed by a `0x4000xxxx` word is `.pdata`, not a vtable.
- Cooked packages (`CookedXbox360/*.xxx`) are whole-file compressed except `StartMenu.xxx` and `ChavoKartEntry.xxx`.
  Format: `u32 BE tag 0x9E2A83C1, block size 0x20000, total compressed, total uncompressed`, then one `{compressed,
  uncompressed}` BE pair per 128 KB block, then the blocks. Each block is a series of XMemCompress frames
  `[BE16 size][payload]` (a leading `0xFF` means `[0xFF][BE16 uncompressed][BE16 compressed]`), 32,768 bytes per frame.
  Concatenating the payloads of one block gives a plain LZX stream, window 2^17, which libmspack `lzxd` decodes
  (checked: ChavoKartGame, Engine, IpDrv, OnlineSubsystemLive, GameFramework, GFxUI, Core, Startup, all with 0 bad
  blocks). The result is a normal UE3 package, summary version word `0x0000035C` (860). No tool for this exists in
  `tools/`; a scratch decoder and a name/import/export lister were used.
- `Coalesced_INT.bin`: `i32 BE count`, then per file `FString name, i32 section count`, per section `FString name,
  i32 key count`, per key `FString key, FString value`; `FString` is `i32 BE length` (negative means UTF-16, length in
  characters including the NUL) followed by the characters in little-endian UTF-16. 20 files, whole 367,604 bytes
  consumed.

## 1. Proven: the game has no online user flow

| Evidence | Where |
| --- | --- |
| No online screen class exists. The 31 `KartScreen_*` classes are Blank, Challenges, Controls, Credits, CupLobby (a cup results table, not a network lobby), CupsSelectMenu, EndRace, ExtrasMenu, GameModeMenu, HUDManager, HUDIndividual, HUDIndividualAI, Loading, MainMenu, MenuBackground, Pause, PlayerSelectIndividual, PlayerSelectManager, Popup, RaceResults, RaceStartCinematicSequence, SaveDataAdvice, Settings, Store, Tittle, TrackSelectMenu, WinnersPodium (plus Base) | `ChavoKartGame.xxx` class exports |
| `ChavoKartGame.xxx` (361 classes, 13,999 exports, 881 imports) imports from Engine only `OnlineSubsystem` (`ELoginStatus`, `PlayerInterface`, `PlayerInterfaceEx`, `AchievementDetails`, `UniqueNetId`), `OnlinePlayerInterface` (GetLoginStatus, GetPlayerNickname, GetUniquePlayerId, IsLocalLogin, Logout, ShowLoginUI, UnlockAchievement, WriteProfileSettings), `OnlinePlayerInterfaceEx` (ShowAchievementsUI, GetDeviceSelectionResults), `OnlinePlayerStorage`, `OnlineProfileSettings`, `GameEngine.GetOnlineSubsystem`, `PlayerController.Register/ClearOnlineDelegates`, `ClientSetOnlineStatus`. Zero imports match OnlineGame, GameSettings, OnlineGameInterface, Session, Beacon, Invite, Friend, Party, Voice, Presence, Stats, Arbitration, NetDriver, NetConnection | import table of `ChavoKartGame.xxx` |
| No game-package subclass of `OnlineGameSettings` / `OnlineGameSearch`; the game's only online-flavoured class is `ChavoKartProfileSettings : Engine.OnlineProfileSettings` | class exports |
| No `Server*` / `Client*` function in any game class. The only match is the override `KartPC_Base.ClientSetOnlineStatus`. (This does not prove there are no replicated properties; it proves there are no game RPCs.) | function exports |
| The only online behaviour in the script is sign-in change handling: `KartGame_Base.OnLoginChangeEvent`, `OnLoginChangePauseHandle`, `Get/SetOnLoginChangeStatus`, `KartPC_Base.OnLoginChanged`, `NotifyLoginChange`, `NotifyGlobalLoginChange`, `UpdateLoginStatus`, `HasSignin[g]Changed`, `KartManager_Screens.ShowOnLoginChangeMessage`, `KartManager_Achievements.CheckLogins`. Localised text `not_logged_in_warn_text` = "A sign-in change has occurred, you are being returned to the title screen." | function exports, `ChavoKartGame_XBOX.int` |
| `Coalesced_INT.bin` has no online menu text. Network strings present are the stock UE3 ones: `Core.int`/`Engine.int` (`NoDownload`, `ConnectionFailed`, `ConnectionTimeout`, `MaxedOutMessage`, `FailedConnect`, `[OnlineGameSettings]` column names such as `bIsLanMatch.FriendlyName=Lan Match`, `OnlineSubsystemGameSpy.int` `GameInviteMessage`). Main menu help for Exhibition: "Choose how many human players, the race mode, the character, the track, then race!" | `Coalesced_INT.bin` |
| Online stack present in the image is the stock UE3 Xbox 360 `OnlineSubsystemLive` (6 classes: `OnlineSubsystemLive : IpDrv.OnlineSubsystemCommonImpl`, `LivePartyChat`, `ClientBeaconAddressResolverLive`, `DownloadableContentEnumeratorLive`, `OnlineEventsInterfaceMcpLive`, `OnlineTitleFileDownloadMcpLive`) plus `IpDrv` (46 classes incl. `OnlineGameInterfaceImpl`, `MeshBeacon*`, `PartyBeacon*`, `TcpLink`) and the Engine interfaces `OnlineGameInterface`, `OnlineGameSettings`, `OnlineGameSearch` | `OnlineSubsystemLive.xxx`, `IpDrv.xxx`, `Engine.xxx` |

Consequence: every online capability below is stock engine code that nothing in the game's own script calls. A port has
to add the UI/flow itself.

## 2. Local multiplayer and race start (the part that is actually used)

All from the cooked UnrealScript (function export names and string literals in the bytecode).

| Step | Class / function | Facts |
| --- | --- | --- |
| Main menu row | `KartScreen_MainMenu.SelectMenu("Exhibition")` | see `docs/main-menu.md` |
| Mode | `KartScreen_GameModeMenu.SelectCompetitionType` | modes by level prefix, `[Engine.GameInfo] DefaultMapPrefixes`: TA TimeAttack, SU Survival, EN Endurance, CT CTF, MA Matador, MAT MatadorTeams, CO Coins, FO FollowTheLeader, TUTBRAWL, TUTDRIVING, RET Challenges, RETF ChallengesCTF, PO Podium. Script classes: `KartGame_NormalRace`, `_TimeAttack`, `_Survival`, `_Endurance`, `_CTF`, `_Coins`, `_Podium`, `_Challenges`, `_ChallengesCTF`, tutorials |
| Humans | `KartScreen_PlayerSelectManager` (`SetupSinglePlayer(ControllerId)`, `SetupMultiplayer`, `IsMultiplayer`, `Show(ControllerId)`, `OnPress*Button` per `theLocalPlayer`, `IsReadyToGo`, `SetKartURLData`, `GotoNextScreen`) and `KartScreen_PlayerSelectIndividual` | one panel per local controller; the screen reads each `LocalPlayer` |
| Params | `KartProgressManager_Exhibition.SetupExhibitionParams` | literals `?NumHumanKarts=`, `?TotalKarts=`, `?NumSelectedTrack=`, `?KartAIDiffculty=`, `?GameType` |
| Common params | `KartProgressManager_Base.SetupCommonRaceParameters` | literals `?PlayersArray=`, `?CharArray=`, `?IsBotArray=`, `?TeamData=`, `?LastTipTextField=`, `?LastTipBGKNumber=`, `?PrimaryControllerID=`; per player struct `KartURLPlayerData {Character, ControllerId, TeamID, bIsBot}`; counters `NumberOfTotalKarts`, `NumberOfBotKarts`, `NumberOfHumanKarts` |
| Launch | `KartProgressManager_Base.LoadRace` | literals `open` and `start`, selected by `bUseOpenInsteadOfStart` (a console command with the map plus the URL options above) |
| Consume | `KartGame_Base.InitGame` (reads `PrimaryControllerID`, `DISABLEALLSCREENMESSAGES`), `KartProgressManager_Base.InitGame` (reads `GameType`, `NumSelectedTrack`, `NumHumanKarts`, `TotalKarts`, `KartAIDiffculty`, `PlayersArray`, `CharArray`, `IsBotArray`, `TeamData`, `GoBackType`, `NumGameMode`) | |
| Local players | `KartGame_Base.SetupLocalPlayers`, `KartGame_Menu.SetupLocalPlayers`, `KartGame_Base.KillAllPlayerControllers` | their bytecode references the names `CreatePlayer` and `RemovePlayer`; `SetupLocalPlayers` runs `getAll LocalPlayer ControllerId` / `getAll LocalPlayer Actor` console commands. Split screen: `KartGameViewportClient.KartMenuSetSplitScreen`, `UpdateActiveSplitscreenType` |
| Spawn | `KartGame_MainGame.StartHumans`, `StartBots`, `AddBot`, `SpawnBot`, `SpawnDefaultPawnFor`, `AddKartToRace`; race end `EndRace`, `NotifyKartFinished`, `KartProgressManager_*.OnRaceEnded` | |

Config facts: `[Engine.GameInfo] MaxPlayers=16`, `MaxSpectators=2`, `TotalNetBandwidth=32000`, `bAllowHostMigration=FALSE`,
`DefaultGame=ChavoKartGame.KartGame_Base`. The text "Race against 7 competitors" (INT `game_mode_normal_description`)
gives 8 karts per race. The Coalesced file has `GFxUI.GFxMoviePlayer SplitscreenLayoutYAdjust=-27` and a
`[SystemSettingsSplitScreen2]` section (no 3 or 4 section found). The locally-visible human limit (1 to 4 pads) is
therefore not proven here; see Unknowns.

## 3. Config that the engine uses for networking (Coalesced `Xbox360-ChavoKartEngine.ini`)

| Section | Values |
| --- | --- |
| `[URL]` | `Port=1000`, `PeerPort=1001`, `Protocol=unreal`, `Map=StartMenu.umap`, `TransitionMap=ChavoKartEntry.umap` |
| `[Engine.Engine]` | `NetworkDevice=OnlineSubsystemLive.IpNetDriverLive`, `FallbackNetworkDevice=IpDrv.TcpNetDriver`, `DefaultOnlineSubsystemName=OnlineSubsystemLive.OnlineSubsystemLive`, `GameViewportClientClassName=ChavoKartGame.KartGameViewportClient`, `LocalPlayerClassName=Engine.LocalPlayer`, `NetClientTicksPerSecond=200`, `ConnectionTimeout=15.0`, `InitialConnectTimeout=30.0`, `AckTimeout=1.0`, `KeepAliveTime=1.0`, `NetServerMaxTickRate=30`, `LanServerMaxTickRate=30` |
| `[IpDrv.TcpNetDriver]` | `NetConnectionClassName=OnlineSubsystemLive.IpConnectionLive`, `MaxClientRate=15000`, `MaxInternetClientRate=10000`, `AllowPeerConnections=False`, `AllowPeerVoice=False`, `LanServerMaxTickRate=35`, `ConnectionTimeout=20.0` |
| `[Engine.Player]` | `ConfiguredInternetSpeed=10000`, `ConfiguredLanSpeed=20000` |
| `[IpDrv.OnlineGameInterfaceImpl]` | `LanAnnouncePort=14001`, `LanQueryTimeout=5.0` |
| `[OnlineSubsystemLive.OnlineSubsystemLive]` | `NumLogins=1`, `MaxLocalTalkers=2`, `MaxRemoteTalkers=9`, `LanAnnouncePort=14001`, `LanPacketPlatformMask=1`, `LanQueryTimeout=3.0`, `LanGameUniqueId=1297287213` (0x4D53082D, not the title id 0x475807D7), `bShouldUseMcp=true`, `VoiceNotificationDelta=0.2` |
| `[LiveSock]` | `bUseVDP=true`, `bUseSecureConnections=false`, `MaxDgramSockets=64`, `MaxStreamSockets=16`, `DefaultRecvBufsizeInK=256`, `DefaultSendBufsizeInK=256`, `SystemLinkPort=14000` |
| `[Engine.OnlineSubsystem]` | `RecentPlayersList` (MaxRecentPlayers=100), `[VoIP] bHasVoiceEnabled=true` |
| `[Engine.AccessControl]` (`Xbox360-ChavoKartGame.ini`) | `bAuthenticateClients=True`, `bAuthenticateServer=True`, `bAuthenticateListenHost=True`, `MaxAuthRetryCount=3` |

The image reads the `[LiveSock]` keys by wide-string name (`SystemLinkPort`, `MaxDgramSockets`, `MaxStreamSockets`,
`DefaultRecvBufsizeInK`, `DefaultSendBufsizeInK`, `DefaultQosProbeRetries`, `DefaultQosMaxSimultaneousResponses`,
`ServiceId`, at `0x82233D6C..0x82233EA0`). `DefaultQosProbeRetries`, `DefaultQosMaxSimultaneousResponses` and
`ServiceId` are not in the Coalesced file, so the title's code defaults apply (the values are unknown).

## 4. Native function tables (UE3 `exec*` natives)

Each table is `{char* "UClassexecName", void* fn}` at an 8-byte stride in `.data`. Registration:

| Registrar | Registers | Tables (examples) |
| --- | --- | --- |
| `sub_82A1CD28` | the OnlineSubsystemLive module | `UDownloadableContentEnumeratorLive` `0x83BE3B68`, `ULivePartyChat` `0x83BE3B78..0x83BE3BB0`, `UOnlineSubsystemLive` `0x83BE3BC0..0x83BE3FF8` (136 entries); it also calls `sub_829FC208`, `sub_829FC450`, `sub_829FCB88`, `sub_829FCDF0`, `sub_82A04610`, `sub_82A05808`, `sub_829FA500`, `sub_829FA718` and pushes `{name, table}` pairs with `sub_827A3C60` into the list at `0x83CF59D8` |
| `sub_8296C4F8` | IpDrv | `UOnlineGameInterfaceImpl` `0x83BFC8C0..0x83BFC910`, `UMeshBeacon*` `0x83BFC780..0x83BFC808`, `UPartyBeacon*` `0x83BFC960..0x83BFC9F0`, `UOnlineAuthInterfaceImpl`, `AInternetLink` `0x83BFC5E0`, `ATcpLink` `0x83BFC620` |
| `sub_82542F58` | Engine | `UEngine`, `UGameEngine` `0x83BFF140`, `UPlayer` `0x83C00130`, `ULocalPlayer` `0x83C00140`, `UGameViewportClient` `0x83C001D8`, `AActor`, `APlayerController` `0x83C00A70`, `AWorldInfo` `0x83BFEE20`, `AGameInfo` `0x83BFF6B0` |
| (game table) | `AKartGame_Base`, `AKartPawn`, `AKartPathNetwork`, `AUtil`, `UKartScreen_Base` | `0x83BE3010..0x83BE3158`; `UKartScreen_Base::execGetFieldValueByName` `sub_82A46B68` at `0x83BE3148` (see `docs/main-menu.md`) |

Natives of `AKartGame_Base` (all non-network): SetGamePausedBySystemMenuEvent `0x82A43F78`, IsGamePausedBySystemMenuEvent
`0x82A43F30`, SetShowTitleScreen `0x82A43E90`, SetOnLoginChangeStatus `0x82A43DB8`, SetDeviceRemoved `0x82A43D18`,
SetCanUnlockAchievements `0x82A43C30`, SetCanSaveData `0x82A43B90`.

### 4.1 `UOnlineSubsystemLive` natives and the virtual that implements each

The exec is generated UE3 glue: it parses the script parameters, then does `lwz r11,<off>(vtable)`, `bctrl` on `this`.
The vtable consistent with all 119 slots is at `0x82234870` (slot address = `0x82234870 + off`). Checked by
reachability from each virtual (depth 5, direct calls only): `StartOnlineGame` reaches the XSessionStart message,
`EndOnlineGame` XSessionEnd, `RegisterPlayers` XSessionJoinRemote, `GetPlayerNickname` XamUserGetName,
`CanPlayOnline` XamUserCheckPrivilege, and so on. The location where an instance stores this vtable pointer was not
located (stored through `lis`/`addi`, no direct reference).

| Exec (`0x83BE...` entry) | Exec fn | Virtual off | Virtual fn | What the virtual reaches |
| --- | --- | --- | --- | --- |
| ShowLoginUI | `0x829FD0F0` | 328 | `0x82A0D000` | |
| GetLoginStatus | `0x829FD390` | 344 | `0x82A0D170` | XamUserGetSigninState |
| IsGuestLogin | `0x829FD410` | 348 | `0x82A0D1F8` | XamUserGetSigninInfo |
| IsLocalLogin | `0x829FD490` | 352 | `0x82A0D260` | XamUserGetSigninInfo |
| GetUniquePlayerId | `0x829FD510` | 356 | `0x82A0D2D0` | XamUserGetSigninInfo |
| GetPlayerNickname | `0x829FD600` | 360 | `0x82A0D328` | XamUserGetName |
| CanPlayOnline | `0x829FD6A8` | 364 | `0x82A0D3C8` | XamUserCheckPrivilege |
| CanCommunicate | `0x829FD728` | 368 | `0x82A0D460` | XamUserCheckPrivilege |
| CanDownloadUserContent | `0x829FD7A8` | 372 | `0x82A0D510` | XamUserCheckPrivilege |
| CanViewPlayerProfiles | `0x829FD8A8` | 380 | `0x82A0D5C0` | XamUserCheckPrivilege |
| CanShowPresenceInformation | `0x829FD928` | 384 | `0x82A0D670` | XamUserCheckPrivilege |
| CanPurchaseContent | `0x829FD828` | 376 | `0x82A0D720` | XamUserCheckPrivilege |
| AreAnyFriends | `0x829FDA60` | 392 | `0x82A0D818` | XamUserAreUsersFriends |
| ShowFriendsUI | `0x829FDC30` | 400 | `0x82A0D940` | XamShowFriendsUI |
| HasLinkConnection | `0x829FE328` | 436 | `0x82A0DE50` | XNetGetEthernetLinkStatus (via `0x82A0DE50` area) |
| IsControllerConnected | `0x829FE400` | 444 | `0x82A0DF00` | |
| GetNATType | `0x829FE480` | 448 | `0x82A0DF20` | XLiveBase message 0x58006, XamUserGetSigninState |
| CreateOnlineGame | `0x829FE7E8` | 472 | `0x82A0E268` | helper `0x82A0ECB0` calls XSession create wrapper `0x82B76128`; also XUserSetContextEx/SetPropertyEx, XamUserGetName/SigninInfo/SigninState |
| UpdateOnlineGame | `0x829FE8D8` | 476 | `0x82A0EF18` | XSessionModify wrapper, XNetQosListen, SetContext/Property |
| DestroyOnlineGame | `0x829FE9E0` | 480 | `0x82A0F0B0` | XSessionLeaveLocal, XSessionDelete (via `0x82A0F2A0`), XNetQosListen |
| FindOnlineGames | `0x829FEA68` | 484 | `0x82A0F5F0` | XSessionSearch wrappers (`0x82A0F518`, `0x82A0F860`), XUserReadStats |
| CancelFindOnlineGames | `0x829FEB20` | 488 | `0x82A0FF30` | |
| ReadPlatformSpecificSessionInfo | `0x829FEB68` | 492 | `0x82A0FFD8` | |
| BindPlatformSpecificSessionToSearch | `0x829FEDA8` | 500 | `0x82A10378` | |
| FreeSearchResults | `0x829FEE90` | 504 | `0x82A105B0` | |
| JoinOnlineGame | `0x829FEF20` | 508 | `0x82A10660` | helper `0x82A107A8` calls XSession create wrapper `0x82B76128`; SetContext/Property |
| GetResolvedConnectString | `0x829FF048` | 512 | `0x82A109C0` | XNetXnAddrToInAddr (`0x82B74BF0`) |
| RegisterPlayers | `0x829FF268` | 520 | `0x82A10D18` | XSessionJoinRemote wrapper `0x82B764F8` (helper `0x82A10A88`) |
| UnregisterPlayers | `0x829FF448` | 528 | `0x82A111D0` | XSessionLeaveRemote wrapper `0x82B76650` |
| RecalculateSkillRating | `0x829FF568` | 532 | `0x82A11460` | XSessionModifySkill |
| MigrateOnlineGame | `0x829FF688` | 536 | `0x82A11650` | XSessionMigrateHost, XNetQosListen |
| JoinMigratedOnlineGame | `0x829FF740` | 540 | `0x82A117F0` | XSessionMigrateHost |
| ReadProfileSettings / WriteProfileSettings | `0x829FF868` / `0x829FF920` | 544 / 548 | `0x82A11BF8` / `0x82A12358` | XamUserReadProfileSettings / WriteProfileSettings |
| SetOnlineStatus | `0x829FFE48` | 572 | `0x82A15180` | XUserSetContextEx/SetPropertyEx |
| ShowInviteUI | `0x82A00008` | 576 | `0x82A151D0` | XamShowGameInviteUI |
| UnlockAchievement | `0x82A00568` | 600 | `0x82A15628` | XGI 0x000B0008 (wrapper `0x830DA900`) |
| StartOnlineGame | `0x82A00728` | 608 | `0x82A158B0` | XSessionStart wrapper `0x82B767D8`, XNetQosListen |
| EndOnlineGame | `0x82A007B0` | 612 | `0x82A15B08` | XSessionEnd wrapper `0x82B76878` |
| RegisterForArbitration | `0x82A00838` | 616 | `0x82A15F88` | XSessionArbitrationRegister |
| AcceptGameInvite | `0x82A008C0` | 620 | `0x82A16188` | XSessionJoinLocal wrapper `0x82B76448` (via `0x82A16240`) |
| ReadFriendsList | `0x82A00978` | 624 | `0x82A16488` | XLiveBase message 0x58020 |
| GetFriendsList | `0x82A00A80` | 628 | `0x82A166D0` | |
| RegisterLocalTalker / RegisterRemoteTalker | `0x82A02120` / `0x82A02220` | 692 / 700 | `0x82A17268` / `0x82A17410` | XamUserGetSigninState (local) |
| MuteRemoteTalker / UnmuteRemoteTalker | `0x82A02590` / `0x82A02698` | 724 / 728 | `0x82A17830` / `0x82A17918` | XLiveBase 0x5800D (mute); unmute calls both 0x5800D and 0x5800E wrappers |
| StartNetworkedVoice / StopNetworkedVoice | `0x82A027A0` / `0x82A02830` | 732 / 736 | `0x82A17A48` / `0x82A17A70` | |
| ReadOnlineStats / WriteOnlineStats / FlushOnlineStats | `0x82A02C88` / `0x82A03100` / `0x82A031F0` | 760 / 780 / 784 | `0x82A180E0` / `0x82A18948` / `0x82A18A58` | XUserReadStats / XSessionWriteStats / XSessionFlushStats |
| SendGameInviteToFriend(s) | `0x82A034D8` / `0x82A03600` | 796 / 800 | `0x82A19468` / `0x82A19508` | `0x82A19508` calls the app-252 wrapper `0x82B75008` (message id not a constant) |
| UnlockAvatarAward | `0x82A03E58` | 824 | `0x82A1AB50` | XGI 0x000B0071 |
| QueryAvailableDownloads | `0x82A01790` | 668 | `0x82A170F0` | XLiveBase 0x58009 |
| Login, Logout, ShowGuideUI | `0x829FD190`, `0x829FD310`, `0x829FDFD8` | 332, 340, 424 | all `0x824147C8` (shared stub) | none |
| AutoLogin | `0x82A34078` is the exec target, a shared stub used by several unrelated natives | | | none |

Other execs relevant to sessions, same table, no `UOnlineSubsystemLive` virtual involved:

| Native | Address |
| --- | --- |
| `UGameEngine::execCreateNamedNetDriver` | `0x8258E2F8` |
| `UGameEngine::execDestroyNamedNetDriver` | `0x8258E400` |
| `UGameEngine::execGetOnlineSubsystem` | `0x825A5E20` |
| `UGameEngine::execHasSecondaryScreenActive` / `GetDLCManager` / `GetDLCEnumerator` | `0x825A5F10` / `0x825A5E60` / `0x825A5EB8` |
| `UEngine::execHasNetworkConnection` | `0x8258E650` |
| `UEngine::execLaunchURL` | `0x82587DF8` |
| `UEngine::execIsSplitScreen` | `0x8258E7A0` |
| `ULocalPlayer::execSpawnPlayActor` | `0x8257BC00` |
| `ULocalPlayer::execSendSplitJoin` | `0x8257BD30` |
| `UPlayer::execSwitchController` | `0x8257C9D8` |
| `APlayerController::execClientTravel` | `0x82574920` |
| `APlayerController::execGetServerNetworkAddress` / `GetPlayerNetworkAddress` | `0x82574840` / `0x82574760` |
| `APlayerController::execSetNetSpeed` | `0x82574690` |
| `APlayerController::execHasPeerConnection` | `0x82576230` |
| `APlayerController::execTellPeerToTravel` / `TellPeerToTravelToSession` / `PeerTravelAsHost` | `0x825751B8` / `0x82575248` / `0x825750E0` |
| `APlayerController::execGetPlayerControllerFromNetId` | `0x82575F68` |
| `APlayerController::execIsPlayerMuted` | `0x82575E70` |
| `AGameInfo::execGetNetworkNumber` / `RejectLogin` / `ResumeLogin` / `PauseLogin` / `GetNextPlayerID` / `SwapPlayerControllers` / `SetBandwidthLimit` | `0x825876B8` / `0x825878D8` / `0x82587860` / `0x825877B8` / `0x82587990` / `0x825879B8` / `0x82587AF0` |
| `AWorldInfo::execGetAddressURL` / `GetLocalURL` / `BeginHostMigration` / `NotifyMatchStarted` | `0x82590310` / `0x82590028` / `0x825910A0` / `0x82590388` |
| `AWorldInfo::execSeamlessTravel` / `IsInSeamlessTravel` / `PrepareMapChange` / `CommitMapChange` | `0x82590770` / `0x825908D0` / `0x82590540` / `0x82590710` |

### 4.2 IpDrv natives (stock LAN/beacon implementation, `UOnlineGameInterfaceImpl`)

CreateOnlineGame `0x829653C8`, DestroyOnlineGame `0x829654B8`, FindOnlineGames `0x82965540`, CancelFindOnlineGames
`0x82A34150` (shared stub), FreeSearchResults `0x825419D8`, JoinOnlineGame `0x829655F8`, GetResolvedConnectString
`0x82965720`, StartOnlineGame `0x82965840`, EndOnlineGame `0x829658C8`, ReadPlatformSpecificSessionInfo `0x82965950`,
BindPlatformSpecificSessionToSearch `0x82965A88`; `UOnlineSubsystemCommonImpl::execIsPlayerInSession` `0x82966438`.
`UMeshBeaconHost` (InitHostBeacon `0x82963AE0`, RequestClientBandwidthTest `0x82963B60`, TellClientsToTravel
`0x82963DB8`, ...), `UMeshBeaconClient` (RequestConnection `0x82963598`, BeginBandwidthTest `0x82965540`),
`UPartyBeaconHost` (InitHostBeacon `0x82966F10`, TellClientsToTravel `0x829674A8`, ...), `UPartyBeaconClient`
(RequestReservation `0x829668B8`). The configured subsystem is `OnlineSubsystemLive`, whose virtuals above replace
these for the game interface; which of the two a LAN match would use is unproven (Unknowns).

## 5. XDK layer (statically linked, anonymous)

### 5.1 Layout of the XNet wrappers

Import thunks live at `0x834DA1D4..0x834DB314` (the `__imp__` table in `generated/default/el_chavo_kart_init.cpp`).
The title's own wrappers take `XNCALLER_TITLE` (`li r3,1`) and tail-jump the thunk. They are one contiguous block
`0x82B74868..0x82B74D78`.

| Wrapper | NetDll function | Direct callers (guest) |
| --- | --- | --- |
| `0x82B74868` | WSAStartup | `0x82B74918` |
| `0x82B74AE8` | XNetStartup | `0x82B74BA8` (entry `0x82A1E288` also reaches it) |
| `0x82B74930` | socket | `0x82979EF0`, `0x82979F68`, `0x82A1E5E8`, `0x82A1E688`, `0x82B43388`, `0x82F50EB0`, `0x82F50F90`, `0x832B83C0` |
| `0x82B749D8` | bind | `0x829796A0`, `0x82A1E178`, `0x82F50F90` |
| `0x82B74AA0` | sendto | `0x82979968` |
| `0x82B74A68` | recvfrom | `0x82979A10` |
| `0x82B74A30` | select | `0x82979768`, `0x82979810`, `0x82979AA0`, `0x82B43388` |
| `0x82B74968` | ioctlsocket | `0x82979810`, `0x82979BA8`, `0x82B18CB8`, `0x82B43388`, `0x82B435A8`, `0x82F50F90`, `0x832B83C0` |
| `0x82B749F0` / `0x82B74A18` / `0x82B74A08` / `0x82B74A88` / `0x82B74A50` | connect / accept / listen / send / recv | `0x829796D0`.. (FSocket methods); other users `0x82B43388`, `0x82F50EB0`, `0x832B83C0`, `0x82F51080`, `0x82F51138`, `0x82F511D0`, `0x82F51298`, `0x832B8560`, `0x832B85B8`, `0x82B43648`, `0x82B436A8` |
| `0x82B74BC0` | XNetRandom | `0x829E56D8`, `0x829FA1D8`, `0x82A19200` |
| `0x82B74D10` | **XNetGetTitleXnAddr** | `0x82A1E8E0` |
| `0x82B74BD0` | **XNetRegisterKey** `(pxnkid, pxnkey)` | `0x82A06310` (loads `info+44` as the key) |
| `0x82B74BE0` | XNetUnregisterKey | `0x82A063A8`, `0x82A0F2A0` |
| `0x82B74BF0` | **XNetXnAddrToInAddr** | `0x82A06270`, `0x82A109C0`, `0x82A1E970` |
| `0x82B74C20` | XNetInAddrToXnAddr | `0x82A1E9F8` |
| `0x82B74C08` | XNetServerToInAddr | `0x829F9C78` |
| `0x82B74C38` | XNetUnregisterInAddr | `0x829F9988`, `0x829F9FA8` |
| `0x82B74C48` | XNetConnect | `0x829F9C78` |
| `0x82B74C58` | XNetDnsLookup | `0x82A1E710`, `0x82B43388`, `0x832B83C0` |
| `0x82B74C80` | **XNetQosListen** | `0x82A0E4F0`, `0x82A0EF18`, `0x82A0F0B0`, `0x82A11650`, `0x82A158B0` |
| `0x82B74CA0` | **XNetQosLookup** | `0x82A08F38`, `0x82A0E7A8` |
| `0x82B74D00` | XNetQosRelease | `0x829FAB90` |
| `0x82B74D28` | XNetQosGetListenStats | `0x82A1A8F8` |
| `0x82B74D38` | XNetSetSystemLinkPort | `0x82A1E288` |
| `0x82B74D20` | XNetGetEthernetLinkStatus | `0x82A0DE50`, `0x82A1DAF8` |

`0x82A06270` (`XnAddrToInAddr(info+8, info+0, &out)`), `0x82A06310` (`XNetRegisterKey(info, info+44)`) and `0x82A063A8`
(`XNetUnregisterKey`) first call a virtual (slot 8) of the object at `0x83CE3534` and do nothing unless it returns
non-zero. They have no direct callers: they are virtual methods. The layout `sessionID 8 + XNADDR 36 + key 16` is
`XSESSION_INFO`, as in the XDK.

Socket vtables (both indirect): the base socket class vtable around `0x8221B840` (bind `0x829796A0`, connect
`0x829796D0`, listen `0x82979738`, select `0x82979768`, ioctl `0x82979810`, accept `0x82979898/0x82979900`, sendto
`0x82979968`, send `0x829799B0`, recvfrom `0x82979A10`, recv `0x82979A60`) and the Live socket class vtable at
`0x82234490` (slot 0 bind `0x82A1E178`, slot 22 `0x82A1E288` SetSystemLinkPort+XNetStartup wrapper, create-socket
`0x82A1E5E8`, GetTitleXnAddr `0x82A1E8E0`, XnAddrToInAddr `0x82A1E970`, InAddrToXnAddr `0x82A1E9F8`, DnsLookup
`0x82A1E710`; the middle slots are the base class functions `0x82979xxx`).

### 5.2 XamUser* wrappers

| Wrapper | Imported function | Direct callers (guest) |
| --- | --- | --- |
| `0x830DAB48` | XamUserGetSigninState | `0x829FA928`, `0x82A0BF30`, `0x82A0D170`, `0x82A0ECB0`, `0x82A0EF18`, `0x82A118F8`, `0x82A13D00`, `0x82A154C8`, `0x82A158B0`, `0x82A15B08`, `0x82A16240`, `0x82A17268` |
| `0x830DAB58` | XamUserCheckPrivilege | `0x82A0D3C8`, `0x82A0D460`, `0x82A0D510`, `0x82A0D5C0`, `0x82A0D670`, `0x82A0D720` |
| `0x830DB0B8` | XamUserGetSigninInfo | `0x829FA988`, `0x82A04F30`, `0x82A0B720`, `0x82A0D1F8`, `0x82A0D260`, `0x82A11BF8`, `0x82A12358`, `0x82A13780`, `0x82A14F38`, `0x82A16240`, `0x82A1A9F0`, `0x82A1C5B0` |
| `0x830DA2C0` / `0x830DA408` / `0x82B77430` | XamUserGetXUID (two wrappers, one profile-settings wrapper) | `0x82A0AAC8` / `0x82A1BB60` / `0x82A12358` |
| `0x830DAB40` | XamUserGetName | `0x82A0D328`, `0x82A0E268` |
| `0x830DAB50` | XamUserAreUsersFriends | `0x82A0D7B8`, `0x82A0D818` |
| `0x830DB0B0` | XamNotifyCreateListener | `0x82A06438`, `0x82A0B240` |
| `0x830D8E28` / `0x830D8E88` | XamShowFriendsUI / XamShowGameInviteUI | `0x82A0D940` / `0x82A151D0` |
| `0x831041F8` / `0x83101270` | XamVoice* group (IsActiveProcess, HeadsetPresent, ...) | `0x83101270`, `0x831047B0`, `0x83104988`, `0x83104A00` / `0x83101C40` |

### 5.3 XMsgStartIORequest (XGI / XLiveBase) wrappers

Application ids: 251 (`0xFB`) XGI, 252 (`0xFC`) XLiveBase, 250 (`0xFA`) XMP. Buffer sizes are the `li r7,N` before the
call. Names are the SDK's (`xgi_app.cpp`) for the same ids; the argument order was verified for 0x000B0010 only (the
buffer store order in `0x82B76128` matches the SDK's field order).

| Wrapper | Message | Buffer | SDK name for this id | Wrapper callers |
| --- | --- | --- | --- | --- |
| `0x82B76128` | `0x000B0010` | 28 | XSessionCreate | `0x82A0ECB0`, `0x82A107A8` |
| `0x82B76398` | `0x000B0011` | 16 | XSessionDelete | `0x82A0F2A0` |
| `0x82B76448` / `0x82B764F8` | `0x000B0012` | 20 | JoinLocal / JoinRemote | `0x82A16240` / `0x82A09488`, `0x82A10A88`, `0x82A10D18` |
| `0x82B765A8` / `0x82B76650` | `0x000B0013` | 20 | LeaveLocal / LeaveRemote (not in the SDK) | `0x82A0F2A0` / `0x82A10FB0`, `0x82A111D0` |
| `0x82B767D8` | `0x000B0014` | 16 | XSessionStart | `0x82A158B0` |
| `0x82B76878` | `0x000B0015` | 16 | XSessionEnd | `0x82A15B08` |
| `0x82B76928` | `0x000B0016` | 32 | XSessionSearch | `0x82A0F518`, `0x82A0F860` |
| `0x82B762D8` | `0x000B0018` | 16 | XSessionModify | `0x82A18BA8` |
| `0x82B766F8` | `0x000B001A` | 32 | XSessionArbitrationRegister | `0x82A15F88` |
| `0x82B76A10` | `0x000B001B` | 20 (SDK asserts 32) | XSessionSearchByID | `0x82A0C330` |
| `0x82B77768` | `0x000B001D` | 24 | XSessionGetDetails | `0x82A1A8F8` |
| `0x82B772D0` | `0x000B001E` | 24 | XSessionMigrateHost | `0x82A118F8` |
| `0x82B77138` | `0x000B001F` | 24 | XSessionModifySkill | `0x82A11460` |
| `0x82B76B98` | `0x000B0025` | 24 (SDK asserts 20) | XSessionWriteStats | `0x82A15D58`, `0x82A18948` |
| `0x82B76AF0` | `0x000B0026` | 24 (SDK asserts 20) | XSessionFlushStats | `0x82A18A58` |
| `0x830DA9F0` / `0x830DAA88` | `0x000B0006` / `0x000B0007` | 24 / 32 | XUserSetContextEx / SetPropertyEx | `0x82A0E078`, `0x82A0E1E8` / `0x82A0E0F0` |
| `0x830DA900` | `0x000B0008` | 8 | XUserWriteAchievements (already used by the port) | `0x82A15628` |
| `0x830DA978` | `0x000B0071` | 8 | not in the SDK | `0x82A1AB50` |
| `0x830DABE8` | `0x000B0021` | 28 | XUserReadStats | `0x82A08D80`, `0x82A180E0`, `0x82A1A288` |
| `0x82B77898` | app 252 `0x00058009` | 16 | not in the SDK | `0x82B77968` |
| `0x82B74D78` .. `0x82B77510` via XMsgInProcessCall (`0x834DA4C4`) | app 252 `0x58020` (`0x82B74D78`), `0x58023` (`0x82B74ED8`), `0x58019` (`0x82B752F8`), `0x58046` (`0x82B75478`), `0x5801E` (`0x82B755B0`), `0x58006` (`0x82B76D28`), `0x58004` (`0x82B76ED0`), `0x5800E` (`0x82B77230`), `0x5800D` (`0x82B77278`), `0x58035` (`0x82B77478`, `0x82B77510`) | | | callers: `0x82A16488`, `0x82A0C210`, `0x82A083C0`, `0x82A0B240`, `0x82A083C0`, `0x82A0DF20`/`0x82A1EAB8`/`0x82A1EED8`, all of the above, `0x82A0D8F8`/`0x82A17918`/`0x82A17D68`, `0x82A17830`/`0x82A17918`, `0x82A12DE8`/`0x82A14670`/`0x82A198A8`/`0x82A139D0` |
| `0x82B75008`, `0x82B756F0`, `0x82B759D0`, `0x82B75C58`, `0x82B75EE8` | app 252, message built from a value loaded from memory (not a constant), buffer 40 | 40 | | `0x82A19508`, `0x82A096E8`/`0x82A13048`, `0x82A198A8`, `0x82A148B8`, `0x82A08C58` |
| `0x82B793D0` / `0x82B79478` | app 250 `0x0007001A` (StartIORequestEx) / `0x0007001B` (InProcess) | 12 | XMP | `0x82B79200`, `0x82B792D8` / `0x82B794E0` |

Strings that name the stock Live calls the same TU makes (UTF-16 in `.rdata`, `0x8223265C..0x8223316C`):
`XSessionSearch()`, `XSessionCreate()`, `XSessionMigrate()`, `XSessionArbitrationRegister()`, `XSessionLeaveRemote()`,
`XSessionJoinRemote()`, `XSessionDelete()`, `XSessionModifySkill()`, `XSessionStart()`, `XSessionEnd()`,
`XSessionJoinLocal()`, `XSessionFlushStats()`, `XSessionModify()`, `XInviteSend()`, `XInviteGetAcceptedInfo/XSessionSearchByID()`,
`XPartySendGameInvites`, `XFriendsCreateEnumerator()`, `XUserReadStats()`, `XUserWriteAchievements()`,
`XOnlineStartup() failed with 0x%X`, `XNetStartup() failed with 0x%X`, `LSP Host Resolution`, NAT strings
`NTSC`, `MODERATE`, `STRICT`, and `PlaylistId=%d&NumPlayers=%d&TitleID=%d&...` (the MCP/TMS web calls).

## 6. What the SDK runtime already provides

`out/rexglue-sdk-src` (patched by `patches/rexglue-sdk`; patches that touch xam user/net: 0012, 0024, 0040, 0041).

| Area | Behaviour | Gap |
| --- | --- | --- |
| Sockets (`NetDll_socket/closesocket/shutdown/setsockopt/ioctlsocket/bind/connect/listen/accept/select/recv/recvfrom/send/sendto/__WSAFDIsSet`, `WSAStartup`, WSA events, `inet_addr`) | real host sockets (`XSocket`); `IPPROTO_VDP` (254) is mapped to plain UDP, so a VDP datagram goes out without any VDP header | `getsockname`, `getsockopt` are `REX_EXPORT_STUB` (the title imports both) |
| `XNetStartup/Cleanup/Random/GetTitleXnAddr/GetEthernetLinkStatus/SetSystemLinkPort` | `XNetStartup` stores the params and returns 0. `XNetGetTitleXnAddr` returns `127.0.0.1`, `inaOnline=0`, MAC `CC:CC:CC:CC:CC:CC`, status STATIC. `GetEthernetLinkStatus` returns 0 (no link). `SetSystemLinkPort` returns 1 | no real local address, link reported down |
| `XNetXnAddrToInAddr`, `XNetInAddrToXnAddr` | both return 1 without writing | no address mapping |
| `XNetDnsLookup` | returns a dns struct with `status=1` (error) and signals the event | |
| `XNetQosListen` | returns `X_ERROR_FUNCTION_FAILED` | |
| `XNetQosServiceLookup`, `XNetQosRelease` | success with an empty result | |
| `XNetRegisterKey`, `XNetUnregisterKey`, `XNetServerToInAddr`, `XNetConnect`, `XNetUnregisterInAddr`, `XNetQosLookup`, `XNetQosGetListenStats` | `REX_EXPORT_STUB` (no implementation) | all missing |
| `XamUserGetSigninState` | 0 for user 0 only if the profile is signed out; `user_profile::signin_state()` is hard-coded `1` (signed in locally, not Live); guest pads 1-3 return 1 | never Live-signed (2) |
| `XamUserGetSigninInfo`, `XamUserGetXUID`, `XamUserGetName` | user 0 profile; users 1-3 are "guest users" while a pad is assigned (patch 0040, XUID = profile XUID + index) | |
| `XamUserCheckPrivilege` | writes 0 (everything denied) | `CanPlayOnline` etc. will be false |
| `XamUserAreUsersFriends` | 0 friends | |
| `XamSessionCreateHandle` / `XamSessionRefObjByHandle` | handle `0xCAFEDEAD`, object `0xDEADF00D` | |
| XGI (app `0xFB`) | cases 0x000B0006/7/8/10/11/12/14/15/16/18/19/1A/1B/1C/1D/1E/1F/20/21/25/26/36/3D/41/60/65 only log and return success (0x41 returns failure); `0xB0008` really unlocks achievements | nothing creates session state, no results written. Not handled: `0x000B0013`, `0x000B0071`; they fall through to the default (see `xgi_app.cpp`) |
| XLiveBase (app `0xFC`) | handles 0x58004 (logon id = 1), 0x58006 (NAT = open), 0x58007 (fail 0x80151802), 0x58020 (friends enumerate, returns failure), 0x58023 (invite info, failure), 0x58046, 0x58037 (success) | not handled (logged as `Unimplemented XLIVEBASE message`, failure): 0x58009, 0x58019, 0x5801E, 0x5800D, 0x5800E, 0x58035 |
| `XamShowGameInviteUI`, `XamShowFriendsUI`, `XamShowPlayersUI`, ... | `REX_EXPORT_STUB` | |
| `XNetLogonGetTitleID` | `REX_EXPORT_STUB` | |
| Voice | `XamVoiceCreate/Close/HeadsetPresent/IsActiveProcess` exported, `XamVoiceSubmitPacket` stub | |

The ssf2t project (`ssf2t-hd-remix-recompiled`) overrode the title's own wrappers, not the SDK imports:
`player_match_guest.h` lists per-image addresses of `kSigninState`, `kCheckPrivilege`, `kSigninInfo`, `kGetXuid`,
`kTitleXnAddr`, the XSession wrappers (`kSessionCreate`, `Delete`, `JoinLocal/Remote`, `LeaveLocal/Remote`,
`ArbitrationRegister`, `Start`, `End`, `SearchEx`, `WriteStats`, `MigrateHost`) and the XNet wrappers (`kXnAddrToInAddr`,
`kInAddrToXnAddr`, `kQosListen`, `kQosLookup`, `kSocket`, `kBind`, `kRecvFrom`, `kSendTo`, ...), and installed them with
`REX_HOOK_RAW(base013_sub_XXXXXXXX)`. Every wrapper above has the same structure in this title, so the same technique
applies.

## 7. UE3 net layer facts

- `Engine.Engine.NetworkDevice` is `OnlineSubsystemLive.IpNetDriverLive`; class strings `UIpNetDriverLive` (`0x82233C44`) and
  `UIpConnectionLive` (`0x82233C20`) are in the image; `NetConnection` is an FName at `0x8202C088`.
  `UGameEngine::execCreateNamedNetDriver` exists (`0x8258E2F8`). `IpDrv` and `WinDrv` are `NetNativePackages`.
- `APlayerController` / `AGameInfo` / `AWorldInfo` natives for login, travel, seamless travel, host migration are
  linked (table above), so the stock UE3 server/client code is in the binary.
- The game's `KartGameViewportClient` is the only viewport class; `LocalPlayerClassName=Engine.LocalPlayer`.
- A race is started through the `open`/`start` console command with URL options, from a menu level (`StartMenu.umap`),
  so the engine path the game uses is "load map with options", not "connect to a URL".

## 8. Unknowns and the runtime evidence that resolves each

DEBUG_SAULO telemetry should be added behind the existing `[DEBUG_SAULO]` tag convention (see
`src/debug_telemetry.cpp`) and removed with it. "Hook" below means a logging-only `REX_HOOK_RAW` on the named guest
function that calls the original.

| # | Unknown | Evidence needed (log) | Where |
| --- | --- | --- | --- |
| U1 | Whether any code path ever calls the Live session virtuals, `CreateNamedNetDriver`, or the XSession wrappers during normal play | one line per call: guest function, caller `lr`, args | log hooks on `0x82A0E268`, `0x82A10660`, `0x82A158B0`, `0x82A15B08`, `0x82A0F5F0`, `0x8258E2F8`, `0x82B76128`, `0x82B76448`, `0x82B764F8`, `0x82B767D8` |
| U2 | Exact `XSessionCreate` flags and slots the stock code uses for a host/join | `r3..r10` of `0x82B76128` (flags, user, public, private, nonce ptr, info ptr), plus the 28-byte buffer passed to `XMsgStartIORequest` | hook `0x82B76128` after forcing a call (see U7) |
| U3 | What `bIsLanMatch` does in `CreateOnlineGame` (does it call the LAN beacon in IpDrv `0x829653C8`, or XSessionCreate with a system-link flag?) | the byte `OnlineGameSettings.bIsLanMatch` read at entry of `0x82A0E268` and the callee sequence | log the settings object (r5 / r6 of the exec) and the wrapper calls in order; also hook `0x829653C8` |
| U4 | Whether `XNetRegisterKey` virtual `0x82A06310` is used for a LAN game and what `sessionID`/`key` it gets | entry/exit log of `0x82A06270`, `0x82A06310`, `0x82A063A8` and the object at `0x83CE3534` | hook those three |
| U5 | What the Live socket class does with `bUseVDP=true`: does it send the 2-byte VDP header itself, or rely on the XNet VDP layer? | first 16 bytes of every `sendto` payload and its size, and `recvfrom` | log in `0x82979968`, `0x82979A10` (or `NetDll_sendto/recvfrom` in the SDK) |
| U6 | Whether `IpNetDriverLive` works over plain Winsock after the stubs are replaced (the stock packet handshake) | `CreateNamedNetDriver` args, the `NetDriver`'s `InitConnect/InitListen` results, first packets | hook `0x8258E2F8`, then `0x82A1E178` bind, `0x82A1E5E8` create-socket results |
| U7 | The reachable trigger to exercise U1..U6 (nothing in script calls them) | a one-shot debug hook that invokes the exec from a menu item or hotkey; or a console command `open <map>?listen` (via the existing `UEngine::execLaunchURL` `0x82587DF8` or `ConsoleCommand`) | add a temporary DEBUG_SAULO hotkey that runs `LaunchURL`/`ConsoleCommand` and logs the result |
| U8 | Do `KartPawn` / `KartPC_*` / `KartGame_*` replicate state (Net flags on properties, `RemoteRole`) | per-class `ClassReps` / `bNetTemporary` / `RemoteRole` of spawned pawns when the world runs as listen server | read after U7 by logging `AActor::GetNetMode` and `RemoteRole` of `KartPawn` instances |
| U9 | Maximum local humans in Exhibition (1 to 4?) and how the 4th controller is created | the `NumHumanKarts` value in the `open` URL, the number of `LocalPlayer` objects, `CreatePlayer` ControllerIds | log the console string passed by `KartProgressManager_Base.LoadRace` (hook the `ConsoleCommand` exec in `sub_82574920`'s neighbours, `0x82589470` APlayerController ConsoleCommand and `0x8257B4A0` GameViewportClient ConsoleCommand) |
| U10 | Names of the arguments of the `XSession*` wrappers beyond 0x000B0010 | register dump at each wrapper entry during one hosted match | the wrappers in 5.3 |
| U11 | Whether `ULivePartyChat`, `MeshBeacon`, `PartyBeacon` are ever instantiated | construct/Init log of the objects, or the exec hits | hook `0x82963AE0`, `0x82966F10`, `0x829FC988` |
| U12 | Meaning of the middle slots in the Live socket vtable `0x82234490` and which class the vtable belongs to | log a vtable pointer at object construction | the constructor stores it via `lis`/`addi`, find with `findref` of `0x82234490` and log |
| U13 | `getsockname`/`getsockopt` use (stubbed in the SDK) | log of the arguments when the stub is hit | SDK stub log |
| U14 | The packages `Engine.xxx`, `GameFramework.xxx` were only listed, not bytecode-decompiled | not needed for hooks | |

## 9. Recommended architecture (evidence only)

Facts that decide:

- F1 The game script has no online flow, no RPC functions and no `OnlineGameSettings` subclass (section 1).
- F2 Races are configured as local players plus bots by URL options and run in one process (section 2); the sole
  per-race human data is `{Character, ControllerId, TeamID, bIsBot}` per kart.
- F3 The stock UE3 Live session/net-driver code is present and can be called (sections 4, 5, 7), but depends on XNet
  features the SDK stubs (section 6): XNetRegisterKey, XnAddrToInAddr, QoS, XSession state, system-link.
- F4 A ghost-kart mechanism already exists in script (`KartNPCActor`, `KartGhostNPCActor`, `KartGhostNPCSpawner`,
  `KartSpawnPointNPC`), and `KartPathNetwork`/`KartAIController` drive non-player karts.

Ranking:

1. **C, custom sync (recommended first).** The race, items, AI, laps and results are local script. F1 and F2 mean a
   host-authoritative or lockstep design can be added without the UE3 replication layer: send per-kart state and
   events over a transport the port owns, and drive remote karts through the existing non-player kart machinery (F4).
   The cost is on this side only (hooks plus a state encoder). Proven risk: none of F4's reuse has been tested.
2. **B, native packets bridged through a transport layer.** Keeps the stock handshake (`IpNetDriverLive`) but moves the
   bytes over the port's own channel. Needs everything in A except real LAN routing. It depends on U6 and U8 (the
   pawns being replicable at all). F1 makes success uncertain, because nothing in the game was written for it.
3. **A, native UE3 replication over emulated XNet/XSession.** Highest work: all of the missing XNet functions, XSession
   state, QoS and a faithful VDP/XNADDR model (section 6) before a single packet is meaningful, and the result still
   depends on U8. Worth trying only as a short experiment (U6/U7/U8), because it would reuse the most existing code.

Minimum hooks per option (all are guest functions, signatures as proven):

| Option | Hook list |
| --- | --- |
| C | race parameters: `KartProgressManager_Base.LoadRace` / the `open` command (find via U9); the per-frame kart state read/write on `KartPawn` (script-level; locate the native tick by hooking `AKartPawn` natives `0x82A46698`, `0x82A46438`, `0x82A46260`, `0x82A461C8`); the identity wrappers below if a sign-in is wanted: `0x830DAB48` XamUserGetSigninState `(user)`, `0x830DAB58` XamUserCheckPrivilege `(user, mask, *out)`, `0x830DB0B8` XamUserGetSigninInfo `(user, flags, *info)`, `0x830DA2C0`/`0x830DA408` GetXUID |
| B | all of C plus the socket vtable slots: bind `0x82A1E178`, create-socket `0x82A1E5E8`, sendto `0x82979968`, recvfrom `0x82979A10`, select `0x82979768`/`0x82979810`/`0x82979AA0`, ioctl `0x82979BA8`; `CreateNamedNetDriver` `0x8258E2F8`; `GetTitleXnAddr` `0x82B74D10`; `XnAddrToInAddr` `0x82B74BF0` |
| A | all of B plus the Live virtuals in 4.1 (create `0x82A0E268`, join `0x82A10660`, start `0x82A158B0`, end `0x82A15B08`, find `0x82A0F5F0`, register players `0x82A10D18`), the XSession wrappers `0x82B76128`..`0x82B77768` (5.3, XGI 0x000B0010..0x000B0026), `XNetRegisterKey` `0x82B74BD0`, `XNetUnregisterKey` `0x82B74BE0`, `XNetInAddrToXnAddr` `0x82B74C20`, `XNetQosListen/Lookup/Release` `0x82B74C80`/`0x82B74CA0`/`0x82B74D00`, `XNetSetSystemLinkPort` `0x82B74D38`, plus the XLiveBase messages 0x58004/0x58006/0x58020 |

Signatures: the XNet and XamUser wrappers take the same arguments as the XDK functions without the caller id
(`XNetGetTitleXnAddr(XNADDR*)` is `mr r4,r3; li r3,1; b thunk`). `0x82B76128` is
`XSessionCreate(flags r3, userIndex r4, publicSlots r5, privateSlots r6, nonce* r7, XSESSION_INFO* r8, XOVERLAPPED* r9,
HANDLE* r10)`; it returns 87 on bad parameters. The settings object read by the Live create path holds public slots at
`+108` and private slots at `+112` (`0x82A0ECB0`, `0x82A107A8`).

Telemetry to add first, in order: U1, U7 (to get anything to run), U9, U3, U5, U8.

## 10. Gameplay sync status

Written after the game-side scaffolding (`src/online_game_guest.h`, `src/online_game_hook.cpp`, `src/online_race.*`). Sections 1 to 9
above are unchanged. Nothing below was observed at runtime.

What exists:

- `online_game_guest.h`: the addresses of sections 4 to 5 with a four-word code fingerprint each (26 functions, bytes read from
  `default.image.bin`). Each hook group checks its table once against guest memory; on a mismatch every hook of the group calls
  the original and does nothing else.
- `online_game_hook.cpp`, always on, pass-through plus a `[DEBUG_SAULO][ONLINE-GAME]` line per state transition (a key logs
  when its caller/result changed, at most 4 per second per key, 400 lines in all; only caller, argument words and result, never
  tokens or addresses): XSession wrappers (U1/U2/U10), `XNetGetTitleXnAddr`, `XNetXnAddrToInAddr`, `XNetQosListen`,
  `UOnlineSubsystemLive` Create/Find/Join/Start/End/ShowInviteUI, `CreateNamedNetDriver`, `ClientTravel`.
- Title-local LIVE profile (`SigninState`, `CheckPrivilege`, `SigninInfo`, `GetName`, user 0 only), derived from the public
  friend code. It is off unless `ECK_ONLINE_LIVE_PROFILE=1` and Online play is on and signed in. Reason: `GetLoginStatus`
  (`0x82A0D170`) reads `XamUserGetSigninState`, and the script reacts to a change (`OnLoginChangeEvent`, `UpdateLoginStatus`,
  `HasSigninChanged`: "returned to the title screen"); the service signs in after the game has started, so the value would
  change mid-session. `SigninInfo` also swaps the profile XUID, which the save and achievement paths may key on. Neither was
  checked at runtime, so the default stays the SDK's local profile (state 1).
- `ECK_ONLINE_DEBUG=1`: logs room, seat, per-peer path and a dry-run of the kart slots (`L` local, `R` remote human, `B` bot)
  whenever they change.
- `online_race.*` (pure): seat to kart mapping, the datagram format (magic `ER`, version 1: config, phase, kart state, finish,
  result, leave, loaded), and a host-authoritative phase machine (lobby, starting, countdown, racing, finished; player left,
  connection lost, host left, host connection lost). Not connected to the game or the socket.

Not hooked on purpose (arguments or behaviour not proven):

- `0x830DA2C0` / `0x830DA408` (XUID wrappers): large frames, argument layout unknown. The XUID is only reported through
  `XamUserGetSigninInfo`.
- The Exhibition race URL (`NumHumanKarts`, `TotalKarts`, `NumSelectedTrack`, `PlayersArray`, `CharArray`, `IsBotArray`,
  `PrimaryControllerID`): the script that builds it (`SetupExhibitionParams`, `SetupCommonRaceParameters`, `LoadRace`) has no
  address in this audit, and `UEngine::execLaunchURL` / `ConsoleCommand` read the string from script bytecode (an FFrame), which
  cannot be read without running the expression and so consuming it. U9 stays open.
- Kart state: no `AKartPawn` field offset, tick native or `KartPC` input path is proven (U8). The four `AKartPawn` natives in
  section 9 are only candidates.

Runtime evidence needed before the race model can drive karts:

1. U9: the console string of `LoadRace` (hook the `ConsoleCommand` exec and read the FString after the script evaluated it, or
   hook the FString assignment it ends in) to confirm `NumHumanKarts` allows 2 to 4 local players and what a remote human needs.
2. The `AKartPawn` layout: location, rotation, velocity, steering, throttle, lap, checkpoint and place as fields, found by
   dumping a pawn during a race and diffing frames; which native or script tick writes them.
3. Whether a kart can be driven from outside the controller (a `KartGhostNPCActor` or a pawn without a controller), and how
   the AI controller of a bot kart is replaced by network state without becoming a CPU (`IsBotArray` false, no controller).
4. Race start: where the seed (item and AI randomness) is read, so that every PC can use the host's `RaceConfig.seed`.
5. Race end: the native that reports a finish (`NotifyKartFinished`) and the final place, to fill `Finish`.
6. Whether the lap and checkpoint counters can be set, to snap a remote kart that desynchronises.

Menu: nothing patches Scaleform or the Free Race flow. The Local/Online choice, Create Room, Find Room and Invite screens do
not exist in the game; the existing patch (`main_menu_swf.cpp`) only changes one row and an exit command. They need new Scaleform
movie content or a host-drawn overlay (ImGui, as the Ping panel), a way to start a race from room rules (blocked by item 1),
and per-seat character selection fed from the room (the `KartScreen_PlayerSelect*` flow reads one `LocalPlayer` per pad).

## 11. Race flow, probes and kart state (static, second pass)

Still static: nothing here was observed at runtime. Sources: bytecode of the cooked `ChavoKartGame.xxx` and `Engine.xxx` (a small
UE3 bytecode disassembler over the decoded packages; object references 4 bytes big-endian, names 8 bytes), the generated C++ and
`default.image.bin`. Item 1 of section 10 is answered by this section (the build and the consumer of the URL); its runtime value is
still read by probe 1.

### 11.1 Exhibition race URL, end to end

| Step | Where | Facts |
| --- | --- | --- |
| Build | `KartProgressManager_Exhibition.SetupExhibitionParams` | `Params` order: `<prefix[gameMode]><trackNames[trackIndex]>` (the map), `?NumSelectedTrack=`, `?NumHumanKarts=`, `?TotalKarts=`, `?KartAIDiffculty=`, `?GameType=`. `NumHumanKarts` counts `KartURLPlayerData` entries with `bIsBot == false` and is final here (the PlayerSelect screens filled `KartURLPlayerData`); `TotalKarts` is `len(KartURLPlayerData)` |
| Common | `KartProgressManager_Base.SetupCommonRaceParameters` | appends `?IsBotArray=`, `?PlayersArray=`, `?CharArray=`, `?TeamData=`, `?LastTipTextField=`, `?LastTipBGKNumber=`, `?PrimaryControllerID=` to `raceParms` (a `string[]`). Arrays are `-` separated, one entry per kart: `PlayersArray` = `ControllerId`, `CharArray` = `Character`, `IsBotArray` = 1 for CPU, `TeamData` = `TeamID` |
| Start | `KartProgressManager_Base.LoadRace` | `allParmsString` = the `raceParms` entries concatenated; `Command = bUseOpenInsteadOfStart ? "open" : "start"`; `Util.StopAllSounds`; `DataManager.ClearDelegates`; `achievementsManager.ClearDelegates`; then `TheGame.ConsoleCommand(Command @ allParmsString)` (`@` joins with one space). `TheGame` is the `KartGame_Base`. Nothing else loads the map |
| Native | `AActor::execConsoleCommand` `0x82589470` (the same function serves `APlayerController`) | evaluates the FString parameter into its own frame, then calls `this->vtable[75]` (offset 300) with `r3 = FString* result, r4 = this, r5 = const FString* command, r6 = bWriteToLog` |
| Implementation | `sub_826A1F30` | the function in slot 75 of 98 vtables (the Actor family); runs the command through the engine (`GEngine`, global `0x83CE2624`) and returns the output string. The command FString is already evaluated here, so reading it consumes nothing |
| Consume | `KartGame_Base.InitGame`, then `KartProgressManager_Base.InitGame` (`ParseOption`) | `PrimaryControllerID` -> `PrimaryControllerId`. `NumHumanKarts` -> `NumberOfHumanKarts`; `IsBotArray` defaults to `0` x humans then `1` x rest; `TotalKarts` -> `NumberOfTotalKarts` (clamped to 5 only when `WorldInfo.IsConsoleBuild(8)` is true; the meaning of 8 is not resolved); per kart `i < TotalKarts`: `Character = CharArray[i]` (else 0), `ControllerId = PlayersArray[i]`, `TeamID = TeamData[i]`, `bIsBot = IsBotArray[i] == 1`, stored in `KartURLPlayerData`. Also reads `GameType`, `NumSelectedTrack`, `KartAIDiffculty`, `GoBackType`, `NumGameMode`, `LastTipTextField`, `LastTipBGKNumber` |
| Local players | `KartGame_Base.SetupLocalPlayers` | `required = ProgressManager.NumberOfHumanKarts`; removes `GamePlayers[i]` for `i >= required` (`GameViewport.RemovePlayer`), creates the missing ones with `GameViewport.CreatePlayer(4 + i, OutError, false)` (a ControllerId outside the four pads), then walks `KartURLPlayerData` and calls `LocalPlayer.SetControllerId(kartData.ControllerId)`. `GameViewportClient.CreatePlayer`/`RemovePlayer` are script, not natives: they create the `LocalPlayer`, `AddLocalPlayer`, and call the native `ULocalPlayer::SpawnPlayActor` (`0x8257BC00`) only when `bSpawnActor` is true (it is false here) |
| Pawn | `GameInfo.StartHumans` / `StartBots` -> `RestartPlayer` -> `KartGame_MainGame.SpawnDefaultPawnFor` | human (`IsLocalPlayerController()`): character = `KartURLPlayerData[ProgressManager.FindIndexPlayerByControllerId(LocalPlayer.ControllerId)].Character`; CPU: `KartAIController.idBot` index into `KartURLPlayerData`. Pawn = `Spawn(DefaultPawnClass)` (`KartPawn`), `CharInfo = DynamicLoadCharacterInfo(char)`, `CharInfo.PlayerNum = Controller.PlayerNum`, then `Possess`. A CPU is a `KartAIController` from `SpawnBot`/`AddBot("Rival"$n)` for every `bIsBot` entry |

FString layout (32-bit, guest memory big-endian): `{ wchar_t* Data @0, i32 Num @4 (includes the NUL), i32 Max @8 }`, `wchar_t` is UTF-16
big-endian.

Consequence for a remote human: a seat is human only if `IsBotArray` has `0` for it and a `LocalPlayer`/PlayerController exists for its
`ControllerId`; otherwise it gets no character and no controller. The URL a room race has to produce is therefore `NumHumanKarts` = all
humans (local plus remote), `PlayersArray[kart]` = the pad (0) for the local seat and a distinct virtual id (1..3) for each remote seat,
human karts first in `IsBotArray`. This is what `online_probe::PlanDryRun` prints. Whether the engine accepts `CreatePlayer` and
`SetControllerId` for a pad that does not exist, and how the extra split-screen viewports are hidden, is unproven (probe 3).

### 11.2 Hooks added (fingerprint-gated, pass-through on mismatch; expected words read from the image)

| Address | What | Gate (4 words) | Effect |
| --- | --- | --- | --- |
| `0x826A1F30` | `AActor::ConsoleCommand` implementation | `7D8802A6 48A18E91 9421FF20 7C771B78` | reads `r5` before calling the original; logs only `open`/`start` commands carrying `?NumHumanKarts=`, `?PlayersArray=` or `?TotalKarts=` |
| `0x830B53B0` | CRT `srand(seed)`: stores `r3` in per-thread data +20; `rand` `0x830B53E0` is the 214013/2531011 LCG on that word | `7D8802A6 9181FFF8 FBE1FFF0 9421FFA0` | logs seed and caller. Direct callers: `0x823352D0`, `0x82461B58`, `0x8254C7C8`, `0x82FAC7B8`, `0x83282E30` |
| `0x82B74930`, `0x82B74AA0`, `0x82B74A68` | NetDll socket, sendto, recvfrom (title wrappers) | first four words of each (sendto and recvfrom share them) | logs sizes, flags, destination port, caller, result; never addresses or payload |
| `0x830DA2C0`, `0x830DA408` | XamUserGetXUID wrappers | in the identity table | pass-through trace: caller, first words, result |
| `0x830DAB48`, `0x830DB0B8` | XamUserGetSigninState, SigninInfo | already hooked | now also log per caller and result whether LIVE was reported |

`FRand` (`UObject::execFRand` `0x82306478`) and `Rand` (`0x82304960`) call `rand()` and `appRand` (`0x822D9480`). The CRT state is per
thread; script runs on one guest thread, so a shared seed reproduces item and AI draws only if no other code on that thread calls
`rand()` differently on each PC (`rand()` has 30+ direct callers).

LIVE state: the sign-in wrappers are called by `GetLoginStatus` (`0x82A0D170`) and by the Live session virtuals; the script reacts to a
change (`OnLoginChangeEvent`). A race that stays out of the Live virtuals (see the `ue3_net_summary` line) does not need LIVE state;
`ECK_ONLINE_LIVE_PROFILE=1` stays developer-only and off by default.

### 11.3 Log formats

- `[DEBUG_SAULO][ONLINE-XBOX] api=<name> t_ms=<ms since start> phase=conn:<n>/room:<n> seat=<n> args=<...> result=0x<..> calls=<n>` for the
  session, XNet, Live, engine, sign-in and socket hooks (transition-only; 4 lines/s/key and 400 lines for calls, 80 lines for socket traffic).
- `[DEBUG_SAULO][ONLINE-XBOX] ... ue3_net_summary reason=... socket=N sendto=N recvfrom=N CreateNamedNetDriver=N ClientTravel=N
  CreateOnlineGame=N JoinOnlineGame=N XSessionCreate=N XSessionJoinRemote=N verdict=none_active|active:<list>`, printed by
  `OnOnlineRaceStartRequested`, `OnOnlineRaceStartBlocked`, and when a race URL passes while a room seat is held. `none_active` at a race
  start is evidence that the stock UE3 net stack is idle.
- `[DEBUG_SAULO][ONLINE-RACE] url=...`, `url_origin ...`, `url.<field>=...`, `url.problem ...`, `seed=0x.. srand_caller=0x..`: always on,
  once per race start.
- With `ECK_ONLINE_DEBUG=1` (poll at 2 Hz, changes only except `kart`): `players=N expected_humans=M`;
  `slot=<i> user=<ControllerId> human=yes|no controller=0x.. kart=0x.. character=<id> racing_ai=yes|no role=local`;
  `seat=<n> role=local|remote kart_slot=<n> cpu=no` (dry run); `pawns=N`; `state=cinematic|countdown|racing|menu|finished value=n/a clock=<s>`;
  `kart=<id> seat=<n> lap=<n> checkpoint=<node ptr> place=<n>`; `kart=<id> finish_position=<n>`; and
  `kart role=local|remote_designated id= pos= rot= vel= angvel= steer= throttle= brake= boost=bar/level/speed drift=dir/elapsed speed= lap= node= dist= controller=`.

### 11.4 Race state, kart and input (static)

Race state: `KartGame_MainGame.RaceState` (byte, `EKartRaceState`: 0 Cinematic, 1 Start, 2 InProgress, 3 Menu, 4 Finished). `PlayerMove`
advances `racingTime` while it is 2; `EndRace` sets 4 and calls `ProgressManager.OnRaceEnded`. The 3-2-1 countdown is HUD
(`KartScreen_HUDIndividual.UpdateCountdown`, Scaleform), not a property of a game object, so `value=` stays `n/a`. Per kart (`KartPawn`):
`CompletedLaps` (incremented by `KartPawn.CompletedLap`, which calls `MyGame.NotifyLapCompleted` -> `NotifyKismetLapCompleted` and the
`IKartEventListener.OnKartEvent(lapCompleted)` listeners), `CurrentPathNode` and `DistanceInRace` (written by
`KartPawn.UpdatePathNavigation`), `RacePosition` (`RefreshRacePosition`), `RaceFinishPosition` (written by `KartPawn.CheckRaceCompleted`;
`NotifyKartFinished` counts human versus CPU finishes through `Controller.IsA(KartPC_Base)`), `racingTime`. A checkpoint here is a
`KartPathNode` of the path network; `PathDistance` (`KartPathNode+0x290`) is the only numeric proxy.

Input path (cited bytecode): pad -> `KartInput : PlayerInput` (`aForward`, `aTurn`, `aLeftAnalogTrigger`, `aRightAnalogTrigger`, `aLookup2`,
`aTurn2`) -> `KartPC_MainGame` state `PlayerWalking.PlayerMove(DeltaTime)`, which writes `MyKart.KartAccelInput = clamp(aRightAnalogTrigger +
aForward, 0, 1)` and `KartBrakeInput`; steering comes through `KartTurnAxisInput`, read in `KartPawn.UpdateSteering` -> `KartPawn.Tick(DeltaTime)`:
`UpdatePathNavigation`, `UpdateSpeed(dt)`, `UpdateSteering`, `UpdateFloorAlignment`, `UpdateRam`, `UpdateStunAndInvulnerability`; world rays
and suspension through the natives `AKartPawn::PhysXRays` (`0x82A46698`) and `UpdateTireSuspension` (`0x82A46260`). CPU karts write the same
fields in `KartAIController.UpdateInputForAI`. A human PC can enter the `AIRacing` state (`MyKart.bIsRacingAI`), which makes the human kart a
CPU; that state must never be used for a remote seat.

Determinism: every step takes `DeltaTime` (`KartPawn.Tick(float)`, `LastDeltaTime`); speed, `KartMovementQuat`, drift and boost integrate with
it, and collisions (`Bump`, `HitWallKart`, rams) and item/trap logic read the live state of other karts and `rand()`. A variable-timestep,
collision-coupled simulation with per-thread `rand()` does not follow from raw inputs alone, so raw-input-only sync is not sufficient on this
evidence; it supports state authority per kart (each PC simulates its own kart and sends position, rotation, velocity, `KartSpeed`, inputs, lap
and node) with a host-owned race clock and result. Not proven by running it.

Layout (computed from the cooked property chains, declaration order = descending export index, UnrealScript alignment, `UObject` = 0x3C; header
`src/online_probe_layout.h`). Confirmed against code: `Pawn.Controller` 0x1E4 (`APawn::IsHumanControlled`), `Engine.GamePlayers` 0x4B4 and its
num 0x4B8 (`UGameViewportClient::GetPlayerOwner`), `GEngine` global `0x83CE2624` (`UEngine::GetEngine`). Derived, not yet confirmed:
`AActor.Location` 0x54, `Rotation` 0x60, `Physics` 0x90, `Velocity` 0x130, `AngularVelocity` 0x148; `AController.Pawn` 0x1C8, `PlayerNum` 0x1D0;
`APlayerController.Player` 0x354; `ULocalPlayer.Actor` 0x40, `ControllerId` 0x64; `AWorldInfo.Game` 0x430, `PawnList` 0x450; `AKartPawn` (size
0x95C): `KartSteering` 0x544, `KartSpeed` 0x564, `KartMovementQuat` 0x56C, `DriftDirection` 0x5DC, `BoostBar` 0x644, `CurrentBoosterLevel` 0x654,
`CurrentPathNode` 0x674, `DistanceInRace` 0x6E0, `RacePosition` 0x6E8, `RaceFinishPosition` 0x6F0, `CompletedLaps` 0x6F4, `racingTime` 0x6F8,
`KartAccelInput` 0x720, `KartBrakeInput` 0x728, `KartTurnAxisInput` 0x72C, `CharInfo` 0x834, `idPawn` 0x838; `KartGame_MainGame.RaceState` 0x3D4.
Authoritative (simulation state): position, rotation, velocity, `KartSpeed`, `KartMovementQuat`, drift and boost state, `CurrentPathNode`,
`CompletedLaps`. Derived each tick: `DistanceInRace`, `RacePosition`, `FloorAlignedRotation`, `SmoothedVehicleMeshRotation`, animation and camera
quaternions. Inputs are rewritten every tick by `PlayerMove`.

### 11.5 The six missing runtime pieces and their probes

| # | Unknown | Probe | Expected log | Unlocks |
| --- | --- | --- | --- | --- |
| 1 | The string `LoadRace` evaluates | hook `sub_826A1F30` -> `online_probe::OnConsoleCommand` | `[ONLINE-RACE] url=open TA_...?NumHumanKarts=2?...`, `url.NumHumanKarts=2`, `url.PlayersArray=[..]` | URL builder for room races; the human limit |
| 2 | `AKartPawn` fields in guest memory | `PumpGuestProbes` `kart ...` line (one local, one remote-designated, 2 Hz) | `kart role=local id=0 pos=(x,y,z) ... speed=.. lap=..`; speed must rise with throttle | `KartState` encoder/decoder; corrects the derived offsets |
| 3 | A remote kart as a human without a pad, never a CPU | `slot=` lines (GamePlayers, controller, `racing_ai`) after a forced race with the dry-run URL | `slot=1 user=1 human=yes controller=0x.. kart=0x.. racing_ai=no role=local` | virtual `LocalPlayer` per remote seat versus a controllerless pawn |
| 4 | Where item and AI randomness is seeded | hook `sub_830B53B0` (srand) | `[ONLINE-RACE] seed=0x........ srand_caller=0x........` | broadcast `RaceConfig.seed`; see 11.2 for the per-thread limit |
| 5 | Finish and final place | `state=finished` and `kart=<id> finish_position=<n>` (reads `RaceState`, `RaceFinishPosition`) | `state=finished`; `kart=0 seat=0 finish_position=2` | fill `Finish`/`RaceResult` from the game |
| 6 | Lap and checkpoint as settable fields | `kart=<id> seat=.. lap=<n> checkpoint=<node> place=<n>` transitions | `kart=0 seat=0 lap=1 checkpoint=0x.. place=3` | snap a desynchronised remote kart; the script already stores `CompletedLaps++` and `CurrentPathNode`, a native-side write was not tried |

Open: the human limit and `CreatePlayer` for a pad that does not exist; the countdown value; whether `GamePlayers` order equals the human order
of `KartURLPlayerData`; the pawn list order of karts; `getsockname`/`getsockopt` use; the meaning of `IsConsoleBuild(8)`. All `KartPawn` offsets above
are derived and unverified.
