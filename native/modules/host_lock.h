#pragma once

#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QHash>

namespace gpvst3::host {

struct Verification {
    bool supported = false;
    bool qtCoreSupported = false;
    QHash<QString, bool> files;
};

// Keep the runtime table stable; the P6 gate test compares it with the
// published host_manifest.json before reading the original installation.
inline QHash<QString, QByteArray> expectedHashes() {
    QHash<QString, QByteArray> hashes{
        {"GuitarPro.exe", "B233B0F1C87DEB3AECE693D51E8D3C3A841C88FEE78828607B20034737C4C6DF"},
        {"GPCore.dll", "9425F3E8EB627D328E0CB01146D43045D86D1BA639F73718BBE7FCCF733BD250"},
        {"GPRSE.dll", "E983122951B94C2513A1F05828DD03DCB11620DDC50F6B497723CAE0EB32BA6A"},
        {"AMAudio.dll", "0151B8D484A0DBEDBB74AA1A43975932349F812A2D8929DFAAD149EFBD992394"},
        {"AMOverloud.dll", "F3D750AD266E86C4254F6C9E58A9416E335B46A8DBD72150AFFF9DD1098452E2"}
    };
#ifdef GPVST3_TEST_REJECT_HOST_INDEX
    // Only the separately built negative-test DLL changes an expected hash.
    // The real host is always read-only, and production builds have no override.
    constexpr const char *files[]{"GuitarPro.exe", "GPCore.dll", "GPRSE.dll", "AMAudio.dll", "AMOverloud.dll"};
    static_assert(GPVST3_TEST_REJECT_HOST_INDEX >= 0 && GPVST3_TEST_REJECT_HOST_INDEX < 5);
    hashes[QString::fromLatin1(files[GPVST3_TEST_REJECT_HOST_INDEX])] = QByteArray(64, '0');
#endif
    return hashes;
}

inline QByteArray sha256(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash digest(QCryptographicHash::Sha256);
    if (!digest.addData(&file)) return {};
    return digest.result().toHex().toUpper();
}

inline Verification verifyDirectory(const QString &directory,
                                    const QHash<QString, QByteArray> &expected = expectedHashes()) {
    Verification result;
    const QDir hostDir(directory);
    for (auto it = expected.cbegin(); it != expected.cend(); ++it) {
        result.files.insert(it.key(), sha256(hostDir.filePath(it.key())) == it.value());
    }
    result.supported = !result.files.isEmpty();
    for (bool matches : result.files) result.supported = result.supported && matches;
    return result;
}

inline Verification verify() { return verifyDirectory(QCoreApplication::applicationDirPath()); }

}
