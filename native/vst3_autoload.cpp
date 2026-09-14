#include <QtCore/QCoreApplication>
#include <QtCore/QFileInfo>
#include <QtCore/QTimer>
#include <QtGui/QImageIOPlugin>
#include <QtCore/QJsonDocument>

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
        name.contains("thread_id") || name.contains("frame_count") || name.endsWith("_blocks") ||
        name == QStringLiteral("last_process_nanoseconds") || name == QStringLiteral("max_process_nanoseconds") ||
        name == QStringLiteral("total_process_nanoseconds");
}

QJsonValue stableObservation(const QJsonValue &value) {
    if (value.isArray()) {
        QJsonArray result;
        for (const auto &item : value.toArray()) result.append(stableObservation(item));
        return result;
    }
    if (!value.isObject()) return value;
    QJsonObject result;
    const auto object = value.toObject();
    for (auto it = object.constBegin(); it != object.constEnd(); ++it)
        if (!telemetryKey(it.key())) result.insert(it.key(), stableObservation(it.value()));
    return result;
}

void writeObservation() {
    const auto hook = gpvst3::bootstrap::hookSnapshot();
    static QByteArray lastStable, lastFull;
    static auto lastSubmit = std::chrono::steady_clock::time_point{};
    const auto stable = QJsonDocument(stableObservation(hook).toObject()).toJson(QJsonDocument::Compact);
    const auto full = QJsonDocument(hook).toJson(QJsonDocument::Compact);
    const auto now = std::chrono::steady_clock::now();
    const bool detailed = qEnvironmentVariable("GPVST3_DIAGNOSTIC_MODE") == QStringLiteral("detailed");
    if (detailed || stable != lastStable ||
        (!lastSubmit.time_since_epoch().count() || (full != lastFull && now - lastSubmit >= std::chrono::seconds(5)))) {
        if (gpvst3::state::writeRealtimeObservation(hook)) {
            lastStable = stable; lastFull = full; lastSubmit = now;
        }
    }
}

class ObservationMonitor {
public:
    void start() {
        if (thread_.joinable()) return;
        stopping_.store(false, std::memory_order_release);
        thread_ = std::thread([this] {
            writeObservation();
            std::unique_lock<std::mutex> lock(mutex_);
            while (!stopping_.load(std::memory_order_acquire)) {
                const auto detailed = qEnvironmentVariable("GPVST3_DIAGNOSTIC_MODE") == QStringLiteral("detailed");
                condition_.wait_for(lock, detailed ? std::chrono::milliseconds(250) : std::chrono::seconds(1));
                if (stopping_.load(std::memory_order_acquire)) break;
                lock.unlock(); writeObservation(); lock.lock();
            }
        });
    }
    void stop() noexcept {
        stopping_.store(true, std::memory_order_release);
        condition_.notify_all();
        if (thread_.joinable()) thread_.join();
    }
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
    gpvst3::state::startStatusWriter();
    gpvst3::ui::showEffectChainPanel(false);
    if (!gpvst3::state::pluginEnabled()) {
        // The About dialog remains available so the user can re-enable the
        // plugin for the next Guitar Pro launch, but no scanner, observer or
        // audio hook is started while disabled.
        return;
    }
    // A new Guitar Pro session starts with every VST3 effect bypassed. The
    // persisted chain is shown in the selector, but processors are created
    // only after an explicit user enable action.
    // bootstrap owns a single 100 ms poll timer and starts it only while a
    // static scan or recognition job is active.
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
