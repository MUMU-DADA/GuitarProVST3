#include "state_manager.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QDir>
#include <QtCore/QTemporaryDir>
#include <iostream>

namespace {
bool check(bool value, const char *message) {
    if (!value) std::cerr << "FAIL: " << message << '\n';
    return value;
}
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;
    if (!check(dir.isValid(), "temporary directory")) return 1;
    qputenv("GPVST3_DATA_DIR", dir.path().toUtf8());
    qputenv("GPVST3_SCORE_PATH", "C:/scores/p8.gp");
    qputenv("GPVST3_TRACK", "1");

    const QJsonObject legacy{{"schema", 1}, {"score_id", "C:/scores/p8.gp"}, {"track", 1},
        {"effects", QJsonArray{QJsonObject{{"module", "global.vst3"}, {"class_id", "A"},
            {"name", "Global"}, {"enabled", true}, {"component_state", "AQID"}}}}};
    QFile file(gpvst3::state::sidecarPath());
    if (!check(QDir().mkpath(dir.path()) && file.open(QIODevice::WriteOnly), "write legacy")) return 1;
    file.write(QJsonDocument(legacy).toJson()); file.close();

    QJsonObject chain;
    if (!check(gpvst3::state::loadChain(chain), "migrate legacy")) return 1;
    if (!check(chain.value("schema").toInt() == 2 && chain.value("global").isObject(), "schema 2")) return 1;
    if (!check(gpvst3::state::scopeEffects(chain, gpvst3::state::ScopeKind::Global).size() == 1,
               "legacy moved to global")) return 1;

    const QJsonArray track1{QJsonObject{{"module", "track.vst3"}, {"class_id", "T1"},
        {"name", "Track one"}, {"enabled", true}, {"order", 0}}};
    const QJsonArray track2{QJsonObject{{"module", "track.vst3"}, {"class_id", "T2"},
        {"name", "Track two"}, {"enabled", true}, {"order", 0}}};
    gpvst3::state::setScopeEffects(chain, gpvst3::state::ScopeKind::Track, track1,
                                   "C:/scores/p8.gp", "C:/scores/p8.gp#track-1", 1, "Bass");
    gpvst3::state::setScopeEffects(chain, gpvst3::state::ScopeKind::Track, track2,
                                   "C:/scores/p8.gp", "C:/scores/p8.gp#track-2", 2, "Guitar");
    if (!check(gpvst3::state::writeChain(chain), "write schema 2")) return 1;
    QJsonObject loaded;
    if (!check(gpvst3::state::loadChain(loaded), "read schema 2")) return 1;
    if (!check(gpvst3::state::scopeEffects(loaded, gpvst3::state::ScopeKind::Track,
                                            "C:/scores/p8.gp", "C:/scores/p8.gp#track-1").first().toObject().value("class_id") == "T1",
               "track one isolated")) return 1;
    if (!check(gpvst3::state::scopeEffects(loaded, gpvst3::state::ScopeKind::Track,
                                            "C:/scores/p8.gp", "C:/scores/p8.gp#track-2").first().toObject().value("class_id") == "T2",
               "track two isolated")) return 1;
    std::cout << "PASS: P8 schema migration, global scope and isolated track state.\n";
    return 0;
}
