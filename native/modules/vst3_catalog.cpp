// Static discovery only: this translation unit has no module loader, VST3
// factory, process launcher or networking dependency.
#include "vst3_host.h"
#include "state_manager.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>
#include <QtCore/QSaveFile>
#include <QtCore/QSet>
#include <QtCore/QtEndian>
#ifdef GPVST3_TEST_SCAN_DELAY_MS
#include <QtCore/QThread>
#endif
#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <thread>
#include <mutex>

namespace gpvst3::vst3 {
namespace {
constexpr int kSchema = 1;
constexpr int kScanner = 1;
std::mutex scanMutex;
std::atomic<bool> stopping{false};
State snapshot;
// Destroy/join the worker before the snapshot it publishes into.
std::future<State> scanFuture;
struct RecognitionJob {
    QString module;
    std::chrono::steady_clock::time_point started;
    std::atomic<bool> finished{false};
    std::atomic<bool> timedOut{false};
    std::mutex resultMutex;
    State result;
};
std::shared_ptr<RecognitionJob> recognitionJob;
QStringList recognitionQueue;
QString recognitionModule;
RecognitionControl recognitionControl = nullptr;
constexpr std::chrono::seconds kRecognitionTimeout{10};
unsigned revision = 0, delivered = 0;
int generation = 0;
int recognitionWorkersStarted = 0;
int recognitionWorkersDetached = 0;

QString normalized(const QString &path) {
    const auto clean = QDir::fromNativeSeparators(path);
    if (clean.startsWith("//")) return QDir::cleanPath(clean).toLower();
    const QFileInfo file(path);
    const auto canonical = file.isSymLink() ? QString{} : file.canonicalFilePath();
    return QDir::cleanPath(canonical.isEmpty() ? file.absoluteFilePath() : canonical).toLower();
}

QStringList roots() {
    QString paths = qEnvironmentVariable("GPVST3_VST3_PATHS");
    if (paths.isEmpty()) paths = qEnvironmentVariable("GPVST3_VST3_ROOT");
    QStringList result;
    if (!paths.isEmpty()) result = paths.split(';', Qt::SkipEmptyParts);
    else {
        for (const auto *base : {"ProgramW6432", "ProgramFiles", "ProgramFiles(x86)"}) {
            const auto value = qEnvironmentVariable(base);
            if (!value.isEmpty()) result.append(value + "/Common Files/VST3");
        }
        const auto local = qEnvironmentVariable("LOCALAPPDATA");
        if (!local.isEmpty()) result.append(local + "/Programs/Common/VST3");
    }
    for (auto &path : result) path = normalized(path);
    result.removeDuplicates();
    result.sort();
    return result;
}

QString cachePath() { return QDir(state::dataDirectory()).filePath("vst3-catalog-cache.json"); }
QString scopeKey(const QStringList &paths) {
    return QString::fromLatin1(QCryptographicHash::hash(paths.join('\n').toUtf8(), QCryptographicHash::Sha256).toHex());
}

QJsonObject readCache(QString &status) {
    if (QDir::fromNativeSeparators(cachePath()).startsWith("//")) { status = "network_cache_rejected"; return {}; }
    QFile file(cachePath());
    if (!file.exists()) { status = "missing"; return {}; }
    if (!file.open(QIODevice::ReadOnly)) { status = "read_failed"; return {}; }
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(file.read(16 * 1024 * 1024 + 1), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) { status = "corrupt"; return {}; }
    const auto object = doc.object();
    if (object.value("schema").toInt() != kSchema || object.value("scanner").toInt() != kScanner ||
        object.value("architecture").toString() != "x64" || !object.value("scopes").isObject()) {
        status = "incompatible";
        return {};
    }
    status = "hit";
    return object;
}

bool inRoots(const QString &path, const QStringList &paths) {
    if (path.startsWith("//") || !QDir::isAbsolutePath(path)) return false;
    for (const auto &root : paths)
        if (path == root || path.startsWith(root + '/')) return true;
    return false;
}

bool validUid(const QString &value) {
    static const QRegularExpression uid("^[0-9A-F]{32}$");
    return uid.match(value).hasMatch() && value != QString(32, '0');
}

CatalogEntry entryFrom(const QString &module, const QJsonObject &value) {
    CatalogEntry entry;
    entry.module = module.toStdString();
    entry.classId = value.value("class_id").toString().toStdString();
    entry.name = value.value("name").toString().toStdString();
    entry.vendor = value.value("vendor").toString().toStdString();
    entry.category = value.value("category").toString().toStdString();
    entry.identified = value.value("identified").toBool();
    entry.source = value.value("source").toString().toStdString();
    entry.error = value.value("error").toString().toStdString();
    entry.recognitionStatus = value.value("recognition_status").toString().toStdString();
    entry.recognitionSource = value.value("recognition_source").toString().toStdString();
    entry.recognitionAttempts = value.value("recognition_attempts").toInt();
    entry.recognitionError = value.value("recognition_error").toString().toStdString();
    entry.recognitionRetryAfter = static_cast<long long>(value.value("recognition_retry_after").toDouble());
    entry.recognitionDeadlineAt = static_cast<long long>(value.value("recognition_deadline_at").toDouble());
    entry.recognitionIgnoredReason = value.value("recognition_ignored_reason").toString().toStdString();
    return entry;
}

bool validEntries(const QJsonArray &entries) {
    QSet<QString> seen;
    for (const auto &value : entries) {
        if (!value.isObject()) return false;
        const auto entry = value.toObject();
        const auto uid = entry.value("class_id").toString();
        if (entry.value("name").toString().isEmpty() || seen.contains(uid)) return false;
        seen.insert(uid);
        if (entry.value("identified").toBool()) {
            if (!validUid(uid) || entry.value("category").toString() != "Audio Module Class" ||
                (entry.value("source").toString() != "moduleinfo" &&
                 entry.value("source").toString() != "factory")) return false;
        } else if (!uid.isEmpty()) return false;
    }
    return true;
}

void addEntries(State &result, const QString &module, const QJsonArray &entries) {
    for (const auto &value : entries) result.catalog.push_back(entryFrom(module, value.toObject()));
}

void discover(const QString &path, QStringList &modules, QSet<QString> &visited, State &result) {
    if (stopping.load()) return;
    if (path.startsWith("//")) { result.errors.push_back("network_root_rejected"); return; }
    const QFileInfo info(path);
    if (info.isSymLink()) return;
    if (!info.exists()) return;
    const auto key = normalized(path);
    if (key.startsWith("//") || visited.contains(key) || info.isSymLink()) return;
    visited.insert(key);
    if (!info.isReadable()) { result.errors.push_back((path + ":read_failed").toStdString()); return; }
    if (info.suffix().compare("vst3", Qt::CaseInsensitive) == 0) { modules.append(key); return; }
    if (!info.isDir()) return;
    const QDir dir(path);
    for (const auto &child : dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks))
        discover(child.absoluteFilePath(), modules, visited, result);
}

QByteArray fingerprint(const QString &module, State &result) {
    QStringList files;
    if (QFileInfo(module).isDir()) {
        QDirIterator iterator(module, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks,
                              QDirIterator::Subdirectories);
        while (iterator.hasNext()) files.append(iterator.next());
    } else files.append(module);
    files.sort();
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (const auto &path : files) {
        if (stopping.load()) break;
        const QFileInfo info(path);
        const auto record = QDir(module).relativeFilePath(path) + '\n' + QString::number(info.size()) + '\n' +
                            QString::number(info.lastModified().toMSecsSinceEpoch()) + '\n';
        hash.addData(record.toUtf8());
        ++result.filesChecked;
        // Include metadata bytes even when a writer preserves size and mtime.
        if (info.fileName().compare("moduleinfo.json", Qt::CaseInsensitive) == 0) {
            QFile metadata(path);
            if (metadata.open(QIODevice::ReadOnly)) hash.addData(metadata.read(4 * 1024 * 1024 + 1));
            else hash.addData("unreadable");
        }
    }
    return hash.result().toHex();
}

QString architecture(const QString &module) {
    const QFileInfo info(module);
    if (info.isDir() && (QFileInfo(module + "/Contents").isSymLink() ||
                        QFileInfo(module + "/Contents/x86_64-win").isSymLink())) return "binary_symlink_rejected";
    const QString binary = info.isDir() ? module + "/Contents/x86_64-win/" + info.fileName() : module;
    if (QFileInfo(binary).isSymLink()) return "binary_symlink_rejected";
    QFile file(binary);
    if (!file.open(QIODevice::ReadOnly)) return "binary_read_failed";
    const auto dos = file.read(64);
    if (dos.size() != 64 || dos.left(2) != "MZ") return "invalid_pe";
    const auto offset = qFromLittleEndian<quint32>(dos.constData() + 60);
    if (offset > file.size() - 6 || !file.seek(offset)) return "invalid_pe";
    const auto pe = file.read(6);
    if (pe.size() != 6 || pe.left(4) != QByteArray("PE\0\0", 4)) return "invalid_pe";
    return qFromLittleEndian<quint16>(pe.constData() + 4) == 0x8664 ? QString{} : "architecture_not_x64";
}

// SDK moduleinfo uses FUID text; runtime identities use the Windows TUID bytes.
QString windowsUid(const QString &cid) {
    const auto bytes = QByteArray::fromHex(cid.toLatin1());
    QByteArray result = bytes;
    std::reverse(result.begin(), result.begin() + 4);
    std::reverse(result.begin() + 4, result.begin() + 6);
    std::reverse(result.begin() + 6, result.begin() + 8);
    return QString::fromLatin1(result.toHex().toUpper());
}

QJsonArray inspect(const QString &module, State &result, QString &error) {
    error = architecture(module);
    QJsonArray entries;
    if (error.isEmpty()) {
        QFile file(module + "/Contents/Resources/moduleinfo.json");
        if (QFileInfo(module + "/Contents/Resources").isSymLink() || QFileInfo(file).isSymLink()) error = "metadata_symlink_rejected";
        else if (!file.exists()) error = "metadata_missing";
        else if (!file.open(QIODevice::ReadOnly)) error = "metadata_read_failed";
        else {
            ++result.metadataReads;
            QJsonParseError parse;
            const auto doc = QJsonDocument::fromJson(file.read(4 * 1024 * 1024 + 1), &parse);
            const auto object = doc.object();
            if (parse.error != QJsonParseError::NoError || !doc.isObject()) error = "metadata_invalid_json";
            else if (object.value("Name").toString().isEmpty() || !object.value("Classes").isArray() ||
                     object.value("Classes").toArray().isEmpty()) error = "metadata_unsupported";
            else {
                QSet<QString> seen;
                for (const auto &value : object.value("Classes").toArray()) {
                    const auto item = value.toObject();
                    const auto cid = item.value("CID").toString().toUpper();
                    if (!validUid(cid) || seen.contains(cid) || item.value("Name").toString().isEmpty() ||
                        !item.value("Category").isString()) { error = "metadata_invalid_class"; break; }
                    seen.insert(cid);
                    if (item.value("Category").toString() != "Audio Module Class") continue;
                    if (!item.value("Sub Categories").isArray()) { error = "metadata_unsupported"; break; }
                    QStringList subcategories;
                    for (const auto &sub : item.value("Sub Categories").toArray()) subcategories.append(sub.toString());
                    if (subcategories.contains("Instrument")) continue;
                    if (!subcategories.contains("Fx")) { error = "metadata_unknown_category"; break; }
                    entries.append(QJsonObject{{"class_id", windowsUid(cid)}, {"name", item.value("Name")},
                        {"vendor", item.value("Vendor").toString(object.value("Factory Info").toObject().value("Vendor").toString())},
                        {"category", "Audio Module Class"}, {"identified", true}, {"source", "moduleinfo"}});
                }
            }
        }
    }
    if (!error.isEmpty()) {
        entries = QJsonArray{QJsonObject{{"class_id", ""}, {"name", QFileInfo(module).completeBaseName()},
            {"identified", false}, {"source", "file"}, {"error", error}}};
    }
    return entries;
}

void publish(const State &state) {
    std::lock_guard<std::mutex> lock(scanMutex);
    snapshot = state;
    ++revision;
}

void persistRecognition(const QString &module, const State &identified) {
    const auto path = cachePath();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return;
    QJsonParseError parse{};
    auto document = QJsonDocument::fromJson(file.readAll(), &parse);
    file.close();
    if (parse.error != QJsonParseError::NoError || !document.isObject()) return;
    auto cache = document.object();
    auto scopes = cache.value("scopes").toObject();
    std::vector<CatalogEntry> recognized = identified.catalog;
    if (recognized.empty()) {
        for (const auto &item : identified.classes) {
            if (item.category != "Audio Module Class") continue;
            recognized.push_back(CatalogEntry{item.module, item.classId, item.name, item.vendor,
                item.category, item.processorReady, item.error, item.effectIdentified || item.processorReady,
                "factory"});
        }
    }
    QJsonArray jsonEntries;
    for (const auto &entry : recognized) {
        if (entry.module != module.toStdString() || !entry.identified ||
            entry.category != "Audio Module Class") continue;
        jsonEntries.append(QJsonObject{{"class_id", QString::fromStdString(entry.classId)},
            {"name", QString::fromStdString(entry.name)}, {"vendor", QString::fromStdString(entry.vendor)},
            {"category", QString::fromStdString(entry.category)}, {"identified", true}, {"source", "factory"}});
    }
    for (auto scope = scopes.begin(); scope != scopes.end(); ++scope) {
        auto scopeObject = scope.value().toObject();
        auto modules = scopeObject.value("modules").toObject();
        auto record = modules.value(module).toObject();
        if (record.isEmpty()) continue;
        record.insert("recognition_status", jsonEntries.isEmpty() ? "failed" : "ready");
        record.insert("recognition_source", "factory");
        record.insert("recognition_attempts", record.value("recognition_attempts").toInt() + 1);
        record.insert("recognition_error", jsonEntries.isEmpty() && !identified.errors.empty()
                     ? QString::fromStdString(identified.errors.front()) : QString{});
        record.insert("recognition_retry_after", jsonEntries.isEmpty()
                     ? static_cast<double>(QDateTime::currentSecsSinceEpoch() + 60) : 0.0);
        if (!jsonEntries.isEmpty()) record.insert("entries", jsonEntries);
        modules.insert(module, record);
        scopeObject.insert("modules", modules);
        scopes.insert(scope.key(), scopeObject);
    }
    cache.insert("scopes", scopes);
    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly)) return;
    const auto bytes = QJsonDocument(cache).toJson();
    if (output.write(bytes) != bytes.size()) return;
    output.commit();
}

void startNextRecognition(bool hostSupported) {
    if (!hostSupported || recognitionJob || recognitionQueue.isEmpty() || stopping.load()) return;
    recognitionModule = recognitionQueue.takeFirst();
    auto job = std::make_shared<RecognitionJob>();
    job->module = recognitionModule;
    job->started = std::chrono::steady_clock::now();
    recognitionJob = job;
    ++recognitionWorkersStarted;
    const auto control = recognitionControl;
    std::thread([job, hostSupported, control] {
        State result;
        if (stopping.load(std::memory_order_acquire)) {
            result.status = "scan_stopped";
            result.errors.push_back("recognition_stopped");
        } else if (!control) {
            result.status = "recognition_unavailable";
            result.errors.push_back("recognition_control_unavailable");
        } else {
            result = control(job->module.toStdString(), hostSupported);
        }
        {
            std::lock_guard<std::mutex> lock(job->resultMutex);
            job->result = std::move(result);
        }
        job->finished.store(true, std::memory_order_release);
    }).detach();
}

void persistRecognitionTimeout(const QString &module, long long deadline, const char *reason) {
    const auto path = cachePath();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return;
    QJsonParseError parse{};
    auto document = QJsonDocument::fromJson(file.readAll(), &parse);
    file.close();
    if (parse.error != QJsonParseError::NoError || !document.isObject()) return;
    auto cache = document.object();
    auto scopes = cache.value("scopes").toObject();
    for (auto scope = scopes.begin(); scope != scopes.end(); ++scope) {
        auto scopeObject = scope.value().toObject();
        auto modules = scopeObject.value("modules").toObject();
        auto record = modules.value(module).toObject();
        if (record.isEmpty()) continue;
        record.insert("recognition_status", "timeout");
        record.insert("recognition_source", "factory");
        record.insert("recognition_attempts", record.value("recognition_attempts").toInt() + 1);
        record.insert("recognition_error", reason);
        record.insert("recognition_deadline_at", static_cast<double>(deadline));
        record.insert("recognition_ignored_reason", reason);
        record.insert("recognition_retry_after", 0.0);
        modules.insert(module, record);
        scopeObject.insert("modules", modules);
        scopes.insert(scope.key(), scopeObject);
    }
    cache.insert("scopes", scopes);
    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly)) return;
    const auto bytes = QJsonDocument(cache).toJson();
    if (output.write(bytes) == bytes.size()) output.commit();
}

State scan(const QStringList &paths, QJsonObject cache, State result, bool retryTimedOut) {
    QElapsedTimer elapsed;
    elapsed.start();
    const auto scope = scopeKey(paths);
    const auto old = cache.value("scopes").toObject().value(scope).toObject().value("modules").toObject();
    QStringList modules;
    QSet<QString> visited;
    for (const auto &path : paths) discover(path, modules, visited, result);
    modules.sort();
    result.modulesDiscovered = modules.size();
    publish(result);
    QJsonObject records;
    std::vector<CatalogEntry> found;
    const auto now = QDateTime::currentSecsSinceEpoch();
    for (const auto &module : modules) {
        if (stopping.load()) break;
#ifdef GPVST3_TEST_SCAN_DELAY_MS
        // Only the separate regression build uses a deterministic slow file
        // check to observe pending UI through MCP. Release builds omit it.
        QThread::msleep(GPVST3_TEST_SCAN_DELAY_MS);
#endif
        if (stopping.load()) break;
        result.currentModule = QFileInfo(module).fileName().toStdString();
        const auto stamp = QString::fromLatin1(fingerprint(module, result));
        auto record = old.value(module).toObject();
        QString error = record.value("error").toString();
        if (record.value("fingerprint").toString() == stamp && record.value("entries").isArray() &&
            validEntries(record.value("entries").toArray()) &&
            (error.isEmpty() || now < record.value("retry_after").toDouble() ||
             record.value("recognition_status").toString() == "timeout" ||
             (record.value("recognition_status").toString() == "ready" &&
              record.value("recognition_source").toString() == "factory"))) {
            ++result.cacheReused;
        } else {
            const auto entries = inspect(module, result, error);
            record = QJsonObject{{"fingerprint", stamp}, {"entries", entries}, {"error", error},
                                 {"retry_after", error.isEmpty() ? 0.0 : static_cast<double>(now + 60)},
                                 {"recognition_status", error.isEmpty() && !entries.isEmpty() &&
                                      entries.first().toObject().value("identified").toBool() ? "ready" : "queued"},
                                 {"recognition_source", "static"}, {"recognition_attempts", 0},
                                 {"recognition_error", QString{}}, {"recognition_retry_after", 0.0},
                                 {"recognition_deadline_at", 0.0}, {"recognition_ignored_reason", QString{}},
                                 {"recognition_scanner_version", kScanner}};
        }
        if (retryTimedOut && record.value("recognition_status").toString() == "timeout") {
            record.insert("recognition_status", "queued");
            record.insert("recognition_error", QString{});
            record.insert("recognition_ignored_reason", QString{});
            record.insert("recognition_deadline_at", 0.0);
        }
        if (!record.contains("recognition_status")) {
            const auto cachedEntries = record.value("entries").toArray();
            const bool identified = !cachedEntries.isEmpty() &&
                cachedEntries.first().toObject().value("identified").toBool();
            record.insert("recognition_status", identified ? "ready" : "queued");
        }
        if (!record.contains("recognition_source")) record.insert("recognition_source", "static");
        if (!record.contains("recognition_attempts")) record.insert("recognition_attempts", 0);
        if (!record.contains("recognition_error")) record.insert("recognition_error", QString{});
        if (!record.contains("recognition_retry_after")) record.insert("recognition_retry_after", 0.0);
        if (!record.contains("recognition_deadline_at")) record.insert("recognition_deadline_at", 0.0);
        if (!record.contains("recognition_ignored_reason")) record.insert("recognition_ignored_reason", QString{});
        if (!record.contains("recognition_scanner_version")) record.insert("recognition_scanner_version", kScanner);
        auto entryValues = record.value("entries").toArray();
        for (int entryIndex = 0; entryIndex < entryValues.size(); ++entryIndex) {
            auto entry = entryValues.at(entryIndex).toObject();
            entry.insert("recognition_status", record.value("recognition_status"));
            entry.insert("recognition_source", record.value("recognition_source"));
            entry.insert("recognition_attempts", record.value("recognition_attempts"));
            entry.insert("recognition_error", record.value("recognition_error"));
            entry.insert("recognition_retry_after", record.value("recognition_retry_after"));
            entry.insert("recognition_deadline_at", record.value("recognition_deadline_at"));
            entry.insert("recognition_ignored_reason", record.value("recognition_ignored_reason"));
            entryValues.replace(entryIndex, entry);
        }
        record.insert("entries", entryValues);
        if (!error.isEmpty() && error != "metadata_missing") result.errors.push_back((module + ':' + error).toStdString());
        records.insert(module, record);
        for (const auto &entry : record.value("entries").toArray()) found.push_back(entryFrom(module, entry.toObject()));
        result.catalog.erase(std::remove_if(result.catalog.begin(), result.catalog.end(), [&](const CatalogEntry &entry) {
            return entry.module == module.toStdString();
        }), result.catalog.end());
        addEntries(result, module, record.value("entries").toArray());
        ++result.modulesChecked;
        result.elapsedMs = elapsed.elapsed();
        publish(result);
    }
    result.catalog = std::move(found);
    if (stopping.load()) {
        result.status = "scan_cancelled";
        result.scanPending = false;
        return result;
    }
    std::sort(result.catalog.begin(), result.catalog.end(), [](const CatalogEntry &a, const CatalogEntry &b) {
        if (a.name != b.name) return a.name < b.name;
        return a.module < b.module;
    });
    auto scopes = cache.value("scopes").toObject();
    scopes.insert(scope, QJsonObject{{"roots", QJsonArray::fromStringList(paths)}, {"modules", records}});
    cache = QJsonObject{{"schema", kSchema}, {"scanner", kScanner}, {"architecture", "x64"}, {"scopes", scopes}};
    const bool localCache = !QDir::fromNativeSeparators(cachePath()).startsWith("//");
    if (localCache) QDir().mkpath(state::dataDirectory());
    QSaveFile file(cachePath());
    const auto bytes = QJsonDocument(cache).toJson();
    if (!localCache || !file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        result.cacheStatus = "write_failed";
        result.errors.push_back("cache_write_failed");
    }
    result.scanPending = false;
    result.ready = true;
    result.status = result.errors.empty() ? "catalog_ready" : "partial_failure";
    if (result.catalog.empty() && !result.errors.empty()) result.status = "scan_failed";
    result.elapsedMs = elapsed.elapsed();
    result.currentModule.clear();
    return result;
}
} // namespace

State beginAsync(bool hostSupported, bool retryTimedOut) noexcept {
    std::lock_guard<std::mutex> lock(scanMutex);
    if (stopping.load()) { State state; state.status = "scan_stopped"; return state; }
    if (!hostSupported) { State state; state.status = "host_unsupported"; return state; }
    if (scanFuture.valid() || recognitionJob || !recognitionQueue.isEmpty()) return snapshot;
    const auto paths = roots();
    QString cacheStatus;
    const auto cache = readCache(cacheStatus);
    const auto scope = cache.value("scopes").toObject().value(scopeKey(paths)).toObject();
    State pending;
    pending.staticScan = true;
    pending.hostSupported = hostSupported;
    pending.scanPending = true;
    pending.workerThread = true;
    pending.status = "scanning";
    pending.scanGeneration = ++generation;
    pending.cacheStatus = cacheStatus.toStdString();
    if (scope.value("roots").toArray() == QJsonArray::fromStringList(paths) && scope.value("modules").isObject()) {
        const auto modules = scope.value("modules").toObject();
        pending.cacheHit = true;
        for (auto it = modules.begin(); it != modules.end(); ++it) {
            const auto entries = it.value().toObject().value("entries").toArray();
            if (!inRoots(it.key(), paths) || !it.key().endsWith(".vst3") || !validEntries(entries)) {
                pending.cacheHit = false; pending.catalog.clear(); pending.cacheStatus = "invalid_entries"; break;
            }
            addEntries(pending, it.key(), entries);
        }
    }
    snapshot = pending;
    ++revision;
    scanFuture = std::async(std::launch::async, [paths, cache, pending, retryTimedOut] {
        try { return scan(paths, cache, pending, retryTimedOut); }
        catch (...) {
            auto failed = pending;
            failed.status = "scan_failed";
            failed.scanPending = false;
            failed.errors.push_back("static_scan_exception");
            return failed;
        }
    });
    return pending;
}

bool poll(State &completed) noexcept {
    std::lock_guard<std::mutex> lock(scanMutex);
    if (scanFuture.valid() && scanFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
        snapshot = scanFuture.get();
        recognitionQueue.clear();
        QSet<QString> prioritized;
        QJsonObject sidecar;
        if (state::loadChain(sidecar)) {
            const auto mark = [&](const QJsonArray &effects) {
                for (const auto &value : effects) {
                    const auto effect = value.toObject();
                    if (!effect.value("enabled").toBool()) continue;
                    const auto module = effect.value("module").toString();
                    if (!module.isEmpty()) prioritized.insert(module);
                }
            };
            mark(sidecar.value("global").toObject().value("effects").toArray());
            const auto scores = sidecar.value("scores").toObject();
            for (auto score = scores.begin(); score != scores.end(); ++score) {
                const auto tracks = score.value().toObject().value("tracks").toObject();
                for (auto track = tracks.begin(); track != tracks.end(); ++track)
                    mark(track.value().toObject().value("effects").toArray());
            }
        }
        // Recognition is an injected host capability. Standalone catalog
        // fixtures and unsupported hosts keep the P7 static-scan contract and
        // never manufacture an unavailable worker task.
        if (recognitionControl) {
            for (const auto &entry : snapshot.catalog)
                if (!entry.identified && !entry.module.empty() &&
                    ((entry.recognitionStatus != "failed" && entry.recognitionStatus != "timeout") ||
                     (entry.recognitionStatus == "failed" &&
                      QDateTime::currentSecsSinceEpoch() >= entry.recognitionRetryAfter)) &&
                    std::find(recognitionQueue.cbegin(), recognitionQueue.cend(),
                              QString::fromStdString(entry.module)) == recognitionQueue.cend())
                    recognitionQueue.append(QString::fromStdString(entry.module));
            std::stable_sort(recognitionQueue.begin(), recognitionQueue.end(),
                             [&](const QString &left, const QString &right) {
                const bool lp = prioritized.contains(left), rp = prioritized.contains(right);
                return lp != rp ? lp > rp : left < right;
            });
        } else {
            recognitionQueue.clear();
        }
        snapshot.recognitionPending = !recognitionQueue.isEmpty();
        snapshot.recognitionStatus = snapshot.recognitionPending ? "queued" : "idle";
        startNextRecognition(snapshot.hostSupported);
        snapshot.recognitionWorker = static_cast<bool>(recognitionJob);
        ++revision;
    }
    if (recognitionJob && recognitionJob->finished.load(std::memory_order_acquire)) {
        auto job = recognitionJob;
        State identified;
        {
            std::lock_guard<std::mutex> resultLock(job->resultMutex);
            identified = job->result;
        }
        recognitionJob.reset();
        persistRecognition(recognitionModule, identified);
        std::vector<CatalogEntry> replacement = identified.catalog;
        if (replacement.empty()) {
            for (const auto &item : identified.classes) {
                if (item.category != "Audio Module Class" ||
                    (!item.effectIdentified && !item.processorReady)) continue;
                replacement.push_back(CatalogEntry{item.module, item.classId, item.name, item.vendor,
                    item.category, item.processorReady, item.error,
                    item.effectIdentified || item.processorReady, "factory"});
            }
        }
        replacement.erase(std::remove_if(replacement.begin(), replacement.end(), [](const CatalogEntry &entry) {
            return !entry.identified || entry.category != "Audio Module Class";
        }), replacement.end());
        int attempts = 1;
        for (const auto &entry : snapshot.catalog)
            if (entry.module == recognitionModule.toStdString()) attempts = std::max(attempts, entry.recognitionAttempts + 1);
        for (auto &entry : replacement) {
            entry.recognitionStatus = "ready";
            entry.recognitionSource = "factory";
            entry.recognitionAttempts = attempts;
            entry.recognitionError.clear();
            entry.recognitionIgnoredReason.clear();
            entry.recognitionRetryAfter = entry.recognitionDeadlineAt = 0;
        }
        if (!replacement.empty()) {
            snapshot.catalog.erase(std::remove_if(snapshot.catalog.begin(), snapshot.catalog.end(),
                [&](const CatalogEntry &entry) { return entry.module == recognitionModule.toStdString(); }), snapshot.catalog.end());
            snapshot.catalog.insert(snapshot.catalog.end(), replacement.begin(), replacement.end());
        } else {
            const auto reason = identified.errors.empty() ? std::string("recognition_failed") : identified.errors.front();
            for (auto &entry : snapshot.catalog) {
                if (entry.module != recognitionModule.toStdString()) continue;
                entry.error = entry.recognitionError = reason;
                entry.recognitionStatus = "failed";
                entry.recognitionSource = "factory";
                entry.recognitionAttempts = attempts;
                entry.recognitionRetryAfter = QDateTime::currentSecsSinceEpoch() + 60;
            }
        }
        std::sort(snapshot.catalog.begin(), snapshot.catalog.end(), [](const CatalogEntry &a, const CatalogEntry &b) {
            if (a.name != b.name) return a.name < b.name;
            return a.module < b.module;
        });
        ++snapshot.recognitionAttempted;
        if (replacement.empty()) ++snapshot.recognitionFailed;
        ++snapshot.recognitionCompleted;
        snapshot.recognitionPending = !recognitionQueue.isEmpty();
        snapshot.recognitionStatus = snapshot.recognitionPending ? "running" : "complete";
        snapshot.recognitionWorker = static_cast<bool>(recognitionJob);
        snapshot.recognitionCurrentModule.clear();
        startNextRecognition(snapshot.hostSupported);
        ++revision;
    }
    if (recognitionJob && !recognitionJob->finished.load(std::memory_order_acquire)) {
        const auto now = std::chrono::steady_clock::now();
        if (now - recognitionJob->started >= kRecognitionTimeout &&
            !recognitionJob->timedOut.exchange(true, std::memory_order_acq_rel)) {
            const auto deadline = QDateTime::currentSecsSinceEpoch();
            persistRecognitionTimeout(recognitionModule, deadline, "recognition_timeout");
            snapshot.catalog.erase(std::remove_if(snapshot.catalog.begin(), snapshot.catalog.end(),
                [&](const CatalogEntry &entry) { return entry.module == recognitionModule.toStdString(); }),
                snapshot.catalog.end());
            ++snapshot.recognitionAttempted;
            ++snapshot.recognitionTimedOut;
            ++snapshot.recognitionCompleted;
            ++recognitionWorkersDetached;
            snapshot.recognitionStatus = "timeout";
            snapshot.recognitionCurrentModule.clear();
            // The callback has no cancellation ABI. Its detached result is
            // ignored when it eventually returns; the UI and cache advance
            // immediately so a blocked third-party module cannot stall GP.
            recognitionJob.reset();
            startNextRecognition(snapshot.hostSupported);
            ++revision;
        }
    }
    snapshot.recognitionCurrentModule = recognitionJob ? recognitionModule.toStdString() : std::string{};
    snapshot.recognitionWorker = static_cast<bool>(recognitionJob);
    snapshot.recognitionPending = recognitionJob || !recognitionQueue.isEmpty();
    snapshot.recognitionWorkersStarted = recognitionWorkersStarted;
    snapshot.recognitionWorkersDetached = recognitionWorkersDetached;
    if (revision == delivered) return false;
    completed = snapshot;
    delivered = revision;
    return true;
}

void shutdownScan() noexcept {
    stopping.store(true);
    std::future<State> worker;
    {
        std::lock_guard<std::mutex> lock(scanMutex);
        worker = std::move(scanFuture);
        recognitionQueue.clear();
        recognitionJob.reset();
    }
    // The worker can still publish a final progress snapshot; never join it
    // with scanMutex held. It exits before the next module and skips caching.
    if (worker.valid()) worker.wait();
}

void setRecognitionControl(RecognitionControl control) noexcept {
    std::lock_guard<std::mutex> lock(scanMutex);
    recognitionControl = control;
}
} // namespace gpvst3::vst3
