#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "gp_audio_runtime.h"
#include "gp_audio_abi.h"
#include "state_manager.h"
#include "host_lock.h"
#include "gp_native_discovery.h"
#include "gp_object_registry.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QEvent>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QPointer>
#include <QtCore/QSet>
#include <QtCore/QString>
#include <QtCore/QUuid>
#include <QtCore/QUrl>
#include <QtWidgets/QApplication>
#include <QtWidgets/QWidget>
#include <QtWidgets/QStackedWidget>

#include <algorithm>
#include <atomic>
#include <mutex>

namespace gpvst3::gp_audio {
namespace {

constexpr std::size_t kMaxBindings = 64;
NativeObjectRegistry g_objects;

using McpAudioBindingVisitor = void (*)(void *user, void *chain, int trackIndex,
                                        int soundIndex, const char *documentId,
                                        const char *trackId, const char *scoreKey,
                                        bool activeDocument, bool selectedTrack) noexcept;
using McpAudioEnumerateFn = std::size_t (*)(McpAudioBindingVisitor visitor,
                                            void *user) noexcept;

class ControllerObserver final : public QObject {
public:
    bool eventFilter(QObject *object, QEvent *event) override {
        if (!object) return false;
        if (event && event->type() == QEvent::ThreadChange) {
            g_objects.forget(object);
            std::lock_guard<std::mutex> lock(mutex_);
            controllers_.removeAll(object);
            return false;
        }
        const auto name = QByteArray(object->metaObject()->className());
        if (name != "gp::rse::ConductorController" && name != "gp::gui::IDocumentsManager") return false;
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto &known : controllers_)
            if (known == object) return false;
        if (controllers_.size() < 128) {
            controllers_.append(QPointer<QObject>(object));
            QObject::connect(object, &QObject::destroyed, this, [this, object] {
                std::lock_guard<std::mutex> guard(mutex_);
                controllers_.removeAll(object);
            });
        }
        return false;
    }

    QList<QPointer<QObject>> controllers() {
        std::lock_guard<std::mutex> lock(mutex_);
        QList<QPointer<QObject>> result;
        for (const auto &controller : controllers_)
            if (controller) result.append(controller);
        return result;
    }

private:
    std::mutex mutex_;
    QList<QPointer<QObject>> controllers_;
};

ControllerObserver *g_observer = nullptr;
std::mutex g_snapshotMutex;
Binding g_bindings[kMaxBindings];
Binding g_bindingBuffers[2][kMaxBindings];
std::atomic<int> g_activeBuffer{0};
std::atomic<std::size_t> g_bindingCount{0};
std::atomic<std::uint64_t> g_generation{0};
const char *g_bindingSource = "unresolved";

QString scoreKey() {
    const auto configured = qEnvironmentVariable("GPVST3_SCORE_PATH");
    return configured.isEmpty() ? QStringLiteral("unspecified") :
        QDir::fromNativeSeparators(QFileInfo(configured).absoluteFilePath());
}

struct BridgeCollector {
    std::vector<Binding> result;
};

void collectBridgeBinding(void *user, void *chain, int trackIndex, int soundIndex,
                          const char *documentId, const char *trackId,
                          const char *scoreKeyText, bool activeDocument,
                          bool selectedTrack) noexcept {
    auto *collector = static_cast<BridgeCollector *>(user);
    if (!collector || trackIndex < 0 ||
        collector->result.size() >= kMaxBindings) return;
    const auto duplicate = std::find_if(collector->result.begin(), collector->result.end(),
        [&](const Binding &existing) { return chain ? existing.chain == chain :
            !existing.chain && existing.trackId == (trackId ? trackId : ""); });
    if (duplicate != collector->result.end()) return;
    Binding binding;
    binding.chain = chain;
    binding.trackIndex = trackIndex;
    binding.soundIndex = soundIndex;
    const auto stableScoreKey = scoreKeyText && *scoreKeyText
        ? QString::fromUtf8(scoreKeyText) : scoreKey();
    binding.trackKey = (stableScoreKey + QStringLiteral("#track-") +
                       QString::number(trackIndex)).toStdString();
    binding.trackId = trackId ? trackId : "";
    binding.documentId = documentId ? documentId : "";
    binding.scoreKey = stableScoreKey.toStdString();
    binding.activeDocument = activeDocument;
    binding.selectedTrack = selectedTrack;
    collector->result.push_back(std::move(binding));
}

std::vector<Binding> collectFromMcpBridge() {
    std::vector<Binding> empty;
    const auto module = GetModuleHandleW(L"guitarpro_mcp.dll");
    if (!module) return empty;
    using VersionFn = unsigned (*)() noexcept;
    const auto version = reinterpret_cast<VersionFn>(GetProcAddress(module, "gpmcp_audio_bridge_version"));
    if (!version || version() != 1) return empty;
    auto enumerate = reinterpret_cast<McpAudioEnumerateFn>(GetProcAddress(module, "gpmcp_audio_enumerate_contexts"));
    if (!enumerate) enumerate = reinterpret_cast<McpAudioEnumerateFn>(GetProcAddress(module, "gpmcp_audio_enumerate"));
    if (!enumerate) return empty;
    BridgeCollector collector;
    const auto count = enumerate(&collectBridgeBinding, &collector);
    if (!count || collector.result.empty()) return empty;
    return collector.result;
}

void observeObjectTree(QObject *root, QSet<QObject *> &seen) {
    if (!root || seen.contains(root)) return;
    seen.insert(root);
    if (g_observer) g_observer->eventFilter(root, nullptr);
    for (QObject *child : root->children()) observeObjectTree(child, seen);
}

struct NativeDocument {
    QWidget *view = nullptr;
    QObject *object = nullptr;
    gp::core::Score *score = nullptr;
    QString id, key;
};

std::vector<NativeDocument> nativeDocuments() {
    std::vector<NativeDocument> result;
    for (auto *view : QApplication::allWidgets()) {
        if (QByteArray(view->metaObject()->className()) != "gp::gui::IDocumentView") continue;
        quintptr implementation = 0, document = 0, impl = 0, score = 0, control = 0, model = 0, self = 0;
        if (!native::read(reinterpret_cast<quintptr>(view) + 0x30, implementation) ||
            !native::read(implementation + 0x98, document) || native::type(document) != ".?AVIDocument@gui@gp@@") continue;
        auto *object = native::asQObject(document);
        if (!object || QByteArray(object->metaObject()->className()) != "gp::gui::IDocument") continue;
        if (!native::read(document + 0x10, impl) || !native::read(impl + 0x68, score) ||
            !native::read(impl + 0x70, control) || native::type(control) != ".?AV?$_Ref_count_obj2@VScore@core@gp@@@std@@" ||
            !native::read(score, self) || self != score || !native::read(score + 0x38, model) ||
            native::type(model) != ".?AVScoreModel@core@gp@@") continue;
        auto *candidate = reinterpret_cast<gp::core::Score *>(score);
        if (reinterpret_cast<quintptr>(candidate->modelPrivate().get()) != model) continue;
        auto id = object->property("gpmcpDocumentId").toString();
        if (id.isEmpty()) {
            id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            object->setProperty("gpmcpDocumentId", id);
        }
        auto path = object->property("saveFilePath").toString();
        if (path.isEmpty()) path = object->property("openedFilePath").toString();
        const QUrl url(path);
        if (url.isLocalFile()) path = url.toLocalFile();
        const auto key = path.isEmpty() ? id : QDir::fromNativeSeparators(QFileInfo(path).absoluteFilePath());
        result.push_back({view, object, candidate, id, key});
    }
    return result;
}

QObject *nativeActiveDocument(const std::vector<NativeDocument> &documents,
                              const QList<QPointer<QObject>> &services) {
    QObject *active = nullptr;
    for (const auto &guard : services) {
        if (!guard || QByteArray(guard->metaObject()->className()) != "gp::gui::IDocumentsManager") continue;
        const auto manager = reinterpret_cast<quintptr>(guard.data());
        quintptr impl = 0, back = 0, current = 0;
        if (native::type(manager) != ".?AVIDocumentsManager@gui@gp@@" || !native::read(manager + 0x10, impl) ||
            !native::read(impl + 0x50, back) || back != manager || !native::read(impl + 0x70, current) ||
            native::type(current) != ".?AVIDocument@gui@gp@@") continue;
        auto *candidate = native::asQObject(current);
        if (active && active != candidate) return nullptr;
        active = candidate;
    }
    if (active) return active;
    // IDocumentView pages are the same verified document stack used by MCP
    // document navigation. This also works before the manager emits an event.
    for (const auto &document : documents) {
        auto *pages = qobject_cast<QStackedWidget *>(document.view->parentWidget());
        if (!pages || pages->count() != static_cast<int>(documents.size()) || pages->currentWidget() != document.view ||
            QByteArray(document.view->window()->metaObject()->className()) != "gp::gui::MainWindow") continue;
        if (active && active != document.object) return nullptr;
        active = document.object;
    }
    return active;
}

struct NativeTrack {
    QString document, id;
    std::weak_ptr<gp::core::Track> lifetime;
};
std::vector<NativeTrack> g_nativeTracks;

std::vector<Binding> collect() {
    static const bool verified = host::verify().supported;
    if (!verified) return {};
    // GuitarProMCP owns the verified native object registry, including
    // non-parented ConductorController services. Prefer its in-process bridge
    // so this plugin consumes the exact EffectsChain -> track_id mapping that
    // the gp_audio_abi tool validates.
#ifndef GPVST3_FORCE_NATIVE_AUDIO_BINDINGS
    if (const auto module = GetModuleHandleW(L"guitarpro_mcp.dll")) {
        using VersionFn = unsigned (*)() noexcept;
        const auto version = reinterpret_cast<VersionFn>(GetProcAddress(module, "gpmcp_audio_bridge_version"));
        if (version && version() == 1) {
            g_bindingSource = "mcp_bridge";
            return collectFromMcpBridge();
        }
    }
#endif
    g_bindingSource = "native_document_registry";
    std::vector<Binding> result;
    if (!g_observer || !qApp) return result;
    QSet<QObject *> seen;
    observeObjectTree(qApp, seen);
    for (QWidget *widget : QApplication::allWidgets()) observeObjectTree(widget, seen);
    auto controllers = g_observer->controllers();
    for (const auto &object : g_objects.objects()) {
        if (!object) continue;
        const auto name = QByteArray(object->metaObject()->className());
        if ((name == "gp::rse::ConductorController" || name == "gp::gui::IDocumentsManager") && !controllers.contains(object))
            controllers.append(object);
    }
    const auto documents = nativeDocuments();
    auto *activeDocument = nativeActiveDocument(documents, controllers);
    g_nativeTracks.erase(std::remove_if(g_nativeTracks.begin(), g_nativeTracks.end(),
        [](const NativeTrack &track) { return track.lifetime.expired(); }), g_nativeTracks.end());
    for (const auto &guard : controllers) {
        auto *object = guard.data();
        if (!object || QByteArray(object->metaObject()->className()) != "gp::rse::ConductorController") continue;
        auto *controller = reinterpret_cast<gp::rse::ConductorController *>(object);
        const auto &conductor = controller->conductor();
        if (!conductor || !conductor->score()) continue;
        const auto document = std::find_if(documents.begin(), documents.end(),
            [&](const NativeDocument &candidate) { return candidate.score == conductor->score().get(); });
        if (document == documents.end()) continue;
        const auto &tracks = conductor->score()->tracks();
        if (tracks.size() > 32) continue;
        for (std::size_t trackIndex = 0; trackIndex < tracks.size(); ++trackIndex) {
            const auto &coreTrack = tracks[trackIndex];
            auto *musician = conductor->musician(static_cast<unsigned>(trackIndex));
            if (!coreTrack || native::type(reinterpret_cast<quintptr>(coreTrack.get())) != ".?AVTrack@core@gp@@") continue;
            auto identity = std::find_if(g_nativeTracks.begin(), g_nativeTracks.end(), [&](const NativeTrack &track) {
                return track.document == document->id && track.lifetime.lock() == coreTrack;
            });
            if (identity == g_nativeTracks.end()) {
                g_nativeTracks.push_back({document->id, QUuid::createUuid().toString(QUuid::WithoutBraces), coreTrack});
                identity = g_nativeTracks.end() - 1;
            }
            const auto &sounds = coreTrack->sounds();
            const bool musicianReady = musician && native::type(reinterpret_cast<quintptr>(musician)) == ".?AVMusician@rse@gp@@" &&
                musician->coreTrack() && musician->coreTrack().get() == coreTrack.get();
            const auto count = musicianReady ? (std::min)(sounds.size(), std::size_t{64}) : 0;
            const auto beforeTrack = result.size();
            for (std::size_t soundIndex = 0; soundIndex < count; ++soundIndex) {
                auto sound = conductor->sound(static_cast<unsigned>(trackIndex), static_cast<unsigned>(soundIndex));
                if (!sound) sound = musician->soundAtIndex(static_cast<unsigned>(soundIndex));
                if (!sound) {
                    musician->updateAll();
                    sound = musician->soundAtIndex(static_cast<unsigned>(soundIndex));
                }
                if (!sound || !sound->effectChain()) continue;
                Binding binding;
                binding.chain = sound->effectChain().get();
                binding.trackIndex = static_cast<int>(trackIndex);
                binding.soundIndex = static_cast<int>(soundIndex);
                binding.trackId = identity->id.toStdString();
                binding.documentId = document->id.toStdString();
                binding.scoreKey = document->key.toStdString();
                binding.activeDocument = activeDocument == document->object;
                binding.selectedTrack = binding.activeDocument && document->score->cursor().trackIndex() == static_cast<int>(trackIndex);
                auto duplicate = std::find_if(result.begin(), result.end(),
                    [&](const Binding &existing) { return existing.chain == binding.chain; });
                if (duplicate == result.end()) result.push_back(std::move(binding));
                if (result.size() >= kMaxBindings) return result;
            }
            if (result.size() == beforeTrack) {
                Binding context;
                context.trackIndex = static_cast<int>(trackIndex);
                context.trackId = identity->id.toStdString(); context.documentId = document->id.toStdString();
                context.scoreKey = document->key.toStdString();
                context.activeDocument = activeDocument == document->object;
                context.selectedTrack = context.activeDocument && document->score->cursor().trackIndex() == static_cast<int>(trackIndex);
                result.push_back(std::move(context));
                if (result.size() >= kMaxBindings) return result;
            }
        }
    }
    return result;
}

} // namespace

const char *bindingSource() noexcept { return g_bindingSource; }

void initialize() noexcept {
    if (g_observer || !qApp) return;
    try {
        g_observer = new ControllerObserver;
        g_observer->setParent(qApp);
        qApp->installEventFilter(g_observer);
        bool registryRequired = true;
#ifndef GPVST3_FORCE_NATIVE_AUDIO_BINDINGS
        if (const auto module = GetModuleHandleW(L"guitarpro_mcp.dll")) {
            using VersionFn = unsigned (*)() noexcept;
            const auto version = reinterpret_cast<VersionFn>(GetProcAddress(module, "gpmcp_audio_bridge_version"));
            registryRequired = !version || version() != 1;
        }
#endif
        const auto qtCore = QDir(QCoreApplication::applicationDirPath()).filePath("Qt5Core.dll");
        if (registryRequired && host::verify().supported && host::sha256(qtCore) ==
            "C2F85BD55C31E5380DD99F0D517EE183A54C3852480BC497DC30A5483FD70FF2") g_objects.install();
    } catch (...) {
        g_observer = nullptr;
    }
}

void shutdown() noexcept {
    g_objects.uninstall();
    if (qApp && g_observer) qApp->removeEventFilter(g_observer);
    if (g_observer) g_observer->deleteLater();
    g_observer = nullptr;
    g_bindingCount.store(0, std::memory_order_release);
    g_activeBuffer.store(0, std::memory_order_release);
    std::lock_guard<std::mutex> lock(g_snapshotMutex);
    for (auto &buffer : g_bindingBuffers)
        for (auto &binding : buffer) binding = {};
}

std::size_t refresh() noexcept {
    try {
        auto discovered = collect();
        std::vector<state::HostTrackIdentity> identities;
        for (const auto &binding : discovered) identities.push_back({
            QString::fromStdString(binding.documentId), QString::fromStdString(binding.scoreKey),
            QString::fromStdString(binding.trackId), binding.trackIndex});
        if (!state::reconcileTrackIdentities(identities)) discovered.clear();
        for (std::size_t index = 0; index < discovered.size(); ++index)
            discovered[index].trackKey = identities[index].runtimeKey.toStdString();
        std::lock_guard<std::mutex> lock(g_snapshotMutex);
        const auto count = (std::min)(discovered.size(), kMaxBindings);
        const int active = g_activeBuffer.load(std::memory_order_relaxed);
        const int target = active == 0 ? 1 : 0;
        for (std::size_t index = 0; index < count; ++index)
            g_bindingBuffers[target][index] = discovered[index];
        for (std::size_t index = count; index < kMaxBindings; ++index)
            g_bindingBuffers[target][index] = {};
        g_generation.fetch_add(1, std::memory_order_relaxed);
        g_activeBuffer.store(target, std::memory_order_release);
        g_bindingCount.store(count, std::memory_order_release);
        return count;
    } catch (...) {
        return 0;
    }
}

bool currentTrack(Binding &binding) noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_snapshotMutex);
        const auto count = g_bindingCount.load(std::memory_order_acquire);
        const int active = g_activeBuffer.load(std::memory_order_acquire);
        for (std::size_t index = 0; index < count; ++index) {
            const auto &candidate = g_bindingBuffers[active][index];
            if (!candidate.chain || !candidate.activeDocument || !candidate.selectedTrack) continue;
            binding = candidate;
            return true;
        }
    } catch (...) {
    }
    binding = {};
    return false;
}

const Binding *lookup(void *chain) noexcept {
    const auto count = g_bindingCount.load(std::memory_order_acquire);
    const int active = g_activeBuffer.load(std::memory_order_acquire);
    for (std::size_t index = 0; index < count; ++index)
        if (g_bindingBuffers[active][index].chain == chain) return &g_bindingBuffers[active][index];
    return nullptr;
}

std::vector<Binding> snapshot() {
    std::lock_guard<std::mutex> lock(g_snapshotMutex);
    const auto count = g_bindingCount.load(std::memory_order_acquire);
    const int active = g_activeBuffer.load(std::memory_order_acquire);
    std::vector<Binding> result;
    result.reserve(count);
    for (std::size_t index = 0; index < count; ++index)
        result.push_back(g_bindingBuffers[active][index]);
    return result;
}

}
