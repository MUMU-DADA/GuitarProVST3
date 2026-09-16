#pragma once

#include <QtCore/QJsonObject>

namespace gpvst3::bootstrap {

QJsonObject initialize();
void shutdown() noexcept;
bool pollVst3(QJsonObject &status);
QJsonObject hookSnapshot();

}
