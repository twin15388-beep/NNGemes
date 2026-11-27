# Driver Usage Guide

---

## Requirements

- Windows 10 / 11 x64
- [kdmapper](https://github.com/TheCruZ/kdmapper)
- Administrator privileges

---

## Loading the Driver

```
kdmapper.exe driver.sys
```

After loading, the driver writes two values to the registry:

```
HKEY_LOCAL_MACHINE\SYSTEM\NaCoreFilter
  Key  (DWORD)
  Shm  (REG_SZ)
```

`NaInit()` reads these automatically.

---

## Adding to Your Project

Drop `sc_client.h` and `sc_client.cpp` into your project and include the header.

---

## Init / Shutdown

```cpp
if (!NaInit())
    return; // driver not loaded

// ...

NaClose();
```

---

## Read

```cpp
UINT32 pid  = ...;
UINT64 base = NaGetBase(pid);

// typed read
UINT32 val = NaRead<UINT32>(pid, base + 0x1234);
UINT64 ptr = NaRead<UINT64>(pid, base + 0x5678);
float  f   = NaRead<float> (pid, base + 0x9ABC);

// read into a struct
MyStruct s = NaRead<MyStruct>(pid, ptr);

// raw bytes
UINT8 buf[64];
NaRead(pid, addr, sizeof(buf), buf);
```

---

## Write

```cpp
NaWrite<UINT32>(pid, addr, 1337);
NaWrite<float> (pid, addr, 1.0f);
```

---

## Pointer Chain

```cpp
UINT64 p0 = NaRead<UINT64>(pid, base + 0x100);
UINT64 p1 = NaRead<UINT64>(pid, p0 + 0x20);
UINT32 v  = NaRead<UINT32>(pid, p1 + 0x8);
```

---

## Get Base / PEB

```cpp
UINT64 base = NaGetBase(pid);
UINT64 peb  = NaGetPeb(pid);
```

---

## Query Region

```cpp
NaRegion r{};
NaQueryRegion(pid, addr, &r);
// r.base, r.size, r.state, r.protect, r.type
```

---

## Allocate in Target Process

```cpp
UINT64 alloc = 0;
NaAlloc(pid, 0x1000, PAGE_EXECUTE_READWRITE, &alloc);
```

---

## Batch Read

Reads up to 64 addresses in one call. Each item specifies a source address and an offset into the output buffer where the result lands.

```cpp
// expose g_shm from sc_client.cpp or add a getter
NaShm* shm = NaGetShm();

shm->pid   = pid;
shm->key   = NaGetKey();
shm->count = 3;
shm->total = sizeof(UINT32) * 3;

shm->items[0] = { addr0, 0,             sizeof(UINT32) };
shm->items[1] = { addr1, sizeof(UINT32), sizeof(UINT32) };
shm->items[2] = { addr2, sizeof(UINT32) * 2, sizeof(UINT32) };

InterlockedIncrement(&shm->seq);
LONG t = shm->seq;
while (shm->seq_out != t) YieldProcessor();

UINT32 v0 = *(UINT32*)(shm->data + 0);
UINT32 v1 = *(UINT32*)(shm->data + sizeof(UINT32));
UINT32 v2 = *(UINT32*)(shm->data + sizeof(UINT32) * 2);
```
