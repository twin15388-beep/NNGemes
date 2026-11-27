// NZL Studio overlay (Windows) — purple ESP + aim + radar over the game.
// Data path validated live (2026-11-27): Sharedmem Driver -> GameAssembly
// region scan -> TypeInfo klass name checks -> NA_CMD_SCAN Game search ->
// chain -> batched dictionary snapshots -> projection -> GDI layered
// click-through overlay tracking the game client rect.
// INSERT = settings menu (UP/DOWN select, LEFT/RIGHT change), END = exit.
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
#include <windows.h>
#include <tlhelp32.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
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
  nzl::Vec3f cam_fwd{0, 0, 1}, cam_right{1, 0, 0};
  float fov = 90.0f;
  int width = 1920, height = 1080;
  // Menu (INSERT). Order must match menu lines in wnd_proc.
  bool menu = false; int menu_sel = 0;
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
  for (int i = 0; i < n; i++) {
    nzl::Vec3f p = pos[i];
    if ((p.x == 0 && p.y == 0 && p.z == 0) || !std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) continue;
    nzl::PlayerSnap s; s.addr = chars[i]; s.id = ids[i]; s.dead = dead[i] != 0; s.pos = p;
    float dx = p.x - local.x, dy = p.y - local.y, dz = p.z - local.z;
    s.dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (c.show_hp || c.show_armor) {
      // Dependent pointer chains - direct reads, only when enabled.
      auto priv = c.mem->ptr(chars[i] + 0x10);
      if (c.show_hp && priv) {
        auto hm = c.mem->ptr(*priv + 0x1B0);
        if (hm) {
          auto data = c.mem->ptr(*hm + 0x10);
          auto mx = c.mem->i32(*hm + 0x20);
          if (data) { auto hp = c.mem->i32(*data + 0x18); if (hp && *hp >= 0 && *hp <= 100000) s.hp = *hp; }
          if (mx && *mx > 0 && *mx <= 100000) s.maxhp = *mx;
        }
      }
      if (c.show_armor) {
        auto prim = c.mem->ptr(chars[i] + 0x1B8);
        if (prim) { auto data = c.mem->ptr(*prim + 0x10); if (data) { auto v = c.mem->u8(*data + 0x18); if (v) s.armor = *v != 0; } }
      }
    }
    if (c.show_roles) {
      // Role: char+0x208 IPrimitiveModel<PlayerRoleDescription> ->
      // data+0x10 -> description ptr @+0x18 -> EnumItem.Index @desc+0x18.
      auto prim = c.mem->ptr(chars[i] + 0x208);
      if (prim) {
        auto data = c.mem->ptr(*prim + 0x10);
        if (data) {
          auto desc = c.mem->ptr(*data + 0x18);
          if (desc) { auto idx = c.mem->i32(*desc + 0x18); if (idx && *idx >= 0 && *idx <= 4) s.role = *idx; }
        }
      }
    }
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
  std::vector<nzl::Vec3f> pos(n, {});
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
static HWND g_game_wnd = nullptr;
static BOOL CALLBACK enum_wnd(HWND h, LPARAM lp) {
  DWORD wpid = 0; GetWindowThreadProcessId(h, &wpid);
  if (wpid != (DWORD)lp || !IsWindowVisible(h)) return TRUE;
  RECT r{}; if (!GetClientRect(h, &r)) return TRUE;
  static long best = 0;
  long area = (r.right - r.left) * (r.bottom - r.top);
  if (area > best) { best = area; g_game_wnd = h; }
  return TRUE;
}
static Ctx* g_ctx = nullptr;
static const char* kMenuNames[] = {
  "FOV", "Max distance", "Show dead", "Boxes", "Skeleton", "Labels",
  "HP bar*", "Armor mark*", "Roles*", "Loot ESP", "Cars ESP", "Radar",
  "Radar range", "Tracers", "Aim", "Aim key", "Aim bone", "Aim FOV px",
  "Aim smooth", "Basis: LookAt quat", "Visible only*", "Menu key"
};
static constexpr int kMenuCount = 22;
static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_PAINT) {
    PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
    RECT rc; GetClientRect(h, &rc);
    HBRUSH black = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(dc, &rc, black); DeleteObject(black);
    if (g_ctx) {
      HPEN pen = CreatePen(PS_SOLID, 2, RGB(191, 64, 255));
      HPEN thin = CreatePen(PS_SOLID, 1, RGB(160, 32, 240));
      HGDIOBJ oldpen = SelectObject(dc, pen);
      SetBkMode(dc, TRANSPARENT);
      HFONT font = CreateFontW(14, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, 0, 0, 0, 0, 0, L"Segoe UI");
      HGDIOBJ oldfont = SelectObject(dc, font);
      for (auto& e : g_ctx->render) {
        float hgt = std::fabs(e.fy - e.hy); if (hgt < 2) continue;
        float wid = hgt * 0.45f;
        COLORREF col = e.dead ? RGB(120, 120, 120) : RGB(216, 160, 255);
        if (g_ctx->show_roles && !e.dead && e.role >= 0 && e.role <= 4) {
          const COLORREF rc[] = {RGB(200, 200, 200), RGB(255, 80, 80), RGB(90, 170, 255), RGB(120, 255, 140), RGB(255, 180, 80)};
          col = rc[e.role];
        }
        SetTextColor(dc, col);
        if (g_ctx->tracers) {
          SelectObject(dc, thin);
          MoveToEx(dc, g_ctx->width / 2, g_ctx->height, nullptr);
          LineTo(dc, (int)e.fx, (int)e.fy);
        }
        if (g_ctx->show_boxes && !g_ctx->show_skeleton) {
          SelectObject(dc, pen);
          HGDIOBJ oldb = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
          Rectangle(dc, (int)(e.hx - wid * 0.5f), (int)e.hy, (int)(e.hx + wid * 0.5f), (int)e.fy);
          SelectObject(dc, oldb);
        }
        if (g_ctx->show_skeleton) {
          // Screen-space stick skeleton from the box (bone-accurate
          // skeleton needs SkeletonModel bone data - not wired yet).
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
          char t[128];
          const char* rn = (g_ctx->show_roles && e.role >= 0 && e.role <= 4) ? kRoleNames[e.role] : "";
          std::snprintf(t, sizeof(t), "#%d %.0fm%s%s%s", e.id, e.dist,
                        rn[0] ? " " : "", rn, e.armor && g_ctx->show_armor ? " [ARM]" : "");
          TextOutA(dc, (int)(e.hx - wid * 0.5f), ty, t, (int)std::strlen(t));
          ty -= 13;
        }
        if (g_ctx->show_hp && e.hp >= 0 && e.maxhp > 0) {
          float frac = (float)e.hp / (float)e.maxhp; if (frac > 1) frac = 1;
          HBRUSH bg = CreateSolidBrush(RGB(40, 40, 40));
          RECT br{(int)(e.hx - wid * 0.5f), ty - 3, (int)(e.hx + wid * 0.5f), ty + 2};
          FillRect(dc, &br, bg); DeleteObject(bg);
          HBRUSH fg = CreateSolidBrush(frac > 0.5f ? RGB(80, 255, 120) : frac > 0.25f ? RGB(255, 200, 60) : RGB(255, 70, 70));
          RECT fr = br; fr.right = br.left + (int)((br.right - br.left) * frac);
          FillRect(dc, &fr, fg); DeleteObject(fg);
        }
      }
      if (g_ctx->show_cars) {
        SelectObject(dc, thin);
        SetTextColor(dc, RGB(255, 170, 60));
        for (auto& l : g_ctx->cars) {
          if (!l.on) continue;
          Rectangle(dc, (int)l.sx - 6, (int)l.sy - 4, (int)l.sx + 6, (int)l.sy + 4);
          char t[64]; std::snprintf(t, sizeof(t), "car %.0fm", l.dist);
          TextOutA(dc, (int)l.sx + 8, (int)l.sy - 7, t, (int)std::strlen(t));
        }
      }
      if (g_ctx->show_loot) {
        SelectObject(dc, thin);
        SetTextColor(dc, RGB(255, 220, 120));
        for (auto& l : g_ctx->loot) {
          if (!l.on) continue;
          Rectangle(dc, (int)l.sx - 4, (int)l.sy - 4, (int)l.sx + 4, (int)l.sy + 4);
          char t[64]; std::snprintf(t, sizeof(t), "loot %.0fm", l.dist);
          TextOutA(dc, (int)l.sx + 6, (int)l.sy - 7, t, (int)std::strlen(t));
        }
      }
      if (g_ctx->show_radar) {
        int R = 90, cxr = g_ctx->width - R - 16, cyr = R + 16;
        HBRUSH bg = CreateSolidBrush(RGB(24, 0, 36));
        HGDIOBJ oldb = SelectObject(dc, bg);
        SelectObject(dc, thin);
        Ellipse(dc, cxr - R, cyr - R, cxr + R, cyr + R);
        SelectObject(dc, oldb); DeleteObject(bg);
        MoveToEx(dc, cxr, cyr, nullptr); LineTo(dc, cxr, cyr - R);
        nzl::Vec3f f = g_ctx->cam_fwd, r = g_ctx->cam_right;
        for (auto& e : g_ctx->render) {
          float dx = e.pos.x - 0.0f, dz = e.pos.z - 0.0f; // relative added below via static cam pos
          (void)dx; (void)dz;
        }
        // Radar dots use positions relative to camera stored in render
        // order with cam pos subtracted at tick time (see main loop:
        // render positions are world; cam pos kept in g_radar_cam).
        extern nzl::Vec3f g_radar_cam;
        float yaw = std::atan2(f.x, f.z);
        (void)r;
        for (auto& e : g_ctx->render) {
          float dx = e.pos.x - g_radar_cam.x, dz = e.pos.z - g_radar_cam.z;
          float rx = dx * std::cos(yaw) - dz * std::sin(yaw);
          float rz = dx * std::sin(yaw) + dz * std::cos(yaw);
          float scale = R / g_ctx->radar_range;
          float px = cxr + rx * scale, py = cyr - rz * scale;
          float ddx = px - cxr, ddy = py - cyr;
          if (ddx * ddx + ddy * ddy > (R - 4) * (R - 4)) continue;
          HBRUSH dot = CreateSolidBrush(e.dead ? RGB(130, 130, 130) : RGB(255, 120, 255));
          RECT dr{(int)px - 3, (int)py - 3, (int)px + 3, (int)py + 3};
          FillRect(dc, &dr, dot); DeleteObject(dot);
        }
        HBRUSH me = CreateSolidBrush(RGB(255, 255, 255));
        RECT mr{cxr - 3, cyr - 3, cxr + 3, cyr + 3};
        FillRect(dc, &mr, me); DeleteObject(me);
      }
      if (g_ctx->menu) {
        char vals[kMenuCount][64];
        std::snprintf(vals[0], 64, "%.0f", g_ctx->fov);
        std::snprintf(vals[1], 64, "%.0f m", g_ctx->max_dist);
        std::snprintf(vals[2], 64, "%s", g_ctx->show_dead ? "ON" : "OFF");
        std::snprintf(vals[3], 64, "%s", g_ctx->show_boxes ? "ON" : "OFF");
        std::snprintf(vals[4], 64, "%s", g_ctx->show_skeleton ? "ON" : "OFF");
        std::snprintf(vals[5], 64, "%s", g_ctx->show_text ? "ON" : "OFF");
        std::snprintf(vals[6], 64, "%s", g_ctx->show_hp ? "ON" : "OFF");
        std::snprintf(vals[7], 64, "%s", g_ctx->show_armor ? "ON" : "OFF");
        std::snprintf(vals[8], 64, "%s", g_ctx->show_roles ? "ON" : "OFF");
        std::snprintf(vals[9], 64, "%s", g_ctx->show_loot ? "ON" : "OFF");
        std::snprintf(vals[10], 64, "%s", g_ctx->show_cars ? "ON" : "OFF");
        std::snprintf(vals[11], 64, "%s", g_ctx->show_radar ? "ON" : "OFF");
        std::snprintf(vals[12], 64, "%.0f m", g_ctx->radar_range);
        std::snprintf(vals[13], 64, "%s", g_ctx->tracers ? "ON" : "OFF");
        std::snprintf(vals[14], 64, "%s", g_ctx->aim ? "ON" : "OFF");
        std::snprintf(vals[15], 64, "%s", kAimKeyNames[g_ctx->aim_key]);
        std::snprintf(vals[16], 64, "%s", kBoneNames[g_ctx->aim_bone]);
        std::snprintf(vals[17], 64, "%.0f px", g_ctx->aim_fov);
        std::snprintf(vals[18], 64, "%.0f", g_ctx->aim_smooth);
        std::snprintf(vals[19], 64, "%s", g_ctx->basis_quat ? "ON" : "OFF");
        std::snprintf(vals[20], 64, "%s", g_ctx->visible_only ? "ON" : "OFF");
        std::snprintf(vals[21], 64, "%s", kMenuKeyNames[g_ctx->menu_key]);
        HBRUSH panel = CreateSolidBrush(RGB(24, 0, 36));
        RECT pr{10, 10, 330, 10 + 22 * (kMenuCount + 1) + 10};
        FillRect(dc, &pr, panel); DeleteObject(panel);
        HPEN border = CreatePen(PS_SOLID, 1, RGB(191, 64, 255));
        HGDIOBJ ob = SelectObject(dc, border);
        HGDIOBJ obb = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
        Rectangle(dc, pr.left, pr.top, pr.right, pr.bottom);
        SelectObject(dc, ob); SelectObject(dc, obb); DeleteObject(border);
        SetTextColor(dc, RGB(216, 160, 255));
        TextOutA(dc, 20, 18, "NZL Studio  [INSERT close, END exit]", 36);
        for (int i = 0; i < kMenuCount; i++) {
          char line[128];
          std::snprintf(line, sizeof(line), "%-18s %s", kMenuNames[i], vals[i]);
          SetTextColor(dc, i == g_ctx->menu_sel ? RGB(255, 255, 255) : RGB(200, 140, 255));
          TextOutA(dc, 20, 18 + (i + 1) * 22, line, (int)std::strlen(line));
        }
        SetTextColor(dc, RGB(216, 160, 255));
      }
      SelectObject(dc, oldpen); SelectObject(dc, oldfont);
      DeleteObject(pen); DeleteObject(thin); DeleteObject(font);
    }
    EndPaint(h, &ps);
    return 0;
  }
  return DefWindowProcW(h, msg, wp, lp);
}
nzl::Vec3f g_radar_cam{};
int main() {
  std::printf("[NZL Studio overlay] purple ESP. INSERT=menu, END=exit, aim=hold RMB.\n");
  DWORD pid = find_pid(L"ROB RUN.exe");
  if (!pid) { std::printf("[NZL] game not running.\n"); return 2; }
  nzl::DriverMemory mem(pid);
  if (!mem.init()) { std::printf("[NZL] driver NOT loaded (run NZL-LoadDriver.ps1 first).\n"); return 4; }
  std::printf("[NZL] pid=%lu transport OK. Finding GameAssembly...\n", pid);
  uint64_t ga = find_gameassembly(pid);
  if (!ga) { std::printf("[NZL] GameAssembly not found.\n"); return 5; }
  struct Slot { const char* want; uint64_t rva; };
  const Slot slots[] = {{"Game", 0x81B7710ULL}, {"GameModel", 0x81B86D8ULL}, {"BattleModel", 0x82496E8ULL},
                        {"BattleEntitiesModel", 0x8245928ULL}, {"WorldEntitiesModel", 0x82295C8ULL}, {"BattleCameraModel", 0x823C210ULL}};
  uint64_t klass[6] = {};
  for (int i = 0; i < 6; i++) {
    auto k = mem.ptr(ga + slots[i].rva); klass[i] = k ? *k : 0;
    if (!klass[i] || !klass_name_is(mem, klass[i], slots[i].want)) {
      std::printf("[NZL] klass %s validation FAILED. Be in menu/match.\n", slots[i].want); return 6;
    }
  }
  std::printf("[NZL] classes OK. Kernel-side heap scan for Game...\n");
  nzl::Offsets off;
  auto obj_klass = [&](uint64_t o) -> uint64_t { auto k = o ? mem.ptr(o) : std::nullopt; return k ? *k : 0; };
  uint64_t game_obj = 0;
  {
    uint64_t start = 0x10000, cands[256];
    ULONGLONG t0 = GetTickCount64();
    while (start && !game_obj) {
      uint64_t next = 0;
      uint32_t found = NaScan(pid, klass[0], start, cands, 256, &next);
      for (uint32_t i = 0; i < found && !game_obj; i++) {
        uint64_t cand = cands[i];
        auto gm = mem.ptr(cand + off.game_model);
        if (!gm || obj_klass(*gm) != klass[1]) { gm = mem.ptr(cand + off.game_model_alt); if (!gm || obj_klass(*gm) != klass[1]) continue; }
        auto bm = mem.ptr(*gm + off.game_battle);
        if (!bm || obj_klass(*bm) != klass[2]) continue;
        auto be = mem.ptr(*bm + off.battle_entities);
        if (!be || obj_klass(*be) != klass[3]) continue;
        game_obj = cand;
      }
      std::printf("[NZL] scan resume=0x%llx candidates=%u +%llums\r",
                  (unsigned long long)next, found, (unsigned long long)(GetTickCount64() - t0));
      std::fflush(stdout);
      start = next;
    }
    std::printf("\n[NZL] Game=0x%llx in %llums (kernel scan)\n", (unsigned long long)game_obj,
                (unsigned long long)(GetTickCount64() - t0));
  }
  if (!game_obj) { std::printf("[NZL] Game not found. Be in a match.\n"); return 7; }
  Ctx ctx; ctx.pid = pid; ctx.mem = &mem; ctx.off = off;
  ctx.chain = nzl::resolve_chain_from_game(mem, game_obj, off);
  if (!ctx.chain.ok) { std::printf("[NZL] chain FAILED at %s\n", ctx.chain.stage.c_str()); return 8; }
  std::printf("[NZL] chain OK. Overlay live. INSERT opens settings.\n");
  g_ctx = &ctx;
  EnumWindows(enum_wnd, (LPARAM)pid);
  if (!g_game_wnd) { std::printf("[NZL] game window not found.\n"); return 9; }
  WNDCLASSW wc{}; wc.lpfnWndProc = wnd_proc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"NZLStudioOverlay";
  RegisterClassW(&wc);
  HWND ov = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
                            L"NZLStudioOverlay", L"NZL Studio", WS_POPUP, 0, 0, 100, 100,
                            nullptr, nullptr, wc.hInstance, nullptr);
  if (!ov) { std::printf("[NZL] overlay window failed.\n"); return 10; }
  SetLayeredWindowAttributes(ov, RGB(0, 0, 0), 0, LWA_COLORKEY);
  ShowWindow(ov, SW_SHOW);
  ULONGLONG last_snap = 0;
  MSG msg{};
  while (true) {
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) { mem.close(); return 0; }
      TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    if (GetAsyncKeyState(VK_END) & 1) break;
    if (GetAsyncKeyState(kMenuKeys[ctx.menu_key]) & 1) { ctx.menu = !ctx.menu; InvalidateRect(ov, nullptr, FALSE); }
    if (ctx.menu) {
      if (GetAsyncKeyState(VK_UP) & 1) ctx.menu_sel = (ctx.menu_sel + kMenuCount - 1) % kMenuCount;
      if (GetAsyncKeyState(VK_DOWN) & 1) ctx.menu_sel = (ctx.menu_sel + 1) % kMenuCount;
      bool left = (GetAsyncKeyState(VK_LEFT) & 1) != 0;
      bool right = (GetAsyncKeyState(VK_RIGHT) & 1) != 0;
      if (left || right) {
        float d = right ? 1.0f : -1.0f;
        auto adj = [&](float& v, float step, float lo, float hi) { v += d * step; if (v < lo) v = lo; if (v > hi) v = hi; };
        switch (ctx.menu_sel) {
          case 0: adj(ctx.fov, 1, 40, 140); break;
          case 1: adj(ctx.max_dist, 25, 25, 1500); break;
          case 2: ctx.show_dead = !ctx.show_dead; break;
          case 3: ctx.show_boxes = !ctx.show_boxes; break;
          case 4: ctx.show_skeleton = !ctx.show_skeleton; break;
          case 5: ctx.show_text = !ctx.show_text; break;
          case 6: ctx.show_hp = !ctx.show_hp; break;
          case 7: ctx.show_armor = !ctx.show_armor; break;
          case 8: ctx.show_roles = !ctx.show_roles; break;
          case 9: ctx.show_loot = !ctx.show_loot; break;
          case 10: ctx.show_cars = !ctx.show_cars; break;
          case 11: ctx.show_radar = !ctx.show_radar; break;
          case 12: adj(ctx.radar_range, 10, 30, 500); break;
          case 13: ctx.tracers = !ctx.tracers; break;
          case 14: ctx.aim = !ctx.aim; break;
          case 15: ctx.aim_key = (ctx.aim_key + (right ? 1 : 3)) % 4; break;
          case 16: ctx.aim_bone = (ctx.aim_bone + (right ? 1 : 2)) % 3; break;
          case 17: adj(ctx.aim_fov, 10, 30, 600); break;
          case 18: adj(ctx.aim_smooth, 1, 1, 20); break;
          case 19: ctx.basis_quat = !ctx.basis_quat; break;
          case 20: ctx.visible_only = !ctx.visible_only; break;
          case 21: ctx.menu_key = (ctx.menu_key + (right ? 1 : 3)) % 4; break;
        }
        InvalidateRect(ov, nullptr, FALSE);
      }
    }
    if (!IsWindow(g_game_wnd)) break;
    RECT cr{}; GetClientRect(g_game_wnd, &cr);
    POINT pt{0, 0}; ClientToScreen(g_game_wnd, &pt);
    ctx.width = cr.right - cr.left; ctx.height = cr.bottom - cr.top;
    SetWindowPos(ov, HWND_TOPMOST, pt.x, pt.y, ctx.width, ctx.height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    ULONGLONG now = GetTickCount64();
    if (now - last_snap >= 100) {
      last_snap = now;
      nzl::Vec3f cpos{}, cfwd{}, cright{}, cup{};
      auto rd3 = [&](uint64_t a, nzl::Vec3f& v) { float f[3] = {}; if (mem.read(a, f, 12)) v = {f[0], f[1], f[2]}; };
      rd3(ctx.chain.camera_model + off.cam_pos, cpos);
      bool basis_ok = false;
      if (ctx.basis_quat) basis_ok = basis_from_quat(mem, ctx.chain.camera_model + 0x2C, cfwd, cright, cup);
      if (!basis_ok) {
        rd3(ctx.chain.camera_model + off.cam_fwd, cfwd);
        rd3(ctx.chain.camera_model + off.cam_right, cright);
        rd3(ctx.chain.camera_model + off.cam_up, cup);
        cfwd = normalize(cfwd); cright = normalize(cright); cup = normalize(cup);
      }
      ctx.cam_fwd = cfwd; ctx.cam_right = cright; g_radar_cam = cpos;
      auto players = snapshot_players(ctx, cpos);
      nzl::CameraView view;
      view.pos = cpos; view.fwd = cfwd; view.right = cright; view.up = cup;
      view.width = ctx.width; view.height = ctx.height; view.fov_deg = ctx.fov;
      ctx.render.clear();
      const float kBoneH[] = {1.75f, 1.30f, 0.95f};
      for (auto& p : players) {
        if (p.dead && !ctx.show_dead) continue;
        if (p.dist > ctx.max_dist) continue;
        float fx, fy, fd, hx, hy, hd, ax, ay, ad;
        nzl::Vec3f head{p.pos.x, p.pos.y + 1.8f, p.pos.z};
        nzl::Vec3f bone{p.pos.x, p.pos.y + kBoneH[ctx.aim_bone], p.pos.z};
        if (!nzl::world_to_screen(view, p.pos, fx, fy, fd)) continue;
        if (!nzl::world_to_screen(view, head, hx, hy, hd)) continue;
        if (!nzl::world_to_screen(view, bone, ax, ay, ad)) continue;
        if (ctx.visible_only && (hx < 0 || hx > ctx.width || hy < 0 || hy > ctx.height)) continue;
        RenderEnt e; e.hx = hx; e.hy = hy; e.fx = fx; e.fy = fy; e.ax = ax; e.ay = ay;
        e.pos = p.pos; e.id = p.id; e.dist = p.dist; e.dead = p.dead;
        e.hp = p.hp; e.maxhp = p.maxhp; e.armor = p.armor; e.role = p.role;
        ctx.render.push_back(e);
      }
      if (ctx.show_cars) {
        ctx.cars = snapshot_cars(ctx, cpos);
        for (auto& l : ctx.cars) { float dep = 0; l.on = l.dist <= ctx.max_dist && nzl::world_to_screen(view, l.pos, l.sx, l.sy, dep); }
      } else ctx.cars.clear();
      if (ctx.show_loot) {
        ctx.loot = snapshot_loot(ctx, cpos);
        for (auto& l : ctx.loot) { float dep = 0; l.on = l.dist <= ctx.max_dist && nzl::world_to_screen(view, l.pos, l.sx, l.sy, dep); }
      } else ctx.loot.clear();
      // Aim: hold RMB, move mouse toward nearest alive head to centre.
      if (ctx.aim && (GetAsyncKeyState(kAimKeys[ctx.aim_key]) & 0x8000)) {
        float best = ctx.aim_fov; const RenderEnt* target = nullptr;
        float cxp = ctx.width * 0.5f, cyp = ctx.height * 0.5f;
        for (auto& e : ctx.render) {
          if (e.dead) continue;
          float dx = e.ax - cxp, dy = e.ay - cyp;
          float d = std::sqrt(dx * dx + dy * dy);
          if (d < best) { best = d; target = &e; }
        }
        if (target) {
          INPUT in{}; in.type = INPUT_MOUSE;
          in.mi.dwFlags = MOUSEEVENTF_MOVE;
          in.mi.dx = (LONG)((target->ax - cxp) / ctx.aim_smooth);
          in.mi.dy = (LONG)((target->ay - cyp) / ctx.aim_smooth);
          if (in.mi.dx || in.mi.dy) SendInput(1, &in, sizeof(INPUT));
        }
      }
      InvalidateRect(ov, nullptr, FALSE);
    }
    Sleep(16);
  }
  DestroyWindow(ov);
  mem.close();
  std::printf("[NZL] overlay closed.\n");
  return 0;
}
