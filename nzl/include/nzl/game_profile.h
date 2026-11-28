#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <string_view>

namespace nzl::game_profile {

enum class Evidence : std::uint8_t {
  Unknown,
  SingleSource,
  CrossChecked,
  OwnerFingerprint,
  LiveVerified,
  Heuristic,
};

// `present` is independent from `value`: zero is a valid offset, while an
// unset value must never be silently interpreted as zero.
struct OffsetValue {
  std::uint64_t value = 0;
  Evidence evidence = Evidence::Unknown;
  std::string_view source{};

  constexpr bool present() const { return evidence != Evidence::Unknown; }
};

constexpr OffsetValue offset(std::uint64_t value, Evidence evidence,
                             std::string_view source) {
  return {value, evidence, source};
}

struct TypeInfoSlot {
  std::string_view class_name{};
  OffsetValue rva{};
};

struct BuildFingerprint {
  std::string_view profile_id{};
  std::string_view steam_build_id{};
  std::string_view unity_file_version{};
  std::uint32_t metadata_version = 0;  // zero means not measured
  std::uint64_t gameassembly_file_size = 0;
  std::string_view gameassembly_sha256{};
  std::string_view source{};
};

struct ChainOffsets {
  OffsetValue unity_entry_game{};
  OffsetValue game_game_model{};
  OffsetValue game_model_battle{};
  OffsetValue battle_model_entities{};
  OffsetValue battle_entities_world{};
  OffsetValue battle_model_camera{};
  OffsetValue world_entities_characters{};
};

struct DictionaryOffsets {
  OffsetValue object_entries{};
  OffsetValue object_count{};
  OffsetValue entries_array_length{};
  OffsetValue entries_array_data{};
  OffsetValue entry_stride{};
  OffsetValue entry_hash{};
  OffsetValue entry_key{};
  OffsetValue entry_value{};
  std::uint32_t max_entries = 0;  // reader safety bound, not a game offset
};

struct CharacterOffsets {
  OffsetValue id{};
  OffsetValue transform{};
  OffsetValue is_dead{};
  OffsetValue transform_x{};
  OffsetValue transform_y{};
  OffsetValue transform_z{};
  OffsetValue il2cpp_class_name{};
};

// Raw managed SkeletonModel/SkeletonConfig layout. The count/stride/index
// values themselves are read from each live SkeletonConfig object; the fixed
// values here are only field offsets and safety ceilings.
struct SkeletonOffsets {
  OffsetValue character_model{};
  OffsetValue model_description{};
  OffsetValue model_bones_data{};
  OffsetValue config_bones_count{};
  OffsetValue config_bone_data_size{};
  OffsetValue config_local_position_orientation_offset{};
  OffsetValue config_model_orientation_offset{};
  OffsetValue config_bone_length_offset{};
  OffsetValue config_bones_hierarchy_data{};
  OffsetValue config_hierarchy_max_children_count{};
  OffsetValue config_root_index{};
  OffsetValue config_legs_index{};
  OffsetValue config_spine2_index{};
  OffsetValue config_spine3_index{};
  OffsetValue config_neck_index{};
  OffsetValue config_head1_index{};
  OffsetValue config_head2_index{};
  std::uint32_t max_bones = 0;
  std::uint32_t max_bone_data_floats = 0;
  std::uint32_t max_hierarchy_ints = 0;
};

struct NicknameOffsets {
  OffsetValue battle_model_character_slots{};
  OffsetValue client_slots_by_entity_id{};
  OffsetValue character_slot_data{};
  OffsetValue character_slot_entity_id{};
  OffsetValue character_slot_nickname{};
};

struct CameraOffsets {
  OffsetValue position{};
  OffsetValue orientation{};
  OffsetValue look_at_orientation{};
  OffsetValue forward{};
  OffsetValue right{};
  OffsetValue up{};
  OffsetValue field_of_view{};  // unknown in the current reference dump
};

struct OptionalFeatureOffsets {
  OffsetValue character_private_model{};
  OffsetValue health_model{};
  OffsetValue health_data{};
  OffsetValue health_value{};
  OffsetValue health_max_value{};
  OffsetValue armor_primitive{};
  OffsetValue primitive_data{};
  OffsetValue primitive_value{};
  OffsetValue role_primitive{};
  OffsetValue role_description{};
  OffsetValue role_index{};
  OffsetValue loot_dictionary{};
  OffsetValue loot_transform{};
  OffsetValue loot_position_x{};
  OffsetValue loot_position_y{};
  OffsetValue loot_position_z{};
  OffsetValue cars_dictionary{};
  OffsetValue car_transform{};
  OffsetValue car_position{};
  // Nested features stay disabled until checked in a live match. Vehicle
  // fields have an exact-build SDK source and use runtime model-class gates;
  // this support flag does not claim that their live coordinates were tested.
  bool hp_live_verified = false;
  bool armor_live_verified = false;
  bool role_live_verified = false;
  bool loot_live_verified = false;
  bool cars_profile_supported = false;
};

struct GameProfile {
  BuildFingerprint fingerprint{};
  std::array<TypeInfoSlot, 7> type_info{};
  ChainOffsets chain{};
  DictionaryOffsets dictionary{};
  CharacterOffsets character{};
  SkeletonOffsets skeleton{};
  NicknameOffsets nickname{};
  CameraOffsets camera{};
  OptionalFeatureOffsets optional_features{};
  float head_height_m = 1.80f;
  float aim_head_height_m = 1.75f;
  float aim_chest_height_m = 1.30f;
  float aim_pelvis_height_m = 0.95f;
  float assumed_vertical_fov_deg = 90.0f;
};

// The owner fingerprint was reported by NZL-Collect on 2026-11-27 and is
// recorded in HANDOFF.md. It has not been re-read from the owner's Windows
// installation in this sandbox. Windows runtime code must match both the
// on-disk GameAssembly SHA-256/size and the loaded PE image before using it.
inline constexpr GameProfile kOwnerProfile26866884 = []() constexpr {
  GameProfile p{};
  p.fingerprint = {
      "owner-build-26866884-2026-11-27",
      "26866884",
      "",  // Unity file version not re-measured for this profile
      0,   // metadata version not independently captured for the owner binary
      148323840,
      "2dc3326915eb1e62e86dee32d9fd64f8020623f6220de747272cd7ade767b5cf",
      "HANDOFF.md: NZL-Collect owner fingerprint, 2026-11-27"};

  // RVAs originate from the owner's build-matched script.json. Runtime must
  // still check PE bounds and all seven Il2CppClass names before using them.
  p.type_info = {{{"Game", offset(0x81B7710, Evidence::OwnerFingerprint, "owner script.json, build 26866884; runtime name check")},
                  {"GameModel", offset(0x81B86D8, Evidence::OwnerFingerprint, "owner script.json, build 26866884; runtime name check")},
                  {"BattleModel", offset(0x82496E8, Evidence::OwnerFingerprint, "owner script.json, build 26866884; runtime name check")},
                  {"BattleEntitiesModel", offset(0x8245928, Evidence::OwnerFingerprint, "owner script.json, build 26866884; runtime name check")},
                  {"WorldEntitiesModel", offset(0x82295C8, Evidence::OwnerFingerprint, "owner script.json, build 26866884; runtime name check")},
                  {"BattleCameraModel", offset(0x823C210, Evidence::OwnerFingerprint, "owner script.json, build 26866884; runtime name check")},
                  {"BattleCharacterModel", offset(0x823FFD0, Evidence::OwnerFingerprint, "owner script.json, build 26866884; runtime name check")}}};

  // One build only. In particular, 0xD0 from build 26615022 is not a
  // fallback for this 0xD8 profile. The live attach path validates each
  // object klass at the selected offsets and reports the exact failed stage.
  p.chain = {offset(0x28, Evidence::CrossChecked, "InsideExploit/rob-run-sdk@86baa8e/26866884 + justdoshiba/RobRunExternal@0ef798f/26615022"),
             offset(0xD8, Evidence::OwnerFingerprint, "InsideExploit/rob-run-sdk@86baa8e/26866884 dump.cs; live GameModel klass gate"),
             offset(0x10, Evidence::CrossChecked, "InsideExploit/rob-run-sdk@86baa8e/26866884 + justdoshiba/RobRunExternal@0ef798f/26615022 + uxxx00@69c3cae/2026-11-23"),
             offset(0x90, Evidence::CrossChecked, "InsideExploit/rob-run-sdk@86baa8e/26866884 + justdoshiba@0ef798f/26615022 + lm3684759@21df822/dump-2026-11-22 + uxxx00@69c3cae/2026-11-23"),
             offset(0x60, Evidence::CrossChecked, "InsideExploit/rob-run-sdk@86baa8e/26866884 + justdoshiba@0ef798f/26615022 + lm3684759@21df822/dump-2026-11-22 + uxxx00@69c3cae/2026-11-23"),
             offset(0x358, Evidence::CrossChecked, "InsideExploit/rob-run-sdk@86baa8e/26866884 + justdoshiba@0ef798f/26615022; live BattleCamera klass gate"),
             offset(0x18, Evidence::CrossChecked, "InsideExploit/rob-run-sdk@86baa8e/26866884 + justdoshiba@0ef798f/26615022 + lm3684759@21df822/dump-2026-11-22 + uxxx00@69c3cae/2026-11-23 + satanware25@ec3b2a1/2026-11-15")};

  // IL2CPP Dictionary/array layout. Cross-build sources are recorded as
  // corroboration only; they are not alternate active profiles. The runtime
  // uses this set only after the exact owner GameAssembly fingerprint matches.
  // Count is sanity-checked against the full x64 size_t array length, initialized
  // hash slots are filtered, and player values must match the live class pointer.
  p.dictionary = {offset(0x18, Evidence::CrossChecked, "owner NZL-Collect live probe, owner build 26866884; justdoshiba rr_unity.h, build 26615022"),
                 offset(0x20, Evidence::CrossChecked, "owner NZL-Collect live probe, owner build 26866884; justdoshiba rr_unity.h, build 26615022"),
                 offset(0x18, Evidence::SingleSource, "InsideExploit SDK build 26866884; IL2CPP x64 Il2CppArray.max_length; runtime capacity/count gate"),
                 offset(0x20, Evidence::SingleSource, "InsideExploit SDK build 26866884; IL2CPP x64 array data; runtime bounds/hash gate"),
                 offset(0x18, Evidence::CrossChecked, "InsideExploit SDK build 26866884; justdoshiba Dictionary<int,reference> layout, build 26615022"),
                 offset(0x00, Evidence::SingleSource, "InsideExploit SDK build 26866884; IL2CPP Dictionary.Entry.hashCode; nonnegative-hash gate"),
                 offset(0x08, Evidence::SingleSource, "InsideExploit SDK build 26866884; IL2CPP Dictionary.Entry.key; nickname lookup by entity ID"),
                 offset(0x10, Evidence::CrossChecked, "InsideExploit SDK build 26866884; justdoshiba Dictionary<int,reference> layout, build 26615022; runtime player-class gate"),
                 2048};

  p.character = {offset(0x38, Evidence::CrossChecked, "InsideExploit SDK build 26866884; justdoshiba build 26615022; lm3684759 dump 2026-11-22; satanware25 dump 2026-11-15 (Steam build IDs not recorded for latter two)"),
                 offset(0x58, Evidence::CrossChecked, "InsideExploit SDK build 26866884; justdoshiba build 26615022; lm3684759 dump 2026-11-22; satanware25 dump 2026-11-15 (Steam build IDs not recorded for latter two)"),
                 offset(0xF0, Evidence::CrossChecked, "InsideExploit SDK build 26866884; justdoshiba build 26615022; lm3684759 dump 2026-11-22; satanware25 dump 2026-11-15 (Steam build IDs not recorded for latter two)"),
                 offset(0x10, Evidence::OwnerFingerprint, "owner SDK CharacterTransformModel, build 26866884; owner live player-position plausibility sample"),
                 offset(0x14, Evidence::OwnerFingerprint, "owner SDK CharacterTransformModel, build 26866884; owner live player-position plausibility sample"),
                 offset(0x18, Evidence::OwnerFingerprint, "owner SDK CharacterTransformModel, build 26866884; owner live player-position plausibility sample; lm3684759 dump 2026-11-22 differs and is not selected"),
                 offset(0x10, Evidence::SingleSource, "InsideExploit SDK TypeInfo classes, build 26866884; IL2CPP Il2CppClass.name; all seven names checked at runtime")};

  // Raw managed skeleton layout. These field offsets match all eight SDK dumps
  // inspected (26509119 through 26866884); runtime count/stride/index values
  // are read from each SkeletonConfig object, never guessed from another build.
  p.skeleton = {offset(0x138, Evidence::CrossChecked, "BattleCharacterModel.SkeletonModel; InsideExploit SDK dumps 26509119, 26559853, 26586703, 26599424, 26615022, 26726105, 26807668, 26866884"),
                offset(0x10, Evidence::CrossChecked, "SkeletonModel._skeletonDescription; same eight SDK dumps"),
                offset(0x18, Evidence::CrossChecked, "SkeletonModel._bonesData; same eight SDK dumps"),
                offset(0x10, Evidence::CrossChecked, "SkeletonConfig.BonesCount; same eight SDK dumps"),
                offset(0x14, Evidence::CrossChecked, "SkeletonConfig.BoneDataSize; same eight SDK dumps"),
                offset(0x18, Evidence::CrossChecked, "SkeletonConfig.BoneDataLocalPositionOrientationOffset; same eight SDK dumps"),
                offset(0x1C, Evidence::CrossChecked, "SkeletonConfig.BoneDataModelOrientationOffset; same eight SDK dumps"),
                offset(0x20, Evidence::CrossChecked, "SkeletonConfig.BoneDataLengthOffset; same eight SDK dumps"),
                offset(0x28, Evidence::CrossChecked, "SkeletonConfig.BonesHierarchyData; same eight SDK dumps"),
                offset(0x30, Evidence::CrossChecked, "SkeletonConfig.BonesHierarchyMaxChildrenCount; same eight SDK dumps"),
                offset(0x34, Evidence::CrossChecked, "SkeletonConfig.RootIndex; same eight SDK dumps"),
                offset(0x38, Evidence::CrossChecked, "SkeletonConfig.LegsIndex; same eight SDK dumps"),
                offset(0x3C, Evidence::CrossChecked, "SkeletonConfig.Spine2Index; same eight SDK dumps"),
                offset(0x40, Evidence::CrossChecked, "SkeletonConfig.Spine3Index; same eight SDK dumps"),
                offset(0x44, Evidence::CrossChecked, "SkeletonConfig.NeckIndex; same eight SDK dumps"),
                offset(0x48, Evidence::CrossChecked, "SkeletonConfig.Head1Index; same eight SDK dumps"),
                offset(0x4C, Evidence::CrossChecked, "SkeletonConfig.Head2Index; same eight SDK dumps"),
                256, 64, 8192};

  // Nickname lookup: BattleModel.CharacterSlotsModel ->
  // ClientCharacterSlotsModel._slotByEntityId -> BattleCharacterSlotModel._data
  // -> BattleCharacterSlotData.EntityId/Nickname -> StringData.Value (also
  // accepts the SDK's NicknameData subclass). Class names and the slot/entity
  // ID agreement are checked before accepting a name.
  p.nickname = {offset(0x248, Evidence::SingleSource, "InsideExploit/rob-run-sdk@86baa8e/26866884/dump.cs; BattleModel.CharacterSlotsModel"),
                offset(0x18, Evidence::SingleSource, "InsideExploit/rob-run-sdk@86baa8e/26866884/dump.cs; ClientCharacterSlotsModel._slotByEntityId"),
                offset(0x10, Evidence::SingleSource, "InsideExploit/rob-run-sdk@86baa8e/26866884/dump.cs; BattleCharacterSlotModel._data"),
                offset(0x20, Evidence::SingleSource, "InsideExploit/rob-run-sdk@86baa8e/26866884/dump.cs; BattleCharacterSlotData.EntityId, must agree with BattleCharacterModel.Id/dictionary key"),
                offset(0x30, Evidence::SingleSource, "InsideExploit/rob-run-sdk@86baa8e/26866884/dump.cs; BattleCharacterSlotData.Nickname")};

  p.camera = {offset(0x10, Evidence::OwnerFingerprint, "owner live BattleCameraModel sample, build 26866884; uxxx00 headers 2026-11-23 corroborate, Steam build ID not recorded there"),
              offset(0x1C, Evidence::SingleSource, "uxxx00/Robrun-offsets-newest headers 2026-11-23; Steam build ID not recorded; not used by current renderer"),
              offset(0x2C, Evidence::SingleSource, "uxxx00/Robrun-offsets-newest headers 2026-11-23; Steam build ID not recorded; quaternion norm check with axis fallback"),
              offset(0x3C, Evidence::OwnerFingerprint, "owner live BattleCameraModel sample, build 26866884; uxxx00 headers 2026-11-23 corroborate"),
              offset(0x48, Evidence::OwnerFingerprint, "owner live BattleCameraModel sample, build 26866884; uxxx00 headers 2026-11-23 corroborate"),
              offset(0x54, Evidence::OwnerFingerprint, "owner live BattleCameraModel sample, build 26866884; uxxx00 headers 2026-11-23 corroborate"),
              {}};

  p.optional_features = {
      offset(0x10, Evidence::SingleSource, "owner SDK dump, build 26866884; HP/armor/role chains not live-verified"),
      offset(0x1B0, Evidence::SingleSource, "owner SDK dump, build 26866884; HP chain not live-verified"),
      offset(0x10, Evidence::SingleSource, "owner SDK dump, build 26866884; HP chain not live-verified"),
      offset(0x18, Evidence::SingleSource, "owner SDK dump, build 26866884; HP chain not live-verified"),
      offset(0x20, Evidence::SingleSource, "owner SDK dump, build 26866884; HP chain not live-verified"),
      offset(0x1B8, Evidence::SingleSource, "owner SDK dump, build 26866884; armor chain not live-verified"),
      offset(0x10, Evidence::SingleSource, "owner SDK dump, build 26866884; armor/role chain not live-verified"),
      offset(0x18, Evidence::SingleSource, "InsideExploit SDK build 26866884; BasePrimitiveData<T> field order on x64 IL2CPP (_serializer then _value); runtime StringData/System.String class and length gates; not live-verified"),
      offset(0x208, Evidence::SingleSource, "owner SDK dump, build 26866884; role chain not live-verified"),
      offset(0x18, Evidence::SingleSource, "owner SDK dump, build 26866884; role chain not live-verified"),
      offset(0x18, Evidence::SingleSource, "owner SDK dump, build 26866884; role chain not live-verified"),
      offset(0x38, Evidence::SingleSource, "owner SDK dump, build 26866884; loot dictionary/layout not live-verified"),
      offset(0x20, Evidence::SingleSource, "owner SDK dump, build 26866884; loot transform not live-verified"),
      {}, {}, {},  // loot coordinate members are unknown; leave the feature gated off
      offset(0x60, Evidence::SingleSource, "InsideExploit/rob-run-sdk@86baa8e/26866884/dump.cs; WorldEntitiesModel._cars"),
      offset(0xC0, Evidence::SingleSource, "InsideExploit/rob-run-sdk@86baa8e/26866884/dump.cs; CarModel.Transform"),
      offset(0x38, Evidence::SingleSource, "InsideExploit/rob-run-sdk@86baa8e/26866884/dump.cs; VehicleTransformModel center-of-mass position; runtime class/plausibility gates"),
      false, false, false, false, true};

  p.head_height_m = 1.80f;
  p.aim_head_height_m = 1.75f;
  p.aim_chest_height_m = 1.30f;
  p.aim_pelvis_height_m = 0.95f;
  p.assumed_vertical_fov_deg = 90.0f;
  return p;
}();

// The historical value is kept separate; it is never an active-profile
// alternative or fallback.
struct HistoricalProfileNote {
  std::string_view profile_id;
  OffsetValue game_game_model;
  std::string_view source;
};
inline constexpr HistoricalProfileNote kHistoricalBuild26615022{
    "justdoshiba-build-26615022",
    offset(0xD0, Evidence::CrossChecked,
           "justdoshiba/RobRunExternal@0ef798f39179708a2c128444868385d9c9a91d57; separate older build"),
    "Do not use with owner build 26866884."};

constexpr bool is_sha256_hex(std::string_view value) {
  if (value.size() != 64) return false;
  for (char c : value) {
    const bool digit = c >= '0' && c <= '9';
    const bool lower = c >= 'a' && c <= 'f';
    const bool upper = c >= 'A' && c <= 'F';
    if (!digit && !lower && !upper) return false;
  }
  return true;
}

// Evidence::Heuristic is useful as an audit note but must never become a
// live offset merely because it has a numeric value. Every active offset also
// needs an explicit source; build identity is checked separately at runtime.
constexpr bool offset_usable32(OffsetValue value) {
  const bool evidence_ok = value.evidence == Evidence::SingleSource ||
                           value.evidence == Evidence::CrossChecked ||
                           value.evidence == Evidence::OwnerFingerprint ||
                           value.evidence == Evidence::LiveVerified;
  return value.present() && evidence_ok && !value.source.empty() &&
         value.value <= UINT32_MAX;
}

constexpr bool fingerprint_configured(const GameProfile& p) {
  return !p.fingerprint.profile_id.empty() && !p.fingerprint.steam_build_id.empty() &&
         !p.fingerprint.source.empty() && p.fingerprint.gameassembly_file_size > 0 &&
         is_sha256_hex(p.fingerprint.gameassembly_sha256);
}

constexpr bool core_offsets_configured(const GameProfile& p) {
  return offset_usable32(p.chain.unity_entry_game) &&
         offset_usable32(p.chain.game_game_model) &&
         offset_usable32(p.chain.game_model_battle) &&
         offset_usable32(p.chain.battle_model_entities) &&
         offset_usable32(p.chain.battle_entities_world) &&
         offset_usable32(p.chain.battle_model_camera) &&
         offset_usable32(p.chain.world_entities_characters) &&
         offset_usable32(p.dictionary.object_entries) &&
         offset_usable32(p.dictionary.object_count) &&
         offset_usable32(p.dictionary.entries_array_length) &&
         offset_usable32(p.dictionary.entries_array_data) &&
         offset_usable32(p.dictionary.entry_stride) &&
         offset_usable32(p.dictionary.entry_hash) &&
         offset_usable32(p.dictionary.entry_value) && p.dictionary.max_entries > 0 &&
         offset_usable32(p.character.id) &&
         offset_usable32(p.character.transform) &&
         offset_usable32(p.character.is_dead) &&
         offset_usable32(p.character.transform_x) &&
         offset_usable32(p.character.transform_y) &&
         offset_usable32(p.character.transform_z) &&
         offset_usable32(p.character.il2cpp_class_name) &&
         offset_usable32(p.camera.position) &&
         offset_usable32(p.camera.look_at_orientation) &&
         offset_usable32(p.camera.forward) && offset_usable32(p.camera.right) &&
         offset_usable32(p.camera.up);
}

constexpr bool skeleton_offsets_configured(const GameProfile& p) {
  const auto& s = p.skeleton;
  return offset_usable32(s.character_model) &&
         offset_usable32(s.model_description) &&
         offset_usable32(s.model_bones_data) &&
         offset_usable32(s.config_bones_count) &&
         offset_usable32(s.config_bone_data_size) &&
         offset_usable32(s.config_local_position_orientation_offset) &&
         offset_usable32(s.config_model_orientation_offset) &&
         offset_usable32(s.config_bone_length_offset) &&
         offset_usable32(s.config_bones_hierarchy_data) &&
         offset_usable32(s.config_hierarchy_max_children_count) &&
         offset_usable32(s.config_root_index) &&
         offset_usable32(s.config_legs_index) &&
         offset_usable32(s.config_spine2_index) &&
         offset_usable32(s.config_spine3_index) &&
         offset_usable32(s.config_neck_index) &&
         offset_usable32(s.config_head1_index) &&
         offset_usable32(s.config_head2_index) &&
         s.max_bones >= 2 && s.max_bones <= 256 &&
         s.max_bone_data_floats >= 9 && s.max_bone_data_floats <= 64 &&
         s.max_hierarchy_ints > 0 && s.max_hierarchy_ints <= 8192;
}

constexpr bool type_info_rvas_fit_image(const GameProfile& p,
                                        std::uint32_t size_of_image) {
  if (size_of_image < sizeof(std::uint64_t)) return false;
  for (const auto& slot : p.type_info) {
    if (!offset_usable32(slot.rva) ||
        slot.rva.value > size_of_image - sizeof(std::uint64_t)) return false;
  }
  return true;
}

constexpr bool type_info_names_configured(const GameProfile& p) {
  for (const auto& slot : p.type_info) {
    if (slot.class_name.empty() || !offset_usable32(slot.rva)) return false;
  }
  return true;
}

static_assert(fingerprint_configured(kOwnerProfile26866884));
static_assert(core_offsets_configured(kOwnerProfile26866884));
static_assert(type_info_names_configured(kOwnerProfile26866884));

}  // namespace nzl::game_profile
