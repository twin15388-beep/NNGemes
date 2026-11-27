// NZL Studio (Windows) — purple external ESP reader over the Sharedmem
// Driver. Pipeline (every stage validated live, build 26866884):
//   driver transport -> GameAssembly base (region scan, export name)
//   -> Il2CppClass* from TypeInfo slots + class-name check
//   -> heap scan for the live Game object (full chain klass validation)
//   -> resolve_chain_from_game -> _battleCharacters snapshot -> print.
// First live target: console snapshot of players (id / pos / dist).
// The D3D11 overlay consumes the same snapshot/draw-list next.
#ifndef _WIN32
#error "Windows only"
#endif
#include <windows.h>
#include <tlhelp32.h>
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
// GameAssembly base WITHOUT usermode module enum (EAC blocks it):
// walk regions via the driver, match PE export DLL name. (Same algorithm
// as probe, which found both modules live.)
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
int main() {
  std::printf("[NZL Studio] purple build, Sharedmem Driver reader.\n");
  DWORD pid = find_pid(L"ROB RUN.exe");
  if (!pid) { std::printf("[NZL] game not running. Start ROB RUN, enter a match, then rerun.\n"); return 2; }
  nzl::DriverMemory mem(pid);
  if (!mem.init()) {
    std::printf("[NZL] Sharedmem driver NOT loaded. Run tools\\NZL-LoadDriver.ps1 (admin) first, do not reboot after.\n");
    return 4;
  }
  std::printf("[NZL] pid=%lu transport OK. Scanning regions for GameAssembly...\n", pid);
  uint64_t ga = find_gameassembly(pid);
  if (!ga) { std::printf("[NZL] GameAssembly NOT FOUND via region scan. Send this output.\n"); mem.close(); return 5; }
  std::printf("[NZL] GameAssembly=0x%llx\n", (unsigned long long)ga);
  // TypeInfo slots (build 26866884, script.json) -> Il2CppClass* + name check.
  struct Slot { const char* want; uint64_t rva; };
  const Slot slots[] = {
    {"Game", 0x81B7710ULL}, {"GameModel", 0x81B86D8ULL}, {"BattleModel", 0x82496E8ULL},
    {"BattleEntitiesModel", 0x8245928ULL}, {"WorldEntitiesModel", 0x82295C8ULL},
    {"BattleCameraModel", 0x823C210ULL},
  };
  uint64_t klass[6] = {};
  for (int i = 0; i < 6; i++) {
    auto k = mem.ptr(ga + slots[i].rva);
    klass[i] = k ? *k : 0;
    bool ok = klass[i] && klass_name_is(mem, klass[i], slots[i].want);
    std::printf("[NZL] klass %-20s 0x%llx %s\n", slots[i].want, (unsigned long long)klass[i], ok ? "NAME_OK" : "NAME_MISMATCH");
    if (!ok) { std::printf("[NZL] STOP: class validation failed (wrong build or not in menu/match yet).\n"); mem.close(); return 6; }
  }
  // Heap scan for the live Game object (same validation as probe-9, which
  // found ROOT @1344MiB with dict count=25 in a live match).
  nzl::Offsets off;
  auto obj_klass = [&](uint64_t o) -> uint64_t { auto k = o ? mem.ptr(o) : std::nullopt; return k ? *k : 0; };
  uint64_t game_obj = 0;
  {
    std::vector<unsigned char> buf(0x10000);
    uint64_t addr = 0x10000, scanned = 0;
    ULONGLONG t0 = GetTickCount64();
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
        if ((scanned & 0x7FFFFFFULL) < want)
          std::printf("[NZL] heap scan %llu MiB +%llums\r", (unsigned long long)(scanned >> 20), (unsigned long long)(GetTickCount64() - t0));
        for (uint32_t b = 0; b + 8 <= want; b += 8) {
          uint64_t v = 0; std::memcpy(&v, buf.data() + b, 8);
          if (v != klass[0]) continue;
          uint64_t cand = chunk + b;
          // Validate as Game: GameModel klass at +0xD8 (alt +0xD0).
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
    std::printf("\n[NZL] heap scan done: %llu MiB, Game=0x%llx\n", (unsigned long long)(scanned >> 20), (unsigned long long)game_obj);
  }
  if (!game_obj) { std::printf("[NZL] Game object NOT FOUND (be in menu/match). Send this output.\n"); mem.close(); return 7; }
  nzl::ChainResult chain = nzl::resolve_chain_from_game(mem, game_obj, off);
  std::printf("[NZL] chain ok=%d stage=%s BattleModel=0x%llx WorldEntities=0x%llx Dict=0x%llx CameraModel=0x%llx\n",
              (int)chain.ok, chain.stage.c_str(), (unsigned long long)chain.battle_model,
              (unsigned long long)chain.world_entities, (unsigned long long)chain.dict, (unsigned long long)chain.camera_model);
  if (!chain.ok) { mem.close(); return 8; }
  // Camera vectors (BattleCameraModel: pos +0x10, fwd +0x3C, right +0x48, up +0x54).
  nzl::Vec3f cpos{}, cfwd{}, cright{}, cup{};
  auto rd3 = [&](uint64_t a, nzl::Vec3f& v) { float f[3] = {}; if (mem.read(a, f, 12)) v = {f[0], f[1], f[2]}; };
  rd3(chain.camera_model + off.cam_pos, cpos);
  rd3(chain.camera_model + off.cam_fwd, cfwd);
  rd3(chain.camera_model + off.cam_right, cright);
  rd3(chain.camera_model + off.cam_up, cup);
  std::printf("[NZL] camera pos=(%.2f,%.2f,%.2f) fwd=(%.2f,%.2f,%.2f)\n", cpos.x, cpos.y, cpos.z, cfwd.x, cfwd.y, cfwd.z);
  auto players = nzl::snapshot_players(mem, chain, off, cpos);
  std::printf("[NZL] players snapshot: %zu (sorted by distance)\n", players.size());
  for (auto& p : players)
    std::printf("  #%d %s pos=(%.2f,%.2f,%.2f) dist=%.1fm @0x%llx\n", p.id, p.dead ? "DEAD" : "alive",
                p.pos.x, p.pos.y, p.pos.z, p.dist, (unsigned long long)p.addr);
  // Project with screen size (FOV 90 default - BattleCameraModel has no
  // FOV field; native Camera FOV wiring is the overlay step).
  nzl::CameraView view;
  view.pos = cpos; view.fwd = cfwd; view.right = cright; view.up = cup;
  view.width = GetSystemMetrics(SM_CXSCREEN); view.height = GetSystemMetrics(SM_CYSCREEN);
  auto draws = nzl::build_draw_list(view, players, 500.0f);
  std::printf("[NZL] draw commands (overlay input): %zu @%dx%d fov=%.0f\n", draws.size(), view.width, view.height, view.fov_deg);
  for (auto& d : draws)
    if (d.kind == nzl::DrawCmd::Text) std::printf("  text '%s' at (%.0f,%.0f)\n", d.text.c_str(), d.x1, d.y1);
  std::printf("[NZL] RESULT: ESP_DATA_OK - root/chain/players live. Overlay renders build_draw_list next.\n");
  mem.close();
  return 0;
}
