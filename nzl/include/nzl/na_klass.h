/* NZL Studio — IL2CPP class identification, portable and host-tested.
 *
 * The live probe stopped with
 *   [profile] TypeInfo Game rva=0x81B7710 klass=0x23CA2111E30 NAME_MISMATCH
 * and that line alone cannot say which of three things is wrong:
 *   1. the RVA does not point at a class-pointer slot at all,
 *   2. it points at a class-pointer slot for a different class,
 *   3. it is right and the `name` field offset used for the check is wrong.
 *
 * This header answers that by measuring instead of assuming: read the string
 * the slot really leads to, and sweep a bounded window around the claimed RVA
 * for a qword that leads to a class whose name equals the wanted name. The
 * sweep is bounded in bytes, in candidates and in pages so the probe cannot
 * turn back into the 238-second region scan.
 *
 * Same pattern as na_pebwalk.h: pure logic over a read callback, exercised in
 * nzl/tests/test_protocol.cpp against synthetic memory. The probe supplies
 * NaRead as the callback; the driver does not use this at all.
 */
#ifndef NZL_NA_KLASS_H
#define NZL_NA_KLASS_H

#include "na_validate.h"

#define NA_KLASS_MAX_NAME_BYTES   96u
#define NA_KLASS_MIN_CHARS        2u
#define NA_KLASS_PAGE_BYTES       4096u
#define NA_KLASS_DEFAULT_WINDOW   0x2000u
#define NA_KLASS_DEFAULT_CANDIDATES 600u

/* read(): full-or-fail copy from the target process. Returns 1/0. */
typedef int (*na_klass_read_fn)(void* ctx, NA_U64 va, void* out, NA_U32 size);

/* Little-endian load from an already-fetched page buffer. */
static inline NA_U64 na_klass_le_u64(const NA_U8* p)
{
    return (NA_U64)p[0] | ((NA_U64)p[1] << 8) | ((NA_U64)p[2] << 16) |
           ((NA_U64)p[3] << 24) | ((NA_U64)p[4] << 32) | ((NA_U64)p[5] << 40) |
           ((NA_U64)p[6] << 48) | ((NA_U64)p[7] << 56);
}

/* Length of printable ASCII text (0x20..0x7E) up to NUL. A run shorter than
 * NA_KLASS_MIN_CHARS is not a class name; a byte outside the range makes the
 * whole thing not-a-name, which is what separates a real string pointer from
 * a pointer to a vtable, a counter or another struct. */
static inline NA_U32 na_klass_text_len(const char* text, NA_U32 size)
{
    NA_U32 n = 0;
    if (!text) return 0;
    while (n < size && text[n]) {
        const unsigned char c = (unsigned char)text[n];
        if (c < 0x20 || c > 0x7E) return 0;
        n++;
    }
    return (n >= NA_KLASS_MIN_CHARS) ? n : 0;
}

/* Read printable text from `addr` (inclusive of the terminator check).
 * Returns 1 and NUL-terminates `out` on success. */
static inline int na_klass_read_text(na_klass_read_fn read, void* ctx, NA_U64 addr,
                                     char* out, NA_U32 size)
{
    if (!read || !out || size < NA_KLASS_MIN_CHARS + 1 || size > NA_KLASS_MAX_NAME_BYTES)
        return 0;
    if (!na_user_range(addr, size)) return 0;
    if (!read(ctx, addr, out, size - 1)) return 0;
    out[size - 1] = '\0';
    return na_klass_text_len(out, size - 1) != 0;
}

/* Read the class name: qword at klass+name_off, then the string it points to. */
static inline int na_klass_read_name(na_klass_read_fn read, void* ctx, NA_U64 klass,
                                     NA_U32 name_off, char* out, NA_U32 size)
{
    NA_U64 name_ptr = 0;
    if (!read || !out) return 0;
    if (klass > (NA_U64)-1 - name_off) return 0;
    if (!na_user_range(klass + name_off, 8)) return 0;
    if (!read(ctx, klass + name_off, &name_ptr, 8)) return 0;
    return na_klass_read_text(read, ctx, name_ptr, out, size);
}

/* 1 when the class at `klass` is named `want` (exact, case-sensitive: IL2CPP
 * type names are case-sensitive and Object/object must not be confused). */
static inline int na_klass_is(na_klass_read_fn read, void* ctx, NA_U64 klass,
                              NA_U32 name_off, const char* want)
{
    char text[NA_KLASS_MAX_NAME_BYTES];
    NA_U32 i = 0;
    if (!want) return 0;
    if (!na_klass_read_name(read, ctx, klass, name_off, text, sizeof(text))) return 0;
    while (want[i] && i + 1 < sizeof(text)) {
        if (text[i] != want[i]) return 0;
        i++;
    }
    return want[i] == '\0' && text[i] == '\0';
}

/* Bounded search for a class-pointer slot inside
 * [image_base + rva - window, image_base + rva + window).
 *
 * `page_buf` must hold NA_KLASS_PAGE_BYTES bytes. Each page is fetched once;
 * qwords are filtered locally (a class pointer must at least be a user-range
 * address) before a name read is spent on them, so the cost scales with the
 * number of plausible pointers, not with the window size.
 *
 * Returns 1 and writes the image-relative RVA of the first matching slot into
 * *out_rva. `examined` (optional) receives how many candidates were validated.
 */
static inline int na_klass_sweep(na_klass_read_fn read, void* ctx, NA_U64 image_base,
                                 NA_U64 rva, NA_U32 window, NA_U32 name_off,
                                 const char* want, NA_U32 max_candidates,
                                 NA_U8* page_buf, NA_U32* examined, NA_U64* out_rva)
{
    NA_U64 begin, end, at;
    NA_U32 checked = 0;

    if (examined) *examined = 0;
    if (out_rva) *out_rva = 0;
    if (!read || !want || !page_buf || !out_rva) return 0;
    if (!window) return 0;
    if (rva < window) return 0;                    /* would run below the image */
    if (image_base > (NA_U64)-1 - rva) return 0;
    if (!max_candidates) max_candidates = NA_KLASS_DEFAULT_CANDIDATES;

    /* The claimed slot first. The window loop spends a fixed candidate
     * budget in address order, and the .data of a 148 MB image is dense with
     * user-range pointers: on 2026-11-28 the budget ran out 600 candidates
     * before reaching a slot that was correct all along. */
    {
        NA_U64 direct = 0;
        if (na_user_range(image_base + rva, 8) &&
            read(ctx, image_base + rva, &direct, 8) &&
            na_user_range(direct, 8) && direct <= (NA_U64)-1 - name_off &&
            na_klass_is(read, ctx, direct, name_off, want)) {
            if (examined) *examined = 1;
            *out_rva = rva;
            return 1;
        }
    }

    begin = image_base + rva - window;
    end = image_base + rva + window;
    if (end < begin) return 0;

    for (at = begin; at < end && checked < max_candidates; at += NA_KLASS_PAGE_BYTES) {
        NA_U32 off;
        const NA_U32 chunk =
            (end - at >= NA_KLASS_PAGE_BYTES) ? NA_KLASS_PAGE_BYTES : (NA_U32)(end - at);
        if (!na_user_range(at, chunk)) continue;
        if (!read(ctx, at, page_buf, chunk)) continue;

        for (off = 0; off + 8 <= chunk; off += 8) {
            NA_U64 candidate = na_klass_le_u64(page_buf + off);
            if (!na_user_range(candidate, 8)) continue;      /* local filter, free */
            if (candidate > (NA_U64)-1 - name_off) continue;
            if (!na_user_range(candidate + name_off, 8)) continue;
            checked++;
            if (na_klass_is(read, ctx, candidate, name_off, want)) {
                if (examined) *examined = checked;
                *out_rva = (at + off) - image_base;
                return 1;
            }
            if (checked >= max_candidates) break;
        }
    }
    if (examined) *examined = checked;
    return 0;
}

#endif /* NZL_NA_KLASS_H */
