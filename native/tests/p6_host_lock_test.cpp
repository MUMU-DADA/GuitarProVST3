#include "host_lock.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QTemporaryDir>

#include <iostream>

namespace {
bool check(bool value, const char *message) {
    if (!value) std::cerr << "FAIL: " << message << '\n';
    return value;
}
}

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    if (argc != 3) {
        std::cerr << "Usage: p6_host_lock_test <Guitar Pro directory> <host_manifest.json>\n";
        return 2;
    }
    const QString source = QString::fromLocal8Bit(argv[1]);
    QFile manifestFile(QString::fromLocal8Bit(argv[2]));
    if (!check(manifestFile.open(QIODevice::ReadOnly), "open host manifest")) return 1;
    QJsonParseError parseError{};
    const auto manifest = QJsonDocument::fromJson(manifestFile.readAll(), &parseError);
    if (!check(parseError.error == QJsonParseError::NoError && manifest.isObject(),
               "parse host manifest")) return 1;
    const auto expected = gpvst3::host::expectedHashes();
    const auto manifestFiles = manifest.object().value(QStringLiteral("files")).toObject();
    if (!check(manifestFiles.size() == expected.size(), "manifest file count")) return 1;
    for (auto it = expected.cbegin(); it != expected.cend(); ++it) {
        if (!check(manifestFiles.value(it.key()).toString().compare(QString::fromLatin1(it.value()), Qt::CaseInsensitive) == 0,
                   "manifest and runtime hash table agree")) return 1;
    }
    QTemporaryDir directory;
    if (!check(directory.isValid(), "temporary directory")) return 1;

    for (auto it = expected.cbegin(); it != expected.cend(); ++it) {
        const QString input = QDir(source).filePath(it.key());
        const QString output = QDir(directory.path()).filePath(it.key());
        if (!check(QFile::exists(input) && QFile::copy(input, output),
                   "copy locked host file")) return 1;
    }

    const auto valid = gpvst3::host::verifyDirectory(directory.path());
    if (!check(valid.supported, "matching host hashes accepted")) return 1;
    for (auto it = valid.files.cbegin(); it != valid.files.cend(); ++it)
        if (!check(it.value(), "every locked file matched")) return 1;

    QFile tampered(QDir(directory.path()).filePath(QStringLiteral("GuitarPro.exe")));
    if (!check(tampered.open(QIODevice::Append), "open copied executable")) return 1;
    if (!check(tampered.write("p6") == 2, "tamper copied executable")) return 1;
    tampered.close();
    const auto invalid = gpvst3::host::verifyDirectory(directory.path());
    if (!check(!invalid.supported && !invalid.files.value("GuitarPro.exe"),
               "changed host hash rejected")) return 1;
    std::cout << "PASS: P6 host hash gate accepts the locked image and rejects a changed image.\n";
    return 0;
}
