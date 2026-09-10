#pragma once

#include <QtCore/QJsonObject>

namespace gpvst3::bootstrap {

QJsonObject initialize();
bool pollVst3(QJsonObject &status);
QJsonObject hookSnapshot();

}
