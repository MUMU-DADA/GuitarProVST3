/* GuitarProVST3 local extension; see UPSTREAM.md and LICENSE.txt.
 * Strict batch enable/disable, separate from the upstream entry points.
 * All allocations and symbol lookup precede suspension. NtGetNextThread is a
 * kernel enumeration call, so the frozen section does not use Toolhelp/heap/
 * loader APIs that could need a lock held by a suspended thread.
 */
#define STRICT_MAX_HOOKS 32
#define STRICT_MAX_THREADS 4096
#define STRICT_NO_MORE_ENTRIES ((LONG)0x8000001AL)
typedef LONG (NTAPI *STRICT_NEXT_THREAD)(HANDLE, HANDLE, ACCESS_MASK, ULONG, ULONG, PHANDLE);
typedef struct {
    HANDLE handle;
    DWORD id;
    BOOL suspended;
} STRICT_THREAD;
typedef struct {
    PHOOK_ENTRY hook;
    LPBYTE address;
    SIZE_T size;
} STRICT_PATCH;
typedef struct {
    LPVOID address;
    DWORD protection;
    BOOL writable;
} STRICT_PAGE;

#ifndef MH_STRICT_NEXT_THREAD
#define MH_STRICT_NEXT_THREAD(fn, previous, next) \
    fn(GetCurrentProcess(), previous, THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | \
       THREAD_QUERY_INFORMATION | SYNCHRONIZE, 0, 0, next)
#endif
#ifndef MH_STRICT_SUSPEND
#define MH_STRICT_SUSPEND SuspendThread
#define MH_STRICT_CONTEXT GetThreadContext
#define MH_STRICT_RESUME ResumeThread
#define MH_STRICT_PROTECT VirtualProtect
#define MH_STRICT_FLUSH FlushInstructionCache
#endif

static BOOL StrictExited(HANDLE thread)
{
    return WaitForSingleObject(thread, 0) == WAIT_OBJECT_0;
}

static MH_STATUS StrictFreeze(STRICT_NEXT_THREAD nextThread, STRICT_THREAD *threads,
    UINT *threadCount, const STRICT_PATCH *patches, UINT patchCount)
{
    UINT pass;
    const DWORD self = GetCurrentThreadId();
    // Repeat after suspension to catch threads created during the first scan.
    // Once a complete pass adds no live thread, all in-process creators are
    // parked. Remote injection/debugger interference is outside this contract.
    for (pass = 0; pass < 8; ++pass)
    {
        HANDLE previous = NULL;
        BOOL previousOwned = FALSE;
        BOOL added = FALSE;
        for (;;)
        {
            HANDLE thread = NULL;
            UINT i;
            LONG result = MH_STRICT_NEXT_THREAD(nextThread, previous, &thread);
            if (previous && !previousOwned) CloseHandle(previous);
            previous = NULL;
            if (result == STRICT_NO_MORE_ENTRIES) break;
            if (result < 0 || !thread) return MH_ERROR_THREAD_FREEZE;
            previous = thread;
            previousOwned = FALSE;
            {
                DWORD id = GetThreadId(thread);
                if (!id) { CloseHandle(thread); return MH_ERROR_THREAD_FREEZE; }
                if (id == self) continue;
                for (i = 0; i < *threadCount; ++i)
                    if (threads[i].id == id) break;
                if (i != *threadCount) continue;
                if (*threadCount == STRICT_MAX_THREADS)
                { CloseHandle(thread); return MH_ERROR_THREAD_FREEZE; }
                threads[i].handle = thread;
                threads[i].id = id;
                threads[i].suspended = FALSE;
                ++*threadCount;
                previousOwned = TRUE;
                if (MH_STRICT_SUSPEND(thread) == (DWORD)-1)
                {
                    if (StrictExited(thread)) continue;
                    return MH_ERROR_THREAD_FREEZE;
                }
                threads[i].suspended = TRUE;
                added = TRUE;
            }
            {
                CONTEXT context;
                DWORD_PTR ip;
                context.ContextFlags = CONTEXT_CONTROL;
                if (!MH_STRICT_CONTEXT(thread, &context))
                {
                    if (StrictExited(thread)) continue;
                    return MH_ERROR_THREAD_FREEZE;
                }
#if defined(_M_X64) || defined(__x86_64__)
                ip = (DWORD_PTR)context.Rip;
#else
                ip = (DWORD_PTR)context.Eip;
#endif
                for (i = 0; i < patchCount; ++i)
                    if (ip >= (DWORD_PTR)patches[i].address &&
                        ip < (DWORD_PTR)patches[i].address + patches[i].size)
                        return MH_ERROR_THREAD_BUSY;
            }
        }
        if (!added) return MH_OK;
    }
    return MH_ERROR_THREAD_FREEZE;
}

static BOOL StrictResume(STRICT_THREAD *threads, UINT count)
{
    BOOL resumed = TRUE;
    UINT i;
    for (i = 0; i < count; ++i)
    {
        BOOL done = !threads[i].suspended;
        UINT retry;
        for (retry = 0; !done && retry < 3; ++retry)
            done = MH_STRICT_RESUME(threads[i].handle) != (DWORD)-1 || StrictExited(threads[i].handle);
        if (done) CloseHandle(threads[i].handle);
        else resumed = FALSE; // Retain handle; never pretend the peer resumed.
    }
    return resumed;
}

static MH_STATUS StrictSetHooks(LPVOID const *targets, UINT count, BOOL enable)
{
    MH_STATUS status = MH_OK;
    STRICT_PATCH patches[STRICT_MAX_HOOKS];
    STRICT_PAGE pages[STRICT_MAX_HOOKS];
    STRICT_THREAD *threads = NULL;
    STRICT_NEXT_THREAD nextThread;
    SYSTEM_INFO systemInfo;
    UINT i, pageCount = 0, threadCount = 0;
    BOOL wrote = FALSE;
    EnterSpinLock();
    if (!g_hHeap) { status = MH_ERROR_NOT_INITIALIZED; goto finish; }
    if (!targets || !count || count > STRICT_MAX_HOOKS)
    { status = MH_ERROR_INVALID_PARAMETER; goto finish; }
    GetSystemInfo(&systemInfo);
    for (i = 0; i < count; ++i)
    {
        UINT j, position = FindHookEntry(targets[i]);
        ULONG_PTR page;
        if (position == INVALID_HOOK_POS) { status = MH_ERROR_NOT_CREATED; goto finish; }
        patches[i].hook = &g_hooks.pItems[position];
        if (patches[i].hook->isEnabled == enable)
        { status = enable ? MH_ERROR_ENABLED : MH_ERROR_DISABLED; goto finish; }
        patches[i].address = (LPBYTE)targets[i] - (patches[i].hook->patchAbove ? sizeof(JMP_REL) : 0);
        patches[i].size = sizeof(JMP_REL) + (patches[i].hook->patchAbove ? sizeof(JMP_REL_SHORT) : 0);
        for (j = 0; j < i; ++j)
            if ((ULONG_PTR)patches[i].address < (ULONG_PTR)patches[j].address + patches[j].size &&
                (ULONG_PTR)patches[j].address < (ULONG_PTR)patches[i].address + patches[i].size)
            { status = MH_ERROR_INVALID_PARAMETER; goto finish; }
        page = (ULONG_PTR)patches[i].address & ~((ULONG_PTR)systemInfo.dwPageSize - 1);
        if ((ULONG_PTR)patches[i].address + patches[i].size > page + systemInfo.dwPageSize)
        { status = MH_ERROR_UNSUPPORTED_FUNCTION; goto finish; }
        for (j = 0; j < pageCount; ++j) if ((ULONG_PTR)pages[j].address == page) break;
        if (j == pageCount)
        {
            pages[j].address = (LPVOID)page;
            pages[j].writable = FALSE;
            ++pageCount;
        }
    }
    nextThread = (STRICT_NEXT_THREAD)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtGetNextThread");
    if (!nextThread) { status = MH_ERROR_THREAD_FREEZE; goto finish; }
    threads = (STRICT_THREAD *)VirtualAlloc(NULL, sizeof(STRICT_THREAD) * STRICT_MAX_THREADS,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!threads) { status = MH_ERROR_MEMORY_ALLOC; goto finish; }
    status = StrictFreeze(nextThread, threads, &threadCount, patches, count);
    if (status != MH_OK) goto resume;
    for (i = 0; i < count; ++i)
    {
        BYTE expected[sizeof(JMP_REL) + sizeof(JMP_REL_SHORT)];
        PHOOK_ENTRY hook = patches[i].hook;
        memcpy(expected, hook->backup, patches[i].size);
        if (!enable)
        {
            PJMP_REL jump = (PJMP_REL)expected;
            jump->opcode = 0xE9;
            jump->operand = (UINT32)((LPBYTE)hook->pDetour - (patches[i].address + sizeof(JMP_REL)));
            if (hook->patchAbove)
            {
                PJMP_REL_SHORT shortJump = (PJMP_REL_SHORT)(expected + sizeof(JMP_REL));
                shortJump->opcode = 0xEB;
                shortJump->operand = (UINT8)(0 - (sizeof(JMP_REL_SHORT) + sizeof(JMP_REL)));
            }
        }
        if (memcmp(patches[i].address, expected, patches[i].size) != 0)
        { status = MH_ERROR_UNSUPPORTED_FUNCTION; goto resume; }
    }
    // Preflight every distinct page before changing a byte. Same-page targets
    // share the original protection; no later RWX value replaces that record.
    for (i = 0; i < pageCount; ++i)
    {
        if (!MH_STRICT_PROTECT(pages[i].address, systemInfo.dwPageSize, PAGE_EXECUTE_READWRITE, &pages[i].protection))
        { status = MH_ERROR_MEMORY_PROTECT; goto restore; }
        pages[i].writable = TRUE;
    }
    for (i = 0; i < count; ++i)
    {
        PHOOK_ENTRY hook = patches[i].hook;
        if (enable)
        {
            PJMP_REL jump = (PJMP_REL)patches[i].address;
            jump->opcode = 0xE9;
            jump->operand = (UINT32)((LPBYTE)hook->pDetour - (patches[i].address + sizeof(JMP_REL)));
            if (hook->patchAbove)
            {
                PJMP_REL_SHORT shortJump = (PJMP_REL_SHORT)hook->pTarget;
                shortJump->opcode = 0xEB;
                shortJump->operand = (UINT8)(0 - (sizeof(JMP_REL_SHORT) + sizeof(JMP_REL)));
            }
        }
        else memcpy(patches[i].address, hook->backup, patches[i].size);
        hook->isEnabled = hook->queueEnable = enable;
    }
    wrote = TRUE;
    if (!MH_STRICT_FLUSH(GetCurrentProcess(), NULL, 0)) status = MH_ERROR_CACHE_FLUSH;
restore:
    // On preflight/flush failure the pages changed so far are still writable.
    if (status != MH_OK && wrote)
    {
        for (i = 0; i < count; ++i)
        {
            PHOOK_ENTRY hook = patches[i].hook;
            if (enable) memcpy(patches[i].address, hook->backup, patches[i].size);
            else
            {
                PJMP_REL jump = (PJMP_REL)patches[i].address;
                jump->opcode = 0xE9;
                jump->operand = (UINT32)((LPBYTE)hook->pDetour - (patches[i].address + sizeof(JMP_REL)));
                if (hook->patchAbove)
                {
                    PJMP_REL_SHORT shortJump = (PJMP_REL_SHORT)hook->pTarget;
                    shortJump->opcode = 0xEB;
                    shortJump->operand = (UINT8)(0 - (sizeof(JMP_REL_SHORT) + sizeof(JMP_REL)));
                }
            }
            hook->isEnabled = hook->queueEnable = !enable;
        }
        if (!MH_STRICT_FLUSH(GetCurrentProcess(), NULL, 0)) status = MH_ERROR_PATCH_ROLLBACK;
    }
    for (i = 0; i < pageCount; ++i)
    {
        DWORD discarded;
        UINT retry;
        BOOL restored = !pages[i].writable;
        for (retry = 0; !restored && retry < 3; ++retry)
            restored = MH_STRICT_PROTECT(pages[i].address, systemInfo.dwPageSize, pages[i].protection, &discarded);
        if (!restored) status = MH_ERROR_PATCH_ROLLBACK;
    }
resume:
    if (!StrictResume(threads, threadCount)) status = MH_ERROR_THREAD_RESUME;
finish:
    if (threads) VirtualFree(threads, 0, MEM_RELEASE);
    LeaveSpinLock();
    return status;
}

MH_STATUS WINAPI MH_EnableHooksStrict(LPVOID const *targets, UINT count)
{ return StrictSetHooks(targets, count, TRUE); }

MH_STATUS WINAPI MH_DisableHooksStrict(LPVOID const *targets, UINT count)
{ return StrictSetHooks(targets, count, FALSE); }

MH_STATUS WINAPI MH_IsHookEnabled(LPVOID target, BOOL *enabled)
{
    MH_STATUS status = MH_OK;
    UINT position;
    if (!enabled) return MH_ERROR_INVALID_PARAMETER;
    *enabled = FALSE;
    EnterSpinLock();
    if (!g_hHeap) status = MH_ERROR_NOT_INITIALIZED;
    else if (!target) status = MH_ERROR_INVALID_PARAMETER;
    else
    {
        position = FindHookEntry(target);
        if (position == INVALID_HOOK_POS) status = MH_ERROR_NOT_CREATED;
        else *enabled = g_hooks.pItems[position].isEnabled;
    }
    LeaveSpinLock();
    return status;
}
