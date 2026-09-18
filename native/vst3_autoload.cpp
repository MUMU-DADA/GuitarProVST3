#include <QtCore/QCoreApplication>
#include <QtCore/QFileInfo>
#include <QtCore/QTimer>
#include <QtGui/QImageIOPlugin>
#include <QtCore/QJsonDocument>
#include <QtCore/QPointer>
#include <QtCore/QDir>

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
    // A host DSP call can expose a rebuilt EffectsChain before Qt publishes a
    // document event. The callback only sets an atomic; this control monitor
    // turns that concrete invalidation into one coalesced topology refresh.
    if (gpvst3::hook::consumeTrackTopologyInvalidation())
        gpvst3::gp_audio::markExplicitTopologyDirty();
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
std::atomic<bool> g_observationStopped{false};

void stopObservation() {
    if (g_observationStopped.exchange(true, std::memory_order_acq_rel)) return;
    gpvst3::bootstrap::shutdown();
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
    if (!application) return;
    gpvst3::ui::setStartupProgress(-1, QStringLiteral("正在启动 VST3…"));
    const auto applicationDirectory = QCoreApplication::applicationDirPath();
    // Hashing the host binaries is intentionally read-only, but doing it on
    // Guitar Pro's Qt thread blocks repaint/input for several hundred ms (or
    // longer on a cold disk). Keep the gate off the UI thread, then continue
    // the ABI-sensitive setup on the host thread in separate event turns.
    QPointer<QCoreApplication> target = application;
    std::thread([target, applicationDirectory] {
        const auto verification = gpvst3::host::verifyDirectory(applicationDirectory);
        auto preparedVerification = verification;
        preparedVerification.qtCoreSupported = gpvst3::host::sha256(
            QDir(applicationDirectory).filePath(QStringLiteral("Qt5Core.dll"))) ==
            "C2F85BD55C31E5380DD99F0D517EE183A54C3852480BC497DC30A5483FD70FF2";
        if (!target) return;
        QMetaObject::invokeMethod(target.data(), [target, preparedVerification] {
            if (!target || target->property("gpvst3P0Finished").toBool()) return;
            gpvst3::ui::setStartupProgress(28, QStringLiteral("正在连接宿主…"));
            QTimer::singleShot(0, target.data(), [target, preparedVerification] {
                if (!target || target->property("gpvst3P0Finished").toBool()) return;
                gpvst3::ui::setStartupProgress(42, QStringLiteral("正在准备音频链…"));
                auto status = gpvst3::bootstrap::initialize(preparedVerification);
                status.insert("plugin_path", pluginPath());
                gpvst3::state::writeStatus(status);
                gpvst3::state::startStatusWriter();
                gpvst3::ui::showEffectChainPanel(false);
                if (!gpvst3::state::pluginEnabled()) {
                    gpvst3::ui::setStartupProgress(100, QStringLiteral("插件已停用"), false);
                    target->setProperty("gpvst3P0Finished", true);
                    return;
                }
                if (!target->property("gpvst3P0CleanupConnected").toBool()) {
                    QObject::connect(target.data(), &QCoreApplication::aboutToQuit, target.data(), &gpvst3::ui::shutdownEditors);
                    QObject::connect(target.data(), &QCoreApplication::aboutToQuit, target.data(), &gpvst3::hook::saveVst3States);
                    QObject::connect(target.data(), &QCoreApplication::aboutToQuit, target.data(), &gpvst3::vst3::shutdownScan);
                    QObject::connect(target.data(), &QCoreApplication::aboutToQuit, target.data(), &stopObservation);
                    qAddPostRoutine(&stopObservation);
                    target->setProperty("gpvst3P0CleanupConnected", true);
                }
                QTimer::singleShot(0, target.data(), [target] {
                    if (!target || target->property("gpvst3P0Finished").toBool()) return;
                    g_observationMonitor.start();
                    target->setProperty("gpvst3P0Finished", true);
                });
            });
        }, Qt::QueuedConnection);
    }).detach();
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
        gpvst3::gp_audio::observeHostObjects();
        auto *application = qApp;
        QTimer::singleShot(0, application, &initializePlugin);
    }

    Capabilities capabilities(QIODevice *, const QByteArray &) const override { return {}; }
    QImageIOHandler *create(QIODevice *, const QByteArray &) const override { return nullptr; }
};

#include "vst3_autoload.moc"
