#include <windows.h>
#include <stdio.h>
#include <stdint.h>

static LONG NTAPI nextTest(HANDLE, HANDLE, ACCESS_MASK, ULONG, ULONG, PHANDLE);
static DWORD suspendTest(HANDLE);
static BOOL contextTest(HANDLE, LPCONTEXT);
static DWORD resumeTest(HANDLE);
static BOOL protectTest(LPVOID, SIZE_T, DWORD, PDWORD);
static BOOL flushTest(HANDLE, LPCVOID, SIZE_T);
#define MH_STRICT_NEXT_THREAD(fn, previous, next) nextTest(GetCurrentProcess(), previous, 0, 0, 0, next)
#define MH_STRICT_SUSPEND suspendTest
#define MH_STRICT_CONTEXT contextTest
#define MH_STRICT_RESUME resumeTest
#define MH_STRICT_PROTECT protectTest
#define MH_STRICT_FLUSH flushTest
#include "../third_party/minhook/src/hook.c"

enum { NONE, ENUM_FAIL, SUSPEND_FAIL, CONTEXT_FAIL, IP_BUSY, PROTECT_FAIL,
       FLUSH_FAIL, RESTORE_FAIL, RESUME_RETRY, NEW_THREAD, RESUME_FAIL };
static int fault, protectCalls, flushCalls, resumeCalls, enumPass, realEnumeration;
static HANDLE waitingThread, waitingThread2, wakeEvent;
static void *testTarget;
static LONG NTAPI nextTest(HANDLE process, HANDLE previous, ACCESS_MASK access,
                          ULONG attributes, ULONG flags, PHANDLE next) {
    STRICT_NEXT_THREAD realNext = (STRICT_NEXT_THREAD)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtGetNextThread");
    (void)access; (void)attributes; (void)flags;
    if (realEnumeration) return realNext(process, previous, THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
        THREAD_QUERY_INFORMATION | SYNCHRONIZE, 0, 0, next);
    if (!previous) ++enumPass;
    if (fault == ENUM_FAIL) return (LONG)0xC0000022L;
    if (!previous) {
        if (!DuplicateHandle(GetCurrentProcess(), waitingThread, GetCurrentProcess(), next, 0, FALSE, DUPLICATE_SAME_ACCESS))
            return (LONG)0xC0000022L;
        return 0;
    }
    if (fault == NEW_THREAD && enumPass >= 2 && GetThreadId(previous) == GetThreadId(waitingThread)) {
        if (!DuplicateHandle(GetCurrentProcess(), waitingThread2, GetCurrentProcess(), next, 0, FALSE, DUPLICATE_SAME_ACCESS))
            return (LONG)0xC0000022L;
        return 0;
    }
    return STRICT_NO_MORE_ENTRIES;
}
static DWORD suspendTest(HANDLE thread) {
    if (fault == SUSPEND_FAIL) return (DWORD)-1;
    return SuspendThread(thread);
}
static BOOL contextTest(HANDLE thread, LPCONTEXT context) {
    if (fault == CONTEXT_FAIL) return FALSE;
    if (!GetThreadContext(thread, context)) return FALSE;
    if (fault == IP_BUSY) context->Rip = (DWORD64)testTarget + 2;
    return TRUE;
}
static DWORD resumeTest(HANDLE thread) {
    ++resumeCalls;
    if (fault == RESUME_FAIL || (fault == RESUME_RETRY && resumeCalls < 3)) return (DWORD)-1;
    return ResumeThread(thread);
}
static BOOL protectTest(LPVOID address, SIZE_T size, DWORD protection, PDWORD oldProtection) {
    ++protectCalls;
    if (fault == PROTECT_FAIL && protectCalls == 2) return FALSE;
    if (fault == RESTORE_FAIL && protectCalls > 2) return FALSE;
    return VirtualProtect(address, size, protection, oldProtection);
}
static BOOL flushTest(HANDLE process, LPCVOID address, SIZE_T size) {
    if (fault == FLUSH_FAIL && ++flushCalls == 1) return FALSE;
    return FlushInstructionCache(process, address, size);
}
static DWORD WINAPI waitWorker(void *ignored) {
    (void)ignored;
    WaitForSingleObject(wakeEvent, INFINITE);
    return 0;
}
static int detour(void) { return 73; }
typedef int (*Function)(void);
static int expect(int okay, const char *message) {
    if (!okay) fprintf(stderr, "FAIL: %s\n", message);
    return okay;
}
static int runFault(int scenario, MH_STATUS expected, int disable, int patchAbove) {
    BYTE *memory = (BYTE *)VirtualAlloc(NULL, 8192, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    const BYTE body[] = {0xB8, 42, 0, 0, 0, 0xC3};
    const BYTE shortBody[] = {0x6A, 42, 0x58, 0xC3, 0x45};
    void *targets[2];
    void *original[2];
    BYTE before[2][16], plain[2][16];
    LPBYTE addresses[2];
    DWORD oldProtect;
    MH_STATUS result;
    int i, changed, enabled, okay;
    if (!memory) return 0;
    memset(memory, 0x90, 8192);
    targets[0] = memory + 16; targets[1] = memory + 4112;
    for (i = 0; i < 2; ++i) {
        memcpy(targets[i], patchAbove ? shortBody : body, patchAbove ? sizeof(shortBody) : sizeof(body));
        addresses[i] = (LPBYTE)targets[i] - 5;
        memcpy(plain[i], addresses[i], sizeof(plain[i]));
    }
    if (!VirtualProtect(memory, 8192, PAGE_EXECUTE_READ, &oldProtect)) return 0;
    FlushInstructionCache(GetCurrentProcess(), memory, 8192);
    if (MH_CreateHook(targets[0], detour, &original[0]) != MH_OK ||
        MH_CreateHook(targets[1], detour, &original[1]) != MH_OK) return 0;
    for (i = 0; i < 2; ++i)
        if (!expect(g_hooks.pItems[FindHookEntry(targets[i])].patchAbove == (UINT)patchAbove,
            "fixture selects the requested normal/hotpatch layout")) return 0;
    fault = NONE; protectCalls = flushCalls = resumeCalls = enumPass = 0;
    if (disable && MH_EnableHooksStrict(targets, 2) != MH_OK) return 0;
    for (i = 0; i < 2; ++i) memcpy(before[i], addresses[i], sizeof(before[i]));
    // Include the jump above the target in the busy-IP case.
    testTarget = patchAbove ? (LPBYTE)targets[0] - 5 : targets[0];
    fault = scenario; protectCalls = flushCalls = resumeCalls = enumPass = 0;
    result = disable ? MH_DisableHooksStrict(targets, 2) : MH_EnableHooksStrict(targets, 2);
    if (result != expected)
        fprintf(stderr, "scenario=%d disable=%d patchAbove=%d status=%s expected=%s\n",
            scenario, disable, patchAbove, MH_StatusToString(result), MH_StatusToString(expected));
    okay = expect(result == expected, "strict API reports the precise injected failure");
    changed = scenario == NONE || scenario == RESTORE_FAIL || scenario == RESUME_RETRY ||
        scenario == NEW_THREAD || scenario == RESUME_FAIL;
    enabled = changed ? !disable : disable;
    for (i = 0; i < 2; ++i) {
        PHOOK_ENTRY hook = &g_hooks.pItems[FindHookEntry(targets[i])];
        MEMORY_BASIC_INFORMATION page;
        BOOL queriedEnabled = !enabled;
        const int protectionsBeforeQuery = protectCalls, resumesBeforeQuery = resumeCalls,
            flushesBeforeQuery = flushCalls, passesBeforeQuery = enumPass;
        okay &= expect(MH_IsHookEnabled(targets[i], &queriedEnabled) == MH_OK && queriedEnabled == enabled,
            "public state query reports actual state after every success and injected failure");
        okay &= expect(protectCalls == protectionsBeforeQuery && resumeCalls == resumesBeforeQuery &&
            flushCalls == flushesBeforeQuery && enumPass == passesBeforeQuery,
            "state query does not modify instructions or control any peer thread");
        okay &= expect(((Function)targets[i])() == (enabled ? 73 : 42) &&
            ((Function)original[i])() == 42, "whole batch and retained trampolines remain executable");
        okay &= expect(hook->isEnabled == (UINT)enabled && hook->queueEnable == (UINT)enabled,
            "hook metadata matches the actual bytes after success or failure");
        if (!changed)
            okay &= expect(memcmp(addresses[i], before[i], sizeof(before[i])) == 0,
                "preflight or flush failure restores all prior bytes including hotpatch jumps");
        if (!enabled)
            okay &= expect(memcmp(addresses[i], plain[i], sizeof(plain[i])) == 0,
                "disabled batch restores the complete original region");
        if (scenario != RESTORE_FAIL) {
            okay &= expect(VirtualQuery(targets[i], &page, sizeof(page)) == sizeof(page) &&
                page.Protect == PAGE_EXECUTE_READ, "page protection is restored after success or rollback");
        }
    }
    if (scenario == ENUM_FAIL || scenario == SUSPEND_FAIL || scenario == CONTEXT_FAIL || scenario == IP_BUSY)
        okay &= expect(protectCalls == 0, "freeze failures cannot reach a patch page");
    if (scenario == RESUME_RETRY) okay &= expect(resumeCalls == 3, "resume retries preserve the original thread handle");
    if (scenario == NEW_THREAD) okay &= expect(enumPass == 3 && resumeCalls == 2, "second-pass newcomer is frozen and rescanned");
    if (scenario == RESUME_FAIL) {
        okay &= expect(resumeCalls == 3, "persistent resume failure is reported");
        ResumeThread(waitingThread); // Explicit test recovery, never claim the API recovered.
    }
    fault = NONE;
    // A failed transaction remains retryable in either direction.
    if (!changed) {
        protectCalls = flushCalls = resumeCalls = enumPass = 0;
        okay &= expect((disable ? MH_DisableHooksStrict(targets, 2) : MH_EnableHooksStrict(targets, 2)) == MH_OK,
            "transaction can be retried after a preflight or flush failure");
    }
    MH_RemoveHook(targets[0]); MH_RemoveHook(targets[1]);
    VirtualFree(memory, 0, MEM_RELEASE);
    return okay;
}

static volatile LONG stopWorkers, badResult;
static Function concurrentFunction, concurrentOriginal;
static int concurrentDetour(void) {
    return concurrentOriginal() + 31;
}
static DWORD WINAPI callWorker(void *ignored) {
    (void)ignored;
    while (!InterlockedCompareExchange(&stopWorkers, 0, 0)) {
        int value = concurrentFunction();
        if (value != 42 && value != 73) InterlockedExchange(&badResult, 1);
    }
    return 0;
}
static int concurrentInstall(void) {
    BYTE *memory = (BYTE *)VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    const BYTE body[] = {0xB8, 42, 0, 0, 0, 0xC3};
    HANDLE workers[4];
    DWORD previous;
    void *original, *target = memory;
    MH_STATUS result = MH_ERROR_THREAD_BUSY;
    int i, okay;
    if (!memory) return 0;
    memcpy(memory, body, sizeof(body));
    if (!VirtualProtect(memory, 4096, PAGE_EXECUTE_READ, &previous)) return 0;
    FlushInstructionCache(GetCurrentProcess(), memory, 4096);
    if (MH_CreateHook(memory, concurrentDetour, &original) != MH_OK) return 0;
    concurrentOriginal = (Function)original;
    concurrentFunction = (Function)memory;
    for (i = 0; i < 4; ++i) workers[i] = CreateThread(NULL, 0, callWorker, NULL, 0, NULL);
    realEnumeration = 1;
    for (i = 0; i < 100 && result == MH_ERROR_THREAD_BUSY; ++i)
        result = MH_EnableHooksStrict(&target, 1);
    okay = expect(result == MH_OK && concurrentFunction() == 73 && ((Function)original)() == 42,
        "concurrent installation publishes complete detour and valid trampoline");
    result = MH_ERROR_THREAD_BUSY;
    for (i = 0; i < 100 && result == MH_ERROR_THREAD_BUSY; ++i)
        result = MH_DisableHooksStrict(&target, 1);
    InterlockedExchange(&stopWorkers, 1);
    WaitForMultipleObjects(4, workers, TRUE, INFINITE);
    for (i = 0; i < 4; ++i) CloseHandle(workers[i]);
    okay &= expect(result == MH_OK && !badResult && concurrentFunction() == 42 && ((Function)original)() == 42,
        "strict retirement preserves trampolines while four threads execute the target and forwarding detour");
    MH_RemoveHook(memory);
    VirtualFree(memory, 0, MEM_RELEASE);
    return okay;
}
int main(void) {
    const struct { int scenario; MH_STATUS expected; } cases[] = {
        {ENUM_FAIL, MH_ERROR_THREAD_FREEZE}, {SUSPEND_FAIL, MH_ERROR_THREAD_FREEZE},
        {CONTEXT_FAIL, MH_ERROR_THREAD_FREEZE}, {IP_BUSY, MH_ERROR_THREAD_BUSY},
        {PROTECT_FAIL, MH_ERROR_MEMORY_PROTECT}, {FLUSH_FAIL, MH_ERROR_CACHE_FLUSH},
        {RESTORE_FAIL, MH_ERROR_PATCH_ROLLBACK}, {RESUME_RETRY, MH_OK},
        {NEW_THREAD, MH_OK}, {RESUME_FAIL, MH_ERROR_THREAD_RESUME}, {NONE, MH_OK}
    };
    int okay = 1, disable, patchAbove;
    BOOL enabled = TRUE;
    UINT i;
    okay &= expect(MH_IsHookEnabled((void *)&detour, &enabled) == MH_ERROR_NOT_INITIALIZED && !enabled,
        "state query rejects an uninitialized library and clears its output");
    wakeEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    waitingThread = CreateThread(NULL, 0, waitWorker, NULL, 0, NULL);
    waitingThread2 = CreateThread(NULL, 0, waitWorker, NULL, 0, NULL);
    if (!wakeEvent || !waitingThread || !waitingThread2 || MH_Initialize() != MH_OK) return 1;
    enabled = TRUE;
    okay &= expect(MH_IsHookEnabled((void *)&detour, &enabled) == MH_ERROR_NOT_CREATED && !enabled,
        "state query rejects unknown targets and clears its output");
    enabled = TRUE;
    okay &= expect(MH_IsHookEnabled(NULL, &enabled) == MH_ERROR_INVALID_PARAMETER && !enabled &&
        MH_IsHookEnabled((void *)&detour, NULL) == MH_ERROR_INVALID_PARAMETER,
        "state query rejects null and batch parameters");
    for (disable = 0; disable < 2; ++disable)
        for (patchAbove = 0; patchAbove < 2; ++patchAbove)
            for (i = 0; i < ARRAYSIZE(cases); ++i)
                okay &= runFault(cases[i].scenario, cases[i].expected, disable, patchAbove);
    okay &= concurrentInstall();
    SetEvent(wakeEvent);
    WaitForSingleObject(waitingThread, INFINITE);
    WaitForSingleObject(waitingThread2, INFINITE);
    CloseHandle(waitingThread); CloseHandle(waitingThread2); CloseHandle(wakeEvent);
    MH_Uninitialize();
    if (okay) puts("PASS: strict enable/disable fault injection (44 cases), hotpatch rollback, hook state, page protection, real concurrent transitions.");
    return okay ? 0 : 1;
}
