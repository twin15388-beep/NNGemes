// NZL Studio (Windows only) — own external reader skeleton.
// Build: cmake -S nzl -B build-nzl -A x64 && cmake --build build-nzl --config Release
// It does NOT load any driver and does NOT use foreign overlays.
// Step 1 (no game running): prints what live dump it needs (docs/EXTRACT.md).
// Step 2: after you provide nzl_root.json (from tools/collect_nzl_info.py +
// Il2CppDumper), root resolution must be validated live before ESP loop.
#ifndef _WIN32
#error "Windows only"
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <string>
#include "nzl/chain.h"
#include "nzl/driver_memory.h"

struct WinMemory final : nzl::IMemory {
  HANDLE h = nullptr;
  bool read(uint64_t addr, void* out, size_t size) const override {
    SIZE_T got = 0; return ReadProcessMemory(h, (LPCVOID)addr, out, size, &got) && got == size;
  }
};
static DWORD find_pid(const wchar_t* name) {
  HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0); if (s == INVALID_HANDLE_VALUE) return 0;
  PROCESSENTRY32W e{sizeof(e)}; DWORD pid = 0;
  if (Process32FirstW(s, &e)) do { if (_wcsicmp(e.szExeFile, name) == 0) { pid = e.th32ProcessID; break; } } while (Process32NextW(s, &e));
  CloseHandle(s); return pid;
}
static uint64_t module_base(DWORD pid, const wchar_t* name) {
  HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid); if (s == INVALID_HANDLE_VALUE) return 0;
  MODULEENTRY32W e{sizeof(e)}; uint64_t base = 0;
  if (Module32FirstW(s, &e)) do { if (_wcsicmp(e.szModule, name) == 0) { base = (uint64_t)e.modBaseAddr; break; } } while (Module32NextW(s, &e));
  CloseHandle(s); return base;
}
int main() {
  std::printf("[NZL Studio] purple build, own reader. No driver loaded.\n");
  DWORD pid = find_pid(L"ROB RUN.exe");
  if (!pid) { std::printf("[NZL] game not running. Start ROB RUN, enter a match, then rerun.\n"); return 2; }
  uint64_t ga = module_base(pid, L"GameAssembly.dll");
  uint64_t up = module_base(pid, L"UnityPlayer.dll");
  std::printf("[NZL] pid=%lu GameAssembly=0x%llx UnityPlayer=0x%llx\n", pid, (unsigned long long)ga, (unsigned long long)up);
  nzl::DriverMemory mem(pid);
  if (!mem.init()) {
    std::printf("[NZL] Sharedmem driver NOT loaded (HKLM\\SYSTEM\\NaCoreFilter missing). Build nzl\\driver with build_driver.bat, load driver.sys via kdmapper as admin, rerun.\n");
    return 4;
  }
  std::printf("[NZL] driver transport OK. exe base=0x%llx peb=0x%llx\n",
              (unsigned long long)mem.base(), (unsigned long long)mem.peb());
  uint64_t probe = 0;
  bool ok = mem.read(ga, &probe, sizeof(probe));
  std::printf("[NZL] GameAssembly first bytes read: %s (0x%llx)\n", ok ? "OK" : "FAILED - EAC/driver denied", (unsigned long long)probe);
  mem.close();
  if (!ok) return 5;
  std::printf("[NZL] STOP: root pointer is UNKNOWN for updated build. Run docs/EXTRACT.md, provide dump.cs/script.json + hashes, then root will be wired and validated (class name check) before any ESP read.\n");
  return 3;
}
