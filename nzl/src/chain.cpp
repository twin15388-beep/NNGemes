#include "nzl/chain.h"
#include <cmath>
namespace nzl {
bool plausible_ptr(uint64_t p){ return p>0x10000 && p<0x00007FFFFFFFFFFFULL && (p&7)==0; }
std::optional<uint64_t> follow(const IMemory& m, uint64_t base, uint32_t off, const char* stage, ChainResult& r){
  if(!plausible_ptr(base)){ r.stage=stage; return std::nullopt; }
  auto v=m.ptr(base+off); if(!v || !plausible_ptr(*v)){ r.stage=stage; return std::nullopt; } return v;
}
static ChainResult resolve_from_game(const IMemory& m, uint64_t ue, uint64_t game, uint32_t gm_off, const Offsets& o){
  ChainResult r; r.unity_entry=ue; r.game=game;
  auto gm=follow(m,game,gm_off,"Game.GameModel",r); if(!gm) return r; r.game_model=*gm;
  auto bm=follow(m,*gm,o.game_battle,"GameModel._battleModel",r); if(!bm) return r; r.battle_model=*bm;
  auto be=follow(m,*bm,o.battle_entities,"BattleModel.BattleEntitiesModel",r); if(!be) return r; r.battle_entities=*be;
  auto we=follow(m,*be,o.battle_world,"BattleEntitiesModel._worldEntitiesModel",r); if(!we) return r; r.world_entities=*we;
  auto d=follow(m,*we,o.wem_battle_chars,"WorldEntitiesModel._battleCharacters",r); if(!d) return r; r.dict=*d;
  auto cam=follow(m,*bm,o.battle_camera,"BattleModel.BattleCameraModel",r); if(!cam) return r; r.camera_model=*cam;
  r.ok=true; r.stage="ok"; return r;
}
ChainResult resolve_chain(const IMemory& m, uint64_t ue, const Offsets& o){
  ChainResult r; r.unity_entry=ue;
  auto game=follow(m,ue,o.unity_entry_game,"UnityEntry._game",r); if(!game) return r;
  ChainResult a=resolve_from_game(m,ue,*game,o.game_model,o);
  if(a.ok) return a;
  if(o.game_model_alt && o.game_model_alt!=o.game_model){
    ChainResult b=resolve_from_game(m,ue,*game,o.game_model_alt,o);
    if(b.ok) return b;
  }
  return a;
}
std::optional<DictHeader> read_dict(const IMemory& m, uint64_t dict, const Offsets& o){
  if(!plausible_ptr(dict)) return std::nullopt; DictHeader h;
  auto e=m.ptr(dict+o.dict_entries); auto c=m.i32(dict+o.dict_count);
  if(!e||!c||!plausible_ptr(*e)||*c<0||*c>10000) return std::nullopt; h.entries=*e; h.count=*c; return h;
}
std::optional<uint64_t> dict_value_at(const IMemory& m, const DictHeader& h, int index, const Offsets& o){
  if(index<0||index>=h.count) return std::nullopt;
  uint64_t entry=h.entries+0x20ULL+ (uint64_t)index*o.dict_stride; // IL2CPP array data @+0x20
  auto hash=m.i32(entry); if(!hash||*hash<0) return std::nullopt;
  auto v=m.ptr(entry+0x10); if(!v||!plausible_ptr(*v)) return std::nullopt; return v;
}
std::optional<Vec3f> read_vec3(const IMemory& m, uint64_t a){ auto x=m.f32(a),y=m.f32(a+4),z=m.f32(a+8); if(!x||!y||!z||!std::isfinite(*x)||!std::isfinite(*y)||!std::isfinite(*z)) return std::nullopt; return Vec3f{*x,*y,*z}; }
std::optional<Vec3f> read_char_pos(const IMemory& m, uint64_t ch, const Offsets& o){
  if(!plausible_ptr(ch)) return std::nullopt;
  auto t=m.ptr(ch+o.char_transform); if(!t||!plausible_ptr(*t)) return std::nullopt;
  auto x=m.f32(*t+o.ctm_x),y=m.f32(*t+o.ctm_y),z=m.f32(*t+o.ctm_z);
  if(!x||!y||!z||!std::isfinite(*x)||!std::isfinite(*y)||!std::isfinite(*z)) return std::nullopt;
  if(*x==0&&*y==0&&*z==0) return std::nullopt; return Vec3f{*x,*y,*z};
}
}
