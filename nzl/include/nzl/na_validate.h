/* NZL Studio — portable validation predicates for the Sharedmem Driver.
 *
 * Header-only, no platform dependencies, compiled by:
 *   - nzl/driver/sc_drv.c            (kernel, C)
 *   - nzl/tests/test_protocol.cpp    (host, C++20, fuzzed)
 *
 * Every value that arrives through the shared section is attacker-grade
 * input: a second process can write NA_SHM at any time. With
 * PreviousMode=KernelMode, MmCopyVirtualMemory does NOT probe the
 * addresses it is given; a bad VA is a raw memcpy and an instant bugcheck
 * (BSOD #4: source 0xfffffede9deede7f). So every span must be rejected
 * here, before the copy.
 */
#ifndef NZL_NA_VALIDATE_H
#define NZL_NA_VALIDATE_H

#include "na_wire.h"

/* x64 user-mode address ceiling (canonical low half). */
#define NA_USER_LIMIT 0x0000800000000000ULL

/* True when [addr, addr+size) is entirely inside the user address range
 * and does not wrap. size == 0 is accepted here (a no-op copy); callers
 * that require bytes reject it themselves. */
static inline int na_user_range(NA_U64 addr, NA_U64 size)
{
    if (addr >= NA_USER_LIMIT) return 0;
    if (size > NA_USER_LIMIT - addr) return 0;
    return 1;
}

/* Span containment: [inner, inner+inner_sz) inside [outer, outer+outer_sz).
 * Pure 128-bit-safe arithmetic, no overflow. */
static inline int na_span_within(NA_U64 outer, NA_U64 outer_sz,
                                NA_U64 inner, NA_U64 inner_sz)
{
    NA_U64 outer_end, inner_end;
    if (outer > 0xFFFFFFFFFFFFFFFFULL - outer_sz) return 0;
    if (inner > 0xFFFFFFFFFFFFFFFFULL - inner_sz) return 0;
    outer_end = outer + outer_sz;
    inner_end = inner + inner_sz;
    return inner >= outer && inner_end <= outer_end;
}

/* One NA_ITEM of a BATCH request: non-empty, inside the caller's output
 * buffer (total bytes), inside the user range, no wrap. */
static inline int na_batch_item_ok(const NA_ITEM* it, NA_U32 total)
{
    if (!it) return 0;
    if (!it->size) return 0;
    if ((NA_U64)it->offset > (NA_U64)total) return 0;
    if ((NA_U64)it->size > (NA_U64)total - (NA_U64)it->offset) return 0;
    return na_user_range(it->address, (NA_U64)it->size);
}

/* Whole BATCH request header: bounded count and bounded total. */
static inline int na_batch_header_ok(NA_U32 count, NA_U32 total)
{
    if (!count || count > NA_BATCH_MAX) return 0;
    if (!total || total > NA_MAX_XFER) return 0;
    return 1;
}

/* SCAN candidate budget: 0 is meaningless, more than the payload can hold
 * must be clamped (256 * 8 = 2048 < NA_MAX_XFER). */
static inline NA_U32 na_scan_maxcand(NA_U32 requested)
{
    if (!requested) return NA_SCAN_MAX_CAND;
    return requested > NA_SCAN_MAX_CAND ? NA_SCAN_MAX_CAND : requested;
}

/* Bytes to copy for the chunk that starts at `cursor` in a region ending
 * at `rend`. Returns 0 when no full qword remains. Never exceeds `cap`
 * and never crosses `rend`. */
static inline NA_U32 na_scan_chunk_bytes(NA_U64 cursor, NA_U64 rend, NA_U32 cap)
{
    NA_U64 remain;
    if (!cap) return 0;
    if (cursor > rend) return 0;
    remain = rend - cursor;
    if (remain < 8) return 0;
    return (NA_U32)(remain < (NA_U64)cap ? remain : (NA_U64)cap);
}

/* Cursor advance after a chunk was (partially) consumed: qword aligned,
 * strictly forward, saturating instead of wrapping. */
static inline NA_U64 na_scan_advance(NA_U64 cursor, NA_U64 consumed)
{
    NA_U64 step = consumed & ~7ULL;
    if (!step) step = 8;
    if (cursor > 0xFFFFFFFFFFFFFFFFULL - step) return 0xFFFFFFFFFFFFFFFFULL;
    return cursor + step;
}

/* Scan stop conditions, kept in one place so the driver and the host
 * simulation cannot drift. */
static inline int na_scan_should_stop(NA_U32 found, NA_U32 maxcand,
                                     NA_U64 scanned, NA_U64 max_bytes)
{
    return found >= maxcand || scanned >= max_bytes;
}

/* Readable, committed, private, guard-free mapping — the only regions the
 * pattern scan walks. Kept here so the classification is testable. */
static inline int na_scan_region_ok(NA_U32 state, NA_U32 protect, NA_U32 type)
{
    NA_U32 base_prot = protect & 0xFFu;
    int readable = (base_prot == 0x02u /* PAGE_READONLY   */ ||
                    base_prot == 0x04u /* PAGE_READWRITE  */ ||
                    base_prot == 0x20u /* PAGE_EXECUTE_READ  */ ||
                    base_prot == 0x40u /* PAGE_EXECUTE_READWRITE */ ||
                    base_prot == 0x08u /* PAGE_WRITECOPY  */ ||
                    base_prot == 0x80u /* PAGE_EXECUTE_WRITECOPY */);
    if (state != 0x1000u /* MEM_COMMIT */) return 0;
    if (type != 0x20000u  /* MEM_PRIVATE */) return 0;
    if (protect & 0x100u  /* PAGE_GUARD */) return 0;
    return readable;
}

#endif /* NZL_NA_VALIDATE_H */
