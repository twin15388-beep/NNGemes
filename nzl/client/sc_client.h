#pragma once
#include <Windows.h>

#define NA_CMD_READ   1
#define NA_CMD_WRITE  2
#define NA_CMD_BASE   3
#define NA_CMD_PEB    4
#define NA_CMD_REGION 5
#define NA_CMD_BATCH  6
#define NA_CMD_ALLOC  7

#define NA_MAX_XFER  0x10000
#define NA_BATCH_MAX 64

#pragma pack(push, 1)

struct NaItem {
    UINT64 address;
    UINT32 offset;
    UINT32 size;
};

struct NaRegion {
    UINT64 base;
    UINT64 size;
    UINT32 state;
    UINT32 protect;
    UINT32 type;
    UINT32 pad;
};

struct NaShm {
    volatile LONG seq;
    volatile LONG seq_out;
    UINT32        key;
    UINT32        cmd;
    UINT32        pid;
    UINT32        size;
    UINT64        address;
    UINT32        protect;
    UINT32        count;
    UINT32        total;
    LONG          status;
    UINT32        pad;
    NaRegion      region;
    NaItem        items[NA_BATCH_MAX];
    UINT8         data[NA_MAX_XFER];
};

#pragma pack(pop)

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
