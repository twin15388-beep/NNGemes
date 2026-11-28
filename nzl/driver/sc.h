#ifndef SC_H
#define SC_H

#include <ntddk.h>

#define NA_CMD_READ   1
#define NA_CMD_WRITE  2
#define NA_CMD_BASE   3
#define NA_CMD_PEB    4
#define NA_CMD_REGION 5
#define NA_CMD_BATCH  6
#define NA_CMD_ALLOC  7
#define NA_CMD_MODULE 8
#define NA_CMD_SCAN   9

#define NA_MAX_XFER  0x10000
#define NA_BATCH_MAX 64

#pragma pack(push, 1)

typedef struct _NA_ITEM {
    ULONGLONG address;
    ULONG     offset;
    ULONG     size;
} NA_ITEM, *PNA_ITEM;

typedef struct _NA_REGION {
    ULONGLONG base;
    ULONGLONG size;
    ULONG     state;
    ULONG     protect;
    ULONG     type;
    ULONG     pad;
} NA_REGION, *PNA_REGION;

typedef struct _NA_SHM {
    volatile LONG seq;
    volatile LONG seq_out;
    ULONG         key;
    ULONG         cmd;
    ULONG         pid;
    ULONG         size;
    ULONGLONG     address;
    ULONG         protect;
    ULONG         count;
    ULONG         total;
    NTSTATUS      status;
    ULONG         pad;
    NA_REGION     region;
    NA_ITEM       items[NA_BATCH_MAX];
    UCHAR         data[NA_MAX_XFER];
} NA_SHM, *PNA_SHM;

#pragma pack(pop)

#endif
