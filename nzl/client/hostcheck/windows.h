/* NZL Studio — MINIMAL Win32 shim, for a host-side syntax check of the
 * shared-memory client (nzl/client/sc_client.cpp).
 *
 * Not a Windows SDK, not an ABI model: it exists so check_client.sh can
 * compile the client translation unit with a host compiler and catch typos,
 * wrong argument counts and type drift against na_wire.h. The real client is
 * built by MSVC in NZL-BuildAll.ps1.
 */
#ifndef NZL_HOSTCHECK_WINDOWS_H
#define NZL_HOSTCHECK_WINDOWS_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>

typedef void*        HANDLE;
typedef void*        HKEY;
typedef unsigned long DWORD;      /* Win32 DWORD is 32-bit; see -m32 note */
typedef int          LONG;   /* matches NA_I32 on the host; both 32-bit */
typedef int          BOOL;
typedef unsigned char BYTE;
typedef unsigned char UINT8;
typedef unsigned int  UINT32;
typedef unsigned long long UINT64;
typedef unsigned long long ULONGLONG;
typedef wchar_t      WCHAR;
typedef const wchar_t* LPCWSTR;

#ifndef FALSE
#define FALSE 0
#endif
#ifndef TRUE
#define TRUE 1
#endif
#ifndef NULL
#define NULL 0
#endif

#define HKEY_LOCAL_MACHINE ((HKEY)0x80000002)
#define KEY_READ 0x20019
#define ERROR_SUCCESS 0L
#define FILE_MAP_READ 0x0004
#define FILE_MAP_WRITE 0x0002
#define TIMERR_NOERROR 0
#define REG_DWORD 4
#define REG_SZ 1

LONG RegOpenKeyExW(HKEY key, LPCWSTR sub, DWORD opts, DWORD access, HKEY* out);
LONG RegQueryValueExW(HKEY key, LPCWSTR name, DWORD* reserved, DWORD* type,
                      BYTE* data, DWORD* size);
LONG RegCloseKey(HKEY key);
HANDLE OpenFileMappingW(DWORD access, BOOL inherit, LPCWSTR name);
void* MapViewOfFile(HANDLE mapping, DWORD access, DWORD hi, DWORD lo, size_t size);
BOOL UnmapViewOfFile(const void* base);
BOOL CloseHandle(HANDLE h);
unsigned long long GetTickCount64(void);
void YieldProcessor(void);
LONG InterlockedIncrement(volatile LONG* value);
int swprintf_s(wchar_t* buffer, size_t count, const wchar_t* format, ...);
/* MSVC provides an array-bound template overload; mirror it so
 * swprintf_s(buf, fmt, arg) type-checks the same way. */
template <size_t N>
int swprintf_s(wchar_t (&buffer)[N], const wchar_t* format, ...);

#endif
