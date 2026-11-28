#include "nzl/esp_pipeline.h"
#include <algorithm>
#include <cmath>
namespace nzl {
static float dot(Vec3f a, Vec3f b){ return a.x*b.x+a.y*b.y+a.z*b.z; }
static Vec3f sub(Vec3f a, Vec3f b){ return {a.x-b.x,a.y-b.y,a.z-b.z}; }
static float len(Vec3f a){ return std::sqrt(dot(a,a)); }
std::vector<PlayerSnap> snapshot_players(const IMemory& memory,
                                         const ChainResult& chain,
                                         const Offsets& offsets,
                                         const Vec3f& local_pos,
                                         uint64_t expected_character_class) {
  std::vector<PlayerSnap> players;
  if (!chain.ok || !offsets.profile_configured) return players;
  const auto header = read_dict(memory, chain.dict, offsets);
  if (!header) return players;

  for (int index = 0; index < header->count; ++index) {
    const auto character = dict_value_at(memory, *header, index, offsets);
    if (!character) continue;
    if (expected_character_class) {
      const auto klass = memory.ptr(*character);
      if (!klass || *klass != expected_character_class) continue;
    }

    const auto position = read_char_pos(memory, *character, offsets);
    if (!position) continue;
    PlayerSnap player;
    player.addr = *character;

    if (*character <= UINT64_MAX - offsets.char_id) {
      const auto id = memory.i32(*character + offsets.char_id);
      if (id) player.id = *id;
    }
    if (*character <= UINT64_MAX - offsets.char_is_dead) {
      const auto dead = memory.u8(*character + offsets.char_is_dead);
      if (dead) player.dead = *dead != 0;
    }

    player.pos = *position;
    player.dist = len(sub(*position, local_pos));
    if (!std::isfinite(player.dist)) continue;
    players.push_back(player);
  }
  std::sort(players.begin(), players.end(),
            [](const PlayerSnap& left, const PlayerSnap& right) {
              return left.dist < right.dist;
            });
  return players;
}
bool world_to_screen(const CameraView& c, const Vec3f& w, float& sx, float& sy, float& depth){
  if(c.width<=0||c.height<=0||c.fov_deg<=1||c.fov_deg>=179) return false;
  Vec3f d = sub(w, c.pos);
  float cz = dot(d, c.fwd); depth = cz;
  if(!(cz>0.05f) || !std::isfinite(cz)) return false;
  float cx = dot(d, c.right), cy = dot(d, c.up);
  float tan_half = std::tan(c.fov_deg*0.01745329252f*0.5f);
  float aspect = (float)c.width/(float)c.height;
  float nx = (cx/cz)/(tan_half*aspect), ny = (cy/cz)/tan_half;
  if(!std::isfinite(nx)||!std::isfinite(ny)) return false;
  sx = (nx*0.5f+0.5f)*(float)c.width; sy = (1.0f-(ny*0.5f+0.5f))*(float)c.height;
  return nx>-1.2f && nx<1.2f && ny>-1.2f && ny<1.2f;
}
std::vector<DrawCmd> build_draw_list(const CameraView& c, const std::vector<PlayerSnap>& players, float max_dist){
  std::vector<DrawCmd> out;
  for(auto& p: players){
    if(p.dead || p.dist>max_dist) continue;
    float fx,fy,fd,hx,hy,hd;
    if(!world_to_screen(c,p.pos,fx,fy,fd)) continue;
    Vec3f head{p.pos.x, p.pos.y + c.head_height_m, p.pos.z};
    if(!world_to_screen(c,head,hx,hy,hd)) continue;
    float hgt = std::fabs(fy-hy); if(hgt<2) continue;
    float wid = hgt*0.45f;
    DrawCmd r; r.kind=DrawCmd::Rect; r.x1=hx-wid*0.5f; r.y1=hy; r.x2=hx+wid*0.5f; r.y2=fy; out.push_back(r);
    DrawCmd t; t.kind=DrawCmd::Text; t.x1=r.x1; t.y1=hy-14;
    t.text = "#" + std::to_string(p.id) + " " + std::to_string((int)p.dist) + "m";
    out.push_back(t);
  }
  return out;
}
}
