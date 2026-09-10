#include "vst3_host.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>
#include <chrono>
#include <iostream>
#include <thread>

namespace {
bool check(bool value, const char *message) {
    if (!value) std::cerr << "FAIL: " << message << '\n';
    return value;
}
void write(const QString &path, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    file.open(QIODevice::WriteOnly);
    file.write(bytes);
}
QString binary(const QString &module) { return module + "/Contents/x86_64-win/" + QFileInfo(module).fileName(); }

gpvst3::vst3::State recognize(const std::string &module, bool hostSupported) noexcept {
    static bool slow = true;
    if (slow) {
        slow = false;
        std::this_thread::sleep_for(std::chrono::seconds(11));
    }
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
    const auto first = root + "/A-Slow.vst3";
    const auto second = root + "/B-Fast.vst3";
    QByteArray pe(128, '\0');
    pe.replace(0, 2, "MZ");
    pe[60] = 64;
    pe.replace(64, 4, QByteArray("PE\0\0", 4));
    pe[68] = 0x64;
    pe[69] = char(0x86);
    write(binary(first), pe);
    write(binary(second), pe);
    qputenv("GPVST3_DATA_DIR", data.toUtf8());
    qputenv("GPVST3_VST3_ROOT", root.toUtf8());
    gpvst3::vst3::setRecognitionControl(&recognize);
    if (!check(gpvst3::vst3::beginAsync(true).scanPending, "static scan starts")) return 1;

    gpvst3::vst3::State result;
    bool timedOut = false;
    bool fastReady = false;
    for (int i = 0; i < 14000; ++i) {
        if (gpvst3::vst3::poll(result)) {
            timedOut |= result.recognitionTimedOut > 0;
            fastReady |= result.recognitionCompleted >= 2 && result.catalog.size() == 1 &&
                         QString::fromStdString(result.catalog.front().module).compare(second, Qt::CaseInsensitive) == 0;
            if (fastReady) break;
        }
        QThread::msleep(1);
    }
    if (!check(timedOut, "slow bundle reaches the ten second watchdog")) return 1;
    if (!check(fastReady, "queue continues with the next bundle after timeout")) return 1;
    if (!check(result.recognitionWorkersDetached == 1, "detached timeout worker is counted")) return 1;

    QFile cache(QDir(data).filePath("vst3-catalog-cache.json"));
    if (!check(cache.open(QIODevice::ReadOnly), "timeout cache written")) return 1;
    const auto scopes = QJsonDocument::fromJson(cache.readAll()).object().value("scopes").toObject();
    const auto modules = scopes.begin().value().toObject().value("modules").toObject();
    QJsonObject timeout;
    for (auto it = modules.begin(); it != modules.end(); ++it)
        if (QString::fromLatin1(it.key().toUtf8()).compare(first, Qt::CaseInsensitive) == 0)
            timeout = it.value().toObject();
    if (!check(timeout.value("recognition_status").toString() == "timeout" &&
               timeout.value("recognition_error").toString() == "recognition_timeout" &&
               timeout.value("recognition_deadline_at").toDouble() > 0,
               "timeout reason and deadline persist")) return 1;
    gpvst3::vst3::shutdownScan();
    std::cout << "PASS: P8 recognition watchdog hides timed out bundles and advances the queue.\n";
    return 0;
}
