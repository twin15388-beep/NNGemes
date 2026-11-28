/* NZL Studio — Sharedmem Driver wire protocol, single source of truth.
 *
 * Included by BOTH sides of the protocol:
 *   - nzl/driver/sc.h        (kernel, C, WDK/MSVC)
 *   - nzl/client/sc_client.h (user, C++17, Windows SDK)
 *
 * Why this file exists: the struct was previously duplicated by hand in
 * sc.h and sc_client.h. Any divergence (one field added on one side, a
 * different pack setting, a different Ntstatus width) shifts every later
 * offset and the driver starts reading client VAs out of the wrong slot —
 * that is a wild-address MmCopyVirtualMemory, i.e. the BSOD class from
 * builds #1-#4. Layout is now asserted at compile time on both sides.
 *
 * Rules:
 *   - no platform headers except <stdint.h>;
 *   - fixed widths only (NA_I32/NA_U32 are 32-bit on LP64 too);
 *   - pack(1) is mandatory and asserted;
 *   - field offsets are exposed as NA_SHM_OFF_* for tests and diagnostics.
 */
#ifndef NZL_NA_WIRE_H
#define NZL_NA_WIRE_H

/* ---- widths ------------------------------------------------------------
 * On Windows (LLP64) long/unsigned long are 32-bit, so the wire types are
 * defined as long there to stay assignment-compatible with LONG/NTSTATUS/
 * ULONG in the driver (InterlockedIncrement, NT_SUCCESS). On any LP64 host
 * (Linux build of the tests) long is 64-bit, so int/unsigned int are used
 * instead. Both are exactly 32 bit, so the packed layout is identical.
 */
#if defined(_WIN32)
typedef long           NA_I32;
typedef unsigned long  NA_U32;
#else
typedef int            NA_I32;
typedef unsigned int   NA_U32;
#endif
typedef unsigned long long NA_U64;
typedef unsigned char      NA_U8;

#define NA_WIRE_VERSION 2

/* ---- commands ---------------------------------------------------------- */
#define NA_CMD_READ   1
#define NA_CMD_WRITE  2
#define NA_CMD_BASE   3
#define NA_CMD_PEB    4
#define NA_CMD_REGION 5
#define NA_CMD_BATCH  6
#define NA_CMD_ALLOC  7
#define NA_CMD_MODULE 8
#define NA_CMD_SCAN   9

/* ---- limits ------------------------------------------------------------ */
#define NA_MAX_XFER     0x10000u   /* max bytes per single read/write/batch */
#define NA_BATCH_MAX    64u        /* NA_ITEM slots in one BATCH command    */
#define NA_SCAN_MAX_CAND 256u      /* most candidates a SCAN call may return */

/* ---- payload ----------------------------------------------------------- */
#pragma pack(push, 1)

typedef struct _NA_ITEM {
    NA_U64 address;   /* source VA in the target process */
    NA_U32 offset;    /* destination offset inside NA_SHM.data */
    NA_U32 size;      /* byte count */
} NA_ITEM, *PNA_ITEM;

typedef struct _NA_REGION {
    NA_U64 base;
    NA_U64 size;
    NA_U32 state;
    NA_U32 protect;
    NA_U32 type;
    NA_U32 pad;
} NA_REGION, *PNA_REGION;

typedef struct _NA_SHM {
    volatile NA_I32 seq;      /* +0x00 client increments to post a command */
    volatile NA_I32 seq_out;  /* +0x04 worker echoes seq after completion   */
    NA_U32 key;               /* +0x08 session key                          */
    NA_U32 cmd;               /* +0x0C NA_CMD_*                             */
    NA_U32 pid;               /* +0x10 target process id                    */
    NA_U32 size;              /* +0x14 byte count (READ/WRITE)              */
    NA_U64 address;           /* +0x18 target VA / scan start / alloc size  */
    NA_U32 protect;           /* +0x20 ALLOC protection                     */
    NA_U32 count;             /* +0x24 BATCH item count / SCAN maxcand      */
    NA_U32 total;             /* +0x28 BATCH output byte count              */
    NA_I32 status;            /* +0x2C NTSTATUS of the last command         */
    NA_U32 pad;               /* +0x30 reserved, must stay 0                */
    NA_REGION region;         /* +0x34 REGION output                        */
    NA_ITEM items[NA_BATCH_MAX]; /* +0x54 BATCH input                       */
    NA_U8   data[NA_MAX_XFER];   /* +0x454 payload / blackboard             */
} NA_SHM, *PNA_SHM;

#pragma pack(pop)

/* ---- fixed ABI surface ------------------------------------------------- */
#define NA_SHM_OFF_SEQ     0x00
#define NA_SHM_OFF_SEQ_OUT 0x04
#define NA_SHM_OFF_KEY     0x08
#define NA_SHM_OFF_CMD     0x0C
#define NA_SHM_OFF_PID     0x10
#define NA_SHM_OFF_SIZE    0x14
#define NA_SHM_OFF_ADDRESS 0x18
#define NA_SHM_OFF_PROTECT 0x20
#define NA_SHM_OFF_COUNT   0x24
#define NA_SHM_OFF_TOTAL   0x28
#define NA_SHM_OFF_STATUS  0x2C
#define NA_SHM_OFF_PAD     0x30
#define NA_SHM_OFF_REGION  0x34
#define NA_SHM_OFF_ITEMS   0x54
#define NA_SHM_OFF_DATA    0x454
#define NA_SHM_SIZE        0x10454

/* Compile-time layout asserts. C89/C99/MSVC-C and C++ all accept the
 * negative-array-size trick, so this works in the WDK build too. */
#define NA_JOIN2(a, b) a##b
#define NA_JOIN(a, b) NA_JOIN2(a, b)
#define NA_STATIC_ASSERT(cond, tag) \
    typedef char NA_JOIN(na_static_assert_, tag)[(cond) ? 1 : -1]

NA_STATIC_ASSERT(sizeof(NA_ITEM) == 16, item_size);
NA_STATIC_ASSERT(sizeof(NA_REGION) == 32, region_size);
NA_STATIC_ASSERT(sizeof(NA_SHM) == NA_SHM_SIZE, shm_size);
NA_STATIC_ASSERT(NA_SHM_OFF_REGION == 0x34, region_off);
NA_STATIC_ASSERT(NA_SHM_OFF_ITEMS == 0x54, items_off);
NA_STATIC_ASSERT(NA_SHM_OFF_DATA == 0x454, data_off);
NA_STATIC_ASSERT(NA_MAX_XFER >= NA_SCAN_MAX_CAND * 8u, scan_fits);

#endif /* NZL_NA_WIRE_H */
