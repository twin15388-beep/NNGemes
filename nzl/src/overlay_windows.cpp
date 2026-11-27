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
};
// Fast player snapshot: 3 batch commands per tick (values, char fields,
// positions) instead of ~6 small reads per player.
static std::vector<nzl::PlayerSnap> snapshot_fast(Ctx& c, const nzl::Vec3f& local) {
  std::vector<nzl::PlayerSnap> out;
  auto h = nzl::read_dict(*c.mem, c.chain.dict, c.off);
  if (!h || h->count <= 0) return out;
  int n = h->count < 64 ? h->count : 64;
  std::vector<NaItem> items(n);
  std::vector<uint8_t> buf(n * 8, 0);
  for (int i = 0; i < n; i++) {
    items[i].address = h->entries + 0x20ULL + (uint64_t)i * c.off.dict_stride + 0x10;
    items[i].offset = (uint32_t)(i * 8); items[i].size = 8;
  }
  if (!NaBatchRead(c.pid, items.data(), n, buf.data(), n * 8)) return out;
  std::vector<uint64_t> chars(n, 0);
  for (int i = 0; i < n; i++) std::memcpy(&chars[i], buf.data() + i * 8, 8);

  // Simpler correct path: one batch for transforms, one for ids/dead is
  // over-engineering; do transforms in one batch, ids via direct reads
  // (few, once per tick).
  std::vector<uint8_t> tb(n * 8, 0);
  std::vector<NaItem> ti;
  for (int i = 0; i < n; i++) if (chars[i]) ti.push_back(NaItem{chars[i] + c.off.char_transform, (uint32_t)(i * 8), 8});
  if (ti.empty() || !NaBatchRead(c.pid, ti.data(), (uint32_t)ti.size(), tb.data(), n * 8)) return out;
  std::vector<uint8_t> pb(n * 12, 0);
  std::vector<NaItem> pi;
  for (int i = 0; i < n; i++) {
    uint64_t tr = 0; std::memcpy(&tr, tb.data() + i * 8, 8);
    if (tr) pi.push_back(NaItem{tr + c.off.ctm_x, (uint32_t)(i * 12), 12});
  }
  if (pi.empty() || !NaBatchRead(c.pid, pi.data(), (uint32_t)pi.size(), pb.data(), n * 12)) return out;
  for (int i = 0; i < n; i++) {
    if (!chars[i]) continue;
    float xyz[3] = {}; std::memcpy(xyz, pb.data() + i * 12, 12);
    if (xyz[0] == 0 && xyz[1] == 0 && xyz[2] == 0) continue;
    nzl::PlayerSnap s; s.addr = chars[i];
    if (auto id = c.mem->i32(chars[i] + c.off.char_id)) s.id = *id;
    if (auto d = c.mem->u8(chars[i] + c.off.char_is_dead)) s.dead = (*d != 0);
    s.pos = {xyz[0], xyz[1], xyz[2]};
    float dx = s.pos.x - local.x, dy = s.pos.y - local.y, dz = s.pos.z - local.z;
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
      SelectObject(dc, oldpen); SelectObject(dc, oldfont);
      DeleteObject(pen); DeleteObject(font);
    }
    EndPaint(h, &ps);
    return 0;
  }
  return DefWindowProcW(h, msg, wp, lp);
}
int main() {
  std::printf("[NZL Studio overlay] purple ESP. END=exit, UP/DOWN=FOV.\n");
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
  std::printf("[NZL] classes OK. Heap scan for Game (minutes)...\n");
  nzl::Offsets off;
  auto obj_klass = [&](uint64_t o) -> uint64_t { auto k = o ? mem.ptr(o) : std::nullopt; return k ? *k : 0; };
  uint64_t game_obj = 0;
  {
    std::vector<unsigned char> buf(0x10000);
    uint64_t addr = 0x10000, scanned = 0;
    for (int i = 0; i < 16384 && addr < 0x0000800000000000ULL && !game_obj; i++) {
      NaRegion r{};
      if (!NaQueryRegion(pid, addr, &r)) break;
      uint64_t next = r.base + (r.size ? r.size : 0x1000);
      if (next <= addr) break;
      addr = next;
      bool readable = (r.protect == 0x02 || r.protect == 0x04 || r.protect == 0x20 || r.protect == 0x40 || r.protect == 0x80);
      if (r.state != 0x1000 || r.type != 0x20000 || !readable) continue;
      for (uint64_t o = 0; o < r.size && !game_obj; o += 0x10000) {
        uint64_t chunk = r.base + o;
        uint32_t want = (uint32_t)((r.size - o) < 0x10000 ? (r.size - o) : 0x10000);
        if (chunk >= 0x0000800000000000ULL) continue;
        if (!mem.read(chunk, buf.data(), want)) continue;
        scanned += want;
        if ((scanned & 0x3FFFFFFULL) < want) { std::printf("[NZL] heap scan %llu MiB\r", (unsigned long long)(scanned >> 20)); std::fflush(stdout); }
        for (uint32_t b = 0; b + 8 <= want; b += 8) {
          uint64_t v = 0; std::memcpy(&v, buf.data() + b, 8);
          if (v != klass[0]) continue;
          uint64_t cand = chunk + b;
          auto gm = mem.ptr(cand + off.game_model);
          if (!gm || obj_klass(*gm) != klass[1]) { gm = mem.ptr(cand + off.game_model_alt); if (!gm || obj_klass(*gm) != klass[1]) continue; }
          auto bm = mem.ptr(*gm + off.game_battle);
          if (!bm || obj_klass(*bm) != klass[2]) continue;
          auto be = mem.ptr(*bm + off.battle_entities);
          if (!be || obj_klass(*be) != klass[3]) continue;
          game_obj = cand;
        }
      }
    }
    std::printf("\n[NZL] Game=0x%llx (scanned %llu MiB)\n", (unsigned long long)game_obj, (unsigned long long)(scanned >> 20));
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
    if (GetAsyncKeyState(VK_UP) & 1 && ctx.fov < 140) ctx.fov += 1;
    if (GetAsyncKeyState(VK_DOWN) & 1 && ctx.fov > 40) ctx.fov -= 1;
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
      nzl::CameraView view;
      view.pos = cpos; view.fwd = cfwd; view.right = cright; view.up = cup;
      view.width = ctx.width; view.height = ctx.height; view.fov_deg = ctx.fov;
      ctx.draws = nzl::build_draw_list(view, players, 500.0f);
      InvalidateRect(ov, nullptr, FALSE);
    }
    Sleep(16);
  }
  DestroyWindow(ov);
  mem.close();
  std::printf("[NZL] overlay closed.\n");
  return 0;
}
