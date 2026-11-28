#include "nzl/esp_pipeline.h"
#include <algorithm>
#include <cmath>
namespace nzl {
static float dot(Vec3f a, Vec3f b){ return a.x*b.x+a.y*b.y+a.z*b.z; }
static Vec3f sub(Vec3f a, Vec3f b){ return {a.x-b.x,a.y-b.y,a.z-b.z}; }
static Vec3f cross(Vec3f a, Vec3f b){ return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
static float len(Vec3f a){ return std::sqrt(dot(a,a)); }
static bool normalized_axis(Vec3f in, Vec3f& out) {
  if (!std::isfinite(in.x) || !std::isfinite(in.y) || !std::isfinite(in.z)) return false;
  const float magnitude = len(in);
  if (!std::isfinite(magnitude) || magnitude < 0.5f || magnitude > 1.5f) return false;
  out = {in.x / magnitude, in.y / magnitude, in.z / magnitude};
  return true;
}
static bool orthonormal_basis(Vec3f fwd, Vec3f right, Vec3f up) {
  const float orthogonal_limit = 0.20f;
  return std::fabs(dot(fwd, right)) < orthogonal_limit &&
         std::fabs(dot(fwd, up)) < orthogonal_limit &&
         std::fabs(dot(right, up)) < orthogonal_limit &&
         dot(cross(fwd, right), up) > 0.5f;
}
static bool basis_from_axes(Vec3f fwd, Vec3f right, Vec3f up,
                            Vec3f& out_fwd, Vec3f& out_right, Vec3f& out_up) {
  if (!normalized_axis(fwd, out_fwd) || !normalized_axis(right, out_right) ||
      !normalized_axis(up, out_up)) return false;
  return orthonormal_basis(out_fwd, out_right, out_up);
}
static bool basis_from_quaternion(const float q[4], Vec3f& fwd,
                                  Vec3f& right, Vec3f& up) {
  if (!q) return false;
  for (int i = 0; i < 4; ++i) if (!std::isfinite(q[i])) return false;
  const float magnitude = std::sqrt(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3]);
  if (!std::isfinite(magnitude) || magnitude < 0.5f || magnitude > 1.5f) return false;
  const float x = q[0] / magnitude, y = q[1] / magnitude;
  const float z = q[2] / magnitude, w = q[3] / magnitude;
  fwd = {2.0f * (x*z + w*y), 2.0f * (y*z - w*x), 1.0f - 2.0f * (x*x + y*y)};
  right = {1.0f - 2.0f * (y*y + z*z), 2.0f * (x*y + w*z), 2.0f * (x*z - w*y)};
  up = {2.0f * (x*y - w*z), 1.0f - 2.0f * (x*x + z*z), 2.0f * (y*z + w*x)};
  Vec3f normalized_fwd{}, normalized_right{}, normalized_up{};
  if (!normalized_axis(fwd, normalized_fwd) || !normalized_axis(right, normalized_right) ||
      !normalized_axis(up, normalized_up) ||
      !orthonormal_basis(normalized_fwd, normalized_right, normalized_up)) return false;
  fwd = normalized_fwd; right = normalized_right; up = normalized_up;
  return true;
}
CameraBasisSelection select_camera_basis(const float quaternion_xyzw[4],
                                         const Vec3f& axis_fwd,
                                         const Vec3f& axis_right,
                                         const Vec3f& axis_up,
                                         bool prefer_quaternion) {
  CameraBasisSelection result;
  Vec3f axes_fwd{}, axes_right{}, axes_up{};
  Vec3f quat_fwd{}, quat_right{}, quat_up{};
  result.axes_valid = basis_from_axes(axis_fwd, axis_right, axis_up,
                                      axes_fwd, axes_right, axes_up);
  result.quaternion_valid = basis_from_quaternion(quaternion_xyzw,
                                                  quat_fwd, quat_right, quat_up);
  if (result.axes_valid && result.quaternion_valid) {
    result.quaternion_axis_alignment =
        (dot(quat_fwd, axes_fwd) + dot(quat_right, axes_right) + dot(quat_up, axes_up)) / 3.0f;
    result.disagreement = result.quaternion_axis_alignment < 0.90f;
  }
  if (result.quaternion_valid && prefer_quaternion &&
      (!result.axes_valid || !result.disagreement)) {
    result.fwd = quat_fwd; result.right = quat_right; result.up = quat_up;
    result.source = CameraBasisSource::Quaternion;
  } else if (result.axes_valid) {
    result.fwd = axes_fwd; result.right = axes_right; result.up = axes_up;
    result.source = CameraBasisSource::RawAxes;
  } else if (result.quaternion_valid) {
    // A valid quaternion is still safer than rendering with an invalid basis.
    result.fwd = quat_fwd; result.right = quat_right; result.up = quat_up;
    result.source = CameraBasisSource::Quaternion;
  }
  return result;
}
std::vector<PlayerSnap> snapshot_players(const IMemory& memory,
                                         const ChainResult& chain,
                                         const Offsets& offsets,
                                         const Vec3f& local_pos,
                                         uint64_t expected_character_class) {
  std::vector<PlayerSnap> players;
  if (!chain.ok || !offsets.profile_configured) return players;
  const auto header = read_dict(memory, chain.dict, offsets);
  if (!header) return players;

  // Span the entries array, not the live count: holes are skipped by
  // dict_value_at, and an entry that moved past Count still gets drawn.
  for (int index = 0; static_cast<uint32_t>(index) < header->array_length; ++index) {
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
  // Prefer the game's own projection terms. A guessed FOV of 90 degrees plus
  // the overlay's aspect ratio is only correct for one lens on one monitor;
  // everything else drifts off the player towards the screen edges.
  const bool exact = c.proj_x > 0.01f && c.proj_y > 0.01f &&
                     std::isfinite(c.proj_x) && std::isfinite(c.proj_y);
  if(c.width<=0||c.height<=0) return false;
  if(!exact && (c.fov_deg<=1||c.fov_deg>=179)) return false;
  Vec3f d = sub(w, c.pos);
  float cz = dot(d, c.fwd); depth = cz;
  if(!(cz>0.05f) || !std::isfinite(cz)) return false;
  float cx = dot(d, c.right), cy = dot(d, c.up);
  float nx, ny;
  if(exact){
    nx = (cx/cz)/c.proj_x; ny = (cy/cz)/c.proj_y;
  } else {
    float tan_half = std::tan(c.fov_deg*0.01745329252f*0.5f);
    float aspect = (float)c.width/(float)c.height;
    nx = (cx/cz)/(tan_half*aspect); ny = (cy/cz)/tan_half;
  }
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
