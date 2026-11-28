#include <ntifs.h>
#include "sc.h"
/* Portable, host-tested pieces: address validation, batch checks, the scan
 * state machine and the PEB module walk. Same code runs in
 * nzl/tests/test_protocol.cpp against synthetic memory. */
#include "nzl/na_validate.h"
#include "nzl/na_scan.h"
#include "nzl/na_pebwalk.h"

#define POOL_TAG 'aNcs'

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
    // SIDs built manually: SeLocalSystemSid/SeAliasAdminsSid/SeWorldSid are
    // not declared in this WDK (C2065 observed) - do not rely on them.
    static UCHAR sidSys[32], sidAdm[32], sidWorld[16];
    SID_IDENTIFIER_AUTHORITY authNt = SECURITY_NT_AUTHORITY;
    SID_IDENTIFIER_AUTHORITY authWorld = SECURITY_WORLD_SID_AUTHORITY;
    RtlInitializeSid((PSID)sidSys, &authNt, 1);
    *RtlSubAuthoritySid((PSID)sidSys, 0) = SECURITY_LOCAL_SYSTEM_RID;
    RtlInitializeSid((PSID)sidAdm, &authNt, 2);
    *RtlSubAuthoritySid((PSID)sidAdm, 0) = SECURITY_BUILTIN_DOMAIN_RID;
    *RtlSubAuthoritySid((PSID)sidAdm, 1) = DOMAIN_ALIAS_RID_ADMINS;
    RtlInitializeSid((PSID)sidWorld, &authWorld, 1);
    *RtlSubAuthoritySid((PSID)sidWorld, 0) = SECURITY_WORLD_RID;
    g_dbg_ace1 = RtlAddAccessAllowedAce(acl, ACL_REVISION, SECTION_ALL_ACCESS, (PSID)sidSys);
    g_dbg_ace2 = RtlAddAccessAllowedAce(acl, ACL_REVISION, SECTION_ALL_ACCESS, (PSID)sidAdm);
    g_dbg_ace3 = RtlAddAccessAllowedAce(acl, ACL_REVISION, SECTION_MAP_READ | SECTION_MAP_WRITE | SECTION_QUERY, (PSID)sidWorld);
    st = RtlSetDaclSecurityDescriptor(&g_section_sd, TRUE, acl, FALSE);
    g_dbg_dacl = st;
    if (!NT_SUCCESS(st)) return NULL;
    // Object manager wants a self-relative SD.
    static UCHAR selfRel[1024];
    ULONG srLen = sizeof(selfRel);
    st = RtlAbsoluteToSelfRelativeSD(&g_section_sd, (PSECURITY_DESCRIPTOR)selfRel, &srLen);
    g_dbg_dacl = st;
    if (!NT_SUCCESS(st)) return NULL;
    return (PSECURITY_DESCRIPTOR)selfRel;
}
static PVOID           g_shm_kernel;
static HANDLE          g_shm_handle;
static HANDLE          g_worker_thread;

// MmCopyVirtualMemory handles bad user addresses internally and returns
// NTSTATUS - no KeStackAttachProcess, no ProbeFor*, no __try/__except and
// therefore no reliance on manually registered unwind tables (kdmapper).
NTSYSAPI NTSTATUS NTAPI MmCopyVirtualMemory(PEPROCESS SourceProcess, PVOID SourceAddress,
    PEPROCESS TargetProcess, PVOID TargetAddress, SIZE_T BufferSize,
    KPROCESSOR_MODE PreviousMode, PSIZE_T ReturnSize);

// BSOD #4 (0x50, nt!memcpy <- MiCopyVirtualMemory <- driver): a copy was
// requested with a kernel-range source VA (0xfffffede9deede7f).
// MmCopyVirtualMemory with PreviousMode=KernelMode does NOT probe the
// addresses - it raw-memcpys and bugchecks. Every address that arrives
// via the shared section is untrusted: reject anything outside the
// user range (and any end-overflow) BEFORE MmCopy ever sees it.
// The predicate itself now lives in nzl/include/nzl/na_validate.h so the
// host test suite fuzzes the exact code the kernel runs.

static NTSTATUS NaRead(ULONG pid, ULONGLONG addr, ULONG sz, PVOID out)
{
    if (!na_user_range(addr, (NA_U64)sz)) return STATUS_INVALID_PARAMETER;
    PEPROCESS proc;
    NTSTATUS st = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)pid, &proc);
    if (!NT_SUCCESS(st)) return st;
    SIZE_T copied = 0;
    st = MmCopyVirtualMemory(proc, (PVOID)(ULONG_PTR)addr, PsGetCurrentProcess(),
                             out, sz, KernelMode, &copied);
    ObDereferenceObject(proc);
    return (NT_SUCCESS(st) && copied == sz) ? STATUS_SUCCESS : (NT_SUCCESS(st) ? STATUS_PARTIAL_COPY : st);
}

static NTSTATUS NaWrite(ULONG pid, ULONGLONG addr, ULONG sz, PVOID in)
{
    if (!na_user_range(addr, (NA_U64)sz)) return STATUS_INVALID_PARAMETER;
    PEPROCESS proc;
    NTSTATUS st = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)pid, &proc);
    if (!NT_SUCCESS(st)) return st;
    SIZE_T copied = 0;
    st = MmCopyVirtualMemory(PsGetCurrentProcess(), in, proc,
                             (PVOID)(ULONG_PTR)addr, sz, KernelMode, &copied);
    ObDereferenceObject(proc);
    return (NT_SUCCESS(st) && copied == sz) ? STATUS_SUCCESS : (NT_SUCCESS(st) ? STATUS_PARTIAL_COPY : st);
}

static NTSTATUS NaBase(ULONG pid, PULONGLONG out)
{
    PEPROCESS proc;
    NTSTATUS st = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)pid, &proc);
    if (!NT_SUCCESS(st)) return st;
    PUCHAR peb = (PUCHAR)PsGetProcessPeb(proc);
    ULONGLONG val = 0;
    if (peb && na_user_range((NA_U64)(ULONG_PTR)peb,
                             0x10 + sizeof(val))) {
        SIZE_T copied = 0;
        st = MmCopyVirtualMemory(proc, peb + 0x10, PsGetCurrentProcess(),
                                 &val, sizeof(val), KernelMode, &copied);
        if (NT_SUCCESS(st) && copied == sizeof(val)) *out = val;
        else if (NT_SUCCESS(st)) st = STATUS_PARTIAL_COPY;
    } else st = STATUS_INVALID_PARAMETER;
    ObDereferenceObject(proc);
    return st;
}

static NTSTATUS NaPeb(ULONG pid, PULONGLONG out)
{
    PEPROCESS proc;
    NTSTATUS st = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)pid, &proc);
    if (!NT_SUCCESS(st)) return st;
    {
        PUCHAR peb = (PUCHAR)PsGetProcessPeb(proc);
        if (!peb || !na_user_range((NA_U64)(ULONG_PTR)peb, 0x1000)) {
            ObDereferenceObject(proc);
            return STATUS_INVALID_PARAMETER;
        }
        *out = (ULONGLONG)(ULONG_PTR)peb;
    }
    ObDereferenceObject(proc);
    return STATUS_SUCCESS;
}

static NTSTATUS NaRegion(ULONG pid, ULONGLONG addr, PNA_REGION out)
{
    if (!na_user_range(addr, 1)) return STATUS_INVALID_PARAMETER;
    PEPROCESS proc;
    NTSTATUS st = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)pid, &proc);
    if (!NT_SUCCESS(st)) return st;
    KAPC_STATE apc;
    MEMORY_BASIC_INFORMATION mbi;
    SIZE_T ret;
    RtlZeroMemory(&mbi, sizeof(mbi));
    KeStackAttachProcess(proc, &apc);
    st = ZwQueryVirtualMemory(ZwCurrentProcess(), (PVOID)(ULONG_PTR)addr,
                              MemoryBasicInformation, &mbi, sizeof(mbi), &ret);
    KeUnstackDetachProcess(&apc);
    // CRITICAL (BSOD #1-#3 root cause, RVA 0x1998): `out` points into the
    // shared section view (g_shm_kernel), which is mapped only into the
    // System process address space. Writing *out WHILE attached to the
    // target process dereferences that VA in the TARGET address space -
    // a wild user-mode pointer (observed: write to 0x...0034, BugCheck
    // 0x1E). Fill the local `mbi` while attached, copy to `out` only
    // after detach, when the System address space is current again.
    if (NT_SUCCESS(st)) {
        out->base    = (ULONGLONG)(ULONG_PTR)mbi.BaseAddress;
        out->size    = (ULONGLONG)mbi.RegionSize;
        out->state   = mbi.State;
        out->protect = mbi.Protect;
        out->type    = mbi.Type;
        out->pad     = 0;
    }
    ObDereferenceObject(proc);
    return st;
}

static NTSTATUS NaAlloc(ULONG pid, ULONGLONG sz, ULONG prot, PULONGLONG out)
{
    PEPROCESS proc;
    // Bounded: the size arrives from the shared section, and an unbounded
    // ZwAllocateVirtualMemory in someone else's address space is a resource
    // exhaustion primitive at best and a typo'd address at worst.
    if (!sz || sz > 0x40000000ULL) return STATUS_INVALID_PARAMETER;
    NTSTATUS st = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)pid, &proc);
    if (!NT_SUCCESS(st)) return st;
    KAPC_STATE apc;
    KeStackAttachProcess(proc, &apc);
    PVOID base = NULL;
    SIZE_T size = (SIZE_T)sz;
    st = ZwAllocateVirtualMemory(ZwCurrentProcess(), &base, 0, &size, MEM_COMMIT | MEM_RESERVE, prot);
    KeUnstackDetachProcess(&apc);
    // Same rule as NaRegion: never dereference System-process pointers
    // (here `out` happens to be a kernel-stack local, but keep the
    // ordering so no future caller passes shared-section memory).
    if (NT_SUCCESS(st)) *out = (ULONGLONG)(ULONG_PTR)base;
    ObDereferenceObject(proc);
    return st;
}

// NA_CMD_SCAN: kernel-side qword pattern scan over the target's committed
// MEM_PRIVATE readable regions. Replaces ~22k client round trips (64KB each,
// ~16ms poll tick each = 6 minutes for 1.4GB) with a handful of commands.
// The walk itself is the portable na_scan_run() state machine
// (nzl/include/nzl/na_scan.h) so the driver and the host test run identical
// logic; this file only supplies the two adapters.
//   region: KeStackAttachProcess + ZwQueryVirtualMemory, result written to a
//           kernel-stack local, detached BEFORE anything else touches it
//           (BSOD #1-#3 rule: never dereference a System-only VA while the
//           target address space is current).
//   read:   MmCopyVirtualMemory into the pool scratch buffer, never a raw
//           deref, always behind na_user_range().
// Output: shm->address = resume VA (0 = exhausted), shm->count = candidates,
// candidates are ULONGLONGs in shm->data. Input: shm->address = start,
// shm->count = max candidates, pattern qword in shm->data[0..8).
// Per-call copy budget keeps one command well under the client's 2000 ms
// Send() timeout; the resume VA carries the rest across calls.
#define NA_SCAN_MAX_BYTES NA_SCAN_MAX_BYTES_DEFAULT
#define NA_SCAN_CHUNK NA_SCAN_CHUNK_DEFAULT

typedef struct _NA_SCAN_CTX {
    PEPROCESS proc;
} NA_SCAN_CTX, *PNA_SCAN_CTX;

static int NaScanRegionCB(void* ctx, NA_U64 va, NA_REGION* out)
{
    PNA_SCAN_CTX c = (PNA_SCAN_CTX)ctx;
    KAPC_STATE apc;
    MEMORY_BASIC_INFORMATION mbi;
    SIZE_T ret = 0;
    NTSTATUS st;

    RtlZeroMemory(&mbi, sizeof(mbi));
    KeStackAttachProcess(c->proc, &apc);
    st = ZwQueryVirtualMemory(ZwCurrentProcess(), (PVOID)(ULONG_PTR)va,
                              MemoryBasicInformation, &mbi, sizeof(mbi), &ret);
    KeUnstackDetachProcess(&apc);
    if (!NT_SUCCESS(st)) return 0;

    out->base    = (NA_U64)(ULONG_PTR)mbi.BaseAddress;
    out->size    = (NA_U64)mbi.RegionSize;
    out->state   = (NA_U32)mbi.State;
    out->protect = (NA_U32)mbi.Protect;
    out->type    = (NA_U32)mbi.Type;
    out->pad     = 0;
    return 1;
}

static int NaScanReadCB(void* ctx, NA_U64 va, void* out, NA_U32 size, NA_U32* copied)
{
    PNA_SCAN_CTX c = (PNA_SCAN_CTX)ctx;
    SIZE_T got = 0;
    NTSTATUS st;
    *copied = 0;
    if (!na_user_range(va, (NA_U64)size)) return 0;
    st = MmCopyVirtualMemory(c->proc, (PVOID)(ULONG_PTR)va, PsGetCurrentProcess(),
                             out, (SIZE_T)size, KernelMode, &got);
    if (!NT_SUCCESS(st) || !got) return 0;
    *copied = (NA_U32)got;
    return 1;
}

static NTSTATUS NaScan(PNA_SHM shm, PULONGLONG next_out, PULONG found_out)
{
    NA_SCAN_CTX ctx;
    NA_SCAN_IO io;
    PVOID buf;
    NA_U32 found = 0;
    NA_U64 nxt = 0;
    NA_U64 pattern = 0;
    NA_U32 maxcand;
    NTSTATUS st;
    int ok;

    RtlCopyMemory(&pattern, shm->data, sizeof(pattern));
    maxcand = na_scan_maxcand(shm->count);
    if (!na_user_range(shm->address, 8)) return STATUS_INVALID_PARAMETER;

    st = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)shm->pid, &ctx.proc);
    if (!NT_SUCCESS(st)) return st;

    buf = ExAllocatePoolWithTag(NonPagedPool, NA_SCAN_CHUNK, POOL_TAG);
    if (!buf) { ObDereferenceObject(ctx.proc); return STATUS_INSUFFICIENT_RESOURCES; }

    io.region = NaScanRegionCB;
    io.read = NaScanReadCB;
    io.ctx = &ctx;
    io.buf = buf;
    io.chunk = NA_SCAN_CHUNK;
    io.max_bytes = NA_SCAN_MAX_BYTES;

    ok = na_scan_run(&io, shm->address, pattern, maxcand,
                     (NA_U64*)shm->data, &found, &nxt);

    ExFreePoolWithTag(buf, POOL_TAG);
    ObDereferenceObject(ctx.proc);

    if (!ok) return STATUS_INVALID_PARAMETER;
    *next_out = (ULONGLONG)nxt;
    *found_out = (ULONG)found;
    return STATUS_SUCCESS;
}

// NA_CMD_MODULE: base of a module by BaseDllName in the target PEB.
// Previously declared on both sides and implemented on NEITHER (no case in
// the worker switch), so NaGetModule() always returned 0 while looking like
// a working API. Input: UTF-16 name in shm->data (NUL-padded, max 63 chars).
// Output: 8-byte base in shm->data.
static int NaPebReadFullCB(void* ctx, NA_U64 va, void* out, NA_U32 size)
{
    PNA_SCAN_CTX c = (PNA_SCAN_CTX)ctx;
    SIZE_T got = 0;
    NTSTATUS st;

    if (!na_user_range(va, (NA_U64)size)) return 0;
    st = MmCopyVirtualMemory(c->proc, (PVOID)(ULONG_PTR)va, PsGetCurrentProcess(),
                             out, (SIZE_T)size, KernelMode, &got);
    if (!NT_SUCCESS(st)) return 0;
    return got == (SIZE_T)size;
}

static NTSTATUS NaModuleByPid(ULONG pid, PNA_SHM shm)
{
    NA_SCAN_CTX ctx;
    NA_MODULE_INFO info;
    unsigned short name[NA_PEBWALK_MAX_NAME_BYTES / 2];
    ULONG chars = 0;
    PUCHAR peb;
    NTSTATUS st;
    PEPROCESS proc;

    st = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)pid, &proc);
    if (!NT_SUCCESS(st)) return st;
    peb = (PUCHAR)PsGetProcessPeb(proc);
    if (!peb || !na_user_range((NA_U64)(ULONG_PTR)peb, 0x20)) {
        ObDereferenceObject(proc);
        return STATUS_INVALID_PARAMETER;
    }

    RtlCopyMemory(name, shm->data, sizeof(name));
    name[(sizeof(name) / sizeof(name[0])) - 1] = 0;
    while (chars < (sizeof(name) / sizeof(name[0])) && name[chars]) chars++;
    if (!chars) {
        ObDereferenceObject(proc);
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(&info, sizeof(info));
    ctx.proc = proc;
    // The PEB walk reads target memory through MmCopyVirtualMemory only.
    if (!na_peb_find_module(NaPebReadFullCB, &ctx, (NA_U64)(ULONG_PTR)peb,
                            name, chars, &info)) {
        ObDereferenceObject(proc);
        return STATUS_NOT_FOUND;
    }
    ObDereferenceObject(proc);
    if (!info.base || !na_user_range(info.base, 0x1000)) return STATUS_NOT_FOUND;
    *(ULONGLONG*)shm->data = (ULONGLONG)info.base;
    return STATUS_SUCCESS;
}

static NTSTATUS NaBatch(PNA_SHM shm)
{
    // Snapshot the request before validation so a concurrent writer cannot
    // change a checked source address before MmCopyVirtualMemory consumes it.
    ULONG count = shm->count;
    ULONG total = shm->total;
    ULONG pid = shm->pid;
    NA_ITEM items[NA_BATCH_MAX];
    if (!na_batch_header_ok((NA_U32)count, (NA_U32)total))
        return STATUS_INVALID_PARAMETER;
    RtlCopyMemory(items, shm->items, count * sizeof(NA_ITEM));

    // Validate the whole batch before making any process-memory copies.
    // With PreviousMode=KernelMode, MmCopyVirtualMemory does not probe an
    // invalid kernel-range source VA; reject malformed user-writable spans.
    for (ULONG i = 0; i < count; i++) {
        if (!na_batch_item_ok(&items[i], (NA_U32)total))
            return STATUS_INVALID_PARAMETER;
    }

    PVOID tmp = ExAllocatePoolWithTag(NonPagedPool, total, POOL_TAG);
    if (!tmp) return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(tmp, total);
    PEPROCESS proc;
    NTSTATUS st = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)pid, &proc);
    if (!NT_SUCCESS(st)) { ExFreePoolWithTag(tmp, POOL_TAG); return st; }
    for (ULONG i = 0; i < count; i++) {
        const NA_ITEM* it = &items[i];
        SIZE_T copied = 0;
        MmCopyVirtualMemory(proc, (PVOID)(ULONG_PTR)it->address, PsGetCurrentProcess(),
                            (PUCHAR)tmp + it->offset, it->size, KernelMode, &copied);
        // Unreadable but structurally valid regions stay zeroed.
    }
    ObDereferenceObject(proc);
    RtlCopyMemory(shm->data, tmp, total);
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
            case NA_CMD_SCAN: {
                ULONGLONG nxt = 0; ULONG found = 0;
                shm->status = NaScan(shm, &nxt, &found);
                if (NT_SUCCESS(shm->status)) {
                    shm->address = nxt;
                    shm->count = found;
                }
                break;
            }
            case NA_CMD_ALLOC: {
                ULONGLONG val = 0;
                shm->status = NaAlloc(shm->pid, shm->address, shm->protect, &val);
                if (NT_SUCCESS(shm->status)) *(ULONGLONG*)shm->data = val;
                break;
            }
            case NA_CMD_MODULE:
                shm->status = NaModuleByPid(shm->pid, shm);
                break;
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

static VOID NaUnload(PDRIVER_OBJECT drv)
{
    UNREFERENCED_PARAMETER(drv);
    InterlockedExchange(&g_active, 0);
    if (g_worker_thread) {
        LARGE_INTEGER to;
        NTSTATUS wst;
        to.QuadPart = -10000LL * 2000;
        wst = ZwWaitForSingleObject(g_worker_thread, FALSE, &to);
        if (!NT_SUCCESS(wst)) {
            // Worker still alive inside the shared view: unmapping now would
            // free memory under a running thread (instant bugcheck). Leak the
            // view and the thread; the section dies with the process.
            return;
        }
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

    // g_active must be live BEFORE the worker exists: the worker loop is
    // `while (InterlockedAdd(&g_active, 0))`, so a thread that wins the race
    // would see 0, fall through and terminate itself - a driver that loads,
    // publishes the registry key and silently never answers a command.
    InterlockedExchange(&g_active, 1);

    InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    st = PsCreateSystemThread(&g_worker_thread, THREAD_ALL_ACCESS, &oa,
                              NULL, NULL, NaWorker, NULL);
    if (!NT_SUCCESS(st)) {
        InterlockedExchange(&g_active, 0);
        goto done;
    }

done:
    PsTerminateSystemThread(STATUS_SUCCESS);
}

/* Manual mappers call the entry point with a scratch or NULL driver object:
 * it is NOT a DRIVER_OBJECT because there is no I/O manager in the picture.
 * Writing drv->DriverUnload unconditionally was the first change to touch
 * drv at all, and the first load of that build bugchecked before the game
 * was even started. Never write through drv unless it is a kernel pointer
 * that also looks like a real driver object. */
#define NA_KERNEL_PTR_BASE 0xFFFF800000000000ULL
#define NA_IO_TYPE_DRIVER  3

static BOOLEAN NaDriverObjectWritable(PDRIVER_OBJECT drv, PUNICODE_STRING reg)
{
    if (!ARGUMENT_PRESENT(drv) || !ARGUMENT_PRESENT(reg)) return FALSE;
    if ((ULONG_PTR)drv < (ULONG_PTR)NA_KERNEL_PTR_BASE) return FALSE;
    if (!MmIsAddressValid(drv)) return FALSE;
    if (drv->Type != NA_IO_TYPE_DRIVER) return FALSE;
    if ((SIZE_T)drv->Size != sizeof(DRIVER_OBJECT)) return FALSE;
    return TRUE;
}

NTSTATUS DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING reg)
{
    // A service/sc.exe load gets a real driver object and wants run-down
    // handling; a kdmapper load gets a scratch block and can never be
    // unloaded anyway. Only wire the unload routine for the former.
    if (NaDriverObjectWritable(drv, reg)) drv->DriverUnload = NaUnload;
    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    HANDLE hThread;
    NTSTATUS st = PsCreateSystemThread(&hThread, THREAD_ALL_ACCESS, &oa,
                                       NULL, NULL, NaStartup, NULL);
    if (NT_SUCCESS(st)) ZwClose(hThread);
    return STATUS_SUCCESS;
}
