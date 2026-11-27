#include "sc_client.h"
#include <mmsystem.h>
#include <cstdio>
#include <cwchar>
#include <cstring>

static HANDLE  g_map = nullptr;
static NaShm*  g_shm = nullptr;
static UINT32  g_key = 0;

static NaShm* Send(UINT32 cmd)
{
    // NZL: timeout 2000 ms. Original spun forever; if the driver dies
    // NZL must report failure instead of hanging the overlay thread.
    g_shm->key = g_key;
    g_shm->cmd = cmd;
    InterlockedIncrement(&g_shm->seq);
    LONG target = g_shm->seq;
    ULONGLONG start = GetTickCount64();
    while (g_shm->seq_out != target) {
        if (GetTickCount64() - start > 2000)
            return nullptr;
        YieldProcessor();
    }
    return g_shm;
}

bool NaInit()
{
    // The driver worker polls with a sub-ms KeDelay, but Windows timer
    // granularity (~15.6ms default) made EVERY command cost ~16ms
    // (region scan ~80s, heap scan ~370s). Raising the system timer
    // resolution to 1ms from the client shrinks that wake latency an
    // order of magnitude without touching the driver.
    timeBeginPeriod(1);
    HKEY hk;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\NaCoreFilter",
                      0, KEY_READ, &hk) != ERROR_SUCCESS)
        return false;

    DWORD type, sz;

    DWORD keyVal = 0;
    sz = sizeof(keyVal);
    RegQueryValueExW(hk, L"Key", nullptr, &type, (BYTE*)&keyVal, &sz);

    WCHAR shmSuffix[16] = {};
    sz = sizeof(shmSuffix);
    RegQueryValueExW(hk, L"Shm", nullptr, &type, (BYTE*)shmSuffix, &sz);

    RegCloseKey(hk);

    if (!keyVal || !shmSuffix[0])
        return false;

    WCHAR fullName[32];
    swprintf_s(fullName, L"Global\\%s", shmSuffix);

    g_map = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, fullName);
    if (!g_map)
        return false;

    g_shm = (NaShm*)MapViewOfFile(g_map, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(NaShm));
    if (!g_shm) {
        CloseHandle(g_map);
        g_map = nullptr;
        return false;
    }

    g_key = keyVal;
    return true;
}

void NaClose()
{
    if (g_shm) { UnmapViewOfFile(g_shm); g_shm = nullptr; }
    if (g_map) { CloseHandle(g_map);     g_map = nullptr; }
    g_key = 0;
}

bool NaRead(UINT32 pid, UINT64 addr, UINT32 size, void* out)
{
    if (!g_shm || size > NA_MAX_XFER)
        return false;
    g_shm->pid     = pid;
    g_shm->address = addr;
    g_shm->size    = size;
    NaShm* s = Send(NA_CMD_READ);
    if (!s || s->status < 0)
        return false;
    memcpy(out, s->data, size);
    return true;
}

bool NaWrite(UINT32 pid, UINT64 addr, UINT32 size, void* in)
{
    if (!g_shm || size > NA_MAX_XFER)
        return false;
    g_shm->pid     = pid;
    g_shm->address = addr;
    g_shm->size    = size;
    memcpy(g_shm->data, in, size);
    NaShm* s = Send(NA_CMD_WRITE);
    return s && s->status >= 0;
}

UINT64 NaGetBase(UINT32 pid)
{
    if (!g_shm) return 0;
    g_shm->pid = pid;
    NaShm* s = Send(NA_CMD_BASE);
    return (s && s->status >= 0) ? *(UINT64*)s->data : 0;
}

UINT64 NaGetPeb(UINT32 pid)
{
    if (!g_shm) return 0;
    g_shm->pid = pid;
    NaShm* s = Send(NA_CMD_PEB);
    return (s && s->status >= 0) ? *(UINT64*)s->data : 0;
}

bool NaQueryRegion(UINT32 pid, UINT64 addr, NaRegion* out)
{
    if (!g_shm) return false;
    g_shm->pid     = pid;
    g_shm->address = addr;
    NaShm* s = Send(NA_CMD_REGION);
    if (!s || s->status < 0)
        return false;
    *out = s->region;
    return true;
}

bool NaBatchRead(UINT32 pid, NaItem* items, UINT32 count, void* out, UINT32 total)
{
    if (!g_shm || !items || !count || count > NA_BATCH_MAX || !total || total > NA_MAX_XFER)
        return false;
    g_shm->pid   = pid;
    g_shm->count = count;
    g_shm->total = total;
    memcpy(g_shm->items, items, count * sizeof(NaItem));
    NaShm* s = Send(NA_CMD_BATCH);
    if (!s || s->status < 0)
        return false;
    memcpy(out, s->data, total);
    return true;
}

UINT32 NaScan(UINT32 pid, UINT64 pattern, UINT64 start, UINT64* candidates, UINT32 maxcand, UINT64* next)
{
    if (!g_shm || !candidates || !maxcand || maxcand > 256) return 0;
    g_shm->pid     = pid;
    g_shm->address = start;
    g_shm->count   = maxcand;
    memcpy(g_shm->data, &pattern, 8);
    NaShm* s = Send(NA_CMD_SCAN);
    if (!s || s->status < 0) { if (next) *next = 0; return 0; }
    UINT32 found = s->count;
    if (found > maxcand) found = maxcand;
    memcpy(candidates, s->data, found * 8);
    if (next) *next = s->address;
    return found;
}

bool NaAlloc(UINT32 pid, UINT64 size, UINT32 protect, UINT64* out)
{
    if (!g_shm) return false;
    g_shm->pid     = pid;
    g_shm->address = size;
    g_shm->protect = protect;
    NaShm* s = Send(NA_CMD_ALLOC);
    if (!s || s->status < 0)
        return false;
    *out = *(UINT64*)s->data;
    return true;
}

NaShm* NaGetShm()  { return g_shm; }
UINT32 NaGetKey()  { return g_key; }

UINT64 NaGetModule(UINT32 pid, const wchar_t* name)
{
    if (!g_shm || !name) return 0;
    g_shm->pid = pid;
    memset(g_shm->data, 0, 256);
    size_t len = wcslen(name);
    if (len > 63) len = 63;
    memcpy(g_shm->data, name, len * sizeof(wchar_t));
    NaShm* s = Send(NA_CMD_MODULE);
    return (s && s->status >= 0) ? *(UINT64*)s->data : 0;
}
