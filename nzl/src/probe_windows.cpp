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
  if (g_log) std::fputs(buf, g_log);
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
  if (!NaInit()) {
    logf("[probe] DRIVER NOT LOADED: HKLM\\SYSTEM\\NaCoreFilter missing/Key empty. Load driver.sys via kdmapper (admin) BEFORE starting the game, then rerun.\n");
    if (g_log) std::fclose(g_log); return 4;
  }
  logf("[probe] NaInit OK (sharedmem transport alive)\n");
  uint64_t base = NaGetBase(pid);
  uint64_t peb = NaGetPeb(pid);
  logf("[probe] exe base=0x%llx peb=0x%llx\n", (unsigned long long)base, (unsigned long long)peb);
  bool transport = read_mz_pe((uint32_t)pid, base, "exe");
  bool snap = false;
  uint64_t ga = module_base(pid, L"GameAssembly.dll", &snap);
  uint64_t up = module_base(pid, L"UnityPlayer.dll", &snap);
  logf("[probe] module snapshot %s: GameAssembly=0x%llx UnityPlayer=0x%llx\n", snap ? "OK" : "FAILED (EAC may block usermode enum; driver read still usable if base known)", (unsigned long long)ga, (unsigned long long)up);
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
