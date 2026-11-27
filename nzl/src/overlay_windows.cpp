// NZL Studio overlay (Windows) — purple ESP over the live game.
// Data path is the one validated live by nzl_studio (probe-9 / studio
// run 2026-11-27): Sharedmem Driver -> GameAssembly region scan ->
// TypeInfo klass name checks -> Game heap scan -> chain -> batch-read
// _battleCharacters snapshot -> build_draw_list -> GDI layered overlay.
// Rendering: Win32 layered (colour-key) topmost click-through window
// tracking the game client rect. Purple style. END exits, UP/DOWN
// adjusts FOV (default 90; BattleCameraModel carries no FOV field, so
// verify box alignment visually and tune).
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
struct Ctx {
  DWORD pid = 0;
  nzl::DriverMemory* mem = nullptr;
  nzl::Offsets off;
  nzl::ChainResult chain;
  std::vector<nzl::DrawCmd> draws;
  float fov = 90.0f;
  int width = 1920, height = 1080;
  // In-game menu (INSERT toggles): settings applied every tick.
  bool menu = false;
  int menu_sel = 0;
  bool show_dead = false;
  bool show_boxes = true;
  bool show_text = true;
  float max_dist = 500.0f;
};
// Player snapshot v2: iterate the WHOLE entries array of the dictionary
// (array length @entries+0x18), not just _count - Dictionary keeps free
// slots (hash<0) inside, and _count alone dropped live players
// ("not everyone captured"). All reads are batched, chunked to the
// driver's 64-item batch limit.
static std::vector<nzl::PlayerSnap> snapshot_fast(Ctx& c, const nzl::Vec3f& local) {
  std::vector<nzl::PlayerSnap> out;
  auto h = nzl::read_dict(*c.mem, c.chain.dict, c.off);
  if (!h) return out;
  uint64_t arrlen64 = 0;
  if (!c.mem->read(h->entries + 0x18, &arrlen64, 8)) return out;
  int total = (int)(arrlen64 > 1024 ? 1024 : arrlen64);
  if (total <= 0) return out;
  std::vector<uint64_t> chars;
  // entries chunk of 24: hash(i32)+value(u64) packed stride 16
  for (int base = 0; base < total; base += 24) {
    int n = (total - base) < 24 ? (total - base) : 24;
    std::vector<NaItem> items;
    std::vector<uint8_t> buf(n * 16, 0);
    for (int i = 0; i < n; i++) {
      uint64_t e = h->entries + 0x20ULL + (uint64_t)(base + i) * c.off.dict_stride;
      items.push_back(NaItem{e, (uint32_t)(i * 16), 4});
      items.push_back(NaItem{e + 0x10, (uint32_t)(i * 16 + 8), 8});
    }
    if (!NaBatchRead(c.pid, items.data(), (uint32_t)items.size(), buf.data(), n * 16)) continue;
    for (int i = 0; i < n; i++) {
      int32_t hash = 0; uint64_t v = 0;
      std::memcpy(&hash, buf.data() + i * 16, 4);
      std::memcpy(&v, buf.data() + i * 16 + 8, 8);
      if (hash >= 0 && v) chars.push_back(v);
    }
  }
  int n = (int)chars.size();
  if (!n) return out;
  std::vector<uint64_t> transforms(n, 0);
  std::vector<float> pos(n * 3, 0.0f);
  std::vector<int32_t> ids(n, 0);
  std::vector<uint8_t> dead(n, 0);
  for (int base = 0; base < n; base += 32) {
    int m = (n - base) < 32 ? (n - base) : 32;
    std::vector<NaItem> ti; std::vector<uint8_t> tb(m * 8, 0);
    for (int i = 0; i < m; i++) ti.push_back(NaItem{chars[base + i] + c.off.char_transform, (uint32_t)(i * 8), 8});
    if (NaBatchRead(c.pid, ti.data(), (uint32_t)ti.size(), tb.data(), m * 8))
      for (int i = 0; i < m; i++) std::memcpy(&transforms[base + i], tb.data() + i * 8, 8);
    std::vector<NaItem> pi; std::vector<uint8_t> pb(m * 12, 0);
    for (int i = 0; i < m; i++) if (transforms[base + i]) pi.push_back(NaItem{transforms[base + i] + c.off.ctm_x, (uint32_t)(i * 12), 12});
    if (!pi.empty() && NaBatchRead(c.pid, pi.data(), (uint32_t)pi.size(), pb.data(), m * 12))
      for (int i = 0; i < m; i++) std::memcpy(&pos[(base + i) * 3], pb.data() + i * 12, 12);
  }
  for (int base = 0; base < n; base += 32) {
    int m = (n - base) < 32 ? (n - base) : 32;
    std::vector<NaItem> ii; std::vector<uint8_t> ib(m * 8, 0);
    for (int i = 0; i < m; i++) {
      ii.push_back(NaItem{chars[base + i] + c.off.char_id, (uint32_t)(i * 8), 4});
      ii.push_back(NaItem{chars[base + i] + c.off.char_is_dead, (uint32_t)(i * 8 + 4), 1});
    }
    if (NaBatchRead(c.pid, ii.data(), (uint32_t)ii.size(), ib.data(), m * 8))
      for (int i = 0; i < m; i++) {
        std::memcpy(&ids[base + i], ib.data() + i * 8, 4);
        dead[base + i] = ib[i * 8 + 4];
      }
  }
  for (int i = 0; i < n; i++) {
    float x = pos[i * 3], y = pos[i * 3 + 1], z = pos[i * 3 + 2];
    if ((x == 0 && y == 0 && z == 0) || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) continue;
    nzl::PlayerSnap s; s.addr = chars[i]; s.id = ids[i]; s.dead = dead[i] != 0;
    s.pos = {x, y, z};
    float dx = x - local.x, dy = y - local.y, dz = z - local.z;
    s.dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    out.push_back(s);
  }
  return out;
}
static HWND g_game_wnd = nullptr;
static BOOL CALLBACK enum_wnd(HWND h, LPARAM lp) {
  DWORD wpid = 0; GetWindowThreadProcessId(h, &wpid);
  if (wpid != (DWORD)lp || !IsWindowVisible(h)) return TRUE;
  RECT r{}; if (!GetClientRect(h, &r)) return TRUE;
  long area = (r.right - r.left) * (r.bottom - r.top);
  static long best = 0;
  if (area > best) { best = area; g_game_wnd = h; }
  return TRUE;
}
static Ctx* g_ctx = nullptr;
static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_PAINT) {
    PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
    RECT rc; GetClientRect(h, &rc);
    HBRUSH black = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(dc, &rc, black); DeleteObject(black);
    if (g_ctx) {
      HPEN pen = CreatePen(PS_SOLID, 2, RGB(191, 64, 255));  // NZL purple
      HGDIOBJ oldpen = SelectObject(dc, pen);
      SetBkMode(dc, TRANSPARENT);
      SetTextColor(dc, RGB(216, 160, 255));
      HFONT font = CreateFontW(14, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, 0, 0, 0, 0, 0, L"Segoe UI");
      HGDIOBJ oldfont = SelectObject(dc, font);
      for (auto& d : g_ctx->draws) {
        if (d.kind == nzl::DrawCmd::Rect) {
          HGDIOBJ oldb = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
          Rectangle(dc, (int)d.x1, (int)d.y1, (int)d.x2, (int)d.y2);
          SelectObject(dc, oldb);
        } else {
          TextOutA(dc, (int)d.x1, (int)d.y1, d.text.c_str(), (int)d.text.size());
        }
      }
      if (g_ctx->menu) {
        // NZL menu panel: INSERT toggles, UP/DOWN select, LEFT/RIGHT adjust.
        char lines[6][96];
        std::snprintf(lines[0], 96, "NZL Studio  [INSERT close]");
        std::snprintf(lines[1], 96, "FOV          %.0f", g_ctx->fov);
        std::snprintf(lines[2], 96, "Max distance %.0f m", g_ctx->max_dist);
        std::snprintf(lines[3], 96, "Show dead    %s", g_ctx->show_dead ? "ON" : "OFF");
        std::snprintf(lines[4], 96, "Boxes        %s", g_ctx->show_boxes ? "ON" : "OFF");
        std::snprintf(lines[5], 96, "Labels       %s", g_ctx->show_text ? "ON" : "OFF");
        HBRUSH panel = CreateSolidBrush(RGB(24, 0, 36));
        RECT pr{10, 10, 300, 10 + 22 * 6 + 10};
        FillRect(dc, &pr, panel); DeleteObject(panel);
        HPEN border = CreatePen(PS_SOLID, 1, RGB(191, 64, 255));
        HGDIOBJ ob = SelectObject(dc, border);
        HGDIOBJ obb = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
        Rectangle(dc, pr.left, pr.top, pr.right, pr.bottom);
        SelectObject(dc, ob); SelectObject(dc, obb); DeleteObject(border);
        for (int i = 0; i < 6; i++) {
          if (i > 0 && i - 1 == g_ctx->menu_sel) SetTextColor(dc, RGB(255, 255, 255));
          else if (i == 0) SetTextColor(dc, RGB(216, 160, 255));
          else SetTextColor(dc, RGB(200, 140, 255));
          TextOutA(dc, 20, 18 + i * 22, lines[i], (int)std::strlen(lines[i]));
        }
        SetTextColor(dc, RGB(216, 160, 255));
      }
      SelectObject(dc, oldpen); SelectObject(dc, oldfont);
      DeleteObject(pen); DeleteObject(font);
    }
    EndPaint(h, &ps);
    return 0;
  }
  return DefWindowProcW(h, msg, wp, lp);
}
int main() {
  std::printf("[NZL Studio overlay] purple ESP. INSERT=menu, END=exit.\n");
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
    // NA_CMD_SCAN: driver walks regions + copies in-kernel, returns up
    // to 256 klass candidates per call; we validate each by the class
    // chain and resume from the returned VA. Old client-side scan took
    // ~370s for 1.4GB; this should be seconds.
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
  std::printf("[NZL] chain OK. Starting overlay. Switch to the game.\n");
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
    if (GetAsyncKeyState(VK_INSERT) & 1) { ctx.menu = !ctx.menu; InvalidateRect(ov, nullptr, FALSE); }
    if (ctx.menu) {
      if (GetAsyncKeyState(VK_UP) & 1) ctx.menu_sel = (ctx.menu_sel + 4) % 5;
      if (GetAsyncKeyState(VK_DOWN) & 1) ctx.menu_sel = (ctx.menu_sel + 1) % 5;
      bool left = (GetAsyncKeyState(VK_LEFT) & 1) != 0;
      bool right = (GetAsyncKeyState(VK_RIGHT) & 1) != 0;
      if (left || right) {
        int d = right ? 1 : -1;
        switch (ctx.menu_sel) {
          case 0: ctx.fov = ctx.fov + d * 1.0f; if (ctx.fov < 40) ctx.fov = 40; if (ctx.fov > 140) ctx.fov = 140; break;
          case 1: ctx.max_dist = ctx.max_dist + d * 25.0f; if (ctx.max_dist < 25) ctx.max_dist = 25; if (ctx.max_dist > 1500) ctx.max_dist = 1500; break;
          case 2: ctx.show_dead = !ctx.show_dead; break;
          case 3: ctx.show_boxes = !ctx.show_boxes; break;
          case 4: ctx.show_text = !ctx.show_text; break;
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
      rd3(ctx.chain.camera_model + off.cam_fwd, cfwd);
      rd3(ctx.chain.camera_model + off.cam_right, cright);
      rd3(ctx.chain.camera_model + off.cam_up, cup);
      auto players = snapshot_fast(ctx, cpos);
      if (!ctx.show_dead) {
        std::vector<nzl::PlayerSnap> alive;
        for (auto& p : players) if (!p.dead) alive.push_back(p);
        players = alive;
      }
      // Normalise camera basis: BattleCameraModel vectors are direction
      // fields, not guaranteed unit length - unnormalised basis skews
      // projection away from screen centre ("right only at one angle").
      auto norm = [](nzl::Vec3f v) {
        float l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        return l > 0.001f ? nzl::Vec3f{v.x / l, v.y / l, v.z / l} : v;
      };
      nzl::CameraView view;
      view.pos = cpos; view.fwd = norm(cfwd); view.right = norm(cright); view.up = norm(cup);
      view.width = ctx.width; view.height = ctx.height; view.fov_deg = ctx.fov;
      ctx.draws = nzl::build_draw_list(view, players, ctx.max_dist);
      std::vector<nzl::DrawCmd> kept;
      for (auto& d : ctx.draws) {
        if (d.kind == nzl::DrawCmd::Rect && !ctx.show_boxes) continue;
        if (d.kind == nzl::DrawCmd::Text && !ctx.show_text) continue;
        kept.push_back(d);
      }
      ctx.draws = kept;
      InvalidateRect(ov, nullptr, FALSE);
    }
    Sleep(16);
  }
  DestroyWindow(ov);
  mem.close();
  std::printf("[NZL] overlay closed.\n");
  return 0;
}
