/* NZL Studio — host tests for the driver wire protocol and its validation.
 *
 * Everything the kernel runs before it touches MmCopyVirtualMemory is
 * compiled here from the SAME headers the driver uses:
 *   nzl/include/nzl/na_wire.h      layout + compile-time ABI asserts
 *   nzl/include/nzl/na_validate.h  address/span/batch predicates
 *   nzl/include/nzl/na_scan.h      the full scan state machine
 *   nzl/include/nzl/na_pebwalk.h   PEB module walk
 *
 * What this proves: the logic terminates, never emits an address outside the
 * user range, never re-emits or skips a candidate, and the client and driver
 * agree on the packed layout. What it does NOT prove: that the live Windows
 * PEB matches the declared offsets, or that the driver binary behaves the
 * same under a real target — that needs the WDK build and a live probe.
 */
#include "nzl/na_wire.h"
#include "nzl/na_validate.h"
#include "nzl/na_scan.h"
#include "nzl/na_pebwalk.h"
#include "nzl/na_klass.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <vector>

/* Type discipline (learned the hard way, MSVC C2664 on the owner's box):
 * NA_U32 is `unsigned long` under LLP64 but `unsigned int` under LP64, and
 * uint32_t is `unsigned int` on both. They are DIFFERENT TYPES with the same
 * size, so `uint32_t*` will not convert to `NA_U32*` under MSVC even though
 * g++ on Linux accepts it silently. Everything in this file that crosses the
 * na_* interface uses NA_U32/NA_U64/NA_U8 exclusively - no <cstdint> names. */
static_assert(sizeof(NA_U32) == 4, "NA_U32 must be 32 bit");
static_assert(sizeof(NA_U64) == 8, "NA_U64 must be 64 bit");
static_assert(sizeof(NA_I32) == 4, "NA_I32 must be 32 bit");

namespace {

int failures = 0;
int checks = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    ++checks;                                                                  \
    if (!(cond)) {                                                             \
      std::printf("FAIL %s  (%s:%d)\n", #cond, __FILE__, __LINE__);            \
      ++failures;                                                              \
    }                                                                          \
  } while (false)

#define CHECK_EQ(a, b)                                                         \
  do {                                                                         \
    ++checks;                                                                  \
    const auto va_ = (a);                                                      \
    const auto vb_ = (b);                                                      \
    if (!(va_ == vb_)) {                                                       \
      std::printf("FAIL %s == %s  (%llu != %llu)  (%s:%d)\n", #a, #b,          \
                  (unsigned long long)va_, (unsigned long long)vb_, __FILE__,  \
                  __LINE__);                                                   \
      ++failures;                                                              \
    }                                                                          \
  } while (false)

NA_U64 g_rng = 0x9E3779B97F4A7C15ULL;
NA_U64 next_rand() {
  g_rng ^= g_rng << 13;
  g_rng ^= g_rng >> 7;
  g_rng ^= g_rng << 17;
  return g_rng;
}

/* ---- synthetic target address space ----------------------------------- */

struct Region {
  NA_U64 base = 0;
  NA_U64 size = 0;
  NA_U32 state = 0x1000;   /* MEM_COMMIT */
  NA_U32 protect = 0x04;   /* PAGE_READWRITE */
  NA_U32 type = 0x20000;   /* MEM_PRIVATE */
  bool unreadable = false;   /* simulate a page that MmCopy cannot read */
};

struct SynthSpace {
  std::map<NA_U64, Region> regions;                 /* keyed by base */
  std::map<NA_U64, std::vector<NA_U8>> mem;       /* keyed by base */
  int region_queries = 0;

  Region& add(NA_U64 base, NA_U64 size, NA_U32 state = 0x1000,
              NA_U32 protect = 0x04, NA_U32 type = 0x20000) {
    Region r;
    r.base = base;
    r.size = size;
    r.state = state;
    r.protect = protect;
    r.type = type;
    regions[base] = r;
    mem[base] = std::vector<NA_U8>(size, 0);
    return regions[base];
  }

  void put_u64(NA_U64 va, NA_U64 value) {
    for (auto& kv : mem) {
      if (va >= kv.first && va + 8 <= kv.first + kv.second.size()) {
        std::memcpy(kv.second.data() + (va - kv.first), &value, 8);
        return;
      }
    }
    std::printf("put outside any region: 0x%llx\n", (unsigned long long)va);
    ++failures;
  }

  void fill(NA_U64 va, const std::vector<NA_U8>& bytes) {
    for (auto& kv : mem) {
      if (va >= kv.first && va + bytes.size() <= kv.first + kv.second.size()) {
        std::memcpy(kv.second.data() + (va - kv.first), bytes.data(),
                    bytes.size());
        return;
      }
    }
    std::printf("fill outside any region: 0x%llx\n", (unsigned long long)va);
    ++failures;
  }

  /* Mirrors ZwQueryVirtualMemory(MemoryBasicInformation): the region that
   * contains `va`, or a MEM_FREE span up to the next described region. */
  int region_cb(NA_U64 va, NA_REGION* out) {
    ++region_queries;
    if (out == nullptr) return 0;
    std::memset(out, 0, sizeof(*out));
    auto it = regions.upper_bound(va);
    if (it != regions.begin()) {
      auto prev = std::prev(it);
      if (va >= prev->second.base && va < prev->second.base + prev->second.size) {
        out->base = prev->second.base;
        out->size = prev->second.size;
        out->state = prev->second.state;
        out->protect = prev->second.protect;
        out->type = prev->second.type;
        return 1;
      }
    }
    /* Nothing described above `va`: a free span up to the user ceiling,
     * exactly like ZwQueryVirtualMemory at the top of the address space. */
    const NA_U64 limit = (it == regions.end()) ? NA_USER_LIMIT : it->first;
    if (limit <= va) return 0;
    out->base = va;
    out->size = limit - va;
    out->state = 0x10000;  /* MEM_FREE */
    out->protect = 0x01;   /* PAGE_NOACCESS */
    out->type = 0;
    return out->size != 0;
  }

  int read_cb(NA_U64 va, void* out, NA_U32 size, NA_U32* copied) {
    *copied = 0;
    if (!out || !size) return 0;
    auto it = regions.upper_bound(va);
    if (it == regions.begin()) return 0;
    auto prev = std::prev(it);
    const Region& r = prev->second;
    if (va < r.base || va >= r.base + r.size) return 0;
    if (r.unreadable) return 0;
    if (!na_scan_region_ok(r.state, r.protect, r.type)) return 0;

    const NA_U64 room = r.base + r.size - va;
    NA_U32 n = size;
    if (room < n) n = static_cast<NA_U32>(room);
    if (!n) return 0;
    const auto& blob = mem.at(r.base);
    std::memcpy(out, blob.data() + (va - r.base), n);
    *copied = n;
    return 1;
  }
};

int synth_region(void* ctx, NA_U64 va, NA_REGION* out) {
  return static_cast<SynthSpace*>(ctx)->region_cb(va, out);
}
int synth_read(void* ctx, NA_U64 va, void* out, NA_U32 size, NA_U32* copied) {
  return static_cast<SynthSpace*>(ctx)->read_cb(va, out, size, copied);
}
int synth_read_full(void* ctx, NA_U64 va, void* out, NA_U32 size) {
  NA_U32 got = 0;
  return synth_read(ctx, va, out, size, &got) && got == size;
}

/* Brute-force reference: every qword-aligned match in every region the
 * classifier accepts. */
std::vector<NA_U64> reference_matches(SynthSpace& space, NA_U64 pattern) {
  std::vector<NA_U64> hits;
  for (const auto& kv : space.regions) {
    const Region& r = kv.second;
    if (!na_scan_region_ok(r.state, r.protect, r.type)) continue;
    auto blob_it = space.mem.find(r.base);
    if (blob_it == space.mem.end()) continue;
    const std::vector<NA_U8>& blob = blob_it->second;
    for (NA_U64 off = 0; off + 8 <= r.size; off += 8) {
      NA_U64 v = 0;
      std::memcpy(&v, blob.data() + off, 8);
      if (v == pattern) hits.push_back(r.base + off);
    }
  }
  std::sort(hits.begin(), hits.end());
  return hits;
}

struct ScanRunner {
  SynthSpace& space;
  std::vector<NA_U8> scratch;
  NA_SCAN_IO io{};

  explicit ScanRunner(SynthSpace& s, NA_U32 chunk = 0x10000,
                      NA_U64 max_bytes = 0)
      : space(s) {
    scratch.assign(chunk ? chunk : 0x10000, 0);
    io.region = synth_region;
    io.read = synth_read;
    io.ctx = &space;
    io.buf = scratch.data();
    io.chunk = chunk;
    io.max_bytes = max_bytes;
  }
};

/* Runs the scan the way the client does: resume until next == 0. */
std::vector<NA_U64> scan_all(ScanRunner& runner, NA_U64 start,
                               NA_U64 pattern, NA_U32 maxcand,
                               NA_U32* calls_out = nullptr) {
  std::vector<NA_U64> all;
  NA_U64 cursor = start;
  NA_U32 calls = 0;
  NA_U64 cands[NA_SCAN_MAX_CAND];
  bool first = true;
  while (first || cursor) {
    first = false;
    NA_U32 found = 0;
    NA_U64 next = 0;
    const int ok = na_scan_run(&runner.io, cursor, pattern, maxcand, cands,
                               &found, &next);
    CHECK(ok);
    if (!ok) {
      std::printf("     scan rejected at cursor=0x%llx\n",
                  (unsigned long long)cursor);
      break;
    }
    ++calls;
    CHECK(found <= na_scan_maxcand(maxcand));
    for (NA_U32 i = 0; i < found; ++i) {
      CHECK(cands[i] < NA_USER_LIMIT);
      all.push_back(cands[i]);
    }
    if (next) {
      /* Invariant 3: strict progress, otherwise a client resume loop hangs. */
      CHECK(next > cursor);
      if (!(next > cursor)) break;
    }
    cursor = next;
    CHECK(calls < 5000);
    if (calls >= 5000) break;
  }
  if (calls_out) *calls_out = calls;
  return all;
}

/* ---- tests ------------------------------------------------------------- */

void test_layout() {
  std::printf("-- layout\n");
  CHECK_EQ(static_cast<int>(sizeof(NA_ITEM)), 16);
  CHECK_EQ(static_cast<int>(sizeof(NA_REGION)), 32);
  CHECK_EQ(static_cast<int>(sizeof(NA_SHM)), 0x10454);
  CHECK_EQ(static_cast<unsigned long long>(offsetof(NA_SHM, seq)), NA_SHM_OFF_SEQ);
  CHECK_EQ(static_cast<unsigned long long>(offsetof(NA_SHM, key)), NA_SHM_OFF_KEY);
  CHECK_EQ(static_cast<unsigned long long>(offsetof(NA_SHM, address)), NA_SHM_OFF_ADDRESS);
  CHECK_EQ(static_cast<unsigned long long>(offsetof(NA_SHM, status)), NA_SHM_OFF_STATUS);
  CHECK_EQ(static_cast<unsigned long long>(offsetof(NA_SHM, region)), NA_SHM_OFF_REGION);
  CHECK_EQ(static_cast<unsigned long long>(offsetof(NA_SHM, items)), NA_SHM_OFF_ITEMS);
  CHECK_EQ(static_cast<unsigned long long>(offsetof(NA_SHM, data)), NA_SHM_OFF_DATA);
  /* The two crash dumps in HANDOFF (0x1E write at ...0034 and 0x50 read at
   * ...0454) land exactly on NA_SHM.region and NA_SHM.data, so those two
   * offsets are the ones to keep pinned. */
  CHECK_EQ(static_cast<int>(NA_SHM_OFF_REGION), 0x34);
  CHECK_EQ(static_cast<int>(NA_SHM_OFF_DATA), 0x454);
}

void test_user_range() {
  std::printf("-- na_user_range\n");
  CHECK(na_user_range(0x1000, 0x1000));
  CHECK(na_user_range(0, 8));
  CHECK(!na_user_range(NA_USER_LIMIT, 1));
  CHECK(!na_user_range(NA_USER_LIMIT - 1, 2));
  CHECK(na_user_range(NA_USER_LIMIT - 8, 8));
  CHECK(!na_user_range(0xFFFFFFFFFFFFFFFFULL, 1));
  CHECK(!na_user_range(0x7FFFFFFFFFFFFFFFULL, 0x1000));
  CHECK(!na_user_range(0xFFFFFEDE9DEEDE7FULL, 8));  /* BSOD #4 source VA */
  CHECK(na_user_range(0x1000, 0));

  for (int i = 0; i < 200000; ++i) {
    NA_U64 addr = next_rand();
    NA_U64 size = next_rand() % 0x100000;
    if (i & 1) size = next_rand() >> 40;
    bool ref = addr < NA_USER_LIMIT && size <= NA_USER_LIMIT - addr;
    bool got = na_user_range(addr, size) != 0;
    if (ref != got) {
      std::printf("FAIL random range addr=0x%llx size=0x%llx ref=%d got=%d\n",
                  (unsigned long long)addr, (unsigned long long)size, (int)ref,
                  (int)got);
      ++failures;
      return;
    }
    ++checks;
  }
}

void test_span_and_batch() {
  std::printf("-- na_span_within / batch\n");
  CHECK(na_span_within(0x1000, 0x1000, 0x1100, 0x100));
  CHECK(!na_span_within(0x1000, 0x1000, 0x1F00, 0x200));
  CHECK(!na_span_within(0xFFFFFFFFFFFFFF00ULL, 0x200, 0xFFFFFFF0, 8));

  NA_ITEM it{};
  it.address = 0x1000; it.offset = 0; it.size = 8;
  CHECK(na_batch_item_ok(&it, 0x1000));
  it.size = 0;
  CHECK(!na_batch_item_ok(&it, 0x1000));
  it.size = 8;
  it.offset = 0xFF8;
  CHECK(na_batch_item_ok(&it, 0x1000));
  it.offset = 0xFFC;
  CHECK(!na_batch_item_ok(&it, 0x1000));      /* 8 bytes would overrun total */
  it.offset = 0x2000;
  CHECK(!na_batch_item_ok(&it, 0x1000));
  it.offset = 0;
  it.address = NA_USER_LIMIT;
  CHECK(!na_batch_item_ok(&it, 0x1000));
  it.address = NA_USER_LIMIT - 4;
  CHECK(!na_batch_item_ok(&it, 0x1000));      /* span crosses the ceiling */
  it.address = NA_USER_LIMIT - 8;
  CHECK(na_batch_item_ok(&it, 0x1000));

  CHECK(na_batch_header_ok(1, 1));
  CHECK(!na_batch_header_ok(0, 1));
  CHECK(!na_batch_header_ok(NA_BATCH_MAX + 1, 1));
  CHECK(!na_batch_header_ok(1, 0));
  CHECK(!na_batch_header_ok(1, NA_MAX_XFER + 1));

  for (int i = 0; i < 100000; ++i) {
    NA_ITEM item{};
    NA_U32 total = static_cast<NA_U32>(next_rand() % 0x20000);
    item.address = next_rand() % (NA_USER_LIMIT + 0x1000);
    item.offset = static_cast<NA_U32>(next_rand() % 0x20000);
    item.size = static_cast<NA_U32>(next_rand() % 0x20000);
    bool ref = item.size != 0 && static_cast<NA_U64>(item.offset) <= total &&
               static_cast<NA_U64>(item.size) <=
                   static_cast<NA_U64>(total) - item.offset &&
               item.address < NA_USER_LIMIT &&
               static_cast<NA_U64>(item.size) <= NA_USER_LIMIT - item.address;
    bool got = na_batch_item_ok(&item, total) != 0;
    if (ref != got) {
      std::printf("FAIL batch addr=0x%llx off=0x%x size=0x%x total=0x%x\n",
                  (unsigned long long)item.address, item.offset, item.size,
                  total);
      ++failures;
      return;
    }
    ++checks;
  }
}

void test_region_classifier() {
  std::printf("-- na_scan_region_ok\n");
  const NA_U32 readable[] = {0x02, 0x04, 0x08, 0x20, 0x40, 0x80};
  for (NA_U32 p : readable) CHECK(na_scan_region_ok(0x1000, p, 0x20000));
  CHECK(!na_scan_region_ok(0x1000, 0x01, 0x20000));            /* NOACCESS */
  CHECK(!na_scan_region_ok(0x1000, 0x04, 0x1000000));          /* MEM_IMAGE */
  CHECK(!na_scan_region_ok(0x1000, 0x04, 0x40000));            /* MEM_MAPPED */
  CHECK(!na_scan_region_ok(0x10000, 0x04, 0x20000));           /* MEM_FREE */
  CHECK(!na_scan_region_ok(0x1000, 0x04 | 0x100, 0x20000));    /* GUARD */
  CHECK(na_scan_region_ok(0x1000, 0x04 | 0x200, 0x20000));     /* NOCACHE ok */
}

void test_scan_completeness() {
  std::printf("-- scan completeness vs brute force\n");
  SynthSpace space;
  space.add(0x100000, 0x20000);                        /* private RW  */
  space.add(0x140000, 0x10000, 0x1000, 0x01);          /* NOACCESS     */
  space.add(0x160000, 0x20000, 0x1000, 0x40);          /* private RWX  */
  space.add(0x1A0000, 0x10000, 0x1000, 0x04, 0x1000000); /* MEM_IMAGE  */
  space.add(0x1C0000, 0x10000, 0x1000, 0x04);          /* private RW   */
  space.add(0x220000, 0x8000, 0x1000, 0x04);           /* private RW   */

  const NA_U64 pattern = 0xDEADBEEFCAFEBABEULL;
  space.put_u64(0x100000, pattern);
  space.put_u64(0x100FF8, pattern);
  space.put_u64(0x11FFF8, pattern);          /* last qword of the region */
  space.put_u64(0x140000, pattern);          /* NOACCESS: must be ignored */
  space.put_u64(0x160010, pattern);          /* RWX: must be found */
  space.put_u64(0x1A0010, pattern);          /* MEM_IMAGE: ignored */
  space.put_u64(0x1C0000, pattern);
  space.put_u64(0x220010, pattern);
  space.put_u64(0x220018, pattern);          /* adjacent pair */

  auto ref = reference_matches(space, pattern);
  CHECK_EQ(ref.size(), static_cast<size_t>(7));

  ScanRunner runner(space);
  NA_U32 calls = 0;
  auto got = scan_all(runner, 0x100000, pattern, 256, &calls);
  std::sort(got.begin(), got.end());
  CHECK_EQ(got.size(), ref.size());
  CHECK(got == ref);

  /* Same scan from address 0: it must walk the free hole in front. */
  ScanRunner runner2(space);
  auto got2 = scan_all(runner2, 0, pattern, 256);
  std::sort(got2.begin(), got2.end());
  CHECK(got2 == ref);

  /* Small chunks must not change the result set. */
  const NA_U32 chunks[] = {8u, 24u, 64u, 4096u, 0x10000u};
  for (unsigned ci = 0; ci < sizeof(chunks) / sizeof(chunks[0]); ++ci) {
    ScanRunner r3(space, chunks[ci]);
    auto g = scan_all(r3, 0, pattern, 256);
    std::sort(g.begin(), g.end());
    CHECK(g == ref);
  }
}

void test_scan_resume_no_livelock() {
  std::printf("-- scan resume (regression: infinite resume VA)\n");
  SynthSpace space;
  space.add(0x100000, 0x10000);
  const NA_U64 pattern = 0x1122334455667788ULL;
  std::vector<NA_U64> expected;
  for (int i = 0; i < 10; ++i) {
    NA_U64 va = 0x100000 + i * 0x1000;
    space.put_u64(va, pattern);
    expected.push_back(va);
  }

  /* maxcand = 1 inside one 64 KB chunk: the old code returned the chunk
   * start as the resume VA and never terminated. */
  ScanRunner runner(space);
  NA_U64 cursor = 0x100000;
  std::vector<NA_U64> got;
  NA_U32 calls = 0;
  while (cursor && calls < 64) {
    NA_U64 cands[NA_SCAN_MAX_CAND];
    NA_U32 found = 0;
    NA_U64 next = 0;
    CHECK(na_scan_run(&runner.io, cursor, pattern, 1, cands, &found, &next));
    ++calls;
    if (found) got.push_back(cands[0]);
    CHECK(found <= 1);
    if (next) CHECK(next > cursor);
    cursor = next;
  }
  CHECK_EQ(got.size(), expected.size());
  CHECK(got == expected);
  CHECK_EQ(calls, 11);   /* 10 hits + the exhausting call */
}

void test_scan_unreadable() {
  std::printf("-- scan with unreadable pages\n");
  SynthSpace space;
  auto& r1 = space.add(0x100000, 0x20000);
  space.add(0x140000, 0x20000);
  space.add(0x180000, 0x20000);
  space.put_u64(0x100010, 0xABCDEF0123456789ULL);
  space.put_u64(0x140010, 0xABCDEF0123456789ULL);
  space.put_u64(0x180010, 0xABCDEF0123456789ULL);
  r1.unreadable = true;   /* classified readable, but MmCopy would fail */
  space.regions[0x140000].unreadable = true;

  ScanRunner runner(space, 4096);
  auto got = scan_all(runner, 0x100000, 0xABCDEF0123456789ULL, 256);
  std::sort(got.begin(), got.end());
  CHECK_EQ(got.size(), static_cast<size_t>(1));
  if (got.size() == 1) CHECK_EQ(got[0], 0x180010ULL);
}

void test_scan_budget() {
  std::printf("-- scan byte budget\n");
  SynthSpace space;
  space.add(0x100000, 0x100000);
  const NA_U64 pattern = 0x5555AAAA5555AAAAULL;
  for (int i = 0; i < 8; ++i) space.put_u64(0x100000 + i * 0x20000, pattern);

  /* 64 KB budget per call: the scan must stop early, return a resume VA and
   * still find all 8 matches across calls. */
  ScanRunner runner(space, 0x10000, 0x10000);
  NA_U32 calls = 0;
  auto got = scan_all(runner, 0x100000, pattern, 256, &calls);
  std::sort(got.begin(), got.end());
  CHECK_EQ(got.size(), static_cast<size_t>(8));
  CHECK(calls >= 8);   /* 1 MB / 64 KB budget */
}

void test_degenerate_args() {
  std::printf("-- degenerate arguments\n");
  SynthSpace space;
  space.add(0x100000, 0x1000);
  ScanRunner runner(space);
  NA_U64 cands[NA_SCAN_MAX_CAND];
  NA_U32 found = 123;
  NA_U64 next = 456;

  CHECK(!na_scan_run(nullptr, 0x100000, 0, 1, cands, &found, &next));
  CHECK(!na_scan_run(&runner.io, NA_USER_LIMIT, 0, 1, cands, &found, &next));
  CHECK(!na_scan_run(&runner.io, 0xFFFFFFFFFFFFFFFFULL, 0, 1, cands, &found, &next));
  CHECK_EQ(na_scan_maxcand(0), NA_SCAN_MAX_CAND);
  CHECK_EQ(na_scan_maxcand(1), 1u);
  CHECK_EQ(na_scan_maxcand(100000), NA_SCAN_MAX_CAND);
  CHECK_EQ(na_scan_chunk_bytes(0x1000, 0x1007, 0x10000), 0u);
  CHECK_EQ(na_scan_chunk_bytes(0x1000, 0x1008, 0x10000), 8u);
  CHECK_EQ(na_scan_chunk_bytes(0x1000, 0x9000, 0x1000), 0x1000u);
  CHECK_EQ(na_scan_advance(0x1000, 0), 0x1008ULL);
  CHECK_EQ(na_scan_advance(0x1000, 5), 0x1008ULL);
  CHECK_EQ(na_scan_advance(0x1004, 9), 0x100CULL);   /* 9 -> one qword */

  /* Empty address space: must terminate immediately, not spin. */
  SynthSpace empty;
  ScanRunner empty_runner(empty);
  auto none = scan_all(empty_runner, 0x100000, 0x1122334455667788ULL, 256);
  CHECK(none.empty());
}

void test_peb_walk() {
  std::printf("-- PEB module walk\n");
  const NA_U64 peb = 0x200000;
  const NA_U64 ldr = 0x300000;
  const NA_U64 head = ldr + NA_LDR_INLOADORDER;
  const NA_U64 e1 = 0x400000;
  const NA_U64 e2 = 0x410000;
  const NA_U64 name1 = 0x500000;   /* L"GameAssembly.dll"  */
  const NA_U64 name2 = 0x510000;   /* L"UnityPlayer.dll"   */
  const NA_U64 base1 = 0x7FFB10000000ULL;
  const NA_U64 base2 = 0x7FFB20000000ULL;

  auto put = [](SynthSpace& s, NA_U64 va, NA_U64 v) { s.put_u64(va, v); };
  SynthSpace space;
  space.add(0x200000, 0x1000);
  space.add(0x300000, 0x1000);
  space.add(0x400000, 0x1000);
  space.add(0x410000, 0x1000);
  space.add(0x500000, 0x1000);
  space.add(0x510000, 0x1000);

  auto wstr = [](const char* ascii) {
    std::vector<NA_U8> out;
    for (const char* p = ascii; *p; ++p) {
      out.push_back(static_cast<NA_U8>(*p));
      out.push_back(0);
    }
    return out;
  };

  put(space, peb + NA_PEB_LDR, ldr);
  put(space, head, e1);                                  /* head->Flink = e1 */
  put(space, e1 + 0x00, e2);                             /* e1->Flink = e2   */
  put(space, e1 + NA_LDR_ENTRY_DLLBASE, base1);
  put(space, e1 + NA_LDR_ENTRY_SIZE, 0x8D40000);
  put(space, e1 + NA_LDR_ENTRY_NAMEDLL,
      static_cast<NA_U64>(std::strlen("GameAssembly.dll") * 2));
  space.put_u64(e1 + NA_LDR_ENTRY_NAMEDLL + NA_UNICODE_STRING_BUF, name1);
  space.fill(name1, wstr("GameAssembly.dll"));

  put(space, e2 + 0x00, head);                           /* e2->Flink = head */
  put(space, e2 + NA_LDR_ENTRY_DLLBASE, base2);
  put(space, e2 + NA_LDR_ENTRY_SIZE, 0x22D4000);
  put(space, e2 + NA_LDR_ENTRY_NAMEDLL,
      static_cast<NA_U64>(std::strlen("UnityPlayer.dll") * 2));
  space.put_u64(e2 + NA_LDR_ENTRY_NAMEDLL + NA_UNICODE_STRING_BUF, name2);
  space.fill(name2, wstr("UnityPlayer.dll"));

  auto wide = [](const char* ascii, std::vector<unsigned short>& out) {
    out.clear();
    for (const char* p = ascii; *p; ++p) out.push_back(static_cast<unsigned short>(*p));
  };

  std::vector<unsigned short> want;
  NA_MODULE_INFO info{};

  wide("GameAssembly.dll", want);
  CHECK(na_peb_find_module(synth_read_full, &space, peb, want.data(),
                           static_cast<NA_U32>(want.size()), &info));
  CHECK_EQ(info.base, base1);
  CHECK_EQ(info.size, 0x8D40000u);

  wide("unityplayer.dll", want);   /* case-insensitive */
  CHECK(na_peb_find_module(synth_read_full, &space, peb, want.data(),
                           static_cast<NA_U32>(want.size()), &info));
  CHECK_EQ(info.base, base2);

  wide("NotLoaded.dll", want);
  CHECK(!na_peb_find_module(synth_read_full, &space, peb, want.data(),
                            static_cast<NA_U32>(want.size()), &info));

  /* A corrupt list must terminate: entry->Flink pointing at itself must not
   * spin the kernel thread, and a name that is not in the list must miss. */
  put(space, e1 + 0x00, e1);
  put(space, head, e1);
  wide("NotLoaded.dll", want);
  CHECK(!na_peb_find_module(synth_read_full, &space, peb, want.data(),
                            static_cast<NA_U32>(want.size()), &info));
  CHECK_EQ(info.base, base2);   /* untouched on miss */
  put(space, e1 + 0x00, e2);    /* restore the list */
  CHECK(space.region_queries >= 0);

  /* Kernel-range PEB must be refused before any read. */
  wide("GameAssembly.dll", want);
  CHECK(!na_peb_find_module(synth_read_full, &space, 0xFFFFF80000000000ULL,
                            want.data(), static_cast<NA_U32>(want.size()), &info));
}

}  // namespace

void test_klass_identify() {
  std::printf("-- class identification (na_klass)\n");
  const NA_U64 image = 0x10000000;
  const NA_U64 klass = 0x20000000;
  const NA_U64 name = 0x30000000;
  const NA_U64 slot = image + 0x1000;
  const NA_U32 name_off = 0x10;

  SynthSpace space;
  space.add(image, 0x4000);
  space.add(klass, 0x1000);
  space.add(name, 0x1000);
  space.put_u64(slot, klass);
  space.put_u64(klass + name_off, name);
  space.fill(name, std::vector<NA_U8>{'G', 'a', 'm', 'e', 0});
  space.put_u64(klass + 0x00, 0x40000000);   /* image pointer, not text */
  space.put_u64(klass + name_off + 8, 0x0);

  /* text rules: printable, >= 2 chars, NUL-terminated */
  CHECK_EQ(na_klass_text_len("Game", 32), 4u);
  CHECK_EQ(na_klass_text_len("G", 32), 0u);
  CHECK_EQ(na_klass_text_len("", 32), 0u);
  CHECK_EQ(na_klass_text_len("Ga\x01me", 32), 0u);

  /* exact, case-sensitive name match at the configured offset */
  CHECK(na_klass_is(synth_read_full, &space, klass, name_off, "Game"));
  CHECK(!na_klass_is(synth_read_full, &space, klass, name_off, "GameModel"));
  CHECK(!na_klass_is(synth_read_full, &space, klass, name_off, "game"));
  CHECK(!na_klass_is(synth_read_full, &space, klass, 0x18, "Game"));

  std::vector<NA_U8> page(NA_KLASS_PAGE_BYTES);
  NA_U32 examined = 0;
  NA_U64 found_rva = 0;

  /* finds the slot that leads to the wanted class, reports image-relative rva */
  CHECK(na_klass_sweep(synth_read_full, &space, image, 0x1000, 0x800, name_off, "Game",
                       NA_KLASS_DEFAULT_CANDIDATES, page.data(), &examined, &found_rva));
  CHECK_EQ(found_rva, 0x1000u);
  CHECK(examined >= 1u);

  /* a slot with the wrong class is not reported as a match */
  examined = 0;
  CHECK(!na_klass_sweep(synth_read_full, &space, image, 0x1000, 0x800, name_off, "GameModel",
                        NA_KLASS_DEFAULT_CANDIDATES, page.data(), &examined, &found_rva));
  CHECK_EQ(found_rva, 0u);

  /* a window that would start below the image is refused, not wrapped */
  CHECK(!na_klass_sweep(synth_read_full, &space, image, 0x10, 0x800, name_off, "Game",
                        NA_KLASS_DEFAULT_CANDIDATES, page.data(), &examined, &found_rva));
  CHECK(!na_klass_sweep(synth_read_full, &space, image, 0x1000, 0, name_off, "Game",
                        NA_KLASS_DEFAULT_CANDIDATES, page.data(), &examined, &found_rva));

  /* the candidate budget is a hard cap: user-range pointers that are not
   * readable still cost a name read, so they must be counted and stopped */
  SynthSpace cap;
  cap.add(image, 0x2000);
  for (NA_U32 i = 0; i < 512; ++i)
    cap.put_u64(image + i * 8, 0x50000000ULL + i * 0x1000ULL);  /* nothing mapped */
  examined = 0;
  NA_U32 cap_examined = 0;
  CHECK(!na_klass_sweep(synth_read_full, &cap, image, 0x1000, 0x800, name_off, "Game",
                        5, page.data(), &cap_examined, &found_rva));
  CHECK_EQ(cap_examined, 5u);
}

int main() {
  std::printf("NA_SHM layout: sizeof=0x%X region=0x%X items=0x%X data=0x%X\n",
              static_cast<unsigned>(sizeof(NA_SHM)), NA_SHM_OFF_REGION,
              NA_SHM_OFF_ITEMS, NA_SHM_OFF_DATA);
  test_layout();
  test_user_range();
  test_span_and_batch();
  test_region_classifier();
  test_scan_completeness();
  test_scan_resume_no_livelock();
  test_scan_unreadable();
  test_scan_budget();
  test_degenerate_args();
  test_peb_walk();
  test_klass_identify();

  std::printf("%d checks, %d failures\n", checks, failures);
  if (failures) {
    std::printf("PROTOCOL TESTS FAILED\n");
    return 1;
  }
  std::printf("ALL PROTOCOL TESTS PASSED\n");
  return 0;
}
