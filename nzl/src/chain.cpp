#include "nzl/chain.h"

#include <cmath>
#include <limits>

namespace nzl {

bool plausible_ptr(uint64_t pointer) {
  return pointer > 0x10000 && pointer < 0x00007FFFFFFFFFFFULL &&
         (pointer & 7) == 0;
}

namespace {

bool add_offset(uint64_t base, uint32_t offset, uint64_t& address) {
  if (base > std::numeric_limits<uint64_t>::max() - offset) return false;
  address = base + offset;
  return true;
}

ChainResult resolve_from_game(const IMemory& memory, uint64_t unity_entry,
                              uint64_t game, const Offsets& offsets) {
  ChainResult result;
  result.unity_entry = unity_entry;
  result.game = game;
  if (!offsets.profile_configured) {
    result.stage = "profile.unconfigured";
    return result;
  }

  auto game_model = follow(memory, game, offsets.game_model,
                           "Game.GameModel", result);
  if (!game_model) return result;
  result.game_model = *game_model;

  auto battle_model = follow(memory, *game_model, offsets.game_battle,
                             "GameModel._battleModel", result);
  if (!battle_model) return result;
  result.battle_model = *battle_model;

  auto battle_entities = follow(memory, *battle_model, offsets.battle_entities,
                                "BattleModel.BattleEntitiesModel", result);
  if (!battle_entities) return result;
  result.battle_entities = *battle_entities;

  auto world_entities = follow(memory, *battle_entities, offsets.battle_world,
                               "BattleEntitiesModel._worldEntitiesModel", result);
  if (!world_entities) return result;
  result.world_entities = *world_entities;

  auto dictionary = follow(memory, *world_entities, offsets.wem_battle_chars,
                           "WorldEntitiesModel._battleCharacters", result);
  if (!dictionary) return result;
  result.dict = *dictionary;

  auto camera = follow(memory, *battle_model, offsets.battle_camera,
                       "BattleModel.BattleCameraModel", result);
  if (!camera) return result;
  result.camera_model = *camera;

  result.ok = true;
  result.stage = "ok";
  return result;
}

}  // namespace

std::optional<uint64_t> follow(const IMemory& memory, uint64_t base,
                               uint32_t offset, const char* stage,
                               ChainResult& result) {
  uint64_t address = 0;
  if (!plausible_ptr(base) || !add_offset(base, offset, address)) {
    result.stage = stage;
    return std::nullopt;
  }

  auto value = memory.ptr(address);
  if (!value || !plausible_ptr(*value)) {
    result.stage = stage;
    return std::nullopt;
  }
  return value;
}

ChainResult resolve_chain_from_game(const IMemory& memory, uint64_t game,
                                    const Offsets& offsets) {
  return resolve_from_game(memory, game, game, offsets);
}

ChainResult resolve_chain(const IMemory& memory, uint64_t unity_entry,
                          const Offsets& offsets) {
  ChainResult result;
  result.unity_entry = unity_entry;
  if (!offsets.profile_configured) {
    result.stage = "profile.unconfigured";
    return result;
  }

  auto game = follow(memory, unity_entry, offsets.unity_entry_game,
                     "UnityEntry._game", result);
  if (!game) return result;
  return resolve_chain_from_game(memory, *game, offsets);
}

std::optional<DictHeader> read_dict(const IMemory& memory, uint64_t dict,
                                    const Offsets& offsets) {
  if (!offsets.profile_configured || !plausible_ptr(dict) ||
      offsets.max_dict_entries == 0) return std::nullopt;

  uint64_t entries_address = 0;
  uint64_t count_address = 0;
  if (!add_offset(dict, offsets.dict_entries, entries_address) ||
      !add_offset(dict, offsets.dict_count, count_address)) return std::nullopt;

  auto entries = memory.ptr(entries_address);
  auto count = memory.i32(count_address);
  if (!entries || !plausible_ptr(*entries) || !count || *count < 0 ||
      static_cast<uint32_t>(*count) > offsets.max_dict_entries) return std::nullopt;

  uint64_t length_address = 0;
  if (!add_offset(*entries, offsets.array_length, length_address))
    return std::nullopt;
  // Il2CppArray.max_length is size_t on the active x64 process. Reading only
  // four bytes could accept a truncated or corrupt capacity as a valid array.
  auto array_length = memory.ptr(length_address);
  if (!array_length || *array_length > offsets.max_dict_entries ||
      *array_length > std::numeric_limits<uint32_t>::max() ||
      static_cast<uint64_t>(*count) > *array_length) return std::nullopt;

  return DictHeader{*entries, *count, static_cast<uint32_t>(*array_length)};
}

std::optional<uint64_t> dict_value_at(const IMemory& memory,
                                      const DictHeader& header, int index,
                                      const Offsets& offsets) {
  if (index < 0 || header.count < 0 ||
      static_cast<uint32_t>(index) >= header.array_length ||
      index >= header.count ||
      static_cast<uint64_t>(header.count) > header.array_length ||
      !plausible_ptr(header.entries) || offsets.dict_stride == 0)
    return std::nullopt;

  uint64_t array_data = 0;
  if (!add_offset(header.entries, offsets.array_data, array_data) ||
      static_cast<uint64_t>(index) >
          (std::numeric_limits<uint64_t>::max() - array_data) /
              offsets.dict_stride) return std::nullopt;
  const uint64_t entry = array_data +
                         static_cast<uint64_t>(index) * offsets.dict_stride;

  uint64_t hash_address = 0;
  uint64_t value_address = 0;
  if (!add_offset(entry, offsets.dict_hash, hash_address) ||
      !add_offset(entry, offsets.dict_value, value_address)) return std::nullopt;

  auto hash = memory.i32(hash_address);
  if (!hash || *hash < 0) return std::nullopt;
  auto value = memory.ptr(value_address);
  if (!value || !plausible_ptr(*value)) return std::nullopt;
  return value;
}

std::optional<Vec3f> read_vec3(const IMemory& memory, uint64_t address) {
  // Vector fields may be four-byte aligned (for example camera +0x3C) even
  // though object pointers themselves are eight-byte aligned.
  if (address <= 0x10000 || address >= 0x00007FFFFFFFFFFFULL ||
      address > std::numeric_limits<uint64_t>::max() - 8) return std::nullopt;
  auto x = memory.f32(address);
  auto y = memory.f32(address + 4);
  auto z = memory.f32(address + 8);
  if (!x || !y || !z || !std::isfinite(*x) || !std::isfinite(*y) ||
      !std::isfinite(*z)) return std::nullopt;
  return Vec3f{*x, *y, *z};
}

std::optional<Vec3f> read_char_pos(const IMemory& memory, uint64_t character,
                                   const Offsets& offsets) {
  if (!offsets.profile_configured || !plausible_ptr(character))
    return std::nullopt;

  uint64_t transform_address = 0;
  if (!add_offset(character, offsets.char_transform, transform_address))
    return std::nullopt;
  auto transform = memory.ptr(transform_address);
  if (!transform || !plausible_ptr(*transform)) return std::nullopt;

  uint64_t x_address = 0;
  uint64_t y_address = 0;
  uint64_t z_address = 0;
  if (!add_offset(*transform, offsets.ctm_x, x_address) ||
      !add_offset(*transform, offsets.ctm_y, y_address) ||
      !add_offset(*transform, offsets.ctm_z, z_address)) return std::nullopt;

  auto x = memory.f32(x_address);
  auto y = memory.f32(y_address);
  auto z = memory.f32(z_address);
  constexpr float kMaxWorldCoordinate = 1.0e6f;
  if (!x || !y || !z || !std::isfinite(*x) || !std::isfinite(*y) ||
      !std::isfinite(*z) || std::fabs(*x) > kMaxWorldCoordinate ||
      std::fabs(*y) > kMaxWorldCoordinate || std::fabs(*z) > kMaxWorldCoordinate ||
      (*x == 0 && *y == 0 && *z == 0)) return std::nullopt;
  return Vec3f{*x, *y, *z};
}

}  // namespace nzl
