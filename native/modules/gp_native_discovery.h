#pragma once

// The same bounded MSVC RTTI checks used by GuitarProMCP/native/discovery.h.
// Only called for known Qt roots after the complete host hash lock passes.
#include <QtCore/QString>
#include <QtCore/QByteArray>
#include <QtCore/QObject>
#include <windows.h>
#include <array>

namespace gpvst3::gp_audio::native {
template<class T> bool read(quintptr address, T &out) {
    SIZE_T size = 0;
    return address && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void *>(address),
        &out, sizeof(out), &size) && size == sizeof(out);
}
struct Locator { quint32 signature, offset, constructionOffset, typeRva, hierarchyRva, selfRva; };
inline QString type(quintptr object) {
    quintptr vtable = 0, locator = 0;
    MEMORY_BASIC_INFORMATION memory{};
    if (!read(object, vtable) || !VirtualQuery(reinterpret_cast<void *>(vtable), &memory, sizeof(memory)) ||
        memory.Type != MEM_IMAGE || !read(vtable - 8, locator)) return {};
    Locator col{};
    if (!read(locator, col) || col.signature != 1 ||
        locator - col.selfRva != reinterpret_cast<quintptr>(memory.AllocationBase)) return {};
    std::array<char, 256> name{};
    if (!read(locator - col.selfRva + col.typeRva + 16, name)) return {};
    name.back() = 0;
    return QString::fromLatin1(name.data());
}
inline QObject *asQObject(quintptr object) {
    quintptr vtable = 0, locator = 0;
    Locator col{};
    if (!read(object, vtable) || !read(vtable - 8, locator) || !read(locator, col) || col.signature != 1) return nullptr;
    const auto base = locator - col.selfRva;
    struct Hierarchy { quint32 signature, attributes, count, arrayRva; } hierarchy{};
    if (!read(base + col.hierarchyRva, hierarchy) || hierarchy.count > 128) return nullptr;
    for (quint32 index = 0; index < hierarchy.count; ++index) {
        quint32 entry = 0;
        struct Base { quint32 typeRva, contained; qint32 displacement, vbtable, vdisp; quint32 attributes; } descriptor{};
        std::array<char, 96> name{};
        if (!read(base + hierarchy.arrayRva + index * 4, entry) || !read(base + entry, descriptor) ||
            !read(base + descriptor.typeRva + 16, name)) continue;
        name.back() = 0;
        if (QByteArray(name.data()) == ".?AVQObject@@" && descriptor.vbtable == -1 && descriptor.displacement >= 0)
            return reinterpret_cast<QObject *>(object - col.offset + descriptor.displacement);
    }
    return nullptr;
}

}
