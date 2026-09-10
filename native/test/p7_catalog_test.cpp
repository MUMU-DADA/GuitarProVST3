#include "vst3_host.h"
#include "state_manager.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>
#include <iostream>
#include <stdexcept>

using gpvst3::vst3::State;
namespace {
QJsonArray evidence;
void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}
void write(const QString &path, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(), "write fixture");
}
QByteArray read(const QString &path) { QFile file(path); require(file.open(QIODevice::ReadOnly), "read fixture"); return file.readAll(); }
QString binary(const QString &module) { return module + "/Contents/x86_64-win/" + QFileInfo(module).fileName(); }
void bundle(const QString &module) {
    QByteArray pe(128, '\0');
    pe.replace(0, 2, "MZ"); pe[60] = 64; pe.replace(64, 4, QByteArray("PE\0\0", 4)); pe[68] = 0x64; pe[69] = char(0x86);
    write(binary(module), pe);
}
QByteArray metadata(const QString &sub = "Fx") {
    return QJsonDocument(QJsonObject{{"Name", "Fixture"}, {"Classes", QJsonArray{
        QJsonObject{{"CID", "123456789ABCDEF0123456789ABCDEF0"}, {"Category", "Audio Module Class"},
                    {"Name", "Fixture Effect"}, {"Vendor", "Test"}, {"Sub Categories", QJsonArray{sub}}}}}}).toJson();
}
State scan(const char *name) {
    const auto first = gpvst3::vst3::beginAsync();
    require(gpvst3::vst3::beginAsync().scanGeneration == first.scanGeneration, "one concurrent task");
    State result;
    QElapsedTimer clock; clock.start();
    while (clock.elapsed() < 10000) {
        if (gpvst3::vst3::poll(result) && !result.scanPending) break;
        QThread::msleep(1);
    }
    require(!result.scanPending && result.staticScan, "static scan terminates");
    require(result.modulesLoaded == 0 && result.instancesCreated == 0 && result.processCalls == 0, "scan executes no plugin code");
    evidence.append(QJsonObject{{"case", name}, {"status", QString::fromStdString(result.status)},
        {"cache_hit", result.cacheHit}, {"cache_reused", result.cacheReused}, {"metadata_reads", result.metadataReads},
        {"files_checked", result.filesChecked}, {"catalog_count", int(result.catalog.size())},
        {"modules_loaded", result.modulesLoaded}, {"elapsed_ms", qint64(result.elapsedMs)}});
    return result;
}
}

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    try {
        QTemporaryDir directory;
        require(directory.isValid(), "temporary directory");
        const auto root = directory.path() + "/plugins";
        const auto data = directory.path() + "/data";
        qputenv("GPVST3_DATA_DIR", data.toUtf8());
        qputenv("GPVST3_VST3_ROOT", root.toUtf8());
        qunsetenv("GPVST3_VST3_PATHS");
        const auto one = root + "/One.vst3", two = root + "/Two.vst3";
        const auto info = one + "/Contents/Resources/moduleinfo.json";
        bundle(one); bundle(two); write(info, metadata());
        const auto cache = data + "/vst3-catalog-cache.json";
        const auto sidecar = data + "/effect-chain.json";
        const QByteArray settings = "saved opaque plugin parameters must survive discovery";
        write(sidecar, settings);
        auto cold = scan("cold");
        require(cold.catalog.size() == 2 && cold.metadataReads == 1 && !cold.cacheHit, "cold discovery");
        require(cold.catalog[0].identified && !cold.catalog[0].compatible &&
            cold.catalog[0].classId == "78563412BC9AF0DE123456789ABCDEF0", "static identity is not runtime compatibility");
        require(!cold.catalog[1].identified && cold.catalog[1].classId.empty(), "missing metadata preserves candidate");
        auto hot = scan("cache_hit");
        require(hot.cacheHit && hot.cacheReused == 2 && hot.metadataReads == 0, "cache reuses unchanged metadata");
        write(binary(one), read(binary(one)) + "update");
        require(scan("binary_update").cacheReused == 1, "binary fingerprint invalidated");
        write(info, "{ broken json");
        auto corrupt = scan("metadata_corrupt");
        require(corrupt.catalog.size() == 2 && !corrupt.catalog[0].identified, "invalid metadata stays pending");
        require(scan("retry_deferred").metadataReads == 0, "failed read is cached until retry");
        auto cacheObject = QJsonDocument::fromJson(read(cache)).object();
        auto scopes = cacheObject.value("scopes").toObject();
        for (auto scope = scopes.begin(); scope != scopes.end(); ++scope) {
            auto object = scope.value().toObject();
            auto modules = object.value("modules").toObject();
            for (auto module = modules.begin(); module != modules.end(); ++module) {
                auto record = module.value().toObject(); record.insert("retry_after", 0); module.value() = record;
            }
            object.insert("modules", modules); scope.value() = object;
        }
        cacheObject.insert("scopes", scopes); write(cache, QJsonDocument(cacheObject).toJson());
        require(scan("static_retry").metadataReads == 1, "due retry reads metadata only");
        write(info, metadata()); require(scan("metadata_repaired").metadataReads == 1, "file change retries immediately");
        require(QDir(two).removeRecursively(), "remove test bundle");
        require(scan("bundle_deleted").catalog.size() == 1, "deleted bundle removed from discovery");
        bundle(root + "/Three.vst3");
        require(scan("bundle_added").catalog.size() == 2, "new bundle discovered");
        write(cache, "[broken cache"); require(!scan("cache_corrupt").cacheHit, "corrupt cache rebuilt");
        write(cache, "{\"schema\":0}"); require(!scan("cache_old_schema").cacheHit, "old schema rebuilt");
        write(info, metadata("Instrument")); require(scan("instrument_filtered").catalog.size() == 1, "instrument is excluded");
        write(info, "{\"Name\":\"Fixture\",\"Classes\":\"unsupported\"}");
        require(scan("unsupported_metadata").catalog.size() == 2, "unsupported metadata preserved");
        write(info, metadata().replace("123456789ABCDEF0123456789ABCDEF0", "invalid"));
        require(!scan("invalid_class_uid").catalog[0].identified, "invalid CID rejected");
        require(QFile::remove(cache) && QDir().mkdir(cache), "make cache write failure");
        require(scan("cache_write_failed").cacheStatus == "write_failed", "write failure still returns results");
        require(QDir(cache).removeRecursively(), "remove test cache directory");
        write(info, metadata()); scan("cache_recovered");
        const auto empty = directory.path() + "/empty"; QDir().mkpath(empty);
        qputenv("GPVST3_VST3_ROOT", empty.toUtf8());
        require(scan("empty_scope").catalog.empty(), "empty directory has success terminal state");
        qputenv("GPVST3_VST3_ROOT", root.toUtf8());
        require(scan("scope_isolation").cacheHit, "one cache preserves separate root scopes");
        qputenv("GPVST3_VST3_ROOT", "//invalid-p7-root/vst3");
        require(scan("network_root_rejected_without_io").status == "scan_failed", "nonlocal roots rejected before IO");
        require(read(sidecar) == settings, "all cache paths preserve sidecar bytes");
        if (argc > 1) write(QString::fromLocal8Bit(argv[1]), QJsonDocument(QJsonObject{{"passed", true}, {"cases", evidence}}).toJson());
        std::cout << "PASS: static catalog, fingerprints, retries, cache failures and sidecar isolation.\n";
    } catch (const std::exception &error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
    return 0;
}
