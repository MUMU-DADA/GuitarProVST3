#include "state_manager.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QTemporaryDir>
#include <chrono>
#include <iostream>
#include <thread>

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;
    if (!dir.isValid()) return 1;
    qputenv("GPVST3_DATA_DIR", dir.path().toUtf8());
    gpvst3::state::startRealtimeObservationWriter();
    gpvst3::state::writeRealtimeObservation(QJsonObject{{"selection_status", "queued"}, {"callback_count", 1}});
    gpvst3::state::writeRealtimeObservation(QJsonObject{{"selection_status", "applied"}, {"callback_count", 2}});
    const auto path = QDir(dir.path()).filePath("p2-observation.json");
    for (int i = 0; i < 50 && !QFile::exists(path); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    gpvst3::state::stopRealtimeObservationWriter();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return 1;
    QJsonParseError error{};
    const auto doc = QJsonDocument::fromJson(file.readAll(), &error);
    const auto hook = doc.object().value("gp_hook").toObject();
    if (error.error != QJsonParseError::NoError || hook.value("selection_status").toString() != "applied") return 1;
    std::cout << "PASS: P11 bounded observation writer publishes the latest snapshot.\n";
    return 0;
}
