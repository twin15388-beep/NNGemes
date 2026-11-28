#pragma once
// NZL ESP pipeline: memory -> snapshot -> projection -> draw commands.
// No Windows/D3D here: draw commands are plain data the overlay renders.
#include "nzl/chain.h"
#include <string>
#include <vector>
namespace nzl {
struct CameraView {
  Vec3f pos{}, fwd{0,0,1}, right{1,0,0}, up{0,1,0};
  float fov_deg = 90.0f;
  float head_height_m = 1.80f;
  int width = 1920, height = 1080;
  // Projection terms taken from the game's own projection matrix (m00 and
  // m11). When both are set world_to_screen uses them instead of a guessed
  // FOV and a guessed aspect ratio, which is what put boxes off the player.
  float proj_x = 0.0f, proj_y = 0.0f;
};
enum class CameraBasisSource { Invalid, RawAxes, Quaternion };
struct CameraBasisSelection {
  Vec3f fwd{}, right{}, up{};
  CameraBasisSource source = CameraBasisSource::Invalid;
  float quaternion_axis_alignment = -1.0f; // mean dot product: 1 = identical bases
  bool axes_valid = false;
  bool quaternion_valid = false;
  bool disagreement = false;
};
// Cross-check the profile-verified Forward/Right/Up vectors against the
// lower-confidence LookAtOrientation quaternion. Prefer the quaternion only
// when requested and it agrees with a valid raw basis (mean dot >= 0.90).
CameraBasisSelection select_camera_basis(const float quaternion_xyzw[4],
                                         const Vec3f& axis_fwd,
                                         const Vec3f& axis_right,
                                         const Vec3f& axis_up,
                                         bool prefer_quaternion);
struct PlayerSnap {
  uint64_t addr = 0; int32_t id = 0; Vec3f pos{}; bool dead = false; float dist = 0;
  int32_t hp = -1;    // -1 = unknown (HP chain marked unverified until live check)
  int32_t maxhp = 0;
  bool armor = false; // HasArmor primitive (unverified layout, live check)
  int32_t role = -2;  // Settler0 Robber1 Cop2 Citizen3 Prisoner4 via description Index (unverified)
};
struct DrawCmd {
  enum Kind { Line, Rect, Text } kind = Line;
  float x1 = 0, y1 = 0, x2 = 0, y2 = 0; // Line/Rect coords; Text at (x1,y1)
  std::string text;                      // Text only
  uint32_t color = 0xFFB388FF;           // NZL purple RGBA
};
// Enumerate _battleCharacters dict -> player snapshots (positions via Transform).
std::vector<PlayerSnap> snapshot_players(const IMemory& m, const ChainResult& chain,
                                         const Offsets& o, const Vec3f& local_pos,
                                         uint64_t expected_character_class = 0);
// Unity LH, Y-up, +Z forward, vertical FOV. 20% NDC margin like reference.
bool world_to_screen(const CameraView& c, const Vec3f& w, float& sx, float& sy, float& depth);
// Box (feet->head 1.8m) + name/dist text commands for on-screen players.
std::vector<DrawCmd> build_draw_list(const CameraView& c, const std::vector<PlayerSnap>& players, float max_dist);
}
