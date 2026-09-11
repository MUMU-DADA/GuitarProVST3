#pragma once
#include <QtCore/QHash>
#include <QtCore/QPointer>
#include <QtCore/QObject>
#include <QtCore/QList>
#include <windows.h>
#include <atomic>

namespace gpvst3::gp_audio {
// Qt 5.15.3 lifecycle hooks, guarded by the exact Qt5Core hash at the caller.
// Chaining preserves an existing observer (including GuitarProMCP).
class NativeObjectRegistry {
    using Hook = void (*)(QObject *);
    inline static NativeObjectRegistry *active = nullptr;
    inline static std::atomic<DWORD> guiThread{0};
    inline static std::atomic<Hook> previousAdd{nullptr}, previousRemove{nullptr};
    QHash<QObject *, QPointer<QObject>> entries;
    quintptr *hooks = nullptr;
    static void added(QObject *object) {
        if (const auto previous = previousAdd.load()) previous(object);
        if (GetCurrentThreadId() == guiThread.load() && active && active->entries.size() < 100000)
            active->entries.insert(object, QPointer<QObject>(object));
    }
    static void removed(QObject *object) {
        if (const auto previous = previousRemove.load()) previous(object);
        if (GetCurrentThreadId() == guiThread.load() && active) active->entries.remove(object);
    }
public:
    bool install() {
        if (active) return false;
        hooks = reinterpret_cast<quintptr *>(GetProcAddress(GetModuleHandleW(L"Qt5Core.dll"), "?qtHookData@@3PA_KA"));
        if (!hooks || hooks[0] != 3 || hooks[1] < 7 || hooks[2] != 0x050f03) { hooks = nullptr; return false; }
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&added), &module)) { hooks = nullptr; return false; }
        previousAdd.store(reinterpret_cast<Hook>(hooks[3]));
        previousRemove.store(reinterpret_cast<Hook>(hooks[4]));
        guiThread.store(GetCurrentThreadId()); active = this;
        hooks[3] = reinterpret_cast<quintptr>(&added);
        hooks[4] = reinterpret_cast<quintptr>(&removed);
        return true;
    }
    void uninstall() {
        if (hooks) {
            if (hooks[3] == reinterpret_cast<quintptr>(&added)) hooks[3] = reinterpret_cast<quintptr>(previousAdd.load());
            if (hooks[4] == reinterpret_cast<quintptr>(&removed)) hooks[4] = reinterpret_cast<quintptr>(previousRemove.load());
            hooks = nullptr;
        }
        if (active == this) active = nullptr;
        entries.clear();
    }
    void forget(QObject *object) { entries.remove(object); }
    QList<QPointer<QObject>> objects() const {
        QList<QPointer<QObject>> result;
        for (const auto &object : entries) if (object) result.append(object);
        return result;
    }
};
}
