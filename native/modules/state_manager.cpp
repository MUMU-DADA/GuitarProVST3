#include "state_manager.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QJsonDocument>
#include <QtCore/QSaveFile>
#include <QtCore/QStandardPaths>

namespace gpvst3::state {

QString dataDirectory() {
    const QString configured = qEnvironmentVariable("GPVST3_DATA_DIR");
    if (!configured.isEmpty()) return configured;
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/GuitarProVST3";
}

bool writeStatus(const QJsonObject &input) {
    QJsonObject status = input;
    status.insert("pid", QCoreApplication::applicationPid());
    status.insert("executable", QCoreApplication::applicationFilePath());
    status.insert("time", QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    const QDir directory(dataDirectory());
    if (!QDir().mkpath(directory.absolutePath())) return false;
    QSaveFile file(directory.filePath("status.json"));
    if (!file.open(QIODevice::WriteOnly)) return false;
    const QByteArray bytes = QJsonDocument(status).toJson(QJsonDocument::Indented);
    return file.write(bytes) == bytes.size() && file.commit();
}

}
