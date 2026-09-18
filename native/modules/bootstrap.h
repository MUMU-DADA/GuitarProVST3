#pragma once

#include <QtCore/QJsonObject>

#include "host_lock.h"

namespace gpvst3::bootstrap {

QJsonObject initialize();
QJsonObject initialize(const host::Verification &verification);
void shutdown() noexcept;
bool pollVst3(QJsonObject &status);
QJsonObject hookSnapshot();

}
