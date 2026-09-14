#include <QtCore/QCoreApplication>
#include <QtCore/QFileInfo>
#include <QtCore/QTimer>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonValue>
#include <QtGui/QImageIOPlugin>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "modules/bootstrap.h"
#include "modules/gp_audio_runtime.h"
#include "modules/gp_hook.h"
#include "modules/qt_ui.h"
#include "modules/state_manager.h"
#include "modules/vst3_host.h"

namespace {

QString pluginPath() {
    HMODULE module = nullptr;
    wchar_t path[32768]{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&pluginPath), &module)) return {};
    const DWORD length = GetModuleFileNameW(module, path, 32768);
    return length && length < 32768 ? QString::fromWCharArray(path, static_cast<int>(length)) : QString{};
}

bool telemetryKey(const QString &key) {
    const auto name = key.toLower();
    return name.contains("count") || name.contains("sequence") || name.contains("address") ||
        name.contains("hash") || name.contains("peak") || name.contains("rms") ||
        name.endsWith("_level") || name == QStringLiteral("observation_nanoseconds") ||
        name == QStringLiteral("vst3_output_non_silent") ||
        name.contains("thread_id") || name.contains("frame_count") || name.endsWith("_blocks") ||
        name == QStringLiteral("last_process_nanoseconds") ||
        name == QStringLiteral("max_process_nanoseconds") ||
        name == QStringLiteral("total_process_nanoseconds");
}

QJsonValue stableObservationValue(const QJsonValue &value) {
    if (value.isArray()) {
        QJsonArray array;
        for (const auto &item : value.toArray()) array.append(stableObservationValue(item));
        return array;
    }
    if (!value.isObject()) return value;
    QJsonObject object;
    const auto objectValue = value.toObject();
    for (auto it = objectValue.constBegin(); it != objectValue.constEnd(); ++it) {
        if (telemetryKey(it.key())) continue;
        object.insert(it.key(), stableObservationValue(it.value()));
    }
    return object;
}

void writeObservation() {
    const auto hook = gpvst3::bootstrap::hookSnapshot();
    static QByteArray lastStable;
    static QByteArray lastFull;
    static auto lastSubmit = std::chrono::steady_clock::time_point{};
    const auto stable = QJsonDocument(stableObservationValue(hook).toObject()).toJson(QJsonDocument::Compact);
    const auto full = QJsonDocument(hook).toJson(QJsonDocument::Compact);
    const auto now = std::chrono::steady_clock::now();
    const bool detailed = qEnvironmentVariable("GPVST3_DIAGNOSTIC_MODE") == QStringLiteral("detailed");
    if (stable != lastStable || lastSubmit.time_since_epoch().count() == 0 ||
        (full != lastFull && now - lastSubmit >= std::chrono::seconds(5)) || detailed) {
        if (gpvst3::state::writeRealtimeObservation(hook)) {
            lastStable = stable;
            lastFull = full;
            lastSubmit = now;
        }
    }
}

class ObservationMonitor final {
public:
    void start() {
        if (thread_.joinable()) return;
        stopping_.store(false, std::memory_order_release);
        thread_ = std::thread([this] {
            writeObservation();
            std::unique_lock<std::mutex> lock(mutex_);
            const auto interval = qEnvironmentVariable("GPVST3_DIAGNOSTIC_MODE") == QStringLiteral("detailed")
                ? std::chrono::milliseconds(250) : std::chrono::seconds(1);
            while (!stopping_.load(std::memory_order_acquire)) {
                condition_.wait_for(lock, interval);
                if (stopping_.load(std::memory_order_acquire)) break;
                lock.unlock();
                writeObservation();
                lock.lock();
            }
        });
    }
    void stop() noexcept {
        stopping_.store(true, std::memory_order_release);
        condition_.notify_all();
        if (thread_.joinable()) thread_.join();
    }
    ~ObservationMonitor() { stop(); }
private:
    std::atomic<bool> stopping_{false};
    std::mutex mutex_;
    std::condition_variable condition_;
    std::thread thread_;
};

ObservationMonitor g_observationMonitor;

void stopObservation() {
    g_observationMonitor.stop();
    gpvst3::vst3::shutdownScan();
    gpvst3::state::writeRealtimeObservation(gpvst3::bootstrap::hookSnapshot());
    gpvst3::state::stopRealtimeObservationWriter();
    gpvst3::state::stopStatusWriter();
    gpvst3::ui::shutdownEditors();
    gpvst3::hook::shutdown();
    gpvst3::gp_audio::shutdown();
}

void initializePlugin() {
    auto *application = QCoreApplication::instance();
    auto status = gpvst3::bootstrap::initialize();
    status.insert("plugin_path", pluginPath());
    gpvst3::state::writeStatus(status);
    gpvst3::ui::showEffectChainPanel(false);
    if (!gpvst3::state::pluginEnabled()) {
        // The About dialog remains available so the user can re-enable the
        // plugin for the next Guitar Pro launch, but no scanner, observer or
        // audio hook is started while disabled.
        return;
    }
    gpvst3::state::startRealtimeObservationWriter();
    gpvst3::state::startStatusWriter();
    // A new Guitar Pro session starts with every VST3 effect bypassed. The
    // persisted chain is shown in the selector, but processors are created
    // only after an explicit user enable action.
    // A P7 selection can start the hook after bootstrap. Keep its
    // observation and shutdown lifecycle available in default launches.
    g_observationMonitor.start();
    QObject::connect(application, &QCoreApplication::aboutToQuit, application, &gpvst3::ui::shutdownEditors);
    QObject::connect(application, &QCoreApplication::aboutToQuit, application, &gpvst3::hook::saveVst3States);
    QObject::connect(application, &QCoreApplication::aboutToQuit, application, &gpvst3::vst3::shutdownScan);
    qAddPostRoutine(&stopObservation);
}

}

class GuitarProVst3Autoload final : public QImageIOPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.qt-project.Qt.QImageIOHandlerFactoryInterface" FILE "vst3-autoload.json")

public:
    GuitarProVst3Autoload() {
        if (!qApp || QFileInfo(QCoreApplication::applicationFilePath()).baseName().compare("GuitarPro", Qt::CaseInsensitive) != 0)
            return;
        if (qApp->property("gpvst3P0Scheduled").toBool()) return;
        qApp->setProperty("gpvst3P0Scheduled", true);
        auto *application = qApp;
        QTimer::singleShot(0, application, &initializePlugin);
    }

    Capabilities capabilities(QIODevice *, const QByteArray &) const override { return {}; }
    QImageIOHandler *create(QIODevice *, const QByteArray &) const override { return nullptr; }
};

#include "vst3_autoload.moc"
