/* NZL Studio — MINIMAL WDK shim, for a host-side syntax check of sc_drv.c.
 *
 * This is NOT a WDK replacement and NOT an ABI model. It exists so that
 * `nzl/driver/check_driver.sh` can compile the driver translation unit with
 * a host compiler in CI/sandbox, catching typos, wrong argument counts,
 * undeclared identifiers and type mistakes in the code we touch. The real
 * build remains build_driver.bat (MSVC + WDK km headers); that is the only
 * thing that proves the driver binary.
 *
 * Deliberate deviations from Windows, all harmless for a syntax check:
 *   - WCHAR is the host wchar_t (4 bytes on Linux) so L"" literals typecheck;
 *   - every function is a printf-free stub with the documented signature;
 *   - STATUS_* are real NTSTATUS bit patterns (unsigned -> cast to LONG).
 */
#ifndef NZL_HOSTCHECK_NTDDK_H
#define NZL_HOSTCHECK_NTDDK_H

#include <stddef.h>
#include <stdint.h>

typedef unsigned char      UCHAR, *PUCHAR, BOOLEAN, *PBOOLEAN;
typedef unsigned short     USHORT, *PUSHORT;
typedef unsigned long      ULONG, *PULONG;
typedef long               LONG, *PLONG;
typedef int                INT32;
typedef unsigned int       UINT32;
typedef unsigned long long ULONGLONG, *PULONGLONG, UINT64;
typedef void               VOID, *PVOID;
typedef void*              HANDLE;
typedef size_t             SIZE_T;
typedef unsigned long long ULONG_PTR;
typedef char KPROCESSOR_MODE;
typedef SIZE_T* PSIZE_T;
#ifndef NTSYSAPI
#define NTSYSAPI
#endif
#ifndef NTAPI
#define NTAPI
#endif
typedef wchar_t            WCHAR, *PWCHAR, *PWSTR;
typedef long               NTSTATUS, *PNTSTATUS;
typedef struct _EPROCESS   EPROCESS, *PEPROCESS;
struct _DRIVER_OBJECT;
typedef VOID (*PDRIVER_UNLOAD)(struct _DRIVER_OBJECT*);
typedef struct _DRIVER_OBJECT {
    LONG Type;
    PDRIVER_UNLOAD DriverUnload;
} DRIVER_OBJECT, *PDRIVER_OBJECT;

#ifndef NULL
#define NULL ((void*)0)
#endif
#define TRUE 1
#define FALSE 0
#define UNREFERENCED_PARAMETER(x) (void)(x)
#define MAXLONG 0x7fffffffL

#define NT_SUCCESS(s) ((NTSTATUS)(s) >= 0)

#define STATUS_SUCCESS              ((NTSTATUS)0x00000000L)
#define STATUS_INVALID_PARAMETER    ((NTSTATUS)0xC000000DL)
#define STATUS_INVALID_DEVICE_REQUEST ((NTSTATUS)0xC0000010L)
#define STATUS_ACCESS_DENIED        ((NTSTATUS)0xC0000022L)
#define STATUS_INSUFFICIENT_RESOURCES ((NTSTATUS)0xC000009AL)
#define STATUS_PARTIAL_COPY         ((NTSTATUS)0x8000000DL)
#define STATUS_NOT_FOUND            ((NTSTATUS)0xC0000225L)
#define STATUS_UNSUCCESSFUL         ((NTSTATUS)0xC0000001L)

typedef union _LARGE_INTEGER {
    struct { ULONG LowPart; LONG HighPart; };
    long long QuadPart;
} LARGE_INTEGER, *PLARGE_INTEGER;

typedef struct _UNICODE_STRING {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR  Buffer;
} UNICODE_STRING, *PUNICODE_STRING;

typedef struct _OBJECT_ATTRIBUTES {
    ULONG Length;
    HANDLE RootDirectory;
    PUNICODE_STRING ObjectName;
    ULONG Attributes;
    PVOID SecurityDescriptor;
    PVOID SecurityQualityOfService;
} OBJECT_ATTRIBUTES, *POBJECT_ATTRIBUTES;

typedef struct _KAPC_STATE { ULONG Reserved; } KAPC_STATE, *PKAPC_STATE;

typedef struct _MEMORY_BASIC_INFORMATION {
    PVOID  BaseAddress;
    PVOID  AllocationBase;
    ULONG  AllocationProtect;
    SIZE_T RegionSize;
    ULONG  State;
    ULONG  Protect;
    ULONG  Type;
} MEMORY_BASIC_INFORMATION, *PMEMORY_BASIC_INFORMATION;

typedef struct _ACL {
    UCHAR AclRevision;
    UCHAR Sbz1;
    USHORT AclSize;
    USHORT AceCount;
    USHORT Sbz2;
} ACL, *PACL;

typedef struct _SECURITY_DESCRIPTOR {
    UCHAR Revision;
    UCHAR Sbz1;
    USHORT Control;
    PVOID Owner;
    PVOID Group;
    PACL  Sacl;
    PACL  Dacl;
} SECURITY_DESCRIPTOR, *PSECURITY_DESCRIPTOR;

typedef struct _SID_IDENTIFIER_AUTHORITY { UCHAR Value[6]; } SID_IDENTIFIER_AUTHORITY;
typedef struct _SID { UCHAR Revision; UCHAR SubAuthorityCount;
                      SID_IDENTIFIER_AUTHORITY IdentifierAuthority;
                      ULONG SubAuthority[1]; } SID, *PSID;

#define SECURITY_NT_AUTHORITY {{0,0,0,0,0,5}}
#define SECURITY_WORLD_SID_AUTHORITY {{0,0,0,0,0,1}}
#define SECURITY_LOCAL_SYSTEM_RID 18L
#define SECURITY_BUILTIN_DOMAIN_RID 32L
#define SECURITY_WORLD_RID 0L
#define DOMAIN_ALIAS_RID_ADMINS 544L
#define SECURITY_DESCRIPTOR_REVISION 1
#define ACL_REVISION 2
#define OBJ_KERNEL_HANDLE 0x00000200L
#define OBJ_CASE_INSENSITIVE 0x00000040L
#define SECTION_ALL_ACCESS 0xF001FL
#define SECTION_MAP_READ 0x0004L
#define SECTION_MAP_WRITE 0x0002L
#define SECTION_QUERY 0x0001L
#define SEC_COMMIT 0x08000000L
#define PAGE_READONLY 0x02L
#define PAGE_READWRITE 0x04L
#define PAGE_WRITECOPY 0x08L
#define PAGE_EXECUTE_READ 0x20L
#define PAGE_EXECUTE_READWRITE 0x40L
#define PAGE_EXECUTE_WRITECOPY 0x80L
#define PAGE_GUARD 0x100L
#define MEM_COMMIT 0x1000L
#define MEM_RESERVE 0x2000L
#define MEM_PRIVATE 0x20000L
#define MemoryBasicInformation 0
#define KernelMode 0
#define UserMode 1
#define NonPagedPool 1
#define PagedPool 0
#define THREAD_ALL_ACCESS 0x1FFFFFL
#define KEY_ALL_ACCESS 0xF003FL
#define KEY_READ 0x20019L
#define REG_OPTION_VOLATILE 0x00000001L
#define REG_DWORD 4
#define REG_SZ 1

/* --- kernel API stubs (documented signatures, body irrelevant) --------- */
NTSTATUS PsLookupProcessByProcessId(HANDLE pid, PEPROCESS* proc);
PEPROCESS PsGetCurrentProcess(VOID);
VOID     ObDereferenceObject(PVOID object);
VOID     KeStackAttachProcess(PEPROCESS proc, PKAPC_STATE apc);
VOID     KeUnstackDetachProcess(PKAPC_STATE apc);
NTSTATUS ZwQueryVirtualMemory(HANDLE proc, PVOID base, ULONG cls, PVOID info,
                              SIZE_T len, SIZE_T* ret);
HANDLE   ZwCurrentProcess(VOID);
NTSTATUS ZwAllocateVirtualMemory(HANDLE proc, PVOID* base, ULONG_PTR zero_bits,
                                 SIZE_T* size, ULONG type, ULONG protect);
NTSTATUS ZwCreateSection(HANDLE* sec, ULONG access, POBJECT_ATTRIBUTES oa,
                         PLARGE_INTEGER size, ULONG protect, ULONG attrs, HANDLE file);
NTSTATUS ZwMapViewOfSection(HANDLE sec, HANDLE proc, PVOID* base, ULONG_PTR zero,
                            SIZE_T commit, PLARGE_INTEGER offset, SIZE_T* view_size,
                            ULONG inherit, ULONG alloc_type, ULONG protect);
NTSTATUS ZwUnmapViewOfSection(HANDLE proc, PVOID base);
NTSTATUS ZwClose(HANDLE h);
NTSTATUS ZwCreateKey(HANDLE* key, ULONG access, POBJECT_ATTRIBUTES oa, ULONG title_idx,
                     PUNICODE_STRING cls, ULONG options, PULONG disposition);
NTSTATUS ZwOpenKey(HANDLE* key, ULONG access, POBJECT_ATTRIBUTES oa);
NTSTATUS ZwDeleteKey(HANDLE key);
NTSTATUS ZwSetValueKey(HANDLE key, PUNICODE_STRING name, ULONG index, ULONG type,
                       PVOID data, ULONG size);
NTSTATUS ZwWaitForSingleObject(HANDLE h, BOOLEAN alertable, PLARGE_INTEGER timeout);
NTSTATUS PsCreateSystemThread(HANDLE* thread, ULONG access, POBJECT_ATTRIBUTES oa,
                              HANDLE proc, PVOID cid, VOID (*start)(PVOID), PVOID ctx);
NTSTATUS PsTerminateSystemThread(NTSTATUS status);
NTSTATUS KeDelayExecutionThread(ULONG mode, BOOLEAN alertable, PLARGE_INTEGER interval);
VOID     KeQueryTickCount(PLARGE_INTEGER tick);
PVOID    ExAllocatePoolWithTag(ULONG type, SIZE_T size, ULONG tag);
VOID     ExFreePoolWithTag(PVOID p, ULONG tag);
VOID     RtlZeroMemory(PVOID dst, SIZE_T len);
VOID     RtlCopyMemory(PVOID dst, const VOID* src, SIZE_T len);
VOID     RtlInitUnicodeString(PUNICODE_STRING dst, const WCHAR* src);
VOID     InitializeObjectAttributes(POBJECT_ATTRIBUTES oa, PUNICODE_STRING name,
                                    ULONG attrs, HANDLE root, PVOID sd);
NTSTATUS RtlCreateSecurityDescriptor(PSECURITY_DESCRIPTOR sd, ULONG revision);
NTSTATUS RtlCreateAcl(PACL acl, ULONG size, ULONG revision);
NTSTATUS RtlAddAccessAllowedAce(PACL acl, ULONG revision, ULONG access, PSID sid);
NTSTATUS RtlSetDaclSecurityDescriptor(PSECURITY_DESCRIPTOR sd, BOOLEAN dacl_present,
                                      PACL dacl, BOOLEAN dacl_defaulted);
NTSTATUS RtlAbsoluteToSelfRelativeSD(PSECURITY_DESCRIPTOR abs, PSECURITY_DESCRIPTOR rel,
                                     PULONG size);
NTSTATUS RtlInitializeSid(PSID sid, const SID_IDENTIFIER_AUTHORITY* auth, UCHAR count);
PULONG   RtlSubAuthoritySid(PSID sid, ULONG index);
LONG     InterlockedAdd(volatile LONG* target, LONG value);
LONG     InterlockedExchange(volatile LONG* target, LONG value);

#endif
