// NZL Studio probe (Windows) — validates the Sharedmem Driver transport
// WITHOUT any game offsets. Safe first step after loading the driver:
// finds the game, reads PE headers through the driver, queries regions.
// Writes nzl_probe.txt in the current folder. Send me that file.
#ifndef _WIN32
#error "Windows only"
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
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
  PROCESSENTRY32W e{sizeof(e)}; DWORD pid = 0;
  if (Process32FirstW(s, &e)) do { if (_wcsicmp(e.szExeFile, name) == 0) { pid = e.th32ProcessID; break; } } while (Process32NextW(s, &e));
  CloseHandle(s); return pid;
}
static uint64_t module_base(DWORD pid, const wchar_t* name, bool* snapshot_ok) {
  HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
  if (s == INVALID_HANDLE_VALUE) { if (snapshot_ok) *snapshot_ok = false; return 0; }
  MODULEENTRY32W e{sizeof(e)}; uint64_t base = 0;
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
  // ONE pass for BOTH modules: each NaQueryRegion costs ~16 ms (worker
  // poll tick), and the old code scanned the whole address space twice
  // (~4600+ regions seen live on explorer = minutes per pass). Return as
  // soon as both are found.
  uint64_t found_ga = 0, found_up = 0;
  auto find_images = [&]() {
    const char* want = "GameAssembly.dll+UnityPlayer.dll";
    uint64_t addr = 0x10000;
    ULONGLONG scan_t0 = GetTickCount64();
    logf("[probe] region-scan start want=%s\n", want);
    for (int i = 0; i < 16384 && addr < 0x0000800000000000ULL; i++) {
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
  find_images();
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
  logf("[probe] RESULT: %s\n", transport ? "TRANSPORT_OK - driver reads the game; next step is offsets from Il2CppDumper (nzl_dump.zip)" : "TRANSPORT_FAIL - see lines above, send me nzl_probe.txt");
  NaClose();
  if (g_log) std::fclose(g_log);
  return transport ? 0 : 5;
}
