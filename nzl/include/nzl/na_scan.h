/* NZL Studio — kernel-side pattern scan, portable state machine.
 *
 * The scan loop used to live inline in NaScan() and had one real defect:
 * when the candidate budget filled up in the middle of a 64 KB chunk the
 * resume VA was the chunk START (`cur`), so the next call re-examined the
 * same chunk, re-emitted the same candidates, returned the same resume VA
 * again, and the client's `while (next) { ... }` loop never terminated —
 * an ESP/discovery scan that spins the driver worker forever.
 *
 * The loop is now a pure state machine over two callbacks, so the exact
 * same code is executed by the driver (MmCopyVirtualMemory adapter) and
 * by the host test (synthetic address space, brute-force reference).
 *
 * Invariants, all asserted by nzl/tests/test_protocol.cpp:
 *   1. Every emitted candidate is a qword-aligned match inside a region
 *      accepted by na_scan_region_ok().
 *   2. `out_next` is the first VA NOT yet examined: resuming there emits
 *      no duplicate and skips no match.
 *   3. `out_next > start` whenever `*out_next != 0` (strict progress), so
 *      a client resume loop always terminates.
 *   4. `*out_next == 0` means the address space above `start` is exhausted.
 *   5. Bounded: `max_bytes` limits copied bytes, `maxcand` limits output,
 *      region queries are capped by NA_SCAN_MAX_REGIONS.
 *
 * Callback contracts:
 *   region(): must not leave the target process attached on return and
 *             must write results only into the caller's locals.
 *   read():   copies at most the requested bytes; *copied is set to the
 *             number of bytes actually copied (0 on failure).
 */
#ifndef NZL_NA_SCAN_H
#define NZL_NA_SCAN_H

#include "na_validate.h"

#define NA_SCAN_MAX_REGIONS 200000u   /* runaway guard on region queries */
#define NA_SCAN_CHUNK_DEFAULT 0x10000u
/* Per-command copy budget. Deliberately far below the client's 2000 ms
 * Send() timeout: at 64 KB per MmCopyVirtualMemory call this is ~512
 * copies, and the resume VA carries the rest to the next command. */
#define NA_SCAN_MAX_BYTES_DEFAULT (32ULL * 1024 * 1024)

typedef struct _NA_SCAN_IO {
    /* 1 on success, 0 when the region query failed hard. */
    int (*region)(void* ctx, NA_U64 va, NA_REGION* out);
    /* 1 on success, 0 on failure. *copied may be < size on partial read. */
    int (*read)(void* ctx, NA_U64 va, void* out, NA_U32 size, NA_U32* copied);
    void* ctx;
    void* buf;         /* scratch buffer, at least `chunk` bytes            */
    NA_U32 chunk;      /* 0 -> NA_SCAN_CHUNK_DEFAULT */
    NA_U64 max_bytes;  /* 0 -> NA_SCAN_MAX_BYTES_DEFAULT */
} NA_SCAN_IO;

/* Returns 1 when the scan completed (including the exhausted case),
 * 0 when a region query failed hard or arguments were rejected.
 * out_cands must hold at least maxcand qwords (clamped to
 * NA_SCAN_MAX_CAND). */
static inline int na_scan_run(const NA_SCAN_IO* io, NA_U64 start,
                              NA_U64 pattern, NA_U32 maxcand, NA_U64* out_cands,
                              NA_U32* out_found, NA_U64* out_next)
{
    NA_U32 found = 0;
    NA_U64 scanned = 0;
    NA_U64 cursor;
    NA_U64 chunk;
    NA_U64 max_bytes;
    NA_U32 budget;
    NA_U32 guard = 0;

    if (!io || !io->region || !io->read || !io->buf || !out_cands ||
        !out_found || !out_next)
        return 0;   /* a null scratch buffer would be copied into */
    *out_found = 0;
    *out_next = 0;
    if (!na_user_range(start, 8)) return 0;

    budget = na_scan_maxcand(maxcand);
    chunk = io->chunk ? io->chunk : NA_SCAN_CHUNK_DEFAULT;
    /* qword-aligned stepping only: the scan matches on 8-byte boundaries. */
    chunk &= ~7ULL;
    if (chunk < 8) chunk = 8;
    if (chunk > 0x00100000ULL) chunk = 0x00100000ULL;
    max_bytes = io->max_bytes ? io->max_bytes : NA_SCAN_MAX_BYTES_DEFAULT;
    cursor = start;

    while (cursor < NA_USER_LIMIT) {
        NA_REGION reg;
        NA_U64 rend;

        if (na_scan_should_stop(found, budget, scanned, max_bytes)) break;
        if (++guard > NA_SCAN_MAX_REGIONS) break;

        reg.base = reg.size = 0;
        reg.state = reg.protect = reg.type = reg.pad = 0;
        /* The region callback models ZwQueryVirtualMemory exactly: an
         * unmapped VA yields a MEM_FREE span (never "not found"), and the
         * top of the address space yields a final span that ends at the
         * user ceiling. A 0 return is therefore a hard query failure and
         * aborts the scan instead of silently reporting a short result. */
        if (!io->region(io->ctx, cursor, &reg)) return 0;
        if (!reg.size) return 0;                       /* no forward motion */
        if (reg.base > 0xFFFFFFFFFFFFFFFFULL - reg.size) return 0;
        rend = reg.base + reg.size;
        if (rend <= cursor) return 0;                  /* region ends behind us */

        if (!na_scan_region_ok(reg.state, reg.protect, reg.type)) {
            cursor = rend;
            continue;
        }

        for (;;) {
            NA_U32 want, copied = 0;
            NA_U64 step;

            if (na_scan_should_stop(found, budget, scanned, max_bytes))
                goto done;
            want = na_scan_chunk_bytes(cursor, rend, (NA_U32)chunk);
            if (!want) { cursor = rend; break; }        /* region tail < qword */
            if (!na_user_range(cursor, (NA_U64)want)) { cursor = rend; break; }

            scanned += want;
            if (!io->read(io->ctx, cursor, io->buf ? io->buf : 0, want, &copied)) {
                /* Unreadable page: step one chunk, never stall. */
                cursor = na_scan_advance(cursor, want);
                continue;
            }

            if (copied >= 8) {
                NA_U64 o;
                for (o = 0; o + 8 <= (NA_U64)copied; o += 8) {
                    NA_U64 value = 0;
                    NA_U8* p = (NA_U8*)io->buf;
                    NA_U8 i;
                    for (i = 0; i < 8; i++) value |= (NA_U64)p[o + i] << (8u * i);
                    if (value != pattern) continue;
                    out_cands[found] = cursor + o;
                    found++;
                    if (found >= budget) {
                        cursor = cursor + o + 8;        /* resume after hit */
                        goto done;
                    }
                }
                step = copied;
            } else {
                step = want;
            }
            cursor = na_scan_advance(cursor, step);
            if (cursor >= rend) { cursor = rend; break; }
        }
    }

done:
    *out_found = found;
    *out_next = (cursor >= NA_USER_LIMIT) ? 0 : cursor;
    if (*out_next && *out_next <= start) return 0;      /* no progress: bug */
    return 1;
}

#endif /* NZL_NA_SCAN_H */
