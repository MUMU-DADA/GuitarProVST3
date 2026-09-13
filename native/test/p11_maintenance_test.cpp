#include "state_manager.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QTemporaryDir>

#include <chrono>
#include <iostream>
#include <thread>

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    if (!directory.isValid()) {
        std::cerr << "FAIL: temporary observation directory\n";
        return 1;
    }
    qputenv("GPVST3_DATA_DIR", directory.path().toUtf8());
    gpvst3::state::startRealtimeObservationWriter();
    gpvst3::state::writeRealtimeObservation(QJsonObject{{"selection_status", "queued"},
                                                        {"callback_count", 1}});
    gpvst3::state::writeRealtimeObservation(QJsonObject{{"selection_status", "applied"},
                                                        {"callback_count", 2}});
    const auto path = QDir(directory.path()).filePath(QStringLiteral("p2-observation.json"));
    for (int attempt = 0; attempt < 30 && !QFile::exists(path); ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    gpvst3::state::stopRealtimeObservationWriter();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        std::cerr << "FAIL: observation writer did not publish a file\n";
        return 1;
    }
    QJsonParseError error{};
    const auto document = QJsonDocument::fromJson(file.readAll(), &error);
    const auto hook = document.object().value("gp_hook").toObject();
    if (error.error != QJsonParseError::NoError || hook.value("selection_status").toString() != "applied") {
        std::cerr << "FAIL: observation writer lost the latest bounded snapshot\n";
        return 1;
    }
    std::cout << "PASS: P11 observation writer publishes the newest snapshot off the caller thread.\n";
    return 0;
}
