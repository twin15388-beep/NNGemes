#include "nzl/chain.h"

#include <cstdio>

int main() {
  using namespace nzl::game_profile;
  constexpr const GameProfile& profile = kOwnerProfile26866884;
  static_assert(fingerprint_configured(profile));
  static_assert(core_offsets_configured(profile));
  static_assert(type_info_names_configured(profile));
  static_assert(type_info_rvas_fit_image(profile, 0x09000000));
  static_assert(!type_info_rvas_fit_image(profile, 0x00100000));
  static_assert(is_sha256_hex(profile.fingerprint.gameassembly_sha256));

  int failures = 0;
  auto check = [&](bool condition, const char* label) {
    std::printf("%s: %s\n", condition ? "ok" : "FAIL", label);
    if (!condition) ++failures;
  };

  const nzl::Offsets offsets(profile);
  check(offsets.profile_configured, "owner profile is structurally configured");
  check(offsets.game_model == 0xD8, "owner profile selects only Game.GameModel +0xD8");
  check(offsets.dict_entries == 0x18 && offsets.dict_count == 0x20,
        "Dictionary fields are copied from the profile");
  check(offsets.array_length == 0x18 && offsets.array_data == 0x20,
        "IL2CPP array bounds/data are copied from the profile");
  check(offsets.ctm_x == 0x10 && offsets.ctm_y == 0x14 && offsets.ctm_z == 0x18,
        "CharacterTransformModel coordinates are build-profile values");
  check(!offsets.hp_live_verified && !offsets.armor_live_verified &&
            !offsets.role_live_verified && !offsets.loot_live_verified &&
            !offsets.cars_live_verified,
        "unverified optional features remain disabled");

  GameProfile unknown{};
  const nzl::Offsets unset(unknown);
  check(!fingerprint_configured(unknown) && !core_offsets_configured(unknown),
        "unknown profile fails closed");
  check(!unset.profile_configured && unset.game_model == 0,
        "unknown offsets are not silently given owner-build defaults");
  check(nzl::profile_offset32(offset(0, Evidence::CrossChecked, "test")) == 0,
        "explicit zero remains representable");
  check(nzl::profile_offset32(offset(UINT64_MAX, Evidence::CrossChecked, "test")) == 0,
        "offsets wider than uint32_t are rejected");
  const auto heuristic = offset(0xD0, Evidence::Heuristic, "unverified guess");
  check(!offset_usable32(heuristic) && nzl::profile_offset32(heuristic) == 0,
        "heuristic offsets are audit-only and cannot enter runtime offsets");
  GameProfile heuristic_profile = profile;
  heuristic_profile.chain.game_game_model = heuristic;
  check(!core_offsets_configured(heuristic_profile) &&
            !nzl::Offsets(heuristic_profile).profile_configured,
        "a heuristic in a core chain offset fails closed");
  check(!offset_usable32(offset(0x10, Evidence::CrossChecked, "")),
        "an offset without provenance is rejected");

  if (failures) {
    std::printf("%d profile checks failed\n", failures);
    return 1;
  }
  std::printf("ALL PROFILE TESTS PASSED\n");
  return 0;
}
