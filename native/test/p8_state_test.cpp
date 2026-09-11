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
    using gpvst3::state::HostTrackIdentity;
    using gpvst3::state::ScopeKind;
    std::vector<HostTrackIdentity> bindings{{"document-1", "untitled-1", "native-A", 0},
                                           {"document-1", "untitled-1", "native-B", 1}};
    if (!check(gpvst3::state::reconcileTrackIdentities(bindings), "bind new native identities")) return 1;
    const auto keyA = bindings[0].runtimeKey, keyB = bindings[1].runtimeKey;
    gpvst3::state::loadChain(chain);
    gpvst3::state::setScopeEffects(chain, ScopeKind::Track, track1, "untitled-1", keyA, 0);
    gpvst3::state::setScopeEffects(chain, ScopeKind::Track, track2, "untitled-1", keyB, 1);
    if (!check(gpvst3::state::writeChain(chain), "save independent native track states")) return 1;
    for (auto &binding : bindings) binding.scoreKey = "C:/scores/renamed.gp";
    bindings[0].index = 1; bindings[1].index = 0;
    if (!check(gpvst3::state::reconcileTrackIdentities(bindings) && bindings[0].runtimeKey == keyA &&
               bindings[1].runtimeKey == keyB, "save-as and reorder preserve native runtime identity")) return 1;
    gpvst3::state::loadChain(chain);
    if (!check(gpvst3::state::scopeEffects(chain, ScopeKind::Track, "untitled-1", keyA) ==
               gpvst3::state::scopeEffects(chain, ScopeKind::Track, "C:/scores/p8.gp", "C:/scores/p8.gp#track-1"),
               "a UI bound before save-as still resolves to the same track state")) return 1;
    bindings[0].index = 2; bindings[1].index = 1;
    bindings.push_back({"document-1", "C:/scores/renamed.gp", "native-C", 0});
    if (!check(gpvst3::state::reconcileTrackIdentities(bindings), "insert before existing tracks")) return 1;
    gpvst3::state::loadChain(chain);
    if (!check(gpvst3::state::scopeEffects(chain, ScopeKind::Track, {}, bindings[2].runtimeKey).isEmpty(),
               "inserted track never inherits the former index owner's effects")) return 1;
    const auto removed = bindings[0];
    bindings.erase(bindings.begin());
    if (!check(gpvst3::state::reconcileTrackIdentities(bindings), "remove track")) return 1;
    bindings.push_back(removed);
    if (!check(gpvst3::state::reconcileTrackIdentities(bindings) && bindings.back().runtimeKey == keyA,
               "undo reuses the original native identity")) return 1;
    gpvst3::state::resetTrackIdentities(); // Simulate a fresh process with new MCP UUIDs.
    bindings = {{"document-2", "C:/scores/renamed.gp", "fresh-C", 0},
                {"document-2", "C:/scores/renamed.gp", "fresh-B", 1},
                {"document-2", "C:/scores/renamed.gp", "fresh-A", 2}};
    if (!check(gpvst3::state::reconcileTrackIdentities(bindings), "bind reopened score")) return 1;
    gpvst3::state::loadChain(chain);
    if (!check(gpvst3::state::scopeEffects(chain, ScopeKind::Track, {}, bindings[0].runtimeKey).isEmpty() &&
               gpvst3::state::scopeEffects(chain, ScopeKind::Track, {}, bindings[1].runtimeKey).first().toObject().value("class_id") == "T2" &&
               gpvst3::state::scopeEffects(chain, ScopeKind::Track, {}, bindings[2].runtimeKey).first().toObject().value("class_id") == "T1",
               "reopen restores every track by the last saved index mapping")) return 1;
    std::cout << "PASS: P8 schema migration, scope isolation, save-as, reorder, insert/delete/undo and reopened identities.\n";
    return 0;
}
