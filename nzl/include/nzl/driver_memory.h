#pragma once
// NZL Studio: IMemory over the Sharedmem Driver (nzl/client/sc_client.*).
// Windows only. Driver must already be loaded (kdmapper, admin) and have
// published HKLM\SYSTEM\NaCoreFilter {Key, Shm}; NaInit() reads those.
#include "nzl/memory.h"
#include <cstdint>
namespace nzl {
class DriverMemory final : public IMemory {
 public:
  explicit DriverMemory(uint32_t pid) : pid_(pid) {}
  bool init();          // NaInit(); false = driver not loaded
  void close();         // NaClose()
  uint64_t base() const; // NaGetBase(pid_) - main exe image base
  uint64_t peb() const;  // NaGetPeb(pid_)
  bool read(uint64_t addr, void* out, size_t size) const override;
 private:
  uint32_t pid_ = 0;
  bool ready_ = false;
};
}  // namespace nzl
