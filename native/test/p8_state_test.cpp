#include "state_manager.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QDir>
#include <QtCore/QTemporaryDir>
#include <QtCore/QFileInfo>
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

    // Unconfigured topology must remain in memory only. Repeated document
    // discovery must not create one empty JSON record per score/track.
    const auto bounded = dir.path() + "/bounded";
    qputenv("GPVST3_DATA_DIR", bounded.toUtf8());
    gpvst3::state::resetTrackIdentities();
    std::vector<HostTrackIdentity> emptyBindings;
    for (int document = 0; document < 80; ++document) {
        emptyBindings.clear();
        for (int track = 0; track < 8; ++track)
            emptyBindings.push_back({QString("doc-%1").arg(document), QString("score-%1").arg(document),
                                     QString("native-%1-%2").arg(document).arg(track), track});
        if (!check(gpvst3::state::reconcileTrackIdentities(emptyBindings), "unconfigured topology reconcile")) return 1;
    }
    QJsonObject boundedChain;
    if (!check(gpvst3::state::loadChain(boundedChain), "load bounded topology")) return 1;
    if (!check(boundedChain.value("scores").toObject().isEmpty(), "empty topology is not persisted")) return 1;
    if (!check(QFileInfo(gpvst3::state::sidecarPath()).size() < 4096, "bounded sidecar remains small")) return 1;

    // A large legacy file is compacted on read, while an explicit configured
    // disabled entry and its opaque plugin state survive the repair.
    qputenv("GPVST3_DATA_DIR", (dir.path() + "/large").toUtf8());
    QJsonArray noisyEffects;
    for (int index = 0; index < 12; ++index)
        noisyEffects.append(QJsonObject{{"module", QString("C:/VST3/noisy-%1.vst3").arg(index)},
            {"class_id", QString("NOISY%1").arg(index)}, {"name", "Noisy"}, {"enabled", false}, {"bypass", true}});
    noisyEffects.append(QJsonObject{{"module", "C:/VST3/configured.vst3"}, {"class_id", "CONFIGURED"},
        {"enabled", false}, {"configured", true}, {"bypass", true}, {"component_state", "AQID"}});
    QJsonObject noisyScores;
    for (int score = 0; score < 150; ++score) {
        QJsonObject tracks;
        for (int track = 0; track < 8; ++track)
            tracks.insert(QString("track-%1").arg(track), QJsonObject{{"track_index", track},
                {"effects", noisyEffects}});
        noisyScores.insert(QString("score-%1").arg(score), QJsonObject{{"tracks", tracks}});
    }
    QJsonObject noisy{{"schema", 2}, {"global", QJsonObject{{"effects", noisyEffects}}},
                      {"scores", noisyScores}, {"effects", noisyEffects}};
    QFile noisyFile(gpvst3::state::sidecarPath());
    if (!check(QDir().mkpath(QFileInfo(noisyFile).absolutePath()) && noisyFile.open(QIODevice::WriteOnly),
               "write noisy sidecar")) return 1;
    const auto noisyBytes = QJsonDocument(noisy).toJson(QJsonDocument::Indented);
    noisyFile.write(noisyBytes); noisyFile.close();
    if (!check(noisyBytes.size() > 1024 * 1024, "noisy sidecar fixture is large")) return 1;
    QJsonObject compacted;
    if (!check(gpvst3::state::loadChain(compacted), "compact noisy sidecar")) return 1;
    if (!check(QFileInfo(gpvst3::state::sidecarPath()).size() < noisyBytes.size() / 4,
               "large sidecar was compacted")) return 1;
    const auto compactedGlobal = compacted.value("global").toObject().value("effects").toArray();
    if (!check(compactedGlobal.size() == 1 && compactedGlobal.first().toObject().value("configured").toBool(),
               "configured disabled state survives compaction")) return 1;

    // Rewriting an unchanged compact state must not update the file merely to
    // refresh saved_at.
    QFile stableFile(gpvst3::state::sidecarPath());
    if (!check(stableFile.open(QIODevice::ReadOnly), "read stable sidecar")) return 1;
    auto stableDocument = QJsonDocument::fromJson(stableFile.readAll()); stableFile.close();
    auto stableObject = stableDocument.object(); stableObject.insert("saved_at", "stable");
    if (!check(stableFile.open(QIODevice::WriteOnly | QIODevice::Truncate), "write stable marker")) return 1;
    stableFile.write(QJsonDocument(stableObject).toJson(QJsonDocument::Indented)); stableFile.close();
    const auto beforeStable = [&] { QFile f(gpvst3::state::sidecarPath()); f.open(QIODevice::ReadOnly); return f.readAll(); }();
    if (!check(gpvst3::state::writeChain(stableObject), "write unchanged compact state")) return 1;
    const auto afterStable = [&] { QFile f(gpvst3::state::sidecarPath()); f.open(QIODevice::ReadOnly); return f.readAll(); }();
    if (!check(beforeStable == afterStable, "unchanged sidecar is not rewritten")) return 1;

    if (!check(gpvst3::state::setPluginEnabled(false) && !gpvst3::state::pluginEnabled(),
               "startup enable switch persists")) return 1;
    if (!check(gpvst3::state::setPluginEnabled(true) && gpvst3::state::pluginEnabled(),
               "startup enable switch restores")) return 1;
    std::cout << "PASS: P8 schema migration, scope isolation, save-as, reorder, insert/delete/undo and reopened identities.\n";
    return 0;
}
