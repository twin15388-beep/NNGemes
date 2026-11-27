// NZL Studio overlay (Windows): persistent interactive menu + background ESP reconnect.
// UI stays responsive while the worker waits for ROB RUN, IL2CPP classes,
// and an active battle chain. The ESP window is click-through; the separate
// draggable menu window receives mouse input and releases game focus/cursor.
// INSERT toggles the menu; END exits.
// Aim: hold RIGHT MOUSE, nearest alive head to crosshair inside Aim FOV.
// Projection basis: BattleCameraModel LookAtCameraOrientation quaternion
// (menu can switch to Forward/Right/Up fields). HP/Armor chains are from
// the build-matched dump but marked UNVERIFIED until the owner confirms
// the bars match in game: HP = char+0x10 PrivateModel -> +0x1B0 Health
// -> +0x10 primitive data -> +0x18 value, max @Health+0x20;
// Armor flag = char+0x1B8 HasArmor primitive -> +0x10 -> +0x18.
#ifndef _WIN32
#error "Windows only"
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <algorithm>
#include <memory>
#include <iterator>
#include <windowsx.h>
#include "nzl/chain.h"
#include "nzl/driver_memory.h"
#include "nzl/esp_pipeline.h"
#include "../client/sc_client.h"

static DWORD find_pid(const wchar_t* name) {
  HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (s == INVALID_HANDLE_VALUE) return 0;
  PROCESSENTRY32W e{sizeof(e)}; DWORD pid = 0;
  if (Process32FirstW(s, &e)) do { if (_wcsicmp(e.szExeFile, name) == 0) { pid = e.th32ProcessID; break; } } while (Process32NextW(s, &e));
  CloseHandle(s); return pid;
}
static uint64_t find_gameassembly(DWORD pid) {
  uint64_t addr = 0x10000;
  for (int i = 0; i < 16384 && addr < 0x0000800000000000ULL; i++) {
    NaRegion r{};
    if (!NaQueryRegion(pid, addr, &r)) break;
    uint64_t next = r.base + (r.size ? r.size : 0x1000);
    if (next <= addr) break;
    addr = next;
    if (r.type != 0x1000000 || !r.base) continue;
    uint16_t mz = 0;
    if (!NaRead(pid, r.base, &mz) || mz != 0x5A4D) continue;
    uint32_t lfanew = 0; NaRead(pid, r.base + 0x3C, &lfanew);
    if (!lfanew || lfanew >= 0x1000) continue;
    uint16_t magic = 0; NaRead(pid, r.base + lfanew + 24, &magic);
    uint32_t dd = (magic == 0x20b) ? 112 : (magic == 0x10b) ? 96 : 0;
    if (!dd) continue;
    uint32_t erva = 0; NaRead(pid, r.base + lfanew + 24 + dd, &erva);
    if (!erva) continue;
    uint32_t nrva = 0; NaRead(pid, r.base + erva + 12, &nrva);
    char name[64] = {};
    if (nrva && NaRead(pid, r.base + nrva, 63, name)) {
      name[63] = 0;
      if (_stricmp(name, "GameAssembly.dll") == 0) return r.base;
    }
  }
  return 0;
}
static bool klass_name_is(const nzl::IMemory& m, uint64_t klass, const char* want) {
  auto np = m.ptr(klass + 0x10);
  if (!np || !*np) return false;
  char nm[64] = {};
  if (!m.read(*np, nm, sizeof(nm) - 1)) return false;
  nm[sizeof(nm) - 1] = 0;
  return std::strcmp(nm, want) == 0;
}
static nzl::Vec3f normalize(nzl::Vec3f v) {
  float l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
  return l > 0.001f ? nzl::Vec3f{v.x / l, v.y / l, v.z / l} : v;
}
// Unity quaternion (x,y,z,w) -> camera basis (forward = rotation * +Z).
static bool basis_from_quat(const nzl::IMemory& m, uint64_t quat_addr,
                            nzl::Vec3f& fwd, nzl::Vec3f& right, nzl::Vec3f& up) {
  float q[4] = {};
  if (!m.read(quat_addr, q, 16)) return false;
  float x = q[0], y = q[1], z = q[2], w = q[3];
  float len = std::sqrt(x * x + y * y + z * z + w * w);
  if (len < 0.5f || len > 1.5f) return false;
  x /= len; y /= len; z /= len; w /= len;
  fwd = normalize({2 * (x * z + w * y), 2 * (y * z - w * x), 1 - 2 * (x * x + y * y)});
  right = normalize({1 - 2 * (y * y + z * z), 2 * (x * y + w * z), 2 * (x * z - w * y)});
  up = normalize({2 * (x * y - w * z), 1 - 2 * (x * x + z * z), 2 * (y * z + w * x)});
  return true;
}
struct RenderEnt {
  float hx = 0, hy = 0, fx = 0, fy = 0; // head / feet screen
  float ax = 0, ay = 0;                 // aim point screen (selected bone)
  nzl::Vec3f pos{}; int id = 0; float dist = 0; bool dead = false;
  int hp = -1, maxhp = 0; bool armor = false;
  int role = -2; // PlayerRole via description Index: 0 Settler 1 Robber 2 Cop 3 Citizen 4 Prisoner (unverified mapping)
};
struct CarEnt { nzl::Vec3f pos{}; float dist = 0; float sx = 0, sy = 0; bool on = false; };
struct LootEnt { nzl::Vec3f pos{}; float dist = 0; float sx = 0, sy = 0; bool on = false; };
struct Ctx {
  DWORD pid = 0;
  nzl::DriverMemory* mem = nullptr;
  nzl::Offsets off;
  nzl::ChainResult chain;
  std::vector<RenderEnt> render;
  std::vector<LootEnt> loot;
  std::vector<CarEnt> cars;
  nzl::Vec3f cam_fwd{0, 0, 1}, cam_right{1, 0, 0}, radar_cam{};
  float fov = 90.0f;
  int width = 1920, height = 1080;
  // UI state is owned by the window thread; worker-published data is protected by g_state_mutex.
  bool menu = false;
  int menu_tab = 0;
  int menu_x = 32, menu_y = 48;
  bool esp_ready = false;
  DWORD attached_pid = 0;
  std::string status = "Waiting for ROB RUN";
  HWND overlay_wnd = nullptr, menu_wnd = nullptr;
  bool show_dead = false, show_boxes = true, show_skeleton = false,
       show_text = true, show_hp = false, show_armor = false,
       show_loot = false, show_radar = false, aim = false, basis_quat = true;
  bool show_roles = true, show_cars = false, tracers = false, visible_only = false;
  float max_dist = 500.0f, aim_fov = 150.0f, aim_smooth = 5.0f, radar_range = 120.0f;
  int aim_bone = 0;   // 0 Head 1 Chest 2 Pelvis
  int aim_key = 0;    // index into kAimKeys
  int menu_key = 0;   // index into kMenuKeys
};
static const int kAimKeys[] = {VK_RBUTTON, VK_LBUTTON, VK_MENU, VK_SHIFT};
static const char* kAimKeyNames[] = {"RMB", "LMB", "ALT", "SHIFT"};
static const int kMenuKeys[] = {VK_INSERT, VK_DELETE, VK_HOME, VK_F9};
static const char* kMenuKeyNames[] = {"INSERT", "DELETE", "HOME", "F9"};
static const char* kBoneNames[] = {"Head", "Chest", "Pelvis"};
static const char* kRoleNames[] = {"SETTLER", "ROBBER", "COP", "CITIZEN", "PRISONER"};
static std::mutex g_state_mutex;
static std::atomic_bool g_stop{false};
// Generic dictionary walk: entries array length, skip hash<0 slots,
// batched in chunks respecting the 64-item driver batch cap.
static std::vector<uint64_t> dict_values(Ctx& c, uint64_t dict) {
  std::vector<uint64_t> out;
  if (!dict) return out;
  auto entries = c.mem->ptr(dict + c.off.dict_entries);
  if (!entries || !*entries) return out;
  uint64_t arrlen = 0;
  if (!c.mem->read(*entries + 0x18, &arrlen, 8)) return out;
  int total = (int)(arrlen > 2048 ? 2048 : arrlen);
  for (int base = 0; base < total; base += 24) {
    int n = (total - base) < 24 ? (total - base) : 24;
    std::vector<NaItem> items; std::vector<uint8_t> buf(n * 16, 0);
    for (int i = 0; i < n; i++) {
      uint64_t e = *entries + 0x20ULL + (uint64_t)(base + i) * c.off.dict_stride;
      items.push_back(NaItem{e, (uint32_t)(i * 16), 4});
      items.push_back(NaItem{e + 0x10, (uint32_t)(i * 16 + 8), 8});
    }
    if (!NaBatchRead(c.pid, items.data(), (uint32_t)items.size(), buf.data(), n * 16)) continue;
    for (int i = 0; i < n; i++) {
      int32_t hash = 0; uint64_t v = 0;
      std::memcpy(&hash, buf.data() + i * 16, 4);
      std::memcpy(&v, buf.data() + i * 16 + 8, 8);
      if (hash >= 0 && v) out.push_back(v);
    }
  }
  return out;
}
struct BatchField { uint64_t address = 0; uint32_t size = 0; };
// Read up to 64 independent fields per driver command, preserving one
// output slot per request. This avoids hundreds of serialized 1 ms reads
// for HP/armor/role chains on a full player list.
static std::vector<uint64_t> batch_read_fields(uint32_t pid, const std::vector<BatchField>& fields) {
  std::vector<uint64_t> values(fields.size(), 0);
  for (size_t base = 0; base < fields.size(); base += 64) {
    uint32_t n = (uint32_t)std::min<size_t>(64, fields.size() - base);
    std::vector<NaItem> items; items.reserve(n);
    std::vector<uint8_t> buf((size_t)n * 8, 0);
    for (uint32_t i = 0; i < n; ++i) {
      const auto& f = fields[base + i];
      items.push_back(NaItem{f.address, i * 8, f.address ? f.size : 0});
    }
    if (!NaBatchRead(pid, items.data(), n, buf.data(), n * 8)) continue;
    for (uint32_t i = 0; i < n; ++i) std::memcpy(&values[base + i], buf.data() + i * 8, 8);
  }
  return values;
}
// Positions for objects whose transform field sits at `tr_off` and whose
// transform concrete type is CharacterTransformModel layout
// (float X/Y/Z @ +0x10/+0x14/+0x18, verified in build dump).
static void fill_positions(Ctx& c, const std::vector<uint64_t>& objs, uint32_t tr_off,
                           std::vector<nzl::Vec3f>& pos_out) {
  int n = (int)objs.size();
  pos_out.assign(n, {});
  std::vector<uint64_t> trs(n, 0);
  for (int base = 0; base < n; base += 32) {
    int m = (n - base) < 32 ? (n - base) : 32;
    std::vector<NaItem> ti; std::vector<uint8_t> tb(m * 8, 0);
    for (int i = 0; i < m; i++) ti.push_back(NaItem{objs[base + i] + tr_off, (uint32_t)(i * 8), 8});
    if (NaBatchRead(c.pid, ti.data(), (uint32_t)ti.size(), tb.data(), m * 8))
      for (int i = 0; i < m; i++) std::memcpy(&trs[base + i], tb.data() + i * 8, 8);
    std::vector<NaItem> pi; std::vector<uint8_t> pb(m * 12, 0);
    for (int i = 0; i < m; i++) if (trs[base + i]) pi.push_back(NaItem{trs[base + i] + 0x10, (uint32_t)(i * 12), 12});
    if (!pi.empty() && NaBatchRead(c.pid, pi.data(), (uint32_t)pi.size(), pb.data(), m * 12))
      for (int i = 0; i < m; i++) { float f[3]; std::memcpy(f, pb.data() + i * 12, 12); pos_out[base + i] = {f[0], f[1], f[2]}; }
  }
}
static std::vector<nzl::PlayerSnap> snapshot_players(Ctx& c, const nzl::Vec3f& local) {
  std::vector<nzl::PlayerSnap> out;
  auto chars = dict_values(c, c.chain.dict);
  int n = (int)chars.size();
  if (!n) return out;
  std::vector<nzl::Vec3f> pos; fill_positions(c, chars, c.off.char_transform, pos);
  std::vector<int32_t> ids(n, 0); std::vector<uint8_t> dead(n, 0);
  for (int base = 0; base < n; base += 32) {
    int m = (n - base) < 32 ? (n - base) : 32;
    std::vector<NaItem> ii; std::vector<uint8_t> ib(m * 8, 0);
    for (int i = 0; i < m; i++) {
      ii.push_back(NaItem{chars[base + i] + c.off.char_id, (uint32_t)(i * 8), 4});
      ii.push_back(NaItem{chars[base + i] + c.off.char_is_dead, (uint32_t)(i * 8 + 4), 1});
    }
    if (NaBatchRead(c.pid, ii.data(), (uint32_t)ii.size(), ib.data(), m * 8))
      for (int i = 0; i < m; i++) { std::memcpy(&ids[base + i], ib.data() + i * 8, 4); dead[base + i] = ib[i * 8 + 4]; }
  }
  std::vector<uint64_t> hp_priv(n), armor_prim(n), role_prim(n);
  if (c.show_hp || c.show_armor || c.show_roles) {
    std::vector<BatchField> req; std::vector<int> hp_i(n, -1), armor_i(n, -1), role_i(n, -1);
    for (int i = 0; i < n; ++i) {
      if (c.show_hp) { hp_i[i] = (int)req.size(); req.push_back({chars[i] + 0x10, 8}); }
      if (c.show_armor) { armor_i[i] = (int)req.size(); req.push_back({chars[i] + 0x1B8, 8}); }
      if (c.show_roles) { role_i[i] = (int)req.size(); req.push_back({chars[i] + 0x208, 8}); }
    }
    auto v = batch_read_fields(c.pid, req);
    for (int i = 0; i < n; ++i) {
      if (hp_i[i] >= 0) hp_priv[i] = v[hp_i[i]];
      if (armor_i[i] >= 0) armor_prim[i] = v[armor_i[i]];
      if (role_i[i] >= 0) role_prim[i] = v[role_i[i]];
    }
  }
  std::vector<uint64_t> hp_model(n), armor_data(n), role_data(n);
  {
    std::vector<BatchField> req; std::vector<int> hp_i(n, -1), armor_i(n, -1), role_i(n, -1);
    for (int i = 0; i < n; ++i) {
      if (c.show_hp && hp_priv[i]) { hp_i[i] = (int)req.size(); req.push_back({hp_priv[i] + 0x1B0, 8}); }
      if (c.show_armor && armor_prim[i]) { armor_i[i] = (int)req.size(); req.push_back({armor_prim[i] + 0x10, 8}); }
      if (c.show_roles && role_prim[i]) { role_i[i] = (int)req.size(); req.push_back({role_prim[i] + 0x10, 8}); }
    }
    auto v = batch_read_fields(c.pid, req);
    for (int i = 0; i < n; ++i) {
      if (hp_i[i] >= 0) hp_model[i] = v[hp_i[i]];
      if (armor_i[i] >= 0) armor_data[i] = v[armor_i[i]];
      if (role_i[i] >= 0) role_data[i] = v[role_i[i]];
    }
  }
  std::vector<uint64_t> hp_value_data(n), hp_max(n), armor_value(n), role_desc(n);
  {
    std::vector<BatchField> req; std::vector<int> hp_data_i(n, -1), hp_max_i(n, -1), armor_i(n, -1), role_i(n, -1);
    for (int i = 0; i < n; ++i) {
      if (c.show_hp && hp_model[i]) {
        hp_data_i[i] = (int)req.size(); req.push_back({hp_model[i] + 0x10, 8});
        hp_max_i[i] = (int)req.size(); req.push_back({hp_model[i] + 0x20, 4});
      }
      if (c.show_armor && armor_data[i]) { armor_i[i] = (int)req.size(); req.push_back({armor_data[i] + 0x18, 1}); }
      if (c.show_roles && role_data[i]) { role_i[i] = (int)req.size(); req.push_back({role_data[i] + 0x18, 8}); }
    }
    auto v = batch_read_fields(c.pid, req);
    for (int i = 0; i < n; ++i) {
      if (hp_data_i[i] >= 0) hp_value_data[i] = v[hp_data_i[i]];
      if (hp_max_i[i] >= 0) hp_max[i] = (uint32_t)v[hp_max_i[i]];
      if (armor_i[i] >= 0) armor_value[i] = v[armor_i[i]];
      if (role_i[i] >= 0) role_desc[i] = v[role_i[i]];
    }
  }
  std::vector<uint64_t> hp_value(n), role_index(n);
  {
    std::vector<BatchField> req; std::vector<int> hp_i(n, -1), role_i(n, -1);
    for (int i = 0; i < n; ++i) {
      if (c.show_hp && hp_value_data[i]) { hp_i[i] = (int)req.size(); req.push_back({hp_value_data[i] + 0x18, 4}); }
      if (c.show_roles && role_desc[i]) { role_i[i] = (int)req.size(); req.push_back({role_desc[i] + 0x18, 4}); }
    }
    auto v = batch_read_fields(c.pid, req);
    for (int i = 0; i < n; ++i) {
      if (hp_i[i] >= 0) hp_value[i] = (uint32_t)v[hp_i[i]];
      if (role_i[i] >= 0) role_index[i] = (uint32_t)v[role_i[i]];
    }
  }
  for (int i = 0; i < n; i++) {
    nzl::Vec3f p = pos[i];
    if ((p.x == 0 && p.y == 0 && p.z == 0) || !std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) continue;
    nzl::PlayerSnap s; s.addr = chars[i]; s.id = ids[i]; s.dead = dead[i] != 0; s.pos = p;
    float dx = p.x - local.x, dy = p.y - local.y, dz = p.z - local.z;
    s.dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (c.show_hp) {
      int32_t hp = (int32_t)hp_value[i], mx = (int32_t)hp_max[i];
      if (hp_value_data[i] && hp >= 0 && hp <= 100000) s.hp = hp;
      if (mx > 0 && mx <= 100000) s.maxhp = mx;
    }
    if (c.show_armor) s.armor = armor_data[i] && armor_value[i] != 0;
    if (c.show_roles && role_desc[i] && role_index[i] <= 4) s.role = (int32_t)role_index[i];
    out.push_back(s);
  }
  return out;
}
static std::vector<LootEnt> snapshot_loot(Ctx& c, const nzl::Vec3f& local) {
  std::vector<LootEnt> out;
  auto dict = c.mem->ptr(c.chain.world_entities + 0x38); // _loots (dump-verified)
  if (!dict) return out;
  auto objs = dict_values(c, *dict);
  std::vector<nzl::Vec3f> pos; fill_positions(c, objs, 0x20 /*LootModel.Transform*/, pos);
  for (size_t i = 0; i < objs.size(); i++) {
    nzl::Vec3f p = pos[i];
    if ((p.x == 0 && p.y == 0 && p.z == 0) || !std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) continue;
    float dx = p.x - local.x, dy = p.y - local.y, dz = p.z - local.z;
    LootEnt e; e.pos = p; e.dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    out.push_back(e);
  }
  return out;
}
static std::vector<CarEnt> snapshot_cars(Ctx& c, const nzl::Vec3f& local) {
  std::vector<CarEnt> out;
  auto dict = c.mem->ptr(c.chain.world_entities + 0x60); // _cars (dump-verified)
  if (!dict) return out;
  auto objs = dict_values(c, *dict);
  int n = (int)objs.size();
  std::vector<uint64_t> trs(n, 0);
  std::vector<nzl::Vec3f> pos(n, nzl::Vec3f{});
  for (int base = 0; base < n; base += 32) {
    int m = (n - base) < 32 ? (n - base) : 32;
    std::vector<NaItem> ti; std::vector<uint8_t> tb(m * 8, 0);
    for (int i = 0; i < m; i++) ti.push_back(NaItem{objs[base + i] + 0xC0, (uint32_t)(i * 8), 8}); // CarModel.Transform
    if (NaBatchRead(c.pid, ti.data(), (uint32_t)ti.size(), tb.data(), m * 8))
      for (int i = 0; i < m; i++) std::memcpy(&trs[base + i], tb.data() + i * 8, 8);
    std::vector<NaItem> pi; std::vector<uint8_t> pb(m * 12, 0);
    for (int i = 0; i < m; i++) if (trs[base + i]) pi.push_back(NaItem{trs[base + i] + 0x38, (uint32_t)(i * 12), 12}); // _centerOfMassPosition
    if (!pi.empty() && NaBatchRead(c.pid, pi.data(), (uint32_t)pi.size(), pb.data(), m * 12))
      for (int i = 0; i < m; i++) { float f[3]; std::memcpy(f, pb.data() + i * 12, 12); pos[base + i] = {f[0], f[1], f[2]}; }
  }
  for (int i = 0; i < n; i++) {
    nzl::Vec3f p = pos[i];
    if ((p.x == 0 && p.y == 0 && p.z == 0) || !std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) continue;
    float dx = p.x - local.x, dy = p.y - local.y, dz = p.z - local.z;
    CarEnt e; e.pos = p; e.dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    out.push_back(e);
  }
  return out;
}
// Settings persistence: nzl_settings.ini next to the exe, written on
// every menu change, read at startup.
static std::string settings_path() {
  char exe[MAX_PATH] = {};
  GetModuleFileNameA(nullptr, exe, MAX_PATH);
  std::string p = exe;
  size_t slash = p.find_last_of("\\/");
  return (slash == std::string::npos ? std::string("nzl_settings.ini") : p.substr(0, slash + 1) + "nzl_settings.ini");
}
static void save_settings(Ctx& c) {
  FILE* f = std::fopen(settings_path().c_str(), "w");
  if (!f) return;
  std::fprintf(f, "fov=%.1f\nmax_dist=%.1f\nradar_range=%.1f\naim_fov=%.1f\naim_smooth=%.1f\n",
               c.fov, c.max_dist, c.radar_range, c.aim_fov, c.aim_smooth);
  std::fprintf(f, "show_dead=%d\nboxes=%d\nskeleton=%d\nlabels=%d\nhp=%d\narmor=%d\nroles=%d\nloot=%d\ncars=%d\nradar=%d\ntracers=%d\naim=%d\nbasis_quat=%d\nvisible_only=%d\naim_bone=%d\naim_key=%d\nmenu_key=%d\n",
               c.show_dead, c.show_boxes, c.show_skeleton, c.show_text, c.show_hp, c.show_armor,
               c.show_roles, c.show_loot, c.show_cars, c.show_radar, c.tracers, c.aim,
               c.basis_quat, c.visible_only, c.aim_bone, c.aim_key, c.menu_key);
  std::fclose(f);
}
static void load_settings(Ctx& c) {
  FILE* f = std::fopen(settings_path().c_str(), "r");
  if (!f) return;
  char line[128];
  while (std::fgets(line, sizeof(line), f)) {
    char key[64]; double val;
    if (std::sscanf(line, "%63[^=]=%lf", key, &val) != 2) continue;
    std::string k = key;
    if (k == "fov") c.fov = (float)val; else if (k == "max_dist") c.max_dist = (float)val;
    else if (k == "radar_range") c.radar_range = (float)val; else if (k == "aim_fov") c.aim_fov = (float)val;
    else if (k == "aim_smooth") c.aim_smooth = (float)val;
    else if (k == "show_dead") c.show_dead = val != 0; else if (k == "boxes") c.show_boxes = val != 0;
    else if (k == "skeleton") c.show_skeleton = val != 0; else if (k == "labels") c.show_text = val != 0;
    else if (k == "hp") c.show_hp = val != 0; else if (k == "armor") c.show_armor = val != 0;
    else if (k == "roles") c.show_roles = val != 0; else if (k == "loot") c.show_loot = val != 0;
    else if (k == "cars") c.show_cars = val != 0; else if (k == "radar") c.show_radar = val != 0;
    else if (k == "tracers") c.tracers = val != 0; else if (k == "aim") c.aim = val != 0;
    else if (k == "basis_quat") c.basis_quat = val != 0; else if (k == "visible_only") c.visible_only = val != 0;
    else if (k == "aim_bone") c.aim_bone = std::clamp((int)val, 0, 2); else if (k == "aim_key") c.aim_key = std::clamp((int)val, 0, 3);
    else if (k == "menu_key") c.menu_key = std::clamp((int)val, 0, 3);
  }
  c.fov = std::clamp(c.fov, 40.0f, 140.0f);
  c.max_dist = std::clamp(c.max_dist, 25.0f, 1500.0f);
  c.aim_fov = std::clamp(c.aim_fov, 30.0f, 600.0f);
  c.aim_smooth = std::clamp(c.aim_smooth, 1.0f, 20.0f);
  c.radar_range = std::clamp(c.radar_range, 30.0f, 500.0f);
  c.aim_bone = std::clamp(c.aim_bone, 0, 2);
  c.aim_key = std::clamp(c.aim_key, 0, 3);
  c.menu_key = std::clamp(c.menu_key, 0, 3);
  std::fclose(f);
}
static HWND g_game_wnd = nullptr;
static Ctx* g_ctx = nullptr;
static constexpr int kMenuWidth = 800;
static constexpr int kMenuHeight = 560;
static constexpr int kSidebarWidth = 152;
static constexpr int kContentX = 166;
static constexpr int kCardWidth = 300;
static constexpr int kCardHeight = 58;
static constexpr int kCardGapY = 66;
static constexpr int kRowsPerColumn = 6;
static const char* kTabNames[] = {"Aim", "Visuals", "Loot", "Radar", "Misc"};
static constexpr int kTabCount = 5;

enum class OptionId {
  AimEnabled, AimKey, AimBone, AimFov, AimSmooth,
  Fov, MaxDistance, ShowDead, Boxes, Skeleton, Labels, Hp, Armor, Roles, Tracers, VisibleOnly,
  Loot, Cars, Radar, RadarRange, BasisQuat, MenuKey
};
enum class OptionKind { Toggle, Slider, Cycle };
struct MenuOption { OptionId id; const char* label; OptionKind kind; float minv = 0, maxv = 0, step = 1; };
static const MenuOption kAimOptions[] = {
  {OptionId::AimEnabled, "Aim assist", OptionKind::Toggle},
  {OptionId::AimKey, "Hold key", OptionKind::Cycle},
  {OptionId::AimBone, "Target point", OptionKind::Cycle},
  {OptionId::AimFov, "Aim radius", OptionKind::Slider, 30, 600, 10},
  {OptionId::AimSmooth, "Smoothing", OptionKind::Slider, 1, 20, 1},
};
static const MenuOption kVisualOptions[] = {
  {OptionId::Fov, "Projection FOV", OptionKind::Slider, 40, 140, 1},
  {OptionId::MaxDistance, "Max distance", OptionKind::Slider, 25, 1500, 25},
  {OptionId::Boxes, "Player boxes", OptionKind::Toggle},
  {OptionId::Skeleton, "Skeleton", OptionKind::Toggle},
  {OptionId::Labels, "Names / distance", OptionKind::Toggle},
  {OptionId::ShowDead, "Show dead", OptionKind::Toggle},
  {OptionId::Hp, "HP bars", OptionKind::Toggle},
  {OptionId::Armor, "Armor marks", OptionKind::Toggle},
  {OptionId::Roles, "Role labels", OptionKind::Toggle},
  {OptionId::Tracers, "Snaplines", OptionKind::Toggle},
  {OptionId::VisibleOnly, "On-screen only", OptionKind::Toggle},
};
static const MenuOption kLootOptions[] = {
  {OptionId::Loot, "Loot ESP", OptionKind::Toggle},
  {OptionId::Cars, "Vehicle ESP", OptionKind::Toggle},
};
static const MenuOption kRadarOptions[] = {
  {OptionId::Radar, "Radar", OptionKind::Toggle},
  {OptionId::RadarRange, "Radar range", OptionKind::Slider, 30, 500, 10},
};
static const MenuOption kMiscOptions[] = {
  {OptionId::BasisQuat, "Quaternion camera basis", OptionKind::Toggle},
  {OptionId::MenuKey, "Menu hotkey", OptionKind::Cycle},
};
static const MenuOption* page_options(int tab, size_t& count) {
  switch (tab) {
    case 0: count = sizeof(kAimOptions) / sizeof(kAimOptions[0]); return kAimOptions;
    case 1: count = sizeof(kVisualOptions) / sizeof(kVisualOptions[0]); return kVisualOptions;
    case 2: count = sizeof(kLootOptions) / sizeof(kLootOptions[0]); return kLootOptions;
    case 3: count = sizeof(kRadarOptions) / sizeof(kRadarOptions[0]); return kRadarOptions;
    default: count = sizeof(kMiscOptions) / sizeof(kMiscOptions[0]); return kMiscOptions;
  }
}
static void copy_config(Ctx& dst, const Ctx& src) {
  dst.show_dead = src.show_dead; dst.show_boxes = src.show_boxes; dst.show_skeleton = src.show_skeleton;
  dst.show_text = src.show_text; dst.show_hp = src.show_hp; dst.show_armor = src.show_armor;
  dst.show_loot = src.show_loot; dst.show_radar = src.show_radar; dst.aim = src.aim; dst.basis_quat = src.basis_quat;
  dst.show_roles = src.show_roles; dst.show_cars = src.show_cars; dst.tracers = src.tracers; dst.visible_only = src.visible_only;
  dst.fov = src.fov; dst.max_dist = src.max_dist; dst.aim_fov = src.aim_fov; dst.aim_smooth = src.aim_smooth; dst.radar_range = src.radar_range;
  dst.aim_bone = src.aim_bone; dst.aim_key = src.aim_key; dst.menu_key = src.menu_key;
  dst.width = src.width; dst.height = src.height; dst.menu = src.menu;
}
static bool option_value(const Ctx& c, OptionId id) {
  switch (id) {
    case OptionId::AimEnabled: return c.aim;
    case OptionId::ShowDead: return c.show_dead;
    case OptionId::Boxes: return c.show_boxes;
    case OptionId::Skeleton: return c.show_skeleton;
    case OptionId::Labels: return c.show_text;
    case OptionId::Hp: return c.show_hp;
    case OptionId::Armor: return c.show_armor;
    case OptionId::Roles: return c.show_roles;
    case OptionId::Tracers: return c.tracers;
    case OptionId::VisibleOnly: return c.visible_only;
    case OptionId::Loot: return c.show_loot;
    case OptionId::Cars: return c.show_cars;
    case OptionId::Radar: return c.show_radar;
    case OptionId::BasisQuat: return c.basis_quat;
    default: return false;
  }
}
static float option_number(const Ctx& c, OptionId id) {
  switch (id) {
    case OptionId::Fov: return c.fov;
    case OptionId::MaxDistance: return c.max_dist;
    case OptionId::AimFov: return c.aim_fov;
    case OptionId::AimSmooth: return c.aim_smooth;
    case OptionId::RadarRange: return c.radar_range;
    default: return 0;
  }
}
static void set_option_number(Ctx& c, OptionId id, float value) {
  switch (id) {
    case OptionId::Fov: c.fov = value; break;
    case OptionId::MaxDistance: c.max_dist = value; break;
    case OptionId::AimFov: c.aim_fov = value; break;
    case OptionId::AimSmooth: c.aim_smooth = value; break;
    case OptionId::RadarRange: c.radar_range = value; break;
    default: break;
  }
}
static void set_slider_from_x(Ctx& c, const MenuOption& item, int x, int left) {
  float lo = item.minv, hi = item.maxv;
  float t = (float)(x - (left + 16)) / (float)(kCardWidth - 32);
  t = std::clamp(t, 0.0f, 1.0f);
  float value = lo + t * (hi - lo);
  value = lo + std::round((value - lo) / item.step) * item.step;
  set_option_number(c, item.id, std::clamp(value, lo, hi));
}
static void cycle_option(Ctx& c, OptionId id, int direction = 1) {
  switch (id) {
    case OptionId::AimKey: c.aim_key = (c.aim_key + direction + 4) % 4; break;
    case OptionId::AimBone: c.aim_bone = (c.aim_bone + direction + 3) % 3; break;
    case OptionId::MenuKey: c.menu_key = (c.menu_key + direction + 4) % 4; break;
    default: break;
  }
}
static void activate_option(Ctx& c, OptionId id) {
  switch (id) {
    case OptionId::AimEnabled: c.aim = !c.aim; break;
    case OptionId::ShowDead: c.show_dead = !c.show_dead; break;
    case OptionId::Boxes: c.show_boxes = !c.show_boxes; break;
    case OptionId::Skeleton: c.show_skeleton = !c.show_skeleton; break;
    case OptionId::Labels: c.show_text = !c.show_text; break;
    case OptionId::Hp: c.show_hp = !c.show_hp; break;
    case OptionId::Armor: c.show_armor = !c.show_armor; break;
    case OptionId::Roles: c.show_roles = !c.show_roles; break;
    case OptionId::Tracers: c.tracers = !c.tracers; break;
    case OptionId::VisibleOnly: c.visible_only = !c.visible_only; break;
    case OptionId::Loot: c.show_loot = !c.show_loot; break;
    case OptionId::Cars: c.show_cars = !c.show_cars; break;
    case OptionId::Radar: c.show_radar = !c.show_radar; break;
    case OptionId::BasisQuat: c.basis_quat = !c.basis_quat; break;
    case OptionId::AimKey: case OptionId::AimBone: case OptionId::MenuKey: cycle_option(c, id); break;
    default: break;
  }
}
static RECT option_rect(size_t index) {
  int col = (int)(index / kRowsPerColumn), row = (int)(index % kRowsPerColumn);
  int left = kContentX + col * (kCardWidth + 12);
  int top = 104 + row * kCardGapY;
  return RECT{left, top, left + kCardWidth, top + kCardHeight};
}
static void draw_text(HDC dc, int x, int y, const char* text, COLORREF color) {
  SetTextColor(dc, color);
  TextOutA(dc, x, y, text, (int)std::strlen(text));
}
static void draw_menu_option(HDC dc, const Ctx& c, const MenuOption& item, size_t index) {
  RECT r = option_rect(index);
  HBRUSH card = CreateSolidBrush(RGB(28, 29, 35));
  FillRect(dc, &r, card); DeleteObject(card);
  HPEN border = CreatePen(PS_SOLID, 1, RGB(48, 49, 58));
  HGDIOBJ oldpen = SelectObject(dc, border);
  HGDIOBJ oldbrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
  Rectangle(dc, r.left, r.top, r.right, r.bottom);
  SelectObject(dc, oldbrush); SelectObject(dc, oldpen); DeleteObject(border);
  draw_text(dc, r.left + 12, r.top + 8, item.label, RGB(230, 230, 238));
  char value[48] = {};
  if (item.kind == OptionKind::Toggle) {
    bool on = option_value(c, item.id);
    std::snprintf(value, sizeof(value), "%s", on ? "ON" : "OFF");
    RECT pill{r.right - 70, r.top + 8, r.right - 12, r.top + 31};
    HBRUSH pb = CreateSolidBrush(on ? RGB(107, 63, 190) : RGB(55, 56, 64));
    FillRect(dc, &pill, pb); DeleteObject(pb);
    draw_text(dc, pill.left + 16, pill.top + 5, value, RGB(255, 255, 255));
    draw_text(dc, r.left + 12, r.top + 36, "Click to toggle", RGB(132, 134, 146));
  } else if (item.kind == OptionKind::Slider) {
    float v = option_number(c, item.id);
    std::snprintf(value, sizeof(value), "%.0f", v);
    draw_text(dc, r.right - 54, r.top + 8, value, RGB(200, 174, 255));
    RECT track{r.left + 14, r.top + 43, r.right - 14, r.top + 48};
    HBRUSH tb = CreateSolidBrush(RGB(60, 61, 70)); FillRect(dc, &track, tb); DeleteObject(tb);
    float t = item.maxv > item.minv ? (v - item.minv) / (item.maxv - item.minv) : 0;
    t = std::clamp(t, 0.0f, 1.0f);
    RECT fill = track; fill.right = fill.left + (int)((fill.right - fill.left) * t);
    HBRUSH fb = CreateSolidBrush(RGB(158, 95, 245)); FillRect(dc, &fill, fb); DeleteObject(fb);
    HBRUSH knob = CreateSolidBrush(RGB(236, 220, 255));
    int kx = fill.right;
    RECT kr{kx - 4, track.top - 3, kx + 4, track.bottom + 3};
    FillRect(dc, &kr, knob); DeleteObject(knob);
  } else {
    if (item.id == OptionId::AimKey) std::snprintf(value, sizeof(value), "%s", kAimKeyNames[std::clamp(c.aim_key, 0, 3)]);
    else if (item.id == OptionId::AimBone) std::snprintf(value, sizeof(value), "%s", kBoneNames[std::clamp(c.aim_bone, 0, 2)]);
    else std::snprintf(value, sizeof(value), "%s", kMenuKeyNames[std::clamp(c.menu_key, 0, 3)]);
    RECT pill{r.right - 102, r.top + 9, r.right - 12, r.top + 34};
    HBRUSH pb = CreateSolidBrush(RGB(49, 39, 68)); FillRect(dc, &pill, pb); DeleteObject(pb);
    draw_text(dc, pill.left + 9, pill.top + 6, value, RGB(210, 188, 255));
    draw_text(dc, r.left + 12, r.top + 38, "Click to cycle", RGB(132, 134, 146));
  }
}
static void draw_menu(HDC dc, const RECT& client, const Ctx& c) {
  HBRUSH bg = CreateSolidBrush(RGB(16, 17, 21)); FillRect(dc, &client, bg); DeleteObject(bg);
  RECT side{0, 0, kSidebarWidth, client.bottom};
  HBRUSH sb = CreateSolidBrush(RGB(20, 20, 25)); FillRect(dc, &side, sb); DeleteObject(sb);
  SetBkMode(dc, TRANSPARENT);
  HFONT title = CreateFontA(22, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");
  HFONT normal = CreateFontA(14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");
  HFONT small = CreateFontA(12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");
  HGDIOBJ oldfont = SelectObject(dc, title);
  draw_text(dc, 18, 20, "NZL", RGB(198, 145, 255));
  SelectObject(dc, normal); draw_text(dc, 66, 25, "STUDIO", RGB(238, 238, 244));
  SelectObject(dc, small); draw_text(dc, 18, 57, "ROB RUN  |  OVERLAY", RGB(120, 122, 136));
  for (int i = 0; i < kTabCount; ++i) {
    RECT tr{10, 96 + i * 54, kSidebarWidth - 10, 140 + i * 54};
    if (i == c.menu_tab) {
      HBRUSH active = CreateSolidBrush(RGB(53, 35, 76)); FillRect(dc, &tr, active); DeleteObject(active);
      RECT stripe{10, tr.top, 13, tr.bottom}; HBRUSH accent = CreateSolidBrush(RGB(176, 104, 255)); FillRect(dc, &stripe, accent); DeleteObject(accent);
    }
    SelectObject(dc, normal);
    draw_text(dc, 25, tr.top + 14, kTabNames[i], i == c.menu_tab ? RGB(245, 235, 255) : RGB(155, 157, 170));
  }
  SelectObject(dc, small);
  draw_text(dc, 18, client.bottom - 42, "INSERT  hide menu", RGB(122, 124, 138));
  draw_text(dc, 18, client.bottom - 24, "END  exit", RGB(122, 124, 138));
  SelectObject(dc, title);
  draw_text(dc, kContentX, 20, kTabNames[std::clamp(c.menu_tab, 0, kTabCount - 1)], RGB(240, 239, 245));
  SelectObject(dc, small);
  COLORREF statusColor = c.esp_ready ? RGB(120, 230, 160) : RGB(232, 190, 105);
  draw_text(dc, kContentX, 58, c.status.c_str(), statusColor);
  RECT close{client.right - 42, 10, client.right - 10, 40};
  HBRUSH cb = CreateSolidBrush(RGB(42, 34, 48)); FillRect(dc, &close, cb); DeleteObject(cb);
  draw_text(dc, close.left + 11, close.top + 7, "X", RGB(220, 198, 245));
  size_t count = 0; const MenuOption* items = page_options(c.menu_tab, count);
  for (size_t i = 0; i < count; ++i) draw_menu_option(dc, c, items[i], i);
  SelectObject(dc, oldfont);
  DeleteObject(title); DeleteObject(normal); DeleteObject(small);
}
static RECT g_option_rect_drag{};
static int g_drag_option = -1;
static bool g_drag_modified = false;
static bool set_menu_visible(bool visible);
static void focus_window(HWND target) {
  if (!target || !IsWindow(target)) return;
  HWND fg = GetForegroundWindow();
  DWORD fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
  DWORD myThread = GetCurrentThreadId();
  bool attached = fgThread && fgThread != myThread && AttachThreadInput(myThread, fgThread, TRUE);
  BringWindowToTop(target);
  SetForegroundWindow(target);
  SetFocus(target);
  if (attached) AttachThreadInput(myThread, fgThread, FALSE);
}
static bool set_menu_visible(bool visible) {
  if (!g_ctx) return false;
  HWND menu = nullptr; HWND game = nullptr; int x = 32, y = 48;
  {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (g_ctx->menu == visible) return true;
    g_ctx->menu = visible;
    menu = g_ctx->menu_wnd;
    game = g_game_wnd;
    x = g_ctx->menu_x; y = g_ctx->menu_y;
  }
  ClipCursor(nullptr);
  if (visible) {
    while (ShowCursor(TRUE) < 0) {}
    if (menu) {
      SetWindowPos(menu, HWND_TOPMOST, x, y, kMenuWidth, kMenuHeight, SWP_SHOWWINDOW);
      ShowWindow(menu, SW_SHOW);
      focus_window(menu);
      SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)));
      InvalidateRect(menu, nullptr, FALSE);
    }
  } else {
    if (menu) ShowWindow(menu, SW_HIDE);
    if (game && IsWindow(game)) focus_window(game);
  }
  return true;
}
static LRESULT CALLBACK overlay_wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_ERASEBKGND) return 1;
  if (msg == WM_PAINT) {
    PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
    RECT rc{}; GetClientRect(h, &rc);
    HBRUSH black = CreateSolidBrush(RGB(0, 0, 0)); FillRect(dc, &rc, black); DeleteObject(black);
    SetBkMode(dc, TRANSPARENT);
    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (g_ctx) {
      if (!g_ctx->esp_ready) {
        SetTextColor(dc, RGB(205, 165, 255));
        const char* wait = "NZL Studio - waiting for ROB RUN match (INSERT: menu)";
        TextOutA(dc, 12, 12, wait, (int)std::strlen(wait));
      } else {
        HPEN pen = CreatePen(PS_SOLID, 2, RGB(216, 160, 255));
        HPEN thin = CreatePen(PS_SOLID, 1, RGB(177, 106, 244));
        HFONT font = CreateFontA(14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");
        HGDIOBJ oldpen = SelectObject(dc, pen); HGDIOBJ oldfont = SelectObject(dc, font);
        SetTextColor(dc, RGB(190, 145, 255));
        TextOutA(dc, 12, 12, g_ctx->status.c_str(), (int)g_ctx->status.size());
        for (auto& e : g_ctx->render) {
          float hgt = std::fabs(e.fy - e.hy); if (hgt < 2) continue;
          float wid = hgt * 0.45f;
          COLORREF col = e.dead ? RGB(120, 120, 120) : RGB(216, 160, 255);
          if (g_ctx->show_roles && !e.dead && e.role >= 0 && e.role <= 4) {
            const COLORREF roleColors[] = {RGB(200, 200, 200), RGB(255, 80, 80), RGB(90, 170, 255), RGB(120, 255, 140), RGB(255, 180, 80)};
            col = roleColors[e.role];
          }
          SetTextColor(dc, col);
          if (g_ctx->tracers) {
            SelectObject(dc, thin);
            MoveToEx(dc, g_ctx->width / 2, g_ctx->height, nullptr);
            LineTo(dc, (int)e.fx, (int)e.fy);
          }
          if (g_ctx->show_boxes) {
            SelectObject(dc, pen);
            HGDIOBJ oldbrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
            Rectangle(dc, (int)(e.hx - wid * 0.5f), (int)e.hy, (int)(e.hx + wid * 0.5f), (int)e.fy);
            SelectObject(dc, oldbrush);
          }
          if (g_ctx->show_skeleton) {
            SelectObject(dc, thin);
            float cx = e.hx, top = e.hy, bot = e.fy;
            float neck = top + hgt * 0.16f, chest = top + hgt * 0.38f, pelvis = top + hgt * 0.58f, knee = top + hgt * 0.79f;
            MoveToEx(dc, (int)cx, (int)neck, nullptr); LineTo(dc, (int)cx, (int)pelvis);
            MoveToEx(dc, (int)(cx - wid * 0.55f), (int)(chest + hgt * 0.10f), nullptr); LineTo(dc, (int)cx, (int)chest); LineTo(dc, (int)(cx + wid * 0.55f), (int)(chest + hgt * 0.10f));
            MoveToEx(dc, (int)cx, (int)pelvis, nullptr); LineTo(dc, (int)(cx - wid * 0.30f), (int)knee); LineTo(dc, (int)(cx - wid * 0.32f), (int)bot);
            MoveToEx(dc, (int)cx, (int)pelvis, nullptr); LineTo(dc, (int)(cx + wid * 0.30f), (int)knee); LineTo(dc, (int)(cx + wid * 0.32f), (int)bot);
            Ellipse(dc, (int)(cx - wid * 0.16f), (int)top, (int)(cx + wid * 0.16f), (int)neck);
          }
          int ty = (int)e.hy - 15;
          if (g_ctx->show_text) {
            char text[128];
            const char* role = (g_ctx->show_roles && e.role >= 0 && e.role <= 4) ? kRoleNames[e.role] : "";
            std::snprintf(text, sizeof(text), "#%d %.0fm%s%s%s", e.id, e.dist, role[0] ? " " : "", role,
                          e.armor && g_ctx->show_armor ? " [ARM]" : "");
            TextOutA(dc, (int)(e.hx - wid * 0.5f), ty, text, (int)std::strlen(text));
            ty -= 13;
          }
          if (g_ctx->show_hp && e.hp >= 0 && e.maxhp > 0) {
            float frac = std::clamp((float)e.hp / (float)e.maxhp, 0.0f, 1.0f);
            RECT bar{(int)(e.hx - wid * 0.5f), ty - 3, (int)(e.hx + wid * 0.5f), ty + 2};
            HBRUSH bgb = CreateSolidBrush(RGB(40, 40, 40)); FillRect(dc, &bar, bgb); DeleteObject(bgb);
            RECT fill = bar; fill.right = bar.left + (int)((bar.right - bar.left) * frac);
            HBRUSH fgb = CreateSolidBrush(frac > 0.5f ? RGB(80, 255, 120) : frac > 0.25f ? RGB(255, 200, 60) : RGB(255, 70, 70));
            FillRect(dc, &fill, fgb); DeleteObject(fgb);
          }
        }
        if (g_ctx->show_cars) {
          SelectObject(dc, thin); SetTextColor(dc, RGB(255, 170, 60));
          for (auto& car : g_ctx->cars) if (car.on) {
            Rectangle(dc, (int)car.sx - 6, (int)car.sy - 4, (int)car.sx + 6, (int)car.sy + 4);
            char text[64]; std::snprintf(text, sizeof(text), "car %.0fm", car.dist);
            TextOutA(dc, (int)car.sx + 8, (int)car.sy - 7, text, (int)std::strlen(text));
          }
        }
        if (g_ctx->aim) {
          SelectObject(dc, thin); HGDIOBJ oldbrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
          int cx = g_ctx->width / 2, cy = g_ctx->height / 2, radius = (int)g_ctx->aim_fov;
          Ellipse(dc, cx - radius, cy - radius, cx + radius, cy + radius);
          SelectObject(dc, oldbrush);
        }
        if (g_ctx->show_loot) {
          SelectObject(dc, thin); SetTextColor(dc, RGB(255, 220, 120));
          for (auto& loot : g_ctx->loot) if (loot.on) {
            Rectangle(dc, (int)loot.sx - 4, (int)loot.sy - 4, (int)loot.sx + 4, (int)loot.sy + 4);
            char text[64]; std::snprintf(text, sizeof(text), "loot %.0fm", loot.dist);
            TextOutA(dc, (int)loot.sx + 6, (int)loot.sy - 7, text, (int)std::strlen(text));
          }
        }
        if (g_ctx->show_radar) {
          int radius = 90, cx = g_ctx->width - radius - 16, cy = radius + 16;
          HBRUSH rb = CreateSolidBrush(RGB(24, 0, 36)); HGDIOBJ oldbrush = SelectObject(dc, rb);
          SelectObject(dc, thin); Ellipse(dc, cx - radius, cy - radius, cx + radius, cy + radius);
          SelectObject(dc, oldbrush); DeleteObject(rb);
          float yaw = std::atan2(g_ctx->cam_fwd.x, g_ctx->cam_fwd.z);
          float scale = radius / std::max(g_ctx->radar_range, 1.0f);
          for (auto& e : g_ctx->render) {
            float dx = e.pos.x - g_ctx->radar_cam.x, dz = e.pos.z - g_ctx->radar_cam.z;
            float rx = dx * std::cos(yaw) - dz * std::sin(yaw);
            float rz = dx * std::sin(yaw) + dz * std::cos(yaw);
            float px = cx + rx * scale, py = cy - rz * scale;
            float ox = px - cx, oy = py - cy;
            if (ox * ox + oy * oy > (radius - 4) * (radius - 4)) continue;
            HBRUSH dot = CreateSolidBrush(e.dead ? RGB(130, 130, 130) : RGB(255, 120, 255));
            RECT dr{(int)px - 3, (int)py - 3, (int)px + 3, (int)py + 3}; FillRect(dc, &dr, dot); DeleteObject(dot);
          }
          HBRUSH me = CreateSolidBrush(RGB(255, 255, 255)); RECT mr{cx - 3, cy - 3, cx + 3, cy + 3};
          FillRect(dc, &mr, me); DeleteObject(me);
        }
        SelectObject(dc, oldfont); SelectObject(dc, oldpen); DeleteObject(pen); DeleteObject(thin); DeleteObject(font);
      }
    }
    EndPaint(h, &ps); return 0;
  }
  return DefWindowProcW(h, msg, wp, lp);
}
static LRESULT CALLBACK menu_wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_ERASEBKGND) return 1;
  if (msg == WM_NCHITTEST) {
    POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}; ScreenToClient(h, &p);
    if (p.y >= 0 && p.y < 42 && p.x >= 0 && p.x < kMenuWidth - 48) return HTCAPTION;
    return HTCLIENT;
  }
  if (msg == WM_PAINT) {
    PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); RECT rc{}; GetClientRect(h, &rc);
    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (g_ctx) draw_menu(dc, rc, *g_ctx);
    EndPaint(h, &ps); return 0;
  }
  if (msg == WM_LBUTTONDOWN) {
    int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
    if (y < 42 && x >= kMenuWidth - 48) { set_menu_visible(false); return 0; }
    bool changed = false;
    {
      std::lock_guard<std::mutex> lock(g_state_mutex);
      if (!g_ctx) return 0;
      for (int i = 0; i < kTabCount; ++i) {
        RECT r{10, 96 + i * 54, kSidebarWidth - 10, 140 + i * 54};
        if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) {
          g_ctx->menu_tab = i; changed = true; break;
        }
      }
      if (!changed && x >= kSidebarWidth) {
        size_t count = 0; const MenuOption* items = page_options(g_ctx->menu_tab, count);
        for (size_t i = 0; i < count; ++i) {
          RECT r = option_rect(i);
          if (x < r.left || x >= r.right || y < r.top || y >= r.bottom) continue;
          if (items[i].kind == OptionKind::Slider) {
            g_drag_option = (int)i; g_option_rect_drag = r; g_drag_modified = true;
            set_slider_from_x(*g_ctx, items[i], x, r.left);
            SetCapture(h);
          } else {
            activate_option(*g_ctx, items[i].id);
            save_settings(*g_ctx);
          }
          changed = true; break;
        }
      }
    }
    if (changed) { InvalidateRect(h, nullptr, FALSE); if (g_ctx->overlay_wnd) InvalidateRect(g_ctx->overlay_wnd, nullptr, FALSE); }
    return 0;
  }
  if (msg == WM_MOUSEMOVE && g_drag_option >= 0 && (wp & MK_LBUTTON)) {
    int x = GET_X_LPARAM(lp); bool changed = false;
    {
      std::lock_guard<std::mutex> lock(g_state_mutex);
      if (g_ctx) {
        size_t count = 0; const MenuOption* items = page_options(g_ctx->menu_tab, count);
        if ((size_t)g_drag_option < count && items[g_drag_option].kind == OptionKind::Slider) {
          set_slider_from_x(*g_ctx, items[g_drag_option], x, g_option_rect_drag.left); changed = true;
        }
      }
    }
    if (changed) { InvalidateRect(h, nullptr, FALSE); if (g_ctx->overlay_wnd) InvalidateRect(g_ctx->overlay_wnd, nullptr, FALSE); }
    return 0;
  }
  if (msg == WM_LBUTTONUP) {
    if (g_drag_option >= 0) {
      g_drag_option = -1; ReleaseCapture();
      { std::lock_guard<std::mutex> lock(g_state_mutex); if (g_ctx && g_drag_modified) save_settings(*g_ctx); }
      g_drag_modified = false; return 0;
    }
  }
  if (msg == WM_MOUSEMOVE) SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)));
  if (msg == WM_KEYDOWN && wp == VK_ESCAPE) { set_menu_visible(false); return 0; }
  if (msg == WM_CLOSE) { set_menu_visible(false); return 0; }
  if (msg == WM_MOVE && g_ctx) {
    RECT r{}; GetWindowRect(h, &r);
    std::lock_guard<std::mutex> lock(g_state_mutex); g_ctx->menu_x = r.left; g_ctx->menu_y = r.top;
  }
  return DefWindowProcW(h, msg, wp, lp);
}
static HWND enum_candidate = nullptr;
static long enum_best_area = 0;
static BOOL CALLBACK enum_game_window(HWND h, LPARAM lp) {
  DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
  if (pid != (DWORD)lp || !IsWindowVisible(h)) return TRUE;
  RECT r{}; if (!GetClientRect(h, &r)) return TRUE;
  long area = (long)(r.right - r.left) * (long)(r.bottom - r.top);
  if (area > enum_best_area) { enum_best_area = area; enum_candidate = h; }
  return TRUE;
}
static HWND find_game_window(DWORD pid) {
  enum_candidate = nullptr; enum_best_area = 0;
  EnumWindows(enum_game_window, (LPARAM)pid);
  return enum_candidate;
}
static void set_runtime(Ctx* ui, DWORD pid, bool ready, const std::string& status, bool clear_data) {
  HWND overlay = nullptr, menu = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    ui->attached_pid = pid; ui->esp_ready = ready; ui->status = status;
    if (clear_data) { ui->render.clear(); ui->loot.clear(); ui->cars.clear(); }
    overlay = ui->overlay_wnd; menu = ui->menu_wnd;
  }
  if (overlay) InvalidateRect(overlay, nullptr, FALSE);
  if (menu) InvalidateRect(menu, nullptr, FALSE);
}
static bool read_classes(nzl::DriverMemory& mem, uint64_t ga, uint64_t klass[6], const char*& failed) {
  struct Slot { const char* name; uint64_t rva; };
  static const Slot slots[] = {
    {"Game", 0x81B7710ULL}, {"GameModel", 0x81B86D8ULL}, {"BattleModel", 0x82496E8ULL},
    {"BattleEntitiesModel", 0x8245928ULL}, {"WorldEntitiesModel", 0x82295C8ULL}, {"BattleCameraModel", 0x823C210ULL}
  };
  for (int i = 0; i < 6; ++i) {
    auto k = mem.ptr(ga + slots[i].rva);
    klass[i] = k ? *k : 0;
    if (!klass[i] || !klass_name_is(mem, klass[i], slots[i].name)) { failed = slots[i].name; return false; }
  }
  failed = nullptr; return true;
}
static uint64_t find_live_game(nzl::DriverMemory& mem, DWORD pid, const uint64_t klass[6], const nzl::Offsets& off, Ctx* ui) {
  auto obj_klass = [&](uint64_t o) -> uint64_t { auto k = o ? mem.ptr(o) : std::nullopt; return k ? *k : 0; };
  uint64_t start = 0x10000, cands[256];
  while (start && !g_stop.load()) {
    uint64_t next = 0;
    uint32_t found = NaScan(pid, klass[0], start, cands, 256, &next);
    for (uint32_t i = 0; i < found && !g_stop.load(); ++i) {
      uint64_t cand = cands[i];
      auto gm = mem.ptr(cand + off.game_model);
      if (!gm || obj_klass(*gm) != klass[1]) {
        gm = mem.ptr(cand + off.game_model_alt);
        if (!gm || obj_klass(*gm) != klass[1]) continue;
      }
      auto bm = mem.ptr(*gm + off.game_battle);
      if (!bm || obj_klass(*bm) != klass[2]) continue;
      auto be = mem.ptr(*bm + off.battle_entities);
      if (!be || obj_klass(*be) != klass[3]) continue;
      return cand;
    }
    char status[128]; std::snprintf(status, sizeof(status), "Waiting for battle object... scan 0x%llX", (unsigned long long)next);
    set_runtime(ui, pid, false, status, false);
    if (!next) break;
    start = next;
  }
  return 0;
}
static void esp_worker(Ctx* ui) {
  DWORD active_pid = 0;
  std::unique_ptr<nzl::DriverMemory> mem;
  bool mem_ready = false;
  uint64_t ga = 0, klass[6] = {}, game_obj = 0;
  const char* failed_class = nullptr;
  nzl::Offsets off;
  nzl::ChainResult chain;
  while (!g_stop.load()) {
    DWORD pid = find_pid(L"ROB RUN.exe");
    if (!pid) {
      if (mem) { mem->close(); mem.reset(); }
      active_pid = 0; mem_ready = false; ga = 0; game_obj = 0; chain = {};
      set_runtime(ui, 0, false, "Waiting for ROB RUN process", true);
      Sleep(500); continue;
    }
    if (pid != active_pid) {
      if (mem) { mem->close(); mem.reset(); }
      active_pid = pid; mem_ready = false; ga = 0; game_obj = 0; chain = {};
      std::fill(std::begin(klass), std::end(klass), 0);
      mem = std::make_unique<nzl::DriverMemory>(pid);
      set_runtime(ui, pid, false, "ROB RUN found - connecting to driver", true);
    }
    if (!mem_ready) {
      if (!mem || !mem->init()) {
        set_runtime(ui, pid, false, "Driver not ready - waiting", true);
        Sleep(1200); continue;
      }
      mem_ready = true;
      set_runtime(ui, pid, false, "Driver connected - waiting for GameAssembly", true);
    }
    if (!ga) {
      ga = find_gameassembly(pid);
      if (!ga) {
        set_runtime(ui, pid, false, "Waiting for GameAssembly to finish loading", true);
        Sleep(1000); continue;
      }
    }
    if (!read_classes(*mem, ga, klass, failed_class)) {
      std::string s = "Waiting for game initialization: "; s += failed_class ? failed_class : "class";
      set_runtime(ui, pid, false, s, true);
      Sleep(1000); continue;
    }
    if (!game_obj) {
      set_runtime(ui, pid, false, "Game loaded - waiting for an active match", true);
      game_obj = find_live_game(*mem, pid, klass, off, ui);
      if (!game_obj) {
        if (find_pid(L"ROB RUN.exe") != active_pid) continue;
        set_runtime(ui, pid, false, "Waiting for match; menu stays available", true);
        Sleep(2500); continue;
      }
    }
    chain = nzl::resolve_chain_from_game(*mem, game_obj, off);
    if (!chain.ok) {
      game_obj = 0;
      set_runtime(ui, pid, false, "Match is loading - waiting for battle chain", true);
      Sleep(1000); continue;
    }
    Ctx work; work.pid = pid; work.mem = mem.get(); work.off = off; work.chain = chain;
    set_runtime(ui, pid, true, "ESP connected - syncing players", false);
    ULONGLONG last_chain_check = 0, last_pid_check = 0;
    bool leave_match = false;
    while (!g_stop.load() && !leave_match) {
      ULONGLONG now = GetTickCount64();
      if (now - last_pid_check > 1000) {
        last_pid_check = now;
        if (find_pid(L"ROB RUN.exe") != active_pid) { leave_match = true; break; }
      }
      {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        copy_config(work, *ui);
      }
      if (now - last_chain_check > 1500) {
        last_chain_check = now;
        nzl::ChainResult fresh = nzl::resolve_chain_from_game(*mem, game_obj, off);
        if (!fresh.ok) {
          game_obj = 0; chain = {}; leave_match = true;
          set_runtime(ui, pid, false, "Match changed - waiting to reconnect", true);
          break;
        }
        chain = fresh; work.chain = fresh;
      }
      auto rd3 = [&](uint64_t a, nzl::Vec3f& v) {
        float f[3] = {};
        if (!mem->read(a, f, sizeof(f))) return false;
        v = {f[0], f[1], f[2]};
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
      };
      nzl::Vec3f cpos{}, cfwd{}, cright{}, cup{};
      if (!rd3(chain.camera_model + off.cam_pos, cpos)) { Sleep(100); continue; }
      bool basis_ok = work.basis_quat && basis_from_quat(*mem, chain.camera_model + 0x2C, cfwd, cright, cup);
      if (!basis_ok) {
        rd3(chain.camera_model + off.cam_fwd, cfwd);
        rd3(chain.camera_model + off.cam_right, cright);
        rd3(chain.camera_model + off.cam_up, cup);
        cfwd = normalize(cfwd); cright = normalize(cright); cup = normalize(cup);
      }
      work.cam_fwd = cfwd; work.cam_right = cright; work.radar_cam = cpos;
      auto players = snapshot_players(work, cpos);
      nzl::CameraView view;
      view.pos = cpos; view.fwd = cfwd; view.right = cright; view.up = cup;
      view.width = std::max(work.width, 1); view.height = std::max(work.height, 1); view.fov_deg = work.fov;
      work.render.clear();
      const float bone_height[] = {1.75f, 1.30f, 0.95f};
      int bone = std::clamp(work.aim_bone, 0, 2);
      for (auto& p : players) {
        if (p.dead && !work.show_dead) continue;
        if (p.dist > work.max_dist) continue;
        float fx, fy, fd, hx, hy, hd, ax, ay, ad;
        nzl::Vec3f head{p.pos.x, p.pos.y + 1.8f, p.pos.z};
        nzl::Vec3f aim_point{p.pos.x, p.pos.y + bone_height[bone], p.pos.z};
        if (!nzl::world_to_screen(view, p.pos, fx, fy, fd) || !nzl::world_to_screen(view, head, hx, hy, hd) ||
            !nzl::world_to_screen(view, aim_point, ax, ay, ad)) continue;
        if (work.visible_only && (hx < 0 || hx > view.width || hy < 0 || hy > view.height)) continue;
        RenderEnt e; e.hx = hx; e.hy = hy; e.fx = fx; e.fy = fy; e.ax = ax; e.ay = ay;
        e.pos = p.pos; e.id = p.id; e.dist = p.dist; e.dead = p.dead;
        e.hp = p.hp; e.maxhp = p.maxhp; e.armor = p.armor; e.role = p.role;
        work.render.push_back(e);
      }
      if (work.show_cars) {
        work.cars = snapshot_cars(work, cpos);
        for (auto& car : work.cars) { float depth = 0; car.on = car.dist <= work.max_dist && nzl::world_to_screen(view, car.pos, car.sx, car.sy, depth); }
      } else work.cars.clear();
      if (work.show_loot) {
        work.loot = snapshot_loot(work, cpos);
        for (auto& loot : work.loot) { float depth = 0; loot.on = loot.dist <= work.max_dist && nzl::world_to_screen(view, loot.pos, loot.sx, loot.sy, depth); }
      } else work.loot.clear();
      int aim_key = kAimKeys[std::clamp(work.aim_key, 0, 3)];
      HWND foreground = GetForegroundWindow(); DWORD foreground_pid = 0;
      if (foreground) GetWindowThreadProcessId(foreground, &foreground_pid);
      if (work.aim && !work.menu && foreground_pid == pid && (GetAsyncKeyState(aim_key) & 0x8000)) {
        float best = work.aim_fov; const RenderEnt* target = nullptr;
        float cx = view.width * 0.5f, cy = view.height * 0.5f;
        for (auto& e : work.render) {
          if (e.dead) continue;
          float dx = e.ax - cx, dy = e.ay - cy, d = std::sqrt(dx * dx + dy * dy);
          if (d < best) { best = d; target = &e; }
        }
        if (target) {
          INPUT in{}; in.type = INPUT_MOUSE; in.mi.dwFlags = MOUSEEVENTF_MOVE;
          in.mi.dx = (LONG)((target->ax - cx) / std::max(work.aim_smooth, 1.0f));
          in.mi.dy = (LONG)((target->ay - cy) / std::max(work.aim_smooth, 1.0f));
          if (in.mi.dx || in.mi.dy) SendInput(1, &in, sizeof(INPUT));
        }
      }
      char status[128];
      std::snprintf(status, sizeof(status), "ESP active  |  players: %zu  |  projected: %zu", players.size(), work.render.size());
      {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        ui->render = work.render; ui->loot = work.loot; ui->cars = work.cars;
        ui->cam_fwd = work.cam_fwd; ui->cam_right = work.cam_right; ui->radar_cam = work.radar_cam;
        ui->attached_pid = pid; ui->esp_ready = true; ui->status = status;
      }
      if (ui->overlay_wnd) InvalidateRect(ui->overlay_wnd, nullptr, FALSE);
      Sleep(90);
    }
  }
  if (mem) mem->close();
}
int main() {
  std::printf("[NZL Studio] persistent menu + background ESP reconnect. INSERT=menu, END=exit.\n");
  Ctx ctx; load_settings(ctx); g_ctx = &ctx;
  HINSTANCE instance = GetModuleHandleW(nullptr);
  WNDCLASSW overlayClass{}; overlayClass.lpfnWndProc = overlay_wnd_proc; overlayClass.hInstance = instance; overlayClass.lpszClassName = L"NZLStudioEspOverlay";
  overlayClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); RegisterClassW(&overlayClass);
  WNDCLASSW menuClass{}; menuClass.lpfnWndProc = menu_wnd_proc; menuClass.hInstance = instance; menuClass.lpszClassName = L"NZLStudioInteractiveMenu";
  menuClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); RegisterClassW(&menuClass);
  ctx.overlay_wnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
    overlayClass.lpszClassName, L"NZL ESP", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance, nullptr);
  ctx.menu_wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
    menuClass.lpszClassName, L"NZL Studio", WS_POPUP, ctx.menu_x, ctx.menu_y, kMenuWidth, kMenuHeight, nullptr, nullptr, instance, nullptr);
  if (!ctx.overlay_wnd || !ctx.menu_wnd) { std::printf("[NZL] UI window creation failed.\n"); return 10; }
  SetLayeredWindowAttributes(ctx.overlay_wnd, RGB(0, 0, 0), 0, LWA_COLORKEY);
  ShowWindow(ctx.overlay_wnd, SW_HIDE); ShowWindow(ctx.menu_wnd, SW_HIDE);
  std::thread worker(esp_worker, &ctx);
  ULONGLONG last_window_check = 0, last_paint = 0;
  DWORD window_pid = 0;
  bool done = false;
  MSG msg{};
  while (!done && !g_stop.load()) {
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) { done = true; break; }
      TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    int menu_key = VK_INSERT;
    bool menu_open = false;
    { std::lock_guard<std::mutex> lock(g_state_mutex); menu_key = kMenuKeys[std::clamp(ctx.menu_key, 0, 3)]; menu_open = ctx.menu; }
    if (GetAsyncKeyState(menu_key) & 1) set_menu_visible(!menu_open);
    if (GetAsyncKeyState(VK_END) & 1) { done = true; break; }
    ULONGLONG now = GetTickCount64();
    if (now - last_window_check >= 500) {
      last_window_check = now;
      DWORD p = find_pid(L"ROB RUN.exe");
      HWND w = p ? find_game_window(p) : nullptr;
      if (p != window_pid || w != g_game_wnd) { window_pid = p; g_game_wnd = w; }
    }
    if (g_game_wnd && IsWindow(g_game_wnd)) {
      RECT cr{}; GetClientRect(g_game_wnd, &cr); POINT pt{0, 0}; ClientToScreen(g_game_wnd, &pt);
      int width = std::max<int>((int)(cr.right - cr.left), 1), height = std::max<int>((int)(cr.bottom - cr.top), 1);
      { std::lock_guard<std::mutex> lock(g_state_mutex); ctx.width = width; ctx.height = height; }
      SetWindowPos(ctx.overlay_wnd, HWND_TOPMOST, pt.x, pt.y, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
      ShowWindow(ctx.overlay_wnd, SW_SHOWNOACTIVATE);
    } else ShowWindow(ctx.overlay_wnd, SW_HIDE);
    { std::lock_guard<std::mutex> lock(g_state_mutex); menu_open = ctx.menu; }
    if (menu_open) SetWindowPos(ctx.menu_wnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    if (now - last_paint >= 50) {
      last_paint = now;
      InvalidateRect(ctx.overlay_wnd, nullptr, FALSE);
      if (menu_open) InvalidateRect(ctx.menu_wnd, nullptr, FALSE);
    }
    Sleep(8);
  }
  g_stop.store(true);
  ClipCursor(nullptr);
  if (worker.joinable()) worker.join();
  if (ctx.menu_wnd && IsWindow(ctx.menu_wnd)) DestroyWindow(ctx.menu_wnd);
  if (ctx.overlay_wnd && IsWindow(ctx.overlay_wnd)) DestroyWindow(ctx.overlay_wnd);
  std::printf("[NZL] Studio closed.\n");
  return 0;
}
