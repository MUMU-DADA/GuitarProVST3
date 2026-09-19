#pragma once

#include <QtCore/QJsonObject>
#include <QtCore/QJsonArray>
#include <QtCore/QString>
#include <vector>
#include <cstdint>

namespace gpvst3::state {

enum class ScopeKind { Global, Track, Input };
enum class InputMonitorMode { Off, Legacy, LowLatencyOverlay };
struct InputMonitorSettings {
    // This is requested configuration only. A device session owns the actual
    // mode, processor instances and generation; none of those are persisted.
    InputMonitorMode mode = InputMonitorMode::Off;
    double gain = 0.5;
};
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
bool migrateDesiredEnabledIntent();
bool loadChain(QJsonObject &chain, QString *error = nullptr);
bool writeChain(const QJsonObject &chain);
// Missing input/settings fields use Off and 0.5. Invalid persisted values
// return false, reset settings to those safe defaults and optionally set error.
// Both helpers are pure JSON operations; neither activates a monitor or saves.
bool readInputMonitorSettings(const QJsonObject &chain, InputMonitorSettings &settings,
                              QString *error = nullptr);
// Reject invalid modes, non-finite gains and gains outside [0, 4] atomically.
// Existing input metadata, effects and unknown fields remain intact.
bool setInputMonitorSettings(QJsonObject &chain, const InputMonitorSettings &settings,
                             QString *error = nullptr);
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
std::uint64_t runtimeSelectionGeneration();
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
bool writeRealtimeObservation(const QJsonObject &hookStatus);
void startRealtimeObservationWriter();
void stopRealtimeObservationWriter() noexcept;
void startStatusWriter();
void stopStatusWriter() noexcept;

}
