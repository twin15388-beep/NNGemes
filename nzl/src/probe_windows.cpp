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
  uint64_t ga = NaGetModule((uint32_t)pid, L"GameAssembly.dll");
  uint64_t up = NaGetModule((uint32_t)pid, L"UnityPlayer.dll");
  logf("[probe] driver PEB module walk: GameAssembly=0x%llx UnityPlayer=0x%llx\n", (unsigned long long)ga, (unsigned long long)up);
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
