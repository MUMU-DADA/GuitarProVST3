#include "state_manager.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QSaveFile>
#include <QtCore/QStandardPaths>

namespace gpvst3::state {
namespace {

constexpr int kSchema = 1;

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
                       {"bus", bus}, {"effects", QJsonArray{}}};
}

}

QString dataDirectory() {
    const QString configured = qEnvironmentVariable("GPVST3_DATA_DIR");
    if (!configured.isEmpty()) return configured;
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/GuitarProVST3";
}

QString sidecarPath() { return QDir(dataDirectory()).filePath(QStringLiteral("effect-chain.json")); }

bool loadChain(QJsonObject &chain, QString *error) {
    QFile file(sidecarPath());
    if (!file.exists()) { chain = emptyChain(); return true; }
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = file.errorString();
        chain = emptyChain();
        return false;
    }
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error) *error = parseError.errorString();
        chain = emptyChain();
        return false;
    }
    chain = document.object();
    if (chain.value("schema").toInt() != kSchema || !chain.value("effects").isArray()) {
        if (error) *error = QStringLiteral("unsupported_sidecar_schema");
        chain = emptyChain();
        return false;
    }
    return true;
}

bool writeChain(const QJsonObject &input) {
    QJsonObject chain = input;
    chain.insert("schema", kSchema);
    if (!chain.value("effects").isArray()) chain.insert("effects", QJsonArray{});
    chain.insert("saved_at", QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    return writeJson(sidecarPath(), chain);
}

bool writeStatus(const QJsonObject &input) {
    QJsonObject status = input;
    status.insert("pid", QCoreApplication::applicationPid());
    status.insert("executable", QCoreApplication::applicationFilePath());
    status.insert("time", QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    return writeJson(QDir(dataDirectory()).filePath(QStringLiteral("status.json")), status);
}

bool writeRealtimeObservation(const QJsonObject &hookStatus) {
    const QJsonObject status{
        {"schema", 1},
        {"gp_hook", hookStatus},
        {"time", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}};
    return writeJson(QDir(dataDirectory()).filePath(QStringLiteral("p2-observation.json")), status);
}

}
