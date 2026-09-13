#pragma once

#include <QtCore/QJsonObject>
#include <QtCore/QJsonArray>
#include <QtCore/QString>
#include <vector>

namespace gpvst3::state {

enum class ScopeKind { Global, Track };
using ScoreKey = QString;
using TrackKey = QString;

constexpr int kSchema = 2;

QString dataDirectory();
QString sidecarPath();
QString settingsPath();
bool pluginEnabled();
bool setPluginEnabled(bool enabled);
// Clear persisted effect activation flags for a new host process. Processor
// instances are always opt-in for the current session; plugin state bytes and
// catalog identities remain intact for the user to re-enable explicitly.
bool disableAllEffectsAtStartup();
bool loadChain(QJsonObject &chain, QString *error = nullptr);
bool writeChain(const QJsonObject &chain);
QJsonArray scopeEffects(const QJsonObject &chain, ScopeKind scope,
                        const ScoreKey &score = {}, const TrackKey &track = {});
void setScopeEffects(QJsonObject &chain, ScopeKind scope, const QJsonArray &effects,
                     const ScoreKey &score = {}, const TrackKey &track = {},
                     int trackIndex = -1, const QString &trackName = {});
QString currentScoreKey();
TrackKey currentTrackKey(const ScoreKey &score = {});
// Runtime context published by the verified GuitarProMCP bridge. The
// environment variables remain a fixture fallback when no host document is
// connected.
void setRuntimeTrackContext(const ScoreKey &score, const TrackKey &track,
                            int trackIndex, const QString &trackId);
void clearRuntimeTrackContext();
bool runtimeTrackContextAvailable();
int runtimeTrackIndex();
QString runtimeTrackId();
struct HostTrackIdentity {
    QString documentId, scoreKey, trackId;
    int index = -1;
    TrackKey runtimeKey;
};
// Resolve session identities to sidecar records. Index is used only when a
// document is first opened; subsequent inserts, moves and undo follow trackId.
bool reconcileTrackIdentities(std::vector<HostTrackIdentity> &tracks);
void resetTrackIdentities();
bool writeStatus(const QJsonObject &status);
// Scan/status updates are coalesced and committed away from the Qt event
// thread. The synchronous writeStatus() remains for bootstrap initialization
// and explicit shutdown compatibility.
void startStatusWriter();
bool submitStatus(const QJsonObject &status);
void stopStatusWriter();
void startRealtimeObservationWriter();
bool writeRealtimeObservation(const QJsonObject &hookStatus);
void stopRealtimeObservationWriter();

}
