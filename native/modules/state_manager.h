#pragma once

#include <QtCore/QJsonObject>
#include <QtCore/QJsonArray>
#include <QtCore/QString>

namespace gpvst3::state {

enum class ScopeKind { Global, Track };
using ScoreKey = QString;
using TrackKey = QString;

constexpr int kSchema = 2;

QString dataDirectory();
QString sidecarPath();
bool loadChain(QJsonObject &chain, QString *error = nullptr);
bool writeChain(const QJsonObject &chain);
QJsonArray scopeEffects(const QJsonObject &chain, ScopeKind scope,
                        const ScoreKey &score = {}, const TrackKey &track = {});
void setScopeEffects(QJsonObject &chain, ScopeKind scope, const QJsonArray &effects,
                     const ScoreKey &score = {}, const TrackKey &track = {},
                     int trackIndex = -1, const QString &trackName = {});
QString currentScoreKey();
TrackKey currentTrackKey(const ScoreKey &score = {});
bool writeStatus(const QJsonObject &status);
bool writeRealtimeObservation(const QJsonObject &hookStatus);

}
