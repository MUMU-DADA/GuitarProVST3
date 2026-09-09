#pragma once

#include <QtCore/QJsonObject>

namespace gpvst3::state {

QString dataDirectory();
bool writeStatus(const QJsonObject &status);
bool writeRealtimeObservation(const QJsonObject &hookStatus);

}
