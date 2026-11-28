#pragma once

// Все build-зависимые значения ROB RUN в одном месте.
// Система координат: Unity, left-handed, Y-up, +Z forward.
// Матрицы: row-major. Углы: градусы (FOV), радианы только внутри math.h.
//
// Статус каждого значения:
//   VERIFIED  — два независимых источника согласны (см. список ниже).
//   UNVERIFIED— один источник; гейтится видимым сообщением в рантайме.
//   UNKNOWN   — не найден; рецепт получения — в docs/ANALYSIS.md.
//
// Источники (кросс-чек 2026-11-27):
//   S1 justdoshiba/RobRunExternal @ 0ef798f (2026-11-12), build 26615022,
//      metadata 39, GameAssembly SHA-256 03003754…65c18ab.
//   S2 TrentsTuningOverlay.exe v1.04 (Dropbox /rb run, unknowncheats.me),
//      selftest-строки + xfprobe. Подтверждает 0x38/0x18/0x58/0x60,
//      static_fields 0xB8, world static 0x81B7148 (КОНФЛИКТ с S1, см. ниже).
//   S3 UnknownCheats 779959 (catalog 2026.11.12.2) + 781224: цепочка
//      UnityEntry+0x28→Game+0xD0→GameModel+0x10→BattleModel+0x90→
//      BattleEntitiesModel+0x60→WorldEntitiesModel+0x18, BattleModel
//      0x40/0x90/0x198/0x218/0x358/0x4D0/0x518/0x630, GOM 0x2230768,
//      camera RVA 0x62801A0/0x61D23F0/0x61D1890/0x61D04B0/0x61D2AE0/0x61D2940.
//   S4 platinmods FULL DUMP 17.11.26: BattleModel/Game/GameModel/GlobalModel/
//      UnityEntry/AimTargetModel/CameraState — совпадает с S3.
//   S5 lm3684759-lang/robrun-offsets (dump-2026-11-22) offsets.json:
//      BattleModel 0x90, BattleEntitiesModel 0x60/0xD0/0x80,
//      WorldEntitiesModel 0x18, BattleCharacterModel 0x10/0x38/0x40/0x48/
//      0x58/0xB8/0xF0/0x120/0x138/0x150, Data 0x20/0x28, Role 0x20,
//      IsDead 0xE0, Health 0x38, MaxHealth 0x28, Transform 0x10/0x18/0x20.
//   S6 uxxx00/Robrun-offsets-newest (Il2CppDumper headers, 11 файлов):
//      BattleEntitiesModel полный список 0x10..0xE0, BattleCameraModel
//      0x10/0x1C/0x2C/0x3C/0x48/0x54/0x60 (TypeInfo 0x823C210),
//      BattleModel 0x10..0x90, GameModel 0x10, BattleCharacterData
//      0x20/0x28/0x30, CommonData Role 0x20..IsDead 0xE0,
//      PrivateData Health 0x38, PublicData HasArmor 0x38.
//   S7 satanware25-beep/robrunnn (Il2CppDumper grep-dump): BattleEntitiesModel
//      0x10..0xE0 — совпадает с S6.
//
// Конфликты (зафиксированы, не слиты в один конфиг):
//   C1 Статический RVA world: S1 0x81D36C8 (WorldModel, 1/20) / 0x81D0710
//      (WorldEntitiesModel, 56/347) vs S2 0x81B7148. Разные билды
//      (S1 2026-11-12, S2 2026-11-26). Оба гейтятся, живая проверка обязательна.
//   C2 Game+0xD0 (S1/S3/S4/S6) vs Game+0xD8 (один пост S3). Выбран 0xD0
//      (4 источника против 1).
//   C3 RR_CAMERA (S3: pos 0x00/rot 0x0C/fwd 0x1C/right 0x28/up 0x34) —
//      упакованная нативная камера, НЕ BattleCameraModel. Не смешивать.

#include <cstdint>
#include <optional>

namespace robrun {

// --- Идентификация сборки (S1) ---
inline constexpr const char* kGameBuild = "26615022";
inline constexpr int kMetadataVersion = 39;
inline constexpr const char* kGameAssemblySha256 =
    "03003754353ff08380dc8e91920f2c77f7977b370d413083acfe9b4bc65c18ab";
inline constexpr const char* kMetadataSha256 =
    "23bed54126a2b89e9c78cfdbd59dc3be2bc14a5817823cf057cde33984618cc9";
inline constexpr const char* kOffsetSource =
    "S1 justdoshiba/RobRunExternal@0ef798f + S2 TrentsTuning v1.04 + "
    "S3 UC 779959/781224 + S4 platinmods 17.11.26 + S5 robrun-offsets + "
    "S6 uxxx00 headers + S7 satanware dump";

// --- Корневая цепочка (VERIFIED: S1+S3+S4+S6) ---
inline constexpr std::uint32_t kUnityEntryGame = 0x28;       // UnityEntry._game
inline constexpr std::uint32_t kGameModel = 0xD0;            // Game.GameModel (C2: 0xD8 отклонён)
inline constexpr std::uint32_t kGameBattle = 0x10;           // GameModel._battleModel
inline constexpr std::uint32_t kBattleEntities = 0x90;       // BattleModel.BattleEntitiesModel
inline constexpr std::uint32_t kBattleWorld = 0x60;          // BattleEntitiesModel._worldEntitiesModel
inline constexpr std::uint32_t kBattleModels = 0x50;         // BattleEntitiesModel._models
inline constexpr std::uint32_t kBattleSlots = 0x80;          // BattleEntitiesModel._characterSlotModel
inline constexpr std::uint32_t kBattlePlayer = 0xD0;         // BattleEntitiesModel.Player
inline constexpr std::uint32_t kBattleEntityModels = 0xE0;   // BattleEntitiesModel.EntityModels
inline constexpr std::uint32_t kBattleCamera = 0x358;        // BattleModel.BattleCameraModel
inline constexpr std::uint32_t kBattleAim = 0x198;           // BattleModel.AimTargetModel
inline constexpr std::uint32_t kBattlePlayerSlots = 0x4D0;   // BattleModel.BattleCharacterSlotsModel
inline constexpr std::uint32_t kBattleWorldAlt = 0x518;      // BattleModel.WorldEntitiesModel (S3+S4+S2 bm+0x518)
inline constexpr std::uint32_t kBattleVoiceRoom = 0x630;     // BattleModel.VoiceChatBattleRoomModel
inline constexpr std::uint32_t kBattlePlayerRot = 0x40;      // BattleModel.PlayerRotationInputModel
inline constexpr std::uint32_t kBattleNetStatus = 0x218;     // BattleModel.NetworkConnectionStatusModel

// --- Полевые офсеты сущностей (VERIFIED: 2+ источника) ---
inline constexpr std::uint32_t kWorldEntities = 0x38;      // WorldModel.WorldEntitiesModel (S1+S2)
inline constexpr std::uint32_t kWemBattleChars = 0x18;     // WorldEntitiesModel._battleCharacters (S1+S2+S3+S5)
inline constexpr std::uint32_t kWemVehicles = 0x58;        // WorldEntitiesModel._vehicles (S1)
inline constexpr std::uint32_t kCharPrivate = 0x10;        // BattleCharacterModel.PrivateModel (S3+S5)
inline constexpr std::uint32_t kCharId = 0x38;             // BattleCharacterModel.Id (S1+S3+S5)
inline constexpr std::uint32_t kCharData = 0x40;           // BattleCharacterModel.Data (S1+S3+S5)
inline constexpr std::uint32_t kCharConfig = 0x48;         // BattleCharacterModel.Config (S1+S3+S5)
inline constexpr std::uint32_t kCharTransform = 0x58;      // BattleCharacterModel.Transform (S1+S2+S3+S5)
inline constexpr std::uint32_t kCharReplTransform = 0x60;  // BattleCharacterModel.ReplicatedTransform (S1+S2+S3)
inline constexpr std::uint32_t kCharLook = 0x70;           // BattleCharacterModel.LookDirection (S3)
inline constexpr std::uint32_t kCharIsDead = 0xF0;         // BattleCharacterModel.IsDead (S1+S3+S5)
inline constexpr std::uint32_t kCharSkeleton = 0x138;      // BattleCharacterModel.SkeletonModel (S1+S3+S5)
inline constexpr std::uint32_t kCharWeapon = 0x150;        // BattleCharacterModel.Weapon (S1+S3+S5)
inline constexpr std::uint32_t kCharCrouch = 0x160;        // BattleCharacterModel.CrouchModel (S1+S3)
inline constexpr std::uint32_t kCharInVeh = 0x1C8;         // BattleCharacterModel.InVehicleModel (S1+S3)
inline constexpr std::uint32_t kCharState = 0x1E0;         // BattleCharacterModel.StateModel (S1+S3)
inline constexpr std::uint32_t kCharStance = 0x1F0;        // BattleCharacterModel.BattleStanceModel (S1+S3)
inline constexpr std::uint32_t kCharPbm = 0x258;           // BattleCharacterModel.PlayerBattleModel (S1+S3)
inline constexpr std::uint32_t kCtmX = 0x10;               // CharacterTransformModel.X (S1+S3)
inline constexpr std::uint32_t kCtmY = 0x14;               // CharacterTransformModel.Y (S1+S3)
inline constexpr std::uint32_t kCtmZ = 0x18;               // CharacterTransformModel.Z (S1+S3)
inline constexpr std::uint32_t kCtmPitch = 0x1C;           // CharacterTransformModel.Pitch (S1+S3)
inline constexpr std::uint32_t kCtmYaw = 0x20;             // CharacterTransformModel.Yaw (S1+S3)
inline constexpr std::uint32_t kCtmRoll = 0x24;            // CharacterTransformModel.Roll (S1+S3)
inline constexpr std::uint32_t kDataCommon = 0x20;         // BattleCharacterData.CommonData (S1+S3+S5+S6)
inline constexpr std::uint32_t kDataPrivate = 0x28;        // BattleCharacterData.PrivateData (S1+S3+S5+S6)
inline constexpr std::uint32_t kDataPublic = 0x30;         // BattleCharacterData.PublicData (S1+S6)
inline constexpr std::uint32_t kCommonRole = 0x20;         // BattleCharacterCommonData.Role (S1+S3+S5+S6)
inline constexpr std::uint32_t kCommonIsDead = 0xE0;       // BattleCharacterCommonData.IsDead (S3+S5+S6)
inline constexpr std::uint32_t kPrivateHealth = 0x38;      // BattleCharacterPrivateData.Health (S1+S3+S5+S6)
inline constexpr std::uint32_t kPublicArmor = 0x38;        // BattleCharacterPublicData.HasArmor (S1+S6)
inline constexpr std::uint32_t kConfigMaxHealth = 0x28;    // BattleCharacterConfig.MaxHealth (S5)

// --- Камера (VERIFIED: S1+S6) ---
inline constexpr std::uint32_t kCamPos = 0x10;             // BattleCameraModel.Position
inline constexpr std::uint32_t kCamRot = 0x1C;             // BattleCameraModel.Orientation
inline constexpr std::uint32_t kCamLookAt = 0x2C;          // BattleCameraModel.LookAtCameraOrientation (S4+S6)
inline constexpr std::uint32_t kCamFwd = 0x3C;             // BattleCameraModel.Forward
inline constexpr std::uint32_t kCamRight = 0x48;           // BattleCameraModel.Right
inline constexpr std::uint32_t kCamUp = 0x54;              // BattleCameraModel.Up
inline constexpr std::uint32_t kCamDist = 0x60;            // BattleCameraModel.DistanceToFollowTarget

// --- Словарь и IL2CPP (VERIFIED: S1+S2+S3) ---
inline constexpr std::uint32_t kDictEntries = 0x18;
inline constexpr std::uint32_t kDictCount = 0x20;
inline constexpr std::uint32_t kDictStride = 0x18;
inline constexpr std::uint32_t kIl2CppHeader = 16;         // object header (S2)
inline constexpr std::uint32_t kIl2CppStaticFields = 0xB8; // Il2CppClass.static_fields, metadata 39 (S2)
inline constexpr std::uint32_t kUClassName = 0x10;
inline constexpr std::uint32_t kUStrLen = 0x10;
inline constexpr std::uint32_t kUStrChars = 0x14;
inline constexpr std::uint32_t kUArrLen = 0x18;
inline constexpr std::uint32_t kUArrData = 0x20;

// --- Статические глобалы: КОНФЛИКТ C1, оба гейтятся ---
// S1 (build 26615022): WorldModel 0x81D36C8 (1/20), WorldEntitiesModel 0x81D0710 (56/347).
// S2 (TrentsTuning v1.04): world 0x81B7148. Разные билды — не сливать.
inline constexpr std::optional<std::uint32_t> kSfWorldModelS1 = 0x81D36C8;
inline constexpr std::optional<std::uint32_t> kSfWorldEntitiesModelS1 = 0x81D0710;
inline constexpr std::optional<std::uint32_t> kSfBattleCameraModelS1 = 0x81CB420;
inline constexpr std::optional<std::uint32_t> kSfBattleCharacterModelS1 = 0x8196748;
inline constexpr std::optional<std::uint32_t> kSfWorldS2 = 0x81B7148;
// TypeInfo (S6, не static): BattleEntitiesModel 0x8245928, BattleCameraModel 0x823C210.
inline constexpr std::optional<std::uint32_t> kTypeInfoBattleEntities = 0x8245928;
inline constexpr std::optional<std::uint32_t> kTypeInfoBattleCamera = 0x823C210;
// GOM (S3, single-source UNVERIFIED): UnityPlayer.dll + 0x2230768.
inline constexpr std::optional<std::uint32_t> kGomRva = 0x2230768;
// Camera RVA (S3, single-source UNVERIFIED).
inline constexpr std::optional<std::uint32_t> kRvaFindObjectOfType = 0x62801A0;
inline constexpr std::optional<std::uint32_t> kRvaCameraGetMain = 0x61D23F0;
inline constexpr std::optional<std::uint32_t> kRvaCameraGetCurrent = 0x61D1890;
inline constexpr std::optional<std::uint32_t> kRvaCameraWorldToScreen = 0x61D04B0;
inline constexpr std::optional<std::uint32_t> kRvaCameraPixelWidth = 0x61D2AE0;
inline constexpr std::optional<std::uint32_t> kRvaCameraPixelHeight = 0x61D2940;

// --- UNKNOWN: нет в источниках, рецепт в docs/ANALYSIS.md ---
// ViewMatrix (Camera.main worldToCameraMatrix): UNKNOWN (есть только RVA выше).
// Bone world positions: UNKNOWN (риг есть, сэмплер — нет; см. esp.h).
// LOS/visibility: UNKNOWN в cpp/ (в S1 есть rr_los*.c, не перенесён).

// Видимое сообщение о статусе (вызывать при старте).
inline const char* offset_status() {
    return "VERIFIED(fields, 5+ sources) + CONFLICT(statics C1: S1 0x81D36C8/0x81D0710 "
           "vs S2 0x81B7148) + UNVERIFIED(GOM/camera RVA single-source): "
           "требуется живая проверка Il2CppDumper + SHA GameAssembly.dll";
}

}  // namespace robrun
