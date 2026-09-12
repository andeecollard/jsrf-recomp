/*
 * kernel_rtl.c - Xbox Runtime Library Functions
 *
 * Implements Rtl* functions: critical sections, string init/conversion,
 * NTSTATUS→Win32 error mapping, time conversion, sprintf variants.
 *
 * Most of these map 1:1 to Win32 CRT functions.
 */

#include "kernel.h"
#include "xbox_memory_layout.h"   /* RECOMP_TLS, for the guest-thread g_esp */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ============================================================================
 * String Initialization
 * ============================================================================ */

VOID __stdcall xbox_RtlInitAnsiString(PXBOX_ANSI_STRING DestinationString, const char* SourceString)
{
    if (SourceString) {
        USHORT len = (USHORT)strlen(SourceString);
        DestinationString->Length = len;
        DestinationString->MaximumLength = len + 1;
        DestinationString->Buffer = (PCHAR)SourceString;
    } else {
        DestinationString->Length = 0;
        DestinationString->MaximumLength = 0;
        DestinationString->Buffer = NULL;
    }
}

VOID __stdcall xbox_RtlInitUnicodeString(PXBOX_UNICODE_STRING DestinationString, const WCHAR* SourceString)
{
    if (SourceString) {
        /* macOS portability: Xbox WCHAR is 16-bit; host wchar_t is 32-bit, so
         * use the runtime's 16-bit-aware xbox_wcslen (standard wcslen miscounts). */
        USHORT len = (USHORT)(xbox_wcslen(SourceString) * sizeof(WCHAR));
        DestinationString->Length = len;
        DestinationString->MaximumLength = len + sizeof(WCHAR);
        DestinationString->Buffer = (PWCHAR)SourceString;
    } else {
        DestinationString->Length = 0;
        DestinationString->MaximumLength = 0;
        DestinationString->Buffer = NULL;
    }
}

/* ============================================================================
 * String Conversion (ANSI ↔ Unicode)
 * ============================================================================ */

NTSTATUS __stdcall xbox_RtlAnsiStringToUnicodeString(
    PXBOX_UNICODE_STRING DestinationString,
    PXBOX_ANSI_STRING SourceString,
    BOOLEAN AllocateDestinationString)
{
    ULONG unicode_len;

    if (!DestinationString || !SourceString)
        return STATUS_INVALID_PARAMETER;

    unicode_len = (SourceString->Length + 1) * sizeof(WCHAR);

    if (AllocateDestinationString) {
        DestinationString->Buffer = (PWCHAR)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, unicode_len);
        if (!DestinationString->Buffer)
            return STATUS_NO_MEMORY;
        DestinationString->MaximumLength = (USHORT)unicode_len;
    } else if (DestinationString->MaximumLength < unicode_len) {
        return STATUS_BUFFER_OVERFLOW;
    }

    int result = MultiByteToWideChar(CP_ACP, 0,
        SourceString->Buffer, SourceString->Length,
        DestinationString->Buffer, DestinationString->MaximumLength / sizeof(WCHAR));

    if (result > 0) {
        DestinationString->Length = (USHORT)(result * sizeof(WCHAR));
        DestinationString->Buffer[result] = L'\0';
        return STATUS_SUCCESS;
    }

    return STATUS_UNSUCCESSFUL;
}

NTSTATUS __stdcall xbox_RtlUnicodeStringToAnsiString(
    PXBOX_ANSI_STRING DestinationString,
    PXBOX_UNICODE_STRING SourceString,
    BOOLEAN AllocateDestinationString)
{
    ULONG ansi_len;

    if (!DestinationString || !SourceString)
        return STATUS_INVALID_PARAMETER;

    ansi_len = SourceString->Length / sizeof(WCHAR) + 1;

    if (AllocateDestinationString) {
        DestinationString->Buffer = (PCHAR)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, ansi_len);
        if (!DestinationString->Buffer)
            return STATUS_NO_MEMORY;
        DestinationString->MaximumLength = (USHORT)ansi_len;
    } else if (DestinationString->MaximumLength < ansi_len) {
        return STATUS_BUFFER_OVERFLOW;
    }

    int result = WideCharToMultiByte(CP_ACP, 0,
        SourceString->Buffer, SourceString->Length / sizeof(WCHAR),
        DestinationString->Buffer, DestinationString->MaximumLength,
        NULL, NULL);

    if (result >= 0) {
        DestinationString->Length = (USHORT)result;
        if ((USHORT)result < DestinationString->MaximumLength)
            DestinationString->Buffer[result] = '\0';
        return STATUS_SUCCESS;
    }

    return STATUS_UNSUCCESSFUL;
}

/* ============================================================================
 * String Comparison
 * ============================================================================ */

BOOLEAN __stdcall xbox_RtlEqualString(
    PXBOX_ANSI_STRING String1,
    PXBOX_ANSI_STRING String2,
    BOOLEAN CaseInSensitive)
{
    if (String1->Length != String2->Length)
        return FALSE;

    if (CaseInSensitive)
        return _strnicmp(String1->Buffer, String2->Buffer, String1->Length) == 0;
    else
        return strncmp(String1->Buffer, String2->Buffer, String1->Length) == 0;
}

/*
 * RtlCompareMemoryUlong - Scans memory for a ULONG pattern.
 * Returns the number of bytes that matched.
 */
ULONG __stdcall xbox_RtlCompareMemoryUlong(PVOID Source, ULONG Length, ULONG Pattern)
{
    PULONG src = (PULONG)Source;
    ULONG count = Length / sizeof(ULONG);

    for (ULONG i = 0; i < count; i++) {
        if (src[i] != Pattern)
            return i * sizeof(ULONG);
    }
    return count * sizeof(ULONG);
}

/* ============================================================================
 * Critical Sections (direct 1:1 mapping)
 * ============================================================================ */

/* Xbox critical sections are 20-byte, 32-bit structures and cannot contain a
 * host CRITICAL_SECTION (40 bytes on Win64 and larger in the POSIX shim).
 * Keep the host lock in a shadow table keyed by the translated guest address.
 *
 * This used to be a no-op while every translated thread ran synchronously.
 * Real guest workers make that actively unsafe: JSRF's shared RTL heap uses
 * one of these locks, and concurrent allocate/free operations corrupt its
 * intrusive free lists without it. */
typedef struct XboxShadowCriticalSection {
    PRTL_CRITICAL_SECTION guest;
    CRITICAL_SECTION host;
    /* Who holds it, and how deep.
     *
     * A contention report can already say which lock and which thread is
     * waiting. What it cannot say is who is holding it, and that is the half
     * that points at code. Written after the lock is held and cleared as the
     * last recursion is released, so a waiter reads a consistent pair or a
     * stale one -- never a torn value. Nothing depends on them being current;
     * they exist to be printed. */
    volatile unsigned long owner_tid;
    volatile LONG depth;
    struct XboxShadowCriticalSection *next;
} XboxShadowCriticalSection;

static XboxShadowCriticalSection *g_shadow_critical_sections;
static volatile LONG g_shadow_critical_sections_guard;

static void shadow_critical_sections_lock(void)
{
    while (InterlockedCompareExchange(&g_shadow_critical_sections_guard,
                                      1, 0) != 0) {
        Sleep(0);
    }
}

static void shadow_critical_sections_unlock(void)
{
    InterlockedExchange(&g_shadow_critical_sections_guard, 0);
}

static XboxShadowCriticalSection *shadow_critical_section_get(
    PRTL_CRITICAL_SECTION guest, BOOL create)
{
    XboxShadowCriticalSection *entry;

    if (!guest)
        return NULL;

    shadow_critical_sections_lock();
    for (entry = g_shadow_critical_sections; entry; entry = entry->next) {
        if (entry->guest == guest)
            break;
    }
    if (!entry && create) {
        entry = (XboxShadowCriticalSection *)malloc(sizeof(*entry));
        if (entry) {
            entry->guest = guest;
            entry->owner_tid = 0;    /* malloc does not zero, and a waiter
                                      * prints these before anything sets
                                      * them */
            entry->depth = 0;
            InitializeCriticalSection(&entry->host);
            entry->next = g_shadow_critical_sections;
            g_shadow_critical_sections = entry;
        }
    }
    shadow_critical_sections_unlock();
    return entry;
}

/* ----------------------------------------------------------------------------
 * Lock diagnostics
 *
 * Restored from upstream v0.8.0, which found them by chasing a Half-Life 2
 * deadlock: a missed function boundary skipped an epilogue, the epilogue was
 * where _unlock lived, and a recursive lock meant the holder sailed on while
 * only the SECOND thread blocked. That reads as an AB-BA deadlock between two
 * locks rather than one lock leaking, and nothing in a silent hang tells you
 * which. These are the instruments that separate them.
 *
 * They were lost here in merge 36b4076, which resolved this file in favour of
 * the JSRF side. Rebuilt on our own shadow list rather than transplanted:
 * upstream keys an open-addressed table with an SRWLOCK and an INIT_ONCE, and
 * this tree also builds on macOS, where the POSIX shim has neither.
 * ------------------------------------------------------------------------- */

extern ptrdiff_t g_xbox_mem_offset;
extern RECOMP_TLS uint32_t g_esp;
extern uint32_t g_xbox_code_lo, g_xbox_code_hi;

static LONG g_cs_contention_reports;
static LONG g_cs_enters, g_cs_leaves;

/* A guest backtrace, printed where the guest is standing.
 *
 * Runs on the guest thread, which is what makes it real: g_esp is thread-local
 * and meaningless anywhere else. Lifted calls push their guest return address
 * before jumping, so scanning the stack for words landing in the code range
 * recovers the chain. Approximate by construction -- a stale word from an
 * earlier call can look like a frame -- and still the difference between
 * knowing two threads are stuck and knowing which two paths did it. */
static void xbox_cs_guest_backtrace(int depth)
{
    uint32_t sp = g_esp;
    int shown = 0, i;

    if (!sp || !g_xbox_code_hi)
        return;
    for (i = 0; i < 256 && shown < depth; i++) {
        uint32_t word = *(const uint32_t *)((uintptr_t)(sp + (uint32_t)i * 4)
                                            + g_xbox_mem_offset);
        if (word >= g_xbox_code_lo && word < g_xbox_code_hi) {
            fprintf(stderr, "  [CS]     guest 0x%08X\n", word);
            shown++;
        }
    }
}

static uint32_t cs_guest_va(PRTL_CRITICAL_SECTION guest)
{
    return (uint32_t)((uintptr_t)guest - (uintptr_t)g_xbox_mem_offset);
}

/* Which CRT lock is this?
 *
 * MSVC keeps its internal locks in a table of {CRITICAL_SECTION*, refcount}
 * pairs, so a contended address can be named by index rather than left as a
 * bare pointer: 1 is the stdio scan lock, 4 the heap, 16 upwards the per-stream
 * locks. The table address is per-title, so it is supplied, never assumed. */
static uint32_t g_crt_lock_table, g_crt_lock_count;

void xbox_SetCrtLockTable(uint32_t table_va, uint32_t count)
{
    g_crt_lock_table = table_va;
    g_crt_lock_count = count;
}

static int xbox_crt_lock_index(uint32_t guest_va)
{
    uint32_t i;

    if (!g_crt_lock_table)
        return -1;
    for (i = 0; i < g_crt_lock_count; i++) {
        uint32_t slot = *(const uint32_t *)((uintptr_t)(g_crt_lock_table + i * 8)
                                            + g_xbox_mem_offset);
        if (slot == guest_va)
            return (int)i;
    }
    return -1;
}

/* Every acquire and release of a lock the CRT names, in order.
 *
 * A deadlock report says which locks are crossed but not how they got that
 * way, and the two explanations need opposite fixes: the title really does take
 * them in two orders (its problem, and it shipped, so unlikely), or this
 * runtime dropped a release and a lock that should be free is still held (our
 * problem). The order in this log tells them apart.
 *
 * Gated because it is a table scan per lock operation and CRT locks are hot:
 *   RECOMP_CS_TRACE_CRT=1     locks the table names, by index
 *   RECOMP_CS_TRACE_CRT=all   every lock, by address as well as index
 *   RECOMP_CS_WATCH=<va>      one lock, both sides, with a guest backtrace
 */
static void crt_lock_trace(const char *what, PRTL_CRITICAL_SECTION guest)
{
    static int enabled = -1, all;
    uint32_t va;
    int idx;

    if (enabled < 0) {
        const char *v = getenv("RECOMP_CS_TRACE_CRT");
        enabled = v != NULL;
        all = v && !strcmp(v, "all");
    }
    va = cs_guest_va(guest);

    /* The watch is independent of the trace: a take at one address and a
     * release 0x78 lower is not a protocol this runtime can reason about from
     * addresses alone -- it needs both call sites, because the question is
     * whether the guest computed different addresses or we did. */
    {
        static int watch = -1;
        static uint32_t watch_va;
        if (watch < 0) {
            const char *w = getenv("RECOMP_CS_WATCH");
            watch = w != NULL;
            watch_va = w ? (uint32_t)strtoul(w, NULL, 0) : 0;
        }
        if (watch && va == watch_va) {
            fprintf(stderr, "  [CSWATCH] t%-6lu %s 0x%08X\n",
                    (unsigned long)GetCurrentThreadId(), what, va);
            xbox_cs_guest_backtrace(8);
            fflush(stderr);
        }
    }

    if (!enabled)
        return;
    idx = xbox_crt_lock_index(va);
    if (all) {
        fprintf(stderr, "  [CSTRACE] t%-6lu %-4s 0x%08X idx %d\n",
                (unsigned long)GetCurrentThreadId(), what, va, idx);
        fflush(stderr);
        return;
    }
    if (idx < 0)
        return;
    fprintf(stderr, "  [CSTRACE] t%-6lu %-4s lock %d\n",
            (unsigned long)GetCurrentThreadId(), what, idx);
    fflush(stderr);
}

VOID __stdcall xbox_RtlEnterCriticalSection(PRTL_CRITICAL_SECTION CriticalSection)
{
    XboxShadowCriticalSection *entry =
        shadow_critical_section_get(CriticalSection, TRUE);
    if (!entry)
        return;

    InterlockedIncrement(&g_cs_enters);
    crt_lock_trace("take", CriticalSection);

    /* TryEnter first, so the uncontended path -- which is nearly all of them --
     * costs one extra call and no reporting. */
    if (TryEnterCriticalSection(&entry->host)) {
        entry->owner_tid = (unsigned long)GetCurrentThreadId();
        entry->depth++;
        return;
    }

    /* Contention is worth saying out loud. These locks are real now, so a
     * title that leaks one, or a thread of ours holding one somewhere the
     * console's would not, deadlocks instead of sailing through -- and that
     * looks like a hang with no output at all. Capped so a merely busy lock
     * cannot bury the log. */
    if (InterlockedIncrement(&g_cs_contention_reports) <= 16) {
        uint32_t va = cs_guest_va(CriticalSection);
        int idx = xbox_crt_lock_index(va);
        fprintf(stderr, "  [CS] thread %lu waiting on guest lock 0x%08X\n",
                (unsigned long)GetCurrentThreadId(), va);
        if (idx >= 0)
            fprintf(stderr, "  [CS]   that is CRT lock %d\n", idx);
        if (entry->owner_tid)
            fprintf(stderr, "  [CS]   held by thread %lu (depth %ld)\n",
                    entry->owner_tid, (long)entry->depth);
        fprintf(stderr, "  [CS] enters=%ld leaves=%ld (outstanding %ld)\n",
                (long)g_cs_enters, (long)g_cs_leaves,
                (long)(g_cs_enters - g_cs_leaves));
        xbox_cs_guest_backtrace(10);
        fflush(stderr);
    }

    EnterCriticalSection(&entry->host);
    entry->owner_tid = (unsigned long)GetCurrentThreadId();
    entry->depth++;
    if (g_cs_contention_reports <= 16) {
        fprintf(stderr, "  [CS] thread %lu acquired 0x%08X\n",
                (unsigned long)GetCurrentThreadId(),
                cs_guest_va(CriticalSection));
        fflush(stderr);
    }
}

VOID __stdcall xbox_RtlLeaveCriticalSection(PRTL_CRITICAL_SECTION CriticalSection)
{
    XboxShadowCriticalSection *entry =
        shadow_critical_section_get(CriticalSection, FALSE);
    if (!entry)
        return;
    InterlockedIncrement(&g_cs_leaves);
    crt_lock_trace("drop", CriticalSection);
    if (--entry->depth <= 0) {
        entry->depth = 0;
        entry->owner_tid = 0;
    }
    LeaveCriticalSection(&entry->host);
}

VOID __stdcall xbox_RtlInitializeCriticalSection(PRTL_CRITICAL_SECTION CriticalSection)
{
    /* Creating the shadow here is not required -- Enter does it -- but doing
     * it now keeps the first Enter off the allocating path. */
    (void)shadow_critical_section_get(CriticalSection, TRUE);
}

/* ============================================================================
 * NTSTATUS → Win32 Error Code Mapping
 * ============================================================================ */

ULONG __stdcall xbox_RtlNtStatusToDosError(NTSTATUS Status)
{
    switch (Status) {
        case STATUS_SUCCESS:                    return ERROR_SUCCESS;
        case STATUS_INVALID_PARAMETER:          return ERROR_INVALID_PARAMETER;
        case STATUS_NO_MEMORY:                  return ERROR_NOT_ENOUGH_MEMORY;
        case STATUS_INSUFFICIENT_RESOURCES:     return ERROR_NO_SYSTEM_RESOURCES;
        case STATUS_ACCESS_DENIED:              return ERROR_ACCESS_DENIED;
        case STATUS_OBJECT_NAME_NOT_FOUND:      return ERROR_FILE_NOT_FOUND;
        case STATUS_OBJECT_PATH_NOT_FOUND:      return ERROR_PATH_NOT_FOUND;
        case STATUS_OBJECT_NAME_COLLISION:      return ERROR_ALREADY_EXISTS;
        case STATUS_NO_SUCH_FILE:               return ERROR_FILE_NOT_FOUND;
        case STATUS_END_OF_FILE:                return ERROR_HANDLE_EOF;
        case STATUS_INVALID_HANDLE:             return ERROR_INVALID_HANDLE;
        case STATUS_NOT_IMPLEMENTED:            return ERROR_CALL_NOT_IMPLEMENTED;
        case STATUS_UNSUCCESSFUL:               return ERROR_GEN_FAILURE;
        case STATUS_PENDING:                    return ERROR_IO_PENDING;
        case STATUS_BUFFER_OVERFLOW:            return ERROR_MORE_DATA;
        case STATUS_NO_MORE_FILES:              return ERROR_NO_MORE_FILES;
        case STATUS_NOT_SUPPORTED:              return ERROR_NOT_SUPPORTED;
        case STATUS_CANCELLED:                  return ERROR_CANCELLED;
        case STATUS_ALREADY_COMMITTED:          return ERROR_COMMITMENT_LIMIT;
        default:
            /* Fall back to RtlNtStatusToDosError from ntdll if available */
            xbox_log(XBOX_LOG_WARN, XBOX_LOG_RTL,
                "RtlNtStatusToDosError: unmapped status 0x%08X", Status);
            return ERROR_MR_MID_NOT_FOUND;
    }
}

/* ============================================================================
 * Time Conversion
 * ============================================================================ */

BOOLEAN __stdcall xbox_RtlTimeFieldsToTime(PXBOX_TIME_FIELDS TimeFields, PLARGE_INTEGER Time)
{
    SYSTEMTIME st;
    FILETIME ft;

    st.wYear         = (WORD)TimeFields->Year;
    st.wMonth        = (WORD)TimeFields->Month;
    st.wDayOfWeek    = (WORD)TimeFields->Weekday;
    st.wDay          = (WORD)TimeFields->Day;
    st.wHour         = (WORD)TimeFields->Hour;
    st.wMinute       = (WORD)TimeFields->Minute;
    st.wSecond       = (WORD)TimeFields->Second;
    st.wMilliseconds = (WORD)TimeFields->Milliseconds;

    if (!SystemTimeToFileTime(&st, &ft))
        return FALSE;

    Time->LowPart  = ft.dwLowDateTime;
    Time->HighPart = ft.dwHighDateTime;
    return TRUE;
}

VOID __stdcall xbox_RtlTimeToTimeFields(PLARGE_INTEGER Time, PXBOX_TIME_FIELDS TimeFields)
{
    FILETIME ft;
    SYSTEMTIME st;

    ft.dwLowDateTime  = Time->LowPart;
    ft.dwHighDateTime = Time->HighPart;

    if (FileTimeToSystemTime(&ft, &st)) {
        TimeFields->Year         = (SHORT)st.wYear;
        TimeFields->Month        = (SHORT)st.wMonth;
        TimeFields->Day          = (SHORT)st.wDay;
        TimeFields->Hour         = (SHORT)st.wHour;
        TimeFields->Minute       = (SHORT)st.wMinute;
        TimeFields->Second       = (SHORT)st.wSecond;
        TimeFields->Milliseconds = (SHORT)st.wMilliseconds;
        TimeFields->Weekday      = (SHORT)st.wDayOfWeek;
    } else {
        memset(TimeFields, 0, sizeof(XBOX_TIME_FIELDS));
    }
}

/* ============================================================================
 * Exception Handling
 * ============================================================================ */

VOID __stdcall xbox_RtlUnwind(PVOID TargetFrame, PVOID TargetIp, PVOID ExceptionRecord, PVOID ReturnValue)
{
    /* Delegate to Win32 RtlUnwind */
    RtlUnwind(TargetFrame, TargetIp, (PEXCEPTION_RECORD)ExceptionRecord, ReturnValue);
}

VOID __stdcall xbox_RtlRaiseException(PVOID ExceptionRecord)
{
    RaiseException(
        ((PEXCEPTION_RECORD)ExceptionRecord)->ExceptionCode,
        ((PEXCEPTION_RECORD)ExceptionRecord)->ExceptionFlags,
        ((PEXCEPTION_RECORD)ExceptionRecord)->NumberParameters,
        ((PEXCEPTION_RECORD)ExceptionRecord)->ExceptionInformation);
}

VOID __stdcall xbox_RtlRip(PCHAR ApiName, PCHAR Expression, PCHAR Message)
{
    xbox_log(XBOX_LOG_ERROR, XBOX_LOG_RTL, "RtlRip: %s - %s: %s",
        ApiName ? ApiName : "?",
        Expression ? Expression : "?",
        Message ? Message : "?");

#ifdef _DEBUG
    DebugBreak();
#endif
}

/* ============================================================================
 * String Formatting (Rtl sprintf variants → CRT)
 * ============================================================================ */

int __cdecl xbox_RtlSnprintf(char* buffer, size_t count, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    int result = vsnprintf(buffer, count, format, args);
    va_end(args);
    return result;
}

int __cdecl xbox_RtlSprintf(char* buffer, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    int result = vsprintf(buffer, format, args);
    va_end(args);
    return result;
}

int __cdecl xbox_RtlVsnprintf(char* buffer, size_t count, const char* format, va_list argptr)
{
    return vsnprintf(buffer, count, format, argptr);
}

int __cdecl xbox_RtlVsprintf(char* buffer, const char* format, va_list argptr)
{
    return vsprintf(buffer, format, argptr);
}
