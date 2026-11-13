#pragma once

// Guest object layout the race probes and race telemetry read. Offsets are computed from the cooked property chains
// (declaration order, UnrealScript alignment rules, Object = 0x3C bytes, an FQuat aligned to 16). The AKartPawn chain was
// checked against a real race: KartSpeed equals the velocity's length, racingTime and SavedLocation hold their values.
// Pure: no ReXGlue.

#include <cstdint>

namespace eck::online_probe::layout {

inline constexpr uint32_t kGEngine = 0x83CE2624;  // pointer to the UGameEngine
inline constexpr uint32_t kGWorld = 0x83CE2DD0;   // pointer to the UWorld; WorldInfo = [[[world+80]+60]+0]
inline constexpr uint32_t kWorldLevel = 80;
inline constexpr uint32_t kLevelActors = 60;

// UEngine
inline constexpr uint32_t kEngineGamePlayers = 0x4B4;  // TArray<ULocalPlayer*>: data, num

// UPlayer / ULocalPlayer
inline constexpr uint32_t kPlayerActor = 0x40;
inline constexpr uint32_t kLocalPlayerControllerId = 0x64;

// AActor
inline constexpr uint32_t kActorLocation = 0x54;         // FVector
inline constexpr uint32_t kActorRotation = 0x60;         // FRotator (int pitch, yaw, roll)
inline constexpr uint32_t kActorPhysics = 0x90;          // byte
inline constexpr uint32_t kActorVelocity = 0x130;        // FVector
inline constexpr uint32_t kActorAngularVelocity = 0x148;  // FVector
inline constexpr uint32_t kObjectClass = 0x34;

// AWorldInfo
inline constexpr uint32_t kWorldGame = 0x430;
inline constexpr uint32_t kWorldPawnList = 0x450;

// APawn / AController / APlayerController
inline constexpr uint32_t kPawnController = 0x1E4;
inline constexpr uint32_t kPawnNext = 0x1E8;
inline constexpr uint32_t kControllerPawn = 0x1C8;
inline constexpr uint32_t kControllerPlayerNum = 0x1D0;
inline constexpr uint32_t kPlayerControllerPlayer = 0x354;

// AKartPawn
inline constexpr uint32_t kKartFlags1 = 0x494;  // bIsInitialized = bit 1, bIsRacingAI = bit 22
inline constexpr uint32_t kKartFlags2 = 0x498;  // bGhostMode = bit 3, bIsRaceFinished = bit 21
inline constexpr uint32_t kKartSteering = 0x544;
inline constexpr uint32_t kKartSpeed = 0x564;
inline constexpr uint32_t kKartMovementQuat = 0x570;  // X, Y, Z, W: the heading the kart drives and the mesh follows
inline constexpr uint32_t kKartDriftDirection = 0x5E0;
inline constexpr uint32_t kKartDriftingElapsed = 0x5F0;
inline constexpr uint32_t kKartBoostBar = 0x654;
inline constexpr uint32_t kKartBoosterSpeed = 0x65C;
inline constexpr uint32_t kKartBoosterLevel = 0x664;
inline constexpr uint32_t kKartCurrentPathNode = 0x684;
// The item the kart is holding: 0 for none, then the item's own id. Found by ECK_ITEM_PROBE=1 in a real race — four
// pickups in thirty seconds took it 0 -> 3, 0 -> 2, 0 -> 2, 0 -> 1, and no other word of the kart does that. The four
// neighbours are floats (0x508 and 0x510 are), which is what makes a small integer here unmistakable.
inline constexpr uint32_t kKartHeldItem = 0x50C;
inline constexpr uint32_t kKartDistanceInRace = 0x708;
inline constexpr uint32_t kKartRacePosition = 0x710;
inline constexpr uint32_t kKartRaceFinishPosition = 0x718;
inline constexpr uint32_t kKartCompletedLaps = 0x71C;
inline constexpr uint32_t kKartRacingTime = 0x720;
inline constexpr uint32_t kKartAccelInput = 0x748;
inline constexpr uint32_t kKartBrakeInput = 0x750;
inline constexpr uint32_t kKartTurnAxisInput = 0x754;
inline constexpr uint32_t kKartCharInfo = 0x85C;
inline constexpr uint32_t kKartIdPawn = 0x860;  // the kart's slot in the race URL, on every PC
inline constexpr uint32_t kKartSize = 0x958;
inline constexpr uint32_t kKartInitializedBit = 1;
inline constexpr uint32_t kKartRacingAiBit = 22;
inline constexpr uint32_t kKartRaceFinishedBit = 21;

// KartCharacterInfo, KartPathNode, KartGame_MainGame
inline constexpr uint32_t kCharInfoType = 0x1C4;  // byte
inline constexpr uint32_t kPathNodeDistance = 0x290;  // PathDistance, float
inline constexpr uint32_t kMainGameRaceState = 0x3E0;  // byte, EKartRaceState

// KartScreen_PlayerSelectIndividual: a TMap is 60 bytes and a script delegate 12. ScanPanels still confirms them at runtime.
inline constexpr uint32_t kPanelPad = 0x44C;     // ControlledById, followed by TheGame
inline constexpr uint32_t kPanelParent = 0x498;  // ScreenParent
inline constexpr uint32_t kPanelState = 0x49D;   // byte, EKartPlayerSelectIndividualState
inline constexpr uint32_t kPanelCharacter = 0x4A8;

// KartGame_Base
inline constexpr uint32_t kGamePrimaryController = 0x390;

inline constexpr const char* RaceStateName(unsigned state) {
  switch (state) {
    case 0: return "cinematic";
    case 1: return "countdown";
    case 2: return "racing";
    case 3: return "menu";
    case 4: return "finished";
    default: return "unknown";
  }
}

}  // namespace eck::online_probe::layout
