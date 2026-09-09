#pragma once

#include <QtCore/QJsonObject>
#include <QtCore/QString>

namespace gpvst3::state {

QString dataDirectory();
QString sidecarPath();
bool loadChain(QJsonObject &chain, QString *error = nullptr);
bool writeChain(const QJsonObject &chain);
bool writeStatus(const QJsonObject &status);
bool writeRealtimeObservation(const QJsonObject &hookStatus);

}
