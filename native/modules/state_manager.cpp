#include "state_manager.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QSaveFile>
#include <QtCore/QStandardPaths>

namespace gpvst3::state {
namespace {

constexpr int kLegacySchema = 1;

bool writeJson(const QString &path, const QJsonObject &object) {
    const QFileInfo info(path);
    if (!QDir().mkpath(info.absolutePath())) return false;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Indented);
    return file.write(bytes) == bytes.size() && file.commit();
}

QJsonObject emptyChain() {
    const QString configuredScore = qEnvironmentVariable("GPVST3_SCORE_PATH");
    const QString scoreId = configuredScore.isEmpty()
        ? QStringLiteral("unspecified") : QFileInfo(configuredScore).absoluteFilePath();
    bool trackOk = false;
    const int track = qEnvironmentVariable("GPVST3_TRACK").toInt(&trackOk);
    const QString bus = qEnvironmentVariable("GPVST3_BUS", QStringLiteral("master"));
    const auto trackKey = scoreId + QStringLiteral("#track-") + QString::number(trackOk ? track : 0);
    return QJsonObject{{"schema", kSchema}, {"score_id", scoreId}, {"track", trackOk ? track : 0},
                       {"bus", bus}, {"effects", QJsonArray{}},
                       {"global", QJsonObject{{"effects", QJsonArray{}}}},
                       {"scores", QJsonObject{{scoreId, QJsonObject{{"tracks", QJsonObject{{
                           {trackKey, QJsonObject{{"track_index", trackOk ? track : 0}, {"track_name", QString{}},
                                                 {"effects", QJsonArray{}}}}}}}}}}}};
}

void normalizeEffects(QJsonObject &chain) {
    auto effects = chain.value("effects").toArray();
    for (int index = 0; index < effects.size(); ++index) {
        auto effect = effects.at(index).toObject();
        // P5 stored the inverse flag. Keep it for older readers, but expose
        // the P7 two-state meaning to the UI and runtime.
        if (!effect.contains("enabled"))
            effect.insert("enabled", !effect.value("bypass").toBool());
        if (!effect.contains("bypass"))
            effect.insert("bypass", !effect.value("enabled").toBool());
        effects.replace(index, effect);
    }
    chain.insert("effects", effects);
}

void normalizeScope(QJsonObject &scope) {
    auto effects = scope.value("effects").toArray();
    QJsonArray normalized;
    int order = 0;
    for (const auto &value : effects) {
        if (!value.isObject()) continue;
        auto effect = value.toObject();
        if (effect.value("module").toString().isEmpty() && effect.value("plugin_path").toString().isEmpty()) continue;
        if (effect.value("module").toString().isEmpty()) effect.insert("module", effect.value("plugin_path"));
        if (effect.value("class_id").toString().isEmpty() && !effect.value("class_uid").toString().isEmpty())
            effect.insert("class_id", effect.value("class_uid"));
        if (!effect.contains("entry_id")) {
            const auto module = effect.value("module").toString().isEmpty()
                ? effect.value("plugin_path").toString() : effect.value("module").toString();
            const auto classId = effect.value("class_id").toString().isEmpty()
                ? effect.value("class_uid").toString() : effect.value("class_id").toString();
            effect.insert("entry_id", QDir::cleanPath(QDir::fromNativeSeparators(module)).toLower() +
                          QStringLiteral("\n") + classId.toUpper());
        }
        if (!effect.contains("enabled")) effect.insert("enabled", !effect.value("bypass").toBool());
        if (!effect.contains("bypass")) effect.insert("bypass", !effect.value("enabled").toBool());
        effect.insert("order", order++);
        normalized.append(effect);
    }
    scope.insert("effects", normalized);
}

void migrateToSchema2(QJsonObject &chain) {
    if (chain.value("schema").toInt() == kSchema && chain.value("global").isObject()) {
        auto global = chain.value("global").toObject();
        normalizeScope(global);
        chain.insert("global", global);
        auto scores = chain.value("scores").toObject();
        for (auto score = scores.begin(); score != scores.end(); ++score) {
            auto scoreObject = score.value().toObject();
            auto tracks = scoreObject.value("tracks").toObject();
            for (auto track = tracks.begin(); track != tracks.end(); ++track) {
                auto trackObject = track.value().toObject();
                normalizeScope(trackObject);
                tracks.insert(track.key(), trackObject);
            }
            scoreObject.insert("tracks", tracks);
            scores.insert(score.key(), scoreObject);
        }
        chain.insert("scores", scores);
        return;
    }
    const auto legacy = chain.value("effects").toArray();
    QJsonObject global{{"effects", legacy}};
    normalizeScope(global);
    const auto configuredScore = qEnvironmentVariable("GPVST3_SCORE_PATH");
    const auto score = chain.value("score_id").toString().isEmpty()
        ? (configuredScore.isEmpty() ? QStringLiteral("unspecified") : QFileInfo(configuredScore).absoluteFilePath())
        : chain.value("score_id").toString();
    const int track = chain.value("track").toInt(0);
    const auto trackKey = score + QStringLiteral("#track-") + QString::number(track);
    chain.insert("schema", kSchema);
    chain.insert("migrated_from_schema", kLegacySchema);
    chain.insert("migration_marker", QStringLiteral("schema1_to_schema2"));
    chain.insert("global", global);
    chain.insert("scores", QJsonObject{{score, QJsonObject{{"tracks", QJsonObject{{
        {trackKey, QJsonObject{{"track_index", track}, {"track_name", QString{}}, {"effects", QJsonArray{}}}}}}}}}});
    // Keep the legacy view for older P5 readers while all new writes use scopes.
    chain.insert("effects", legacy);
}

}

QString dataDirectory() {
    const QString configured = qEnvironmentVariable("GPVST3_DATA_DIR");
    if (!configured.isEmpty()) return configured;
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/GuitarProVST3";
}

QString sidecarPath() { return QDir(dataDirectory()).filePath(QStringLiteral("effect-chain.json")); }

bool loadChain(QJsonObject &chain, QString *error) {
    QFile file(sidecarPath());
    if (!file.exists()) { chain = emptyChain(); return true; }
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = file.errorString();
        chain = emptyChain();
        return false;
    }
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error) *error = parseError.errorString();
        chain = emptyChain();
        return false;
    }
    chain = document.object();
    if (chain.value("schema").toInt() != kSchema && chain.value("schema").toInt() != kLegacySchema) {
        if (error) *error = QStringLiteral("unsupported_sidecar_schema");
        chain = emptyChain();
        return false;
    }
    if (chain.value("schema").toInt() == kLegacySchema && !chain.value("effects").isArray()) {
        if (error) *error = QStringLiteral("unsupported_sidecar_schema");
        chain = emptyChain();
        return false;
    }
    migrateToSchema2(chain);
    normalizeEffects(chain);
    return true;
}

bool writeChain(const QJsonObject &input) {
    QJsonObject chain = input;
    if (!chain.value("effects").isArray()) chain.insert("effects", QJsonArray{});
    bool legacyView = false;
    for (const auto &value : chain.value("effects").toArray())
        legacyView |= value.toObject().contains("plugin_path");
    if (legacyView) {
        chain.insert("schema", kLegacySchema);
        chain.remove("global");
        chain.remove("scores");
    }
    migrateToSchema2(chain);
    normalizeEffects(chain);
    auto global = chain.value("global").toObject();
    normalizeScope(global);
    chain.insert("global", global);
    chain.insert("saved_at", QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    return writeJson(sidecarPath(), chain);
}

QString currentScoreKey() {
    const auto configured = qEnvironmentVariable("GPVST3_SCORE_PATH");
    return configured.isEmpty() ? QStringLiteral("unspecified") : QFileInfo(configured).absoluteFilePath();
}

TrackKey currentTrackKey(const ScoreKey &score) {
    bool ok = false;
    const int track = qEnvironmentVariable("GPVST3_TRACK").toInt(&ok);
    return (score.isEmpty() ? currentScoreKey() : score) + QStringLiteral("#track-") + QString::number(ok ? track : 0);
}

QJsonArray scopeEffects(const QJsonObject &input, ScopeKind scope, const ScoreKey &score,
                        const TrackKey &track) {
    QJsonObject chain = input;
    migrateToSchema2(chain);
    if (scope == ScopeKind::Global) return chain.value("global").toObject().value("effects").toArray();
    const auto scoreObject = chain.value("scores").toObject().value(score.isEmpty() ? currentScoreKey() : score).toObject();
    return scoreObject.value("tracks").toObject().value(track.isEmpty() ? currentTrackKey(score) : track).toObject().value("effects").toArray();
}

void setScopeEffects(QJsonObject &chain, ScopeKind scope, const QJsonArray &effects,
                     const ScoreKey &score, const TrackKey &track, int trackIndex,
                     const QString &trackName) {
    migrateToSchema2(chain);
    QJsonObject scopeObject{{"effects", effects}};
    normalizeScope(scopeObject);
    if (scope == ScopeKind::Global) {
        chain.insert("global", scopeObject);
        chain.insert("effects", scopeObject.value("effects"));
        return;
    }
    const auto scoreKey = score.isEmpty() ? currentScoreKey() : score;
    const auto trackKey = track.isEmpty() ? currentTrackKey(scoreKey) : track;
    auto scores = chain.value("scores").toObject();
    auto scoreObject = scores.value(scoreKey).toObject();
    auto tracks = scoreObject.value("tracks").toObject();
    auto trackObject = tracks.value(trackKey).toObject();
    if (trackIndex >= 0) trackObject.insert("track_index", trackIndex);
    if (!trackName.isEmpty()) trackObject.insert("track_name", trackName);
    trackObject.insert("effects", scopeObject.value("effects"));
    tracks.insert(trackKey, trackObject);
    scoreObject.insert("tracks", tracks);
    scores.insert(scoreKey, scoreObject);
    chain.insert("scores", scores);
    // Compatibility view used by P5/P7 readers follows the currently active
    // track scope. The canonical data remains under scores.
    chain.insert("effects", scopeObject.value("effects"));
}

bool writeStatus(const QJsonObject &input) {
    QJsonObject status = input;
    status.insert("pid", QCoreApplication::applicationPid());
    status.insert("executable", QCoreApplication::applicationFilePath());
    status.insert("time", QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    return writeJson(QDir(dataDirectory()).filePath(QStringLiteral("status.json")), status);
}

bool writeRealtimeObservation(const QJsonObject &hookStatus) {
    const QJsonObject status{
        {"schema", 1},
        {"gp_hook", hookStatus},
        {"time", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}};
    return writeJson(QDir(dataDirectory()).filePath(QStringLiteral("p2-observation.json")), status);
}

}
