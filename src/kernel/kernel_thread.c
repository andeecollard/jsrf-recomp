/*
 * kernel_thread.c - Xbox Threading Subsystem
 *
 * Implements Xbox thread creation, termination, delays, and priority
 * management using Win32 threading APIs.
 *
 * Xbox threading model:
 *   - PsCreateSystemThreadEx creates kernel-mode threads (→ CreateThread)
 *   - Thread start routines are __stdcall with a single PVOID context
 *   - Time intervals use NT 100-nanosecond units (negative = relative)
 *   - Thread priorities use NT KPRIORITY increments
 */

#include "kernel.h"
#include "xbox_memory_layout.h"   /* XBOX_WORKER_STACK_* + worker-stack decls */

/* ============================================================================
 * Thread Start Wrapper
 *
 * Xbox start routines are __stdcall void(*)(PVOID), but Win32 CreateThread
 * expects DWORD WINAPI (*)(LPVOID). We wrap the Xbox routine to bridge
 * the calling convention and return type.
 * ============================================================================ */

typedef struct _XBOX_THREAD_START_INFO {
    PXBOX_SYSTEM_ROUTINE StartRoutine;
    PVOID                StartContext;
} XBOX_THREAD_START_INFO;

static DWORD WINAPI xbox_thread_wrapper(LPVOID lpParameter)
{
    XBOX_THREAD_START_INFO info = *(XBOX_THREAD_START_INFO*)lpParameter;

    /* Free the start info before calling the routine - the routine may
     * never return (calling PsTerminateSystemThread instead) */
    HeapFree(GetProcessHeap(), 0, lpParameter);

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_THREAD, "Thread %u starting at %p",
        GetCurrentThreadId(), info.StartRoutine);

    info.StartRoutine(info.StartContext);

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_THREAD, "Thread %u returned normally",
        GetCurrentThreadId());

    return 0;
}

/* ============================================================================
 * PsCreateSystemThreadEx
 *
 * Xbox signature:
 *   PsCreateSystemThreadEx(
 *     OUT PHANDLE ThreadHandle,
 *     IN ULONG ThreadExtraSize,      // extra bytes in thread object (ignored)
 *     IN ULONG KernelStackSize,      // stack size (0 = default)
 *     IN ULONG TlsDataSize,          // TLS data size (Xbox-specific, ignored)
 *     OUT PULONG ThreadId,           // optional thread ID
 *     IN PVOID StartContext1,        // context passed to StartRoutine
 *     IN PVOID StartContext2,        // alternate context (unused by game code)
 *     IN BOOLEAN CreateSuspended,
 *     IN BOOLEAN DebugStack,         // debug stack (ignored)
 *     IN PXBOX_SYSTEM_ROUTINE StartRoutine
 *   )
 *
 * Maps to: CreateThread with a wrapper for calling convention adaptation.
 * ============================================================================ */

NTSTATUS __stdcall xbox_PsCreateSystemThreadEx(
    PHANDLE ThreadHandle,
    ULONG ThreadExtraSize,
    ULONG KernelStackSize,
    ULONG TlsDataSize,
    PULONG ThreadId,
    PVOID StartContext1,
    PVOID StartContext2,
    BOOLEAN CreateSuspended,
    BOOLEAN DebugStack,
    PXBOX_SYSTEM_ROUTINE StartRoutine)
{
    XBOX_THREAD_START_INFO* info;
    HANDLE hThread;
    DWORD dwThreadId;
    DWORD dwCreationFlags;

    (void)ThreadExtraSize;
    (void)TlsDataSize;
    (void)StartContext2;
    (void)DebugStack;

    if (!ThreadHandle || !StartRoutine)
        return STATUS_INVALID_PARAMETER;

    /* Allocate start info - freed by the wrapper thread */
    info = (XBOX_THREAD_START_INFO*)HeapAlloc(GetProcessHeap(), 0, sizeof(XBOX_THREAD_START_INFO));
    if (!info)
        return STATUS_NO_MEMORY;

    info->StartRoutine = StartRoutine;
    info->StartContext = StartContext1;

    dwCreationFlags = CreateSuspended ? CREATE_SUSPENDED : 0;

    /* Use default stack size if 0 (Xbox default is 64KB) */
    if (KernelStackSize == 0)
        KernelStackSize = 65536;

    hThread = CreateThread(NULL, KernelStackSize, xbox_thread_wrapper, info,
                           dwCreationFlags, &dwThreadId);
    if (!hThread) {
        HeapFree(GetProcessHeap(), 0, info);
        xbox_log(XBOX_LOG_ERROR, XBOX_LOG_THREAD,
            "PsCreateSystemThreadEx: CreateThread failed (error %u)", GetLastError());
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    *ThreadHandle = hThread;
    if (ThreadId)
        *ThreadId = dwThreadId;

    xbox_log(XBOX_LOG_INFO, XBOX_LOG_THREAD,
        "PsCreateSystemThreadEx: created thread %u (handle=%p, routine=%p, suspended=%d)",
        dwThreadId, hThread, StartRoutine, CreateSuspended);

    return STATUS_SUCCESS;
}

/* ============================================================================
 * PsTerminateSystemThread
 *
 * Terminates the calling thread. On Xbox this is the standard way for
 * system threads to exit. Maps directly to ExitThread.
 * ============================================================================ */

NTSTATUS __stdcall xbox_PsTerminateSystemThread(NTSTATUS ExitStatus)
{
    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_THREAD,
        "PsTerminateSystemThread: thread %u exiting with status 0x%08X",
        GetCurrentThreadId(), ExitStatus);

    ExitThread((DWORD)ExitStatus);

    /* ExitThread never returns, but the compiler needs this */
    return STATUS_SUCCESS;
}

/* ============================================================================
 * KeDelayExecutionThread
 *
 * Delays the current thread. The interval uses NT 100-nanosecond units:
 *   - Negative values = relative delay (most common)
 *   - Positive values = absolute time (rare)
 *   - Zero = yield
 *
 * Maps to: SleepEx (for alertable waits) or Sleep
 * ============================================================================ */

NTSTATUS __stdcall xbox_KeDelayExecutionThread(
    KPROCESSOR_MODE WaitMode,
    BOOLEAN Alertable,
    PLARGE_INTEGER Interval)
{
    DWORD ms;

    (void)WaitMode;

    if (!Interval)
        return STATUS_INVALID_PARAMETER;

    if (Interval->QuadPart == 0) {
        /* Zero interval = yield the thread's time slice */
        SwitchToThread();
        return STATUS_SUCCESS;
    }

    if (Interval->QuadPart < 0) {
        /* Negative = relative time in 100ns units. Convert to milliseconds. */
        LONGLONG relative_100ns = -Interval->QuadPart;
        ms = (DWORD)(relative_100ns / 10000);
        /* Ensure at least 1ms for very short intervals */
        if (ms == 0 && relative_100ns > 0)
            ms = 1;
    } else {
        /* Positive = absolute time. Calculate relative delay from now. */
        LARGE_INTEGER now;
        GetSystemTimeAsFileTime((LPFILETIME)&now);
        LONGLONG diff = Interval->QuadPart - now.QuadPart;
        if (diff <= 0)
            return STATUS_SUCCESS; /* Already past */
        ms = (DWORD)(diff / 10000);
    }

    if (Alertable) {
        DWORD result = SleepEx(ms, TRUE);
        if (result == WAIT_IO_COMPLETION)
            return STATUS_ALERTED;
    } else {
        Sleep(ms);
    }

    return STATUS_SUCCESS;
}

/* ============================================================================
 * Thread Priority
 *
 * Xbox uses NT KPRIORITY base priority increments relative to the process.
 * We map these to Win32 thread priority levels.
 * ============================================================================ */

/*
 * Map Xbox priority increment to Win32 priority level.
 * Xbox base priorities typically range from -2 to +2 for game threads.
 */
static int xbox_priority_to_win32(LONG increment)
{
    if (increment <= -15)       return THREAD_PRIORITY_IDLE;
    else if (increment <= -2)   return THREAD_PRIORITY_LOWEST;
    else if (increment == -1)   return THREAD_PRIORITY_BELOW_NORMAL;
    else if (increment == 0)    return THREAD_PRIORITY_NORMAL;
    else if (increment == 1)    return THREAD_PRIORITY_ABOVE_NORMAL;
    else if (increment <= 2)    return THREAD_PRIORITY_HIGHEST;
    else                        return THREAD_PRIORITY_TIME_CRITICAL;
}

static LONG win32_priority_to_xbox(int priority)
{
    switch (priority) {
        case THREAD_PRIORITY_IDLE:          return -15;
        case THREAD_PRIORITY_LOWEST:        return -2;
        case THREAD_PRIORITY_BELOW_NORMAL:  return -1;
        case THREAD_PRIORITY_NORMAL:        return 0;
        case THREAD_PRIORITY_ABOVE_NORMAL:  return 1;
        case THREAD_PRIORITY_HIGHEST:       return 2;
        case THREAD_PRIORITY_TIME_CRITICAL: return 15;
        default:                            return 0;
    }
}

/* THE EXACT-PRIORITY STORE ON WINDOWS.
 *
 * SetThreadPriorityXboxExact / GetThreadPriorityXboxExact live in
 * src/platform/win32_compat.c, whose entire body -- declarations included --
 * is inside `#if !defined(_WIN32)`, and which the Windows build does not
 * compile at all: src/platform/CMakeLists.txt makes `platform` an INTERFACE
 * library there, because Windows has the real <windows.h>.
 *
 * They are not Win32 API, though. They are OURS, added by 4537424 ("The guest
 * sets base priority 16 and could only ever read back 15"), and the calls
 * below are unconditional. So on Windows they were implicitly declared and
 * then had nothing to link against. mingw-w64 reports the first half as an
 * error; the second half is an unresolved external.
 *
 * The POSIX pair keys the exact value off this layer's own w32_object, which
 * dies with the thread. Windows hands us a real OS handle instead, so the
 * store is keyed by thread id.
 *
 * SAME CONTRACT AS THE POSIX PAIR, deliberately: a handle that cannot be named
 * is COUNTED AND DROPPED rather than charged to the caller. Answering an
 * unidentified handle with the caller's priority is the confident wrong answer
 * that win32_compat.c's own comment records paying for.
 *
 * KNOWN LIMITATION, stated rather than discovered later: nothing removes an
 * entry when a thread exits, so a thread id the OS reuses inherits the old
 * thread's exact priority. The POSIX side cannot have this because the store
 * dies with the object. JSRF creates a handful of threads and never churns
 * them, and a stale entry is a far smaller defect than a platform that does
 * not link -- but it is a defect, and a title that cycles threads needs the
 * table keyed on something that retires.
 *
 * UNTESTED AT RUNTIME. This was written on macOS and syntax-checked with
 * x86_64-w64-mingw32-gcc; no Windows session has run it. */
#if defined(_WIN32)

#define XBOX_EXACT_SLOTS 128
static SRWLOCK s_exact_lock = SRWLOCK_INIT;
static struct { DWORD tid; int prio; } s_exact[XBOX_EXACT_SLOTS];
static unsigned s_exact_n;

/* Counted, not silent -- the same reading win32_compat.c's
 * g_w32_priority_unnamed carries: nonzero means a guest priority read or write
 * went nowhere, which is a defect upstream and not normal. */
unsigned long g_w32_priority_unnamed_win32;
unsigned long g_w32_exact_table_full;

static DWORD xbox_exact_tid(HANDLE h)
{
    /* The pseudo-handle is resolved explicitly rather than relying on
     * GetThreadId accepting it. */
    if (h == GetCurrentThread()) return GetCurrentThreadId();
    if (!h) return 0;
    return GetThreadId(h);              /* 0 when the handle cannot be named */
}

void SetThreadPriorityXboxExact(HANDLE h, int xbox_priority)
{
    DWORD tid = xbox_exact_tid(h);
    unsigned i;

    if (!tid) { ++g_w32_priority_unnamed_win32; return; }

    AcquireSRWLockExclusive(&s_exact_lock);
    for (i = 0; i < s_exact_n; ++i)
        if (s_exact[i].tid == tid) break;
    if (i < s_exact_n) {
        s_exact[i].prio = xbox_priority;
    } else if (s_exact_n < XBOX_EXACT_SLOTS) {
        s_exact[s_exact_n].tid  = tid;
        s_exact[s_exact_n].prio = xbox_priority;
        ++s_exact_n;
    } else {
        ++g_w32_exact_table_full;       /* the value is lost; say so */
    }
    ReleaseSRWLockExclusive(&s_exact_lock);
}

int GetThreadPriorityXboxExact(HANDLE h, int *have)
{
    DWORD tid = xbox_exact_tid(h);
    unsigned i;
    int value = 0, found = 0;

    if (!tid) {
        ++g_w32_priority_unnamed_win32;
        if (have) *have = 0;
        return 0;
    }
    AcquireSRWLockShared(&s_exact_lock);
    for (i = 0; i < s_exact_n; ++i)
        if (s_exact[i].tid == tid) { value = s_exact[i].prio; found = 1; break; }
    ReleaseSRWLockShared(&s_exact_lock);

    if (have) *have = found;
    return value;
}

#endif /* _WIN32 */

/* THE ROUND TRIP MUST BE EXACT, and for a long time it was not.
 *
 * xbox_priority_to_win32 collapses the Xbox base priority into one of seven
 * Win32 buckets, and win32_priority_to_xbox expands a bucket back to one
 * representative value. Everything outside {-15,-2,-1,0,1,2,15} does not
 * survive: set 16, read 15.
 *
 * JSRF sets 16. Measured 21 Sep 2026 from the scheduler trace --
 * `KeSetBasePriority ... ra=0x00147CF8 extra=0x00000010` -- with ra inside
 * XAPI's SetThreadPriority. Something then polls until the query agrees, and
 * it never can: the per-thread ordinal histogram shows 64,522,063 /
 * 64,522,064 / 64,517,185 calls to ObReferenceObjectByHandle,
 * ObfDereferenceObject and KeQueryBasePriorityThread on ONE thread during a
 * cutscene hang -- a three-call cycle run 64.5 million times.
 *
 * The bucket is still what the host scheduler is told, because a bucket is
 * all it can use. The exact value is stored beside it and is what the guest
 * reads back. */
LONG __stdcall xbox_KeSetBasePriorityThread(PVOID Thread, LONG Increment)
{
    HANDLE hThread = (HANDLE)Thread;
    LONG previous;
    int have_exact = 0;
    LONG prev_exact = (LONG)GetThreadPriorityXboxExact(hThread, &have_exact);

    /* Get previous priority before setting new one */
    int prev_win32 = GetThreadPriority(hThread);
    previous = have_exact ? prev_exact : win32_priority_to_xbox(prev_win32);

    int new_win32 = xbox_priority_to_win32(Increment);
    SetThreadPriorityXboxExact(hThread, (int)Increment);
    SetThreadPriority(hThread, new_win32);

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_THREAD,
        "KeSetBasePriorityThread: thread=%p, increment=%d (win32=%d), prev=%d",
        Thread, Increment, new_win32, previous);

    return previous;
}

LONG __stdcall xbox_KeQueryBasePriorityThread(PVOID Thread)
{
    HANDLE hThread = (HANDLE)Thread;
    int have_exact = 0;
    int exact = GetThreadPriorityXboxExact(hThread, &have_exact);
    /* The exact value when this thread's priority was ever set through the
     * Xbox path; the bucket only for a thread that never was, where there is
     * nothing exact to remember. */
    if (have_exact) return (LONG)exact;
    return win32_priority_to_xbox(GetThreadPriority(hThread));
}

/* ============================================================================
 * KeSetDisableBoostThread (ordinal 144)
 *
 * BOOLEAN KeSetDisableBoostThread(PKTHREAD Thread, BOOLEAN Disable)
 *
 * Turns off the scheduler's priority boost for one thread and returns whether
 * it was already off. Titles use it on threads that must not drift up in
 * priority when they come out of a wait -- audio mixers and streaming threads,
 * typically, where a boost would starve the very thread that feeds them.
 *
 * Disable has the same polarity as Win32's bDisablePriorityBoost, so this is a
 * direct forward. What it is NOT is a scheduling change on this host: see
 * SetThreadPriorityBoost in win32_compat.c -- POSIX has no wakeup boost to
 * disable, so the flag is tracked and returned, not applied.
 *
 * The return value is the *previous* Disable state, which is the whole reason
 * to track it: the idiom is save-set-restore, and a version that always
 * returned FALSE would have every caller restore the wrong state.
 * ============================================================================ */
BOOLEAN __stdcall xbox_KeSetDisableBoostThread(PVOID Thread, BOOLEAN Disable)
{
    HANDLE hThread = (HANDLE)Thread;
    BOOL previous = FALSE;

    /* A thread we do not know is not an error the caller can act on -- it gets
     * the documented default, FALSE, the same answer a never-set thread gives. */
    if (!GetThreadPriorityBoost(hThread, &previous))
        previous = FALSE;

    SetThreadPriorityBoost(hThread, Disable ? TRUE : FALSE);

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_THREAD,
        "KeSetDisableBoostThread: thread=%p, disable=%u, prev=%u (tracked, "
        "not applied -- no POSIX wakeup boost exists)",
        Thread, (unsigned)Disable, (unsigned)previous);

    return previous ? TRUE : FALSE;
}

/* ============================================================================
 * KeAlertThread
 *
 * Sends an alert to a thread, which can wake it from an alertable wait.
 * On Xbox, this sets the alerted flag on the thread object.
 * We approximate this with QueueUserAPC using a no-op APC routine.
 * ============================================================================ */

static VOID CALLBACK xbox_alert_apc(ULONG_PTR dwParam)
{
    (void)dwParam;
    /* No-op - the purpose is just to wake the thread from alertable wait */
}

NTSTATUS __stdcall xbox_KeAlertThread(PVOID Thread, KPROCESSOR_MODE AlertMode)
{
    HANDLE hThread = (HANDLE)Thread;

    (void)AlertMode;

    if (!QueueUserAPC(xbox_alert_apc, hThread, 0)) {
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_THREAD,
            "KeAlertThread: QueueUserAPC failed (error %u)", GetLastError());
        return STATUS_UNSUCCESSFUL;
    }

    return STATUS_SUCCESS;
}

/* ============================================================================
 * NtYieldExecution
 *
 * Yields the current thread's remaining time slice.
 * Maps directly to SwitchToThread.
 * ============================================================================ */

NTSTATUS __stdcall xbox_NtYieldExecution(void)
{
    SwitchToThread();
    return STATUS_SUCCESS;
}

/* ============================================================================
 * NtDuplicateObject
 *
 * Duplicates a kernel handle. On Xbox this is simpler than Win32 since
 * there's only one process. Maps to DuplicateHandle within the same process.
 * ============================================================================ */

NTSTATUS __stdcall xbox_NtDuplicateObject(
    HANDLE SourceHandle,
    PHANDLE TargetHandle,
    ULONG Options)
{
    HANDLE hProcess = GetCurrentProcess();
    DWORD dwOptions = 0;

    if (!TargetHandle)
        return STATUS_INVALID_PARAMETER;

    /* Xbox DUPLICATE_CLOSE_SOURCE = 0x1, same as Win32 */
    if (Options & 0x1)
        dwOptions |= DUPLICATE_CLOSE_SOURCE;
    /* Xbox DUPLICATE_SAME_ACCESS = 0x2, same as Win32 */
    if (Options & 0x2)
        dwOptions |= DUPLICATE_SAME_ACCESS;

    if (!DuplicateHandle(hProcess, SourceHandle, hProcess, TargetHandle,
                         0, FALSE, dwOptions)) {
        xbox_log(XBOX_LOG_ERROR, XBOX_LOG_THREAD,
            "NtDuplicateObject: DuplicateHandle failed (error %u)", GetLastError());
        return STATUS_UNSUCCESSFUL;
    }

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_THREAD,
        "NtDuplicateObject: source=%p → target=%p (options=0x%X)",
        SourceHandle, *TargetHandle, Options);

    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_NtSuspendThread(
    HANDLE ThreadHandle,
    PULONG PreviousSuspendCount)
{
    DWORD prev;

    /*
     * SuspendThread returns the previous suspend count, or (DWORD)-1 on
     * failure. The Xbox call reports that count through an out-parameter, so
     * the two are not interchangeable: -1 must become an error status rather
     * than a suspend count of 0xFFFFFFFF.
     */
    prev = SuspendThread(ThreadHandle);
    if (prev == (DWORD)-1) {
        xbox_log(XBOX_LOG_ERROR, XBOX_LOG_THREAD,
            "NtSuspendThread: SuspendThread failed (error %u)", GetLastError());
        return STATUS_UNSUCCESSFUL;
    }

    if (PreviousSuspendCount)
        *PreviousSuspendCount = (ULONG)prev;

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_THREAD,
        "NtSuspendThread: thread=%p previous_count=%u", ThreadHandle, prev);

    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_NtResumeThread(
    HANDLE ThreadHandle,
    PULONG PreviousSuspendCount)
{
    DWORD prev;

    /* Mirror of NtSuspendThread: (DWORD)-1 is failure, not a count. */
    prev = ResumeThread(ThreadHandle);
    if (prev == (DWORD)-1) {
        xbox_log(XBOX_LOG_ERROR, XBOX_LOG_THREAD,
            "NtResumeThread: ResumeThread failed (error %u)", GetLastError());
        return STATUS_UNSUCCESSFUL;
    }

    if (PreviousSuspendCount)
        *PreviousSuspendCount = (ULONG)prev;

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_THREAD,
        "NtResumeThread: thread=%p previous_count=%u", ThreadHandle, prev);

    return STATUS_SUCCESS;
}


/* ================================================================
 * Worker stack slices + game-thread tracking (host-tick-driven titles)
 * ================================================================
 *
 * Ported from the Burnout 3 fork as the runtimes reunite. A host-driven title
 * returns from its entry after spawning an init thread and expects the host's
 * own thread to drive the per-frame tick; to call recompiled code from there it
 * needs a guest stack, which a worker slice provides. Additive: a default-model
 * title never calls any of this, so it is inert for Halo, Crimson Skies, etc.
 * See docs/technical/burnout3-reunification.md and XBOX_WORKER_STACK_* in
 * xbox_memory_layout.h.
 */

/* The game's own thread, kept so a wedged boot can be inspected from the host
 * watchdog. Set when a title's game thread is spawned; NULL under the default
 * inline model, where there is no separate thread to sample. */
static HANDLE g_game_thread = NULL;

void  xbox_set_game_thread(void *h) { g_game_thread = (HANDLE)h; }
void *xbox_thread_debug_handle(void) { return (void *)g_game_thread; }

/* One bit per worker stack slice. Interlocked because a host-driven title can
 * allocate a slice from the host thread while recompiled code allocates one for
 * a spawned worker, so the allocator itself must be thread-safe. */
/* Zero slices is the default -- see XBOX_WORKER_STACK_COUNT. C has no
 * zero-length array, so keep one element and let the count gate every use. */
static volatile LONG g_worker_stack_used[XBOX_WORKER_STACK_COUNT > 0
                                         ? XBOX_WORKER_STACK_COUNT : 1];

int xbox_worker_stack_alloc(void)
{
    int i;
    for (i = 0; i < XBOX_WORKER_STACK_COUNT; i++) {
        if (InterlockedCompareExchange(&g_worker_stack_used[i], 1, 0) == 0)
            return i;
    }
    return -1;  /* all slices in use */
}

void xbox_worker_stack_free(int slot)
{
    if (slot >= 0 && slot < XBOX_WORKER_STACK_COUNT)
        InterlockedExchange(&g_worker_stack_used[slot], 0);
}
