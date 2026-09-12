#include "state_manager.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
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
    QTemporaryDir directory;
    if (!check(directory.isValid(), "temporary directory")) return 1;
    qputenv("GPVST3_DATA_DIR", directory.path().toUtf8());
    qputenv("GPVST3_SCORE_PATH", "C:/scores/p5.gp");
    qputenv("GPVST3_TRACK", "3");
    qputenv("GPVST3_BUS", "master");

    QJsonObject empty;
    if (!check(gpvst3::state::loadChain(empty), "load default sidecar")) return 1;
    if (!check(empty.value("schema").toInt() == 2 && empty.value("track").toInt() == 3 &&
                   empty.value("global").isObject() && empty.value("scores").isObject(),
               "default schema 2 identity")) return 1;
    if (!check(empty.value("effects").isArray() && empty.value("effects").toArray().isEmpty(),
               "default effects")) return 1;

    const QJsonObject effect{{"plugin_path", "C:/VST3/Test.vst3"}, {"class_uid", "0011AABB"},
                             {"parameters", QJsonObject{{"0", 0.75}}},
                             {"state_chunk", "AQID"}, {"bypass", false}};
    const QJsonObject saved{{"score_id", "C:/scores/p5.gp"}, {"track", 3}, {"bus", "master"},
                            {"effects", QJsonArray{effect}}};
    if (!check(gpvst3::state::writeChain(saved), "write sidecar")) return 1;
    if (!check(QFile::exists(gpvst3::state::sidecarPath()), "sidecar file exists")) return 1;

    QJsonObject loaded;
    if (!check(gpvst3::state::loadChain(loaded), "load written sidecar")) return 1;
    const auto loadedEffect = loaded.value("effects").toArray().first().toObject();
    if (!check(loadedEffect.value("class_uid").toString() == "0011AABB" &&
                   loadedEffect.value("state_chunk").toString() == "AQID" &&
                   loadedEffect.value("parameters").toObject().value("0").toDouble() == 0.75,
               "effect fields round trip")) return 1;

    QJsonObject startupChain;
    startupChain.insert("schema", gpvst3::state::kSchema);
    startupChain.insert("effects", QJsonArray{});
    startupChain.insert("global", QJsonObject{{"effects", QJsonArray{
        QJsonObject{{"module", "C:/VST3/Global.vst3"}, {"class_id", "GLOBAL"},
                     {"enabled", true}, {"bypass", false}}}}});
    startupChain.insert("scores", QJsonObject{{"C:/scores/p5.gp", QJsonObject{{"tracks", QJsonObject{
        {"track-1", QJsonObject{{"track_index", 1}, {"effects", QJsonArray{
            QJsonObject{{"module", "C:/VST3/Track.vst3"}, {"class_id", "TRACK"},
                         {"enabled", true}, {"bypass", false}}}}}}}}}}});
    if (!check(gpvst3::state::writeChain(startupChain), "write startup activation fixture")) return 1;
    if (!check(gpvst3::state::disableAllEffectsAtStartup(), "disable saved effects at startup")) return 1;
    QJsonObject startupLoaded;
    if (!check(gpvst3::state::loadChain(startupLoaded), "load startup activation fixture")) return 1;
    const auto startupGlobal = startupLoaded.value("global").toObject().value("effects").toArray().first().toObject();
    const auto startupTrack = startupLoaded.value("scores").toObject().value("C:/scores/p5.gp")
        .toObject().value("tracks").toObject().value("track-1").toObject()
        .value("effects").toArray().first().toObject();
    if (!check(!startupGlobal.value("enabled").toBool() && startupGlobal.value("bypass").toBool() &&
                   startupGlobal.value("desired_enabled").toBool() &&
                   !startupTrack.value("enabled").toBool() && startupTrack.value("bypass").toBool() &&
                   startupTrack.value("desired_enabled").toBool(),
               "global and track effects start bypassed with intent preserved")) return 1;

    QFile corrupt(gpvst3::state::sidecarPath());
    if (!check(corrupt.open(QIODevice::WriteOnly | QIODevice::Truncate), "open corrupt sidecar")) return 1;
    corrupt.write("{bad json");
    corrupt.close();
    QString error;
    QJsonObject fallback;
    if (!check(!gpvst3::state::loadChain(fallback, &error), "reject corrupt sidecar")) return 1;
    if (!check(!error.isEmpty() && fallback.value("effects").toArray().isEmpty(),
               "fallback is empty chain")) return 1;
    std::cout << "PASS: P5 sidecar identity, effect fields and corrupt-state fallback.\n";
    return 0;
}
