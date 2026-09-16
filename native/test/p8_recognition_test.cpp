#include "vst3_host.h"
#include "state_manager.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>
#include <iostream>

namespace {
bool check(bool value, const char *message) {
    if (!value) std::cerr << "FAIL: " << message << '\n';
    return value;
}
void write(const QString &path, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path); file.open(QIODevice::WriteOnly); file.write(bytes);
}
QString binary(const QString &module) { return module + "/Contents/x86_64-win/" + QFileInfo(module).fileName(); }
gpvst3::vst3::State recognize(const std::string &module, bool hostSupported) noexcept {
    gpvst3::vst3::State result;
    result.hostSupported = hostSupported;
    result.catalog.push_back({module, "ABCDEF0123456789ABCDEF0123456789", "Recognized", "Fixture",
                              "Audio Module Class", true, {}, true, "factory"});
    result.status = "ready";
    result.ready = true;
    return result;
}
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;
    if (!check(dir.isValid(), "temporary directory")) return 1;
    const auto root = dir.path() + "/plugins";
    const auto data = dir.path() + "/data";
    const auto module = root + "/Pending.vst3";
    QByteArray pe(128, '\0'); pe.replace(0, 2, "MZ"); pe[60] = 64;
    pe.replace(64, 4, QByteArray("PE\0\0", 4)); pe[68] = 0x64; pe[69] = char(0x86);
    write(binary(module), pe);
    qputenv("GPVST3_DATA_DIR", data.toUtf8());
    qputenv("GPVST3_VST3_ROOT", root.toUtf8());
    gpvst3::vst3::setRecognitionControl(&recognize);
    const auto pending = gpvst3::vst3::beginAsync(true);
    if (!check(pending.scanPending, "static scan starts asynchronously")) return 1;
    gpvst3::vst3::State result;
    bool completed = false;
    for (int i = 0; i < 10000; ++i) {
        if (gpvst3::vst3::poll(result) && result.recognitionCompleted > 0) { completed = true; break; }
        QThread::msleep(1);
    }
    if (!check(completed && result.recognitionAttempted == 1 && result.recognitionFailed == 0,
               "one background recognition attempt")) return 1;
    if (!check(result.catalog.size() == 1 && result.catalog.front().identified &&
               result.catalog.front().recognitionStatus == "ready" &&
               result.catalog.front().recognitionSource == "factory" && result.catalog.front().recognitionAttempts == 1,
               "recognized catalog replaces candidate")) return 1;
    QFile cache(QDir(data).filePath("vst3-catalog-cache.json"));
    for (int i = 0; i < 500 && !cache.exists(); ++i) QThread::msleep(10);
    if (!check(cache.open(QIODevice::ReadOnly), "recognition cache written")) return 1;
    QJsonObject cachedDocument;
    for (int i = 0; i < 500; ++i) {
        cache.seek(0);
        cachedDocument = QJsonDocument::fromJson(cache.readAll()).object();
        const auto record = cachedDocument.value("scopes").toObject().begin().value()
            .toObject().value("modules").toObject().begin().value().toObject();
        if (record.value("recognition_status").toString() == "ready") break;
        cache.close(); QThread::msleep(10); cache.open(QIODevice::ReadOnly);
    }
    const auto record = cachedDocument.value("scopes").toObject().begin().value()
        .toObject().value("modules").toObject().begin().value().toObject();
    if (!check(record.value("recognition_status").toString() == "ready" &&
               record.value("recognition_source").toString() == "factory" &&
               record.value("recognition_attempts").toInt() == 1, "recognition evidence persisted")) return 1;
    gpvst3::vst3::shutdownScan();
    std::cout << "PASS: P8 background recognition, cache evidence and nonblocking catalog.\n";
    return 0;
}
