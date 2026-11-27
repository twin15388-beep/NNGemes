#include <ntifs.h>
#include "sc.h"

#define POOL_TAG 'aNcs'

typedef BOOLEAN (NTAPI *FN_RtlAddFunctionTable)(PVOID, ULONG, ULONG64);
#ifndef RUNTIME_FUNCTION
typedef struct _RUNTIME_FUNCTION { ULONG BeginAddress; ULONG EndAddress; ULONG UnwindData; } RUNTIME_FUNCTION, *PRUNTIME_FUNCTION;
#endif

NTSYSAPI PVOID NTAPI PsGetProcessPeb(PEPROCESS);

static ULONG           g_key;
static volatile LONG   g_active;
static SECURITY_DESCRIPTOR g_section_sd;
static UCHAR           g_section_acl[512];
static NTSTATUS        g_dbg_sd = 0xEEEE0001, g_dbg_acl = 0xEEEE0002, g_dbg_ace1 = 0xEEEE0003,
                       g_dbg_ace2 = 0xEEEE0004, g_dbg_ace3 = 0xEEEE0005, g_dbg_dacl = 0xEEEE0006,
                       g_dbg_section = 0xEEEE0007;

// Build a DACL for the shared section: SYSTEM + Administrators full,
// everyone else map read/write. Without this, ZwCreateSection from a
// system thread leaves the section SYSTEM-only and every usermode
// client gets ERROR_ACCESS_DENIED (observed live, win32=5).
static PSECURITY_DESCRIPTOR NaSectionSecurity(void)
{
    PACL acl = (PACL)g_section_acl;
    NTSTATUS st = RtlCreateSecurityDescriptor(&g_section_sd, SECURITY_DESCRIPTOR_REVISION);
    g_dbg_sd = st;
    if (!NT_SUCCESS(st)) return NULL;
    st = RtlCreateAcl(acl, sizeof(g_section_acl), ACL_REVISION);
    g_dbg_acl = st;
    if (!NT_SUCCESS(st)) return NULL;
    g_dbg_ace1 = RtlAddAccessAllowedAce(acl, ACL_REVISION, SECTION_ALL_ACCESS, SeLocalSystemSid);
    g_dbg_ace2 = RtlAddAccessAllowedAce(acl, ACL_REVISION, SECTION_ALL_ACCESS, SeAliasAdminsSid);
    g_dbg_ace3 = RtlAddAccessAllowedAce(acl, ACL_REVISION, SECTION_MAP_READ | SECTION_MAP_WRITE | SECTION_QUERY, SeWorldSid);
    st = RtlSetDaclSecurityDescriptor(&g_section_sd, TRUE, acl, FALSE);
    g_dbg_dacl = st;
    if (!NT_SUCCESS(st)) return NULL;
    return &g_section_sd;
}
static PVOID           g_shm_kernel;
static HANDLE          g_shm_handle;
static HANDLE          g_worker_thread;

static PVOID NaImageBase(void)
{
    PUCHAR p = (PUCHAR)(ULONG_PTR)NaImageBase;
    p = (PUCHAR)((ULONG_PTR)p & ~0xFFFULL);
    for (ULONG i = 0; i < 0x10000; i += 0x1000, p -= 0x1000) {
        if (!MmIsAddressValid(p))
            break;
        if (*(USHORT*)p == 0x5A4D)
            return p;
    }
    return NULL;
}

static VOID NaRegisterExcTable(PVOID base)
{
    if (!base) return;
    PUCHAR nt = (PUCHAR)base + *(ULONG*)((PUCHAR)base + 0x3C);
    ULONG rva = *(ULONG*)(nt + 160);
    ULONG rsz = *(ULONG*)(nt + 164);
    if (!rva || !rsz) return;
    UNICODE_STRING name;
    RtlInitUnicodeString(&name, L"RtlAddFunctionTable");
    FN_RtlAddFunctionTable fn = (FN_RtlAddFunctionTable)MmGetSystemRoutineAddress(&name);
    if (fn) fn((PUCHAR)base + rva, rsz / sizeof(RUNTIME_FUNCTION), (ULONG64)base);
}

static NTSTATUS NaRead(ULONG pid, ULONGLONG addr, ULONG sz, PVOID out)
{
    PEPROCESS proc;
    NTSTATUS st = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)pid, &proc);
    if (!NT_SUCCESS(st)) return st;
    KAPC_STATE apc;
    KeStackAttachProcess(proc, &apc);
    __try {
        ProbeForRead((PVOID)(ULONG_PTR)addr, sz, 1);
        RtlCopyMemory(out, (PVOID)(ULONG_PTR)addr, sz);
        st = STATUS_SUCCESS;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        st = GetExceptionCode();
    }
    KeUnstackDetachProcess(&apc);
    ObDereferenceObject(proc);
    return st;
}

static NTSTATUS NaWrite(ULONG pid, ULONGLONG addr, ULONG sz, PVOID in)
{
    PEPROCESS proc;
    NTSTATUS st = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)pid, &proc);
    if (!NT_SUCCESS(st)) return st;
    KAPC_STATE apc;
    KeStackAttachProcess(proc, &apc);
    __try {
        ProbeForWrite((PVOID)(ULONG_PTR)addr, sz, 1);
        RtlCopyMemory((PVOID)(ULONG_PTR)addr, in, sz);
        st = STATUS_SUCCESS;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        st = GetExceptionCode();
    }
    KeUnstackDetachProcess(&apc);
    ObDereferenceObject(proc);
    return st;
}

static NTSTATUS NaBase(ULONG pid, PULONGLONG out)
{
    PEPROCESS proc;
    NTSTATUS st = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)pid, &proc);
    if (!NT_SUCCESS(st)) return st;
    KAPC_STATE apc;
    KeStackAttachProcess(proc, &apc);
    __try {
        PUCHAR peb = (PUCHAR)PsGetProcessPeb(proc);
        *out = *(ULONGLONG*)(peb + 0x10); /* ImageBaseAddress */
        st = STATUS_SUCCESS;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        st = GetExceptionCode();
    }
    KeUnstackDetachProcess(&apc);
    ObDereferenceObject(proc);
    return st;
}

static NTSTATUS NaPeb(ULONG pid, PULONGLONG out)
{
    PEPROCESS proc;
    NTSTATUS st = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)pid, &proc);
    if (!NT_SUCCESS(st)) return st;
    *out = (ULONGLONG)(ULONG_PTR)PsGetProcessPeb(proc);
    ObDereferenceObject(proc);
    return STATUS_SUCCESS;
}

static NTSTATUS NaRegion(ULONG pid, ULONGLONG addr, PNA_REGION out)
{
    PEPROCESS proc;
    NTSTATUS st = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)pid, &proc);
    if (!NT_SUCCESS(st)) return st;
    KAPC_STATE apc;
    KeStackAttachProcess(proc, &apc);
    MEMORY_BASIC_INFORMATION mbi;
    SIZE_T ret;
    st = ZwQueryVirtualMemory(ZwCurrentProcess(), (PVOID)(ULONG_PTR)addr,
                              MemoryBasicInformation, &mbi, sizeof(mbi), &ret);
    if (NT_SUCCESS(st)) {
        out->base    = (ULONGLONG)(ULONG_PTR)mbi.BaseAddress;
        out->size    = (ULONGLONG)mbi.RegionSize;
        out->state   = mbi.State;
        out->protect = mbi.Protect;
        out->type    = mbi.Type;
        out->pad     = 0;
    }
    KeUnstackDetachProcess(&apc);
    ObDereferenceObject(proc);
    return st;
}

static NTSTATUS NaAlloc(ULONG pid, ULONGLONG sz, ULONG prot, PULONGLONG out)
{
    PEPROCESS proc;
    NTSTATUS st = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)pid, &proc);
    if (!NT_SUCCESS(st)) return st;
    KAPC_STATE apc;
    KeStackAttachProcess(proc, &apc);
    PVOID base = NULL;
    SIZE_T size = (SIZE_T)sz;
    st = ZwAllocateVirtualMemory(ZwCurrentProcess(), &base, 0, &size, MEM_COMMIT | MEM_RESERVE, prot);
    if (NT_SUCCESS(st)) *out = (ULONGLONG)(ULONG_PTR)base;
    KeUnstackDetachProcess(&apc);
    ObDereferenceObject(proc);
    return st;
}

static NTSTATUS NaBatch(PNA_SHM shm)
{
    if (!shm->count || shm->count > NA_BATCH_MAX ||
        !shm->total || shm->total > NA_MAX_XFER)
        return STATUS_INVALID_PARAMETER;

    PVOID tmp = ExAllocatePoolWithTag(NonPagedPool, shm->total, POOL_TAG);
    if (!tmp) return STATUS_INSUFFICIENT_RESOURCES;

    PEPROCESS proc;
    NTSTATUS st = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)shm->pid, &proc);
    if (!NT_SUCCESS(st)) { ExFreePoolWithTag(tmp, POOL_TAG); return st; }

    KAPC_STATE apc;
    KeStackAttachProcess(proc, &apc);
    for (ULONG i = 0; i < shm->count; i++) {
        PNA_ITEM it = &shm->items[i];
        if (!it->size || (ULONGLONG)it->offset + it->size > shm->total) continue;
        __try {
            ProbeForRead((PVOID)(ULONG_PTR)it->address, it->size, 1);
            RtlCopyMemory((PUCHAR)tmp + it->offset, (PVOID)(ULONG_PTR)it->address, it->size);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            RtlZeroMemory((PUCHAR)tmp + it->offset, it->size);
        }
    }
    KeUnstackDetachProcess(&apc);
    ObDereferenceObject(proc);

    RtlCopyMemory(shm->data, tmp, shm->total);
    ExFreePoolWithTag(tmp, POOL_TAG);
    return STATUS_SUCCESS;
}

static VOID NaWorker(PVOID ctx)
{
    UNREFERENCED_PARAMETER(ctx);
    PNA_SHM shm = (PNA_SHM)g_shm_kernel;
    LARGE_INTEGER delay;
    delay.QuadPart = -5000LL;

    while (InterlockedAdd(&g_active, 0)) {
        LONG cur = InterlockedAdd((volatile LONG*)&shm->seq, 0);
        if (cur != InterlockedAdd((volatile LONG*)&shm->seq_out, 0)) {
            if (shm->key != g_key) {
                shm->status = STATUS_ACCESS_DENIED;
                InterlockedExchange((volatile LONG*)&shm->seq_out, cur);
                goto sleep;
            }
            switch (shm->cmd) {
            case NA_CMD_READ:
                if (shm->size && shm->size <= NA_MAX_XFER)
                    shm->status = NaRead(shm->pid, shm->address, shm->size, shm->data);
                else
                    shm->status = STATUS_INVALID_PARAMETER;
                break;
            case NA_CMD_WRITE:
                if (shm->size && shm->size <= NA_MAX_XFER)
                    shm->status = NaWrite(shm->pid, shm->address, shm->size, shm->data);
                else
                    shm->status = STATUS_INVALID_PARAMETER;
                break;
            case NA_CMD_BASE: {
                ULONGLONG val = 0;
                shm->status = NaBase(shm->pid, &val);
                if (NT_SUCCESS(shm->status)) *(ULONGLONG*)shm->data = val;
                break;
            }
            case NA_CMD_PEB: {
                ULONGLONG val = 0;
                shm->status = NaPeb(shm->pid, &val);
                if (NT_SUCCESS(shm->status)) *(ULONGLONG*)shm->data = val;
                break;
            }
            case NA_CMD_REGION:
                shm->status = NaRegion(shm->pid, shm->address, &shm->region);
                break;
            case NA_CMD_BATCH:
                shm->status = NaBatch(shm);
                break;
            case NA_CMD_ALLOC: {
                ULONGLONG val = 0;
                shm->status = NaAlloc(shm->pid, shm->address, shm->protect, &val);
                if (NT_SUCCESS(shm->status)) *(ULONGLONG*)shm->data = val;
                break;
            }
            default:
                shm->status = STATUS_INVALID_DEVICE_REQUEST;
                break;
            }
            InterlockedExchange((volatile LONG*)&shm->seq_out, cur);
        }
sleep:
        KeDelayExecutionThread(KernelMode, FALSE, &delay);
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static VOID NaToHex8(ULONG v, WCHAR* out)
{
    static const WCHAR h[] = L"0123456789ABCDEF";
    for (int i = 7; i >= 0; i--, v >>= 4)
        out[i] = h[v & 0xF];
}

static VOID NaUnload(void)
{
    InterlockedExchange(&g_active, 0);
    if (g_worker_thread) {
        LARGE_INTEGER to;
        to.QuadPart = -10000LL * 2000;
        ZwWaitForSingleObject(g_worker_thread, FALSE, &to);
        ZwClose(g_worker_thread);
        g_worker_thread = NULL;
    }
    if (g_shm_kernel) {
        ZwUnmapViewOfSection(ZwCurrentProcess(), g_shm_kernel);
        g_shm_kernel = NULL;
    }
    if (g_shm_handle) {
        ZwClose(g_shm_handle);
        g_shm_handle = NULL;
    }
    UNICODE_STRING regPath;
    RtlInitUnicodeString(&regPath, L"\\Registry\\Machine\\SYSTEM\\NaCoreFilter");
    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, &regPath, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    HANDLE hKey;
    if (NT_SUCCESS(ZwOpenKey(&hKey, KEY_ALL_ACCESS, &oa))) {
        ZwDeleteKey(hKey);
        ZwClose(hKey);
    }
}

static VOID NaStartup(PVOID ctx)
{
    UNREFERENCED_PARAMETER(ctx);

    LARGE_INTEGER tick;
    KeQueryTickCount(&tick);
    g_key = (ULONG)(tick.LowPart ^ 0xC3A7F519UL ^ (ULONG)(ULONG_PTR)NaStartup);
    if (!g_key) g_key = 0xDEADBEEF;

    WCHAR shmBuf[28];
    RtlCopyMemory(shmBuf, L"\\BaseNamedObjects\\", 18 * sizeof(WCHAR));
    NaToHex8(g_key ^ 0x91B4F3E7UL, shmBuf + 18);
    shmBuf[26] = 0;

    UNICODE_STRING shmName;
    RtlInitUnicodeString(&shmName, shmBuf);
    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, &shmName, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    oa.SecurityDescriptor = NaSectionSecurity();
    LARGE_INTEGER secSize;
    secSize.QuadPart = sizeof(NA_SHM);
    NTSTATUS st = ZwCreateSection(&g_shm_handle, SECTION_ALL_ACCESS, &oa,
                                  &secSize, PAGE_READWRITE, SEC_COMMIT, NULL);
    g_dbg_section = st;
    if (!NT_SUCCESS(st)) goto done;

    SIZE_T viewSize = sizeof(NA_SHM);
    st = ZwMapViewOfSection(g_shm_handle, ZwCurrentProcess(), &g_shm_kernel,
                            0, 0, NULL, &viewSize, 2, 0, PAGE_READWRITE);
    if (!NT_SUCCESS(st)) goto done;

    RtlZeroMemory(g_shm_kernel, sizeof(NA_SHM));

    UNICODE_STRING regPath;
    RtlInitUnicodeString(&regPath, L"\\Registry\\Machine\\SYSTEM\\NaCoreFilter");
    OBJECT_ATTRIBUTES regOa;
    InitializeObjectAttributes(&regOa, &regPath, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    HANDLE hKey;
    ULONG disp;
    st = ZwCreateKey(&hKey, KEY_ALL_ACCESS, &regOa, 0, NULL, REG_OPTION_VOLATILE, &disp);
    if (NT_SUCCESS(st)) {
        UNICODE_STRING valName;
        RtlInitUnicodeString(&valName, L"Key");
        ZwSetValueKey(hKey, &valName, 0, REG_DWORD, &g_key, sizeof(g_key));
        RtlInitUnicodeString(&valName, L"Shm");
        ZwSetValueKey(hKey, &valName, 0, REG_SZ, shmBuf + 18, 9 * sizeof(WCHAR));
        struct { const WCHAR* n; NTSTATUS v; } dbg[] = {
            { L"DbgSd", g_dbg_sd }, { L"DbgAcl", g_dbg_acl }, { L"DbgAce1", g_dbg_ace1 },
            { L"DbgAce2", g_dbg_ace2 }, { L"DbgAce3", g_dbg_ace3 }, { L"DbgDacl", g_dbg_dacl },
            { L"DbgSection", g_dbg_section } };
        for (ULONG i = 0; i < sizeof(dbg) / sizeof(dbg[0]); i++) {
            RtlInitUnicodeString(&valName, dbg[i].n);
            ZwSetValueKey(hKey, &valName, 0, REG_DWORD, &dbg[i].v, sizeof(dbg[i].v));
        }
        ZwClose(hKey);
    }

    InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    st = PsCreateSystemThread(&g_worker_thread, THREAD_ALL_ACCESS, &oa,
                              NULL, NULL, NaWorker, NULL);
    if (!NT_SUCCESS(st)) goto done;

    InterlockedExchange(&g_active, 1);

done:
    PsTerminateSystemThread(STATUS_SUCCESS);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING reg)
{
    UNREFERENCED_PARAMETER(drv);
    UNREFERENCED_PARAMETER(reg);
    PVOID base = NaImageBase();
    NaRegisterExcTable(base);
    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    HANDLE hThread;
    NTSTATUS st = PsCreateSystemThread(&hThread, THREAD_ALL_ACCESS, &oa,
                                       NULL, NULL, NaStartup, NULL);
    if (NT_SUCCESS(st)) ZwClose(hThread);
    return STATUS_SUCCESS;
}
