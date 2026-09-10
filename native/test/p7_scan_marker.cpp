// Deliberately observable VST3 candidate for the real Guitar Pro scan test.
// Any loader/entry invocation in the host or a child records its actual PID.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>

namespace {
HMODULE module;
void mark(const wchar_t *entry) {
    wchar_t target[32768]{}, path[32768]{}, line[33000]{};
    if (!GetEnvironmentVariableW(L"GPVST3_TEST_ENTRY_LOG", target, 32768)) return;
    GetModuleFileNameW(module, path, 32768);
    const int length = swprintf_s(line, L"%lu\t%s\t%s\r\n", GetCurrentProcessId(), path, entry);
    HANDLE file = CreateFileW(target, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(file, line, length * sizeof(wchar_t), &written, nullptr);
        CloseHandle(file);
    }
}
}
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { module = instance; DisableThreadLibraryCalls(instance); mark(L"DllMain"); }
    return TRUE;
}
extern "C" __declspec(dllexport) bool InitDll() { mark(L"InitDll"); return true; }
extern "C" __declspec(dllexport) bool ExitDll() { mark(L"ExitDll"); return true; }
extern "C" __declspec(dllexport) void *GetPluginFactory() { mark(L"GetPluginFactory"); return nullptr; }
