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
#include "nzl/windows_profile.h"
#include "../client/sc_client.h"

static DWORD find_pid(const wchar_t* name) {
  HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (s == INVALID_HANDLE_VALUE) return 0;
  PROCESSENTRY32W e{}; e.dwSize = sizeof(e); DWORD pid = 0;
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
static bool klass_name_is(const nzl::IMemory& m, uint64_t klass,
                          uint32_t name_offset, const char* want) {
  if (klass > UINT64_MAX - name_offset) return false;
  auto name_ptr = m.ptr(klass + name_offset);
  if (!name_ptr || !*name_ptr) return false;
  char name[64] = {};
  if (!m.read(*name_ptr, name, sizeof(name) - 1)) return false;
  name[sizeof(name) - 1] = 0;
  return std::strcmp(name, want) == 0;
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
  constexpr const auto& profile = nzl::game_profile::kOwnerProfile26866884;
  nzl::Offsets off(profile);
  if (!off.profile_configured) {
    std::printf("[NZL][profile] STOP: profile data is incomplete; no offsets used.\n");
    mem.close();
    return 6;
  }
  const auto file_check = nzl::windows_profile::verify_gameassembly_file(pid, profile);
  std::printf("[NZL][profile] id=%.*s build=%.*s file=%s size=%llu/%llu expected_sha256=%.*s actual_sha256=%s win32=%lu bcrypt=0x%08lX\n",
              (int)profile.fingerprint.profile_id.size(), profile.fingerprint.profile_id.data(),
              (int)profile.fingerprint.steam_build_id.size(), profile.fingerprint.steam_build_id.data(),
              nzl::windows_profile::status_name(file_check.status),
              (unsigned long long)file_check.actual_size,
              (unsigned long long)profile.fingerprint.gameassembly_file_size,
              (int)profile.fingerprint.gameassembly_sha256.size(), profile.fingerprint.gameassembly_sha256.data(),
              file_check.actual_sha256.c_str(), (unsigned long)file_check.win32_error,
              (unsigned long)file_check.bcrypt_status);
  if (!file_check.matched()) {
    std::printf("[NZL][profile] STOP: GameAssembly file does not match the owner fingerprint; no build offsets used.\n");
    mem.close();
    return 6;
  }
  const auto remote_read = [&](uint64_t address, void* output, size_t size) {
    return mem.read(address, output, size);
  };
  uint32_t image_size = 0;
  if (!nzl::windows_profile::read_loaded_pe_size_of_image(ga, remote_read, image_size) ||
      !nzl::game_profile::type_info_rvas_fit_image(profile, image_size)) {
    std::printf("[NZL][profile] STOP: loaded PE header is invalid or TypeInfo RVA is outside SizeOfImage (image=0x%x).\n",
                image_size);
    mem.close();
    return 6;
  }
  std::printf("[NZL][profile] loaded SizeOfImage=0x%x; TypeInfo RVA bounds OK; Game.GameModel uses +0x%x only (no historical fallback).\n",
              image_size, off.game_model);

  // Resolve each build-specific TypeInfo slot from the selected profile and
  // verify its class name before scanning any object fields.
  uint64_t klass[7] = {};
  for (size_t i = 0; i < profile.type_info.size(); ++i) {
    const auto& slot = profile.type_info[i];
    if (!slot.rva.present() || slot.rva.value > UINT64_MAX - ga) {
      std::printf("[NZL][profile] STOP: missing/out-of-range TypeInfo slot %.*s.\n",
                  (int)slot.class_name.size(), slot.class_name.data());
      mem.close();
      return 6;
    }
    auto class_ptr = mem.ptr(ga + slot.rva.value);
    klass[i] = class_ptr ? *class_ptr : 0;
    const bool ok = klass[i] && klass_name_is(mem, klass[i], off.il2cpp_class_name,
                                               slot.class_name.data());
    std::printf("[NZL] klass %-20.*s rva=0x%llx klass=0x%llx %s\n",
                (int)slot.class_name.size(), slot.class_name.data(),
                (unsigned long long)slot.rva.value, (unsigned long long)klass[i],
                ok ? "NAME_OK" : "NAME_MISMATCH");
    if (!ok) {
      std::printf("[NZL][profile] STOP: class validation failed; no chain offsets used.\n");
      mem.close();
      return 6;
    }
  }
  // Heap scan uses the single 26866884 profile and validates every chain
  // object's runtime klass before treating it as a live Game instance.
  auto obj_klass = [&](uint64_t o) -> uint64_t {
    auto k = o ? mem.ptr(o) : std::nullopt;
    return k ? *k : 0;
  };
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
      std::printf("[NZL] scan region base=0x%llx size=%llu MiB\n", (unsigned long long)r.base,
                  (unsigned long long)(r.size >> 20));
      std::fflush(stdout);
      uint64_t fails = 0, tries = 0;
      for (uint64_t o = 0; o < r.size && !game_obj; o += 0x10000) {
        uint64_t chunk = r.base + o;
        uint32_t want = (uint32_t)((r.size - o) < 0x10000 ? (r.size - o) : 0x10000);
        if (chunk >= 0x0000800000000000ULL) continue;
        tries++;
        if ((tries & 0xFF) == 0) {
          std::printf("[NZL] ... chunk 0x%llx +%llums scanned=%lluMiB fails=%llu\n",
                      (unsigned long long)chunk, (unsigned long long)(GetTickCount64() - t0),
                      (unsigned long long)(scanned >> 20), (unsigned long long)fails);
          std::fflush(stdout);
        }
        if (!mem.read(chunk, buf.data(), want)) { fails++; continue; }
        scanned += want;
        if ((scanned & 0x7FFFFFFULL) < want)
          std::printf("[NZL] heap scan %llu MiB +%llums\r", (unsigned long long)(scanned >> 20), (unsigned long long)(GetTickCount64() - t0));
        for (uint32_t b = 0; b + 8 <= want; b += 8) {
          uint64_t v = 0; std::memcpy(&v, buf.data() + b, 8);
          if (v != klass[0]) continue;
          uint64_t cand = chunk + b;
          const nzl::ChainResult candidate = nzl::resolve_chain_from_game(mem, cand, off);
          if (!candidate.ok || obj_klass(candidate.game_model) != klass[1] ||
              obj_klass(candidate.battle_model) != klass[2] ||
              obj_klass(candidate.battle_entities) != klass[3] ||
              obj_klass(candidate.world_entities) != klass[4] ||
              obj_klass(candidate.camera_model) != klass[5]) continue;
          const auto roster = nzl::read_dict(mem, candidate.dict, off);
          if (!roster || roster->count <= 0) continue;
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
  const auto roster_header = nzl::read_dict(mem, chain.dict, off);
  if (!roster_header) {
    std::printf("[NZL][profile] STOP: dictionary layout/count/capacity sanity check failed.\n");
    mem.close();
    return 8;
  }
  std::printf("[NZL][profile] roster Count=%d array_length=%d\n",
              roster_header->count, roster_header->array_length);
  auto players = nzl::snapshot_players(mem, chain, off, cpos, klass[6]);
  std::printf("[NZL] players snapshot: %zu (sorted by distance)\n", players.size());
  for (auto& p : players)
    std::printf("  #%d %s pos=(%.2f,%.2f,%.2f) dist=%.1fm @0x%llx\n", p.id, p.dead ? "DEAD" : "alive",
                p.pos.x, p.pos.y, p.pos.z, p.dist, (unsigned long long)p.addr);
  // Project with screen size (FOV 90 default - BattleCameraModel has no
  // FOV field; native Camera FOV wiring is the overlay step).
  nzl::CameraView view;
  view.pos = cpos; view.fwd = cfwd; view.right = cright; view.up = cup;
  view.head_height_m = off.head_height_m;
  view.fov_deg = off.assumed_vertical_fov_deg;
  view.width = GetSystemMetrics(SM_CXSCREEN); view.height = GetSystemMetrics(SM_CYSCREEN);
  auto draws = nzl::build_draw_list(view, players, 500.0f);
  std::printf("[NZL] draw commands (overlay input): %zu @%dx%d fov=%.0f\n", draws.size(), view.width, view.height, view.fov_deg);
  for (auto& d : draws)
    if (d.kind == nzl::DrawCmd::Text) std::printf("  text '%s' at (%.0f,%.0f)\n", d.text.c_str(), d.x1, d.y1);
  std::printf("[NZL] RESULT: ESP_DATA_OK - root/chain/players live. Overlay renders build_draw_list next.\n");
  mem.close();
  return 0;
}
