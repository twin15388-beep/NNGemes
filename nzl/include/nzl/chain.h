#pragma once
#include "nzl/game_profile.h"
#include "nzl/memory.h"
#include <optional>
#include <string>
namespace nzl {
struct Vec3f { float x = 0, y = 0, z = 0; };
struct ChainResult {
  uint64_t unity_entry = 0, game = 0, game_model = 0, battle_model = 0,
           battle_entities = 0, world_entities = 0, dict = 0, camera_model = 0;
  bool ok = false;
  std::string stage;
};
// Runtime numeric offsets are copied from one build profile in game_profile.h.
// Do not add build-specific literals here; profile identity and evidence are
// checked by the Windows attach path before these values are used.
constexpr uint32_t profile_offset32(game_profile::OffsetValue value) {
  return game_profile::offset_usable32(value)
             ? static_cast<uint32_t>(value.value)
             : 0;
}

struct Offsets {
  std::string_view profile_id{};
  bool profile_configured = false;
  uint32_t unity_entry_game = 0;
  uint32_t game_model = 0;
  uint32_t game_battle = 0;
  uint32_t battle_entities = 0;
  uint32_t battle_world = 0;
  uint32_t battle_camera = 0;
  uint32_t wem_battle_chars = 0;
  uint32_t dict_entries = 0;
  uint32_t dict_count = 0;
  uint32_t array_length = 0;
  uint32_t array_data = 0;
  uint32_t dict_stride = 0;
  uint32_t dict_hash = 0;
  uint32_t dict_key = 0;
  uint32_t dict_value = 0;
  uint32_t max_dict_entries = 0;
  uint32_t char_id = 0;
  uint32_t char_transform = 0;
  uint32_t char_is_dead = 0;
  uint32_t ctm_x = 0;
  uint32_t ctm_y = 0;
  uint32_t ctm_z = 0;
  uint32_t il2cpp_class_name = 0;
  uint32_t skeleton_character_model = 0;
  uint32_t skeleton_model_description = 0;
  uint32_t skeleton_model_bones_data = 0;
  uint32_t skeleton_config_bones_count = 0;
  uint32_t skeleton_config_bone_data_size = 0;
  uint32_t skeleton_config_local_pose_offset = 0;
  uint32_t skeleton_config_model_pose_offset = 0;
  uint32_t skeleton_config_bone_length_offset = 0;
  uint32_t skeleton_config_hierarchy_data = 0;
  uint32_t skeleton_config_hierarchy_max_children = 0;
  uint32_t skeleton_config_root_index = 0;
  uint32_t skeleton_config_legs_index = 0;
  uint32_t skeleton_config_spine2_index = 0;
  uint32_t skeleton_config_spine3_index = 0;
  uint32_t skeleton_config_neck_index = 0;
  uint32_t skeleton_config_head1_index = 0;
  uint32_t skeleton_config_head2_index = 0;
  uint32_t skeleton_max_bones = 0;
  uint32_t skeleton_max_bone_data_floats = 0;
  uint32_t skeleton_max_hierarchy_ints = 0;
  bool skeleton_offsets_configured = false;
  uint32_t battle_model_character_slots = 0;
  uint32_t client_slots_by_entity_id = 0;
  uint32_t character_slot_data = 0;
  uint32_t character_slot_entity_id = 0;
  uint32_t character_slot_nickname = 0;
  bool nickname_offsets_configured = false;
  uint32_t cam_pos = 0;
  uint32_t cam_orientation = 0;
  uint32_t cam_look_at_orientation = 0;
  uint32_t cam_fwd = 0;
  uint32_t cam_right = 0;
  uint32_t cam_up = 0;
  uint32_t character_private_model = 0;
  uint32_t health_model = 0;
  uint32_t health_data = 0;
  uint32_t health_value = 0;
  uint32_t health_max_value = 0;
  uint32_t armor_primitive = 0;
  uint32_t primitive_data = 0;
  uint32_t primitive_value = 0;
  uint32_t role_primitive = 0;
  uint32_t role_description = 0;
  uint32_t role_index = 0;
  uint32_t loot_dictionary = 0;
  uint32_t loot_transform = 0;
  uint32_t loot_position_x = 0;
  uint32_t loot_position_y = 0;
  uint32_t loot_position_z = 0;
  bool loot_positions_configured = false;
  uint32_t cars_dictionary = 0;
  uint32_t car_transform = 0;
  uint32_t car_position = 0;
  bool hp_live_verified = false;
  bool armor_live_verified = false;
  bool role_live_verified = false;
  bool loot_live_verified = false;
  bool cars_profile_supported = false;
  float head_height_m = 1.80f;
  float aim_head_height_m = 1.75f;
  float aim_chest_height_m = 1.30f;
  float aim_pelvis_height_m = 0.95f;
  float assumed_vertical_fov_deg = 90.0f;

  Offsets() : Offsets(game_profile::kOwnerProfile26866884) {}
  explicit Offsets(const game_profile::GameProfile& p)
      : profile_id(p.fingerprint.profile_id),
        profile_configured(game_profile::fingerprint_configured(p) &&
                           game_profile::core_offsets_configured(p) &&
                           game_profile::type_info_names_configured(p)),
        unity_entry_game(profile_offset32(p.chain.unity_entry_game)),
        game_model(profile_offset32(p.chain.game_game_model)),
        game_battle(profile_offset32(p.chain.game_model_battle)),
        battle_entities(profile_offset32(p.chain.battle_model_entities)),
        battle_world(profile_offset32(p.chain.battle_entities_world)),
        battle_camera(profile_offset32(p.chain.battle_model_camera)),
        wem_battle_chars(profile_offset32(p.chain.world_entities_characters)),
        dict_entries(profile_offset32(p.dictionary.object_entries)),
        dict_count(profile_offset32(p.dictionary.object_count)),
        array_length(profile_offset32(p.dictionary.entries_array_length)),
        array_data(profile_offset32(p.dictionary.entries_array_data)),
        dict_stride(profile_offset32(p.dictionary.entry_stride)),
        dict_hash(profile_offset32(p.dictionary.entry_hash)),
        dict_key(profile_offset32(p.dictionary.entry_key)),
        dict_value(profile_offset32(p.dictionary.entry_value)),
        max_dict_entries(p.dictionary.max_entries),
        char_id(profile_offset32(p.character.id)),
        char_transform(profile_offset32(p.character.transform)),
        char_is_dead(profile_offset32(p.character.is_dead)),
        ctm_x(profile_offset32(p.character.transform_x)),
        ctm_y(profile_offset32(p.character.transform_y)),
        ctm_z(profile_offset32(p.character.transform_z)),
        il2cpp_class_name(profile_offset32(p.character.il2cpp_class_name)),
        skeleton_character_model(profile_offset32(p.skeleton.character_model)),
        skeleton_model_description(profile_offset32(p.skeleton.model_description)),
        skeleton_model_bones_data(profile_offset32(p.skeleton.model_bones_data)),
        skeleton_config_bones_count(profile_offset32(p.skeleton.config_bones_count)),
        skeleton_config_bone_data_size(profile_offset32(p.skeleton.config_bone_data_size)),
        skeleton_config_local_pose_offset(profile_offset32(p.skeleton.config_local_position_orientation_offset)),
        skeleton_config_model_pose_offset(profile_offset32(p.skeleton.config_model_orientation_offset)),
        skeleton_config_bone_length_offset(profile_offset32(p.skeleton.config_bone_length_offset)),
        skeleton_config_hierarchy_data(profile_offset32(p.skeleton.config_bones_hierarchy_data)),
        skeleton_config_hierarchy_max_children(profile_offset32(p.skeleton.config_hierarchy_max_children_count)),
        skeleton_config_root_index(profile_offset32(p.skeleton.config_root_index)),
        skeleton_config_legs_index(profile_offset32(p.skeleton.config_legs_index)),
        skeleton_config_spine2_index(profile_offset32(p.skeleton.config_spine2_index)),
        skeleton_config_spine3_index(profile_offset32(p.skeleton.config_spine3_index)),
        skeleton_config_neck_index(profile_offset32(p.skeleton.config_neck_index)),
        skeleton_config_head1_index(profile_offset32(p.skeleton.config_head1_index)),
        skeleton_config_head2_index(profile_offset32(p.skeleton.config_head2_index)),
        skeleton_max_bones(p.skeleton.max_bones),
        skeleton_max_bone_data_floats(p.skeleton.max_bone_data_floats),
        skeleton_max_hierarchy_ints(p.skeleton.max_hierarchy_ints),
        skeleton_offsets_configured(game_profile::skeleton_offsets_configured(p)),
        battle_model_character_slots(profile_offset32(p.nickname.battle_model_character_slots)),
        client_slots_by_entity_id(profile_offset32(p.nickname.client_slots_by_entity_id)),
        character_slot_data(profile_offset32(p.nickname.character_slot_data)),
        character_slot_entity_id(profile_offset32(p.nickname.character_slot_entity_id)),
        character_slot_nickname(profile_offset32(p.nickname.character_slot_nickname)),
        nickname_offsets_configured(
            game_profile::offset_usable32(p.nickname.battle_model_character_slots) &&
            game_profile::offset_usable32(p.nickname.client_slots_by_entity_id) &&
            game_profile::offset_usable32(p.nickname.character_slot_data) &&
            game_profile::offset_usable32(p.nickname.character_slot_entity_id) &&
            game_profile::offset_usable32(p.nickname.character_slot_nickname) &&
            game_profile::offset_usable32(p.dictionary.entry_key) &&
            game_profile::offset_usable32(p.optional_features.primitive_value)),
        cam_pos(profile_offset32(p.camera.position)),
        cam_orientation(profile_offset32(p.camera.orientation)),
        cam_look_at_orientation(profile_offset32(p.camera.look_at_orientation)),
        cam_fwd(profile_offset32(p.camera.forward)),
        cam_right(profile_offset32(p.camera.right)),
        cam_up(profile_offset32(p.camera.up)),
        character_private_model(profile_offset32(p.optional_features.character_private_model)),
        health_model(profile_offset32(p.optional_features.health_model)),
        health_data(profile_offset32(p.optional_features.health_data)),
        health_value(profile_offset32(p.optional_features.health_value)),
        health_max_value(profile_offset32(p.optional_features.health_max_value)),
        armor_primitive(profile_offset32(p.optional_features.armor_primitive)),
        primitive_data(profile_offset32(p.optional_features.primitive_data)),
        primitive_value(profile_offset32(p.optional_features.primitive_value)),
        role_primitive(profile_offset32(p.optional_features.role_primitive)),
        role_description(profile_offset32(p.optional_features.role_description)),
        role_index(profile_offset32(p.optional_features.role_index)),
        loot_dictionary(profile_offset32(p.optional_features.loot_dictionary)),
        loot_transform(profile_offset32(p.optional_features.loot_transform)),
        loot_position_x(profile_offset32(p.optional_features.loot_position_x)),
        loot_position_y(profile_offset32(p.optional_features.loot_position_y)),
        loot_position_z(profile_offset32(p.optional_features.loot_position_z)),
        loot_positions_configured(p.optional_features.loot_position_x.present() &&
                                  p.optional_features.loot_position_y.present() &&
                                  p.optional_features.loot_position_z.present()),
        cars_dictionary(profile_offset32(p.optional_features.cars_dictionary)),
        car_transform(profile_offset32(p.optional_features.car_transform)),
        car_position(profile_offset32(p.optional_features.car_position)),
        hp_live_verified(p.optional_features.hp_live_verified),
        armor_live_verified(p.optional_features.armor_live_verified),
        role_live_verified(p.optional_features.role_live_verified),
        loot_live_verified(p.optional_features.loot_live_verified),
        cars_profile_supported(p.optional_features.cars_profile_supported),
        head_height_m(p.head_height_m),
        aim_head_height_m(p.aim_head_height_m),
        aim_chest_height_m(p.aim_chest_height_m),
        aim_pelvis_height_m(p.aim_pelvis_height_m),
        assumed_vertical_fov_deg(p.assumed_vertical_fov_deg) {}
};
std::optional<uint64_t> follow(const IMemory& m, uint64_t base, uint32_t off,
                               const char* stage, ChainResult& r);
ChainResult resolve_chain(const IMemory& m, uint64_t unity_entry_instance, const Offsets& o);
ChainResult resolve_chain_from_game(const IMemory& m, uint64_t game, const Offsets& o);
struct DictHeader {
  uint64_t entries = 0;
  int32_t count = 0;         // Dictionary's initialized-entry bound
  uint32_t array_length = 0;  // validated x64 Il2CppArray capacity
};
std::optional<DictHeader> read_dict(const IMemory& m, uint64_t dict, const Offsets& o);
std::optional<uint64_t> dict_value_at(const IMemory& m, const DictHeader& h,
                                      int index, const Offsets& o);
std::optional<Vec3f> read_char_pos(const IMemory& m, uint64_t character, const Offsets& o);
std::optional<Vec3f> read_vec3(const IMemory& m, uint64_t addr);
}
