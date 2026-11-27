#pragma once
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
namespace nzl {
// NZL Studio own memory abstraction. Windows process reader implements this;
// tests use FakeMemory. No foreign overlay/driver code.
struct IMemory {
  virtual ~IMemory() = default;
  virtual bool read(uint64_t addr, void* out, size_t size) const = 0;
  template<class T> std::optional<T> read_t(uint64_t addr) const { T v{}; if(!read(addr,&v,sizeof(v))) return std::nullopt; return v; }
  std::optional<uint64_t> ptr(uint64_t addr) const { return read_t<uint64_t>(addr); }
  std::optional<float> f32(uint64_t addr) const { return read_t<float>(addr); }
  std::optional<int32_t> i32(uint64_t addr) const { return read_t<int32_t>(addr); }
  std::optional<uint8_t> u8(uint64_t addr) const { return read_t<uint8_t>(addr); }
};
struct FakeMemory final : IMemory {
  std::unordered_map<uint64_t, std::vector<uint8_t>> bytes;
  template<class T> void put(uint64_t addr, T v){ std::vector<uint8_t> b(sizeof(T)); std::memcpy(b.data(),&v,sizeof(T)); bytes[addr]=std::move(b); }
  bool read(uint64_t addr, void* out, size_t size) const override {
    // support reads inside a stored blob; smallest covering blob wins
    const std::vector<uint8_t>* best=nullptr; uint64_t best_base=0;
    for (auto &kv: bytes){ if(addr>=kv.first && addr+size<=kv.first+kv.second.size()){ if(!best || kv.second.size()<best->size()){ best=&kv.second; best_base=kv.first; } } }
    if(!best) return false; std::memcpy(out, best->data()+(addr-best_base), size); return true;
  }
};
bool plausible_ptr(uint64_t p);
} // namespace nzl
