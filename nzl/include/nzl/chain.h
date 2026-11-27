#pragma once
#include "nzl/memory.h"
#include <optional>
#include <string>
namespace nzl {
struct Vec3f{ float x=0,y=0,z=0; };
struct ChainResult {
  uint64_t unity_entry=0, game=0, game_model=0, battle_model=0, battle_entities=0, world_entities=0, dict=0, camera_model=0;
  bool ok=false; std::string stage; // where it failed / "ok"
};
// Root must be obtained from LIVE dump (docs/EXTRACT.md): either
// TypeInfo slot -> Il2CppClass -> static_fields(+0xB8) -> UnityEntry instance,
// or GOM object. We do NOT hardcode old static RVA (conflict C1).
// offsets below mirror cpp/include/robrun/config.h VERIFIED fields.
struct Offsets {
  // Game.GameModel moved between Steam builds of the same Unity version:
  // 0xD0 in builds <=26726105 (17 Nov), 0xD8 in >=26807668 (20 Nov) -
  // cross-checked in the public per-build IL2CPP SDK dumps of this game.
  // resolve_chain tries the primary then the alt and keeps whichever
  // yields a fully sane chain (dict + camera), so a stale value cannot
  // silently win. Build match is still UNVERIFIED until live chain check.
  uint32_t unity_entry_game=0x28, game_model=0xD8, game_model_alt=0xD0, game_battle=0x10, battle_entities=0x90;
  uint32_t battle_world=0x60, battle_camera=0x358, wem_battle_chars=0x18;
  uint32_t dict_entries=0x18, dict_count=0x20, dict_stride=0x18;
  uint32_t char_id=0x38, char_transform=0x58, char_is_dead=0xF0;
  uint32_t ctm_x=0x10, ctm_y=0x14, ctm_z=0x18;
  uint32_t cam_pos=0x10, cam_fwd=0x3C, cam_right=0x48, cam_up=0x54;
};
std::optional<uint64_t> follow(const IMemory& m, uint64_t base, uint32_t off, const char* stage, ChainResult& r);
ChainResult resolve_chain(const IMemory& m, uint64_t unity_entry_instance, const Offsets& o);
struct DictHeader { uint64_t entries=0; int32_t count=0; };
std::optional<DictHeader> read_dict(const IMemory& m, uint64_t dict, const Offsets& o);
// entries: hash i32 @+0, next i32 @+4, key i32 @+8, value ptr @+0x10, stride 0x18
std::optional<uint64_t> dict_value_at(const IMemory& m, const DictHeader& h, int index, const Offsets& o);
std::optional<Vec3f> read_char_pos(const IMemory& m, uint64_t character, const Offsets& o);
std::optional<Vec3f> read_vec3(const IMemory& m, uint64_t addr);
}
