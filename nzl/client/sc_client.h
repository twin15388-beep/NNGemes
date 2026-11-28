#pragma once
#include <windows.h>

/* Wire protocol is defined once, in nzl/include/nzl/na_wire.h, and shared
 * with the kernel driver (nzl/driver/sc.h). The layout asserts inside that
 * header are compiled by BOTH sides, so a client/driver drift cannot ship
 * silently: it is a build error instead of a wild VA in MmCopyVirtualMemory. */
#include "nzl/na_wire.h"

/* C++ aliases so call sites keep the historical names. NA_SHM is packed to
 * 1 on both sides, and NA_I32 is `long` on Windows, so `volatile LONG*`
 * (InterlockedIncrement) type-checks against volatile NA_I32 members. */
typedef NA_SHM    NaShm;
typedef NA_ITEM   NaItem;
typedef NA_REGION NaRegion;

// init / shutdown
bool    NaInit();
void    NaClose();
NaShm*  NaGetShm();
UINT32  NaGetKey();

// core operations
bool   NaRead(UINT32 pid, UINT64 addr, UINT32 size, void* out);
bool   NaWrite(UINT32 pid, UINT64 addr, UINT32 size, void* in);
UINT64 NaGetBase(UINT32 pid);
UINT64 NaGetPeb(UINT32 pid);
bool   NaQueryRegion(UINT32 pid, UINT64 addr, NaRegion* out);
// Batch: up to NA_BATCH_MAX items read by the driver in ONE command
// (one worker wake instead of one per read - the per-command poll tick
// dominates small reads). Items carry (address, offset-in-out, size);
// invalid item bounds/source addresses reject the whole request, while a
// valid but unreadable source remains zeroed. Total bytes <= NA_MAX_XFER.
bool   NaBatchRead(UINT32 pid, NaItem* items, UINT32 count, void* out, UINT32 total);
// Kernel-side qword scan (NA_CMD_SCAN): finds up to maxcand candidate
// addresses holding `pattern` in committed MEM_PRIVATE readable regions
// starting at `start`. Returns candidate count; *next = resume VA
// (0 = exhausted). Client validates candidates (klass chain) and
// resumes until the object is found.
UINT32 NaScan(UINT32 pid, UINT64 pattern, UINT64 start, UINT64* candidates, UINT32 maxcand, UINT64* next);
bool   NaAlloc(UINT32 pid, UINT64 size, UINT32 protect, UINT64* out);

// typed read helpers
template<typename T>
bool NaRead(UINT32 pid, UINT64 addr, T* out) {
    return NaRead(pid, addr, sizeof(T), out);
}

template<typename T>
T NaRead(UINT32 pid, UINT64 addr) {
    T val{};
    NaRead(pid, addr, sizeof(T), &val);
    return val;
}

template<typename T>
bool NaWrite(UINT32 pid, UINT64 addr, T val) {
    return NaWrite(pid, addr, sizeof(T), &val);
}
// Module base by name via driver PEB walk (NA_CMD_MODULE).
// Implemented in NaModuleByPid() in nzl/driver/sc_drv.c over
// nzl/include/nzl/na_pebwalk.h; returns 0 when the module is not loaded.
UINT64 NaGetModule(UINT32 pid, const wchar_t* name);
