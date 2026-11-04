#pragma once

// Guest object layout the race probes and race telemetry read. Offsets are computed from the cooked property chains
// (declaration order, UnrealScript alignment rules, Object = 0x3C bytes). Cross-checked against code only for
// Pawn.Controller (0x1E4, APawn::IsHumanControlled), Engine.GamePlayers (0x4B4, UGameViewportClient::GetPlayerOwner)
// and GEngine (0x83CE2624, UEngine::GetEngine). Every other offset is derived: the first runtime log decides.
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
inline constexpr uint32_t kKartDriftDirection = 0x5DC;
inline constexpr uint32_t kKartDriftingElapsed = 0x5EC;
inline constexpr uint32_t kKartBoostBar = 0x644;
inline constexpr uint32_t kKartBoosterSpeed = 0x650;
inline constexpr uint32_t kKartBoosterLevel = 0x654;
inline constexpr uint32_t kKartCurrentPathNode = 0x674;
inline constexpr uint32_t kKartDistanceInRace = 0x6E0;
inline constexpr uint32_t kKartRacePosition = 0x6E8;
inline constexpr uint32_t kKartRaceFinishPosition = 0x6F0;
inline constexpr uint32_t kKartCompletedLaps = 0x6F4;
inline constexpr uint32_t kKartRacingTime = 0x6F8;
inline constexpr uint32_t kKartAccelInput = 0x720;
inline constexpr uint32_t kKartBrakeInput = 0x728;
inline constexpr uint32_t kKartTurnAxisInput = 0x72C;
inline constexpr uint32_t kKartCharInfo = 0x834;
inline constexpr uint32_t kKartIdPawn = 0x838;
inline constexpr uint32_t kKartRacingAiBit = 22;
inline constexpr uint32_t kKartRaceFinishedBit = 21;

// KartCharacterInfo, KartPathNode, KartGame_MainGame
inline constexpr uint32_t kCharInfoType = 0x1C4;  // byte
inline constexpr uint32_t kPathNodeDistance = 0x290;  // PathDistance, float
inline constexpr uint32_t kMainGameRaceState = 0x3D4;  // byte, EKartRaceState

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
