#include "state_manager.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QSaveFile>
#include <QtCore/QHash>
#include <QtCore/QSet>
#include <QtCore/QUuid>
#include <QtCore/QStandardPaths>
#include <mutex>
#include <algorithm>
#include <condition_variable>
#include <thread>
#include <atomic>

namespace gpvst3::state {
namespace {

constexpr int kLegacySchema = 1;

struct RuntimeTrackContext {
    QString score;
    QString track;
    QString trackId;
    int index = -1;
    bool available = false;
};

RuntimeTrackContext g_runtimeTrackContext;
std::mutex g_runtimeTrackContextMutex;
struct DocumentTracks {
    QString score;
    QHash<QString, QString> persistentKeys;
};
struct TrackLocation { QString score, key; int index = -1; };
QHash<QString, DocumentTracks> g_documentTracks;
QHash<QString, TrackLocation> g_trackLocations;
QString g_identitySignature;
bool writeJson(const QString &path, const QJsonObject &object);

class ObservationWriter {
public:
    explicit ObservationWriter(QString fileName) : fileName_(std::move(fileName)) {}
    ~ObservationWriter() { stop(); }
    void start() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (thread_.joinable()) return;
        stopping_ = false;
        thread_ = std::thread([this] { run(); });
    }
    void submit(const QJsonObject &object) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            latest_ = object;
            pending_ = true;
        }
        condition_.notify_one();
    }
    void stop() noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        condition_.notify_one();
        if (thread_.joinable()) thread_.join();
    }
private:
    void run() {
        for (;;) {
            QJsonObject value;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                condition_.wait(lock, [this] { return stopping_ || pending_; });
                if (stopping_ && !pending_) return;
                value = latest_;
                pending_ = false;
            }
            writeJson(QDir(dataDirectory()).filePath(fileName_), value);
        }
    }
    std::mutex mutex_;
    std::condition_variable condition_;
    std::thread thread_;
    QJsonObject latest_;
    bool pending_ = false;
    bool stopping_ = false;
    QString fileName_;
};

ObservationWriter g_observationWriter(QStringLiteral("p2-observation.json"));
ObservationWriter g_statusWriter(QStringLiteral("status.json"));

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
    return QJsonObject{{"schema", kSchema}, {"score_id", scoreId}, {"track", trackOk ? track : 0},
                       {"bus", bus}, {"effects", QJsonArray{}},
                       {"global", QJsonObject{{"effects", QJsonArray{}}}},
                       {"scores", QJsonObject{}}};
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

bool hasPersistedEffectState(const QJsonObject &effect) {
    if (effect.value("enabled").toBool() || effect.value("configured").toBool() ||
        effect.value("desired_enabled").toBool() || !effect.value("last_error").toString().isEmpty())
        return true;
    for (const char *field : {"component_state", "controller_state", "state_chunk"})
        if (!effect.value(field).toString().isEmpty()) return true;
    const auto parameters = effect.value("parameters");
    return parameters.isObject() && !parameters.toObject().isEmpty();
}

void compactScope(QJsonObject &scope) {
    const auto effects = scope.value("effects").toArray();
    QJsonArray compacted;
    QHash<QString, int> positions;
    int order = 0;
    for (const auto &value : effects) {
        if (!value.isObject()) continue;
        auto effect = value.toObject();
        if (!hasPersistedEffectState(effect)) continue;
        const auto identity = QDir::cleanPath(QDir::fromNativeSeparators(
            effect.value("module").toString())).toLower() + QStringLiteral("\n") +
            effect.value("class_id").toString().toUpper();
        if (positions.contains(identity)) {
            // Catalog refreshes can report the same bundle with path casing
            // changed. Keep one record per module/class and merge fields that
            // are only present in the later copy.
            auto merged = compacted.at(positions.value(identity)).toObject();
            for (auto it = effect.begin(); it != effect.end(); ++it)
                if (!merged.contains(it.key()) || merged.value(it.key()).isNull() ||
                    (merged.value(it.key()).isString() && merged.value(it.key()).toString().isEmpty()))
                    merged.insert(it.key(), it.value());
            merged.insert("order", positions.value(identity));
            compacted.replace(positions.value(identity), merged);
            continue;
        }
        effect.insert("order", order++);
        positions.insert(identity, compacted.size());
        compacted.append(effect);
    }
    scope.insert("effects", compacted);
}

void compactChain(QJsonObject &chain) {
    auto global = chain.value("global").toObject();
    normalizeScope(global);
    compactScope(global);
    chain.insert("global", global);
    const auto scores = chain.value("scores").toObject();
    QJsonObject compactedScores;
    for (auto score = scores.begin(); score != scores.end(); ++score) {
        auto scoreObject = score.value().toObject();
        auto tracks = scoreObject.value("tracks").toObject();
        QJsonObject compactedTracks;
        for (auto track = tracks.begin(); track != tracks.end(); ++track) {
            auto trackObject = track.value().toObject();
            normalizeScope(trackObject);
            compactScope(trackObject);
            // Empty, non-present records are topology bookkeeping rather than
            // user configuration. They must not accumulate across documents.
            if (trackObject.value("effects").toArray().isEmpty()) continue;
            compactedTracks.insert(track.key(), trackObject);
        }
        if (compactedTracks.isEmpty()) continue;
        scoreObject.insert("tracks", compactedTracks);
        compactedScores.insert(score.key(), scoreObject);
    }
    chain.insert("scores", compactedScores);
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

QString settingsPath() { return QDir(dataDirectory()).filePath(QStringLiteral("settings.json")); }

bool pluginEnabled() {
    QFile file(settingsPath());
    if (!file.open(QIODevice::ReadOnly)) return true;
    QJsonParseError error{};
    const auto document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return true;
    const auto value = document.object().value("enabled");
    return !value.isBool() || value.toBool();
}

bool setPluginEnabled(bool enabled) {
    QJsonObject settings;
    QFile file(settingsPath());
    if (file.exists()) {
        if (!file.open(QIODevice::ReadOnly)) return false;
        QJsonParseError error{};
        const auto document = QJsonDocument::fromJson(file.readAll(), &error);
        file.close();
        if (error.error != QJsonParseError::NoError || !document.isObject()) return false;
        settings = document.object();
    }
    settings.insert("enabled", enabled);
    return writeJson(settingsPath(), settings);
}

bool disableAllEffectsAtStartup() {
    QJsonObject chain;
    if (!loadChain(chain)) return false;
    bool changed = false;
    const auto clearScope = [&changed](QJsonObject scope) {
        auto effects = scope.value("effects").toArray();
        for (int index = 0; index < effects.size(); ++index) {
            auto effect = effects.at(index).toObject();
            if (!effect.value("enabled").toBool()) continue;
            // Preserve the user's last explicit choice as an intent marker,
            // while making the current host session start fully bypassed.
            // The marker is consumed only by a later explicit enable action;
            // it never causes a processor to be created during bootstrap.
            effect.insert("enabled", false);
            effect.insert("bypass", true);
            effect.insert("desired_enabled", true);
            effects.replace(index, effect);
            changed = true;
        }
        scope.insert("effects", effects);
        return scope;
    };
    chain.insert("global", clearScope(chain.value("global").toObject()));
    auto scores = chain.value("scores").toObject();
    for (auto score = scores.begin(); score != scores.end(); ++score) {
        auto scoreObject = score.value().toObject();
        auto tracks = scoreObject.value("tracks").toObject();
        for (auto track = tracks.begin(); track != tracks.end(); ++track) {
            auto trackObject = track.value().toObject();
            track.value() = clearScope(trackObject);
        }
        scoreObject.insert("tracks", tracks);
        score.value() = scoreObject;
    }
    chain.insert("scores", scores);
    if (!changed) return true;
    // Keep the legacy view synchronized for older readers while preserving
    // component/controller state captured in each scope.
    chain.insert("effects", chain.value("global").toObject().value("effects"));
    return writeChain(chain);
}

bool loadChain(QJsonObject &chain, QString *error) {
    QFile file(sidecarPath());
    if (!file.exists()) { chain = emptyChain(); return true; }
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = file.errorString();
        chain = emptyChain();
        return false;
    }
    const auto sourceSize = file.size();
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    file.close();
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
    // Older builds copied the complete discovered catalog into every track.
    // Repair that file once on read while preserving configured plugin state.
    if (sourceSize > 1024 * 1024) {
        compactChain(chain);
        chain.insert("effects", chain.value("global").toObject().value("effects"));
        if (!writeChain(chain) && error) *error = QStringLiteral("sidecar_compaction_write_failed");
    }
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
    compactChain(chain);
    // Older readers only understand this one scope. Keep it bounded to the
    // compact global chain rather than copying the full discovery catalog.
    chain.insert("effects", chain.value("global").toObject().value("effects"));
    QJsonObject existing;
    QFile file(sidecarPath());
    if (file.open(QIODevice::ReadOnly)) {
        QJsonParseError error{};
        const auto document = QJsonDocument::fromJson(file.readAll(), &error);
        file.close();
        if (error.error == QJsonParseError::NoError && document.isObject()) existing = document.object();
    }
    auto comparable = chain;
    comparable.remove("saved_at");
    if (!existing.isEmpty()) {
        existing.remove("saved_at");
        if (QJsonDocument(existing) == QJsonDocument(comparable)) return true;
    }
    chain.insert("saved_at", QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    return writeJson(sidecarPath(), chain);
}

QString currentScoreKey() {
    {
        std::lock_guard<std::mutex> lock(g_runtimeTrackContextMutex);
        if (g_runtimeTrackContext.available && !g_runtimeTrackContext.score.isEmpty())
            return g_runtimeTrackContext.score;
    }
    const auto configured = qEnvironmentVariable("GPVST3_SCORE_PATH");
    return configured.isEmpty() ? QStringLiteral("unspecified") : QFileInfo(configured).absoluteFilePath();
}

TrackKey currentTrackKey(const ScoreKey &score) {
    {
        std::lock_guard<std::mutex> lock(g_runtimeTrackContextMutex);
        if (g_runtimeTrackContext.available &&
            (score.isEmpty() || score == g_runtimeTrackContext.score) &&
            !g_runtimeTrackContext.track.isEmpty())
            return g_runtimeTrackContext.track;
    }
    bool ok = false;
    const int track = qEnvironmentVariable("GPVST3_TRACK").toInt(&ok);
    return (score.isEmpty() ? currentScoreKey() : score) + QStringLiteral("#track-") + QString::number(ok ? track : 0);
}

void setRuntimeTrackContext(const ScoreKey &score, const TrackKey &track,
                            int trackIndex, const QString &trackId) {
    std::lock_guard<std::mutex> lock(g_runtimeTrackContextMutex);
    g_runtimeTrackContext.score = score;
    g_runtimeTrackContext.track = track;
    g_runtimeTrackContext.trackId = trackId;
    g_runtimeTrackContext.index = trackIndex;
    g_runtimeTrackContext.available = !score.isEmpty() && !track.isEmpty() && trackIndex >= 0;
}

void clearRuntimeTrackContext() {
    std::lock_guard<std::mutex> lock(g_runtimeTrackContextMutex);
    g_runtimeTrackContext = {};
}

bool runtimeTrackContextAvailable() {
    std::lock_guard<std::mutex> lock(g_runtimeTrackContextMutex);
    return g_runtimeTrackContext.available;
}

int runtimeTrackIndex() {
    std::lock_guard<std::mutex> lock(g_runtimeTrackContextMutex);
    return g_runtimeTrackContext.index;
}

QString runtimeTrackId() {
    std::lock_guard<std::mutex> lock(g_runtimeTrackContextMutex);
    return g_runtimeTrackContext.trackId;
}

QJsonArray scopeEffects(const QJsonObject &input, ScopeKind scope, const ScoreKey &score,
                        const TrackKey &track) {
    QJsonObject chain = input;
    migrateToSchema2(chain);
    if (scope == ScopeKind::Global) return chain.value("global").toObject().value("effects").toArray();
    const auto requested = track.isEmpty() ? currentTrackKey(score) : track;
    const auto location = g_trackLocations.value(requested,
        {score.isEmpty() ? currentScoreKey() : score, requested, -1});
    const auto scoreObject = chain.value("scores").toObject().value(location.score).toObject();
    return scoreObject.value("tracks").toObject().value(location.key).toObject().value("effects").toArray();
}

void setScopeEffects(QJsonObject &chain, ScopeKind scope, const QJsonArray &effects,
                     const ScoreKey &score, const TrackKey &track, int trackIndex,
                     const QString &trackName) {
    migrateToSchema2(chain);
    QJsonObject scopeObject{{"effects", effects}};
    normalizeScope(scopeObject);
    compactScope(scopeObject);
    if (scope == ScopeKind::Global) {
        chain.insert("global", scopeObject);
        chain.insert("effects", scopeObject.value("effects"));
        return;
    }
    const auto requested = track.isEmpty() ? currentTrackKey(score) : track;
    const auto location = g_trackLocations.value(requested,
        {score.isEmpty() ? currentScoreKey() : score, requested, trackIndex});
    const auto scoreKey = location.score;
    const auto trackKey = location.key;
    if (location.index >= 0) trackIndex = location.index;
    auto scores = chain.value("scores").toObject();
    auto scoreObject = scores.value(scoreKey).toObject();
    auto tracks = scoreObject.value("tracks").toObject();
    auto trackObject = tracks.value(trackKey).toObject();
    if (scopeObject.value("effects").toArray().isEmpty()) {
        tracks.remove(trackKey);
        if (tracks.isEmpty()) scores.remove(scoreKey);
        else {
            scoreObject.insert("tracks", tracks);
            scores.insert(scoreKey, scoreObject);
        }
        chain.insert("scores", scores);
        chain.insert("effects", chain.value("global").toObject().value("effects"));
        return;
    }
    if (trackIndex >= 0) trackObject.insert("track_index", trackIndex);
    // A configured track record is part of the current document topology.
    // Keep it marked present so reload/order readers can distinguish it from
    // stale records retained for a different document incarnation.
    trackObject.insert("present", true);
    if (!trackName.isEmpty()) trackObject.insert("track_name", trackName);
    trackObject.insert("effects", scopeObject.value("effects"));
    tracks.insert(trackKey, trackObject);
    scoreObject.insert("tracks", tracks);
    scores.insert(scoreKey, scoreObject);
    chain.insert("scores", scores);
    chain.insert("effects", chain.value("global").toObject().value("effects"));
}

bool reconcileTrackIdentities(std::vector<HostTrackIdentity> &bindings) {
    // Called only on the Qt control thread, alongside UI state writes.
    QStringList parts;
    for (const auto &binding : bindings)
        parts.append(binding.documentId + '\n' + binding.scoreKey + '\n' +
                     binding.trackId + '\n' + QString::number(binding.index));
    parts.removeDuplicates();
    parts.sort();
    const auto signature = parts.join('\t');
    if (signature != g_identitySignature) {
        QJsonObject chain;
        if (!loadChain(chain)) return false;
        auto sessions = g_documentTracks;
        auto locations = g_trackLocations;
        auto scores = chain.value("scores").toObject();
        QSet<QString> documents;
        for (const auto &binding : bindings) documents.insert(binding.documentId);
        for (const auto &documentId : documents) {
            if (documentId.isEmpty()) continue;
            const bool reopened = !sessions.contains(documentId);
            auto &session = sessions[documentId];
            const auto first = std::find_if(bindings.begin(), bindings.end(),
                [&](const HostTrackIdentity &binding) { return binding.documentId == documentId; });
            const auto scoreKey = first->scoreKey;
            if (!session.score.isEmpty() && session.score != scoreKey) {
                // Save As keeps the live identity and copies state to the new
                // score. The previous saved file retains its own record.
                scores.insert(scoreKey, scores.value(session.score));
            }
            session.score = scoreKey;
            auto score = scores.value(scoreKey).toObject();
            auto tracks = score.value("tracks").toObject();
            const auto previous = tracks;
            for (auto it = tracks.begin(); it != tracks.end(); ++it) {
                auto record = it.value().toObject();
                record.insert("present", false);
                it.value() = record;
            }
            for (const auto &binding : bindings) {
                if (binding.documentId != documentId || binding.trackId.isEmpty() || binding.index < 0) continue;
                auto key = session.persistentKeys.value(binding.trackId);
                if (key.isEmpty() && reopened) {
                    QStringList candidates;
                    for (auto it = previous.begin(); it != previous.end(); ++it) {
                        const auto record = it.value().toObject();
                        if (record.value("present").toBool(true) && record.value("track_index").toInt(-1) == binding.index)
                            candidates.append(it.key());
                    }
                    if (candidates.size() == 1 && !session.persistentKeys.values().contains(candidates.front()))
                        key = candidates.front();
                }
                if (key.isEmpty()) key = QStringLiteral("track-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
                session.persistentKeys.insert(binding.trackId, key);
                auto record = tracks.value(key).toObject();
                // New, unconfigured tracks only need a runtime key in this
                // process. Persisting an empty record for each score is what
                // previously made the sidecar grow without bound.
                if (!record.isEmpty()) {
                    record.insert("track_index", binding.index);
                    record.insert("present", true);
                    tracks.insert(key, record);
                }
                locations.insert(documentId + '#' + key, {scoreKey, key, binding.index});
            }
            if (tracks.isEmpty()) scores.remove(scoreKey);
            else {
                score.insert("tracks", tracks);
                scores.insert(scoreKey, score);
            }
        }
        chain.insert("scores", scores);
        if (!writeChain(chain)) return false;
        g_documentTracks = std::move(sessions);
        g_trackLocations = std::move(locations);
        g_identitySignature = signature;
    }
    for (auto &binding : bindings) {
        const auto key = g_documentTracks.value(binding.documentId).persistentKeys.value(binding.trackId);
        if (key.isEmpty()) return false;
        binding.runtimeKey = binding.documentId + '#' + key;
    }
    return true;
}

void resetTrackIdentities() {
    g_documentTracks.clear();
    g_trackLocations.clear();
    g_identitySignature.clear();
}

bool writeStatus(const QJsonObject &input) {
    QJsonObject status = input;
    status.insert("pid", QCoreApplication::applicationPid());
    status.insert("executable", QCoreApplication::applicationFilePath());
    status.insert("time", QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    g_statusWriter.start();
    g_statusWriter.submit(status);
    return true;
}

bool writeRealtimeObservation(const QJsonObject &hookStatus) {
    static std::atomic<quint64> generation{0};
    const QJsonObject status{
        {"schema", 1},
        {"gp_hook", hookStatus},
        {"generation", static_cast<qint64>(generation.fetch_add(1, std::memory_order_relaxed) + 1)},
        {"sample_mode", qEnvironmentVariable("GPVST3_DIAGNOSTIC_MODE") == QStringLiteral("detailed")
            ? QStringLiteral("detailed") : QStringLiteral("normal")},
        {"time", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}};
    g_observationWriter.start();
    g_observationWriter.submit(status);
    return true;
}

void startRealtimeObservationWriter() { g_observationWriter.start(); }
void stopRealtimeObservationWriter() noexcept { g_observationWriter.stop(); }
void startStatusWriter() { g_statusWriter.start(); }
void stopStatusWriter() noexcept { g_statusWriter.stop(); }

}
