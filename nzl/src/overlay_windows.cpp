// NZL Studio overlay (Windows): persistent interactive menu + background ESP reconnect.
// UI stays responsive while the worker waits for ROB RUN, IL2CPP classes,
// and an active battle chain. The ESP window is click-through; the separate
// draggable menu window receives mouse input and releases game focus/cursor.
// INSERT toggles the menu; END exits.
// Aim: hold RIGHT MOUSE, nearest alive head to crosshair inside Aim FOV.
// Projection basis: profile-selected BattleCameraModel quaternion, with the
// profile camera axes as a sanity/fallback path. HP/armor/role/loot chains stay
// disabled; exact-build car fields use runtime class and position sanity gates
// but still need a live-match check.
#ifndef _WIN32
#error "Windows only"
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <algorithm>
#include <memory>
#include <iterator>
#include <optional>
#include <unordered_map>
#include <windowsx.h>
#include "nzl/chain.h"
#include "nzl/na_klass.h"
#include "nzl/driver_memory.h"
#include "nzl/esp_pipeline.h"
#include "nzl/skeleton.h"
#include "nzl/windows_profile.h"
#include "../client/sc_client.h"

static DWORD find_pid(const wchar_t* name) {
  HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (s == INVALID_HANDLE_VALUE) return 0;
  PROCESSENTRY32W e{}; e.dwSize = sizeof(e); DWORD pid = 0;
  if (Process32FirstW(s, &e)) do { if (_wcsicmp(e.szExeFile, name) == 0) { pid = e.th32ProcessID; break; } } while (Process32NextW(s, &e));
  CloseHandle(s); return pid;
}
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
// The name STRING pointer must not be alignment-checked. IL2CPP interns class
// and namespace names in a byte pool, so name pointers are routinely
// unaligned: the owner's live run gave 0x1F18D039A06 pointing at "Game".
// plausible_ptr() demands 8-byte alignment and rejected it, which turned a
// correct profile into a false NAME_MISMATCH and would have made the overlay
// refuse to draw a valid chain.
static int klass_read_cb(void* ctx, NA_U64 va, void* out, NA_U32 size) {
  return static_cast<const nzl::IMemory*>(ctx)->read(va, out, size) ? 1 : 0;
}
static std::optional<std::string> read_klass_name(const nzl::IMemory& m,
                                                   uint64_t klass,
                                                   uint32_t name_offset) {
  if (!nzl::plausible_ptr(klass) || klass > UINT64_MAX - name_offset)
    return std::nullopt;
  auto name_ptr = m.ptr(klass + name_offset);
  if (!name_ptr) return std::nullopt;
  char name[NA_KLASS_MAX_NAME_BYTES] = {};
  if (!na_klass_read_text(klass_read_cb,
                          const_cast<void*>(static_cast<const void*>(&m)),
                          *name_ptr, name, static_cast<NA_U32>(sizeof(name))))
    return std::nullopt;
  return std::string(name);
}
static bool klass_name_is(const nzl::IMemory& m, uint64_t klass,
                          uint32_t name_offset, const char* want) {
  auto name = read_klass_name(m, klass, name_offset);
  return name && *name == want;
}
static bool object_klass_name_is(
    const nzl::IMemory& m, uint64_t object, uint32_t name_offset,
    const char* want,
    std::unordered_map<uint64_t, std::string>* class_name_cache = nullptr) {
  if (!nzl::plausible_ptr(object)) return false;
  auto klass = m.ptr(object);
  if (!klass || !nzl::plausible_ptr(*klass)) return false;
  if (!class_name_cache) return klass_name_is(m, *klass, name_offset, want);
  const auto found = class_name_cache->find(*klass);
  if (found != class_name_cache->end()) return found->second == want;
  auto name = read_klass_name(m, *klass, name_offset);
  if (!name) return false;
  class_name_cache->emplace(*klass, *name);
  return *name == want;
}
static std::optional<std::wstring> read_string_data(
    const nzl::IMemory& memory, uint64_t string_data,
    const nzl::Offsets& offsets,
    std::unordered_map<uint64_t, std::string>* class_name_cache = nullptr) {
  constexpr uint32_t kMaxNicknameCodeUnits = 64;
  constexpr uint32_t kIl2CppStringLengthOffset = 0x10;
  constexpr uint32_t kIl2CppStringCharsOffset = 0x14;
  if (!offsets.nickname_offsets_configured || !offsets.primitive_value)
    return std::nullopt;
  const bool is_string_data =
      object_klass_name_is(memory, string_data, offsets.il2cpp_class_name,
                           "StringData", class_name_cache) ||
      object_klass_name_is(memory, string_data, offsets.il2cpp_class_name,
                           "NicknameData", class_name_cache);
  if (!is_string_data || string_data > UINT64_MAX - offsets.primitive_value)
    return std::nullopt;

  auto value = memory.ptr(string_data + offsets.primitive_value);
  if (!value) return std::nullopt;
  if (!*value) return std::wstring{};  // NullablePrimitiveData<string> value.
  if (!nzl::plausible_ptr(*value) ||
      !object_klass_name_is(memory, *value, offsets.il2cpp_class_name, "String",
                            class_name_cache) ||
      *value > UINT64_MAX - kIl2CppStringLengthOffset)
    return std::nullopt;

  auto length = memory.i32(*value + kIl2CppStringLengthOffset);
  if (!length || *length < 0 ||
      static_cast<uint32_t>(*length) > kMaxNicknameCodeUnits ||
      *value > UINT64_MAX - kIl2CppStringCharsOffset)
    return std::nullopt;

  std::wstring nickname;
  nickname.reserve(static_cast<size_t>(*length));
  if (*length == 0) return nickname;
  const size_t chars_bytes = static_cast<size_t>(*length) * sizeof(uint16_t);
  const uint64_t chars_address = *value + kIl2CppStringCharsOffset;
  if (chars_address > UINT64_MAX - chars_bytes) return std::nullopt;
  uint16_t chars[kMaxNicknameCodeUnits]{};
  if (!memory.read(chars_address, chars, chars_bytes))
    return std::nullopt;
  for (int32_t i = 0; i < *length; ++i) {
    const uint16_t ch = chars[i];
    // Do not let control characters or embedded NULs corrupt the GDI label.
    nickname.push_back(ch < 0x20 || ch == 0x7f ? L' ' : static_cast<wchar_t>(ch));
  }
  if (nickname.find_first_not_of(L" \t\r\n") == std::wstring::npos)
    return std::wstring{};
  return nickname;
}
struct RenderEnt {
  float hx = 0, hy = 0, fx = 0, fy = 0; // head / feet screen
  float ax = 0, ay = 0;                 // aim point screen (selected bone)
  nzl::Vec3f pos{}; int id = 0; float dist = 0; bool dead = false;
  std::wstring nickname;
  int hp = -1, maxhp = 0; bool armor = false;
  int role = -2; // PlayerRole via description Index: 0 Settler 1 Robber 2 Cop 3 Citizen 4 Prisoner (unverified mapping)
};
struct CarEnt { nzl::Vec3f pos{}; float dist = 0; float sx = 0, sy = 0; bool on = false; };
struct LootEnt { nzl::Vec3f pos{}; float dist = 0; float sx = 0, sy = 0; bool on = false; };
struct Ctx {
  DWORD pid = 0;
  nzl::DriverMemory* mem = nullptr;
  nzl::Offsets off;
  nzl::ChainResult chain;
  uint64_t player_class = 0;
  std::vector<RenderEnt> render;
  RECT dirty{};            // union of this and the previous frame's bounds
  bool dirty_valid = false;
  uint32_t dirty_frames = 0;
  std::vector<LootEnt> loot;
  std::vector<CarEnt> cars;
  std::unordered_map<uint64_t, std::string> class_name_cache;
  nzl::Vec3f cam_fwd{0, 0, 1}, cam_right{1, 0, 0}, radar_cam{};
  float fov = nzl::game_profile::kOwnerProfile26866884.assumed_vertical_fov_deg;
  int width = 1920, height = 1080;
  // UI state is owned by the window thread; worker-published data is protected by g_state_mutex.
  bool menu = false;
  int menu_tab = 0;
  int menu_x = 32, menu_y = 48;
  int hover_tab = -1, hover_option = -1;
  bool hover_close = false;
  bool esp_ready = false;
  DWORD attached_pid = 0;
  std::string status = "Waiting for ROB RUN";
  HWND overlay_wnd = nullptr, menu_wnd = nullptr;
  bool show_dead = false, show_boxes = true, show_skeleton = false,
       show_text = true, show_hp = false, show_armor = false,
       show_loot = false, show_radar = false, aim = false, basis_quat = true;
  bool show_roles = false, show_cars = false, tracers = false, visible_only = false;
  float max_dist = 500.0f, aim_fov = 150.0f, aim_smooth = 5.0f, radar_range = 120.0f;
  int aim_bone = 0;   // 0 Head 1 Chest 2 Pelvis
  int aim_key = 0;    // index into kAimKeys
  int menu_key = 0;   // index into kMenuKeys
};
static const int kAimKeys[] = {VK_RBUTTON, VK_LBUTTON, VK_MENU, VK_SHIFT};
static const char* kAimKeyNames[] = {"RMB", "LMB", "ALT", "SHIFT"};
static const int kMenuKeys[] = {VK_INSERT, VK_DELETE, VK_HOME, VK_F9};
static const char* kMenuKeyNames[] = {"INSERT", "DELETE", "HOME", "F9"};
static const char* kBoneNames[] = {"Head", "Chest", "Pelvis"};
static const char* kRoleNames[] = {"SETTLER", "ROBBER", "COP", "CITIZEN", "PRISONER"};
static std::mutex g_state_mutex;
static std::atomic_bool g_stop{false};
static std::atomic<uint32_t> g_overlay_paint_millifps{0};
struct DictionaryStats {
  uint32_t count = 0, capacity = 0;
  size_t active_values = 0, class_matches = 0, read_failures = 0;
  bool header_valid = false;
};
// Read only initialized Dictionary slots, enforce the profile bounds, and
// optionally retain values whose object header matches a live class pointer.
// Stats separate raw Dictionary.Count/capacity from valid pointers and the
// expected player class so the owner can see where a small roster originates.
static std::vector<uint64_t> dict_values(Ctx& c, uint64_t dict,
                                         uint64_t expected_value_class = 0,
                                         DictionaryStats* stats = nullptr) {
  if (stats) *stats = {};
  std::vector<uint64_t> out;
  if (!c.mem || !c.off.profile_configured || !nzl::plausible_ptr(dict) ||
      dict > UINT64_MAX - c.off.dict_entries ||
      dict > UINT64_MAX - c.off.dict_count) return out;

  const auto entries = c.mem->ptr(dict + c.off.dict_entries);
  const auto count = c.mem->i32(dict + c.off.dict_count);
  if (!entries || !nzl::plausible_ptr(*entries) || !count || *count < 0 ||
      static_cast<uint32_t>(*count) > c.off.max_dict_entries ||
      *entries > UINT64_MAX - c.off.array_length) return out;

  // Il2CppArray.max_length is size_t in the x64 target; do not accept a
  // four-byte truncated capacity as a valid header.
  const auto capacity = c.mem->ptr(*entries + c.off.array_length);
  if (!capacity || *capacity > c.off.max_dict_entries ||
      static_cast<uint64_t>(*count) > *capacity || c.off.dict_stride == 0 ||
      *entries > UINT64_MAX - c.off.array_data) return out;

  if (stats) {
    stats->header_valid = true;
    stats->count = static_cast<uint32_t>(*count);
    stats->capacity = static_cast<uint32_t>(*capacity);
  }
  const uint64_t array_data = *entries + c.off.array_data;
  // The live population is *count, but the slots it occupies span the array
  // capacity. Scanning only *count slots is why players went missing.
  const int total = static_cast<int>(*capacity);
  for (int base = 0; base < total; base += 24) {
    const int n = std::min(24, total - base);
    std::vector<NaItem> items;
    std::vector<uint8_t> buffer(static_cast<size_t>(n) * 16, 0);
    items.reserve(static_cast<size_t>(n) * 2);
    bool addresses_ok = true;
    for (int i = 0; i < n; ++i) {
      const uint64_t index = static_cast<uint64_t>(base + i);
      if (index > (UINT64_MAX - array_data) / c.off.dict_stride) {
        addresses_ok = false;
        break;
      }
      const uint64_t entry = array_data + index * c.off.dict_stride;
      if (entry > UINT64_MAX - c.off.dict_hash ||
          entry > UINT64_MAX - c.off.dict_value) {
        addresses_ok = false;
        break;
      }
      items.push_back(NaItem{entry + c.off.dict_hash,
                             static_cast<uint32_t>(i * 16), 4});
      items.push_back(NaItem{entry + c.off.dict_value,
                             static_cast<uint32_t>(i * 16 + 8), 8});
    }
    if (!addresses_ok ||
        !NaBatchRead(c.pid, items.data(), static_cast<uint32_t>(items.size()),
                     buffer.data(), static_cast<uint32_t>(buffer.size()))) {
      if (stats) ++stats->read_failures;
      continue;
    }

    std::vector<uint64_t> values;
    values.reserve(n);
    for (int i = 0; i < n; ++i) {
      int32_t hash = -1;
      uint64_t value = 0;
      std::memcpy(&hash, buffer.data() + static_cast<size_t>(i) * 16, 4);
      std::memcpy(&value, buffer.data() + static_cast<size_t>(i) * 16 + 8, 8);
      if (hash >= 0 && nzl::plausible_ptr(value)) values.push_back(value);
    }
    if (stats) stats->active_values += values.size();
    if (!expected_value_class) {
      out.insert(out.end(), values.begin(), values.end());
      continue;
    }

    if (values.empty()) continue;
    std::vector<NaItem> class_items;
    std::vector<uint8_t> class_buffer(values.size() * sizeof(uint64_t), 0);
    class_items.reserve(values.size());
    for (size_t i = 0; i < values.size(); ++i)
      class_items.push_back(NaItem{values[i], static_cast<uint32_t>(i * 8), 8});
    if (!NaBatchRead(c.pid, class_items.data(),
                     static_cast<uint32_t>(class_items.size()), class_buffer.data(),
                     static_cast<uint32_t>(class_buffer.size()))) {
      if (stats) ++stats->read_failures;
      continue;
    }
    for (size_t i = 0; i < values.size(); ++i) {
      uint64_t klass = 0;
      std::memcpy(&klass, class_buffer.data() + i * sizeof(klass), sizeof(klass));
      if (klass == expected_value_class) out.push_back(values[i]);
    }
  }
  if (stats) stats->class_matches = out.size();
  return out;
}
struct DictIntEntry { int32_t key = 0; uint64_t value = 0; };
// Read the int-keyed slot dictionary without losing the Entry.key/value pairing.
// This uses the same profile-bounded Dictionary layout as the player snapshot.
static std::vector<DictIntEntry> dict_int_entries(Ctx& c, uint64_t dict) {
  std::vector<DictIntEntry> out;
  if (!c.mem || !c.off.profile_configured || !nzl::plausible_ptr(dict) ||
      dict > UINT64_MAX - c.off.dict_entries ||
      dict > UINT64_MAX - c.off.dict_count) return out;

  auto entries = c.mem->ptr(dict + c.off.dict_entries);
  auto count = c.mem->i32(dict + c.off.dict_count);
  if (!entries || !nzl::plausible_ptr(*entries) || !count || *count < 0 ||
      static_cast<uint32_t>(*count) > c.off.max_dict_entries ||
      *entries > UINT64_MAX - c.off.array_length) return out;
  auto capacity = c.mem->ptr(*entries + c.off.array_length);
  if (!capacity || *capacity > c.off.max_dict_entries ||
      static_cast<uint64_t>(*count) > *capacity || !c.off.dict_stride ||
      *entries > UINT64_MAX - c.off.array_data) return out;

  const uint64_t array_data = *entries + c.off.array_data;
  // Same Count-vs-capacity rule as dict_values: the int-keyed slot dictionary
  // that carries nicknames keeps its live entries anywhere inside the entries
  // array, so scanning *count slots loses the slots that sit past it.
  const int span = static_cast<int>(*capacity);
  // Keep 3 fields per dictionary entry within NaBatchRead's 64-item limit.
  for (int base = 0; base < span; base += 21) {
    const int n = std::min(21, span - base);
    std::vector<NaItem> items;
    std::vector<uint8_t> buffer(static_cast<size_t>(n) * 16, 0);
    items.reserve(static_cast<size_t>(n) * 3);
    bool addresses_ok = true;
    for (int i = 0; i < n; ++i) {
      const uint64_t index = static_cast<uint64_t>(base + i);
      if (index > (UINT64_MAX - array_data) / c.off.dict_stride) {
        addresses_ok = false;
        break;
      }
      const uint64_t entry = array_data + index * c.off.dict_stride;
      if (entry > UINT64_MAX - c.off.dict_hash ||
          entry > UINT64_MAX - c.off.dict_key ||
          entry > UINT64_MAX - c.off.dict_value) {
        addresses_ok = false;
        break;
      }
      items.push_back(NaItem{entry + c.off.dict_hash,
                             static_cast<uint32_t>(i * 16), 4});
      items.push_back(NaItem{entry + c.off.dict_key,
                             static_cast<uint32_t>(i * 16 + 4), 4});
      items.push_back(NaItem{entry + c.off.dict_value,
                             static_cast<uint32_t>(i * 16 + 8), 8});
    }
    if (!addresses_ok ||
        !NaBatchRead(c.pid, items.data(), static_cast<uint32_t>(items.size()),
                     buffer.data(), static_cast<uint32_t>(buffer.size())))
      continue;

    for (int i = 0; i < n; ++i) {
      int32_t hash = -1, key = 0;
      uint64_t value = 0;
      const uint8_t* row = buffer.data() + static_cast<size_t>(i) * 16;
      std::memcpy(&hash, row, sizeof(hash));
      std::memcpy(&key, row + 4, sizeof(key));
      std::memcpy(&value, row + 8, sizeof(value));
      if (hash >= 0 && nzl::plausible_ptr(value)) out.push_back({key, value});
    }
  }
  return out;
}
struct BatchField { uint64_t address = 0; uint32_t size = 0; };
// The nickname chain is six gates deep and used to fail completely silently,
// which is why "нет имён" survived four reports: a wrong slot offset and an
// empty store looked identical. Every exit names itself now.
enum class NameStage {
  Disabled, BattleModel, Slots, Dictionary, Entries, SlotClass, SlotData,
  DataClass, EntityId, IdMismatch, Nickname, Ok
};
static const char* name_stage_text(NameStage stage) {
  switch (stage) {
    case NameStage::Ok: return "OK";
    case NameStage::BattleModel: return "battle-model-gate";
    case NameStage::Slots: return "character-slots-gate";
    case NameStage::Dictionary: return "slot-dictionary-gate";
    case NameStage::Entries: return "slot-dictionary-empty";
    case NameStage::SlotClass: return "slot-class-gate";
    case NameStage::SlotData: return "slot-data-gate";
    case NameStage::DataClass: return "slot-data-class-gate";
    case NameStage::EntityId: return "entity-id-gate";
    case NameStage::IdMismatch: return "entity-id-mismatch";
    case NameStage::Nickname: return "nickname-object-gate";
    default: return "offsets-unconfigured";
  }
}
struct PlayerCacheEntry {
  uint64_t character = 0, transform = 0, private_model = 0, armor_prim = 0, role_prim = 0;
  uint64_t hp_value_addr = 0, hp_max_addr = 0, armor_value_addr = 0, role_index_addr = 0;
  nzl::Vec3f pos{}; int32_t id = 0, hp = -1, maxhp = 0, role = -2; bool dead = false, armor = false;
  std::wstring nickname;
  nzl::SkeletonSnapshot skeleton;
  nzl::SkeletonReadStage skeleton_stage = nzl::SkeletonReadStage::ProfileDisabled;
  bool skeleton_valid = false;
};
struct PlayerCache {
  std::vector<PlayerCacheEntry> entries;
  size_t transform_repoints = 0, transform_refresh_failures = 0;
  NameStage name_stage = NameStage::Disabled;
  uint64_t name_slots_root = 0;      // resolved ClientCharacterSlotsModel route
  uint32_t name_slots_offset = 0, name_dict_offset = 0;
  size_t transform_repoints = 0, transform_refresh_failures = 0;
  uint32_t dictionary_count = 0, dictionary_capacity = 0;
  size_t dictionary_active_values = 0, dictionary_read_failures = 0;
  bool dictionary_header_valid = false;
  size_t skeleton_attempted = 0, skeleton_model_matches = 0;
  size_t skeleton_description_matches = 0, skeleton_valid = 0;
  int32_t skeleton_bone_count = 0, skeleton_head1_index = -1;
  nzl::SkeletonReadStage skeleton_last_stage = nzl::SkeletonReadStage::ProfileDisabled;
  ULONGLONG last_roster_check = 0, last_record_refresh = 0, last_meta_refresh = 0;
  ULONGLONG last_nickname_refresh = 0, last_skeleton_refresh = 0;
};
// Read up to 64 independent fields per driver command, preserving one
// output slot per request. Player pointer chains are refreshed separately
// from the per-frame position batch below.
static std::vector<uint64_t> batch_read_fields(uint32_t pid, const std::vector<BatchField>& fields) {
  std::vector<uint64_t> values(fields.size(), 0);
  for (size_t base = 0; base < fields.size(); base += 64) {
    uint32_t n = (uint32_t)std::min<size_t>(64, fields.size() - base);
    std::vector<NaItem> items; items.reserve(n);
    std::vector<uint8_t> buf((size_t)n * 8, 0);
    for (uint32_t i = 0; i < n; ++i) {
      const auto& f = fields[base + i];
      items.push_back(NaItem{f.address, i * 8, f.address ? f.size : 0});
    }
    if (!NaBatchRead(pid, items.data(), n, buf.data(), n * 8)) continue;
    for (uint32_t i = 0; i < n; ++i) std::memcpy(&values[base + i], buf.data() + i * 8, 8);
  }
  return values;
}
// Optional position reader. All field members come from the selected profile;
// the current profile leaves loot XYZ unknown and keeps this path disabled.
static void fill_positions(Ctx& c, const std::vector<uint64_t>& objs,
                           uint32_t transform_offset, uint32_t x_offset,
                           uint32_t y_offset, uint32_t z_offset,
                           std::vector<nzl::Vec3f>& positions) {
  const size_t count = objs.size();
  positions.assign(count, {});
  std::vector<uint64_t> transforms(count, 0);
  for (size_t base = 0; base < count; base += 32) {
    const uint32_t n = static_cast<uint32_t>(std::min<size_t>(32, count - base));
    std::vector<NaItem> items;
    std::vector<uint8_t> buffer(static_cast<size_t>(n) * 8, 0);
    items.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
      const uint64_t object = objs[base + i];
      const bool valid = nzl::plausible_ptr(object) &&
                         object <= UINT64_MAX - transform_offset;
      items.push_back(NaItem{valid ? object + transform_offset : 0,
                             i * 8, valid ? 8u : 0u});
    }
    if (!NaBatchRead(c.pid, items.data(), n, buffer.data(),
                     static_cast<uint32_t>(buffer.size()))) continue;
    for (uint32_t i = 0; i < n; ++i)
      std::memcpy(&transforms[base + i], buffer.data() + i * 8, 8);
  }

  for (size_t base = 0; base < count; base += 21) {
    const uint32_t n = static_cast<uint32_t>(std::min<size_t>(21, count - base));
    std::vector<NaItem> items;
    std::vector<uint8_t> buffer(static_cast<size_t>(n) * 12, 0);
    items.reserve(static_cast<size_t>(n) * 3);
    for (uint32_t i = 0; i < n; ++i) {
      const uint64_t transform = transforms[base + i];
      const uint32_t offsets[] = {x_offset, y_offset, z_offset};
      for (uint32_t axis = 0; axis < 3; ++axis) {
        const bool valid = nzl::plausible_ptr(transform) &&
                           transform <= UINT64_MAX - offsets[axis];
        items.push_back(NaItem{valid ? transform + offsets[axis] : 0,
                               i * 12 + axis * 4, valid ? 4u : 0u});
      }
    }
    if (!NaBatchRead(c.pid, items.data(), static_cast<uint32_t>(items.size()),
                     buffer.data(), static_cast<uint32_t>(buffer.size()))) continue;
    for (uint32_t i = 0; i < n; ++i) {
      float xyz[3]{};
      std::memcpy(xyz, buffer.data() + static_cast<size_t>(i) * 12, 12);
      positions[base + i] = {xyz[0], xyz[1], xyz[2]};
    }
  }
}
static bool same_roster(const PlayerCache& cache, const std::vector<uint64_t>& chars) {
  if (cache.entries.size() != chars.size()) return false;
  for (size_t i = 0; i < chars.size(); ++i) if (cache.entries[i].character != chars[i]) return false;
  return true;
}
static bool refresh_player_records(Ctx& c, PlayerCache& cache) {
  const size_t n = cache.entries.size();
  struct RecordIndices {
    int id = -1, transform = -1, dead = -1;
    int private_model = -1, armor_primitive = -1, role_primitive = -1;
  };
  std::vector<RecordIndices> indices(n);
  std::vector<BatchField> fields;
  auto add_pointer_field = [&](uint64_t base, uint32_t offset, uint32_t size,
                               int& index) {
    if (!nzl::plausible_ptr(base) || base > UINT64_MAX - offset) return;
    index = static_cast<int>(fields.size());
    fields.push_back({base + offset, size});
  };
  const bool read_private_model = c.off.hp_live_verified;
  for (size_t i = 0; i < n; ++i) {
    auto& e = cache.entries[i];
    add_pointer_field(e.character, c.off.char_id, 4, indices[i].id);
    add_pointer_field(e.character, c.off.char_transform, 8, indices[i].transform);
    add_pointer_field(e.character, c.off.char_is_dead, 1, indices[i].dead);
    if (read_private_model)
      add_pointer_field(e.character, c.off.character_private_model, 8,
                        indices[i].private_model);
    if (c.off.armor_live_verified)
      add_pointer_field(e.character, c.off.armor_primitive, 8,
                        indices[i].armor_primitive);
    if (c.off.role_live_verified)
      add_pointer_field(e.character, c.off.role_primitive, 8,
                        indices[i].role_primitive);
    e.hp_value_addr = e.hp_max_addr = e.armor_value_addr = e.role_index_addr = 0;
    e.hp = -1; e.maxhp = 0; e.armor = false; e.role = -2;
  }
  auto record_values = batch_read_fields(c.pid, fields);
  auto field_value = [&](int index) -> uint64_t {
    return index >= 0 && static_cast<size_t>(index) < record_values.size()
               ? record_values[static_cast<size_t>(index)]
               : 0;
  };
  for (size_t i = 0; i < n; ++i) {
    auto& e = cache.entries[i];
    const int32_t id = static_cast<int32_t>(static_cast<uint32_t>(field_value(indices[i].id)));
    if (e.id != id) e.nickname.clear();
    e.id = id;
    e.transform = field_value(indices[i].transform);
    e.dead = field_value(indices[i].dead) != 0;
    e.private_model = field_value(indices[i].private_model);
    e.armor_prim = field_value(indices[i].armor_primitive);
    e.role_prim = field_value(indices[i].role_primitive);
  }

  // Optional nested chains are not read unless the profile explicitly marks
  // that feature live-verified. The current 26866884 profile keeps these flags
  // false, so player ESP only consumes the core ID/Transform/IsDead fields.
  fields.clear();
  std::vector<int> hp_model_i(n, -1), armor_data_i(n, -1), role_data_i(n, -1);
  for (size_t i = 0; i < n; ++i) {
    const auto& e = cache.entries[i];
    if (c.off.hp_live_verified)
      add_pointer_field(e.private_model, c.off.health_model, 8, hp_model_i[i]);
    if (c.off.armor_live_verified)
      add_pointer_field(e.armor_prim, c.off.primitive_data, 8, armor_data_i[i]);
    if (c.off.role_live_verified)
      add_pointer_field(e.role_prim, c.off.primitive_data, 8, role_data_i[i]);
  }
  const auto first_chain_values = batch_read_fields(c.pid, fields);
  auto first_value = [&](int index) -> uint64_t {
    return index >= 0 && static_cast<size_t>(index) < first_chain_values.size()
               ? first_chain_values[static_cast<size_t>(index)]
               : 0;
  };
  std::vector<uint64_t> hp_model(n), armor_data(n), role_data(n);
  for (size_t i = 0; i < n; ++i) {
    hp_model[i] = first_value(hp_model_i[i]);
    armor_data[i] = first_value(armor_data_i[i]);
    role_data[i] = first_value(role_data_i[i]);
  }

  fields.clear();
  std::vector<int> hp_data_i(n, -1), role_desc_i(n, -1);
  for (size_t i = 0; i < n; ++i) {
    if (c.off.hp_live_verified && nzl::plausible_ptr(hp_model[i])) {
      add_pointer_field(hp_model[i], c.off.health_data, 8, hp_data_i[i]);
      if (hp_model[i] <= UINT64_MAX - c.off.health_max_value)
        cache.entries[i].hp_max_addr = hp_model[i] + c.off.health_max_value;
    }
    if (c.off.armor_live_verified && nzl::plausible_ptr(armor_data[i]) &&
        armor_data[i] <= UINT64_MAX - c.off.primitive_value)
      cache.entries[i].armor_value_addr = armor_data[i] + c.off.primitive_value;
    if (c.off.role_live_verified)
      add_pointer_field(role_data[i], c.off.role_description, 8, role_desc_i[i]);
  }
  const auto second_chain_values = batch_read_fields(c.pid, fields);
  auto second_value = [&](int index) -> uint64_t {
    return index >= 0 && static_cast<size_t>(index) < second_chain_values.size()
               ? second_chain_values[static_cast<size_t>(index)]
               : 0;
  };
  for (size_t i = 0; i < n; ++i) {
    const uint64_t health_data = second_value(hp_data_i[i]);
    const uint64_t role_description = second_value(role_desc_i[i]);
    if (c.off.hp_live_verified && nzl::plausible_ptr(health_data) &&
        health_data <= UINT64_MAX - c.off.health_value)
      cache.entries[i].hp_value_addr = health_data + c.off.health_value;
    if (c.off.role_live_verified && nzl::plausible_ptr(role_description) &&
        role_description <= UINT64_MAX - c.off.role_index)
      cache.entries[i].role_index_addr = role_description + c.off.role_index;
  }
  return true;
}
// Reading DoTransform through a pointer that was cached up to three seconds
// ago is what makes boxes sit where a player used to be and then snap: the
// game repoints Transform when objects are pooled or a character re-enters the
// world. Refresh the pointer in the same frame that it is used, in one extra
// batch command, so a position is never read through a stale pointer.
static void refresh_transform_pointers(Ctx& c, PlayerCache& cache) {
  const size_t count = cache.entries.size();
  if (count == 0 || c.off.char_transform == 0) return;
  constexpr size_t kPointerChunk = 60;
  for (size_t base = 0; base < count; base += kPointerChunk) {
    const uint32_t n = static_cast<uint32_t>(
        std::min(kPointerChunk, count - base));
    std::vector<NaItem> items;
    items.reserve(n);
    std::vector<uint8_t> buffer(static_cast<size_t>(n) * 8, 0);
    for (uint32_t i = 0; i < n; ++i) {
      const uint64_t character = cache.entries[base + i].character;
      const bool valid = nzl::plausible_ptr(character) &&
                         character <= UINT64_MAX - c.off.char_transform;
      items.push_back(NaItem{valid ? character + c.off.char_transform : 0,
                             i * 8, valid ? 8u : 0u});
    }
    if (!NaBatchRead(c.pid, items.data(), static_cast<uint32_t>(items.size()),
                     buffer.data(), static_cast<uint32_t>(buffer.size()))) {
      ++cache.transform_refresh_failures;
      continue;
    }
    for (uint32_t i = 0; i < n; ++i) {
      uint64_t transform = 0;
      std::memcpy(&transform, buffer.data() + static_cast<size_t>(i) * 8, 8);
      if (!nzl::plausible_ptr(transform)) continue;
      auto& entry = cache.entries[base + i];
      if (entry.transform && entry.transform != transform)
        ++cache.transform_repoints;
      entry.transform = transform;
    }
  }
}
static void refresh_player_positions(Ctx& c, PlayerCache& cache) {
  const size_t count = cache.entries.size();
  const bool xyz_contiguous = c.off.ctm_x <= UINT32_MAX - 8 &&
                              c.off.ctm_y == c.off.ctm_x + 4 &&
                              c.off.ctm_z == c.off.ctm_x + 8;
  const size_t chunk_size = xyz_contiguous ? 64 : 21;
  for (size_t base = 0; base < count; base += chunk_size) {
    const uint32_t n = static_cast<uint32_t>(
        std::min<size_t>(chunk_size, count - base));
    std::vector<NaItem> items;
    std::vector<uint8_t> buffer(static_cast<size_t>(n) * 12, 0);
    items.reserve(xyz_contiguous ? n : static_cast<size_t>(n) * 3);
    for (uint32_t i = 0; i < n; ++i) {
      const uint64_t transform = cache.entries[base + i].transform;
      const uint32_t offsets[] = {c.off.ctm_x, c.off.ctm_y, c.off.ctm_z};
      const uint32_t axes = xyz_contiguous ? 1 : 3;
      for (uint32_t axis = 0; axis < axes; ++axis) {
        const uint32_t field_offset = xyz_contiguous ? c.off.ctm_x : offsets[axis];
        const uint32_t bytes = xyz_contiguous ? 12 : 4;
        const bool valid = nzl::plausible_ptr(transform) &&
                           transform <= UINT64_MAX - field_offset;
        items.push_back(NaItem{valid ? transform + field_offset : 0,
                               i * 12 + (xyz_contiguous ? 0 : axis * 4),
                               valid ? bytes : 0u});
      }
    }
    if (!NaBatchRead(c.pid, items.data(), static_cast<uint32_t>(items.size()),
                     buffer.data(), static_cast<uint32_t>(buffer.size()))) continue;
    for (uint32_t i = 0; i < n; ++i) {
      float xyz[3]{};
      std::memcpy(xyz, buffer.data() + static_cast<size_t>(i) * 12, 12);
      cache.entries[base + i].pos = {xyz[0], xyz[1], xyz[2]};
    }
  }
}
static void refresh_player_metadata(Ctx& c, PlayerCache& cache) {
  std::vector<BatchField> fields;
  struct Indices { int hp = -1, maxhp = -1, armor = -1, role = -1, dead = -1; };
  std::vector<Indices> ix(cache.entries.size());
  for (size_t i = 0; i < cache.entries.size(); ++i) {
    auto& e = cache.entries[i];
    if (c.show_hp && c.off.hp_live_verified && e.hp_value_addr) {
      ix[i].hp = static_cast<int>(fields.size()); fields.push_back({e.hp_value_addr, 4});
    }
    if (c.show_hp && c.off.hp_live_verified && e.hp_max_addr) {
      ix[i].maxhp = static_cast<int>(fields.size()); fields.push_back({e.hp_max_addr, 4});
    }
    if (c.show_armor && c.off.armor_live_verified && e.armor_value_addr) {
      ix[i].armor = static_cast<int>(fields.size()); fields.push_back({e.armor_value_addr, 1});
    }
    if (c.show_roles && c.off.role_live_verified && e.role_index_addr) {
      ix[i].role = static_cast<int>(fields.size()); fields.push_back({e.role_index_addr, 4});
    }
    if (e.character <= UINT64_MAX - c.off.char_is_dead) {
      ix[i].dead = static_cast<int>(fields.size());
      fields.push_back({e.character + c.off.char_is_dead, 1});
    }
  }
  auto values = batch_read_fields(c.pid, fields);
  for (size_t i = 0; i < cache.entries.size(); ++i) {
    auto& e = cache.entries[i]; const auto& x = ix[i];
    if (x.hp >= 0) { int32_t v = (int32_t)(uint32_t)values[x.hp]; e.hp = (v >= 0 && v <= 100000) ? v : -1; }
    else e.hp = -1;
    if (x.maxhp >= 0) { int32_t v = (int32_t)(uint32_t)values[x.maxhp]; e.maxhp = (v > 0 && v <= 100000) ? v : 0; }
    else e.maxhp = 0;
    e.armor = x.armor >= 0 && values[x.armor] != 0;
    e.role = (x.role >= 0 && values[x.role] <= 4) ? (int32_t)values[x.role] : -2;
    e.dead = x.dead >= 0 && static_cast<size_t>(x.dead) < values.size() &&
             values[static_cast<size_t>(x.dead)] != 0;
  }
}
static void refresh_player_nicknames(Ctx& c, PlayerCache& cache) {
  if (!c.mem || !c.off.profile_configured || !c.off.nickname_offsets_configured) {
    cache.name_stage = NameStage::Disabled;
    return;
  }

  // Different public dumps disagree about where the character slot model
  // lives: the SDK dump says BattleModel+0x248, the 2026-11-28 class dump says
  // BattleEntitiesModel+0x80. Neither is trusted on faith - each candidate is
  // read and accepted only if the object it points at really is a
  // ClientCharacterSlotsModel. The winner is remembered until it stops
  // validating, so the scan costs a handful of reads once per match.
  auto gated_slots = [&](uint64_t root, uint32_t offset) -> uint64_t {
    if (!nzl::plausible_ptr(root) || root > UINT64_MAX - offset) return 0;
    const auto pointer = c.mem->ptr(root + offset);
    if (!pointer || !nzl::plausible_ptr(*pointer)) return 0;
    if (!object_klass_name_is(*c.mem, *pointer, c.off.il2cpp_class_name,
                              "ClientCharacterSlotsModel", &c.class_name_cache))
      return 0;
    return *pointer;
  };
  uint64_t slot_model = 0;
  if (cache.name_slots_root)
    slot_model = gated_slots(cache.name_slots_root, cache.name_slots_offset);
  if (!slot_model) {
    const uint64_t roots[] = {c.chain.battle_entities, c.chain.battle_model,
                              c.chain.world_entities};
    const uint32_t offsets[] = {0x80, c.off.battle_model_character_slots, 0x18,
                                0x10, 0x20};
    for (uint64_t root : roots) {
      if (slot_model) break;
      for (uint32_t offset : offsets) {
        if (offset == 0) continue;
        if ((slot_model = gated_slots(root, offset)) != 0) {
          cache.name_slots_root = root;
          cache.name_slots_offset = offset;
          break;
        }
      }
    }
  }
  if (!slot_model) {
    cache.name_stage = NameStage::Slots;
    return;
  }

  // Same reasoning one level down: take the first field of the slot model that
  // actually holds a non-empty int-keyed dictionary.
  std::vector<DictIntEntry> entries;
  if (cache.name_dict_offset) {
    const auto dict = c.mem->ptr(slot_model + cache.name_dict_offset);
    if (dict && nzl::plausible_ptr(*dict))
      entries = dict_int_entries(c, *dict);
  }
  if (entries.empty()) {
    const uint32_t dict_offsets[] = {c.off.client_slots_by_entity_id, 0x18, 0x10,
                                     0x20, 0x28};
    for (uint32_t offset : dict_offsets) {
      if (offset == 0 || slot_model > UINT64_MAX - offset) continue;
      const auto dict = c.mem->ptr(slot_model + offset);
      if (!dict || !nzl::plausible_ptr(*dict)) continue;
      auto candidate = dict_int_entries(c, *dict);
      if (candidate.empty()) continue;
      entries = std::move(candidate);
      cache.name_dict_offset = offset;
      break;
    }
  }
  if (entries.empty()) {
    cache.name_stage = NameStage::Entries;
    return;
  }
  std::unordered_map<int32_t, uint64_t> slot_by_entity_id;
  slot_by_entity_id.reserve(entries.size());
  for (const auto& entry : entries) slot_by_entity_id.emplace(entry.key, entry.value);

  size_t named = 0;
  NameStage worst = NameStage::Ok;
  auto fail = [&](NameStage stage) {
    if (worst == NameStage::Ok) worst = stage;
  };
  for (auto& player : cache.entries) {
    if (player.id < 0 || !nzl::plausible_ptr(player.character) ||
        player.character > UINT64_MAX - c.off.char_id) continue;
    auto live_id = c.mem->i32(player.character + c.off.char_id);
    if (!live_id || *live_id != player.id) continue;
    const auto found = slot_by_entity_id.find(player.id);
    if (found == slot_by_entity_id.end()) { fail(NameStage::SlotClass); continue; }
    if (!object_klass_name_is(*c.mem, found->second, c.off.il2cpp_class_name,
                              "BattleCharacterSlotModel", &c.class_name_cache) ||
        found->second > UINT64_MAX - c.off.character_slot_data) {
      fail(NameStage::SlotClass);
      continue;
    }
    auto data = c.mem->ptr(found->second + c.off.character_slot_data);
    if (!data || !nzl::plausible_ptr(*data) ||
        *data > UINT64_MAX - c.off.character_slot_entity_id ||
        *data > UINT64_MAX - c.off.character_slot_nickname) {
      fail(NameStage::SlotData);
      continue;
    }
    if (!object_klass_name_is(*c.mem, *data, c.off.il2cpp_class_name,
                              "BattleCharacterSlotData", &c.class_name_cache)) {
      fail(NameStage::DataClass);
      continue;
    }
    auto entity_id_data = c.mem->ptr(*data + c.off.character_slot_entity_id);
    if (!entity_id_data || !nzl::plausible_ptr(*entity_id_data) ||
        *entity_id_data > UINT64_MAX - c.off.primitive_value) {
      fail(NameStage::EntityId);
      continue;
    }
    if (!object_klass_name_is(*c.mem, *entity_id_data, c.off.il2cpp_class_name,
                              "IntData", &c.class_name_cache)) {
      fail(NameStage::EntityId);
      continue;
    }
    auto slot_entity_id = c.mem->i32(*entity_id_data + c.off.primitive_value);
    if (!slot_entity_id || *slot_entity_id != player.id) {
      fail(NameStage::IdMismatch);
      continue;
    }
    auto nickname_data = c.mem->ptr(*data + c.off.character_slot_nickname);
    if (!nickname_data || !nzl::plausible_ptr(*nickname_data)) {
      fail(NameStage::Nickname);
      continue;
    }
    auto nickname = read_string_data(*c.mem, *nickname_data, c.off,
                                     &c.class_name_cache);
    if (!nickname) { fail(NameStage::Nickname); continue; }
    player.nickname = std::move(*nickname);
    ++named;
  }
  cache.name_stage = named ? NameStage::Ok : worst;
}
static void refresh_player_skeletons(Ctx& c, PlayerCache& cache,
                                     ULONGLONG now) {
  cache.skeleton_attempted = 0;
  cache.skeleton_model_matches = 0;
  cache.skeleton_description_matches = 0;
  cache.skeleton_valid = 0;
  cache.skeleton_bone_count = 0;
  cache.skeleton_head1_index = -1;
  cache.skeleton_last_stage = c.off.skeleton_offsets_configured
                                  ? nzl::SkeletonReadStage::Ok
                                  : nzl::SkeletonReadStage::ProfileDisabled;

  for (auto& player : cache.entries) {
    player.skeleton = {};
    player.skeleton_valid = false;
    player.skeleton_stage = cache.skeleton_last_stage;
  }
  if (!c.off.skeleton_offsets_configured || !c.mem) {
    if (!c.mem && c.off.skeleton_offsets_configured)
      cache.skeleton_last_stage = nzl::SkeletonReadStage::ModelPointer;
    cache.last_skeleton_refresh = now;
    return;
  }

  for (auto& player : cache.entries) {
    ++cache.skeleton_attempted;
    if (!nzl::plausible_ptr(player.character) ||
        player.character > UINT64_MAX - c.off.skeleton_character_model) {
      player.skeleton_stage = nzl::SkeletonReadStage::ModelPointer;
      cache.skeleton_last_stage = player.skeleton_stage;
      continue;
    }
    const auto model = c.mem->ptr(player.character + c.off.skeleton_character_model);
    if (!model || !nzl::plausible_ptr(*model)) {
      player.skeleton_stage = nzl::SkeletonReadStage::ModelPointer;
      cache.skeleton_last_stage = player.skeleton_stage;
      continue;
    }
    if (!object_klass_name_is(*c.mem, *model, c.off.il2cpp_class_name,
                              "SkeletonModel", &c.class_name_cache)) {
      player.skeleton_stage = nzl::SkeletonReadStage::ModelClassMismatch;
      cache.skeleton_last_stage = player.skeleton_stage;
      continue;
    }
    ++cache.skeleton_model_matches;
    if (*model > UINT64_MAX - c.off.skeleton_model_description) {
      player.skeleton_stage = nzl::SkeletonReadStage::DescriptionPointer;
      cache.skeleton_last_stage = player.skeleton_stage;
      continue;
    }
    const auto description =
        c.mem->ptr(*model + c.off.skeleton_model_description);
    if (!description || !nzl::plausible_ptr(*description)) {
      player.skeleton_stage = nzl::SkeletonReadStage::DescriptionPointer;
      cache.skeleton_last_stage = player.skeleton_stage;
      continue;
    }
    if (!object_klass_name_is(*c.mem, *description, c.off.il2cpp_class_name,
                              "SkeletonConfig", &c.class_name_cache)) {
      player.skeleton_stage = nzl::SkeletonReadStage::DescriptionClassMismatch;
      cache.skeleton_last_stage = player.skeleton_stage;
      continue;
    }
    ++cache.skeleton_description_matches;

    auto result = nzl::read_skeleton_snapshot(*c.mem, player.character, c.off);
    player.skeleton_stage = result.stage;
    if (!result.ok()) {
      cache.skeleton_last_stage = result.stage;
      continue;
    }
    player.skeleton = std::move(result.snapshot);
    player.skeleton_valid = true;
    ++cache.skeleton_valid;
    if (cache.skeleton_bone_count == 0) {
      cache.skeleton_bone_count = player.skeleton.bones_count;
      cache.skeleton_head1_index = player.skeleton.head1_index;
    }
  }
  if (cache.skeleton_valid == cache.skeleton_attempted)
    cache.skeleton_last_stage = nzl::SkeletonReadStage::Ok;
  cache.last_skeleton_refresh = now;
}
static void clear_player_skeletons(PlayerCache& cache) {
  for (auto& player : cache.entries) {
    player.skeleton = {};
    player.skeleton_valid = false;
    player.skeleton_stage = nzl::SkeletonReadStage::ProfileDisabled;
  }
  cache.skeleton_attempted = 0;
  cache.skeleton_model_matches = 0;
  cache.skeleton_description_matches = 0;
  cache.skeleton_valid = 0;
  cache.skeleton_bone_count = 0;
  cache.skeleton_head1_index = -1;
  cache.skeleton_last_stage = nzl::SkeletonReadStage::ProfileDisabled;
  cache.last_skeleton_refresh = 0;
}
static std::vector<nzl::PlayerSnap> snapshot_players(Ctx& c, const nzl::Vec3f& local,
                                                      PlayerCache& cache, ULONGLONG now) {
  if (!cache.last_roster_check || now - cache.last_roster_check >= 1000) {
    DictionaryStats stats;
    auto chars = dict_values(c, c.chain.dict, c.player_class, &stats);
    cache.dictionary_header_valid = stats.header_valid;
    cache.dictionary_count = stats.count;
    cache.dictionary_capacity = stats.capacity;
    cache.dictionary_active_values = stats.active_values;
    cache.dictionary_read_failures = stats.read_failures;
    bool changed = !same_roster(cache, chars);
    if (changed) {
      cache.entries.clear(); cache.entries.reserve(chars.size());
      for (uint64_t ch : chars) { PlayerCacheEntry e; e.character = ch; cache.entries.push_back(e); }
      cache.last_record_refresh = 0;
      cache.last_nickname_refresh = 0;
      cache.last_skeleton_refresh = 0;
    }
    cache.last_roster_check = now;
  }
  if (cache.entries.empty()) {
    clear_player_skeletons(cache);
    if (c.show_skeleton && c.off.skeleton_offsets_configured)
      cache.skeleton_last_stage = nzl::SkeletonReadStage::Ok;
    return {};
  }
  if ((!cache.last_record_refresh || now - cache.last_record_refresh >= 3000) &&
      refresh_player_records(c, cache)) {
    cache.last_record_refresh = now;
    cache.last_meta_refresh = 0;
  }
  if (!cache.last_nickname_refresh || now - cache.last_nickname_refresh >= 1000) {
    refresh_player_nicknames(c, cache);
    cache.last_nickname_refresh = now;
  }
  refresh_transform_pointers(c, cache);
  refresh_player_positions(c, cache);
  if (!cache.last_meta_refresh || now - cache.last_meta_refresh >= 250) {
    refresh_player_metadata(c, cache);
    cache.last_meta_refresh = now;
  }
  if (c.show_skeleton &&
      (!cache.last_skeleton_refresh || now - cache.last_skeleton_refresh >= 1000)) {
    refresh_player_skeletons(c, cache, now);
  } else if (!c.show_skeleton && cache.last_skeleton_refresh) {
    clear_player_skeletons(cache);
  }
  std::vector<nzl::PlayerSnap> out; out.reserve(cache.entries.size());
  for (const auto& e : cache.entries) {
    const nzl::Vec3f& p = e.pos;
    if ((p.x == 0 && p.y == 0 && p.z == 0) || !std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) continue;
    nzl::PlayerSnap s; s.addr = e.character; s.id = e.id; s.dead = e.dead; s.pos = p;
    float dx = p.x - local.x, dy = p.y - local.y, dz = p.z - local.z;
    s.dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    s.hp = e.hp; s.maxhp = e.maxhp; s.armor = e.armor; s.role = e.role;
    out.push_back(s);
  }
  return out;
}
static std::vector<LootEnt> snapshot_loot(Ctx& c, const nzl::Vec3f& local) {
  std::vector<LootEnt> out;
  if (!c.off.loot_live_verified || !c.off.loot_positions_configured ||
      !c.mem || !c.off.loot_dictionary || !c.off.loot_transform ||
      c.chain.world_entities > UINT64_MAX - c.off.loot_dictionary)
    return out;
  auto dict = c.mem->ptr(c.chain.world_entities + c.off.loot_dictionary);
  if (!dict || !nzl::plausible_ptr(*dict)) return out;
  const auto objects = dict_values(c, *dict);
  std::vector<nzl::Vec3f> positions;
  fill_positions(c, objects, c.off.loot_transform, c.off.loot_position_x,
                 c.off.loot_position_y, c.off.loot_position_z, positions);
  for (const auto& position : positions) {
    if ((position.x == 0 && position.y == 0 && position.z == 0) ||
        !std::isfinite(position.x) || !std::isfinite(position.y) ||
        !std::isfinite(position.z)) continue;
    const float dx = position.x - local.x;
    const float dy = position.y - local.y;
    const float dz = position.z - local.z;
    LootEnt entity;
    entity.pos = position;
    entity.dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    out.push_back(entity);
  }
  return out;
}
// The car chain is enabled only for the matching build profile. The SDK offsets
// are not live-verified; reject objects unless both model classes and resulting
// world coordinates pass runtime checks.
static std::vector<CarEnt> snapshot_cars(Ctx& c, const nzl::Vec3f& local) {
  std::vector<CarEnt> out;
  if (!c.off.cars_profile_supported || !c.mem || !c.off.cars_dictionary ||
      !c.off.car_transform || !c.off.car_position ||
      !nzl::plausible_ptr(c.chain.world_entities) ||
      c.chain.world_entities > UINT64_MAX - c.off.cars_dictionary ||
      !object_klass_name_is(*c.mem, c.chain.world_entities, c.off.il2cpp_class_name,
                            "WorldEntitiesModel", &c.class_name_cache))
    return out;
  auto dict = c.mem->ptr(c.chain.world_entities + c.off.cars_dictionary);
  if (!dict || !nzl::plausible_ptr(*dict)) return out;
  const auto all_objects = dict_values(c, *dict);
  std::vector<uint64_t> objects;
  objects.reserve(all_objects.size());
  for (uint64_t object : all_objects) {
    if (object_klass_name_is(*c.mem, object, c.off.il2cpp_class_name, "CarModel",
                             &c.class_name_cache))
      objects.push_back(object);
  }
  const size_t count = objects.size();
  std::vector<uint64_t> transforms(count, 0);
  std::vector<nzl::Vec3f> positions(count, nzl::Vec3f{});
  for (size_t base = 0; base < count; base += 32) {
    const uint32_t n = static_cast<uint32_t>(std::min<size_t>(32, count - base));
    std::vector<NaItem> transform_items;
    std::vector<uint8_t> transform_buffer(static_cast<size_t>(n) * 8, 0);
    transform_items.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
      const uint64_t object = objects[base + i];
      const bool valid = nzl::plausible_ptr(object) &&
                         object <= UINT64_MAX - c.off.car_transform;
      transform_items.push_back(NaItem{valid ? object + c.off.car_transform : 0,
                                       i * 8, valid ? 8u : 0u});
    }
    if (!NaBatchRead(c.pid, transform_items.data(), n, transform_buffer.data(),
                     static_cast<uint32_t>(transform_buffer.size()))) continue;
    for (uint32_t i = 0; i < n; ++i) {
      uint64_t& transform = transforms[base + i];
      std::memcpy(&transform, transform_buffer.data() + i * 8, 8);
      if (!object_klass_name_is(*c.mem, transform, c.off.il2cpp_class_name,
                                "VehicleTransformModel", &c.class_name_cache))
        transform = 0;
    }

    std::vector<NaItem> position_items;
    std::vector<uint8_t> position_buffer(static_cast<size_t>(n) * 12, 0);
    for (uint32_t i = 0; i < n; ++i) {
      const uint64_t transform = transforms[base + i];
      const bool valid = nzl::plausible_ptr(transform) &&
                         transform <= UINT64_MAX - c.off.car_position;
      position_items.push_back(NaItem{valid ? transform + c.off.car_position : 0,
                                      i * 12, valid ? 12u : 0u});
    }
    if (!position_items.empty() &&
        NaBatchRead(c.pid, position_items.data(),
                    static_cast<uint32_t>(position_items.size()), position_buffer.data(),
                    static_cast<uint32_t>(position_buffer.size()))) {
      for (uint32_t i = 0; i < n; ++i) {
        if (!nzl::plausible_ptr(transforms[base + i])) continue;
        float xyz[3]{};
        std::memcpy(xyz, position_buffer.data() + static_cast<size_t>(i) * 12, 12);
        positions[base + i] = {xyz[0], xyz[1], xyz[2]};
      }
    }
  }
  for (const auto& position : positions) {
    constexpr float kMaxWorldCoordinate = 1.0e6f;
    if ((position.x == 0 && position.y == 0 && position.z == 0) ||
        !std::isfinite(position.x) || !std::isfinite(position.y) ||
        !std::isfinite(position.z) || std::fabs(position.x) > kMaxWorldCoordinate ||
        std::fabs(position.y) > kMaxWorldCoordinate ||
        std::fabs(position.z) > kMaxWorldCoordinate) continue;
    const float dx = position.x - local.x;
    const float dy = position.y - local.y;
    const float dz = position.z - local.z;
    CarEnt entity;
    entity.pos = position;
    entity.dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    out.push_back(entity);
  }
  return out;
}
// Settings persistence: nzl_settings.ini next to the exe, written on
// every menu change, read at startup.
static std::string settings_path() {
  char exe[MAX_PATH] = {};
  GetModuleFileNameA(nullptr, exe, MAX_PATH);
  std::string p = exe;
  size_t slash = p.find_last_of("\\/");
  return (slash == std::string::npos ? std::string("nzl_settings.ini") : p.substr(0, slash + 1) + "nzl_settings.ini");
}
static void save_settings(Ctx& c) {
  FILE* f = std::fopen(settings_path().c_str(), "w");
  if (!f) return;
  std::fprintf(f, "fov=%.1f\nmax_dist=%.1f\nradar_range=%.1f\naim_fov=%.1f\naim_smooth=%.1f\n",
               c.fov, c.max_dist, c.radar_range, c.aim_fov, c.aim_smooth);
  std::fprintf(f, "show_dead=%d\nboxes=%d\nskeleton=%d\nlabels=%d\nhp=%d\narmor=%d\nroles=%d\nloot=%d\ncars=%d\nradar=%d\ntracers=%d\naim=%d\nbasis_quat=%d\nvisible_only=%d\naim_bone=%d\naim_key=%d\nmenu_key=%d\n",
               c.show_dead, c.show_boxes, c.show_skeleton, c.show_text, c.show_hp, c.show_armor,
               c.show_roles, c.show_loot, c.show_cars, c.show_radar, c.tracers, c.aim,
               c.basis_quat, c.visible_only, c.aim_bone, c.aim_key, c.menu_key);
  std::fclose(f);
}
static void load_settings(Ctx& c) {
  FILE* f = std::fopen(settings_path().c_str(), "r");
  if (!f) return;
  char line[128];
  while (std::fgets(line, sizeof(line), f)) {
    char key[64]; double val;
    if (std::sscanf(line, "%63[^=]=%lf", key, &val) != 2) continue;
    std::string k = key;
    if (k == "fov") c.fov = (float)val; else if (k == "max_dist") c.max_dist = (float)val;
    else if (k == "radar_range") c.radar_range = (float)val; else if (k == "aim_fov") c.aim_fov = (float)val;
    else if (k == "aim_smooth") c.aim_smooth = (float)val;
    else if (k == "show_dead") c.show_dead = val != 0; else if (k == "boxes") c.show_boxes = val != 0;
    else if (k == "skeleton") c.show_skeleton = val != 0; else if (k == "labels") c.show_text = val != 0;
    else if (k == "hp") c.show_hp = val != 0; else if (k == "armor") c.show_armor = val != 0;
    else if (k == "roles") c.show_roles = val != 0; else if (k == "loot") c.show_loot = val != 0;
    else if (k == "cars") c.show_cars = val != 0; else if (k == "radar") c.show_radar = val != 0;
    else if (k == "tracers") c.tracers = val != 0; else if (k == "aim") c.aim = val != 0;
    else if (k == "basis_quat") c.basis_quat = val != 0; else if (k == "visible_only") c.visible_only = val != 0;
    else if (k == "aim_bone") c.aim_bone = std::clamp((int)val, 0, 2); else if (k == "aim_key") c.aim_key = std::clamp((int)val, 0, 3);
    else if (k == "menu_key") c.menu_key = std::clamp((int)val, 0, 3);
  }
  c.fov = std::clamp(c.fov, 40.0f, 140.0f);
  c.max_dist = std::clamp(c.max_dist, 25.0f, 1500.0f);
  c.aim_fov = std::clamp(c.aim_fov, 30.0f, 600.0f);
  c.aim_smooth = std::clamp(c.aim_smooth, 1.0f, 20.0f);
  c.radar_range = std::clamp(c.radar_range, 30.0f, 500.0f);
  c.aim_bone = std::clamp(c.aim_bone, 0, 2);
  c.aim_key = std::clamp(c.aim_key, 0, 3);
  c.menu_key = std::clamp(c.menu_key, 0, 3);
  std::fclose(f);
}
static HWND g_game_wnd = nullptr;
static Ctx* g_ctx = nullptr;
static constexpr int kMenuWidth = 800;
static constexpr int kMenuHeight = 560;
static constexpr int kSidebarWidth = 152;
static constexpr int kContentX = 166;
static constexpr int kCardWidth = 300;
static constexpr int kCardHeight = 58;
static constexpr int kCardGapY = 66;
static constexpr int kRowsPerColumn = 6;
static const char* kTabNames[] = {"Aim", "Visuals", "Loot", "Radar", "Misc"};
static constexpr int kTabCount = 5;

enum class OptionId {
  AimEnabled, AimKey, AimBone, AimFov, AimSmooth,
  Fov, MaxDistance, ShowDead, Boxes, Skeleton, Labels, Hp, Armor, Roles, Tracers, VisibleOnly,
  Loot, Cars, Radar, RadarRange, BasisQuat, MenuKey
};
enum class OptionKind { Toggle, Slider, Cycle };
struct MenuOption { OptionId id; const char* label; OptionKind kind; float minv = 0, maxv = 0, step = 1; };
static const MenuOption kAimOptions[] = {
  {OptionId::AimEnabled, "Aim assist", OptionKind::Toggle},
  {OptionId::AimKey, "Hold key", OptionKind::Cycle},
  {OptionId::AimBone, "Target point", OptionKind::Cycle},
  {OptionId::AimFov, "Aim radius", OptionKind::Slider, 30, 600, 10},
  {OptionId::AimSmooth, "Smoothing", OptionKind::Slider, 1, 20, 1},
};
static const MenuOption kVisualOptions[] = {
  {OptionId::Fov, "Projection FOV", OptionKind::Slider, 40, 140, 1},
  {OptionId::MaxDistance, "Max distance", OptionKind::Slider, 25, 1500, 25},
  {OptionId::Boxes, "Player boxes", OptionKind::Toggle},
  {OptionId::Skeleton, "Skeleton (approx.)", OptionKind::Toggle},
  {OptionId::Labels, "Names / distance", OptionKind::Toggle},
  {OptionId::ShowDead, "Show dead", OptionKind::Toggle},
  {OptionId::Hp, "HP bars", OptionKind::Toggle},
  {OptionId::Armor, "Armor marks", OptionKind::Toggle},
  {OptionId::Roles, "Role labels", OptionKind::Toggle},
  {OptionId::Tracers, "Snaplines", OptionKind::Toggle},
  {OptionId::VisibleOnly, "On-screen only", OptionKind::Toggle},
};
static const MenuOption kLootOptions[] = {
  {OptionId::Loot, "Loot ESP", OptionKind::Toggle},
  {OptionId::Cars, "Vehicle ESP", OptionKind::Toggle},
};
static const MenuOption kRadarOptions[] = {
  {OptionId::Radar, "Radar", OptionKind::Toggle},
  {OptionId::RadarRange, "Radar range", OptionKind::Slider, 30, 500, 10},
};
static const MenuOption kMiscOptions[] = {
  {OptionId::BasisQuat, "Prefer quaternion (cross-check)", OptionKind::Toggle},
  {OptionId::MenuKey, "Menu hotkey", OptionKind::Cycle},
};
static const MenuOption* page_options(int tab, size_t& count) {
  switch (tab) {
    case 0: count = sizeof(kAimOptions) / sizeof(kAimOptions[0]); return kAimOptions;
    case 1: count = sizeof(kVisualOptions) / sizeof(kVisualOptions[0]); return kVisualOptions;
    case 2: count = sizeof(kLootOptions) / sizeof(kLootOptions[0]); return kLootOptions;
    case 3: count = sizeof(kRadarOptions) / sizeof(kRadarOptions[0]); return kRadarOptions;
    default: count = sizeof(kMiscOptions) / sizeof(kMiscOptions[0]); return kMiscOptions;
  }
}
static void copy_config(Ctx& dst, const Ctx& src) {
  dst.show_dead = src.show_dead; dst.show_boxes = src.show_boxes; dst.show_skeleton = src.show_skeleton;
  dst.show_text = src.show_text; dst.show_hp = src.show_hp; dst.show_armor = src.show_armor;
  dst.show_loot = src.show_loot; dst.show_radar = src.show_radar; dst.aim = src.aim; dst.basis_quat = src.basis_quat;
  dst.show_roles = src.show_roles; dst.show_cars = src.show_cars; dst.tracers = src.tracers; dst.visible_only = src.visible_only;
  dst.fov = src.fov; dst.max_dist = src.max_dist; dst.aim_fov = src.aim_fov; dst.aim_smooth = src.aim_smooth; dst.radar_range = src.radar_range;
  dst.aim_bone = src.aim_bone; dst.aim_key = src.aim_key; dst.menu_key = src.menu_key;
  dst.width = src.width; dst.height = src.height; dst.menu = src.menu;
}
static bool option_value(const Ctx& c, OptionId id) {
  switch (id) {
    case OptionId::AimEnabled: return c.aim;
    case OptionId::ShowDead: return c.show_dead;
    case OptionId::Boxes: return c.show_boxes;
    case OptionId::Skeleton: return c.show_skeleton;
    case OptionId::Labels: return c.show_text;
    case OptionId::Hp: return c.show_hp;
    case OptionId::Armor: return c.show_armor;
    case OptionId::Roles: return c.show_roles;
    case OptionId::Tracers: return c.tracers;
    case OptionId::VisibleOnly: return c.visible_only;
    case OptionId::Loot: return c.show_loot;
    case OptionId::Cars: return c.show_cars;
    case OptionId::Radar: return c.show_radar;
    case OptionId::BasisQuat: return c.basis_quat;
    default: return false;
  }
}
static float option_number(const Ctx& c, OptionId id) {
  switch (id) {
    case OptionId::Fov: return c.fov;
    case OptionId::MaxDistance: return c.max_dist;
    case OptionId::AimFov: return c.aim_fov;
    case OptionId::AimSmooth: return c.aim_smooth;
    case OptionId::RadarRange: return c.radar_range;
    default: return 0;
  }
}
static void set_option_number(Ctx& c, OptionId id, float value) {
  switch (id) {
    case OptionId::Fov: c.fov = value; break;
    case OptionId::MaxDistance: c.max_dist = value; break;
    case OptionId::AimFov: c.aim_fov = value; break;
    case OptionId::AimSmooth: c.aim_smooth = value; break;
    case OptionId::RadarRange: c.radar_range = value; break;
    default: break;
  }
}
static void set_slider_from_x(Ctx& c, const MenuOption& item, int x, int left) {
  float lo = item.minv, hi = item.maxv;
  float t = (float)(x - (left + 16)) / (float)(kCardWidth - 32);
  t = std::clamp(t, 0.0f, 1.0f);
  float value = lo + t * (hi - lo);
  value = lo + std::round((value - lo) / item.step) * item.step;
  set_option_number(c, item.id, std::clamp(value, lo, hi));
}
static void cycle_option(Ctx& c, OptionId id, int direction = 1) {
  switch (id) {
    case OptionId::AimKey: c.aim_key = (c.aim_key + direction + 4) % 4; break;
    case OptionId::AimBone: c.aim_bone = (c.aim_bone + direction + 3) % 3; break;
    case OptionId::MenuKey: c.menu_key = (c.menu_key + direction + 4) % 4; break;
    default: break;
  }
}
static void activate_option(Ctx& c, OptionId id) {
  switch (id) {
    case OptionId::AimEnabled: c.aim = !c.aim; break;
    case OptionId::ShowDead: c.show_dead = !c.show_dead; break;
    case OptionId::Boxes: c.show_boxes = !c.show_boxes; break;
    case OptionId::Skeleton: c.show_skeleton = !c.show_skeleton; break;
    case OptionId::Labels: c.show_text = !c.show_text; break;
    case OptionId::Hp: c.show_hp = !c.show_hp; break;
    case OptionId::Armor: c.show_armor = !c.show_armor; break;
    case OptionId::Roles: c.show_roles = !c.show_roles; break;
    case OptionId::Tracers: c.tracers = !c.tracers; break;
    case OptionId::VisibleOnly: c.visible_only = !c.visible_only; break;
    case OptionId::Loot: c.show_loot = !c.show_loot; break;
    case OptionId::Cars: c.show_cars = !c.show_cars; break;
    case OptionId::Radar: c.show_radar = !c.show_radar; break;
    case OptionId::BasisQuat: c.basis_quat = !c.basis_quat; break;
    case OptionId::AimKey: case OptionId::AimBone: case OptionId::MenuKey: cycle_option(c, id); break;
    default: break;
  }
}
static RECT option_rect(size_t index) {
  int col = (int)(index / kRowsPerColumn), row = (int)(index % kRowsPerColumn);
  int left = kContentX + col * (kCardWidth + 12);
  int top = 122 + row * kCardGapY;
  return RECT{left, top, left + kCardWidth, top + kCardHeight};
}
static void draw_text(HDC dc, int x, int y, const char* text, COLORREF color) {
  SetTextColor(dc, color);
  TextOutA(dc, x, y, text, (int)std::strlen(text));
}
static void draw_fit_text(HDC dc, RECT rect, const char* text, COLORREF color, UINT flags) {
  SetTextColor(dc, color);
  DrawTextA(dc, text, -1, &rect, flags);
}
static void draw_center_text(HDC dc, RECT rect, const char* text, COLORREF color) {
  draw_fit_text(dc, rect, text, color, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}
static void fill_round_rect(HDC dc, const RECT& rect, COLORREF fill,
                            COLORREF border, int radius = 10) {
  HBRUSH brush = CreateSolidBrush(fill);
  HPEN pen = CreatePen(PS_SOLID, 1, border);
  HGDIOBJ old_brush = SelectObject(dc, brush);
  HGDIOBJ old_pen = SelectObject(dc, pen);
  RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
  SelectObject(dc, old_brush);
  SelectObject(dc, old_pen);
  DeleteObject(brush);
  DeleteObject(pen);
}
static void draw_menu_option(HDC dc, const Ctx& c, const MenuOption& item,
                             size_t index, HFONT label_font, HFONT hint_font) {
  RECT r = option_rect(index);
  const bool hovered = c.hover_option == static_cast<int>(index);
  const COLORREF card_fill = hovered ? RGB(34, 37, 49) : RGB(24, 27, 36);
  const COLORREF card_border = hovered ? RGB(112, 91, 158) : RGB(48, 53, 66);
  fill_round_rect(dc, r, card_fill, card_border, 12);
  if (hovered) {
    RECT accent{r.left, r.top + 10, r.left + 3, r.bottom - 10};
    HBRUSH accent_brush = CreateSolidBrush(RGB(174, 118, 255));
    FillRect(dc, &accent, accent_brush);
    DeleteObject(accent_brush);
  }

  RECT label_rect{r.left + 15, r.top + 7, r.right - 132, r.top + 31};
  SelectObject(dc, label_font);
  draw_fit_text(dc, label_rect, item.label, RGB(237, 239, 247),
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  char value[48] = {};

  if (item.kind == OptionKind::Toggle) {
    const bool on = option_value(c, item.id);
    std::snprintf(value, sizeof(value), "%s", on ? "ON" : "OFF");
    RECT button{r.right - 88, r.top + 13, r.right - 14, r.top + 41};
    fill_round_rect(dc, button, on ? RGB(88, 54, 145) : RGB(45, 49, 60),
                    on ? RGB(150, 105, 219) : RGB(73, 79, 94), 8);
    draw_center_text(dc, button, value, on ? RGB(250, 245, 255) : RGB(186, 191, 204));
    SelectObject(dc, hint_font);
    draw_text(dc, r.left + 16, r.top + 37,
              on ? "Enabled - click to turn off" : "Disabled - click to turn on",
              RGB(145, 151, 166));
  } else if (item.kind == OptionKind::Slider) {
    const float v = option_number(c, item.id);
    std::snprintf(value, sizeof(value), "%.0f", v);
    RECT value_button{r.right - 74, r.top + 6, r.right - 13, r.top + 28};
    fill_round_rect(dc, value_button, RGB(39, 34, 52), RGB(77, 64, 101), 7);
    draw_center_text(dc, value_button, value, RGB(218, 196, 255));
    RECT track{r.left + 16, r.top + 45, r.right - 16, r.top + 50};
    fill_round_rect(dc, track, RGB(54, 59, 72), RGB(54, 59, 72), 4);
    const float t = item.maxv > item.minv
        ? std::clamp((v - item.minv) / (item.maxv - item.minv), 0.0f, 1.0f)
        : 0.0f;
    RECT fill = track;
    fill.right = fill.left + static_cast<int>((fill.right - fill.left) * t);
    if (fill.right > fill.left)
      fill_round_rect(dc, fill, RGB(162, 104, 235), RGB(162, 104, 235), 4);
    const int knob_x = fill.left + static_cast<int>((track.right - track.left) * t);
    HBRUSH knob = CreateSolidBrush(RGB(244, 236, 255));
    HPEN knob_border = CreatePen(PS_SOLID, 1, RGB(160, 116, 226));
    HGDIOBJ old_brush = SelectObject(dc, knob);
    HGDIOBJ old_pen = SelectObject(dc, knob_border);
    Ellipse(dc, knob_x - 5, track.top - 4, knob_x + 5, track.bottom + 4);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(knob);
    DeleteObject(knob_border);
  } else {
    if (item.id == OptionId::AimKey)
      std::snprintf(value, sizeof(value), "< %s >", kAimKeyNames[std::clamp(c.aim_key, 0, 3)]);
    else if (item.id == OptionId::AimBone)
      std::snprintf(value, sizeof(value), "< %s >", kBoneNames[std::clamp(c.aim_bone, 0, 2)]);
    else
      std::snprintf(value, sizeof(value), "< %s >", kMenuKeyNames[std::clamp(c.menu_key, 0, 3)]);
    RECT button{r.right - 122, r.top + 12, r.right - 13, r.top + 41};
    fill_round_rect(dc, button, hovered ? RGB(57, 43, 79) : RGB(40, 35, 52),
                    RGB(84, 69, 111), 8);
    draw_center_text(dc, button, value, RGB(221, 202, 255));
    SelectObject(dc, hint_font);
    draw_text(dc, r.left + 16, r.top + 37, "Click to cycle options", RGB(145, 151, 166));
  }
}
static void draw_menu(HDC dc, const RECT& client, const Ctx& c) {
  HBRUSH bg = CreateSolidBrush(RGB(13, 15, 21));
  FillRect(dc, &client, bg);
  DeleteObject(bg);
  RECT side{0, 0, kSidebarWidth, client.bottom};
  HBRUSH side_brush = CreateSolidBrush(RGB(18, 20, 28));
  FillRect(dc, &side, side_brush);
  DeleteObject(side_brush);
  RECT divider{kSidebarWidth - 1, 0, kSidebarWidth, client.bottom};
  HBRUSH divider_brush = CreateSolidBrush(RGB(38, 41, 53));
  FillRect(dc, &divider, divider_brush);
  DeleteObject(divider_brush);
  SetBkMode(dc, TRANSPARENT);

  HFONT title = CreateFontA(22, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
      CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");
  HFONT normal = CreateFontA(14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
      CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");
  HFONT small_font = CreateFontA(12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
      CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");
  HGDIOBJ old_font = SelectObject(dc, title);

  RECT logo{18, 18, 48, 48};
  fill_round_rect(dc, logo, RGB(112, 69, 178), RGB(157, 111, 220), 9);
  draw_center_text(dc, logo, "N", RGB(255, 255, 255));
  draw_text(dc, 59, 19, "NZL STUDIO", RGB(241, 238, 248));
  SelectObject(dc, small_font);
  draw_text(dc, 18, 59, "ROB RUN  |  CONTROL", RGB(129, 135, 151));

  for (int i = 0; i < kTabCount; ++i) {
    RECT tab{10, 96 + i * 54, kSidebarWidth - 10, 140 + i * 54};
    const bool active = i == c.menu_tab;
    const bool hovered = i == c.hover_tab;
    if (active || hovered) {
      fill_round_rect(dc, tab, active ? RGB(45, 34, 64) : RGB(29, 32, 43),
                      active ? RGB(80, 61, 111) : RGB(50, 55, 69), 9);
    }
    if (active) {
      RECT stripe{tab.left, tab.top + 9, tab.left + 3, tab.bottom - 9};
      HBRUSH accent = CreateSolidBrush(RGB(176, 119, 245));
      FillRect(dc, &stripe, accent);
      DeleteObject(accent);
    }
    SelectObject(dc, normal);
    draw_text(dc, tab.left + 16, tab.top + 14, kTabNames[i],
              active ? RGB(246, 237, 255) : (hovered ? RGB(214, 209, 228) : RGB(154, 160, 175)));
  }

  SelectObject(dc, small_font);
  draw_text(dc, 18, client.bottom - 46, "INSERT  toggle menu", RGB(158, 164, 178));
  draw_text(dc, 18, client.bottom - 26, "END  exit", RGB(121, 128, 144));

  SelectObject(dc, title);
  draw_text(dc, kContentX, 20, kTabNames[std::clamp(c.menu_tab, 0, kTabCount - 1)],
            RGB(241, 239, 248));
  RECT close{client.right - 43, 10, client.right - 11, 42};
  fill_round_rect(dc, close, c.hover_close ? RGB(105, 44, 59) : RGB(34, 36, 47),
                  c.hover_close ? RGB(177, 77, 96) : RGB(57, 61, 75), 9);
  draw_center_text(dc, close, "X", c.hover_close ? RGB(255, 226, 231) : RGB(205, 207, 218));

  RECT status{kContentX, 52, client.right - 56, 110};
  const COLORREF status_fill = c.esp_ready ? RGB(24, 54, 44) : RGB(48, 40, 28);
  const COLORREF status_edge = c.esp_ready ? RGB(47, 102, 75) : RGB(98, 77, 44);
  fill_round_rect(dc, status, status_fill, status_edge, 9);
  HBRUSH dot_brush = CreateSolidBrush(c.esp_ready ? RGB(108, 222, 151) : RGB(236, 184, 84));
  HPEN dot_pen = CreatePen(PS_SOLID, 1, c.esp_ready ? RGB(108, 222, 151) : RGB(236, 184, 84));
  HGDIOBJ old_dot_brush = SelectObject(dc, dot_brush);
  HGDIOBJ old_dot_pen = SelectObject(dc, dot_pen);
  Ellipse(dc, status.left + 12, status.top + 24, status.left + 22, status.top + 34);
  SelectObject(dc, old_dot_brush);
  SelectObject(dc, old_dot_pen);
  DeleteObject(dot_brush);
  DeleteObject(dot_pen);
  SelectObject(dc, small_font);
  RECT status_text{status.left + 32, status.top + 3, status.right - 12, status.bottom - 3};
  draw_fit_text(dc, status_text, c.status.c_str(),
                c.esp_ready ? RGB(183, 239, 202) : RGB(243, 211, 158),
                DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS);

  size_t count = 0;
  const MenuOption* items = page_options(c.menu_tab, count);
  for (size_t i = 0; i < count; ++i)
    draw_menu_option(dc, c, items[i], i, normal, small_font);

  SelectObject(dc, small_font);
  draw_text(dc, kContentX, client.bottom - 23,
            "Click a control to change it  |  Drag a slider  |  Insert hides",
            RGB(115, 122, 138));
  SelectObject(dc, old_font);
  DeleteObject(title);
  DeleteObject(normal);
  DeleteObject(small_font);
}
static RECT g_option_rect_drag{};
static int g_drag_option = -1;
static bool g_drag_modified = false;
static int g_menu_cursor_show_calls = 0;
static bool set_menu_visible(bool visible);
static void focus_window(HWND target) {
  if (!target || !IsWindow(target)) return;
  HWND fg = GetForegroundWindow();
  DWORD fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
  DWORD myThread = GetCurrentThreadId();
  bool attached = fgThread && fgThread != myThread && AttachThreadInput(myThread, fgThread, TRUE);
  BringWindowToTop(target);
  SetForegroundWindow(target);
  SetFocus(target);
  if (attached) AttachThreadInput(myThread, fgThread, FALSE);
}
static bool set_menu_visible(bool visible) {
  if (!g_ctx) return false;
  HWND menu = nullptr; HWND game = nullptr; int x = 32, y = 48;
  {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (g_ctx->menu == visible) return true;
    g_ctx->menu = visible;
    if (!visible) {
      g_ctx->hover_tab = -1;
      g_ctx->hover_option = -1;
      g_ctx->hover_close = false;
    }
    menu = g_ctx->menu_wnd;
    game = g_game_wnd;
    x = g_ctx->menu_x; y = g_ctx->menu_y;
  }
  ClipCursor(nullptr);
  if (visible) {
    int cursor_count = 0;
    do {
      cursor_count = ShowCursor(TRUE);
      ++g_menu_cursor_show_calls;
    } while (cursor_count < 0);
    if (menu) {
      SetWindowPos(menu, HWND_TOPMOST, x, y, kMenuWidth, kMenuHeight, SWP_SHOWWINDOW);
      focus_window(menu);
      SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)));
      InvalidateRect(menu, nullptr, FALSE);
    }
  } else {
    if (g_drag_option >= 0) {
      const bool save_drag = g_drag_modified;
      g_drag_option = -1;
      g_drag_modified = false;
      if (GetCapture() == menu) ReleaseCapture();
      if (save_drag) {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        if (g_ctx) save_settings(*g_ctx);
      }
    }
    if (menu) ShowWindow(menu, SW_HIDE);
    while (g_menu_cursor_show_calls > 0) {
      ShowCursor(FALSE);
      --g_menu_cursor_show_calls;
    }
    if (game && IsWindow(game)) focus_window(game);
  }
  return true;
}
static LRESULT CALLBACK overlay_wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_ERASEBKGND) return 1;
  if (msg == WM_PAINT) {
    static ULONGLONG paint_window_start = 0;
    static uint32_t paint_frames = 0;
    ULONGLONG paint_now = GetTickCount64();
    if (!paint_window_start) paint_window_start = paint_now;
    ++paint_frames;
    ULONGLONG paint_elapsed = paint_now - paint_window_start;
    if (paint_elapsed >= 1000) {
      uint64_t milli_fps = (uint64_t)paint_frames * 1000000ULL / paint_elapsed;
      g_overlay_paint_millifps.store((uint32_t)milli_fps, std::memory_order_relaxed);
      paint_frames = 0; paint_window_start = paint_now;
    }
    PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
    RECT rc{}; GetClientRect(h, &rc);
    HBRUSH black = CreateSolidBrush(RGB(0, 0, 0)); FillRect(dc, &rc, black); DeleteObject(black);
    SetBkMode(dc, TRANSPARENT);
    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (g_ctx) {
      if (!g_ctx->esp_ready) {
        SetTextColor(dc, RGB(232, 192, 132));
        RECT status_rect{12, 12, std::max(static_cast<int>(rc.right) - 12, 13), 68};
        DrawTextA(dc, g_ctx->status.c_str(), -1, &status_rect,
                  DT_LEFT | DT_TOP | DT_NOPREFIX | DT_WORDBREAK | DT_END_ELLIPSIS);
      } else {
        HPEN pen = CreatePen(PS_SOLID, 2, RGB(216, 160, 255));
        HPEN thin = CreatePen(PS_SOLID, 1, RGB(177, 106, 244));
        HFONT font = CreateFontA(14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");
        HGDIOBJ oldpen = SelectObject(dc, pen); HGDIOBJ oldfont = SelectObject(dc, font);
        RECT status_rect{12, 12, std::max(static_cast<int>(rc.right) - 12, 13), 68};
        SetTextColor(dc, RGB(190, 145, 255));
        DrawTextA(dc, g_ctx->status.c_str(), -1, &status_rect,
                  DT_LEFT | DT_TOP | DT_NOPREFIX | DT_WORDBREAK | DT_END_ELLIPSIS);
        for (auto& e : g_ctx->render) {
          float hgt = std::fabs(e.fy - e.hy); if (hgt < 2) continue;
          float wid = hgt * 0.45f;
          COLORREF col = e.dead ? RGB(120, 120, 120) : RGB(216, 160, 255);
          if (g_ctx->show_roles && !e.dead && e.role >= 0 && e.role <= 4) {
            const COLORREF roleColors[] = {RGB(200, 200, 200), RGB(255, 80, 80), RGB(90, 170, 255), RGB(120, 255, 140), RGB(255, 180, 80)};
            col = roleColors[e.role];
          }
          SetTextColor(dc, col);
          if (g_ctx->tracers) {
            SelectObject(dc, thin);
            MoveToEx(dc, g_ctx->width / 2, g_ctx->height, nullptr);
            LineTo(dc, (int)e.fx, (int)e.fy);
          }
          if (g_ctx->show_boxes) {
            SelectObject(dc, pen);
            HGDIOBJ oldbrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
            Rectangle(dc, (int)(e.hx - wid * 0.5f), (int)e.hy, (int)(e.hx + wid * 0.5f), (int)e.fy);
            SelectObject(dc, oldbrush);
          }
          if (g_ctx->show_skeleton) {
            SelectObject(dc, thin);
            float cx = e.hx, top = e.hy, bot = e.fy;
            float neck = top + hgt * 0.16f, chest = top + hgt * 0.38f, pelvis = top + hgt * 0.58f, knee = top + hgt * 0.79f;
            MoveToEx(dc, (int)cx, (int)neck, nullptr); LineTo(dc, (int)cx, (int)pelvis);
            MoveToEx(dc, (int)(cx - wid * 0.55f), (int)(chest + hgt * 0.10f), nullptr); LineTo(dc, (int)cx, (int)chest); LineTo(dc, (int)(cx + wid * 0.55f), (int)(chest + hgt * 0.10f));
            MoveToEx(dc, (int)cx, (int)pelvis, nullptr); LineTo(dc, (int)(cx - wid * 0.30f), (int)knee); LineTo(dc, (int)(cx - wid * 0.32f), (int)bot);
            MoveToEx(dc, (int)cx, (int)pelvis, nullptr); LineTo(dc, (int)(cx + wid * 0.30f), (int)knee); LineTo(dc, (int)(cx + wid * 0.32f), (int)bot);
            Ellipse(dc, (int)(cx - wid * 0.16f), (int)top, (int)(cx + wid * 0.16f), (int)neck);
          }
          int ty = (int)e.hy - 15;
          if (g_ctx->show_text) {
            const char* role = (g_ctx->show_roles && e.role >= 0 && e.role <= 4) ? kRoleNames[e.role] : "";
            std::wstring text = e.nickname;
            if (text.empty()) {
              text = L"#" + std::to_wstring(e.id);
            } else {
              text += L"  #" + std::to_wstring(e.id);
            }
            text += L"  " + std::to_wstring(static_cast<int>(std::lround(e.dist))) + L"m";
            if (role[0]) {
              text.push_back(L' ');
              for (const char* ch = role; *ch; ++ch) text.push_back(static_cast<wchar_t>(*ch));
            }
            if (e.armor && g_ctx->show_armor) text += L" [ARM]";
            SIZE text_size{};
            GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()),
                                 &text_size);
            TextOutW(dc, (int)e.hx - text_size.cx / 2, ty, text.c_str(),
                     static_cast<int>(text.size()));
            ty -= 13;
          }
          if (g_ctx->show_hp && e.hp >= 0 && e.maxhp > 0) {
            float frac = std::clamp((float)e.hp / (float)e.maxhp, 0.0f, 1.0f);
            RECT bar{(int)(e.hx - wid * 0.5f), ty - 3, (int)(e.hx + wid * 0.5f), ty + 2};
            HBRUSH bgb = CreateSolidBrush(RGB(40, 40, 40)); FillRect(dc, &bar, bgb); DeleteObject(bgb);
            RECT fill = bar; fill.right = bar.left + (int)((bar.right - bar.left) * frac);
            HBRUSH fgb = CreateSolidBrush(frac > 0.5f ? RGB(80, 255, 120) : frac > 0.25f ? RGB(255, 200, 60) : RGB(255, 70, 70));
            FillRect(dc, &fill, fgb); DeleteObject(fgb);
          }
        }
        if (g_ctx->show_cars) {
          SelectObject(dc, thin); SetTextColor(dc, RGB(255, 170, 60));
          for (auto& car : g_ctx->cars) if (car.on) {
            Rectangle(dc, (int)car.sx - 6, (int)car.sy - 4, (int)car.sx + 6, (int)car.sy + 4);
            char text[64]; std::snprintf(text, sizeof(text), "car %.0fm", car.dist);
            TextOutA(dc, (int)car.sx + 8, (int)car.sy - 7, text, (int)std::strlen(text));
          }
        }
        if (g_ctx->aim) {
          SelectObject(dc, thin); HGDIOBJ oldbrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
          int cx = g_ctx->width / 2, cy = g_ctx->height / 2, radius = (int)g_ctx->aim_fov;
          Ellipse(dc, cx - radius, cy - radius, cx + radius, cy + radius);
          SelectObject(dc, oldbrush);
        }
        if (g_ctx->show_loot) {
          SelectObject(dc, thin); SetTextColor(dc, RGB(255, 220, 120));
          for (auto& loot : g_ctx->loot) if (loot.on) {
            Rectangle(dc, (int)loot.sx - 4, (int)loot.sy - 4, (int)loot.sx + 4, (int)loot.sy + 4);
            char text[64]; std::snprintf(text, sizeof(text), "loot %.0fm", loot.dist);
            TextOutA(dc, (int)loot.sx + 6, (int)loot.sy - 7, text, (int)std::strlen(text));
          }
        }
        if (g_ctx->show_radar) {
          int radius = 90, cx = g_ctx->width - radius - 16, cy = radius + 16;
          HBRUSH rb = CreateSolidBrush(RGB(24, 0, 36)); HGDIOBJ oldbrush = SelectObject(dc, rb);
          SelectObject(dc, thin); Ellipse(dc, cx - radius, cy - radius, cx + radius, cy + radius);
          SelectObject(dc, oldbrush); DeleteObject(rb);
          float yaw = std::atan2(g_ctx->cam_fwd.x, g_ctx->cam_fwd.z);
          float scale = radius / std::max(g_ctx->radar_range, 1.0f);
          for (auto& e : g_ctx->render) {
            float dx = e.pos.x - g_ctx->radar_cam.x, dz = e.pos.z - g_ctx->radar_cam.z;
            float rx = dx * std::cos(yaw) - dz * std::sin(yaw);
            float rz = dx * std::sin(yaw) + dz * std::cos(yaw);
            float px = cx + rx * scale, py = cy - rz * scale;
            float ox = px - cx, oy = py - cy;
            if (ox * ox + oy * oy > (radius - 4) * (radius - 4)) continue;
            HBRUSH dot = CreateSolidBrush(e.dead ? RGB(130, 130, 130) : RGB(255, 120, 255));
            RECT dr{(int)px - 3, (int)py - 3, (int)px + 3, (int)py + 3}; FillRect(dc, &dr, dot); DeleteObject(dot);
          }
          HBRUSH me = CreateSolidBrush(RGB(255, 255, 255)); RECT mr{cx - 3, cy - 3, cx + 3, cy + 3};
          FillRect(dc, &mr, me); DeleteObject(me);
        }
        SelectObject(dc, oldfont); SelectObject(dc, oldpen); DeleteObject(pen); DeleteObject(thin); DeleteObject(font);
      }
    }
    EndPaint(h, &ps); return 0;
  }
  return DefWindowProcW(h, msg, wp, lp);
}
static LRESULT CALLBACK menu_wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_ERASEBKGND) return 1;
  if (msg == WM_NCHITTEST) {
    POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}; ScreenToClient(h, &p);
    if (p.y >= 0 && p.y < 42 && p.x >= 0 && p.x < kMenuWidth - 48) return HTCAPTION;
    return HTCLIENT;
  }
  if (msg == WM_PAINT) {
    PAINTSTRUCT ps{};
    HDC target = BeginPaint(h, &ps);
    RECT rc{}; GetClientRect(h, &rc);
    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;
    HDC buffer = (width > 0 && height > 0) ? CreateCompatibleDC(target) : nullptr;
    HBITMAP bitmap = buffer ? CreateCompatibleBitmap(target, width, height) : nullptr;
    if (buffer && bitmap) {
      HGDIOBJ old_bitmap = SelectObject(buffer, bitmap);
      {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        if (g_ctx) draw_menu(buffer, rc, *g_ctx);
      }
      BitBlt(target, 0, 0, width, height, buffer, 0, 0, SRCCOPY);
      SelectObject(buffer, old_bitmap);
    } else {
      std::lock_guard<std::mutex> lock(g_state_mutex);
      if (g_ctx) draw_menu(target, rc, *g_ctx);
    }
    if (bitmap) DeleteObject(bitmap);
    if (buffer) DeleteDC(buffer);
    EndPaint(h, &ps);
    return 0;
  }
  if (msg == WM_LBUTTONDOWN) {
    int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
    if (y < 42 && x >= kMenuWidth - 48) { set_menu_visible(false); return 0; }
    bool changed = false;
    {
      std::lock_guard<std::mutex> lock(g_state_mutex);
      if (!g_ctx) return 0;
      for (int i = 0; i < kTabCount; ++i) {
        RECT r{10, 96 + i * 54, kSidebarWidth - 10, 140 + i * 54};
        if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) {
          g_ctx->menu_tab = i;
          g_ctx->hover_option = -1;
          changed = true;
          break;
        }
      }
      if (!changed && x >= kSidebarWidth) {
        size_t count = 0; const MenuOption* items = page_options(g_ctx->menu_tab, count);
        for (size_t i = 0; i < count; ++i) {
          RECT r = option_rect(i);
          if (x < r.left || x >= r.right || y < r.top || y >= r.bottom) continue;
          if (items[i].kind == OptionKind::Slider) {
            g_drag_option = (int)i; g_option_rect_drag = r; g_drag_modified = true;
            set_slider_from_x(*g_ctx, items[i], x, r.left);
            SetCapture(h);
          } else {
            activate_option(*g_ctx, items[i].id);
            save_settings(*g_ctx);
          }
          changed = true; break;
        }
      }
    }
    if (changed) { InvalidateRect(h, nullptr, FALSE); if (g_ctx->overlay_wnd) InvalidateRect(g_ctx->overlay_wnd, nullptr, FALSE); }
    return 0;
  }
  if (msg == WM_MOUSEMOVE) {
    TRACKMOUSEEVENT track{};
    track.cbSize = sizeof(track);
    track.dwFlags = TME_LEAVE;
    track.hwndTrack = h;
    TrackMouseEvent(&track);

    const int x = GET_X_LPARAM(lp);
    const int y = GET_Y_LPARAM(lp);
    if (g_drag_option >= 0 && (wp & MK_LBUTTON)) {
      bool changed = false;
      {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        if (g_ctx) {
          size_t count = 0; const MenuOption* items = page_options(g_ctx->menu_tab, count);
          if ((size_t)g_drag_option < count && items[g_drag_option].kind == OptionKind::Slider) {
            set_slider_from_x(*g_ctx, items[g_drag_option], x, g_option_rect_drag.left);
            changed = true;
          }
        }
      }
      if (changed) {
        InvalidateRect(h, nullptr, FALSE);
        if (g_ctx && g_ctx->overlay_wnd) InvalidateRect(g_ctx->overlay_wnd, nullptr, FALSE);
      }
      SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32644)));
      return 0;
    }

    bool changed = false;
    bool interactive = false;
    {
      std::lock_guard<std::mutex> lock(g_state_mutex);
      if (g_ctx) {
        int hover_tab = -1;
        int hover_option = -1;
        const bool hover_close = y >= 10 && y < 42 && x >= kMenuWidth - 43 && x < kMenuWidth - 11;
        for (int i = 0; i < kTabCount; ++i) {
          RECT r{10, 96 + i * 54, kSidebarWidth - 10, 140 + i * 54};
          if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) {
            hover_tab = i;
            break;
          }
        }
        if (x >= kSidebarWidth) {
          size_t count = 0;
          page_options(g_ctx->menu_tab, count);
          for (size_t i = 0; i < count; ++i) {
            const RECT r = option_rect(i);
            if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) {
              hover_option = static_cast<int>(i);
              break;
            }
          }
        }
        changed = g_ctx->hover_tab != hover_tab ||
                  g_ctx->hover_option != hover_option ||
                  g_ctx->hover_close != hover_close;
        g_ctx->hover_tab = hover_tab;
        g_ctx->hover_option = hover_option;
        g_ctx->hover_close = hover_close;
        interactive = hover_close || hover_tab >= 0 || hover_option >= 0;
      }
    }
    if (changed) InvalidateRect(h, nullptr, FALSE);
    SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(interactive ? 32649 : 32512)));
    return 0;
  }
  if (msg == WM_MOUSELEAVE) {
    bool changed = false;
    {
      std::lock_guard<std::mutex> lock(g_state_mutex);
      if (g_ctx) {
        changed = g_ctx->hover_tab != -1 || g_ctx->hover_option != -1 || g_ctx->hover_close;
        g_ctx->hover_tab = -1;
        g_ctx->hover_option = -1;
        g_ctx->hover_close = false;
      }
    }
    if (changed) InvalidateRect(h, nullptr, FALSE);
    return 0;
  }
  if (msg == WM_LBUTTONUP) {
    if (g_drag_option >= 0) {
      g_drag_option = -1; ReleaseCapture();
      { std::lock_guard<std::mutex> lock(g_state_mutex); if (g_ctx && g_drag_modified) save_settings(*g_ctx); }
      g_drag_modified = false; return 0;
    }
  }
  if (msg == WM_CLOSE) { set_menu_visible(false); return 0; }
  if (msg == WM_MOVE && g_ctx) {
    RECT r{}; GetWindowRect(h, &r);
    std::lock_guard<std::mutex> lock(g_state_mutex); g_ctx->menu_x = r.left; g_ctx->menu_y = r.top;
  }
  return DefWindowProcW(h, msg, wp, lp);
}
static HWND enum_candidate = nullptr;
static long enum_best_area = 0;
static BOOL CALLBACK enum_game_window(HWND h, LPARAM lp) {
  DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
  if (pid != (DWORD)lp || !IsWindowVisible(h)) return TRUE;
  RECT r{}; if (!GetClientRect(h, &r)) return TRUE;
  long area = (long)(r.right - r.left) * (long)(r.bottom - r.top);
  if (area > enum_best_area) { enum_best_area = area; enum_candidate = h; }
  return TRUE;
}
static HWND find_game_window(DWORD pid) {
  enum_candidate = nullptr; enum_best_area = 0;
  EnumWindows(enum_game_window, (LPARAM)pid);
  return enum_candidate;
}
static void set_runtime(Ctx* ui, DWORD pid, bool ready, const std::string& status, bool clear_data) {
  HWND overlay = nullptr, menu = nullptr;
  bool menu_status_changed = false;
  {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    menu_status_changed = ui->attached_pid != pid || ui->esp_ready != ready || ui->status != status;
    ui->attached_pid = pid; ui->esp_ready = ready; ui->status = status;
    if (clear_data) { ui->render.clear(); ui->loot.clear(); ui->cars.clear(); }
    overlay = ui->overlay_wnd; menu = ui->menu_wnd;
  }
  if (menu_status_changed) {
    std::printf("[NZL] %s\n", status.c_str());
    std::fflush(stdout);
  }
  if (overlay) InvalidateRect(overlay, nullptr, FALSE);
  if (menu && menu_status_changed) InvalidateRect(menu, nullptr, FALSE);
}
static bool read_classes(nzl::DriverMemory& mem, uint64_t ga,
                         const nzl::Offsets& off,
                         const nzl::game_profile::GameProfile& profile,
                         uint64_t klass[7], const char*& failed) {
  for (size_t i = 0; i < profile.type_info.size(); ++i) {
    const auto& slot = profile.type_info[i];
    if (!slot.rva.present() || slot.rva.value > UINT64_MAX - ga) {
      failed = slot.class_name.data();
      return false;
    }
    auto class_ptr = mem.ptr(ga + slot.rva.value);
    klass[i] = class_ptr ? *class_ptr : 0;
    if (!klass[i] || !klass_name_is(mem, klass[i], off.il2cpp_class_name,
                                    slot.class_name.data())) {
      failed = slot.class_name.data();
      return false;
    }
  }
  failed = nullptr;
  return true;
}
static bool chain_classes_match(const nzl::IMemory& mem,
                                const nzl::ChainResult& chain,
                                const uint64_t klass[7]) {
  if (!chain.ok) return false;
  const uint64_t objects[] = {chain.game, chain.game_model, chain.battle_model,
                              chain.battle_entities, chain.world_entities,
                              chain.camera_model};
  for (size_t i = 0; i < std::size(objects); ++i) {
    auto object_class = mem.ptr(objects[i]);
    if (!object_class || *object_class != klass[i]) return false;
  }
  return true;
}
static uint64_t find_live_game(nzl::DriverMemory& mem, DWORD pid,
                               const uint64_t klass[7], const nzl::Offsets& off,
                               Ctx* ui) {
  uint64_t start = 0x10000, candidates[256];
  while (start && !g_stop.load()) {
    uint64_t next = 0;
    const uint32_t found = NaScan(pid, klass[0], start, candidates, 256, &next);
    for (uint32_t i = 0; i < found && !g_stop.load(); ++i) {
      const auto candidate = nzl::resolve_chain_from_game(mem, candidates[i], off);
      if (!chain_classes_match(mem, candidate, klass)) continue;
      const auto roster = nzl::read_dict(mem, candidate.dict, off);
      if (!roster || roster->count <= 0) continue;
      return candidates[i];
    }
    char status[128];
    std::snprintf(status, sizeof(status), "Waiting for validated battle object... scan 0x%llX",
                  (unsigned long long)next);
    set_runtime(ui, pid, false, status, false);
    if (!next) break;
    if (next <= start) {
      set_runtime(ui, pid, false, "Profile-gated heap scan stopped: cursor did not advance", false);
      break;
    }
    start = next;
  }
  return 0;
}
static void esp_worker(Ctx* ui) {
  constexpr const auto& profile = nzl::game_profile::kOwnerProfile26866884;
  DWORD active_pid = 0;
  std::unique_ptr<nzl::DriverMemory> mem;
  bool mem_ready = false;
  bool profile_identity_checked = false;
  bool profile_identity_ok = false;
  uint64_t ga = 0, klass[7] = {}, game_obj = 0;
  const char* failed_class = nullptr;
  nzl::Offsets off(profile);
  nzl::ChainResult chain;
  PlayerCache player_cache;
  // Projection terms come from the game's own matrix, refreshed once a second
  // so a resolution or FOV change is picked up without a restart.
  std::optional<nzl::ProjectionTerms> projection;
  ULONGLONG last_projection_check = 0;
  char projection_detail[96] = "PROJ guess (not measured yet)";
  while (!g_stop.load()) {
    DWORD pid = find_pid(L"ROB RUN.exe");
    if (!pid) {
      if (mem) { mem->close(); mem.reset(); }
      active_pid = 0; mem_ready = false; profile_identity_checked = false;
      profile_identity_ok = false; ga = 0; game_obj = 0; chain = {}; player_cache = {};
      projection.reset(); last_projection_check = 0;
      std::fill(std::begin(klass), std::end(klass), 0);
      set_runtime(ui, 0, false, "Waiting for ROB RUN process", true);
      Sleep(500); continue;
    }
    if (pid != active_pid) {
      if (mem) { mem->close(); mem.reset(); }
      active_pid = pid; mem_ready = false; profile_identity_checked = false;
      profile_identity_ok = false; ga = 0; game_obj = 0; chain = {}; player_cache = {};
      projection.reset(); last_projection_check = 0;
      std::fill(std::begin(klass), std::end(klass), 0);
      mem = std::make_unique<nzl::DriverMemory>(pid);
      set_runtime(ui, pid, false, "ROB RUN found - connecting to driver", true);
    }
    if (!mem_ready) {
      if (!mem || !mem->init()) {
        set_runtime(ui, pid, false, "Driver not ready - waiting", true);
        Sleep(1200); continue;
      }
      mem_ready = true;
      set_runtime(ui, pid, false, "Driver connected - waiting for GameAssembly", true);
    }
    if (!ga) {
      ga = find_gameassembly(pid);
      if (!ga) {
        set_runtime(ui, pid, false, "Waiting for GameAssembly to finish loading", true);
        Sleep(1000); continue;
      }
    }
    if (!off.profile_configured) {
      set_runtime(ui, pid, false, "Build profile is incomplete; offsets blocked", true);
      Sleep(2000); continue;
    }
    if (!profile_identity_checked) {
      profile_identity_checked = true;
      set_runtime(ui, pid, false, "Checking GameAssembly fingerprint...", false);
      const auto file_check = nzl::windows_profile::verify_gameassembly_file(pid, profile);
      if (!file_check.matched()) {
        std::string status = "Build profile blocked: ";
        status += nzl::windows_profile::status_name(file_check.status);
        status += " size=" + std::to_string(file_check.actual_size) + "/" +
                  std::to_string(profile.fingerprint.gameassembly_file_size);
        if (!file_check.actual_sha256.empty())
          status += " sha256=" + file_check.actual_sha256;
        set_runtime(ui, pid, false, status, true);
        Sleep(2000); continue;
      }
      const auto remote_read = [&](uint64_t address, void* output, size_t size) {
        return mem->read(address, output, size);
      };
      uint32_t image_size = 0;
      if (!nzl::windows_profile::read_loaded_pe_size_of_image(ga, remote_read, image_size) ||
          !nzl::game_profile::type_info_rvas_fit_image(profile, image_size)) {
        char status[192];
        std::snprintf(status, sizeof(status),
                      "Build profile blocked: PE/RVA bounds failed (SizeOfImage=0x%X)",
                      image_size);
        set_runtime(ui, pid, false, status, true);
        Sleep(2000); continue;
      }
      profile_identity_ok = true;
      std::string status = "Profile ";
      status += profile.fingerprint.steam_build_id;
      status += " fingerprint OK; validating TypeInfo classes";
      set_runtime(ui, pid, false, status, true);
    }
    if (!profile_identity_ok) { Sleep(2000); continue; }
    if (!read_classes(*mem, ga, off, profile, klass, failed_class)) {
      std::string status = "Profile class check failed: ";
      status += failed_class ? failed_class : "unknown";
      set_runtime(ui, pid, false, status, true);
      Sleep(1000); continue;
    }
    if (!game_obj) {
      set_runtime(ui, pid, false, "Profile classes OK - waiting for an active match", true);
      game_obj = find_live_game(*mem, pid, klass, off, ui);
      if (!game_obj) {
        if (find_pid(L"ROB RUN.exe") != active_pid) continue;
        set_runtime(ui, pid, false, "Waiting for a class-validated match chain", true);
        Sleep(2500); continue;
      }
    }
    chain = nzl::resolve_chain_from_game(*mem, game_obj, off);
    if (!chain.ok || !chain_classes_match(*mem, chain, klass) ||
        !nzl::read_dict(*mem, chain.dict, off)) {
      game_obj = 0; player_cache = {};
      set_runtime(ui, pid, false, "Build-profile chain/dictionary check failed; reconnecting", true);
      Sleep(1000); continue;
    }
    Ctx work; work.pid = pid; work.mem = mem.get(); work.off = off;
    work.chain = chain; work.player_class = klass[6];
    set_runtime(ui, pid, true, "ESP connected - syncing validated players", false);
    ULONGLONG last_chain_check = 0, last_pid_check = 0;
    ULONGLONG last_car_refresh = 0, last_loot_refresh = 0;
    ULONGLONG last_menu_status_invalidate = 0;
    ULONGLONG perf_window_start = GetTickCount64();
    ULONGLONG last_work_ms = 0;
    uint32_t perf_frames = 0;
    uint32_t camera_read_fail_streak = 0, camera_position_fail_streak = 0;
    uint32_t camera_basis_fail_streak = 0;
    uint64_t camera_read_failures = 0, camera_position_failures = 0;
    uint64_t camera_basis_failures = 0;
    double measured_fps = 0.0;
    bool leave_match = false;
    while (!g_stop.load() && !leave_match) {
      ULONGLONG frame_start = GetTickCount64();
      ULONGLONG now = frame_start;
      if (now - last_pid_check > 1000) {
        last_pid_check = now;
        if (find_pid(L"ROB RUN.exe") != active_pid) { leave_match = true; break; }
      }
      {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        copy_config(work, *ui);
      }
      if (now - last_chain_check > 1500) {
        last_chain_check = now;
        nzl::ChainResult fresh = nzl::resolve_chain_from_game(*mem, game_obj, off);
        const auto fresh_roster = fresh.ok ? nzl::read_dict(*mem, fresh.dict, off)
                                          : std::optional<nzl::DictHeader>{};
        if (!fresh.ok || !chain_classes_match(*mem, fresh, klass) ||
            !fresh_roster || fresh_roster->count <= 0) {
          game_obj = 0; chain = {}; player_cache = {}; leave_match = true;
          set_runtime(ui, pid, false, "Match changed - waiting to reconnect", true);
          break;
        }
        if (fresh.dict != chain.dict || fresh.world_entities != chain.world_entities) {
          player_cache = {};
          work.cars.clear(); work.loot.clear();
          last_car_refresh = last_loot_refresh = 0;
        }
        chain = fresh; work.chain = fresh;
      }
      // Camera state is fetched in one driver round-trip. Keep the legacy
      // forward/right/up fields in the same batch as a fallback if the
      // quaternion is unavailable or the user selects that basis.
      NaItem camera_items[] = {
        {chain.camera_model + off.cam_pos, 0, 12},
        {chain.camera_model + off.cam_look_at_orientation, 12, 16},
        {chain.camera_model + off.cam_fwd, 28, 12},
        {chain.camera_model + off.cam_right, 40, 12},
        {chain.camera_model + off.cam_up, 52, 12},
      };
      uint8_t camera_buf[64]{};
      if (!NaBatchRead(pid, camera_items, (uint32_t)std::size(camera_items), camera_buf, sizeof(camera_buf))) {
        ++camera_read_failures;
        ++camera_read_fail_streak;
        if (camera_read_fail_streak == 5)
          set_runtime(ui, pid, false, "Camera batch read unavailable - ESP paused", false);
        Sleep(100);
        continue;
      }
      camera_read_fail_streak = 0;
      float cpos_raw[3]{}, quaternion[4]{}, forward_raw[3]{}, right_raw[3]{}, up_raw[3]{};
      std::memcpy(cpos_raw, camera_buf, 12);
      std::memcpy(quaternion, camera_buf + 12, 16);
      std::memcpy(forward_raw, camera_buf + 28, 12);
      std::memcpy(right_raw, camera_buf + 40, 12);
      std::memcpy(up_raw, camera_buf + 52, 12);
      nzl::Vec3f cpos{cpos_raw[0], cpos_raw[1], cpos_raw[2]}, cfwd{}, cright{}, cup{};
      if (!std::isfinite(cpos.x) || !std::isfinite(cpos.y) || !std::isfinite(cpos.z)) {
        ++camera_position_failures;
        ++camera_position_fail_streak;
        if (camera_position_fail_streak == 5)
          set_runtime(ui, pid, false, "Camera position invalid - ESP paused", false);
        Sleep(100);
        continue;
      }
      camera_position_fail_streak = 0;
      const auto basis = nzl::select_camera_basis(
          quaternion,
          {forward_raw[0], forward_raw[1], forward_raw[2]},
          {right_raw[0], right_raw[1], right_raw[2]},
          {up_raw[0], up_raw[1], up_raw[2]}, work.basis_quat);
      if (basis.source == nzl::CameraBasisSource::Invalid) {
        ++camera_basis_failures;
        ++camera_basis_fail_streak;
        if (camera_basis_fail_streak == 5)
          set_runtime(ui, pid, false, "Camera basis invalid - ESP paused", false);
        Sleep(100);
        continue;
      }
      cfwd = basis.fwd; cright = basis.right; cup = basis.up;
      camera_basis_fail_streak = 0;
      work.cam_fwd = cfwd; work.cam_right = cright; work.radar_cam = cpos;
      const ULONGLONG projection_now = GetTickCount64();
      if (!last_projection_check || projection_now - last_projection_check >= 1000) {
        projection = nzl::find_projection_matrix(*mem, chain.camera_model);
        last_projection_check = projection_now;
        if (projection)
          std::snprintf(projection_detail, sizeof(projection_detail),
                        "PROJ exact m00=%.4f m11=%.4f @0x%X fovY=%.1f",
                        projection->proj_x, projection->proj_y,
                        projection->offset, projection->fov_y_deg);
        else
          std::snprintf(projection_detail, sizeof(projection_detail),
                        "PROJ guess fov=%.0f (no matrix found)", work.fov);
      }
      auto players = snapshot_players(work, cpos, player_cache, now);
      nzl::CameraView view;
      view.pos = cpos; view.fwd = cfwd; view.right = cright; view.up = cup;
      view.width = std::max(work.width, 1); view.height = std::max(work.height, 1); view.fov_deg = work.fov;
      if (projection) { view.proj_x = projection->proj_x; view.proj_y = projection->proj_y; }
      work.render.clear();
      size_t culled_dead = 0, culled_distance = 0, culled_view = 0, culled_edge = 0;
      const float bone_height[] = {work.off.aim_head_height_m,
                                   work.off.aim_chest_height_m,
                                   work.off.aim_pelvis_height_m};
      int bone = std::clamp(work.aim_bone, 0, 2);
      for (auto& p : players) {
        if (p.dead && !work.show_dead) { ++culled_dead; continue; }
        if (p.dist > work.max_dist) { ++culled_distance; continue; }
        float fx, fy, fd, hx, hy, hd, ax, ay, ad;
        nzl::Vec3f head{p.pos.x, p.pos.y + work.off.head_height_m, p.pos.z};
        nzl::Vec3f aim_point{p.pos.x, p.pos.y + bone_height[bone], p.pos.z};
        if (!nzl::world_to_screen(view, p.pos, fx, fy, fd) || !nzl::world_to_screen(view, head, hx, hy, hd) ||
            !nzl::world_to_screen(view, aim_point, ax, ay, ad)) { ++culled_view; continue; }
        if (work.visible_only && (hx < 0 || hx > view.width || hy < 0 || hy > view.height)) { ++culled_edge; continue; }
        RenderEnt e; e.hx = hx; e.hy = hy; e.fx = fx; e.fy = fy; e.ax = ax; e.ay = ay;
        e.pos = p.pos; e.id = p.id; e.dist = p.dist; e.dead = p.dead;
        for (const auto& cached : player_cache.entries) {
          if (cached.character == p.addr) { e.nickname = cached.nickname; break; }
        }
        e.hp = p.hp; e.maxhp = p.maxhp; e.armor = p.armor; e.role = p.role;
        work.render.push_back(e);
      }
      if (work.show_cars) {
        // Cars move, but do not need a full dictionary walk at render rate.
        if (!last_car_refresh || now - last_car_refresh >= 250) {
          work.cars = snapshot_cars(work, cpos);
          last_car_refresh = now;
        }
        for (auto& car : work.cars) {
          float dx = car.pos.x - cpos.x, dy = car.pos.y - cpos.y, dz = car.pos.z - cpos.z;
          car.dist = std::sqrt(dx * dx + dy * dy + dz * dz);
          float depth = 0; car.on = car.dist <= work.max_dist && nzl::world_to_screen(view, car.pos, car.sx, car.sy, depth);
        }
      } else { work.cars.clear(); last_car_refresh = 0; }
      if (work.show_loot) {
        // Loot is mostly static; refresh the collection once per second and
        // reproject its cached positions every player frame.
        if (!last_loot_refresh || now - last_loot_refresh >= 1000) {
          work.loot = snapshot_loot(work, cpos);
          last_loot_refresh = now;
        }
        for (auto& loot : work.loot) {
          float dx = loot.pos.x - cpos.x, dy = loot.pos.y - cpos.y, dz = loot.pos.z - cpos.z;
          loot.dist = std::sqrt(dx * dx + dy * dy + dz * dz);
          float depth = 0; loot.on = loot.dist <= work.max_dist && nzl::world_to_screen(view, loot.pos, loot.sx, loot.sy, depth);
        }
      } else { work.loot.clear(); last_loot_refresh = 0; }
      int aim_key = kAimKeys[std::clamp(work.aim_key, 0, 3)];
      HWND foreground = GetForegroundWindow(); DWORD foreground_pid = 0;
      if (foreground) GetWindowThreadProcessId(foreground, &foreground_pid);
      if (work.aim && !work.menu && foreground_pid == pid && (GetAsyncKeyState(aim_key) & 0x8000)) {
        float best = work.aim_fov; const RenderEnt* target = nullptr;
        float cx = view.width * 0.5f, cy = view.height * 0.5f;
        for (auto& e : work.render) {
          if (e.dead) continue;
          float dx = e.ax - cx, dy = e.ay - cy, d = std::sqrt(dx * dx + dy * dy);
          if (d < best) { best = d; target = &e; }
        }
        if (target) {
          INPUT in{}; in.type = INPUT_MOUSE; in.mi.dwFlags = MOUSEEVENTF_MOVE;
          in.mi.dx = (LONG)((target->ax - cx) / std::max(work.aim_smooth, 1.0f));
          in.mi.dy = (LONG)((target->ay - cy) / std::max(work.aim_smooth, 1.0f));
          if (in.mi.dx || in.mi.dy) SendInput(1, &in, sizeof(INPUT));
        }
      }
      ULONGLONG sample_time = GetTickCount64();
      last_work_ms = sample_time - frame_start;
      ++perf_frames;
      ULONGLONG sample_elapsed = sample_time - perf_window_start;
      if (sample_elapsed >= 1000) {
        measured_fps = sample_elapsed ? (double)perf_frames * 1000.0 / (double)sample_elapsed : 0.0;
        perf_frames = 0;
        perf_window_start = sample_time;
      }
      double paint_fps = (double)g_overlay_paint_millifps.load(std::memory_order_relaxed) / 1000.0;
      char dictionary_status[64]{};
      if (player_cache.dictionary_header_valid)
        std::snprintf(dictionary_status, sizeof(dictionary_status),
                      "DICT %u/%u active %zu",
                      player_cache.dictionary_count, player_cache.dictionary_capacity,
                      player_cache.dictionary_active_values);
      else
        std::snprintf(dictionary_status, sizeof(dictionary_status), "DICT INVALID");
      char skeleton_status[128]{};
      if (!work.show_skeleton) {
        std::snprintf(skeleton_status, sizeof(skeleton_status), "off");
      } else {
        const auto stage = nzl::skeleton_read_stage_name(
            player_cache.skeleton_last_stage);
        std::snprintf(skeleton_status, sizeof(skeleton_status),
                      "raw %zu/%zu m%zu c%zu n%d h%d %.*s",
                      player_cache.skeleton_valid,
                      player_cache.skeleton_attempted,
                      player_cache.skeleton_model_matches,
                      player_cache.skeleton_description_matches,
                      player_cache.skeleton_bone_count,
                      player_cache.skeleton_head1_index,
                      static_cast<int>(stage.size()), stage.data());
      }
      char basis_status[64]{};
      const char* basis_source = basis.source == nzl::CameraBasisSource::Quaternion ? "Q" : "AXIS";
      if (basis.axes_valid && basis.quaternion_valid)
        std::snprintf(basis_status, sizeof(basis_status), "BASIS %s%s QA %.2f",
                      basis_source, basis.disagreement ? "!" : "",
                      basis.quaternion_axis_alignment);
      else
        std::snprintf(basis_status, sizeof(basis_status), "BASIS %s QA n/a", basis_source);
      char status[768];
      if (measured_fps > 0.0)
        std::snprintf(status, sizeof(status),
                      "DATA %.1f/s | PAINT %.1f/s | WORK %llums\n%s | ROSTER %zu POS %zu DRAW %zu | SKEL %s\nCULLED dead/far/proj/edge %zu/%zu/%zu/%zu | RD-FAIL %zu | CAM-FAIL %llu/%llu/%llu | FOV %.0f VIEW %dx%d %s",
                      measured_fps, paint_fps, (unsigned long long)last_work_ms,
                      dictionary_status, player_cache.entries.size(), players.size(),
                      work.render.size(), skeleton_status, culled_dead, culled_distance,
                      culled_view, culled_edge, player_cache.dictionary_read_failures,
                      (unsigned long long)camera_read_failures,
                      (unsigned long long)camera_position_failures,
                      (unsigned long long)camera_basis_failures,
                      work.fov, view.width, view.height, basis_status);
      else
        std::snprintf(status, sizeof(status),
                      "DATA measuring | PAINT %.1f/s | WORK %llums\n%s | ROSTER %zu POS %zu DRAW %zu | SKEL %s\nCULLED dead/far/proj/edge %zu/%zu/%zu/%zu | RD-FAIL %zu | CAM-FAIL %llu/%llu/%llu | FOV %.0f VIEW %dx%d %s",
                      paint_fps, (unsigned long long)last_work_ms, dictionary_status,
                      player_cache.entries.size(), players.size(), work.render.size(),
                      skeleton_status, culled_dead, culled_distance, culled_view,
                      culled_edge, player_cache.dictionary_read_failures,
                      (unsigned long long)camera_read_failures,
                      (unsigned long long)camera_position_failures,
                      (unsigned long long)camera_basis_failures,
                      work.fov, view.width, view.height, basis_status);
      {
        // Named-stage diagnostics: "нет имён" must come back as the gate that
        // stopped it, and REPOINT/TFAIL show whether transform pointers are
        // being repointed at runtime (the stale-pointer lag).
        const size_t used = std::strlen(status);
        if (used + 96 < sizeof(status))
          std::snprintf(status + used, sizeof(status) - used,
                        "\nNAME %s slot+0x%X dict+0x%X | REPOINT %zu TFAIL %zu\n%s",
                        name_stage_text(player_cache.name_stage),
                        player_cache.name_slots_offset,
                        player_cache.name_dict_offset,
                        player_cache.transform_repoints,
                        player_cache.transform_refresh_failures,
                        projection_detail);
      }
      // Repainting a full-screen layered window every frame makes Windows
      // recomposite the whole surface, which is its own source of stutter.
      // Track the union of this frame's boxes and the previous frame's, and
      // repaint only that, with a full repaint every 30 frames as a safety net.
      RECT frame_bounds{};
      bool has_frame_bounds = false;
      auto include_rect = [&](long left, long top, long right, long bottom) {
        if (left > right) std::swap(left, right);
        if (top > bottom) std::swap(top, bottom);
        if (!has_frame_bounds) {
          frame_bounds = RECT{(LONG)left, (LONG)top, (LONG)right, (LONG)bottom};
          has_frame_bounds = true;
          return;
        }
        frame_bounds.left = std::min(frame_bounds.left, (LONG)left);
        frame_bounds.top = std::min(frame_bounds.top, (LONG)top);
        frame_bounds.right = std::max(frame_bounds.right, (LONG)right);
        frame_bounds.bottom = std::max(frame_bounds.bottom, (LONG)bottom);
      };
      for (const auto& target : work.render) {
        const float height = std::fabs(target.fy - target.hy);
        if (height < 2.0f) continue;
        const float width = height * 0.62f;
        include_rect((long)(target.hx - width), (long)target.hy - 22,
                     (long)(target.hx + width), (long)target.fy + 4);
        const size_t text_len = target.nickname.empty() ? 9 : target.nickname.size() + 9;
        include_rect((long)target.hx, (long)target.hy - 40,
                     (long)target.hx + (long)(text_len * 9), (long)target.hy - 10);
      }
      include_rect(0, 0, work.width, 96); // status block, always redrawn
      if (work.show_radar)
        include_rect(work.width - 220, 0, work.width, 220);
      if (work.tracers) include_rect(0, 0, work.width, work.height);
      HWND menu_wnd = nullptr;
      bool menu_open = false;
      {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        ui->render = work.render; ui->loot = work.loot; ui->cars = work.cars;
        ui->cam_fwd = work.cam_fwd; ui->cam_right = work.cam_right; ui->radar_cam = work.radar_cam;
        ui->attached_pid = pid; ui->esp_ready = true; ui->status = status;
        if (ui->dirty_valid && has_frame_bounds) {
          frame_bounds.left = std::min(frame_bounds.left, ui->dirty.left);
          frame_bounds.top = std::min(frame_bounds.top, ui->dirty.top);
          frame_bounds.right = std::max(frame_bounds.right, ui->dirty.right);
          frame_bounds.bottom = std::max(frame_bounds.bottom, ui->dirty.bottom);
        }
        ui->dirty = has_frame_bounds ? frame_bounds : RECT{};
        ui->dirty_valid = has_frame_bounds;
        menu_wnd = ui->menu_wnd;
        menu_open = ui->menu;
      }
      if (ui->overlay_wnd) {
        const bool full_repaint = !has_frame_bounds || (++ui->dirty_frames % 30) == 0;
        InvalidateRect(ui->overlay_wnd, full_repaint ? nullptr : &ui->dirty, FALSE);
      }
      if (menu_wnd && menu_open && sample_time - last_menu_status_invalidate >= 250) {
        last_menu_status_invalidate = sample_time;
        InvalidateRect(menu_wnd, nullptr, FALSE);
      }
      // Pace towards 60Hz when processing is fast; if remote reads take longer,
      // immediately continue and let the displayed measurement reveal the rate.
      ULONGLONG cycle_ms = GetTickCount64() - frame_start;
      if (cycle_ms < 16) Sleep((DWORD)(16 - cycle_ms));
    }
  }
  if (mem) mem->close();
}
int main() {
  std::printf("[NZL Studio] persistent menu + background ESP reconnect. INSERT=menu, END=exit.\n");
  Ctx ctx; load_settings(ctx); g_ctx = &ctx;
  HINSTANCE instance = GetModuleHandleW(nullptr);
  WNDCLASSW overlayClass{}; overlayClass.lpfnWndProc = overlay_wnd_proc; overlayClass.hInstance = instance; overlayClass.lpszClassName = L"NZLStudioEspOverlay";
  overlayClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); RegisterClassW(&overlayClass);
  WNDCLASSW menuClass{}; menuClass.lpfnWndProc = menu_wnd_proc; menuClass.hInstance = instance; menuClass.lpszClassName = L"NZLStudioInteractiveMenu";
  menuClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); RegisterClassW(&menuClass);
  ctx.overlay_wnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
    overlayClass.lpszClassName, L"NZL ESP", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance, nullptr);
  ctx.menu_wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
    menuClass.lpszClassName, L"NZL Studio", WS_POPUP, ctx.menu_x, ctx.menu_y, kMenuWidth, kMenuHeight, nullptr, nullptr, instance, nullptr);
  if (!ctx.overlay_wnd || !ctx.menu_wnd) { std::printf("[NZL] UI window creation failed.\n"); return 10; }
  SetLayeredWindowAttributes(ctx.overlay_wnd, RGB(0, 0, 0), 0, LWA_COLORKEY);
  ShowWindow(ctx.overlay_wnd, SW_HIDE);
  // Show the menu at startup. An overlay that runs with no visible window is
  // indistinguishable from an overlay that failed to start: the owner ran it,
  // saw nothing, and asked whether it was supposed to open a menu at all
  // (2026-11-28). The overlay window itself still stays hidden until the game
  // window exists.
  ShowWindow(ctx.menu_wnd, SW_SHOW);
  set_menu_visible(true);
  std::printf("[NZL] menu window is open. INSERT toggles it, END exits.\n"
              "[NZL] ESP draws only when ROB RUN.exe is running in Windowed or "
              "Borderless mode; until then this menu just waits.\n");
  std::fflush(stdout);
  std::thread worker(esp_worker, &ctx);
  ULONGLONG last_window_check = 0;
  DWORD window_pid = 0;
  HWND positioned_game = nullptr;
  int overlay_x = 0, overlay_y = 0, overlay_width = 0, overlay_height = 0;
  bool overlay_visible = false;
  int tracked_menu_key = -1;
  bool menu_key_was_down = false, end_key_was_down = false;
  bool done = false;
  MSG msg{};
  while (!done && !g_stop.load()) {
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) {
        std::printf("[NZL] WM_QUIT received; shutting down.\n");
        done = true;
        break;
      }
      TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    int menu_key = VK_INSERT;
    bool menu_open = false;
    { std::lock_guard<std::mutex> lock(g_state_mutex); menu_key = kMenuKeys[std::clamp(ctx.menu_key, 0, 3)]; menu_open = ctx.menu; }
    const bool menu_key_down = (GetAsyncKeyState(menu_key) & 0x8000) != 0;
    if (menu_key != tracked_menu_key) {
      tracked_menu_key = menu_key;
      menu_key_was_down = menu_key_down;
    } else {
      if (menu_key_down && !menu_key_was_down) set_menu_visible(!menu_open);
      menu_key_was_down = menu_key_down;
    }
    const bool end_key_down = (GetAsyncKeyState(VK_END) & 0x8000) != 0;
    if (end_key_down && !end_key_was_down) {
      std::printf("[NZL] END key pressed; shutting down.\n");
      done = true;
      break;
    }
    end_key_was_down = end_key_down;
    ULONGLONG now = GetTickCount64();
    if (now - last_window_check >= 500) {
      last_window_check = now;
      DWORD p = find_pid(L"ROB RUN.exe");
      HWND w = p ? find_game_window(p) : nullptr;
      if (p != window_pid || w != g_game_wnd) { window_pid = p; g_game_wnd = w; }
    }
    if (g_game_wnd && IsWindow(g_game_wnd) && IsWindowVisible(g_game_wnd) &&
        !IsIconic(g_game_wnd)) {
      RECT cr{};
      POINT pt{0, 0};
      if (GetClientRect(g_game_wnd, &cr)) ClientToScreen(g_game_wnd, &pt);
      const int width = std::max<int>(static_cast<int>(cr.right - cr.left), 1);
      const int height = std::max<int>(static_cast<int>(cr.bottom - cr.top), 1);
      {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        ctx.width = width;
        ctx.height = height;
      }
      if (!overlay_visible || positioned_game != g_game_wnd ||
          overlay_x != pt.x || overlay_y != pt.y ||
          overlay_width != width || overlay_height != height) {
        // Keep the existing topmost z-order. Raising this click-through window
        // on every poll used to put it back above the interactive menu.
        SetWindowPos(ctx.overlay_wnd, nullptr, pt.x, pt.y, width, height,
                     SWP_NOACTIVATE | SWP_NOZORDER);
        positioned_game = g_game_wnd;
        overlay_x = pt.x;
        overlay_y = pt.y;
        overlay_width = width;
        overlay_height = height;
      }
      if (!overlay_visible) {
        ShowWindow(ctx.overlay_wnd, SW_SHOWNOACTIVATE);
        overlay_visible = true;
      }
    } else if (overlay_visible) {
      ShowWindow(ctx.overlay_wnd, SW_HIDE);
      overlay_visible = false;
      positioned_game = nullptr;
    }
    Sleep(8);
  }
  g_stop.store(true);
  ClipCursor(nullptr);
  if (worker.joinable()) worker.join();
  if (ctx.menu_wnd && IsWindow(ctx.menu_wnd)) DestroyWindow(ctx.menu_wnd);
  if (ctx.overlay_wnd && IsWindow(ctx.overlay_wnd)) DestroyWindow(ctx.overlay_wnd);
  std::printf("[NZL] Studio closed.\n");
  return 0;
}
