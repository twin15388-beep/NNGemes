// NZL Studio probe (Windows) — checks Sharedmem transport first, then uses
// only the owner build profile after its GameAssembly fingerprint, loaded PE
// bounds, and TypeInfo class names match. Writes nzl_probe.txt in cwd.
#ifndef _WIN32
#error "Windows only"
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <iterator>
#include <limits>
#include <string_view>
#include "nzl/chain.h"
#include "nzl/esp_pipeline.h"
#include "nzl/windows_profile.h"
#include "../client/sc_client.h"

static FILE* g_log = nullptr;
static void logf(const char* fmt, ...) {
  char buf[1024]; va_list ap; va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
  std::printf("%s", buf);
  std::fflush(stdout);
  if (g_log) { std::fputs(buf, g_log); std::fflush(g_log); }
}
static DWORD find_pid(const wchar_t* name) {
  HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (s == INVALID_HANDLE_VALUE) return 0;
  PROCESSENTRY32W e{}; e.dwSize = sizeof(e); DWORD pid = 0;
  if (Process32FirstW(s, &e)) do { if (_wcsicmp(e.szExeFile, name) == 0) { pid = e.th32ProcessID; break; } } while (Process32NextW(s, &e));
  CloseHandle(s); return pid;
}
static uint64_t module_base(DWORD pid, const wchar_t* name, bool* snapshot_ok) {
  HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
  if (s == INVALID_HANDLE_VALUE) { if (snapshot_ok) *snapshot_ok = false; return 0; }
  MODULEENTRY32W e{}; e.dwSize = sizeof(e); uint64_t base = 0;
  if (Module32FirstW(s, &e)) do { if (_wcsicmp(e.szModule, name) == 0) { base = (uint64_t)e.modBaseAddr; break; } } while (Module32NextW(s, &e));
  CloseHandle(s); if (snapshot_ok) *snapshot_ok = true; return base;
}
static bool read_mz_pe(uint32_t pid, uint64_t base, const char* label) {
  if (!base) { logf("[probe] %s: base=0 SKIP\n", label); return false; }
  uint16_t mz = 0;
  if (!NaRead(pid, base, &mz)) { logf("[probe] %s: driver read MZ FAILED @0x%llx\n", label, (unsigned long long)base); return false; }
  uint32_t lfanew = 0; NaRead(pid, base + 0x3C, &lfanew);
  uint32_t sig = 0; if (lfanew && lfanew < 0x1000) NaRead(pid, base + lfanew, &sig);
  bool ok = (mz == 0x5A4D) && (sig == 0x00004550);
  logf("[probe] %s: MZ=0x%04x e_lfanew=0x%x PE=0x%x -> %s\n", label, mz, lfanew, sig, ok ? "OK" : "MISMATCH");
  return ok;
}
class ProbeMemory final : public nzl::IMemory {
 public:
  explicit ProbeMemory(DWORD pid) : pid_(pid) {}
  bool read(uint64_t address, void* output, size_t size) const override {
    if (!output || size == 0 || size > std::numeric_limits<uint32_t>::max())
      return false;
    return NaRead(static_cast<uint32_t>(pid_), address,
                  static_cast<uint32_t>(size), output);
  }
 private:
  DWORD pid_ = 0;
};
int main(int argc, char** argv) {
  g_log = std::fopen("nzl_probe.txt", "w");
  logf("[NZL Studio probe] transport check, no offsets used\n");
  DWORD pid = 0;
  wchar_t proc[128] = L"ROB RUN.exe";
  if (argc > 1) { pid = (DWORD)std::strtoul(argv[1], nullptr, 10); }
  if (!pid) pid = find_pid(proc);
  if (!pid) { logf("[probe] game process 'ROB RUN.exe' not found. Start the game (normal Steam launch), get into the menu/match, run probe again.\n"); if (g_log) std::fclose(g_log); return 2; }
  logf("[probe] pid=%lu\n", pid);
  // Stage diagnostics: separate registry / section / mapping failures.
  {
    HKEY hk = nullptr;
    LONG ro = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\NaCoreFilter", 0, KEY_READ, &hk);
    if (ro != ERROR_SUCCESS) {
      logf("[probe] registry HKLM\\SYSTEM\\NaCoreFilter OPEN FAILED win32=%ld. Driver not loaded in THIS Windows session (volatile key dies on reboot: reload driver, do not reboot between load and probe).\n", ro);
      if (g_log) std::fclose(g_log); return 4;
    }
    DWORD kv = 0, type = 0, sz = sizeof(kv);
    RegQueryValueExW(hk, L"Key", nullptr, &type, (BYTE*)&kv, &sz);
    WCHAR shm[16] = {}; sz = sizeof(shm);
    RegQueryValueExW(hk, L"Shm", nullptr, &type, (BYTE*)shm, &sz);
    RegCloseKey(hk);
    logf("[probe] registry Key=%lu Shm=%ls\n", kv, shm);
    if (!kv || !shm[0]) { logf("[probe] registry values empty -> driver startup incomplete. Reload driver.\n"); if (g_log) std::fclose(g_log); return 4; }
    WCHAR full[32]; swprintf_s(full, L"Global\\%s", shm);
    HANDLE hm = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, full);
    if (!hm) {
      DWORD e = GetLastError();
      logf("[probe] OpenFileMapping(%ls) FAILED win32=%lu. %s\n", full, e,
           e == ERROR_ACCESS_DENIED ? "ACCESS DENIED: run nzl_probe.exe as ADMINISTRATOR (probe and driver must match privilege)." : "Section missing: driver worker died or wrong Windows session.");
      if (g_log) std::fclose(g_log); return 4;
    }
    CloseHandle(hm);
    logf("[probe] section open OK (%ls)\n", full);
  }
  if (!NaInit()) {
    logf("[probe] NaInit FAILED after section check. Send me nzl_probe.txt.\n");
    if (g_log) std::fclose(g_log); return 4;
  }
  logf("[probe] NaInit OK (sharedmem transport alive)\n");
  ProbeMemory mem(pid);
  uint64_t base = NaGetBase(pid);
  uint64_t peb = NaGetPeb(pid);
  logf("[probe] exe base=0x%llx peb=0x%llx\n", (unsigned long long)base, (unsigned long long)peb);
  bool transport = read_mz_pe((uint32_t)pid, base, "exe");
  // Module bases WITHOUT new kernel code: scan MEM_IMAGE regions via
  // NaQueryRegion and match the PE export DLL name via NaRead.
  // Single REGION test FIRST: this is the command that used to BSOD and
  // the first thing after the exe PE read. If the output stops after the
  // "single NaQueryRegion..." line, the driver worker is stuck inside
  // REGION; if it prints OK, the hang is scan volume, not the driver.
  logf("[probe] single NaQueryRegion(exe base) ...\n");
  {
    NaRegion r0{};
    ULONGLONG t0 = GetTickCount64();
    bool ok0 = NaQueryRegion((uint32_t)pid, base, &r0);
    logf("[probe] single NaQueryRegion -> %s in %llums base=0x%llx size=0x%llx state=0x%x protect=0x%x type=0x%x\n",
         ok0 ? "OK" : "FAIL/TIMEOUT", (unsigned long long)(GetTickCount64() - t0),
         (unsigned long long)r0.base, (unsigned long long)r0.size, r0.state, r0.protect, r0.type);
  }
  uint64_t found_ga = 0, found_up = 0;

  // FIRST PATH: NA_CMD_MODULE. The driver walks the target PEB
  // (PEB+0x18 -> LDR+0x10 -> InLoadOrderModuleList) and returns DllBase in
  // ONE command. The previous probe had only the region scan below: 5809
  // REGION round trips at ~16 ms each = 238 s live (nzl_probe.txt
  // 2026-11-27 22:41) and it still returned 0x0 for both modules because
  // the walk stopped at 0x7fffffff0000 before reaching the system DLL area.
  {
    ULONGLONG t0 = GetTickCount64();
    found_ga = NaGetModule((uint32_t)pid, L"GameAssembly.dll");
    found_up = NaGetModule((uint32_t)pid, L"UnityPlayer.dll");
    logf("[probe] NaGetModule (PEB walk): GameAssembly=0x%llx UnityPlayer=0x%llx (+%llums)\n",
         (unsigned long long)found_ga, (unsigned long long)found_up,
         (unsigned long long)(GetTickCount64() - t0));
  }
  // A module base must point at a real MZ header; anything else is a bad
  // PEB walk and must not be trusted downstream.
  auto image_ok = [&](uint64_t va) -> bool {
    if (!va) return false;
    uint16_t mz = 0;
    if (!NaRead((uint32_t)pid, va, &mz) || mz != 0x5A4D) {
      logf("[probe] reject module base 0x%llx: no MZ header via transport\n",
           (unsigned long long)va);
      return false;
    }
    return true;
  };
  if (!image_ok(found_ga)) found_ga = 0;
  if (!image_ok(found_up)) found_up = 0;

  // SECOND PATH (fallback only): region scan, MEM_IMAGE regions, export
  // DLL name match. Kept because it is the only discovery that works if the
  // PEB walk is wrong on this build; bounded to 4096 regions so it can never
  // burn another four minutes.
  auto find_images = [&]() {
    const char* want = "GameAssembly.dll+UnityPlayer.dll";
    uint64_t addr = 0x10000;
    ULONGLONG scan_t0 = GetTickCount64();
    logf("[probe] region-scan start want=%s\n", want);
    for (int i = 0; i < 4096 && addr < 0x0000800000000000ULL; i++) {
      NaRegion r{};
      if (!NaQueryRegion((uint32_t)pid, addr, &r)) {
        logf("[probe] region-scan %s: NaQueryRegion FAIL at i=%d addr=0x%llx (scan ends)\n",
             want, i, (unsigned long long)addr);
        break;
      }
      if ((i % 256) == 0)
        logf("[probe] region-scan %s: i=%d addr=0x%llx +%llums ga=0x%llx up=0x%llx\n", want, i,
             (unsigned long long)addr, (unsigned long long)(GetTickCount64() - scan_t0),
             (unsigned long long)found_ga, (unsigned long long)found_up);
      uint64_t next = r.base + (r.size ? r.size : 0x1000);
      if (next <= addr) break;
      if (r.type == 0x1000000 /*MEM_IMAGE*/ && r.base) {
        uint16_t mz = 0;
        if (NaRead((uint32_t)pid, r.base, &mz) && mz == 0x5A4D) {
          uint32_t lfanew = 0; NaRead((uint32_t)pid, r.base + 0x3C, &lfanew);
          if (lfanew && lfanew < 0x1000) {
            // DataDirectory[0] (export) offset inside the optional header
            // differs by bitness: PE32 = 96, PE32+ (x64 game) = 112.
            // The previous build always used 96, read garbage as the
            // export RVA and therefore matched NOTHING on a full scan
            // (probe-6: 6469 regions, both modules 0x0). Read the magic.
            uint16_t opt_magic = 0;
            NaRead((uint32_t)pid, r.base + lfanew + 24, &opt_magic);
            uint32_t dd_off = (opt_magic == 0x20b) ? 112 : (opt_magic == 0x10b) ? 96 : 0;
            uint32_t export_rva = 0;
            if (dd_off) NaRead((uint32_t)pid, r.base + lfanew + 24 + dd_off, &export_rva);
            if (export_rva) {
              uint32_t name_rva = 0;
              NaRead((uint32_t)pid, r.base + export_rva + 12, &name_rva);
              char name[64] = {};
              if (name_rva && NaRead((uint32_t)pid, r.base + name_rva, 63, name)) {
                name[63] = 0;
                logf("[probe] image candidate base=0x%llx export-name='%s'\n",
                     (unsigned long long)r.base, name);
                if (_stricmp(name, "GameAssembly.dll") == 0) found_ga = r.base;
                if (_stricmp(name, "UnityPlayer.dll") == 0) found_up = r.base;
                if (found_ga && found_up) {
                  logf("[probe] region-scan: both modules found at i=%d, early exit\n", i);
                  return;
                }
              }
            }
          }
        }
      }
      addr = next;
    }
  };
  if (!found_ga || !found_up) {
    logf("[probe] PEB walk incomplete -> falling back to bounded region scan\n");
    find_images();
  }
  uint64_t ga = found_ga;
  uint64_t up = found_up;
  logf("[probe] region-scan modules: GameAssembly=0x%llx UnityPlayer=0x%llx\n", (unsigned long long)ga, (unsigned long long)up);
  bool snap = false;
  if (!ga) { ga = module_base(pid, L"GameAssembly.dll", &snap); logf("[probe] snapshot fallback GameAssembly=0x%llx (%s)\n", (unsigned long long)ga, snap ? "OK" : "FAILED"); }
  if (!up) { up = module_base(pid, L"UnityPlayer.dll", &snap); }
  if (ga) transport = read_mz_pe((uint32_t)pid, ga, "GameAssembly") && transport;
  if (ga) {
    NaRegion r{};
    if (NaQueryRegion((uint32_t)pid, ga, &r))
      logf("[probe] GameAssembly region: base=0x%llx size=0x%llx state=0x%x protect=0x%x type=0x%x\n",
           (unsigned long long)r.base, (unsigned long long)r.size, r.state, r.protect, r.type);
    else
      logf("[probe] NaQueryRegion(GameAssembly) FAILED\n");
  }
  logf("[probe] transport result: %s\n",
       transport ? "TRANSPORT_OK - driver reads this process" :
                   "TRANSPORT_FAIL - see lines above, send me nzl_probe.txt");

  constexpr const auto& profile = nzl::game_profile::kOwnerProfile26866884;
  const nzl::Offsets offsets(profile);
  int result_code = transport ? 0 : 5;
  bool profile_identity_ok = false;
  bool classes_ok = false;
  bool root_found = false;
  nzl::ChainResult live_chain;
  uint64_t klass[7]{};

  if (!transport) {
    logf("[profile] STOP: transport checks failed; no build offsets used.\n");
  } else if (!ga) {
    logf("[profile] STOP: GameAssembly base was not found; no build offsets used.\n");
    result_code = 6;
  } else if (!offsets.profile_configured) {
    logf("[profile] STOP: selected profile is incomplete; no build offsets used.\n");
    result_code = 6;
  } else {
    // No game-specific offset is used unless the on-disk owner SHA/size and
    // loaded PE image bounds both match this single build profile.
    const auto file_check = nzl::windows_profile::verify_gameassembly_file(pid, profile);
    logf("[profile] id=%.*s build=%.*s status=%s size=%llu/%llu expected_sha256=%.*s actual_sha256=%s win32=%lu bcrypt=0x%08lX\n",
         static_cast<int>(profile.fingerprint.profile_id.size()), profile.fingerprint.profile_id.data(),
         static_cast<int>(profile.fingerprint.steam_build_id.size()), profile.fingerprint.steam_build_id.data(),
         nzl::windows_profile::status_name(file_check.status),
         static_cast<unsigned long long>(file_check.actual_size),
         static_cast<unsigned long long>(profile.fingerprint.gameassembly_file_size),
         static_cast<int>(profile.fingerprint.gameassembly_sha256.size()), profile.fingerprint.gameassembly_sha256.data(),
         file_check.actual_sha256.c_str(), static_cast<unsigned long>(file_check.win32_error),
         static_cast<unsigned long>(file_check.bcrypt_status));

    const auto remote_read = [&](uint64_t address, void* output, size_t size) {
      if (size > std::numeric_limits<uint32_t>::max()) return false;
      return NaRead(static_cast<uint32_t>(pid), address,
                    static_cast<uint32_t>(size), output) != false;
    };
    uint32_t image_size = 0;
    if (!file_check.matched()) {
      logf("[profile] STOP: GameAssembly fingerprint mismatch; no profile offsets used.\n");
      result_code = 7;
    } else if (!nzl::windows_profile::read_loaded_pe_size_of_image(ga, remote_read, image_size) ||
               !nzl::game_profile::type_info_rvas_fit_image(profile, image_size)) {
      logf("[profile] STOP: invalid loaded PE/TypeInfo bounds, SizeOfImage=0x%x.\n",
           image_size);
      result_code = 7;
    } else {
      profile_identity_ok = true;
      logf("[profile] loaded SizeOfImage=0x%x; profile TypeInfo RVAs fit; Game.GameModel=+0x%x (no fallback).\n",
           image_size, offsets.game_model);
    }
  }

  if (profile_identity_ok) {
    auto class_name_matches = [&](uint64_t class_pointer,
                                  std::string_view expected) -> bool {
      if (!nzl::plausible_ptr(class_pointer) ||
          class_pointer > UINT64_MAX - offsets.il2cpp_class_name) return false;
      const auto name_pointer = mem.ptr(class_pointer + offsets.il2cpp_class_name);
      if (!name_pointer || !nzl::plausible_ptr(*name_pointer)) return false;
      char actual[96]{};
      if (!mem.read(*name_pointer, actual, sizeof(actual) - 1)) return false;
      actual[sizeof(actual) - 1] = '\0';
      return std::string_view(actual) == expected;
    };

    classes_ok = true;
    for (size_t i = 0; i < profile.type_info.size(); ++i) {
      const auto& slot = profile.type_info[i];
      if (!slot.rva.present() || slot.rva.value > UINT64_MAX - ga) {
        classes_ok = false;
        logf("[profile] TypeInfo %.*s: missing/out-of-range RVA.\n",
             static_cast<int>(slot.class_name.size()), slot.class_name.data());
        break;
      }
      const auto class_pointer = mem.ptr(ga + slot.rva.value);
      klass[i] = class_pointer ? *class_pointer : 0;
      const bool name_ok = class_name_matches(klass[i], slot.class_name);
      logf("[profile] TypeInfo %-24.*s rva=0x%llX klass=0x%llX %s\n",
           static_cast<int>(slot.class_name.size()), slot.class_name.data(),
           static_cast<unsigned long long>(slot.rva.value),
           static_cast<unsigned long long>(klass[i]),
           name_ok ? "NAME_OK" : "NAME_MISMATCH");
      if (!name_ok) {
        classes_ok = false;
        break;
      }
    }
    if (!classes_ok) result_code = 8;
  }

  if (profile_identity_ok && classes_ok) {
    auto chain_classes_match = [&](const nzl::ChainResult& candidate) -> bool {
      if (!candidate.ok) return false;
      const uint64_t objects[] = {candidate.game, candidate.game_model,
                                  candidate.battle_model, candidate.battle_entities,
                                  candidate.world_entities, candidate.camera_model};
      for (size_t i = 0; i < std::size(objects); ++i) {
        const auto object_class = mem.ptr(objects[i]);
        if (!object_class || *object_class != klass[i]) return false;
      }
      return true;
    };

    uint64_t scan_start = 0x10000;
    uint64_t candidates[256]{};
    uint32_t scan_calls = 0;
    while (scan_start && !root_found) {
      uint64_t next = 0;
      const uint32_t found = NaScan(static_cast<uint32_t>(pid), klass[0],
                                    scan_start, candidates, 256, &next);
      ++scan_calls;
      for (uint32_t i = 0; i < found; ++i) {
        const auto candidate = nzl::resolve_chain_from_game(mem, candidates[i], offsets);
        if (!chain_classes_match(candidate)) continue;
        const auto roster = nzl::read_dict(mem, candidate.dict, offsets);
        if (!roster || roster->count <= 0) continue;
        live_chain = candidate;
        root_found = true;
        break;
      }
      if ((scan_calls % 64) == 0 || root_found)
        logf("[root] scan calls=%lu start=0x%llX next=0x%llX candidates=%lu root=%s\n",
             static_cast<unsigned long>(scan_calls),
             static_cast<unsigned long long>(scan_start),
             static_cast<unsigned long long>(next),
             static_cast<unsigned long>(found), root_found ? "FOUND" : "not-yet");
      if (root_found || !next) break;
      if (next <= scan_start) {
        logf("[root] STOP: scan cursor did not advance (0x%llX -> 0x%llX).\n",
             static_cast<unsigned long long>(scan_start),
             static_cast<unsigned long long>(next));
        break;
      }
      scan_start = next;
    }

    if (!root_found) {
      logf("[root] RESULT: ROOT_NOT_FOUND after %lu scan calls; profile offsets were class-gated.\n",
           static_cast<unsigned long>(scan_calls));
      result_code = 9;
    } else {
      const auto roster = nzl::read_dict(mem, live_chain.dict, offsets);
      logf("[root] ROOT_OK Game=0x%llX GameModel=0x%llX BattleModel=0x%llX BattleEntities=0x%llX WorldEntities=0x%llX Dict=0x%llX Camera=0x%llX\n",
           static_cast<unsigned long long>(live_chain.game),
           static_cast<unsigned long long>(live_chain.game_model),
           static_cast<unsigned long long>(live_chain.battle_model),
           static_cast<unsigned long long>(live_chain.battle_entities),
           static_cast<unsigned long long>(live_chain.world_entities),
           static_cast<unsigned long long>(live_chain.dict),
           static_cast<unsigned long long>(live_chain.camera_model));
      if (roster)
        logf("[profile] roster dictionary Count=%d array_capacity=%u\n",
             roster->count, roster->array_length);

      const auto camera_vector = [&](uint32_t field_offset,
                                     nzl::Vec3f& value) -> bool {
        if (live_chain.camera_model > UINT64_MAX - field_offset) return false;
        const auto read = nzl::read_vec3(mem, live_chain.camera_model + field_offset);
        if (!read) return false;
        value = *read;
        return true;
      };
      nzl::Vec3f camera_position{}, camera_forward{}, camera_right{}, camera_up{};
      const bool camera_ok = camera_vector(offsets.cam_pos, camera_position) &&
                             camera_vector(offsets.cam_fwd, camera_forward) &&
                             camera_vector(offsets.cam_right, camera_right) &&
                             camera_vector(offsets.cam_up, camera_up);
      logf("[camera] vectors=%s pos=(%.2f,%.2f,%.2f) fwd=(%.2f,%.2f,%.2f)\n",
           camera_ok ? "OK" : "PARTIAL", camera_position.x, camera_position.y,
           camera_position.z, camera_forward.x, camera_forward.y, camera_forward.z);

      const auto players = nzl::snapshot_players(mem, live_chain, offsets,
                                                  camera_position, klass[6]);
      nzl::CameraView view;
      view.pos = camera_position;
      view.fwd = camera_forward;
      view.right = camera_right;
      view.up = camera_up;
      view.head_height_m = offsets.head_height_m;
      view.fov_deg = offsets.assumed_vertical_fov_deg;
      view.width = GetSystemMetrics(SM_CXSCREEN);
      view.height = GetSystemMetrics(SM_CYSCREEN);
      const auto draws = nzl::build_draw_list(view, players, 500.0f);
      size_t drawn_players = 0;
      for (const auto& command : draws)
        if (command.kind == nzl::DrawCmd::Text) ++drawn_players;
      logf("[snapshot] roster_count=%d roster_capacity=%u valid_pos=%zu drawn=%zu draw_commands=%zu fov_assumed=%.1f\n",
           roster ? roster->count : 0, roster ? roster->array_length : 0,
           players.size(), drawn_players, draws.size(), view.fov_deg);
      for (const auto& player : players)
        logf("  #%d %s pos=(%.2f,%.2f,%.2f) dist=%.1fm @0x%llX\n",
             player.id, player.dead ? "DEAD" : "alive", player.pos.x,
             player.pos.y, player.pos.z, player.dist,
             static_cast<unsigned long long>(player.addr));
      logf("[probe] RESULT: ROOT_OK - exact owner fingerprint, TypeInfo names, full class chain, and Dictionary bounds validated.\n");
      if (!transport) result_code = 5;
    }
  }

  NaClose();
  if (g_log) std::fclose(g_log);
  return result_code;
}
