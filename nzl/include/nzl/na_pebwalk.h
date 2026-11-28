/* NZL Studio — target PEB module lookup, portable walk.
 *
 * NA_CMD_MODULE was declared in both headers and implemented in neither:
 * the client had NaGetModule() and the driver switch had no case 8, so
 * every call silently returned STATUS_INVALID_DEVICE_REQUEST and the
 * caller got 0. That is worse than missing — it looks like a working API.
 * This is the implementation, written as a pure walk over a read callback
 * so it can be tested against a synthetic PEB on the host.
 *
 * Layout used (x64 only, stable from Win7 to Win11):
 *   PEB                     +0x18  Ldr (PEB_LDR_DATA*)
 *   PEB_LDR_DATA            +0x10  InLoadOrderModuleList (LIST_ENTRY)
 *   LDR_DATA_TABLE_ENTRY    +0x00  InLoadOrderLinks.Flink
 *                           +0x30  DllBase
 *                           +0x40  SizeOfImage
 *                           +0x58  BaseDllName (UNICODE_STRING, 16 bytes)
 *   UNICODE_STRING          +0x00  Length (USHORT)
 *                           +0x08  Buffer (PWSTR)
 *
 * These are the documented x64 PEB/LDR field offsets, not build-specific
 * game offsets: they do not depend on the game or the Unity version. The
 * host test only proves the walk logic against this declared layout, not
 * that the live PEB matches — the driver's PEB probe (nzl_probe.exe,
 * PE offset + MZ signature check) is the live confirmation.
 */
#ifndef NZL_NA_PEBWALK_H
#define NZL_NA_PEBWALK_H

#include "na_validate.h"

#define NA_PEB_LDR          0x18u
#define NA_LDR_INLOADORDER  0x10u
#define NA_LDR_ENTRY_DLLBASE 0x30u
#define NA_LDR_ENTRY_SIZE    0x40u
#define NA_LDR_ENTRY_NAMEDLL 0x58u
#define NA_UNICODE_STRING_LEN 0x00u
#define NA_UNICODE_STRING_BUF 0x08u
#define NA_PEBWALK_MAX_ENTRIES 1024u
#define NA_PEBWALK_MAX_NAME_BYTES 512u

typedef struct _NA_MODULE_INFO {
    NA_U64 base;
    NA_U32 size;         /* SizeOfImage */
    NA_U32 name_chars;   /* BaseDllName length in WCHARs */
} NA_MODULE_INFO;

/* read(): full-or-fail copy from the target process. Returns 1/0. */
typedef int (*na_peb_read_fn)(void* ctx, NA_U64 va, void* out, NA_U32 size);

/* Case-insensitive (ASCII folded) UTF-16 comparison of `chars` code units. */
static inline int na_name_equal(const unsigned short* a, NA_U32 chars,
                                const unsigned short* b, NA_U32 bchars)
{
    NA_U32 i;
    if (!a || !b) return 0;
    if (chars != bchars) return 0;
    for (i = 0; i < chars; i++) {
        unsigned short x = a[i];
        unsigned short y = b[i];
        if (x >= 'A' && x <= 'Z') x = (unsigned short)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (unsigned short)(y - 'A' + 'a');
        if (x != y) return 0;
    }
    return 1;
}

/* Find a loaded module by BaseDllName in the target PEB.
 * Returns 1 and fills `out` on success, 0 when not found or on any read
 * failure. `peb` is the target PEB VA (NA_CMD_PEB). */
static inline int na_peb_find_module(na_peb_read_fn read, void* ctx, NA_U64 peb,
                                     const unsigned short* name, NA_U32 name_chars,
                                     NA_MODULE_INFO* out)
{
    NA_U64 ldr = 0, head = 0, entry = 0;
    NA_U32 guard = 0;

    if (!read || !name || !name_chars || !out) return 0;
    if (name_chars * 2u > NA_PEBWALK_MAX_NAME_BYTES) return 0;
    if (!na_user_range(peb, NA_PEB_LDR + 8)) return 0;
    if (!read(ctx, peb + NA_PEB_LDR, &ldr, 8)) return 0;
    if (!na_user_range(ldr, NA_LDR_INLOADORDER + 8)) return 0;

    head = ldr + NA_LDR_INLOADORDER;
    if (!read(ctx, head, &entry, 8)) return 0;

    while (entry && entry != head && ++guard <= NA_PEBWALK_MAX_ENTRIES) {
        NA_U64 next = 0, name_buffer = 0, base = 0;
        NA_U32 image_size = 0;
        unsigned short len = 0;
        unsigned short local[NA_PEBWALK_MAX_NAME_BYTES / 2];

        if (!na_user_range(entry, NA_LDR_ENTRY_NAMEDLL + 16)) break;
        if (!read(ctx, entry, &next, 8)) break;
        if (!read(ctx, entry + NA_LDR_ENTRY_DLLBASE, &base, 8)) break;
        if (!read(ctx, entry + NA_LDR_ENTRY_SIZE, &image_size, 4)) break;
        if (!read(ctx, entry + NA_LDR_ENTRY_NAMEDLL + NA_UNICODE_STRING_LEN, &len, 2))
            break;
        if (!read(ctx, entry + NA_LDR_ENTRY_NAMEDLL + NA_UNICODE_STRING_BUF,
                  &name_buffer, 8))
            break;

        if (len && (len & 1u) == 0 && len <= (unsigned short)NA_PEBWALK_MAX_NAME_BYTES) {
            NA_U32 chars = (NA_U32)len / 2u;
            if (na_user_range(name_buffer, (NA_U64)len) &&
                read(ctx, name_buffer, local, len) &&
                na_name_equal(local, chars, name, name_chars)) {
                out->base = base;
                out->size = image_size;
                out->name_chars = chars;
                return 1;
            }
        }
        entry = next;
    }
    return 0;
}

#endif /* NZL_NA_PEBWALK_H */
