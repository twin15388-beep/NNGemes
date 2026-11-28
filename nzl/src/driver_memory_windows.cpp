#include "nzl/driver_memory.h"
#ifdef _WIN32
#include "../client/sc_client.h"
namespace nzl {
bool DriverMemory::init() { ready_ = NaInit(); return ready_; }
void DriverMemory::close() { NaClose(); ready_ = false; }
uint64_t DriverMemory::base() const { return ready_ ? NaGetBase(pid_) : 0; }
uint64_t DriverMemory::peb() const { return ready_ ? NaGetPeb(pid_) : 0; }
bool DriverMemory::read(uint64_t addr, void* out, size_t size) const {
  if (!ready_ || !out || size == 0 || size > 0x10000) return false;
  return NaRead(pid_, addr, (UINT32)size, out);
}
}  // namespace nzl
#endif
