#include "gp_hook.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#undef max
#undef min

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <cstdio>
#include <mutex>
#include <condition_variable>
#include <string>
#include <vector>
#include <unordered_map>
#include <thread>
#include <chrono>

#include "audio_adapter.h"
#include "effect_chain.h"
#include "input_monitor_exchange.h"
#include "gp_audio_runtime.h"
#include "state_manager.h"
#include "vst3_parameters.h"
#include "qt_ui.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include <memory>
#include <QtCore/QCoreApplication>
#include <QtCore/QMetaObject>
#include <QtCore/QThread>
#include <QtCore/QEventLoop>
#include <QtCore/QWinEventNotifier>
#include <QtCore/QByteArray>
#include "portaudio_capture_abi.h"
#include "../third_party/minhook/include/MinHook.h"
#include "asio_lifecycle_probe.h"
#include "input_drain_probe.h"
#ifdef GPVST3_P13_PROBE_BUILD
#include "input_probe.h"
#include "input_timing_probe.h"
#include "input_pcm_probe.h"
#include <QtCore/QJsonObject>
#include <QtCore/QJsonDocument>
#include <QtCore/QDir>
#include <QtCore/QSaveFile>
#include <QtCore/QCryptographicHash>
extern "C" __declspec(dllexport) const char *gpvst3P13ProbeBuild() {
    return "GPVST3_P13_EXPERIMENTAL_DIAGNOSTICS_NOT_FOR_RELEASE";
}
#endif
#include "pluginterfaces/base/funknownimpl.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstpluginterfacesupport.h"
#include "public.sdk/source/common/memorystream.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/gui/iplugviewcontentscalesupport.h"

namespace Steinberg {
DEF_CLASS_IID(IPlugView)
DEF_CLASS_IID(IPlugFrame)
DEF_CLASS_IID(IPlugViewContentScaleSupport)
}

namespace gpvst3::hook {

int streamCallbackHook(const void *input, void *output, unsigned long frames,
                       const void *timeInfo, unsigned long status, void *userData);
std::int64_t listenerProbeHook(void *self, const float *input, std::uint32_t inputChannels,
    float *output, std::uint32_t outputChannels, std::int64_t frames, const void *timePoint);
#ifdef GPVST3_P13_PROBE_BUILD
std::int64_t rseProbeHook(void *self, const float *input, std::uint32_t inputChannels,
    float *output, std::uint32_t outputChannels, std::int64_t frames, const void *timePoint);
#endif
void reconfigureInputRouterIfNeeded() noexcept;
bool configureSelectedChain(const std::vector<Vst3SelectionEntry> &selection, std::string *error) noexcept;
State prepare(const host::Verification &verification, bool enableForSelection) noexcept;

namespace {

using SteadyClock = std::chrono::steady_clock;

std::uint64_t steadyNanoseconds() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        SteadyClock::now().time_since_epoch()).count());
}

// Optional trace for diagnosing a third-party editor that blocks the host
// thread. It is enabled only when GPVST3_EDITOR_TRACE is set and writes a
// flushed line before/after each VST3 editor contract call.
void editorTrace(const char *event) noexcept {
    const char *enabled = std::getenv("GPVST3_EDITOR_TRACE");
    const char *directory = std::getenv("GPVST3_DATA_DIR");
    if (!enabled || std::strcmp(enabled, "1") != 0 || !directory || !*directory || !event) return;
    const auto path = std::string(directory) + "\\editor-trace.log";
    FILE *file = nullptr;
    if (fopen_s(&file, path.c_str(), "ab") != 0 || !file) return;
    std::fprintf(file, "%llu tid=%lu %s\n",
                 static_cast<unsigned long long>(steadyNanoseconds()),
                 static_cast<unsigned long>(GetCurrentThreadId()), event);
    std::fflush(file);
    fclose(file);
}

constexpr char kMasterProcess[] =
    "?process@Master@rse@gp@@QEAAXAEAVAudioBuffer@audio@am@@AEBV?$vector@VTick@audio@am@@V?$allocator@VTick@audio@am@@@std@@@std@@AEBV?$vector@PEAVMusician@rse@gp@@V?$allocator@PEAVMusician@rse@gp@@@std@@@8@AEBV?$shared_ptr@VBackingTrack@rse@gp@@@8@@Z";
constexpr char kEffectsChainProcessDsp[] =
    "?processDSP@EffectsChain@rse@gp@@QEAAXAEAVIAudioBuffer@audio@am@@AEAV?$array@VAudioBuffer@audio@am@@$02@std@@AEBV?$vector@VTick@audio@am@@V?$allocator@VTick@audio@am@@@std@@@8@@Z";
constexpr char kEffectsChainIndex[] =
    "?index@EffectsChain@rse@gp@@QEBAIXZ";
constexpr char kRawData[] = "?rawData@AudioBuffer@audio@am@@UEBAAEBV?$array@PEAM$01@std@@XZ";
constexpr char kFrameCount[] = "?frameCount@AudioBuffer@audio@am@@UEBA_JXZ";
constexpr char kChannelCount[] = "?channelCount@AudioBuffer@audio@am@@UEBAIXZ";
constexpr char kLock[] = "?lock@AudioBuffer@audio@am@@UEAAXXZ";
constexpr char kUnlock[] = "?unlock@AudioBuffer@audio@am@@UEAAXXZ";
constexpr char kAudioCoreInstance[] = "?Instance@AudioCore@audio@am@@SAAEAV123@XZ";
constexpr char kSampleRate[] = "?samplingRate@AudioCore@audio@am@@QEBAHXZ";
constexpr char kAudioLayerInstance[] = "?instance@AudioLayer@audio@am@@SAAEAV123@XZ";
constexpr char kAudioLayerInputLevel[] = "?inputLevel@AudioLayer@audio@am@@QEBAMXZ";
constexpr char kAudioLayerIsRunning[] = "?isRunning@AudioLayer@audio@am@@QEBA_NXZ";
constexpr char kAudioLayerBufferSize[] = "?bufferSize@AudioLayer@audio@am@@QEBAHXZ";

// The locked 8.1.1.17 entry points both begin with three complete, position
// independent mov instructions. Refuse installation if memory differs.
constexpr std::size_t kMasterPatchBytes = 15;
constexpr std::size_t kDspPatchBytes = 15;
constexpr std::size_t kStreamPatchBytes = 16;
constexpr std::size_t kCursorMovePatchBytes = 23;
constexpr std::size_t kCursorTrackPatchBytes = 13;
constexpr std::size_t kScoreCreateTrackPatchBytes = 20;
constexpr std::size_t kScoreDuplicateTrackPatchBytes = 21;
constexpr std::size_t kScoreRemoveTrackPatchBytes = 12;
constexpr std::size_t kScoreSwapTracksPatchBytes = 14;
constexpr std::uint8_t kMasterPrologue[kMasterPatchBytes]{
    0x4C, 0x89, 0x44, 0x24, 0x18, 0x48, 0x89, 0x54, 0x24, 0x10, 0x48, 0x89, 0x4C, 0x24, 0x08};
constexpr std::uint8_t kDspPrologue[kDspPatchBytes]{
    0x48, 0x89, 0x5C, 0x24, 0x20, 0x48, 0x89, 0x54, 0x24, 0x10, 0x48, 0x89, 0x4C, 0x24, 0x08};
constexpr std::uint8_t kStreamPrologue[kStreamPatchBytes]{
    0x48, 0x89, 0x5C, 0x24, 0x20, 0x55, 0x56, 0x57,
    0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
constexpr char kCursorMove[] =
    "?moveToCursorAndNotify@ScoreCursor@core@gp@@QEAAXAEBV123@PEBV123@@Z";
constexpr char kCursorTrack[] =
    "?trySetTrackIndex@ScoreCursor@core@gp@@QEAA_NH@Z";
constexpr char kScoreCreateTrack[] =
    "?createTrack@Score@core@gp@@QEAAXIAEBV?$shared_ptr@VTrack@core@gp@@@std@@I_N11I@Z";
constexpr char kScoreDuplicateTrack[] = "?duplicateTrack@Score@core@gp@@QEAAXI@Z";
constexpr char kScoreRemoveTrack[] = "?removeTrack@Score@core@gp@@QEAAXI@Z";
constexpr char kScoreSwapTracks[] = "?swapTracks@Score@core@gp@@QEAAXII@Z";
// Guitar Pro 8.1.1.17 x64 prologues, sampled from the verified GPCore.dll.
// These are position-independent register saves and stack setup; the hash
// gate refuses installation on any other host build.
constexpr std::uint8_t kCursorMovePrologue[kCursorMovePatchBytes]{
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x57,
    0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
    0x48, 0x8B, 0xEC, 0x48, 0x83, 0xEC, 0x70};
constexpr std::uint8_t kCursorTrackPrologue[kCursorTrackPatchBytes]{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83,
    0xEC, 0x20, 0x48, 0x8B, 0x19};
constexpr std::uint8_t kScoreCreateTrackPrologue[kScoreCreateTrackPatchBytes]{
    0x48, 0x83, 0xEC, 0x68, 0x4D, 0x8B, 0x50, 0x08, 0x0F, 0x57, 0xC0,
    0xF3, 0x0F, 0x7F, 0x44, 0x24, 0x50, 0x4D, 0x85, 0xD2};
constexpr std::uint8_t kScoreDuplicateTrackPrologue[kScoreDuplicateTrackPatchBytes]{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x56, 0x57,
    0x41, 0x56, 0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00};
constexpr std::uint8_t kScoreRemoveTrackPrologue[kScoreRemoveTrackPatchBytes]{
    0x44, 0x8B, 0xC2, 0x48, 0x8B, 0xD1, 0xE9, 0x75, 0xEC, 0xFF, 0xFF, 0xCC};
constexpr std::uint8_t kScoreSwapTracksPrologue[kScoreSwapTracksPatchBytes]{
    0x45, 0x8B, 0xC8, 0x44, 0x8B, 0xC2, 0x48, 0x8B, 0xD1, 0xE9, 0xA2, 0xE8, 0xFF, 0xFF};
constexpr std::uintptr_t kStreamCallbackRva = 0xABE0;

struct RawData { float *channels[2]; };
using MasterProcess = void (*)(void *, void *, void *, void *, void *);
using DspProcess = void (*)(void *, void *, void *, void *);
using StreamCallback = int (*)(const void *, void *, unsigned long, const void *, unsigned long, void *);
using RawDataFn = const RawData &(*)(const void *);
using FrameCountFn = std::size_t (*)(const void *);
using ChannelCountFn = unsigned (*)(const void *);
using BufferAccessFn = void (*)(void *);
using AudioCoreInstanceFn = void *(*)();
using SampleRateFn = int (*)(const void *);
using AudioLayerInstanceFn = void *(*)();
using AudioLayerInputLevelFn = float (*)(const void *);
using AudioLayerIsRunningFn = bool (*)(const void *);
using AudioLayerBufferSizeFn = int (*)(const void *);
using EffectsChainIndexFn = unsigned (*)(const void *);
using CursorMove = void (*)(void *, const void *, const void *);
using CursorTrack = bool (*)(void *, int);
using ScoreCreateTrack = void (*)(void *, unsigned, const void *, unsigned, bool, bool, bool, unsigned);
using ScoreDuplicateTrack = void (*)(void *, unsigned);
using ScoreRemoveTrack = void (*)(void *, unsigned);
using ScoreSwapTracks = void (*)(void *, unsigned, unsigned);

namespace fs = std::filesystem;
using Steinberg::FUnknownPtr;
using Steinberg::IPtr;
using Steinberg::TUID;
using Steinberg::tresult;
using Steinberg::Vst::IComponent;
using Steinberg::Vst::IAudioProcessor;
using Steinberg::Vst::IComponentHandler;
using Steinberg::Vst::IEditController;
using Steinberg::Vst::IParamValueQueue;
using Steinberg::Vst::IParameterChanges;
using Steinberg::Vst::ParamID;
using Steinberg::Vst::ParamValue;
using Steinberg::ViewRect;

enum class EditorStage : int {
    None = 0,
    Requested,
    BusyWait,
    ControllerMissing,
    CreateView,
    PlatformCheck,
    SetFrame,
    GetSize,
    Attached,
    Visible,
    Focus,
    Removed,
    Failed
};

const char *editorStageName(EditorStage stage) noexcept {
    switch (stage) {
    case EditorStage::Requested: return "requested";
    case EditorStage::BusyWait: return "busy_wait";
    case EditorStage::ControllerMissing: return "controller_missing";
    case EditorStage::CreateView: return "create_view";
    case EditorStage::PlatformCheck: return "platform_check";
    case EditorStage::SetFrame: return "set_frame";
    case EditorStage::GetSize: return "get_size";
    case EditorStage::Attached: return "attached";
    case EditorStage::Visible: return "visible";
    case EditorStage::Focus: return "focus";
    case EditorStage::Removed: return "removed";
    case EditorStage::Failed: return "failed";
    default: return "none";
    }
}

struct RuntimeEffect;
void notifyInputLatencyChange() noexcept;
bool succeeded(tresult result) noexcept;
void mirrorInputParameter(RuntimeEffect *source, ParamID id, ParamValue value) noexcept;

class RuntimeComponentHandler final
    : public Steinberg::U::Implements<Steinberg::U::Directly<IComponentHandler>> {
public:
    explicit RuntimeComponentHandler(RuntimeEffect *owner) : owner_(owner) {}
    Steinberg::tresult PLUGIN_API beginEdit(ParamID) override;
    Steinberg::tresult PLUGIN_API performEdit(ParamID, ParamValue) override;
    Steinberg::tresult PLUGIN_API endEdit(ParamID) override;
    Steinberg::tresult PLUGIN_API restartComponent(Steinberg::int32) override;

private:
    RuntimeEffect *owner_ = nullptr;
};

bool succeeded(tresult result) noexcept;
bool onQtThread() noexcept;
bool invokeOnQtThreadBlocking(const std::function<void()> &callback) noexcept;
std::atomic<bool> g_qtDispatchStopping{false};

// The selection worker also owns plug-in objects that may create Qt/native
// objects during VST3 initialization. Keep a small event loop on that thread
// so those objects receive queued work while the worker is idle.
std::mutex g_selectionWorkerLoopMutex;
QEventLoop *g_selectionWorkerLoop = nullptr;

class RuntimePlugFrame final
    : public Steinberg::U::Implements<Steinberg::U::Directly<Steinberg::IPlugFrame>> {
public:
    explicit RuntimePlugFrame(HWND hostWindow) : hostWindow_(hostWindow) {}
    Steinberg::tresult PLUGIN_API resizeView(Steinberg::IPlugView *view,
                                             ViewRect *newSize) override {
        if (!newSize || !hostWindow_ || !IsWindow(hostWindow_)) return Steinberg::kInvalidArgument;
        if (resizing_.test_and_set(std::memory_order_acquire)) return Steinberg::kResultTrue;
        struct ResizeGuard {
            std::atomic_flag &flag;
            ~ResizeGuard() { flag.clear(std::memory_order_release); }
        } resizeGuard{resizing_};
        const int width = (std::max)(1, newSize->getWidth());
        const int height = (std::max)(1, newSize->getHeight());
        try {
            if (!invokeOnQtThreadBlocking([&] {
                    gpvst3::ui::resizeNativeEditor(reinterpret_cast<void *>(hostWindow_), width, height);
                })) {
                return Steinberg::kResultFalse;
            }
        } catch (...) {
            return Steinberg::kResultFalse;
        }
        bool resized = false;
        try { resized = view && succeeded(view->onSize(newSize)); } catch (...) { resized = false; }
        return resized ? Steinberg::kResultTrue : Steinberg::kResultFalse;
    }

private:
    HWND hostWindow_ = nullptr;
    std::atomic_flag resizing_ = ATOMIC_FLAG_INIT;
};

bool succeeded(tresult result) noexcept {
    return result == Steinberg::kResultOk || result == Steinberg::kResultTrue;
}

bool onQtThread() noexcept {
    const auto *application = QCoreApplication::instance();
    return application && QThread::currentThread() == application->thread();
}

// Only operations that access the host UI use this dispatch. Processor
// preparation runs on the selection worker so a slow initialize() cannot
// occupy Guitar Pro's event loop.
bool invokeOnQtThreadBlocking(const std::function<void()> &callback) noexcept {
    if (!callback) return false;
    if (onQtThread()) {
        try { callback(); } catch (...) { return false; }
        return true;
    }
    auto *application = QCoreApplication::instance();
    if (!application || QCoreApplication::closingDown() ||
        g_qtDispatchStopping.load(std::memory_order_acquire)) return false;
    struct InvocationState {
        std::mutex mutex;
        std::condition_variable condition;
        std::function<void()> callback;
        bool started = false;
        bool done = false;
        bool cancelled = false;
        bool failed = false;
    };
    auto state = std::make_shared<InvocationState>();
    state->callback = callback;
    try {
        if (!QMetaObject::invokeMethod(application, [state] {
                std::function<void()> callbackToRun;
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    if (state->cancelled) {
                        state->done = true;
                        state->condition.notify_all();
                        return;
                    }
                    state->started = true;
                    callbackToRun = state->callback;
                }
                bool failed = false;
                try { callbackToRun(); } catch (...) { failed = true; }
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    state->failed = failed;
                    state->done = true;
                }
                state->condition.notify_all();
            }, Qt::QueuedConnection)) return false;
        std::unique_lock<std::mutex> lock(state->mutex);
        while (!state->done) {
            if (g_qtDispatchStopping.load(std::memory_order_acquire) && !state->started) {
                state->cancelled = true;
                return false;
            }
            state->condition.wait_for(lock, std::chrono::milliseconds(20));
        }
        return !state->failed && !state->cancelled;
    } catch (...) {
        return false;
    }
}

class RuntimeHostApplication final
    : public Steinberg::U::Implements<Steinberg::U::Directly<
          Steinberg::Vst::IHostApplication, Steinberg::Vst::IPlugInterfaceSupport>> {
public:
    tresult PLUGIN_API getName(Steinberg::Vst::String128 name) override {
        if (!name) return Steinberg::kInvalidArgument;
        constexpr char text[] = "GuitarProVST3 P2 Runtime";
        int index = 0;
        for (; text[index] != 0 && index < 127; ++index)
            name[index] = static_cast<Steinberg::Vst::TChar>(text[index]);
        name[index] = 0;
        return Steinberg::kResultTrue;
    }

    tresult PLUGIN_API createInstance(TUID, TUID, void **object) override {
        if (object) *object = nullptr;
        return Steinberg::kNoInterface;
    }

    tresult PLUGIN_API isPlugInterfaceSupported(const TUID iid) override {
        if (!iid) return Steinberg::kInvalidArgument;
        // Only advertise contracts that are actually supplied by this host.
        // Returning true for arbitrary interfaces makes some third-party
        // controllers take an unsupported code path and fail later in
        // createView/attached with no useful diagnostic.
        if (std::memcmp(iid, Steinberg::IPlugFrame::iid, sizeof(TUID)) == 0 ||
            std::memcmp(iid, Steinberg::IPlugViewContentScaleSupport::iid, sizeof(TUID)) == 0)
            return Steinberg::kResultTrue;
        return Steinberg::kResultFalse;
    }
};

using InitModuleProc = bool (PLUGIN_API *)();
using ExitModuleProc = bool (PLUGIN_API *)();
using GetFactoryProc = Steinberg::IPluginFactory *(PLUGIN_API *)();

fs::path runtimeModulePath() {
    const char *configured = std::getenv("GPVST3_RUNTIME_VST3");
    if (configured && *configured) return fs::u8path(configured);
    const char *programFiles = std::getenv("ProgramW6432");
    if (!programFiles || !*programFiles) programFiles = std::getenv("ProgramFiles");
    if (!programFiles || !*programFiles) return {};
    return fs::u8path(std::string(programFiles) + "\\Common Files\\VST3\\ParametricOD.vst3");
}

std::string uidString(const Steinberg::TUID value) {
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(32);
    for (int i = 0; i < 16; ++i) {
        const auto byte = static_cast<unsigned char>(value[i]);
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 0x0f]);
    }
    return result;
}

fs::path runtimeBinary(const fs::path &path) {
    std::error_code error;
    if (!fs::is_directory(path, error)) return path;
    const auto candidate = path / L"Contents" / L"x86_64-win" / path.filename();
    return fs::is_regular_file(candidate, error) ? candidate : fs::path{};
}

std::atomic<std::uint64_t> g_nextInstanceId{1};

struct RuntimeEffect {
    const std::uint64_t instanceId = g_nextInstanceId.fetch_add(1, std::memory_order_relaxed);
    std::atomic<std::uint64_t> processedBlocks{0};
    HMODULE module = nullptr;
    ExitModuleProc exit = nullptr;
    IPtr<Steinberg::IPluginFactory> factory;
    IPtr<IComponent> component;
    FUnknownPtr<IAudioProcessor> processor;
    IPtr<IEditController> controller;
    bool separateControllerInitialized = false;
    IPtr<Steinberg::IPlugView> editor;
    IPtr<IComponentHandler> componentHandler;
    IPtr<Steinberg::IPlugFrame> plugFrame;
    std::thread editorThread;
    std::mutex editorThreadMutex;
    std::condition_variable editorThreadCv;
    bool editorThreadStop = false;
    bool editorThreadReady = false;
    bool editorThreadSucceeded = false;
    QEventLoop *editorCloseLoop = nullptr;
    vst3::ParameterChanges parameterChanges;
    Vst3SelectionEntry identity;
    Vst3SelectionEntry loadedSelection;
    bool preloaded = false; // Control-thread diagnostic; never enables processing.
    std::atomic<bool> independentInput{false};
    FUnknownPtr<Steinberg::Vst::IConnectionPoint> componentConnection;
    FUnknownPtr<Steinberg::Vst::IConnectionPoint> controllerConnection;
    std::atomic<std::size_t> parameterEdits{0};
    HWND editorParent = nullptr;
    std::atomic<bool> editorAttached{false};
    // Only a global effect receives a mirror to the independent live-input
    // instance. Track effects must remain playback-only even when they use
    // the same VST3 class as the global chain.
    std::weak_ptr<RuntimeEffect> inputParameterMirror;
    mutable std::mutex editorErrorMutex;
    std::string editorError;
    std::atomic<int> editorStage{static_cast<int>(EditorStage::None)};
    std::atomic<long> editorResultCode{0};
    RuntimeHostApplication host;
    audio::PlanarBuffer scratch;
    std::string name;
    std::string error;
    std::atomic<bool> ready{false};
    std::atomic<bool> outputWritten{false};
    std::atomic<bool> ownerObserved{false};
    std::atomic<int> configuredRate{0};
    std::atomic<std::size_t> configuredBlock{0};
    bool forceError = false;
    std::atomic_flag processing = ATOMIC_FLAG_INIT;

    void setEditorStage(EditorStage stage, tresult result = Steinberg::kResultOk) noexcept {
        editorStage.store(static_cast<int>(stage), std::memory_order_release);
        editorResultCode.store(static_cast<long>(result), std::memory_order_release);
    }

    void setEditorError(std::string value) noexcept {
        try {
            std::lock_guard<std::mutex> lock(editorErrorMutex);
            editorError = std::move(value);
        } catch (...) {}
    }

    void clearEditorError() noexcept {
        try {
            std::lock_guard<std::mutex> lock(editorErrorMutex);
            editorError.clear();
        } catch (...) {}
    }

    std::string getEditorError() const noexcept {
        try {
            std::lock_guard<std::mutex> lock(editorErrorMutex);
            return editorError;
        } catch (...) {
            return {};
        }
    }

    ~RuntimeEffect() { shutdown(); }

    void shutdown() noexcept {
        ready.store(false, std::memory_order_release);
        bool hasEditor = false;
        {
            std::lock_guard<std::mutex> lock(editorThreadMutex);
            hasEditor = static_cast<bool>(editor) || editorAttached.load(std::memory_order_acquire) ||
                editorThread.joinable();
        }
        if (hasEditor && !onQtThread()) {
            if (!invokeOnQtThreadBlocking([this] { closeEditor(); })) closeEditor();
        } else {
            closeEditor();
        }
        const auto teardownPlugin = [this] {
            try {
                if (componentConnection && controllerConnection) {
                    componentConnection->disconnect(controllerConnection);
                    controllerConnection->disconnect(componentConnection);
                }
            } catch (...) {}
            componentConnection = nullptr;
            controllerConnection = nullptr;
            try { if (controller) controller->setComponentHandler(nullptr); } catch (...) {}
            try { if (controller && separateControllerInitialized) controller->terminate(); } catch (...) {}
            separateControllerInitialized = false;
            componentHandler = nullptr;
            try { if (processor) processor->setProcessing(false); } catch (...) {}
            try {
                if (component) {
                    component->setActive(false);
                    component->terminate();
                }
            } catch (...) {}
            processor = nullptr;
            controller = nullptr;
            component = nullptr;
            factory = nullptr;
        };
        if ((!component && !controller && !processor && !factory && !componentHandler &&
             !componentConnection && !controllerConnection) || onQtThread() || !QCoreApplication::instance() ||
            !invokeOnQtThreadBlocking(teardownPlugin)) teardownPlugin();
        plugFrame = nullptr;
        parameterChanges.clear();
        clearEditorError();
        setEditorStage(EditorStage::None);
        outputWritten.store(false, std::memory_order_release);
        ownerObserved.store(false, std::memory_order_release);
        configuredRate.store(0, std::memory_order_release);
        configuredBlock.store(0, std::memory_order_release);
        if (exit) {
            exit();
            exit = nullptr;
        }
        if (module) {
            FreeLibrary(module);
            module = nullptr;
        }
    }

    bool initialize(double sampleRate = 44100.0, std::size_t maxSamplesPerBlock = 16384,
                    const fs::path &requestedPath = {}, const std::string &requestedClassId = {},
                    const Vst3SelectionEntry *saved = nullptr) noexcept {
        try {
        shutdown();
        forceError = false;
        if (const char *forced = std::getenv("GPVST3_FORCE_P3_ERROR");
            forced && std::strcmp(forced, "1") == 0)
            forceError = true;
        const auto path = runtimeBinary(requestedPath.empty() ? runtimeModulePath() : requestedPath);
        std::error_code errorCode;
        if (path.empty() || !fs::is_regular_file(path, errorCode)) {
            error = "runtime_vst3_not_found";
            return false;
        }
        module = LoadLibraryW(path.wstring().c_str());
        if (!module) {
            error = "runtime_vst3_load_failed";
            return false;
        }
        if (const auto init = reinterpret_cast<InitModuleProc>(GetProcAddress(module, "InitDll"));
            init && !init()) {
            error = "runtime_vst3_init_failed";
            return false;
        }
        exit = reinterpret_cast<ExitModuleProc>(GetProcAddress(module, "ExitDll"));
        const auto getFactory = reinterpret_cast<GetFactoryProc>(GetProcAddress(module, "GetPluginFactory"));
        if (!getFactory) {
            error = "runtime_vst3_factory_missing";
            return false;
        }
        factory = Steinberg::owned(getFactory());
        if (!factory) {
            error = "runtime_vst3_factory_null";
            return false;
        }
        Steinberg::PClassInfo selected{};
        bool found = false;
        for (Steinberg::int32 index = 0; index < factory->countClasses(); ++index) {
            Steinberg::PClassInfo info{};
            if (!succeeded(factory->getClassInfo(index, &info))) continue;
            if (std::strcmp(info.category, "Audio Module Class") != 0) continue;
            if (!requestedClassId.empty() && uidString(info.cid) != requestedClassId) continue;
            if (auto factory2 = FUnknownPtr<Steinberg::IPluginFactory2>(factory.get())) {
                Steinberg::PClassInfo2 details{};
                if (!succeeded(factory2->getClassInfo2(index, &details)) ||
                    std::string(details.subCategories).find("Instrument") != std::string::npos) continue;
            }
            selected = info;
            found = true;
            break;
        }
        if (!found) {
            error = "runtime_vst3_audio_class_missing";
            return false;
        }
        name = selected.name;
        identity = saved ? *saved : Vst3SelectionEntry{path.u8string(), uidString(selected.cid)};
        loadedSelection = identity;
        auto *hostUnknown = static_cast<Steinberg::FUnknown *>(
            static_cast<Steinberg::Vst::IHostApplication *>(&host));
        Steinberg::MemoryStream componentState;
        bool componentStateReady = false;
        // Factories may construct GUI-affine objects. Keep construction on Qt,
        // but run processing initialization and state preparation on the worker.
        const bool componentCreated = invokeOnQtThreadBlocking([&] {
            editorTrace("component_create.before");
            IComponent *rawComponent = nullptr;
            if (!succeeded(factory->createInstance(selected.cid, IComponent::iid,
                                                   reinterpret_cast<void **>(&rawComponent))) || !rawComponent) {
                error = "runtime_vst3_component_failed";
                return;
            }
            component = Steinberg::owned(rawComponent);
            editorTrace("component_create.after");
            if (auto factory3 = FUnknownPtr<Steinberg::IPluginFactory3>(factory.get()))
                factory3->setHostContext(hostUnknown);
        });
        if (!componentCreated || !component) {
            if (error.empty()) error = "runtime_vst3_component_ui_thread_unavailable";
            return false;
        }
        {
            if (!succeeded(component->initialize(hostUnknown))) {
                error = "runtime_vst3_component_initialize_failed";
                return false;
            }
            editorTrace("component_initialize.after");
            for (int direction = Steinberg::Vst::kInput; direction <= Steinberg::Vst::kOutput; ++direction) {
                const auto count = component->getBusCount(Steinberg::Vst::kAudio, direction);
                for (Steinberg::int32 index = 0; index < count; ++index) {
                    Steinberg::Vst::BusInfo bus{};
                    if (succeeded(component->getBusInfo(Steinberg::Vst::kAudio, direction, index, bus)) &&
                        ((bus.flags & Steinberg::Vst::BusInfo::kDefaultActive) || index == 0))
                        component->activateBus(Steinberg::Vst::kAudio, direction, index, true);
                }
            }
            componentStateReady = succeeded(component->getState(&componentState));
            if (componentStateReady) {
                componentState.seek(0, Steinberg::IBStream::kIBSeekSet, nullptr);
                component->setState(&componentState);
            }
        }
        processor = FUnknownPtr<IAudioProcessor>(component.get());
        if (!processor) {
            error = "runtime_vst3_processor_missing";
            return false;
        }
        editorTrace("component_setup.after");
        bool separateController = false;
        const bool controllerSetupInvoked = invokeOnQtThreadBlocking([&] {
            TUID controllerClassId{};
            if (succeeded(component->getControllerClassId(controllerClassId))) {
                IEditController *rawController = nullptr;
                if (succeeded(factory->createInstance(controllerClassId, IEditController::iid,
                                                      reinterpret_cast<void **>(&rawController))) &&
                    rawController)
                    controller = Steinberg::owned(rawController);
            }
            separateController = static_cast<bool>(controller);
            if (!controller) controller = FUnknownPtr<IEditController>(component.get());
        });
        if (!controllerSetupInvoked) {
            error = "runtime_vst3_controller_ui_thread_unavailable";
            return false;
        }
        const auto initializeController = [&] {
            if (!controller) {
                setEditorError("runtime_vst3_controller_create_failed");
                return;
            }
            auto handler = Steinberg::owned(new RuntimeComponentHandler(this));
            // Single-component plug-ins already initialized their controller
            // through IComponent. Reinitializing returns kResultFalse.
            const auto controllerInit = separateController ? controller->initialize(hostUnknown)
                                                           : Steinberg::kResultOk;
            separateControllerInitialized = separateController && succeeded(controllerInit);
            if (!succeeded(controllerInit)) {
                controller = nullptr;
                setEditorError("runtime_vst3_controller_initialize_failed_" +
                    std::to_string(static_cast<long>(controllerInit)));
                return;
            }
            if (handler && succeeded(controller->setComponentHandler(handler.get()))) {
                componentHandler = std::move(handler);
                if (separateController) {
                    componentConnection = FUnknownPtr<Steinberg::Vst::IConnectionPoint>(component.get());
                    controllerConnection = FUnknownPtr<Steinberg::Vst::IConnectionPoint>(controller.get());
                    if (componentConnection && controllerConnection) {
                        componentConnection->connect(controllerConnection);
                        controllerConnection->connect(componentConnection);
                    }
                }
            } else {
                setEditorError("runtime_vst3_component_handler_failed");
            }
        };
        initializeController();
        if (controller && componentStateReady) {
            componentState.seek(0, Steinberg::IBStream::kIBSeekSet, nullptr);
            controller->setComponentState(&componentState);
        }
        editorTrace("controller_setup.after");
        if (!controller) {
            const auto controllerError = getEditorError();
            if (!controllerError.empty()) error = controllerError;
        }
        if (!parameterChanges.prepare(controller.get())) {
            error = "runtime_vst3_parameter_setup_failed";
            return false;
        }
        if (saved && !restoreState(*saved)) return false;
        editorTrace("restore_state.after");
        Steinberg::Vst::ProcessSetup setup{};
        setup.processMode = Steinberg::Vst::kRealtime;
        setup.symbolicSampleSize = Steinberg::Vst::kSample32;
        setup.maxSamplesPerBlock = static_cast<Steinberg::int32>(maxSamplesPerBlock);
        setup.sampleRate = sampleRate;
        const bool processingSetup = succeeded(processor->setupProcessing(setup)) &&
            succeeded(component->setActive(true)) && succeeded(processor->setProcessing(true));
        if (!processingSetup || !scratch.prepare(2, maxSamplesPerBlock)) {
            error = "runtime_vst3_processing_setup_failed";
            return false;
        }
        editorTrace("processing_setup.after");
        ready.store(true, std::memory_order_release);
        configuredRate.store(static_cast<int>(sampleRate), std::memory_order_release);
        configuredBlock.store(maxSamplesPerBlock, std::memory_order_release);
        error.clear();
        return true;
        } catch (...) {
            shutdown();
            error = "runtime_vst3_exception";
            return false;
        }
    }

    bool restoreState(const Vst3SelectionEntry &saved) {
        const auto restored = [this](tresult result, const char *stage) {
            if (succeeded(result)) return true;
            error = std::string("runtime_vst3_state_restore_failed:") + stage + ":" +
                    std::to_string(static_cast<long>(result));
            return false;
        };
        if (!saved.componentState.empty()) {
            Steinberg::MemoryStream stream(const_cast<unsigned char *>(saved.componentState.data()),
                                           saved.componentState.size());
            stream.seek(0, Steinberg::IBStream::kIBSeekSet, nullptr);
            const auto componentResult = component ? component->setState(&stream) : Steinberg::kNoInterface;
            if (!restored(componentResult, "component")) return false;
            stream.seek(0, Steinberg::IBStream::kIBSeekSet, nullptr);
            if (controller) {
                // Some controllers rely on the component's state and do not
                // implement a separate component-state copy (e.g. Mateus Asato).
                // Keep genuine restore failures fatal; kNotImplemented alone
                // does not invalidate the component state restored above.
                const auto copied = controller->setComponentState(&stream);
                if (copied != Steinberg::kNotImplemented && !restored(copied, "component_controller"))
                    return false;
            }
        }
        if (!saved.controllerState.empty()) {
            Steinberg::MemoryStream stream(const_cast<unsigned char *>(saved.controllerState.data()),
                                           saved.controllerState.size());
            stream.seek(0, Steinberg::IBStream::kIBSeekSet, nullptr);
            const auto result = controller ? controller->setState(&stream) : Steinberg::kNoInterface;
            if (!restored(result, "controller")) return false;
        }
        return true;
    }

    Vst3SelectionEntry captureState() {
        auto result = identity;
        Steinberg::MemoryStream stream;
        if (component && succeeded(component->getState(&stream)) && stream.getSize() > 0)
            result.componentState.assign(stream.getData(), stream.getData() + stream.getSize());
        Steinberg::MemoryStream control;
        if (controller && succeeded(controller->getState(&control))) {
            result.controllerState.clear();
            if (control.getSize() > 0)
                result.controllerState.assign(control.getData(), control.getData() + control.getSize());
        }
        identity = result;
        // Parameter edits have already been mirrored to the independent input
        // instance. Keep its saved-state identity in sync for warm reuse.
        if (const auto mirror = inputParameterMirror.lock()) mirror->identity = result;
        return result;
    }

    bool queueParameter(ParamID id, ParamValue value) noexcept {
        if (!std::isfinite(value) || !parameterChanges.publish(id, value)) return false;
        parameterEdits.fetch_add(1, std::memory_order_relaxed);
        mirrorInputParameter(this, id, value);
        return true;
    }

    bool openEditor(HWND parentWindow) noexcept {
        editorTrace("open.begin");
        setEditorStage(EditorStage::Requested);
        if (!onQtThread()) {
            setEditorError("editor_ui_thread_required");
            setEditorStage(EditorStage::Failed, Steinberg::kNotImplemented);
            return false;
        }
        if (!ready.load(std::memory_order_acquire) || !controller || !parentWindow) {
            if (!controller) setEditorStage(EditorStage::ControllerMissing, Steinberg::kNoInterface);
            if (getEditorError().empty()) setEditorError("editor_host_unavailable");
            return false;
        }
        if (!IsWindow(parentWindow)) {
            setEditorError("editor_host_invalid");
            setEditorStage(EditorStage::Failed, Steinberg::kInvalidArgument);
            return false;
        }
        bool alreadyAttached = false;
        {
            std::lock_guard<std::mutex> lock(editorThreadMutex);
            alreadyAttached = static_cast<bool>(editor) &&
                editorAttached.load(std::memory_order_acquire) && editorParent == parentWindow;
        }
        if (alreadyAttached) {
            try {
                setEditorStage(EditorStage::Focus);
                const auto focused = editor->onFocus(true);
                if (!succeeded(focused)) {
                    setEditorError("editor_focus_failed");
                    setEditorStage(EditorStage::Failed, focused);
                    return false;
                }
            } catch (...) { setEditorError("editor_focus_failed"); setEditorStage(EditorStage::Failed); return false; }
            return true;
        }
        try {
            editorTrace("open.close.before");
            closeEditor();
            editorTrace("open.close.after");
            // Commercial editors can perform a long native/UI bootstrap in
            // createView or attached. Keep those calls off Guitar Pro's Qt
            // thread while a nested event loop keeps the host responsive.
            setEditorStage(EditorStage::CreateView);
            const auto contentScale = gpvst3::ui::nativeEditorScale(
                reinterpret_cast<void *>(parentWindow));
            {
                std::lock_guard<std::mutex> lock(editorThreadMutex);
                editorThreadStop = false;
                editorThreadReady = false;
                editorThreadSucceeded = false;
            }
            QEventLoop loop;
            editorThread = std::thread([this, parentWindow, contentScale, &loop] {
                struct ComApartment {
                    HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
                    ~ComApartment() { if (SUCCEEDED(result)) CoUninitialize(); }
                } comApartment;
                Steinberg::IPtr<Steinberg::IPlugView> workerEditor;
                Steinberg::IPtr<Steinberg::IPlugFrame> workerFrame;
                ViewRect workerRect{};
                tresult workerResult = Steinberg::kResultFalse;
                std::string workerError;
                try {
                    editorTrace("create_view.before");
                    auto *rawView = controller->createView("editor");
                    editorTrace(rawView ? "create_view.after.ok" : "create_view.after.null");
                    if (!rawView) {
                        workerError = "editor_view_unavailable";
                        setEditorStage(EditorStage::Failed, Steinberg::kNoInterface);
                    } else {
                        workerEditor = Steinberg::owned(rawView);
                        setEditorStage(EditorStage::PlatformCheck);
                        editorTrace("platform.before");
                        const auto platformResult = workerEditor->isPlatformTypeSupported(
                            Steinberg::kPlatformTypeHWND);
                        editorTrace("platform.after");
                        if (!succeeded(platformResult)) {
                            workerError = "editor_hwnd_unsupported";
                            workerResult = platformResult;
                            setEditorStage(EditorStage::Failed, platformResult);
                        } else {
                            workerFrame = Steinberg::owned(new RuntimePlugFrame(parentWindow));
                            setEditorStage(EditorStage::SetFrame);
                            editorTrace("set_frame.before");
                            const auto frameResult = workerFrame
                                ? workerEditor->setFrame(workerFrame.get()) : Steinberg::kOutOfMemory;
                            editorTrace("set_frame.after");
                            if (!workerFrame || !succeeded(frameResult)) {
                                workerError = "editor_frame_failed";
                                workerResult = frameResult;
                                setEditorStage(EditorStage::Failed, frameResult);
                            } else {
                                if (auto scale = FUnknownPtr<Steinberg::IPlugViewContentScaleSupport>(
                                        workerEditor.get())) {
                                    editorTrace("scale.before");
                                    scale->setContentScaleFactor(static_cast<float>(contentScale));
                                    editorTrace("scale.after");
                                }
                                setEditorStage(EditorStage::GetSize);
                                editorTrace("get_size.before");
                                const auto sizeResult = workerEditor->getSize(&workerRect);
                                editorTrace("get_size.after");
                                if (!succeeded(sizeResult)) workerRect = ViewRect(0, 0, 420, 260);
                                if (!invokeOnQtThreadBlocking([&] {
                                        gpvst3::ui::resizeNativeEditor(
                                            reinterpret_cast<void *>(parentWindow),
                                            workerRect.getWidth(), workerRect.getHeight());
                                    })) {
                                    workerError = "editor_host_resize_failed";
                                    workerResult = Steinberg::kResultFalse;
                                    setEditorStage(EditorStage::Failed, workerResult);
                                } else {
                                    setEditorStage(EditorStage::Attached);
                                    editorTrace("attached.before");
                                    const bool attachedInvoked = invokeOnQtThreadBlocking([&] {
                                        workerResult = workerEditor->attached(
                                            reinterpret_cast<void *>(parentWindow), Steinberg::kPlatformTypeHWND);
                                        if (succeeded(workerResult)) workerEditor->onSize(&workerRect);
                                    });
                                    editorTrace("attached.after");
                                    if (!attachedInvoked || !succeeded(workerResult)) {
                                        workerError = "editor_attach_failed";
                                        if (!attachedInvoked) workerResult = Steinberg::kResultFalse;
                                        setEditorStage(EditorStage::Failed, workerResult);
                                    }
                                }
                            }
                        }
                    }
                } catch (...) {
                    workerError = "editor_exception";
                    setEditorStage(EditorStage::Failed);
                    editorTrace("open.exception");
                }
                const bool succeededAttach = workerError.empty() && workerEditor && succeeded(workerResult);
                {
                    std::lock_guard<std::mutex> lock(editorThreadMutex);
                    editorThreadReady = true;
                    editorThreadSucceeded = succeededAttach;
                    if (succeededAttach) {
                        editor = workerEditor;
                        plugFrame = workerFrame;
                        editorParent = parentWindow;
                        editorAttached.store(true, std::memory_order_release);
                    } else {
                        setEditorError(workerError.empty() ? "editor_attach_failed" : workerError);
                        if (workerResult == Steinberg::kResultFalse)
                            setEditorStage(EditorStage::Failed, workerResult);
                    }
                }
                if (succeededAttach) {
                    QMetaObject::invokeMethod(&loop, [&loop] { loop.quit(); }, Qt::QueuedConnection);
                    std::unique_lock<std::mutex> lock(editorThreadMutex);
                    editorThreadCv.wait(lock, [this] { return editorThreadStop; });
                    lock.unlock();
                    if (parentWindow && IsWindow(parentWindow)) {
                        editorTrace("removed.before");
                        invokeOnQtThreadBlocking([&] {
                            try { workerEditor->removed(); } catch (...) {}
                        });
                        editorTrace("removed.after");
                        editorTrace("clear_frame.before");
                        invokeOnQtThreadBlocking([&] {
                            try { workerEditor->setFrame(nullptr); } catch (...) {}
                        });
                        editorTrace("clear_frame.after");
                    } else {
                        editorTrace("removed.skip_host_closed");
                    }
                    workerFrame = nullptr;
                    workerEditor = nullptr;
                    std::lock_guard<std::mutex> clearLock(editorThreadMutex);
                    editor = nullptr;
                    plugFrame = nullptr;
                    editorAttached.store(false, std::memory_order_release);
                    if (editorCloseLoop) {
                        auto *closeLoop = editorCloseLoop;
                        QMetaObject::invokeMethod(closeLoop, [closeLoop] { closeLoop->quit(); },
                                                  Qt::QueuedConnection);
                    }
                } else {
                    invokeOnQtThreadBlocking([&] {
                        try { if (workerEditor) workerEditor->setFrame(nullptr); } catch (...) {}
                    });
                    workerEditor = nullptr;
                    workerFrame = nullptr;
                    QMetaObject::invokeMethod(&loop, [&loop] { loop.quit(); }, Qt::QueuedConnection);
                }
            });
            loop.exec();
            bool workerSucceeded = false;
            {
                std::lock_guard<std::mutex> lock(editorThreadMutex);
                workerSucceeded = editorThreadReady && editorThreadSucceeded;
            }
            if (!workerSucceeded) {
                if (editorThread.joinable()) editorThread.join();
                if (getEditorError().empty()) setEditorError("editor_attach_failed");
                return false;
            }
            setEditorStage(EditorStage::Visible);
            editorTrace("open.visible");
            clearEditorError();
            return true;
        } catch (...) {
            editorTrace("open.exception");
            closeEditor();
            setEditorError("editor_exception");
            setEditorStage(EditorStage::Failed);
            return false;
        }
    }

    void closeEditor() noexcept {
        std::thread threadToJoin;
        bool hadEditor = false;
        QEventLoop closeLoop;
        const bool keepQtResponsive = onQtThread();
        {
            std::lock_guard<std::mutex> lock(editorThreadMutex);
            editorParent = nullptr;
            hadEditor = editorAttached.load(std::memory_order_acquire) ||
                static_cast<bool>(editor) || editorThread.joinable();
            editorCloseLoop = keepQtResponsive && editorThread.joinable() && editorThreadSucceeded
                ? &closeLoop : nullptr;
            editorThreadStop = true;
            editorThreadCv.notify_all();
            if (editorThread.joinable()) threadToJoin = std::move(editorThread);
        }
        if (threadToJoin.joinable()) {
            // removed() may marshal native/Qt destruction back to the host.
            // Pump Qt until the editor thread has completed that contract;
            // joining it while blocking Qt deadlocks Neural DSP editors.
            if (editorCloseLoop) closeLoop.exec();
            try { threadToJoin.join(); } catch (...) {}
        }
        {
            std::lock_guard<std::mutex> lock(editorThreadMutex);
            editor = nullptr;
            plugFrame = nullptr;
            editorThreadReady = false;
            editorThreadSucceeded = false;
            editorCloseLoop = nullptr;
            editorAttached.store(false, std::memory_order_release);
        }
        if (hadEditor) setEditorStage(EditorStage::Removed);
    }

    bool reconfigure(double sampleRate, std::size_t maxSamplesPerBlock) noexcept {
        if (!ready.load(std::memory_order_acquire) || !processor || maxSamplesPerBlock == 0)
            return false;
        try {
            Steinberg::Vst::ProcessSetup setup{};
            setup.processMode = Steinberg::Vst::kRealtime;
            setup.symbolicSampleSize = Steinberg::Vst::kSample32;
            setup.maxSamplesPerBlock = static_cast<Steinberg::int32>(maxSamplesPerBlock);
            setup.sampleRate = sampleRate;
            bool configured = false;
            const bool invoked = invokeOnQtThreadBlocking([&] {
                processor->setProcessing(false);
                component->setActive(false);
                configured = succeeded(processor->setupProcessing(setup)) &&
                    succeeded(component->setActive(true)) &&
                    succeeded(processor->setProcessing(true));
            });
            if (!invoked || !configured || !scratch.prepare(2, maxSamplesPerBlock)) {
                error = "runtime_vst3_reconfigure_failed";
                ready.store(false, std::memory_order_release);
                return false;
            }
        } catch (...) {
            error = "runtime_vst3_reconfigure_exception";
            ready.store(false, std::memory_order_release);
            return false;
        }
        error.clear();
        configuredRate.store(static_cast<int>(sampleRate), std::memory_order_release);
        configuredBlock.store(maxSamplesPerBlock, std::memory_order_release);
        ready.store(true, std::memory_order_release);
        return true;
    }

    bool processBlock(const audio::BlockView &block) noexcept {
        if (!ready.load(std::memory_order_acquire) || !processor || forceError ||
            block.frameCount == 0 || block.frameCount > scratch.frameCapacity() ||
            block.channelCount == 0 || block.channelCount > scratch.channelCount()) return false;
        const auto channels = block.channelCount;
        for (std::size_t channel = 0; channel < channels; ++channel) {
            const auto *input = block.generatedChannels ? block.generatedChannels[channel]
                                                        : (block.inputChannels ? block.inputChannels[channel]
                                                                                : (block.channels ? block.channels[channel]
                                                                                                   : nullptr));
            if (!input) return false;
        }
        if (processing.test_and_set(std::memory_order_acquire)) return false;
        audio::ProcessResult result{};
        try {
            parameterChanges.drain();
            result = audio::process(*processor, block, scratch, false, &parameterChanges);
            parameterChanges.clear();
        } catch (...) {
            parameterChanges.clear();
            processing.clear(std::memory_order_release);
            return false;
        }
        processing.clear(std::memory_order_release);
        if (result.outputWritten) outputWritten.store(true, std::memory_order_release);
        if (result.ownerPointerObserved) ownerObserved.store(true, std::memory_order_release);
        if (result.processed) processedBlocks.fetch_add(1, std::memory_order_relaxed);
        return result.processed;
    }

    static bool processCallback(void *context, const audio::BlockView &block) noexcept {
        return static_cast<RuntimeEffect *>(context)->processBlock(block);
    }
};

Steinberg::tresult PLUGIN_API RuntimeComponentHandler::beginEdit(ParamID) {
    return Steinberg::kResultTrue;
}

Steinberg::tresult PLUGIN_API RuntimeComponentHandler::performEdit(ParamID id,
                                                                    ParamValue value) {
    if (!owner_) return Steinberg::kResultFalse;
    return owner_->queueParameter(id, value) ? Steinberg::kResultTrue : Steinberg::kResultFalse;
}

Steinberg::tresult PLUGIN_API RuntimeComponentHandler::endEdit(ParamID) {
    return Steinberg::kResultTrue;
}

Steinberg::tresult PLUGIN_API RuntimeComponentHandler::restartComponent(Steinberg::int32 flags) {
    if (!owner_ || !owner_->controller) return Steinberg::kResultFalse;
    bool handled = false;
    if ((flags & Steinberg::Vst::kLatencyChanged) && owner_->independentInput.load()) {
        notifyInputLatencyChange();
        handled = true;
    }
    if (flags & Steinberg::Vst::kParamValuesChanged) {
        for (int i = 0; i < owner_->controller->getParameterCount(); ++i) {
            Steinberg::Vst::ParameterInfo info{};
            if (succeeded(owner_->controller->getParameterInfo(i, info)))
                owner_->queueParameter(info.id, owner_->controller->getParamNormalized(info.id));
        }
        handled = true;
    }
    return handled ? Steinberg::kResultOk : Steinberg::kNotImplemented;
}

bool matchesEffect(const RuntimeEffect &effect, const Vst3SelectionEntry &entry) noexcept {
    const auto matches = [&](const Vst3SelectionEntry &saved) {
        return saved.module == entry.module && saved.classId == entry.classId &&
            saved.componentState == entry.componentState && saved.controllerState == entry.controllerState;
    };
    return effect.ready.load(std::memory_order_acquire) &&
        (matches(effect.identity) || matches(effect.loadedSelection));
}

bool g_listenerProbeInstalled = false;
const std::uint8_t *g_listenerExecutableBase = nullptr;
#ifdef GPVST3_P13_PROBE_BUILD
input::probe::Recorder g_inputProbe;
input::pcmprobe::Recorder g_inputPcmProbe;
input::timingprobe::Recorder g_inputTimingProbe;
thread_local input::timingprobe::Recorder::Ticket *g_inputTimingTicket = nullptr;
input::probe::Recorder g_listenerProbe;
struct ListenerProbeExtra {
    std::uintptr_t state = 0;
    bool enabled = false;
    bool stateStable = false;
    float inputPeakBefore = 0;
    float inputPeakAfter = 0;
    float outputPeakAfter = 0;
    std::uint32_t peakNonFiniteMask = 0;
    std::array<float, input::probe::kSampleFrames * input::probe::kSampleChannels> outputBefore{};
    std::uint32_t outputBeforeNonFiniteMask = 0;
    bool sinkApplied = false;
    bool sinkBusy = false;
    std::array<float, input::probe::kSampleFrames * input::probe::kSampleChannels> sinkOutput{};
    std::uint32_t sinkNonFiniteMask = 0;
};
std::array<ListenerProbeExtra, input::probe::kCapacity> g_listenerProbeExtra{};
std::atomic<std::uint64_t> g_listenerProbeSequence{0};
thread_local std::uint64_t g_probeOuterSequence = 0;
bool g_listenerProbeRequested = false;
std::atomic<bool> g_rseProbeInstalled{false};
const std::uint8_t *g_rseProbeBase = nullptr;
bool g_listenerSinkRequested = false;
// Test-only, bounded AudioUnit output diversion. The original capture, DSP,
// meter updates, return value and caller output are preserved. An overlapping
// invocation forwards unchanged instead of waiting or sharing writable memory.
std::array<float, 32768 * 2> g_listenerSink{};
std::array<float, 32768 * 2> g_listenerCallerBefore{};
std::array<float, 32768 * 2> g_drainRseSum{};
std::atomic_flag g_listenerSinkBusy = ATOMIC_FLAG_INIT;
std::once_flag g_inputProbeOnce;
std::uint64_t g_inputProbeStart = 0;
bool g_inputProbeSilence = false;
bool g_monitorLatencyProbe = false;
std::atomic<bool> g_monitorLatencyMappingValid{false};
const std::array<float, portaudio::kMaxFrames * 2> g_inputProbeZeros{};

input::timingprobe::Identity timingIdentity() noexcept {
    const auto identity = asioprobe::currentCallback();
    return {identity.generation, identity.rateRevision, identity.actualRate, identity.rateValidated};
}

class InputTimingScope final {
public:
    InputTimingScope(unsigned long frames, unsigned long status) noexcept : previous_(g_inputTimingTicket) {
        g_inputTimingTicket = nullptr;
        if (!g_inputTimingProbe.enabled()) return;
        const auto started = steadyNanoseconds();
        if (started >= g_inputProbeStart && g_inputTimingProbe.begin(ticket_, started, frames, status, timingIdentity()))
            g_inputTimingTicket = &ticket_;
    }
    ~InputTimingScope() {
        if (ticket_.active) {
            const auto identity = timingIdentity();
            g_inputTimingProbe.finish(ticket_, steadyNanoseconds(), result, identity);
        }
        g_inputTimingTicket = previous_;
    }
    int result = -1;
private:
    input::timingprobe::Recorder::Ticket ticket_;
    input::timingprobe::Recorder::Ticket *previous_;
};

// One bounded, uninterrupted experiment, serialized without waiting. The
// listener and SRC records are attached to this outer callback, including
// callbacks which consume the ring without invoking either inner function.
constexpr std::size_t kDrainProbeCapacity = 4096;
struct DrainProbeRecord {
    input::drainprobe::Snapshot before{}, after{};
    input::drain::Snapshot drain{};
    asioprobe::SrcObservation src{};
    std::uint64_t sequence = 0, timestamp = 0, ended = 0, consumed = 0;
    std::uint32_t frames = 0, thread = 0, listenerCalls = 0, listenerSunk = 0;
    std::uint32_t statusFlags = 0, callerChangedSamples = 0, nonFiniteSamples = 0;
    double captureEnergy = 0, nativeEnergy = 0, callerEnergy = 0, outputEnergy = 0;
    double rseEnergy = 0;
    std::uint32_t rseCalls = 0, rseMismatchSamples = 0, rseComparedSamples = 0, sourceObserverCalls = 0;
    std::int64_t rseRequestedFrames = 0;
    float parentGain = 0;
    bool rseValid = true;
    std::uint64_t monitorToken = 0;
    std::uint32_t overlayComparedSamples = 0, overlayMismatchSamples = 0, listenerPeakSamples = 0;
    double preOverlayEnergy = 0, overlayEnergy = 0, inputPeak = 0, outputPeak = 0;
    float nativePreGain = 0;
    bool overlayCommitted = false, overlaySuppressed = false;
    input::drain::Snapshot productionDrain{};
    bool srcObserved = false, listenerValid = true, valid = false;
    int originalResult = -1;
};
struct DrainProbeSlot {
    DrainProbeRecord record{};
    std::atomic<bool> published{false};
};
bool g_drainProbeRequested = false;
bool g_overlayProbeRequested = false;
bool g_overlayTransitionProbe = false;
std::array<float, portaudio::kMaxFrames * 2> g_overlayProbeBefore{};
std::array<DrainProbeSlot, kDrainProbeCapacity> g_drainProbeRecords{};
std::atomic_flag g_drainProbeBusy = ATOMIC_FLAG_INIT;
std::atomic<std::size_t> g_drainProbeCount{0}, g_drainProbeOverlaps{0};
input::drain::Tracker g_drainTracker;
bool g_drainProbeInvalid = false;
thread_local DrainProbeRecord *g_currentDrainRecord = nullptr;

void configureInputProbe() {
    std::call_once(g_inputProbeOnce, [] {
        const auto enabled = [](const char *name) {
            const auto *value = std::getenv(name);
            return value && std::strcmp(value, "1") == 0;
        };
        unsigned long delay = 8000;
        if (const auto *value = std::getenv("GPVST3_P13_PROBE_DELAY_MS")) {
            char *end = nullptr;
            const auto parsed = std::strtoul(value, &end, 10);
            if (end != value && *end == '\0' && parsed <= 60000) delay = parsed;
        }
        g_inputProbeStart = steadyNanoseconds() + static_cast<std::uint64_t>(delay) * 1000000;
        g_overlayTransitionProbe = enabled("GPVST3_P13_OVERLAY_TRANSITION") && enabled("GPVST3_P13_OVERLAY_PROBE");
        g_inputProbeSilence = enabled("GPVST3_P13_SILENCE_INPUT") && enabled("GPVST3_P13_COPY_SCORE");
        g_monitorLatencyProbe = enabled("GPVST3_P13_MONITOR_LATENCY") && enabled("GPVST3_P13_PCM_PROBE") &&
            enabled("GPVST3_P13_COPY_SCORE") && enabled("GPVST3_P13_PROBE");
        g_inputProbe.configure({enabled("GPVST3_P13_PROBE"), true});
        g_inputTimingProbe.configure(enabled("GPVST3_P13_PROBE"), g_inputProbeStart);
        if (enabled("GPVST3_P13_PROBE")) asioprobe::configureTiming(g_inputProbeStart);
        g_inputPcmProbe.configure({enabled("GPVST3_P13_PROBE") &&
            enabled("GPVST3_P13_PCM_PROBE") && enabled("GPVST3_P13_COPY_SCORE") &&
            !enabled("GPVST3_P13_SILENCE_INPUT") && !enabled("GPVST3_P13_LISTENER_SINK")});
        g_listenerProbeRequested = enabled("GPVST3_P13_PROBE") && enabled("GPVST3_P13_LISTENER_PROBE");
        g_listenerSinkRequested = g_listenerProbeRequested && enabled("GPVST3_P13_LISTENER_SINK") &&
            enabled("GPVST3_P13_COPY_SCORE");
        g_drainProbeRequested = g_listenerSinkRequested && enabled("GPVST3_P13_DRAIN_PROBE") &&
            enabled("GPVST3_P13_STREAM_PROBE") && !g_inputProbeSilence;
        g_overlayProbeRequested = enabled("GPVST3_P13_OVERLAY_PROBE") && enabled("GPVST3_P13_PROBE") &&
            enabled("GPVST3_P13_COPY_SCORE") && !g_drainProbeRequested && !g_inputProbeSilence;
        g_listenerExecutableBase = reinterpret_cast<const std::uint8_t *>(GetModuleHandleW(nullptr));
        g_listenerProbe.configure({g_listenerProbeRequested, true});
    });
}
#endif

// Slot handoffs own the current processing order; the pool owns every loaded
// instance for the lifetime of its scope, including disabled effects. Access
// is serialized by selectionMutex and never happens in the audio callback.
struct EffectPool {
    static constexpr std::size_t kMaxWarmInstances = 16;
    std::vector<std::shared_ptr<RuntimeEffect>> effects;
    std::size_t evictions = 0;

    bool hasPreloaded() const noexcept {
        return std::any_of(effects.begin(), effects.end(), [](const auto &effect) {
            return effect->preloaded && effect->ready.load(std::memory_order_acquire);
        });
    }

    bool covers(const std::vector<Vst3SelectionEntry> &entries) const noexcept {
        return std::all_of(entries.begin(), entries.end(), [&](const auto &entry) {
            return std::any_of(effects.begin(), effects.end(), [&](const auto &effect) {
                return effect->preloaded && matchesEffect(*effect, entry);
            });
        });
    }

    std::shared_ptr<RuntimeEffect> acquire(const Vst3SelectionEntry &entry, double rate,
                                         std::size_t maxBlock, std::string *error,
                                         bool preload = false) {
        for (const auto &effect : effects) {
            // Catalog refresh cannot overwrite a live or edited state.
            const bool sameIdentity = effect->ready.load(std::memory_order_acquire) &&
                effect->identity.module == entry.module && effect->identity.classId == entry.classId;
            if (preload ? sameIdentity : matchesEffect(*effect, entry)) {
                if (!preload && (effect->configuredRate.load() != static_cast<int>(rate) ||
                                 effect->configuredBlock.load() < maxBlock) &&
                    !effect->reconfigure(rate, maxBlock)) {
                    if (error) *error = effect->error;
                    return {};
                }
                return effect;
            }
            // A dormant preload may have been created without state. Apply the
            // requested state on first explicit enable, preserving the same
            // processor while still rejecting malformed state chunks.
            if (!preload && sameIdentity && effect->preloaded) {
                if (!effect->restoreState(entry)) {
                    if (error) *error = effect->error;
                    return {};
                }
                effect->identity = entry;
                effect->preloaded = false;
                return effect;
            }
        }
        if (effects.size() >= kMaxWarmInstances) {
            // Evict only detached dormant instances. Active slots and edited
            // processors remain strongly referenced and are never reclaimed
            // behind the callback's back. If every entry is live, fail fast
            // so the caller can report a bounded resource error.
            const auto victim = std::find_if(effects.begin(), effects.end(), [](const auto &candidate) {
                return candidate && candidate->preloaded && candidate.use_count() == 1;
            });
            if (victim == effects.end()) {
                if (error) *error = "runtime_vst3_warm_cache_full";
                return {};
            }
            effects.erase(victim);
            ++evictions;
        }
        auto effect = std::make_shared<RuntimeEffect>();
        if (!effect->initialize(rate, maxBlock, fs::u8path(entry.module), entry.classId, &entry)) {
            if (error) *error = effect->error;
            return {};
        }
        effect->preloaded = preload;
        effects.push_back(effect);
        return effect;
    }
};

// A selected P7 list is prepared as one immutable callback context. Each
// processor writes into the next preallocated planar buffer; the final one
// writes to the host buffer. Rebuilding happens off the audio callback.
struct SelectionSlot {
    static constexpr std::size_t kMaxEffects = 8;
    std::shared_ptr<RuntimeEffect> effects[kMaxEffects];
    audio::PlanarBuffer pipeline[2];
    std::size_t count = 0;
    std::atomic<int> failedIndex{-1};

    bool matches(const std::vector<Vst3SelectionEntry> &entries, double rate,
                 std::size_t maxBlock) const noexcept {
        if (entries.empty() || count != entries.size()) return false;
        for (std::size_t i = 0; i < count; ++i) {
            const auto &effect = effects[i];
            if (!effect || !matchesEffect(*effect, entries[i]) ||
                effect->configuredRate.load() != static_cast<int>(rate) ||
                effect->configuredBlock.load() < maxBlock) return false;
        }
        return true;
    }

    void shutdown() noexcept {
        for (auto &effect : effects) effect.reset();
        count = 0;
        failedIndex.store(-1, std::memory_order_release);
    }

    bool prepare(const std::vector<Vst3SelectionEntry> &entries, double rate,
                 std::size_t maxBlock, EffectPool &pool, std::string *error) {
        shutdown();
        if (entries.empty()) return true;
        if (entries.size() > kMaxEffects || !pipeline[0].prepare(2, maxBlock) ||
            !pipeline[1].prepare(2, maxBlock)) {
            if (error) *error = "runtime_vst3_chain_buffer_failed";
            return false;
        }
        for (std::size_t index = 0; index < entries.size(); ++index) {
            effects[index] = pool.acquire(entries[index], rate, maxBlock, error);
            if (!effects[index]) {
                shutdown();
                return false;
            }
        }
        count = entries.size();
        return true;
    }

    void markActive() noexcept {
        for (std::size_t i = 0; i < count; ++i) effects[i]->preloaded = false;
    }

    bool processBlock(const audio::BlockView &block) noexcept {
        if (count == 0 || block.channelCount == 0 || block.channelCount > 2 ||
            block.frameCount == 0 || block.frameCount > pipeline[0].frameCapacity()) return false;
        const float *const *source = block.generatedChannels ? block.generatedChannels
            : (block.inputChannels ? block.inputChannels : block.channels);
        if (!source) return false;
        for (std::size_t index = 0; index < count; ++index) {
            const bool last = index + 1 == count;
            float **destination = last ? (block.outputChannels ? block.outputChannels : block.channels)
                                       : pipeline[index & 1].outputChannels();
            if (!destination) return false;
            const audio::BlockView view{source, nullptr, destination, nullptr,
                                        block.channelCount, block.frameCount,
                                        block.sampleRate, block.blockSize, block.owner,
                                        block.sequence, block.outputWritable};
            if (effects[index]->configuredRate.load(std::memory_order_acquire) != static_cast<int>(block.sampleRate))
                return false;
            if (!effects[index]->processBlock(view)) {
                failedIndex.store(static_cast<int>(index), std::memory_order_release);
                return false;
            }
            if (!last) source = pipeline[index & 1].outputChannels();
        }
        return true;
    }

    static bool processCallback(void *context, const audio::BlockView &block) noexcept {
        return static_cast<SelectionSlot *>(context)->processBlock(block);
    }
};

struct InputMonitorSlot {
    SelectionSlot selection;
    EffectPool pool;
    input::Router router;
    double rate = 0;
    std::uint64_t generation = 0, revision = 0;
    float gain = 0.5f;
    static bool process(void *context, const audio::BlockView &block) noexcept {
        auto &slot = *static_cast<InputMonitorSlot *>(context);
        if (slot.selection.count ? !slot.selection.processBlock(block) : !audio::bypass(block)) return false;
        for (std::size_t c = 0; c < block.channelCount; ++c)
            for (std::size_t f = 0; f < block.frameCount; ++f)
                block.outputChannels[c][f] *= slot.gain;
        return true;
    }
};

// All owning objects belong exclusively to the input scope. No input mirror,
// global preload or track binding ever references either pool.
struct InputMonitorRuntime {
    input::MonitorExchange exchange;
    InputMonitorSlot monitorSlots[2];
    std::vector<std::shared_ptr<RuntimeEffect>> warmEffects; // selection worker only
    std::vector<Vst3SelectionEntry> desired;
    state::InputMonitorSettings settings;
    std::string error;
    int retained = -1;
    bool intentLoaded = false; // Qt thread, independent of score lifecycle.
    std::atomic<bool> nativeListenerKnown{false}, nativeListenerEnabled{false};
    std::atomic<int> phase{0}; // off, legacy, preparing, draining, active, muted, host_limited
    std::atomic<std::uint64_t> callbackPhase{0};
    std::atomic<bool> nativeSuppressed{false};
    std::atomic<std::uint64_t> blocks{0}, errors{0}, clipped{0};
    std::atomic<std::uint64_t> statusBlocks{0};
    std::atomic<std::uint64_t> configurationRejectedBlocks{0}, configurationToken{0};
    std::atomic<int> callbackFault{0}, listenerFaultFlags{0};
    std::atomic<std::int64_t> listenerReturnedFrames{0}, listenerRequestedFrames{0};
    std::atomic<std::uint32_t> faultListenerCalls{0}, faultSrcCalls{0};
    std::atomic<unsigned long> frames{0};
    std::atomic<std::size_t> driverFrames{0}, processFrames{0}, channels{0};
    std::atomic<int> actualRate{0};
    std::atomic<HANDLE> event{nullptr};
    std::atomic<bool> statusChanged{false};
    std::atomic<bool> reconfigureRequested{false};
    std::atomic_flag processing = ATOMIC_FLAG_INIT;
    void publishCallbackPhase(std::uint64_t token, int value) noexcept {
        const auto word = input::MonitorExchange::versionOf(token) | static_cast<std::uint64_t>(value);
        if (callbackPhase.exchange(word) == word) return;
        statusChanged.store(true, std::memory_order_release);
        if (const auto handle = event.load(std::memory_order_acquire)) SetEvent(handle);
    }
} g_inputMonitor;

void notifyInputLatencyChange() noexcept {
    g_inputMonitor.statusChanged.store(true, std::memory_order_release);
    if (const auto handle = g_inputMonitor.event.load(std::memory_order_acquire)) SetEvent(handle);
}

// One independently prepared VST3 chain per host track.  The object is fixed
// in the runtime table so the audio callback never follows a map or allocates;
// control-thread reconfiguration drains the chain before replacing instances.
struct TrackRuntime {
    SelectionSlot trackSlots[2];
    EffectPool pool;
    effects::Chain chain;
    std::atomic<std::size_t> count{0};
    std::string trackKey;
    std::atomic<std::uint64_t> keyHash{0};
    std::atomic<bool> bypassRequested{true};
    std::string trackId;
    std::string scoreKey;
    int trackIndex = -1;
    std::string error;
    std::vector<Vst3SelectionEntry> failedSelection;
    std::atomic<int> configuredRate{0};
    std::vector<Vst3SelectionEntry> requested;
    std::atomic<bool> configured{false};
    std::atomic<bool> preloaded{false};
    int retainedSlot = -1;
    std::atomic<std::size_t> processBlocks{0};
    std::atomic<std::size_t> processedBlocks{0};
    std::atomic<std::size_t> bypassBlocks{0};
    std::atomic<std::size_t> errorBlocks{0};
    std::atomic<bool> processed{false};
    std::atomic<bool> writeObserved{false};
    std::atomic_flag processing = ATOMIC_FLAG_INIT;

    void shutdown() noexcept {
        chain.setBypassed(true);
        chain.deactivate();
        trackSlots[0].shutdown();
        trackSlots[1].shutdown();
        pool.effects.clear();
        count.store(0, std::memory_order_release);
        keyHash.store(0, std::memory_order_release);
        bypassRequested.store(true, std::memory_order_release);
        configuredRate.store(0, std::memory_order_release);
        processBlocks.store(0, std::memory_order_relaxed);
        processedBlocks.store(0, std::memory_order_relaxed);
        bypassBlocks.store(0, std::memory_order_relaxed);
        errorBlocks.store(0, std::memory_order_relaxed);
        configured.store(false, std::memory_order_release);
        preloaded.store(false, std::memory_order_release);
        retainedSlot = -1;
        processed.store(false, std::memory_order_release);
        writeObserved.store(false, std::memory_order_release);
    }

    bool prepare(const std::vector<Vst3SelectionEntry> &entries, double rate,
                 std::size_t maxBlock, std::string *error) noexcept {
        if (error) error->clear();
        if (entries.size() > SelectionSlot::kMaxEffects) {
            if (error) *error = "runtime_vst3_chain_full";
            return false;
        }
        if (entries.empty()) {
            chain.setBypassed(true);
            chain.deactivate();
            count.store(0, std::memory_order_release);
            configured.store(false, std::memory_order_release);
            configuredRate.store(0, std::memory_order_release);
            bypassRequested.store(true, std::memory_order_release);
            return true;
        }
        const auto old = chain.snapshot().activeSlot;
        const auto previousSlot = old >= 0 ? old : retainedSlot;
        const auto *previous = previousSlot >= 0 ? &trackSlots[previousSlot] : nullptr;
        if (old >= 0 && configuredRate.load() != static_cast<int>(rate)) chain.deactivate();
        if (previous && previous->matches(entries, rate, maxBlock)) {
            if (!chain.activate(static_cast<unsigned>(previousSlot))) {
                if (error) *error = "runtime_vst3_track_chain_activate_failed";
                return false;
            }
            chain.clearFault();
            chain.setBypassed(false);
            trackSlots[previousSlot].markActive();
            preloaded.store(pool.hasPreloaded(), std::memory_order_release);
            count.store(entries.size(), std::memory_order_release);
            configured.store(true, std::memory_order_release);
            configuredRate.store(static_cast<int>(rate), std::memory_order_release);
            bypassRequested.store(false, std::memory_order_release);
            return true;
        }
        const auto target = previousSlot == 0 ? 1U : 0U;
        // Reuse only a slot that has been retired at a block boundary. The
        // chain admission check drains readers before the slot's shared
        // processor instances are replaced.
        if (!chain.prepareSlot(target, {&trackSlots[target], &SelectionSlot::processCallback})) {
            if (error) *error = "runtime_vst3_track_chain_prepare_failed";
            return false;
        }
        if (!trackSlots[target].prepare(entries, rate, maxBlock, pool, error)) {
            if (chain.snapshot().activeSlot < 0) {
                chain.setBypassed(true);
                count.store(0, std::memory_order_release);
                configured.store(false, std::memory_order_release);
                configuredRate.store(0, std::memory_order_release);
                bypassRequested.store(true, std::memory_order_release);
            }
            return false;
        }
        if (!chain.prepareSlot(target, {&trackSlots[target], &SelectionSlot::processCallback}) ||
            !chain.activate(target)) {
            if (error) *error = "runtime_vst3_track_chain_activate_failed";
            trackSlots[target].shutdown();
            // A rate change may have retired the previous slot before the
            // replacement failed.  Publish an explicit bypassed state so the
            // UI and the audio callback cannot mistake stale counters for an
            // active runtime.
            if (chain.snapshot().activeSlot < 0) {
                chain.setBypassed(true);
                count.store(0, std::memory_order_release);
                configured.store(false, std::memory_order_release);
                configuredRate.store(0, std::memory_order_release);
                bypassRequested.store(true, std::memory_order_release);
            }
            return false;
        }
        chain.clearFault();
        chain.setBypassed(false);
        count.store(entries.size(), std::memory_order_release);
        retainedSlot = static_cast<int>(target);
        trackSlots[target].markActive();
        preloaded.store(pool.hasPreloaded(), std::memory_order_release);
        configured.store(true, std::memory_order_release);
        configuredRate.store(static_cast<int>(rate), std::memory_order_release);
        bypassRequested.store(false, std::memory_order_release);
        return true;
    }

    bool processBlock(const audio::BlockView &block, bool *actuallyProcessed = nullptr) noexcept {
        if (actuallyProcessed) *actuallyProcessed = false;
        processBlocks.fetch_add(1, std::memory_order_relaxed);
        if (bypassRequested.load(std::memory_order_acquire) ||
            !configured.load(std::memory_order_acquire) || count.load(std::memory_order_acquire) == 0 ||
            configuredRate.load(std::memory_order_acquire) != static_cast<int>(block.sampleRate)) {
            bypassBlocks.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        if (processing.test_and_set(std::memory_order_acquire)) {
            bypassBlocks.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        const auto result = chain.process(block);
        processing.clear(std::memory_order_release);
        if (result.error || !result.completed) {
            errorBlocks.fetch_add(1, std::memory_order_relaxed);
            audio::bypass(block);
            return false;
        }
        if (result.bypassed) {
            bypassBlocks.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        processedBlocks.fetch_add(1, std::memory_order_relaxed);
        if (actuallyProcessed) *actuallyProcessed = true;
        processed.store(true, std::memory_order_release);
        return true;
    }

    static bool processCallback(void *context, const audio::BlockView &block) noexcept {
        return static_cast<TrackRuntime *>(context)->processBlock(block);
    }
};

struct Patch {
    void *target = nullptr;
    void *trampoline = nullptr;
    std::uint8_t original[32]{};
    std::size_t size = 0;
    bool installed = false;
    bool managed = false;
    MH_STATUS lastStatus = MH_OK;
    bool restartRequired = false;
    bool ready() const noexcept { return installed && lastStatus == MH_OK && !restartRequired; }
};

struct Runtime {
    Patch master;
    Patch dsp;
    Patch stream;
    Patch listenerProbe;
#ifdef GPVST3_P13_PROBE_BUILD
    Patch rseProbe;
#endif
    Patch cursorMove;
    Patch cursorTrack;
    Patch scoreDuplicateTrack;
    Patch scoreCreateTrack;
    Patch scoreSwapTracks;
    std::atomic<std::size_t> topologyDuplicateCalls{0};
    std::atomic<std::size_t> topologySwapCalls{0};
    RawDataFn rawData = nullptr;
    FrameCountFn frameCount = nullptr;
    ChannelCountFn channelCount = nullptr;
    BufferAccessFn lock = nullptr;
    BufferAccessFn unlock = nullptr;
    SampleRateFn sampleRate = nullptr;
    EffectsChainIndexFn effectsChainIndex = nullptr;
    void *audioCore = nullptr;
    void *audioModule = nullptr;
    std::atomic<std::size_t> masterCalls{0};
    std::atomic<std::size_t> dspCalls{0};
    std::atomic<bool> trackContextObserved{false};
    std::atomic<bool> trackContextStable{false};
    std::atomic<bool> trackScopeUnresolved{true};
    std::atomic<bool> effectsChainIndexObserved{false};
    std::atomic<int> observedEffectsChainIndex{-1};
    std::atomic<std::size_t> effectsChainContextCount{0};
    struct EffectsChainObservation {
        std::atomic<void *> self{nullptr};
        std::atomic<int> index{-1};
    } effectsChainObservations[32];
    std::atomic<std::size_t> trackChainProcessBlocks{0};
    std::atomic<std::size_t> trackChainProcessedBlocks{0};
    std::atomic<std::size_t> trackDispatchMisses{0};
    std::atomic<std::uintptr_t> trackLastDspSelf{0};
    std::atomic<std::uintptr_t> trackDispatchSelf0{0};
    std::atomic<std::uintptr_t> trackDispatchSelf1{0};
    std::atomic<std::size_t> trackChainBypassBlocks{0};
    std::atomic<std::size_t> trackChainErrorBlocks{0};
    std::atomic<std::size_t> trackBindingsPublished{0};
    std::atomic<bool> trackRuntimeProcessed{false};
    std::atomic<bool> trackRuntimeWriteObserved{false};
    std::atomic<int> lastTrackRuntimeIndex{-1};
    std::string trackRuntimeError;
    struct TrackDispatch {
        std::atomic<void *> self{nullptr};
        std::atomic<TrackRuntime *> runtime{nullptr};
    } trackDispatch[64];
    TrackRuntime trackRuntimes[32];
    std::atomic<std::uint64_t> trackDispatchGeneration{0};
    std::atomic<std::size_t> trackDispatchReaders{0};
    std::atomic<std::size_t> globalChainProcessBlocks{0};
    std::atomic<std::uint64_t> callbackSequence{0};
    std::atomic<std::uint64_t> masterFirstSequence{0};
    std::atomic<std::uint64_t> masterLastSequence{0};
    std::atomic<std::uint64_t> dspFirstSequence{0};
    std::atomic<std::uint64_t> dspLastSequence{0};
    std::atomic<std::uintptr_t> masterFirstBuffer{0};
    std::atomic<std::uintptr_t> masterLastBuffer{0};
    std::atomic<std::uintptr_t> dspFirstBuffer{0};
    std::atomic<std::uintptr_t> dspLastBuffer{0};
    std::atomic<bool> sameBufferObserved{false};
    std::atomic<bool> dspAfterMasterObserved{false};
    std::atomic<bool> outputObserved{false};
    std::atomic<bool> outputWriteObserved{false};
    std::atomic<bool> outputEvidenceClaimed{false};
    std::atomic<std::size_t> outputCalls{0};
    std::atomic<std::uint64_t> outputBeforeHash{0};
    std::atomic<std::uint64_t> outputAfterHash{0};
    std::atomic<std::size_t> outputFrames{0};
    std::atomic<unsigned long> outputThread{0};
    std::atomic<std::uintptr_t> outputFirstBuffer{0};
    std::atomic<std::uintptr_t> outputLastBuffer{0};
    std::atomic<bool> bufferWriteObserved{false};
    std::atomic<bool> dspInsideMaster{false};
    std::atomic<std::size_t> frames{0};
    std::atomic<std::size_t> channels{0};
    std::atomic<int> rate{0};
    std::atomic<unsigned long> masterThread{0};
    std::atomic<unsigned long> dspThread{0};
    RuntimeEffect effects[2];
    SelectionSlot selectionSlots[2];
    EffectPool selectionPool;
    std::recursive_mutex editorMutex;
    std::mutex selectionMutex;
    std::mutex selectionRequestMutex;
    std::mutex catalogMutex;
    std::atomic<bool> selectionWorkerBusy{false};
    std::atomic<std::uint64_t> audioGeneration{0};
    std::atomic<std::uint64_t> selectionRequestId{0};
    std::atomic<std::uint64_t> selectionQueuedNanoseconds{0};
    std::atomic<std::uint64_t> selectionWorkerStartedNanoseconds{0};
    std::atomic<std::uint64_t> selectionPreparedNanoseconds{0};
    std::atomic<std::uint64_t> selectionCommittedNanoseconds{0};
    std::atomic<std::uint64_t> selectionAppliedGeneration{0};
    std::atomic<int> selectionStatus{0}; // 0 idle, 1 queued, 2 preparing, 3 applied, 4 failed
    std::atomic<std::uint64_t> editorRequestGeneration{0};
    std::atomic<int> editorStage{static_cast<int>(EditorStage::None)};
    std::atomic<long> editorResultCode{0};
    std::string editorIdentity;
    std::string editorError;
    int retainedSelectionSlot = -1;
    std::vector<Vst3SelectionEntry> requestedSelection;
    bool projectGlobalRestored = false;
    std::unordered_map<std::string, std::vector<Vst3SelectionEntry>> requestedTrackSelections;
    std::vector<Vst3SelectionEntry> catalogEntries;
    bool catalogReady = false;
    std::vector<gp_audio::Binding> publishedBindings;
    std::string currentTrackKey;
    std::atomic<bool> selectionMode{false};
    std::atomic<bool> selectionPublished{false};
    std::atomic<int> selectionConfiguredRate{0};
    effects::Chain chain;
    std::atomic<std::size_t> effectCalls{0};
    std::atomic<bool> effectProcessed{false};
    std::atomic<bool> effectWriteObserved{false};
    std::atomic<std::size_t> configurationMismatchBlocks{0};
    std::atomic<std::size_t> reconfigurationPassed{0};
    std::atomic<std::size_t> reconfigurationFailed{0};
    std::atomic<bool> reconfigurationValidated{false};
    // Live guitar input uses an independent copy of the selected global chain.
    // It must not share RuntimeEffect instances with the score/master chain:
    // Guitar Pro can call both paths concurrently from different audio
    // callbacks, while a RuntimeEffect intentionally rejects re-entrant calls.
    SelectionSlot inputSelectionSlots[2];
    EffectPool inputSelectionPool;
    effects::Chain inputChain;
    std::atomic<std::size_t> inputAfterOriginalBlocks{0};
    std::atomic<std::uint64_t> inputPostOriginalHash{0}, inputPostRouteHash{0};
    std::atomic<bool> inputOrderEvidenceClaimed{false};
    std::atomic<bool> inputOrderSamplesClaimed{false}, inputOrderSamplesObserved{false};
    std::atomic<float> inputOrderCaptureSample{0}, inputOrderGeneratedSample{0}, inputOrderOutputSample{0};
    input::Router inputRouter;
    AudioLayerInputLevelFn audioLayerInputLevel = nullptr;
    AudioLayerIsRunningFn audioLayerIsRunning = nullptr;
    AudioLayerBufferSizeFn audioLayerBufferSize = nullptr;
    void *audioLayer = nullptr;
    std::atomic<float> hostInputLevel{0.0F};
    std::atomic<bool> hostInputLevelObserved{false};
    std::atomic<bool> hostStreamRunning{false};
    std::atomic<std::size_t> hostBufferSize{0};
    std::atomic<bool> inputCapturePathLocated{false};
    std::atomic<std::size_t> inputCaptureCalls{0};
    std::size_t inputChannelCount = 2;
    std::size_t outputChannelCount = 2;
    std::atomic<int> inputConfiguredRate{0};
    std::atomic<std::size_t> inputConfiguredChannels{0};
    std::atomic<std::size_t> inputConfiguredOutputChannels{0};
    std::atomic<int> inputObservedRate{0};
    std::atomic<std::size_t> inputObservedChannels{0};
    std::atomic<std::size_t> inputObservedOutputChannels{0};
    std::atomic<std::size_t> inputConfigurationErrors{0};
    std::atomic<bool> inputConfigurationPending{false};
    std::atomic_flag inputProcessing = ATOMIC_FLAG_INIT;
    std::thread selectionWorker;
    bool selectionWorkerStop = false;
    bool inputSelectionRequestPending = false;
    std::vector<Vst3SelectionEntry> pendingInputSelection;
    state::InputMonitorSettings pendingInputSettings;
    std::uint64_t inputRequestGeneration = 0;
    bool selectionRequestPending = false;
    std::uint64_t selectionRequestGeneration = 0;
    std::vector<Vst3SelectionEntry> pendingSelection;
    std::vector<Vst3SelectionEntry> appliedSelection;
    struct PreloadRequest {
        std::vector<Vst3SelectionEntry> selection;
        double rate = 44100.0;
        std::uint64_t generation = 0;
        std::size_t next = 0;
    };
    // At most one request/attempt per global or live track scope. Explicit
    // selections always take priority over this queue on the same worker.
    std::unordered_map<std::string, PreloadRequest> pendingPreloads, preloadAttempts;
    std::atomic<bool> preloadBusy{false};
    bool globalPreloaded = false, inputPreloaded = false;
    std::uint64_t preloadCompleted = 0, preloadFailed = 0;
    std::string preloadError;
    std::unordered_map<std::string, std::vector<Vst3SelectionEntry>> pendingTrackSelections;
    std::unordered_map<std::string, std::uint64_t> pendingTrackGenerations;
    std::vector<gp_audio::Binding> pendingBindings;
    std::size_t pendingDiscovered = 0;
    bool trackContextRequestPending = false;
    int retainedInputSelectionSlot = -1;
};

Runtime g_runtime;
State g_initial;
host::Verification g_verification;
std::shared_ptr<RuntimeEffect> g_openEditorEffect; // Owned and accessed on Qt.
std::atomic<bool> g_selectionStateChanged{false};
std::atomic<bool> g_trackTopologyInvalidated{false};
using SelectionNotifier = void (*)() noexcept;
using TrackContextNotifier = void (*)() noexcept;
std::atomic<SelectionNotifier> g_selectionNotifier{nullptr};
std::atomic<TrackContextNotifier> g_trackContextNotifier{nullptr};
thread_local bool g_inMasterHook = false;
thread_local bool g_inEditorCallback = false;

void mirrorInputParameter(RuntimeEffect *source, ParamID id, ParamValue value) noexcept {
    if (!source) return;
    // The worker can replace input slots while a plug-in editor sends a
    // parameter callback. Do not wait behind initialization; a missed mirror
    // is harmless because the next explicit edit publishes a fresh value.
    std::unique_lock<std::recursive_mutex> editorLock(g_runtime.editorMutex,
                                                       std::try_to_lock);
    if (!editorLock.owns_lock()) return;
    const auto target = source->inputParameterMirror.lock();
    if (!target || target.get() == source) return;
    if (target->parameterChanges.publish(id, value))
        target->parameterEdits.fetch_add(1, std::memory_order_relaxed);
}

void refreshInputParameterMirrors() noexcept {
    for (auto &slot : g_runtime.selectionSlots)
        for (std::size_t index = 0; index < slot.count; ++index)
            if (slot.effects[index]) slot.effects[index]->inputParameterMirror.reset();

    const int globalActive = g_runtime.chain.snapshot().activeSlot;
    const int inputActive = g_runtime.inputChain.snapshot().activeSlot;
    if (globalActive < 0 || globalActive >= 2 || inputActive < 0 || inputActive >= 2 ||
        !g_runtime.selectionMode.load(std::memory_order_acquire)) return;
    auto &global = g_runtime.selectionSlots[globalActive];
    auto &input = g_runtime.inputSelectionSlots[inputActive];
    for (std::size_t globalIndex = 0; globalIndex < global.count; ++globalIndex) {
        const auto &source = global.effects[globalIndex];
        if (!source) continue;
        for (std::size_t inputIndex = 0; inputIndex < input.count; ++inputIndex) {
            const auto &target = input.effects[inputIndex];
            if (!target || source->identity.module != target->identity.module ||
                source->identity.classId != target->identity.classId) continue;
            source->inputParameterMirror = target;
            break;
        }
    }
}

bool processInputChain(void *context, const audio::BlockView &block) noexcept {
    auto *chain = static_cast<effects::Chain *>(context);
    if (!chain) return false;
    const auto result = chain->process(block);
    return result.completed && !result.bypassed && !result.error;
}

void lockInputProcessing() noexcept {
    while (g_runtime.inputProcessing.test_and_set(std::memory_order_acquire))
        std::this_thread::yield();
}

void unlockInputProcessing() noexcept {
    g_runtime.inputProcessing.clear(std::memory_order_release);
}

struct InputProcessingGuard {
    ~InputProcessingGuard() { unlockInputProcessing(); }
};

struct EditorCallbackScope {
    const bool previous = g_inEditorCallback;
    EditorCallbackScope() noexcept { g_inEditorCallback = true; }
    ~EditorCallbackScope() { g_inEditorCallback = previous; }
};

double callbackSampleRate() noexcept;
void updateAudioLayerState() noexcept;
bool sameSelection(const std::vector<Vst3SelectionEntry> &left,
                   const std::vector<Vst3SelectionEntry> &right) noexcept;
bool configureInputSelection(const std::vector<Vst3SelectionEntry> &selection,
                             std::string *error = nullptr) noexcept;
void refreshInputParameterMirrors() noexcept;
bool processInputChain(void *context, const audio::BlockView &block) noexcept;
void rejectSavedEntry(const Vst3SelectionEntry &entry, const std::string &error,
                      state::ScopeKind scope, const std::string &score = {},
                      const std::string &track = {}, int trackIndex = -1);
void rejectSavedEntryOnQtThread(const Vst3SelectionEntry &entry, const std::string &error,
                                state::ScopeKind scope, const std::string &score = {},
                                const std::string &track = {}, int trackIndex = -1) noexcept;

bool containsIdentity(const std::vector<Vst3SelectionEntry> &entries,
                      const Vst3SelectionEntry &value) noexcept {
    return std::any_of(entries.begin(), entries.end(), [&](const Vst3SelectionEntry &entry) {
        return entry.module == value.module && entry.classId == value.classId;
    });
}

bool trackSelectionQueued(const std::string &trackKey) noexcept {
    std::lock_guard<std::mutex> lock(g_runtime.selectionRequestMutex);
    return g_runtime.pendingTrackSelections.find(trackKey) !=
        g_runtime.pendingTrackSelections.end();
}

std::uint64_t stableTrackKeyHash(const std::string &value) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const auto byte : value) {
        hash ^= static_cast<unsigned char>(byte);
        hash *= 1099511628211ULL;
    }
    return hash;
}

void refreshTrackContextWorkerImpl(std::vector<gp_audio::Binding> bindings,
                                   std::size_t discovered) noexcept;
std::vector<Vst3SelectionEntry> selectionFromEffects(const QJsonArray &effects);
std::vector<Vst3SelectionEntry> enabledSelectionFromScope(state::ScopeKind scope,
                                                            const std::string &score = {},
                                                            const std::string &track = {}) {
    QJsonObject chain;
    if (!state::loadChain(chain)) return {};
    const auto effects = state::scopeEffects(chain, scope,
        QString::fromStdString(score), QString::fromStdString(track));
    QJsonArray enabled;
    for (const auto &value : effects)
        if (value.toObject().value("enabled").toBool()) enabled.append(value);
    return selectionFromEffects(enabled);
}
bool inputFeatureEnabled() noexcept;
input::Route configuredInputRoute() noexcept;

void runPreload(const std::string &scope, const Runtime::PreloadRequest &request) {
    std::lock_guard<std::recursive_mutex> editorLock(g_runtime.editorMutex);
    std::lock_guard<std::mutex> lock(g_runtime.selectionMutex);
    {
        std::lock_guard<std::mutex> requestLock(g_runtime.selectionRequestMutex);
        const auto it = g_runtime.preloadAttempts.find(scope);
        if (g_runtime.selectionWorkerStop || it == g_runtime.preloadAttempts.end() ||
            it->second.generation != request.generation) return;
    }
    if (request.next >= request.selection.size()) return;
    const auto &entry = request.selection[request.next];
    std::string error;
    const auto preload = [&](EffectPool &pool) {
        const bool ready = static_cast<bool>(pool.acquire(entry, request.rate, 16384, &error, true));
        if (ready) ++g_runtime.preloadCompleted;
        else {
            ++g_runtime.preloadFailed;
            g_runtime.preloadError = entry.module + ":" + entry.classId + ":" + error;
        }
    };
    if (scope.empty()) {
        preload(g_runtime.selectionPool);
        if (inputFeatureEnabled() && configuredInputRoute() != input::Route::Disabled)
            preload(g_runtime.inputSelectionPool);
        g_runtime.globalPreloaded = g_runtime.selectionPool.covers(request.selection);
        g_runtime.inputPreloaded = g_runtime.inputSelectionPool.covers(request.selection);
    } else {
        auto runtime = std::find_if(std::begin(g_runtime.trackRuntimes), std::end(g_runtime.trackRuntimes),
            [&](const TrackRuntime &value) { return value.trackKey == scope; });
        if (runtime == std::end(g_runtime.trackRuntimes)) {
            // Track context can arrive after catalog delivery. Forget this
            // attempt so the context notifier schedules it again once the
            // fixed runtime table has been published.
            std::lock_guard<std::mutex> requestLock(g_runtime.selectionRequestMutex);
            g_runtime.preloadAttempts.erase(scope);
            return;
        }
        preload(runtime->pool);
        runtime->preloaded.store(runtime->pool.covers(request.selection), std::memory_order_release);
    }
}

void wakeSelectionWorker() {
    std::lock_guard<std::mutex> lock(g_selectionWorkerLoopMutex);
    if (auto *loop = g_selectionWorkerLoop)
        QMetaObject::invokeMethod(loop, &QEventLoop::quit, Qt::QueuedConnection);
}

bool configureIndependentInput(const std::vector<Vst3SelectionEntry> &selection,
                                const state::InputMonitorSettings &settings,
                                std::uint64_t generation, std::string &error) {
    using Mode = state::InputMonitorMode;
    const bool low = settings.mode == Mode::LowLatencyOverlay;
    if (!low) {
        std::lock_guard<std::mutex> requestLock(g_runtime.selectionRequestMutex);
        if (generation != g_runtime.inputRequestGeneration) return false;
        if (settings.mode == Mode::Legacy || (g_inputMonitor.exchange.watching() &&
            g_inputMonitor.exchange.legacyFallback())) g_inputMonitor.exchange.legacy();
        else g_inputMonitor.exchange.off();
        g_inputMonitor.settings = settings;
        g_inputMonitor.phase.store(settings.mode == Mode::Legacy ? 1 : 0);
        g_inputMonitor.error.clear();
        if (sameSelection(selection, g_inputMonitor.desired)) return true;
    }
    if (selection.empty() && !low) {
        std::lock_guard<std::mutex> requestLock(g_runtime.selectionRequestMutex);
        if (generation != g_runtime.inputRequestGeneration) return false;
        g_inputMonitor.phase.store(settings.mode == Mode::Legacy ? 1 : 0);
        g_inputMonitor.desired = selection;
        g_inputMonitor.settings = settings;
        g_inputMonitor.error.clear();
        return true;
    }
    double rate = 44100;
    std::size_t maxBlock = portaudio::kMaxFrames;
    std::uint64_t streamGeneration = 0, rateRevision = 0;
    if (low) {
        auto stream = asioprobe::currentStream();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while ((!stream.bound || !stream.callback.rateValidated) &&
               stream.binding != asioprobe::BindingState::HostLimited &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            stream = asioprobe::currentStream();
        }
        rate = stream.callback.actualRate;
        streamGeneration = stream.callback.generation;
        rateRevision = stream.callback.rateRevision;
        if (stream.driverFrames > 0 && stream.driverFrames <= 8192)
            maxBlock = (std::min)(std::size_t(stream.driverFrames), portaudio::kMaxFrames);
        if (stream.binding == asioprobe::BindingState::HostLimited) {
            error = stream.limit == asioprobe::BindingLimit::ProxyCapacityExhausted
                ? "input_stream_capacity_exhausted_restart_required" : "input_stream_callbacks_unsupported";
            return false;
        }
        if (
#ifdef GPVST3_P13_PROBE_BUILD
            g_drainProbeRequested || g_inputProbeSilence ||
#endif
            !g_listenerProbeInstalled ||
            !stream.bound || !stream.lifetimeProtected || !stream.callback.rateValidated ||
            !input::drain::supportedDestinationRate(rate)) {
            error = "input_host_contract_unavailable";
            return false;
        }
    }
    const int active = g_inputMonitor.exchange.activeIndex();
    const int target = (active >= 0 ? active : g_inputMonitor.retained) == 0 ? 1 : 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    while (!g_inputMonitor.exchange.writable(target)) {
        if (std::chrono::steady_clock::now() >= deadline) {
            error = "input_slot_reader_timeout";
            return false;
        }
        std::this_thread::yield();
    }
    auto &slot = g_inputMonitor.monitorSlots[target];
    auto effectiveSelection = selection;
    std::vector<std::shared_ptr<RuntimeEffect>> reusable;
    for (const auto &effect : g_inputMonitor.warmEffects)
        if (effect->ready.load() && effect->configuredRate.load() == static_cast<int>(rate) &&
            effect->configuredBlock.load() == maxBlock)
            reusable.push_back(effect);
    // A monitor gain/mode change must retain parameter edits made since the
    // last selection request. This API edits order/enabled identity, not state
    // imports: retained active identities always keep their latest live state.
    if (g_inputMonitor.retained >= 0) {
        const auto &previous = g_inputMonitor.monitorSlots[g_inputMonitor.retained].selection;
        for (auto &entry : effectiveSelection)
            for (std::size_t i = 0; i < previous.count; ++i)
                if (previous.effects[i]->identity.module == entry.module &&
                    previous.effects[i]->identity.classId == entry.classId &&
                    containsIdentity(g_inputMonitor.desired, entry)) {
                    entry = previous.effects[i]->captureState();
                    if (previous.effects[i]->configuredRate.load() == static_cast<int>(rate) &&
                        previous.effects[i]->configuredBlock.load() == maxBlock &&
                        std::find(reusable.begin(), reusable.end(), previous.effects[i]) == reusable.end())
                        reusable.push_back(previous.effects[i]);
                    break;
                }
    }
    // Leave room for an entire requested chain even when every requested
    // state needs a new instance. Retiring cache references is safe: active
    // slots and editor windows retain their own shared ownership.
    std::stable_partition(reusable.begin(), reusable.end(), [&](const auto &effect) {
        return containsIdentity(effectiveSelection, effect->identity);
    });
    const auto retainedLimit = EffectPool::kMaxWarmInstances - effectiveSelection.size();
    if (reusable.size() > retainedLimit) reusable.resize(retainedLimit);
    slot.selection.shutdown();
    slot.pool.effects.clear();
    slot.pool.effects = std::move(reusable);
    if (!slot.selection.prepare(effectiveSelection, rate, maxBlock, slot.pool, &error) ||
        !slot.router.prepare(2, portaudio::kMaxFrames)) {
        if (error.empty()) error = "input_overlay_prepare_failed";
        return false;
    }
    slot.rate = rate;
    for (std::size_t i = 0; i < slot.selection.count; ++i)
        slot.selection.effects[i]->independentInput = true;
    slot.generation = streamGeneration;
    slot.revision = rateRevision;
    slot.gain = static_cast<float>(settings.gain);
    slot.router.setProcessor({&slot, &InputMonitorSlot::process});
    slot.router.setRoute(input::Route::Overlay);
    slot.router.setBypassed(false);
    slot.router.setEnabled(true);
    slot.router.setStreamRunning(true);
    {
        const auto opened = std::atomic_load(&g_openEditorEffect);
        if (opened && opened->independentInput.load() &&
            std::none_of(std::begin(slot.selection.effects), std::end(slot.selection.effects),
                [&](const auto &effect) { return effect == opened; })) {
            // The old editor cannot remain attached to a retired-rate/state
            // instance while audio uses the replacement. Keep other scopes'
            // windows untouched. Do not hold the request lock while invoking Qt.
            if (!invokeOnQtThreadBlocking([opened] {
                const auto current = std::atomic_load(&g_openEditorEffect);
                if (current != opened) return;
                std::atomic_store(&g_openEditorEffect, std::shared_ptr<RuntimeEffect>{});
                EditorCallbackScope callbackScope;
                opened->closeEditor();
                ui::closeInputEditorForRuntimeChange();
            })) { error = "input_editor_retire_failed"; return false; }
        }
    }
    std::lock_guard<std::mutex> requestLock(g_runtime.selectionRequestMutex);
    if (generation != g_runtime.inputRequestGeneration) return false;
    if (low) {
        // A reader that saw an older token may briefly reserve this retired
        // slot before rejecting that token. Keep the prepared payload intact
        // and retry on the control thread; no new reader can accept it until
        // publication succeeds.
        const auto publishDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
        while (!g_inputMonitor.exchange.publish(target)) {
            if (std::chrono::steady_clock::now() >= publishDeadline) {
                error = "input_overlay_publish_failed";
                return false;
            }
            std::this_thread::yield();
        }
    }
    slot.selection.markActive();
    g_inputMonitor.warmEffects = slot.pool.effects;
    g_inputMonitor.retained = target;
    g_inputMonitor.desired = effectiveSelection;
    g_runtime.pendingInputSelection = effectiveSelection;
    g_inputMonitor.settings = settings;
    g_inputMonitor.error.clear();
    g_inputMonitor.phase.store(low ? 3 : (settings.mode == Mode::Legacy ? 1 : 0));
    return true;
}

void selectionWorkerLoop() {
    struct ComApartment {
        HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        ~ComApartment() { if (SUCCEEDED(result)) CoUninitialize(); }
    } comApartment;
    QEventLoop idleLoop;
    QWinEventNotifier inputEvent(g_inputMonitor.event.load(), &idleLoop);
    QObject::connect(&inputEvent, &QWinEventNotifier::activated, &idleLoop, [&](HANDLE handle) {
        ResetEvent(handle);
        idleLoop.quit();
    });
    {
        std::lock_guard<std::mutex> lock(g_selectionWorkerLoopMutex);
        g_selectionWorkerLoop = &idleLoop;
    }
    for (;;) {
        std::vector<Vst3SelectionEntry> selection;
        std::string trackKey;
        std::uint64_t generation = 0;
        std::vector<gp_audio::Binding> contextBindings;
        std::size_t contextDiscovered = 0;
        bool contextRefresh = false;
        Runtime::PreloadRequest preloadRequest;
        std::string preloadScope;
        bool preloadRefresh = false;
        bool inputRefresh = false;
        bool inputStatusRefresh = false;
        state::InputMonitorSettings inputSettings;
        {
            std::unique_lock<std::mutex> lock(g_runtime.selectionRequestMutex);
            while (!g_runtime.selectionWorkerStop && !g_runtime.selectionRequestPending &&
                   g_runtime.pendingTrackSelections.empty() && !g_runtime.trackContextRequestPending &&
                   g_runtime.pendingPreloads.empty() && !g_runtime.inputSelectionRequestPending &&
                   !g_inputMonitor.statusChanged.load(std::memory_order_acquire)) {
                lock.unlock();
                idleLoop.exec();
                lock.lock();
            }
            if (g_runtime.selectionWorkerStop && !g_runtime.selectionRequestPending &&
                g_runtime.pendingTrackSelections.empty() && !g_runtime.inputSelectionRequestPending) {
                std::lock_guard<std::mutex> loopLock(g_selectionWorkerLoopMutex);
                g_selectionWorkerLoop = nullptr;
                return;
            }
            // Publish the current track runtimes before consuming a queued
            // track selection.  On a freshly opened score both requests can
            // arrive in the same maintenance turn; taking the selection
            // first used to find no TrackRuntime and silently drop it.
            if (g_inputMonitor.statusChanged.exchange(false, std::memory_order_acq_rel)) {
                inputStatusRefresh = true;
            } else if (g_runtime.inputSelectionRequestPending) {
                inputRefresh = true;
                selection = g_runtime.pendingInputSelection;
                inputSettings = g_runtime.pendingInputSettings;
                generation = g_runtime.inputRequestGeneration;
                g_runtime.inputSelectionRequestPending = false;
            } else if (g_runtime.trackContextRequestPending) {
                contextRefresh = true;
                contextBindings = std::move(g_runtime.pendingBindings);
                contextDiscovered = g_runtime.pendingDiscovered;
                g_runtime.pendingBindings.clear();
                g_runtime.pendingDiscovered = 0;
                g_runtime.trackContextRequestPending = false;
            } else if (g_runtime.selectionRequestPending) {
                selection = std::move(g_runtime.pendingSelection);
                generation = g_runtime.selectionRequestGeneration;
                g_runtime.selectionRequestPending = false;
            } else if (!g_runtime.pendingTrackSelections.empty()) {
                auto it = g_runtime.pendingTrackSelections.begin();
                trackKey = it->first;
                selection = std::move(it->second);
                generation = g_runtime.pendingTrackGenerations[trackKey];
                g_runtime.pendingTrackSelections.erase(it);
            } else {
                auto it = g_runtime.pendingPreloads.begin();
                preloadScope = it->first;
                preloadRequest = std::move(it->second);
                g_runtime.pendingPreloads.erase(it);
                g_runtime.preloadBusy.store(true, std::memory_order_release);
                preloadRefresh = true;
            }
            g_runtime.selectionWorkerBusy.store(true, std::memory_order_release);
            if (!contextRefresh && !preloadRefresh && !inputRefresh && !inputStatusRefresh) {
                g_runtime.selectionWorkerStartedNanoseconds.store(steadyNanoseconds(), std::memory_order_release);
                g_runtime.selectionStatus.store(2, std::memory_order_release);
            }
        }
        if (inputStatusRefresh) {
            if (g_inputMonitor.reconfigureRequested.exchange(false)) {
                std::lock_guard<std::mutex> lock(g_runtime.selectionRequestMutex);
                if (g_runtime.pendingInputSettings.mode == state::InputMonitorMode::LowLatencyOverlay) {
                    g_runtime.inputSelectionRequestPending = true;
                    ++g_runtime.inputRequestGeneration;
                }
            }
            g_runtime.selectionWorkerBusy.store(false, std::memory_order_release);
            if (const auto notifier = g_selectionNotifier.load(std::memory_order_acquire)) notifier();
            continue;
        }
        if (inputRefresh) {
            std::lock_guard<std::recursive_mutex> editorLock(g_runtime.editorMutex);
            std::lock_guard<std::mutex> selectionLock(g_runtime.selectionMutex);
            std::string error;
            bool accepted = false;
            try { accepted = configureIndependentInput(selection, inputSettings, generation, error); }
            catch (...) { error = "input_prepare_exception"; }
            if (!accepted && !error.empty()) {
                const bool hostUnavailable = error == "input_host_contract_unavailable" ||
                    error == "input_stream_capacity_exhausted_restart_required" ||
                    error == "input_stream_callbacks_unsupported";
                bool current = false;
                {
                    std::lock_guard<std::mutex> requestLock(g_runtime.selectionRequestMutex);
                    current = generation == g_runtime.inputRequestGeneration;
                    if (current) {
                        const bool actuallySuppressed = g_inputMonitor.nativeSuppressed.load();
                        if (inputSettings.mode == state::InputMonitorMode::LowLatencyOverlay && actuallySuppressed)
                            g_inputMonitor.exchange.mute();
                        else if (inputSettings.mode == state::InputMonitorMode::LowLatencyOverlay && hostUnavailable)
                            g_inputMonitor.exchange.observe();
                        else if (g_inputMonitor.exchange.watching()) {
                            // Plugin preparation failure is terminal for this
                            // request. Do not let an ordinary callback retry the
                            // previous chain and erase its error or native route.
                            if (g_inputMonitor.exchange.legacyFallback()) g_inputMonitor.exchange.legacy();
                            else g_inputMonitor.exchange.off();
                        }
                        g_inputMonitor.error = error;
                        g_inputMonitor.phase.store(actuallySuppressed ? 5 : 6);
                        // Host rejection says nothing about the selected
                        // plugins. Retain that intent for a later valid stream.
                        g_runtime.pendingInputSelection = hostUnavailable ? selection : g_inputMonitor.desired;
                    }
                }
                if (current && !hostUnavailable)
                    for (const auto &entry : selection)
                        if (!containsIdentity(g_inputMonitor.desired, entry)) {
                            // The UI may enqueue a successful retry while this
                            // worker waits for Qt. Check its generation on Qt,
                            // immediately before the saved rejection is applied.
                            invokeOnQtThreadBlocking([entry, error, generation] {
                                std::lock_guard<std::mutex> requestLock(g_runtime.selectionRequestMutex);
                                if (generation == g_runtime.inputRequestGeneration)
                                    rejectSavedEntry(entry, error, state::ScopeKind::Input);
                            });
                        }
            }
            g_runtime.selectionWorkerBusy.store(false, std::memory_order_release);
            if (const auto notifier = g_selectionNotifier.load(std::memory_order_acquire)) notifier();
            continue;
        }
        if (contextRefresh) {
            refreshTrackContextWorkerImpl(std::move(contextBindings), contextDiscovered);
            g_runtime.selectionWorkerBusy.store(false, std::memory_order_release);
            if (const auto notifier = g_trackContextNotifier.load(std::memory_order_acquire)) notifier();
            continue;
        }
        if (preloadRefresh) {
            try { runPreload(preloadScope, preloadRequest); }
            catch (...) {
                std::lock_guard<std::mutex> lock(g_runtime.selectionMutex);
                ++g_runtime.preloadFailed;
                g_runtime.preloadError = "preload_exception";
            }
            // Yield between plugins so explicit selections are serviced before
            // continuing. A failed plugin never cancels the rest of the catalog.
            {
                std::lock_guard<std::mutex> requestLock(g_runtime.selectionRequestMutex);
                const auto attempt = g_runtime.preloadAttempts.find(preloadScope);
                if (!g_runtime.selectionWorkerStop && attempt != g_runtime.preloadAttempts.end() &&
                    attempt->second.generation == preloadRequest.generation &&
                    ++preloadRequest.next < preloadRequest.selection.size())
                    g_runtime.pendingPreloads[preloadScope] = std::move(preloadRequest);
            }
            g_runtime.preloadBusy.store(false, std::memory_order_release);
            g_runtime.selectionWorkerBusy.store(false, std::memory_order_release);
            // A checkbox completion may have deferred its indicator while
            // this queued preload was still pending. Wake UI on the final
            // completion as well, including the failure path.
            if (const auto notifier = g_selectionNotifier.load(std::memory_order_acquire)) notifier();
            continue;
        }
        {
            // Only the worker waits for runtime ownership. Qt's observation,
            // capture and editor paths try this lock and defer while busy.
            std::lock_guard<std::recursive_mutex> editorLock(g_runtime.editorMutex);
            std::string prepareError;
            // Hook installation and host object reads stay on Qt. Only plug-in
            // preparation and runtime publication belong to this worker.
            const bool prepared = selection.empty() ||
                (trackKey.empty() ? g_runtime.master.ready() : g_runtime.dsp.ready());
            if (!prepared) prepareError = "hook_install_failed";
            std::lock_guard<std::mutex> lock(g_runtime.selectionMutex);
            bool stale = false;
            {
                std::lock_guard<std::mutex> requestLock(g_runtime.selectionRequestMutex);
                if (trackKey.empty())
                    stale = generation != g_runtime.selectionRequestGeneration;
                else {
                    const auto it = g_runtime.pendingTrackGenerations.find(trackKey);
                    stale = it != g_runtime.pendingTrackGenerations.end() && generation != it->second;
                }
            }
            if (stale) {
                g_runtime.selectionStatus.store(1, std::memory_order_release);
                g_runtime.selectionWorkerBusy.store(false, std::memory_order_release);
                continue;
            }
            if (trackKey.empty()) {
                std::string error;
                const bool accepted = prepared && configureSelectedChain(selection, &error);
                if (!accepted && error.empty()) error = prepareError;
                if (accepted) {
                    // Prepare the live-input copy on this worker as well. The
                    // UI request is already asynchronous, so a slow third
                    // party factory cannot block the Qt event loop.
                    configureInputSelection(selection);
                    g_runtime.requestedSelection = selection;
                    g_runtime.appliedSelection = selection;
                    g_runtime.selectionPreparedNanoseconds.store(steadyNanoseconds(), std::memory_order_release);
                    g_runtime.selectionCommittedNanoseconds.store(steadyNanoseconds(), std::memory_order_release);
                    g_runtime.audioGeneration.fetch_add(1, std::memory_order_acq_rel);
                    g_runtime.selectionAppliedGeneration.store(generation, std::memory_order_release);
                    g_runtime.selectionStatus.store(3, std::memory_order_release);
                } else {
                    const auto rejected = error.empty() ?
                        std::string("runtime_vst3_selection_prepare_failed") : error;
                    for (const auto &entry : selection)
                        if (!containsIdentity(g_runtime.appliedSelection, entry))
                            rejectSavedEntryOnQtThread(entry, rejected,
                                                       state::ScopeKind::Global);
                    g_runtime.requestedSelection = g_runtime.appliedSelection;
                    g_runtime.selectionPreparedNanoseconds.store(steadyNanoseconds(), std::memory_order_release);
                    g_runtime.selectionStatus.store(4, std::memory_order_release);
                }
            } else {
                auto runtime = std::find_if(std::begin(g_runtime.trackRuntimes),
                                            std::end(g_runtime.trackRuntimes),
                                            [&](const TrackRuntime &value) {
                                                return value.trackKey == trackKey;
                                            });
                if (runtime != std::end(g_runtime.trackRuntimes)) {
                    std::string error;
                    const bool accepted = prepared && runtime->prepare(selection, callbackSampleRate(), 16384, &error);
                    bool staleAfterPrepare = false;
                    {
                        std::lock_guard<std::mutex> requestLock(g_runtime.selectionRequestMutex);
                        const auto current = g_runtime.pendingTrackGenerations.find(trackKey);
                        staleAfterPrepare = current != g_runtime.pendingTrackGenerations.end() &&
                            generation != current->second;
                    }
                    if (staleAfterPrepare) {
                        // A newer request, including an empty cancellation,
                        // arrived while the plug-in was initializing. Drop the
                        // stale chain before the newer request is consumed.
                        if (accepted) runtime->prepare({}, callbackSampleRate(), 16384, nullptr);
                        g_runtime.selectionStatus.store(1, std::memory_order_release);
                    } else if (accepted) {
                        runtime->requested = selection;
                        g_runtime.requestedTrackSelections[trackKey] = selection;
                        runtime->error.clear();
                        g_runtime.selectionPreparedNanoseconds.store(steadyNanoseconds(), std::memory_order_release);
                        g_runtime.selectionCommittedNanoseconds.store(steadyNanoseconds(), std::memory_order_release);
                        g_runtime.audioGeneration.fetch_add(1, std::memory_order_acq_rel);
                        g_runtime.selectionStatus.store(3, std::memory_order_release);
                    } else {
                        if (error.empty()) error = prepareError;
                        const auto rejected = error.empty() ?
                            std::string("runtime_vst3_selection_prepare_failed") : error;
                        for (const auto &entry : selection)
                            if (!containsIdentity(runtime->requested, entry))
                                rejectSavedEntryOnQtThread(entry, rejected,
                                                           state::ScopeKind::Track, runtime->scoreKey,
                                                           runtime->trackKey, runtime->trackIndex);
                        runtime->error = error.empty() ? prepareError : error;
                        runtime->failedSelection = selection;
                        g_runtime.selectionPreparedNanoseconds.store(steadyNanoseconds(), std::memory_order_release);
                        g_runtime.selectionStatus.store(4, std::memory_order_release);
                    }
                }
            }
            g_selectionStateChanged.store(true, std::memory_order_release);
        }
        g_runtime.selectionWorkerBusy.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> requestLock(g_runtime.selectionRequestMutex);
            if (g_runtime.selectionRequestPending || !g_runtime.pendingTrackSelections.empty())
                g_runtime.selectionStatus.store(1, std::memory_order_release);
        }
        if (const auto notifier = g_selectionNotifier.load(std::memory_order_acquire)) notifier();
    }
}

void startSelectionWorker() {
    std::lock_guard<std::mutex> lock(g_runtime.selectionRequestMutex);
    if (g_runtime.selectionWorker.joinable()) return;
    if (!g_inputMonitor.event.load()) {
        // Keep this process-lifetime event alive even across worker shutdown;
        // a callback may have loaded its handle immediately before retiring.
        g_inputMonitor.event.store(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    }
    g_runtime.selectionWorkerStop = false;
    g_runtime.selectionWorker = std::thread(selectionWorkerLoop);
}

void stopSelectionWorker() noexcept {
    {
        std::lock_guard<std::mutex> lock(g_runtime.selectionRequestMutex);
        if (!g_runtime.selectionWorker.joinable()) return;
        g_runtime.selectionWorkerStop = true;
        g_runtime.inputSelectionRequestPending = false;
        ++g_runtime.inputRequestGeneration;
        g_runtime.selectionRequestPending = false;
        g_runtime.pendingSelection.clear();
        g_runtime.pendingTrackSelections.clear();
        g_runtime.pendingTrackGenerations.clear();
        g_runtime.pendingBindings.clear();
        g_runtime.pendingDiscovered = 0;
        g_runtime.trackContextRequestPending = false;
        g_runtime.pendingPreloads.clear();
    }
    wakeSelectionWorker();
    g_runtime.selectionWorker.join();
    std::lock_guard<std::mutex> lock(g_runtime.selectionRequestMutex);
    g_runtime.selectionWorkerStop = false;
}

bool sameSelection(const std::vector<Vst3SelectionEntry> &left,
                  const std::vector<Vst3SelectionEntry> &right) noexcept {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index)
        if (left[index].module != right[index].module || left[index].classId != right[index].classId ||
            left[index].componentState != right[index].componentState ||
            left[index].controllerState != right[index].controllerState) return false;
    return true;
}

int trackRuntimeIndexFor(const std::string &key) noexcept {
    for (int index = 0; index < 32; ++index)
        if (g_runtime.trackRuntimes[index].trackKey == key) return index;
    for (int index = 0; index < 32; ++index)
        if (g_runtime.trackRuntimes[index].trackKey.empty()) return index;
    return -1;
}

void clearTrackDispatch() noexcept {
    for (auto &entry : g_runtime.trackDispatch) {
        entry.self.store(nullptr, std::memory_order_release);
        entry.runtime.store(nullptr, std::memory_order_release);
    }
}

// An odd generation closes admission while the control thread replaces
// bindings. Audio readers never wait and cannot pair an old self with a new
// runtime, including while that runtime's state and counters are recycled.
struct TrackDispatchRead {
    bool acquired = false;
    TrackDispatchRead() noexcept {
        const auto generation = g_runtime.trackDispatchGeneration.load();
        if (generation & 1) return;
        g_runtime.trackDispatchReaders.fetch_add(1);
        acquired = g_runtime.trackDispatchGeneration.load() == generation;
        if (!acquired) g_runtime.trackDispatchReaders.fetch_sub(1);
    }
    ~TrackDispatchRead() { if (acquired) g_runtime.trackDispatchReaders.fetch_sub(1); }
};

struct TrackDispatchUpdate {
    TrackDispatchUpdate() noexcept {
        g_runtime.trackDispatchGeneration.fetch_add(1);
        while (g_runtime.trackDispatchReaders.load() != 0) std::this_thread::yield();
    }
    ~TrackDispatchUpdate() { g_runtime.trackDispatchGeneration.fetch_add(1); }
};

void persistRuntimeEntries(const std::vector<Vst3SelectionEntry> &entries, state::ScopeKind scope,
                           const std::string &score = {}, const std::string &track = {}, int trackIndex = -1) {
    if (entries.empty()) return;
    QJsonObject chain;
    if (!state::loadChain(chain)) return;
    auto effects = state::scopeEffects(chain, scope, QString::fromStdString(score), QString::fromStdString(track));
    for (int index = 0; index < effects.size(); ++index) {
        auto effect = effects[index].toObject();
        for (const auto &entry : entries) {
            if (effect.value("module").toString().toStdString() != entry.module ||
                effect.value("class_id").toString().toStdString() != entry.classId) continue;
            const auto encode = [](const std::vector<unsigned char> &bytes) {
                return QString::fromLatin1(QByteArray(reinterpret_cast<const char *>(bytes.data()),
                    static_cast<int>(bytes.size())).toBase64());
            };
            effect.insert("component_state", encode(entry.componentState));
            effect.insert("controller_state", encode(entry.controllerState));
        }
        effects[index] = effect;
    }
    state::setScopeEffects(chain, scope, effects, QString::fromStdString(score), QString::fromStdString(track), trackIndex);
    state::writeChain(chain);
}

void rejectSavedEntry(const Vst3SelectionEntry &entry, const std::string &error, state::ScopeKind scope,
                      const std::string &score, const std::string &track, int trackIndex) {
    QJsonObject chain;
    if (!state::loadChain(chain)) return;
    auto entries = state::scopeEffects(chain, scope, QString::fromStdString(score), QString::fromStdString(track));
    for (int index = 0; index < entries.size(); ++index) {
        auto effect = entries[index].toObject();
        if (effect.value("module").toString().toStdString() != entry.module ||
            effect.value("class_id").toString().toStdString() != entry.classId) continue;
        effect.insert("enabled", false);
        effect.insert("bypass", true);
        effect.insert("last_error", QString::fromStdString(error));
        effect.remove("desired_enabled");
        entries[index] = effect;
    }
    state::setScopeEffects(chain, scope, entries, QString::fromStdString(score), QString::fromStdString(track), trackIndex);
    if (state::writeChain(chain)) g_selectionStateChanged = true;
}

void rejectSavedEntryOnQtThread(const Vst3SelectionEntry &entry, const std::string &error,
                                state::ScopeKind scope, const std::string &score,
                                const std::string &track, int trackIndex) noexcept {
    if (onQtThread() || !invokeOnQtThreadBlocking([&] {
            rejectSavedEntry(entry, error, scope, score, track, trackIndex);
        }))
        rejectSavedEntry(entry, error, scope, score, track, trackIndex);
}

void saveTrackRuntime(TrackRuntime &runtime) {
    const auto active = runtime.chain.snapshot().activeSlot;
    if (active < 0) return;
    runtime.chain.deactivate();
    std::vector<Vst3SelectionEntry> entries;
    auto &slot = runtime.trackSlots[active];
    for (std::size_t index = 0; index < slot.count; ++index) entries.push_back(slot.effects[index]->captureState());
    runtime.chain.activate(active);
    runtime.requested = entries;
    g_runtime.requestedTrackSelections[runtime.trackKey] = entries;
    persistRuntimeEntries(entries, state::ScopeKind::Track, runtime.scoreKey, runtime.trackKey, runtime.trackIndex);
}

bool reconfigureSlot(SelectionSlot &slot, effects::Chain &chain, int rate) {
    if (!slot.count || slot.effects[0]->configuredRate.load() == rate) return true;
    const auto active = chain.snapshot().activeSlot;
    chain.deactivate();
    bool valid = true;
    for (std::size_t index = 0; index < slot.count; ++index)
        valid = slot.effects[index]->reconfigure(rate, 16384) && valid;
    if (valid && active >= 0) {
        chain.clearFault();
        chain.activate(active);
    }
    return valid;
}

void refreshTrackContextWorkerImpl(std::vector<gp_audio::Binding> bindings,
                                   std::size_t discovered) noexcept {
    if (g_inEditorCallback) return;
    if (!g_initial.hostSupported) return;
    const auto selected = std::find_if(bindings.begin(), bindings.end(),
        [](const gp_audio::Binding &binding) { return binding.activeDocument && binding.selectedTrack; });
    const bool haveSelectedBinding = selected != bindings.end();
    std::unique_lock<std::recursive_mutex> editorLock(g_runtime.editorMutex, std::try_to_lock);
    if (!editorLock.owns_lock()) return;
    std::unique_lock<std::mutex> lock(g_runtime.selectionMutex, std::try_to_lock);
    if (!lock.owns_lock()) return;
    g_runtime.currentTrackKey = haveSelectedBinding ? selected->trackKey : "";
    // Before the first enable, publish scope metadata without entering a
    // plug-in. Qt still has to install the hooks in prepare(), which takes
    // editorMutex; a worker holding it while waiting for Qt would deadlock.
    const bool hooksInstalled = g_runtime.master.ready() && g_runtime.dsp.ready();
    if (hooksInstalled && !g_runtime.projectGlobalRestored && haveSelectedBinding &&
        qEnvironmentVariable("GPVST3_DISABLE_PROJECT_RESTORE") != "1") {
        const auto desiredGlobal = enabledSelectionFromScope(state::ScopeKind::Global);
        if (!desiredGlobal.empty()) {
            std::string error;
            if (configureSelectedChain(desiredGlobal, &error)) {
                configureInputSelection(desiredGlobal);
                g_runtime.requestedSelection = desiredGlobal;
                g_runtime.appliedSelection = desiredGlobal;
            }
        }
        g_runtime.projectGlobalRestored = true;
    }
    const int rate = static_cast<int>(callbackSampleRate());
    const auto sameBindingTopology = [](const gp_audio::Binding &a, const gp_audio::Binding &b) {
        return a.chain == b.chain && a.trackIndex == b.trackIndex && a.soundIndex == b.soundIndex &&
               a.trackKey == b.trackKey && a.trackId == b.trackId &&
               a.documentId == b.documentId && a.scoreKey == b.scoreKey &&
               a.activeDocument == b.activeDocument;
    };
    const auto sameBindings = bindings.size() == g_runtime.publishedBindings.size() &&
        std::all_of(bindings.begin(), bindings.end(), [&](const gp_audio::Binding &candidate) {
            // Bridge/native collectors can legitimately enumerate the same
            // chains in a different order after a cursor move. Treat that as
            // the same topology so a selection event cannot repeatedly clear
            // the audio dispatch table.
            return std::any_of(g_runtime.publishedBindings.begin(),
                               g_runtime.publishedBindings.end(),
                               [&](const gp_audio::Binding &published) {
                                   return sameBindingTopology(candidate, published);
                               });
        });
    bool pending = !sameBindings || (g_runtime.selectionMode.load() &&
        (g_runtime.selectionConfiguredRate.load() != rate || g_runtime.chain.faulted()));
    if (hooksInstalled) reconfigureInputRouterIfNeeded();
    for (const auto &runtime : g_runtime.trackRuntimes) {
        if (runtime.trackKey.empty()) continue;
        const auto requested = g_runtime.requestedTrackSelections.find(runtime.trackKey);
        pending |= requested != g_runtime.requestedTrackSelections.end() && !sameSelection(runtime.requested, requested->second) &&
            (runtime.error.empty() || !sameSelection(runtime.failedSelection, requested->second));
        pending |= runtime.count.load(std::memory_order_acquire) > 0 &&
            runtime.configuredRate.load() != rate;
        pending |= runtime.chain.faulted();
    }
    if (!pending) return;
    // A pure selection event never changes the EffectsChain -> TrackRuntime
    // topology. Keep the published dispatch table live while the worker
    // updates inspector scope or reconfigures a fixed runtime; clearing it
    // here creates an avoidable window in which every DSP callback reports an
    // unresolved scope. Topology changes still use the guarded replacement
    // below after the affected runtimes have been prepared.
    const bool topologyChanged = !sameBindings;
    std::unique_ptr<TrackDispatchUpdate> topologyUpdate;
    if (topologyChanged) {
        topologyUpdate = std::make_unique<TrackDispatchUpdate>();
        clearTrackDispatch();
    }
    g_runtime.trackRuntimeError.clear();
    bool stable = true;
    std::size_t published = 0;
    std::array<bool, 32> used{};
    const auto globalActive = g_runtime.chain.snapshot().activeSlot;
    if (globalActive >= 0 && g_runtime.selectionMode.load() && g_runtime.chain.faulted()) {
        auto &slot = g_runtime.selectionSlots[globalActive];
        const int failed = slot.failedIndex.load(std::memory_order_acquire);
        if (failed >= 0 && static_cast<std::size_t>(failed) < slot.count) {
            std::vector<Vst3SelectionEntry> remaining;
            g_runtime.chain.deactivate();
            for (std::size_t index = 0; index < slot.count; ++index) remaining.push_back(slot.effects[index]->captureState());
            g_runtime.chain.activate(globalActive);
            persistRuntimeEntries(remaining, state::ScopeKind::Global);
            rejectSavedEntry(remaining[failed], "runtime_vst3_process_failed", state::ScopeKind::Global);
            remaining.erase(remaining.begin() + failed);
            std::string error;
            if (configureSelectedChain(remaining, &error)) g_runtime.requestedSelection = remaining;
        }
    }
    const auto configuredGlobal = g_runtime.chain.snapshot().activeSlot;
    if (configuredGlobal >= 0 && g_runtime.selectionMode.load() &&
        reconfigureSlot(g_runtime.selectionSlots[configuredGlobal], g_runtime.chain, rate))
        g_runtime.selectionConfiguredRate.store(rate, std::memory_order_release);
    // Retire disappeared tracks before allocating slots to newly inserted
    // tracks, while preserving their state for undo or reopening a document.
    for (auto &runtime : g_runtime.trackRuntimes) {
        if (runtime.trackKey.empty()) continue;
        if (std::any_of(bindings.begin(), bindings.end(), [&](const gp_audio::Binding &binding) {
            return binding.chain && binding.activeDocument && binding.trackKey == runtime.trackKey;
        })) continue;
        saveTrackRuntime(runtime);
        runtime.shutdown();
        runtime.trackKey.clear(); runtime.trackId.clear(); runtime.requested.clear();
        runtime.error.clear(); runtime.failedSelection.clear();
    }
    for (const auto &binding : bindings) {
        if (!binding.chain || !binding.activeDocument) continue;
        const auto runtimeIndex = trackRuntimeIndexFor(binding.trackKey);
        if (runtimeIndex < 0) { stable = false; continue; }
        used[static_cast<std::size_t>(runtimeIndex)] = true;
        auto &runtime = g_runtime.trackRuntimes[runtimeIndex];
        runtime.trackKey = binding.trackKey;
        runtime.keyHash.store(stableTrackKeyHash(binding.trackKey), std::memory_order_release);
        runtime.trackId = binding.trackId;
        if (!runtime.scoreKey.empty() && runtime.scoreKey != binding.scoreKey) saveTrackRuntime(runtime);
        runtime.scoreKey = binding.scoreKey;
        runtime.trackIndex = binding.trackIndex;
        // Track selections are explicit runtime requests.  Do not restore the
        // sidecar's enabled flags while a score is being opened: the UI must
        // start with every track plug-in disabled until the user checks it.
        const auto faultSlot = runtime.chain.snapshot().activeSlot;
        if (faultSlot >= 0 && runtime.chain.faulted()) {
            const int failed = runtime.trackSlots[faultSlot].failedIndex.load(std::memory_order_acquire);
            if (failed >= 0 && static_cast<std::size_t>(failed) < runtime.trackSlots[faultSlot].count) {
                saveTrackRuntime(runtime);
                auto remaining = runtime.requested;
                rejectSavedEntry(remaining[failed], "runtime_vst3_process_failed", state::ScopeKind::Track,
                    runtime.scoreKey, runtime.trackKey, runtime.trackIndex);
                remaining.erase(remaining.begin() + failed);
                g_runtime.requestedTrackSelections[runtime.trackKey] = remaining;
            }
        }
        const auto requested = g_runtime.requestedTrackSelections.find(runtime.trackKey);
        const bool queued = trackSelectionQueued(runtime.trackKey);
        if (hooksInstalled && requested != g_runtime.requestedTrackSelections.end() && !queued &&
            (!runtime.configured.load(std::memory_order_acquire) ||
             !sameSelection(runtime.requested, requested->second)) &&
            (runtime.error.empty() || !sameSelection(runtime.failedSelection, requested->second))) {
            std::string error;
            if (runtime.prepare(requested->second, callbackSampleRate(), 16384, &error)) {
                runtime.requested = requested->second;
                runtime.error.clear();
            } else {
                runtime.error = error;
                runtime.failedSelection = requested->second;
                for (const auto &entry : requested->second)
                    if (!containsIdentity(runtime.requested, entry))
                        rejectSavedEntryOnQtThread(entry, error, state::ScopeKind::Track,
                            runtime.scoreKey, runtime.trackKey, runtime.trackIndex);
            }
        }
        if (requested != g_runtime.requestedTrackSelections.end() && sameSelection(runtime.requested, requested->second))
            runtime.error.clear();
        const auto active = runtime.chain.snapshot().activeSlot;
        if (active >= 0 && reconfigureSlot(runtime.trackSlots[active], runtime.chain, rate))
            runtime.configuredRate.store(rate, std::memory_order_release);
        if (!runtime.error.empty()) g_runtime.trackRuntimeError = runtime.error;
        auto &dispatch = g_runtime.trackDispatch[published];
        dispatch.runtime.store(&runtime, std::memory_order_release);
        dispatch.self.store(binding.chain, std::memory_order_release);
        if (published == 0) g_runtime.trackDispatchSelf0.store(reinterpret_cast<std::uintptr_t>(binding.chain), std::memory_order_release);
        if (published == 1) g_runtime.trackDispatchSelf1.store(reinterpret_cast<std::uintptr_t>(binding.chain), std::memory_order_release);
        ++published;
        if (published == 64) break;
    }
    for (std::size_t index = 0; index < 32; ++index) {
        if (used[index]) continue;
        if (!g_runtime.trackRuntimes[index].trackKey.empty()) {
            saveTrackRuntime(g_runtime.trackRuntimes[index]);
            g_runtime.trackRuntimes[index].shutdown();
        }
        g_runtime.trackRuntimes[index].trackKey.clear();
        g_runtime.trackRuntimes[index].trackId.clear();
        g_runtime.trackRuntimes[index].requested.clear();
    }
    g_runtime.trackBindingsPublished.store(published, std::memory_order_release);
    g_runtime.trackContextObserved.store(discovered != 0, std::memory_order_release);
    g_runtime.trackContextStable.store(stable && published != 0, std::memory_order_release);
    g_runtime.trackScopeUnresolved.store(!(stable && published != 0), std::memory_order_release);
    g_runtime.publishedBindings = bindings;
    g_selectionStateChanged.store(true, std::memory_order_release);
}

std::vector<Vst3SelectionEntry> selectionFromEffects(const QJsonArray &effects) {
    std::vector<Vst3SelectionEntry> result;
    for (const auto &value : effects) {
        const auto effect = value.toObject();
        const auto module = effect.value("module").toString();
        const auto classId = effect.value("class_id").toString();
        if (module.isEmpty() || classId.isEmpty()) continue;
        const auto decode = [&](const char *field) {
            const auto bytes = QByteArray::fromBase64(effect.value(field).toString().toLatin1());
            return std::vector<unsigned char>(bytes.cbegin(), bytes.cend());
        };
        result.push_back({module.toStdString(), classId.toStdString(), decode("component_state"), decode("controller_state")});
    }
    return result;
}

void refreshTrackContextImpl() noexcept {
    if (g_inEditorCallback || !g_initial.hostSupported) return;
    updateAudioLayerState();
    const auto discovered = gpvst3::gp_audio::refreshIfNeeded();
    const auto bindings = gpvst3::gp_audio::snapshot();
    gpvst3::gp_audio::Binding selectedBinding;
    if (gpvst3::gp_audio::currentTrack(selectedBinding)) {
        gpvst3::state::setRuntimeTrackContext(QString::fromStdString(selectedBinding.scoreKey),
            QString::fromStdString(selectedBinding.trackKey), selectedBinding.trackIndex,
            QString::fromStdString(selectedBinding.trackId));
    } else gpvst3::state::clearRuntimeTrackContext();
    startSelectionWorker();
    {
        std::lock_guard<std::mutex> lock(g_runtime.selectionRequestMutex);
        g_runtime.pendingBindings = bindings;
        g_runtime.pendingDiscovered = discovered;
        g_runtime.trackContextRequestPending = true;
    }
    wakeSelectionWorker();
}

const Runtime::TrackDispatch *findTrackDispatch(void *self) noexcept {
    if (!self) return nullptr;
    const auto count = (std::min)(g_runtime.trackBindingsPublished.load(std::memory_order_acquire),
                                  std::size_t{64});
    for (std::size_t index = 0; index < count; ++index) {
        auto &entry = g_runtime.trackDispatch[index];
        if (entry.self.load(std::memory_order_acquire) == self) return &entry;
    }
    return nullptr;
}

double callbackSampleRate() noexcept {
    // Private host getters stay on Qt; workers use its published control value.
    const auto observedRate = g_runtime.rate.load(std::memory_order_relaxed);
    if (observedRate > 0) return static_cast<double>(observedRate);
    return 44100.0;
}

void observeEffectsChainContext(void *self) noexcept {
    if (!self || !g_runtime.effectsChainIndex) return;
    const auto index = static_cast<int>(g_runtime.effectsChainIndex(self));
    if (index < 0) return;
    g_runtime.effectsChainIndexObserved.store(true, std::memory_order_release);
    g_runtime.observedEffectsChainIndex.store(index, std::memory_order_relaxed);

    for (auto &observation : g_runtime.effectsChainObservations) {
        auto known = observation.self.load(std::memory_order_acquire);
        if (known == self) {
            if (observation.index.load(std::memory_order_acquire) != index)
                g_runtime.trackContextStable.store(false, std::memory_order_release);
            return;
        }
        if (known != nullptr) continue;
        if (!observation.self.compare_exchange_strong(known, self,
                                                      std::memory_order_acq_rel,
                                                      std::memory_order_acquire))
            continue;
        observation.index.store(index, std::memory_order_release);
        g_runtime.effectsChainContextCount.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // A full observation table is diagnostic only; processing remains bypassed.
    g_runtime.trackContextStable.store(false, std::memory_order_release);
}

std::uint64_t bufferHash(const void *buffer) noexcept {
    if (!buffer || !g_runtime.rawData || !g_runtime.frameCount || !g_runtime.channelCount) return 0;
    const auto &raw = g_runtime.rawData(buffer);
    const auto frames = (std::min)(g_runtime.frameCount(buffer), static_cast<std::size_t>(4096));
    const auto channels = (std::min)(g_runtime.channelCount(buffer), 2U);
    std::uint64_t hash = 1469598103934665603ULL;
    for (unsigned channel = 0; channel < channels; ++channel) {
        if (!raw.channels[channel]) continue;
        for (std::size_t frame = 0; frame < frames; ++frame) {
            std::uint32_t bits = 0;
            std::memcpy(&bits, raw.channels[channel] + frame, sizeof(bits));
            hash ^= bits;
            hash *= 1099511628211ULL;
        }
    }
    return hash;
}

void masterProcessHook(void *self, void *buffer, void *ticks, void *musicians,
                       void *backingTrack) {
    const auto original = reinterpret_cast<MasterProcess>(g_runtime.master.trampoline);
    if (g_inMasterHook) {
        original(self, buffer, ticks, musicians, backingTrack);
        return;
    }
    g_inMasterHook = true;
    const auto sequence = g_runtime.callbackSequence.fetch_add(1, std::memory_order_relaxed) + 1;
    const auto bufferAddress = reinterpret_cast<std::uintptr_t>(buffer);
    auto firstSequence = g_runtime.masterFirstSequence.load(std::memory_order_relaxed);
    if (firstSequence == 0)
        g_runtime.masterFirstSequence.compare_exchange_strong(firstSequence, sequence,
                                                               std::memory_order_relaxed);
    auto firstBuffer = g_runtime.masterFirstBuffer.load(std::memory_order_relaxed);
    if (firstBuffer == 0)
        g_runtime.masterFirstBuffer.compare_exchange_strong(firstBuffer, bufferAddress,
                                                             std::memory_order_relaxed);
    g_runtime.masterLastSequence.store(sequence, std::memory_order_relaxed);
    g_runtime.masterLastBuffer.store(bufferAddress, std::memory_order_relaxed);
    // Hashing a complete audio block is diagnostic only. Once the host write
    // evidence has been captured, avoid paying for another full block scan on
    // every realtime callback.
    const bool needBufferHash = !g_runtime.bufferWriteObserved.load(std::memory_order_relaxed);
    const bool needEffectHash = !g_runtime.effectWriteObserved.load(std::memory_order_relaxed);
    const auto before = needBufferHash ? bufferHash(buffer) : 0;
    if (g_runtime.masterCalls.fetch_add(1, std::memory_order_relaxed) == 0) {
        g_runtime.masterThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    }
    g_runtime.frames.store(g_runtime.frameCount(buffer), std::memory_order_relaxed);
    g_runtime.channels.store(g_runtime.channelCount(buffer), std::memory_order_relaxed);
    if (g_runtime.sampleRate && g_runtime.audioCore)
        g_runtime.rate.store(g_runtime.sampleRate(g_runtime.audioCore), std::memory_order_relaxed);
    original(self, buffer, ticks, musicians, backingTrack);
    updateAudioLayerState();
    if (g_runtime.rawData && g_runtime.frameCount && g_runtime.channelCount) {
        const auto &raw = g_runtime.rawData(buffer);
        const auto frames = g_runtime.frameCount(buffer);
        const auto channels = (std::min)(g_runtime.channelCount(buffer), 2U);
        const auto rate = g_runtime.rate.load(std::memory_order_relaxed);
        const float *inputs[2]{raw.channels[0], raw.channels[1]};
        float *outputs[2]{raw.channels[0], raw.channels[1]};
        const audio::BlockView block{inputs, nullptr, outputs, nullptr, channels, frames,
                                     rate > 0 ? rate : 44100.0, frames, buffer, sequence, true};
        const auto effectBefore = needEffectHash ? bufferHash(buffer) : 0;
        const auto active = g_runtime.chain.snapshot().activeSlot;
        const bool selected = g_runtime.selectionPublished.load(std::memory_order_acquire);
        const auto configuredRate = active < 0 || selected ? 0 :
            g_runtime.effects[active].configuredRate.load(std::memory_order_acquire);
        const auto configuredBlock = active < 0 || selected ? std::size_t{0} :
            g_runtime.effects[active].configuredBlock.load(std::memory_order_acquire);
        const bool configurationMatches = active < 0 || (selected
            ? (g_runtime.selectionConfiguredRate.load(std::memory_order_acquire) == rate && frames <= 16384)
            : (configuredRate == rate && frames <= configuredBlock));
        effects::Chain::ProcessResult chainResult;
        if (configurationMatches)
            chainResult = g_runtime.chain.process(block);
        else {
            g_runtime.configurationMismatchBlocks.fetch_add(1, std::memory_order_relaxed);
            chainResult.bypassed = true;
        }
        if (chainResult.error || chainResult.bypassed || !chainResult.completed)
            audio::bypass(block);
        if (chainResult.completed && !chainResult.bypassed && !chainResult.error) {
            g_runtime.effectCalls.fetch_add(1, std::memory_order_relaxed);
            g_runtime.globalChainProcessBlocks.fetch_add(1, std::memory_order_relaxed);
            g_runtime.effectProcessed.store(true, std::memory_order_relaxed);
        }
        if (needEffectHash && effectBefore != bufferHash(buffer) && !chainResult.bypassed)
            g_runtime.effectWriteObserved.store(true, std::memory_order_relaxed);
    }
    if (needBufferHash && before != bufferHash(buffer))
        g_runtime.bufferWriteObserved.store(true, std::memory_order_relaxed);
    g_inMasterHook = false;
}

bool configureRuntimeChain() noexcept {
    const bool first = g_runtime.effects[0].initialize();
    const bool second = first && g_runtime.effects[1].initialize();
    if (!first) {
        g_runtime.chain.deactivate();
        g_runtime.chain.setBypassed(true);
        return false;
    }
    if (!g_runtime.chain.prepareSlot(0, {&g_runtime.effects[0], &RuntimeEffect::processCallback}) ||
        !g_runtime.chain.activate(0)) {
        g_runtime.effects[0].shutdown();
        g_runtime.chain.deactivate();
        g_runtime.chain.setBypassed(true);
        return false;
    }
    if (second && g_runtime.chain.prepareSlot(1, {&g_runtime.effects[1], &RuntimeEffect::processCallback})) {
        g_runtime.chain.activate(1);
        g_runtime.chain.activate(0);
    }
    const char *bypass = std::getenv("GPVST3_TOTAL_BYPASS");
    g_runtime.chain.setBypassed(bypass && std::strcmp(bypass, "1") == 0);
    return true;
}

input::Route configuredInputRoute() noexcept {
    const char *configured = std::getenv("GPVST3_P4_ROUTE");
    // Global effects process score playback. Input monitoring is a separate,
    // explicit route; enabling a global effect must not open a microphone loop.
    if (!configured || !*configured)
        return input::Route::Disabled;
    if (std::strcmp(configured, "disabled") == 0)
        return input::Route::Disabled;
    if (std::strcmp(configured, "input_insert") == 0)
        return input::Route::InputInsert;
    if (std::strcmp(configured, "bus_mix") == 0)
        return input::Route::BusMix;
    return input::Route::Disabled;
}

void updateAudioLayerState() noexcept {
    if (g_runtime.sampleRate && g_runtime.audioCore) {
        const auto rate = g_runtime.sampleRate(g_runtime.audioCore);
        if (rate > 0) g_runtime.rate.store(rate, std::memory_order_release);
    }
    if (!g_runtime.audioLayer) return;
    if (g_runtime.audioLayerInputLevel) {
        const auto level = g_runtime.audioLayerInputLevel(g_runtime.audioLayer);
        if (std::isfinite(level)) {
            g_runtime.hostInputLevel.store(level, std::memory_order_relaxed);
            g_runtime.hostInputLevelObserved.store(true, std::memory_order_release);
        }
    }
    if (g_runtime.audioLayerIsRunning) {
        const auto running = g_runtime.audioLayerIsRunning(g_runtime.audioLayer);
        g_runtime.hostStreamRunning.store(running, std::memory_order_release);
        g_runtime.inputRouter.setStreamRunning(running);
    }
    if (g_runtime.audioLayerBufferSize) {
        const auto frames = g_runtime.audioLayerBufferSize(g_runtime.audioLayer);
        if (frames > 0)
            g_runtime.hostBufferSize.store(static_cast<std::size_t>(frames),
                                           std::memory_order_relaxed);
    }
}

bool configureInputRouter() noexcept {
    const auto route = configuredInputRoute();
    const auto parseChannels = [](const char *name, std::size_t fallback) noexcept {
        const char *value = std::getenv(name);
        if (!value || !*value) return fallback;
        char *end = nullptr;
        const auto parsed = std::strtoul(value, &end, 10);
        if (end == value || *end != '\0' || parsed == 0 || parsed > 2) return fallback;
        return static_cast<std::size_t>(parsed);
    };
    g_runtime.inputChannelCount = parseChannels("GPVST3_P4_INPUT_CHANNELS", 2);
    g_runtime.outputChannelCount = parseChannels("GPVST3_P4_OUTPUT_CHANNELS", 2);
    g_runtime.inputRouter.setRoute(route);
    g_runtime.inputRouter.setEnabled(false);
    g_runtime.inputRouter.setStreamRunning(true);
    g_runtime.inputRouter.setProcessor({});
    g_runtime.inputChain.setBypassed(true);
    g_runtime.inputChain.deactivate();
    if (route == input::Route::Disabled) return true;
    const double initialRate = callbackSampleRate();
    if (!g_runtime.inputRouter.prepare(2, portaudio::kMaxFrames))
        return false;
    g_runtime.inputConfiguredRate.store(static_cast<int>(initialRate), std::memory_order_release);
    g_runtime.inputConfiguredChannels.store(g_runtime.inputChannelCount, std::memory_order_release);
    g_runtime.inputConfiguredOutputChannels.store(g_runtime.outputChannelCount, std::memory_order_release);
    g_runtime.inputRouter.setBypassed(true);
    updateAudioLayerState();
    return true;
}

bool inputFeatureEnabled() noexcept {
    const char *enabled = std::getenv("GPVST3_ENABLE_P4_INPUT");
    return !enabled || std::strcmp(enabled, "0") != 0;
}

bool configureInputSelection(const std::vector<Vst3SelectionEntry> &selection,
                             std::string *error) noexcept {
    if (error) error->clear();
    if (!g_runtime.stream.ready() || !inputFeatureEnabled()) return true;

    // Stop accepting new input blocks, then wait for the callback currently
    // inside the router before replacing its processor/context pointers.
    g_runtime.inputRouter.setEnabled(false);
    lockInputProcessing();
    const InputProcessingGuard release;

    if (selection.empty() || configuredInputRoute() == input::Route::Disabled) {
        const auto active = g_runtime.inputChain.snapshot().activeSlot;
        if (active >= 0) g_runtime.retainedInputSelectionSlot = active;
        g_runtime.inputChain.setBypassed(true);
        g_runtime.inputChain.deactivate();
        // Keep the prepared instances warm. They are detached from the
        // callback immediately by bypass/deactivate and can be reused on the
        // next enable without loading the module or restoring state again.
        g_runtime.inputRouter.setProcessor({});
        g_runtime.inputRouter.setRoute(configuredInputRoute());
        g_runtime.inputRouter.setBypassed(true);
        refreshInputParameterMirrors();
        return true;
    }
    if (g_runtime.inputRouter.channelCapacity() == 0 && !configureInputRouter()) {
        if (error) *error = "input_router_prepare_failed";
        return false;
    }

    const auto rate = callbackSampleRate() > 0.0 ? callbackSampleRate() : 44100.0;
    std::vector<Vst3SelectionEntry> inputSelection = selection;
    // GPVST3_P4_ROUTE is the old isolated P4 fixture switch. When it is
    // explicitly supplied, retain its standalone test processor semantics;
    // normal installed use (no override) follows the selected global chain.
    if (const char *legacyRoute = std::getenv("GPVST3_P4_ROUTE");
        legacyRoute && *legacyRoute && std::strcmp(legacyRoute, "disabled") != 0) {
        if (const char *runtimePath = std::getenv("GPVST3_RUNTIME_VST3");
            runtimePath && *runtimePath)
            inputSelection = {{runtimePath, {}}};
    }
    const auto old = g_runtime.inputChain.snapshot().activeSlot;
    const auto previousSlot = old >= 0 ? old : g_runtime.retainedInputSelectionSlot;
    const auto *previous = previousSlot >= 0 ? &g_runtime.inputSelectionSlots[previousSlot] : nullptr;
    const bool warm = previous && previous->matches(inputSelection, rate, 16384);
    const auto target = warm ? static_cast<unsigned>(previousSlot) : (previousSlot == 0 ? 1U : 0U);
    if (!warm) {
        if (!g_runtime.inputChain.prepareSlot(target,
                {&g_runtime.inputSelectionSlots[target], &SelectionSlot::processCallback})) {
            if (error) *error = "input_vst3_chain_prepare_failed";
            return false;
        }
        if (!g_runtime.inputSelectionSlots[target].prepare(inputSelection, rate, 16384, g_runtime.inputSelectionPool, error))
            return false;
    }
    if (!g_runtime.inputChain.activate(target)) {
        if (error && error->empty()) *error = "input_vst3_chain_activate_failed";
        g_runtime.inputSelectionSlots[target].shutdown();
        return false;
    }
    g_runtime.inputChain.clearFault();
    g_runtime.inputChain.setBypassed(false);
    g_runtime.inputSelectionSlots[target].markActive();
    g_runtime.inputPreloaded = g_runtime.inputSelectionPool.hasPreloaded();
    g_runtime.retainedInputSelectionSlot = static_cast<int>(target);
    g_runtime.inputRouter.setRoute(configuredInputRoute());
    g_runtime.inputRouter.setProcessor({&g_runtime.inputChain, &processInputChain});
    g_runtime.inputRouter.setBypassed(false);
    // A PortAudio callback can begin before AudioLayer's observation accessor
    // reports its first running state. The callback itself is the authoritative
    // liveness signal; the maintenance path will still clear this flag when GP
    // stops the stream.
    g_runtime.inputRouter.setStreamRunning(true);
    g_runtime.inputRouter.setEnabled(true);
    g_runtime.inputConfiguredRate.store(static_cast<int>(rate), std::memory_order_release);
    g_runtime.inputConfigurationPending.store(false, std::memory_order_release);
    refreshInputParameterMirrors();
    return true;
}

bool validateReconfiguration() noexcept {
    constexpr double rates[] = {44100.0, 48000.0, 96000.0};
    constexpr std::size_t blocks[] = {64, 128, 256};
    std::size_t passed = 0;
    std::size_t failed = 0;
    for (const auto rate : rates) {
        for (const auto block : blocks) {
            const auto active = g_runtime.chain.snapshot().activeSlot;
            const auto target = active == 0 ? 1U : 0U;
            if (!g_runtime.effects[target].ready.load(std::memory_order_acquire) ||
                !g_runtime.effects[target].reconfigure(rate, block) ||
                !g_runtime.chain.prepareSlot(target,
                    {&g_runtime.effects[target], &RuntimeEffect::processCallback}) ||
                !g_runtime.chain.activate(target)) {
                ++failed;
                continue;
            }
            ++passed;
        }
    }
    const auto active = g_runtime.chain.snapshot().activeSlot;
    const auto restore = active == 0 ? 1U : 0U;
    if (g_runtime.effects[restore].ready.load(std::memory_order_acquire) &&
        g_runtime.effects[restore].reconfigure(44100.0, 16384) &&
        g_runtime.chain.prepareSlot(restore,
            {&g_runtime.effects[restore], &RuntimeEffect::processCallback}) &&
        g_runtime.chain.activate(restore)) {
        ++passed;
    } else {
        ++failed;
    }
    g_runtime.reconfigurationPassed.store(passed, std::memory_order_release);
    g_runtime.reconfigurationFailed.store(failed, std::memory_order_release);
    g_runtime.reconfigurationValidated.store(failed == 0 && passed == 10,
                                              std::memory_order_release);
    return failed == 0 && passed == 10;
}

void dspProcessHook(void *self, void *buffer, void *scratch, void *ticks) {
    g_runtime.trackLastDspSelf.store(reinterpret_cast<std::uintptr_t>(self), std::memory_order_relaxed);
    const auto sequence = g_runtime.callbackSequence.fetch_add(1, std::memory_order_relaxed) + 1;
    const auto bufferAddress = reinterpret_cast<std::uintptr_t>(buffer);
    auto firstSequence = g_runtime.dspFirstSequence.load(std::memory_order_relaxed);
    if (firstSequence == 0)
        g_runtime.dspFirstSequence.compare_exchange_strong(firstSequence, sequence,
                                                            std::memory_order_relaxed);
    auto firstBuffer = g_runtime.dspFirstBuffer.load(std::memory_order_relaxed);
    if (firstBuffer == 0)
        g_runtime.dspFirstBuffer.compare_exchange_strong(firstBuffer, bufferAddress,
                                                          std::memory_order_relaxed);
    g_runtime.dspLastSequence.store(sequence, std::memory_order_relaxed);
    g_runtime.dspLastBuffer.store(bufferAddress, std::memory_order_relaxed);
    const auto masterSequence = g_runtime.masterLastSequence.load(std::memory_order_relaxed);
    const auto masterBuffer = g_runtime.masterLastBuffer.load(std::memory_order_relaxed);
    if (masterSequence != 0 && sequence > masterSequence)
        g_runtime.dspAfterMasterObserved.store(true, std::memory_order_relaxed);
    if (masterBuffer != 0 && masterBuffer == bufferAddress)
        g_runtime.sameBufferObserved.store(true, std::memory_order_relaxed);
    if (g_runtime.dspCalls.fetch_add(1, std::memory_order_relaxed) == 0)
        g_runtime.dspThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    // EffectsChain::index() is retained as a diagnostic field. The actual
    // scope is resolved through the control-thread-published chain pointer
    // table built from GuitarProMCP's verified track/IAudioBuffer bridge.
    g_runtime.trackContextObserved.store(self != nullptr, std::memory_order_release);
    observeEffectsChainContext(self);
    if (g_inMasterHook) g_runtime.dspInsideMaster.store(true, std::memory_order_relaxed);
    reinterpret_cast<DspProcess>(g_runtime.dsp.trampoline)(self, buffer, scratch, ticks);

    const TrackDispatchRead reader;
    if (!reader.acquired) return;

    const auto markUnresolved = [] {
        if (!g_runtime.trackRuntimeProcessed.load(std::memory_order_acquire))
            g_runtime.trackScopeUnresolved.store(true, std::memory_order_release);
    };

    const auto *dispatch = findTrackDispatch(self);
    if (!dispatch) {
        g_runtime.trackDispatchMisses.fetch_add(1, std::memory_order_relaxed);
        g_trackTopologyInvalidated.store(true, std::memory_order_release);
    }
    auto *trackRuntime = dispatch ? dispatch->runtime.load(std::memory_order_acquire) : nullptr;
    if (!trackRuntime || !g_runtime.rawData || !g_runtime.frameCount ||
        !g_runtime.channelCount || !buffer) {
        markUnresolved();
        return;
    }
    const auto frames = g_runtime.frameCount(buffer);
    const auto channels = g_runtime.channelCount(buffer);
    if (frames == 0 || frames > 16384 || channels == 0 || channels > 2) {
        markUnresolved();
        return;
    }
    const auto &raw = g_runtime.rawData(buffer);
    if (!raw.channels[0] || (channels > 1 && !raw.channels[1])) {
        markUnresolved();
        return;
    }
    const auto rate = callbackSampleRate();
    const float *inputs[2]{raw.channels[0], raw.channels[1]};
    float *outputs[2]{raw.channels[0], raw.channels[1]};
    const audio::BlockView block{inputs, nullptr, outputs, nullptr,
                                 channels, frames, rate, frames, buffer,
                                 sequence, true};
    const bool needTrackHash = trackRuntime->count.load(std::memory_order_acquire) != 0 &&
        !trackRuntime->writeObserved.load(std::memory_order_relaxed);
    const auto before = needTrackHash ? bufferHash(buffer) : 0;
    bool actuallyProcessed = false;
    const bool completed = trackRuntime->processBlock(block, &actuallyProcessed);
    const auto after = needTrackHash ? bufferHash(buffer) : 0;
    if (completed && trackRuntime->count.load(std::memory_order_acquire) != 0) {
        g_runtime.trackChainProcessBlocks.fetch_add(1, std::memory_order_relaxed);
        if (actuallyProcessed) {
            g_runtime.trackChainProcessedBlocks.fetch_add(1, std::memory_order_relaxed);
            g_runtime.trackRuntimeProcessed.store(true, std::memory_order_release);
        }
        if (needTrackHash && before != after) {
            trackRuntime->writeObserved.store(true, std::memory_order_release);
            g_runtime.trackRuntimeWriteObserved.store(true, std::memory_order_release);
        }
        g_runtime.lastTrackRuntimeIndex.store(
            static_cast<int>(trackRuntime - g_runtime.trackRuntimes),
            std::memory_order_release);
        g_runtime.trackScopeUnresolved.store(false, std::memory_order_release);
    } else {
        if (!completed) g_runtime.trackChainErrorBlocks.fetch_add(1, std::memory_order_relaxed);
        g_runtime.trackScopeUnresolved.store(false, std::memory_order_release);
    }
}

void cursorMoveHook(void *self, const void *current, const void *previous) {
    const auto original = reinterpret_cast<CursorMove>(g_runtime.cursorMove.trampoline);
    if (original) original(self, current, previous);
    // The detour performs no host reads or synchronization. The control
    // notifier coalesces this event and refreshes the immutable selection
    // snapshot after the host cursor has completed its move.
    gpvst3::gp_audio::markSelectionDirty();
}

bool cursorTrackHook(void *self, int index) {
    const auto original = reinterpret_cast<CursorTrack>(g_runtime.cursorTrack.trampoline);
    const bool result = original ? original(self, index) : false;
    if (result) gpvst3::gp_audio::markSelectionDirty();
    return result;
}

void scoreDuplicateTrackHook(void *self, unsigned index) {
    g_runtime.topologyDuplicateCalls.fetch_add(1, std::memory_order_relaxed);
    const auto original = reinterpret_cast<ScoreDuplicateTrack>(g_runtime.scoreDuplicateTrack.trampoline);
    if (original) original(self, index);
    gp_audio::markExplicitTopologyDirty();
}

void scoreCreateTrackHook(void *self, unsigned index, const void *track,
                          unsigned flags, bool first, bool second, bool third, unsigned value) {
    const auto original = reinterpret_cast<ScoreCreateTrack>(g_runtime.scoreCreateTrack.trampoline);
    if (original) original(self, index, track, flags, first, second, third, value);
    gp_audio::markExplicitTopologyDirty();
}

void scoreSwapTracksHook(void *self, unsigned first, unsigned second) {
    g_runtime.topologySwapCalls.fetch_add(1, std::memory_order_relaxed);
    const auto original = reinterpret_cast<ScoreSwapTracks>(g_runtime.scoreSwapTracks.trampoline);
    if (original) original(self, first, second);
    gp_audio::markTopologyDirty();
}


bool install(Patch &patch, void *target, void *detour, const std::uint8_t *expected,
             std::size_t size, std::size_t tailJumpOffset = 0) noexcept {
    if (patch.restartRequired) return false;
    if (patch.installed) return patch.target == target && patch.ready();
    Q_UNUSED(tailJumpOffset);
    if (!target || !detour || !expected || size < 5 || size > sizeof(patch.original) ||
        std::memcmp(target, expected, size) != 0) return false;
    // Retained lifecycle hooks and callbacks can outlive Qt's plugin owner.
    // Keep their code and every trampoline valid until process exit.
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(detour), &pinned)) return false;
    const auto initialized = MH_Initialize();
    if (initialized != MH_OK && initialized != MH_ERROR_ALREADY_INITIALIZED) return false;
    if (!patch.managed) {
        if (MH_CreateHook(target, detour, &patch.trampoline) != MH_OK) return false;
        patch.target = target;
        patch.size = size;
        patch.managed = true;
        std::memcpy(patch.original, expected, size);
    } else if (patch.target != target) return false;
    MH_STATUS status = MH_ERROR_THREAD_BUSY;
    for (int attempt = 0; attempt < 16 && status == MH_ERROR_THREAD_BUSY; ++attempt) {
        status = MH_EnableHooksStrict(&target, 1);
        if (status == MH_ERROR_THREAD_BUSY) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    BOOL enabled = FALSE;
    const auto queried = MH_IsHookEnabled(target, &enabled);
    if (queried == MH_OK) patch.installed = enabled != FALSE;
    patch.lastStatus = status == MH_OK ? queried : status;
    patch.restartRequired = queried != MH_OK || status == MH_ERROR_PATCH_ROLLBACK ||
        status == MH_ERROR_THREAD_RESUME;
    // A late OS failure can leave complete detours installed. Keep that actual
    // state for retirement, but never turn the failed installation into success.
    return patch.ready();
}

void remove(Patch &patch) noexcept {
    if (!patch.installed) return;
    if (patch.managed) {
        MH_STATUS status = MH_ERROR_THREAD_BUSY;
        for (int attempt = 0; attempt < 16 && status == MH_ERROR_THREAD_BUSY; ++attempt) {
            status = MH_DisableHooksStrict(&patch.target, 1);
            if (status == MH_ERROR_THREAD_BUSY) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        BOOL enabled = FALSE;
        const auto queried = MH_IsHookEnabled(patch.target, &enabled);
        if (queried == MH_OK) patch.installed = enabled != FALSE;
        patch.lastStatus = status == MH_OK ? queried : status;
        patch.restartRequired = patch.restartRequired || queried != MH_OK ||
            status == MH_ERROR_PATCH_ROLLBACK || status == MH_ERROR_THREAD_RESUME;
        // Preflight/flush failure keeps detours, but a late protection/resume
        // failure may have disabled them. Track state independently of success.
        // Never free trampoline code while an earlier call may return into it.
        return;
    }
}

EntryPointObservation observe(HMODULE module, const char *symbol) noexcept {
    EntryPointObservation result;
    result.moduleLoaded = module != nullptr;
    result.exportFound = module && GetProcAddress(module, symbol) != nullptr;
    return result;
}

} // namespace

void refreshTrackContext() noexcept {
    refreshTrackContextImpl();
}

bool installListenerProbe() noexcept {
    auto &patch = g_runtime.listenerProbe;
    if (patch.installed) return true;
    const auto *base = reinterpret_cast<const std::uint8_t *>(GetModuleHandleW(nullptr));
    constexpr std::uint8_t prologue[]{0x48, 0x89, 0x5C, 0x24, 0x18,
        0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55};
    auto *original = const_cast<std::uint8_t *>(base + 0x4FDBD0);
    auto *slot = reinterpret_cast<void *volatile *>(const_cast<std::uint8_t *>(base + 0x25ABB00));
    if (std::memcmp(original, prologue, sizeof(prologue)) != 0 ||
        reinterpret_cast<std::uintptr_t>(slot) % alignof(void *) != 0) return false;
    DWORD protection = 0;
    if (!VirtualProtect(const_cast<void **>(slot), sizeof(void *), PAGE_READWRITE, &protection))
        return false;
    // The listener is already called by the device thread. A single aligned
    // pointer exchange permits both old and new calls without patching live
    // instructions. Publish the permanent original before exposing the hook.
    patch.target = const_cast<void **>(slot);
    patch.trampoline = original;
    g_listenerExecutableBase = base;
    const auto previous = InterlockedCompareExchangePointer(slot,
        reinterpret_cast<void *>(&listenerProbeHook), original);
    DWORD unused = 0;
    VirtualProtect(patch.target, sizeof(void *), protection, &unused);
    patch.installed = previous == original;
    return patch.installed;
}

#ifdef GPVST3_P13_PROBE_BUILD
bool installRseProbe() noexcept {
    auto &patch = g_runtime.rseProbe;
    if (patch.installed) return true;
    const auto *base = reinterpret_cast<const std::uint8_t *>(GetModuleHandleW(L"GPRSE.dll"));
    if (!base) return false;
    constexpr std::uint8_t prologue[]{0x4C, 0x89, 0x4C, 0x24, 0x20, 0x55, 0x53, 0x56};
    auto *original = const_cast<std::uint8_t *>(base + 0x492B0);
    auto *slot = reinterpret_cast<void *volatile *>(const_cast<std::uint8_t *>(base + 0x23ADA0));
    if (std::memcmp(original, prologue, sizeof(prologue)) != 0 ||
        reinterpret_cast<std::uintptr_t>(slot) % alignof(void *) != 0) return false;
    DWORD protection = 0;
    if (!VirtualProtect(const_cast<void **>(slot), sizeof(void *), PAGE_READWRITE, &protection)) return false;
    g_rseProbeBase = base;
    patch.target = const_cast<void **>(slot);
    patch.trampoline = original;
    const auto previous = InterlockedCompareExchangePointer(slot,
        reinterpret_cast<void *>(&rseProbeHook), original);
    DWORD unused = 0;
    VirtualProtect(patch.target, sizeof(void *), protection, &unused);
    patch.installed = previous == original;
    return patch.installed;
}

#endif

void removeProbeSlot(Patch &patch, void *hook) noexcept {
    if (!patch.installed) return;
    DWORD protection = 0;
    if (!VirtualProtect(patch.target, sizeof(void *), PAGE_READWRITE, &protection)) return;
    const auto previous = InterlockedCompareExchangePointer(
        static_cast<void *volatile *>(patch.target), patch.trampoline,
        hook);
    DWORD unused = 0;
    VirtualProtect(patch.target, sizeof(void *), protection, &unused);
    if (previous == hook) patch.installed = false;
    // In-flight hooks keep using the permanent original address.
}
void removeListenerProbe() noexcept {
    removeProbeSlot(g_runtime.listenerProbe, reinterpret_cast<void *>(&listenerProbeHook));
#ifdef GPVST3_P13_PROBE_BUILD
    removeProbeSlot(g_runtime.rseProbe, reinterpret_cast<void *>(&rseProbeHook));
#endif
}

bool editorCallbackActive() noexcept {
    return g_inEditorCallback || static_cast<bool>(std::atomic_load(&g_openEditorEffect));
}

void setSelectionNotifier(SelectionNotifier notifier) noexcept {
    g_selectionNotifier.store(notifier, std::memory_order_release);
}

void setTrackContextNotifier(TrackContextNotifier notifier) noexcept {
    g_trackContextNotifier.store(notifier, std::memory_order_release);
}

bool configureSelectedChain(const std::vector<Vst3SelectionEntry> &selection,
                            std::string *error = nullptr) noexcept;

State prepare(const host::Verification &verification, bool enableForSelection) noexcept {
    std::lock_guard<std::recursive_mutex> editorLock(g_runtime.editorMutex);
    g_qtDispatchStopping.store(false, std::memory_order_release);
    g_verification = verification;
    g_trackTopologyInvalidated.store(false, std::memory_order_release);
    State result;
    g_runtime.effectsChainIndex = nullptr;
    result.hostSupported = verification.supported;
    if (!verification.supported) {
        result.reason = "host_unsupported";
        g_initial = result;
        return result;
    }
    const auto gprse = GetModuleHandleW(L"GPRSE.dll");
    const auto amaudio = GetModuleHandleW(L"AMAudio.dll");
    g_runtime.audioModule = amaudio;
#ifdef GPVST3_P13_PROBE_BUILD
    // Called only after the complete host hash gate, before callback install.
    configureInputProbe();
#endif
    result.masterProcess = observe(gprse, kMasterProcess);
    result.effectsChainProcessDsp = observe(gprse, kEffectsChainProcessDsp);
    g_runtime.effectsChainIndex = reinterpret_cast<EffectsChainIndexFn>(GetProcAddress(gprse, kEffectsChainIndex));
    result.audioBufferAccessorsFound =
        observe(amaudio, kRawData).exportFound && observe(amaudio, kFrameCount).exportFound &&
        observe(amaudio, kChannelCount).exportFound;
    result.audioBufferLockAccessorsFound =
        observe(amaudio, kLock).exportFound && observe(amaudio, kUnlock).exportFound;
    result.audioLayerInputLevelAccessorFound =
        observe(amaudio, kAudioLayerInstance).exportFound &&
        observe(amaudio, kAudioLayerInputLevel).exportFound;
    result.audioOutputCallback.moduleLoaded = amaudio != nullptr;
    g_runtime.audioLayerInputLevel =
        reinterpret_cast<AudioLayerInputLevelFn>(GetProcAddress(amaudio, kAudioLayerInputLevel));
    g_runtime.audioLayerIsRunning =
        reinterpret_cast<AudioLayerIsRunningFn>(GetProcAddress(amaudio, kAudioLayerIsRunning));
    g_runtime.audioLayerBufferSize =
        reinterpret_cast<AudioLayerBufferSizeFn>(GetProcAddress(amaudio, kAudioLayerBufferSize));
    const auto audioLayerInstance =
        reinterpret_cast<AudioLayerInstanceFn>(GetProcAddress(amaudio, kAudioLayerInstance));
    if (audioLayerInstance) g_runtime.audioLayer = audioLayerInstance();
    updateAudioLayerState();
    const char *enabled = std::getenv("GPVST3_ENABLE_P2_HOOK");
    result.enabled = (enabled && std::strcmp(enabled, "1") == 0) ||
                     (enableForSelection && (!enabled || !*enabled));
    const char *effectEnabled = std::getenv("GPVST3_ENABLE_P2_EFFECT");
    result.runtimeEffectEnabled = result.enabled && effectEnabled && std::strcmp(effectEnabled, "1") == 0;
    if (!gprse) result.reason = "gprse_not_loaded";
    else if (!result.masterProcess.exportFound || !result.effectsChainProcessDsp.exportFound)
        result.reason = "entry_points_not_found";
    else if (!result.audioBufferAccessorsFound) result.reason = "buffer_accessors_not_found";
    else if (!result.enabled) result.reason = enabled && *enabled
        ? "realtime_disabled_by_environment" : "realtime_waiting_for_selection";
    else {
        g_runtime.rawData = reinterpret_cast<RawDataFn>(GetProcAddress(amaudio, kRawData));
        g_runtime.frameCount = reinterpret_cast<FrameCountFn>(GetProcAddress(amaudio, kFrameCount));
        g_runtime.channelCount = reinterpret_cast<ChannelCountFn>(GetProcAddress(amaudio, kChannelCount));
        g_runtime.lock = reinterpret_cast<BufferAccessFn>(GetProcAddress(amaudio, kLock));
        g_runtime.unlock = reinterpret_cast<BufferAccessFn>(GetProcAddress(amaudio, kUnlock));
        g_runtime.sampleRate = reinterpret_cast<SampleRateFn>(GetProcAddress(amaudio, kSampleRate));
        const auto coreInstance = reinterpret_cast<AudioCoreInstanceFn>(GetProcAddress(amaudio, kAudioCoreInstance));
        if (coreInstance) g_runtime.audioCore = coreInstance();
        updateAudioLayerState();
        const bool master = install(g_runtime.master, GetProcAddress(gprse, kMasterProcess),
                                    reinterpret_cast<void *>(&masterProcessHook), kMasterPrologue,
                                    kMasterPatchBytes);
        const bool dsp = master && install(g_runtime.dsp, GetProcAddress(gprse, kEffectsChainProcessDsp),
                                          reinterpret_cast<void *>(&dspProcessHook), kDspPrologue,
                                          kDspPatchBytes);
        const auto gpcore = GetModuleHandleW(L"GPCore.dll");
        const bool cursorMove = gpcore && install(g_runtime.cursorMove, GetProcAddress(gpcore, kCursorMove),
                                                  reinterpret_cast<void *>(&cursorMoveHook), kCursorMovePrologue,
                                                  kCursorMovePatchBytes);
        const bool cursorTrack = gpcore && install(g_runtime.cursorTrack, GetProcAddress(gpcore, kCursorTrack),
                                                   reinterpret_cast<void *>(&cursorTrackHook), kCursorTrackPrologue,
                                                   kCursorTrackPatchBytes);
        const bool duplicateTrack = gpcore && install(g_runtime.scoreDuplicateTrack,
            GetProcAddress(gpcore, kScoreDuplicateTrack), reinterpret_cast<void *>(&scoreDuplicateTrackHook),
            kScoreDuplicateTrackPrologue, kScoreDuplicateTrackPatchBytes);
        const bool createTrack = gpcore && install(g_runtime.scoreCreateTrack,
            GetProcAddress(gpcore, kScoreCreateTrack), reinterpret_cast<void *>(&scoreCreateTrackHook),
            kScoreCreateTrackPrologue, kScoreCreateTrackPatchBytes);
        const bool swapTracks = gpcore && install(g_runtime.scoreSwapTracks,
            GetProcAddress(gpcore, kScoreSwapTracks), reinterpret_cast<void *>(&scoreSwapTracksHook),
            kScoreSwapTracksPrologue, kScoreSwapTracksPatchBytes, 9);
        Q_UNUSED(duplicateTrack);
        Q_UNUSED(createTrack);
        Q_UNUSED(swapTracks);
        result.selectionHookInstalled = cursorMove || cursorTrack;
        result.selectionHookGatePassed = cursorMove && cursorTrack;
        result.topologyHookInstalled = duplicateTrack || swapTracks;
        result.topologyHookGatePassed = duplicateTrack && swapTracks;
        auto *streamTarget = amaudio
            ? reinterpret_cast<std::uint8_t *>(amaudio) + kStreamCallbackRva
            : nullptr;
        result.audioOutputCallback.exportFound = streamTarget != nullptr;
        install(
            g_runtime.stream, streamTarget, reinterpret_cast<void *>(&streamCallbackHook),
            kStreamPrologue, kStreamPatchBytes);
#ifdef GPVST3_P13_PROBE_BUILD
        if (g_listenerProbeRequested) {
            g_listenerProbeInstalled = installListenerProbe();
        }
        if (g_drainProbeRequested || g_overlayProbeRequested)
            g_rseProbeInstalled.store(installRseProbe(), std::memory_order_release);
        if (qEnvironmentVariable("GPVST3_P13_STREAM_PROBE") == "1" && asioprobe::install(amaudio) &&
            qEnvironmentVariable("GPVST3_P13_RESTART_STREAM") == "1")
            asioprobe::requestNativeReset();
#endif
        result.installed = master && dsp;
        if (!result.installed) {
            remove(g_runtime.master);
            remove(g_runtime.dsp);
            remove(g_runtime.cursorMove);
            remove(g_runtime.cursorTrack);
            remove(g_runtime.scoreDuplicateTrack);
            remove(g_runtime.scoreCreateTrack);
            remove(g_runtime.scoreSwapTracks);
            remove(g_runtime.stream);
            removeListenerProbe();
            g_listenerProbeInstalled = false;
        }
        if (result.installed && result.runtimeEffectEnabled) {
            result.runtimeProcessorReady = configureRuntimeChain();
            result.runtimeEffectError = g_runtime.effects[0].error;
            result.runtimeEffectName = g_runtime.effects[0].name;
            result.observationOnly = !result.runtimeProcessorReady;
            if (result.runtimeProcessorReady)
                result.reconfigurationValidated = validateReconfiguration();
        } else {
            g_runtime.chain.deactivate();
            g_runtime.chain.setBypassed(true);
            g_runtime.effects[0].shutdown();
            g_runtime.effects[1].shutdown();
        }
        if (result.installed && inputFeatureEnabled()) {
            // Prepare the capture adapter before restoring a pre-existing
            // requested selection. configureInputRouter() resets the input
            // slots; doing it afterwards would erase the live copy just
            // prepared for that selection.
            configureInputRouter();
            std::lock_guard<std::mutex> lock(g_runtime.selectionMutex);
            if (!g_runtime.requestedSelection.empty() &&
                configureSelectedChain(g_runtime.requestedSelection)) {
                configureInputSelection(g_runtime.requestedSelection);
                g_runtime.appliedSelection = g_runtime.requestedSelection;
            }
        } else {
            g_runtime.inputRouter.setEnabled(false);
            g_runtime.inputRouter.setRoute(input::Route::Disabled);
            lockInputProcessing();
            g_runtime.inputChain.setBypassed(true);
            g_runtime.inputChain.deactivate();
            g_runtime.inputSelectionSlots[0].shutdown();
            g_runtime.inputSelectionSlots[1].shutdown();
            g_runtime.inputRouter.setProcessor({});
            unlockInputProcessing();
        }
        result.reason = result.installed ? "runtime_observation_active" : "hook_install_failed";
    }
    g_initial = result;
    return result;
}

State snapshot() noexcept {
    static std::shared_ptr<const State> previous = std::make_shared<const State>();
    std::unique_lock<std::mutex> runtimeLock(g_runtime.selectionMutex, std::try_to_lock);
    if (!runtimeLock.owns_lock()) {
        auto result = *std::atomic_load(&previous);
        result.preloadPending = g_runtime.preloadBusy.load(std::memory_order_acquire);
        return result;
    }
    State result = g_initial;
    result.topologyDuplicateCalls = g_runtime.topologyDuplicateCalls.load(std::memory_order_relaxed);
    result.topologySwapCalls = g_runtime.topologySwapCalls.load(std::memory_order_relaxed);
    result.topologyEventCount = gp_audio::topologyEventCount();
    result.trackBindingSource = gp_audio::bindingSource();
    {
        std::lock_guard<std::mutex> preloadLock(g_runtime.selectionRequestMutex);
        result.preloadPending = !g_runtime.pendingPreloads.empty() || g_runtime.preloadBusy.load();
    }
    result.globalPreloaded = g_runtime.globalPreloaded;
    result.inputPreloaded = g_runtime.inputPreloaded;
    result.preloadCompleted = g_runtime.preloadCompleted;
    result.preloadFailed = g_runtime.preloadFailed;
    result.preloadError = g_runtime.preloadError;
    result.trackPreloaded = static_cast<std::size_t>(std::count_if(
        std::begin(g_runtime.trackRuntimes), std::end(g_runtime.trackRuntimes),
        [](const TrackRuntime &runtime) { return runtime.preloaded.load(std::memory_order_acquire); }));
    result.warmCacheLimit = EffectPool::kMaxWarmInstances;
    result.warmCacheEvictions = g_runtime.selectionPool.evictions +
        g_runtime.inputSelectionPool.evictions;
    for (const auto &runtime : g_runtime.trackRuntimes)
        result.warmCacheEvictions += runtime.pool.evictions;
    const auto appendInstances = [&](const char *scope, const std::string &track, const EffectPool &pool,
                                      const SelectionSlot (&preparedSlots)[2], const effects::Chain &chain) {
        const auto slot = chain.snapshot().activeSlot;
        for (const auto &effect : pool.effects) {
            if (!effect || !effect->ready.load(std::memory_order_acquire)) continue;
            bool active = false;
            if (slot >= 0 && !chain.bypassed())
                for (std::size_t i = 0; i < preparedSlots[slot].count; ++i)
                    active |= preparedSlots[slot].effects[i] == effect;
            result.instances.push_back({scope, track, effect->identity.module, effect->identity.classId,
                effect->instanceId, effect->processedBlocks.load(), effect->configuredRate.load(), active,
                !active && effect->preloaded});
        }
    };
    appendInstances("global", {}, g_runtime.selectionPool, g_runtime.selectionSlots, g_runtime.chain);
    appendInstances("input", {}, g_runtime.inputSelectionPool, g_runtime.inputSelectionSlots, g_runtime.inputChain);
    for (const auto &runtime : g_runtime.trackRuntimes)
        if (!runtime.trackKey.empty()) appendInstances("track", runtime.trackKey, runtime.pool,
            runtime.trackSlots, runtime.chain);
    const auto chain = g_runtime.chain.snapshot();
    const auto input = g_runtime.inputRouter.snapshot();
    result.selectionRequestId = g_runtime.selectionRequestId.load(std::memory_order_relaxed);
    result.selectionQueuedNanoseconds = g_runtime.selectionQueuedNanoseconds.load(std::memory_order_relaxed);
    result.selectionWorkerStartedNanoseconds = g_runtime.selectionWorkerStartedNanoseconds.load(std::memory_order_relaxed);
    result.selectionPreparedNanoseconds = g_runtime.selectionPreparedNanoseconds.load(std::memory_order_relaxed);
    result.selectionCommittedNanoseconds = g_runtime.selectionCommittedNanoseconds.load(std::memory_order_relaxed);
    result.selectionAppliedGeneration = g_runtime.selectionAppliedGeneration.load(std::memory_order_relaxed);
    result.selectionGeneration = gp_audio::selectionGeneration();
    result.bindingGeneration = gp_audio::bindingGeneration();
    result.contextPublishLatencyNanoseconds = gp_audio::contextPublishLatencyNanoseconds();
    result.droppedRefreshCount = gp_audio::droppedRefreshCount();
    result.scoreOpen = gp_audio::hasActiveDocument();
    result.selectionEventSource = result.scoreOpen
        ? (result.selectionHookInstalled ? "native_cursor_hook"
            : (std::string(gp_audio::bindingSource()) == "mcp_context_native_registry"
                ? "mcp_bridge_cursor" : "native_cursor_or_fallback"))
        : "none";
    result.audioGeneration = g_runtime.audioGeneration.load(std::memory_order_relaxed);
    switch (g_runtime.selectionStatus.load(std::memory_order_acquire)) {
    case 1: result.selectionStatus = "queued"; break;
    case 2: result.selectionStatus = "preparing"; break;
    case 3: result.selectionStatus = "applied"; break;
    case 4: result.selectionStatus = "failed"; break;
    default: result.selectionStatus = "idle"; break;
    }
    result.chainActivationNanoseconds = chain.activationNanoseconds;
    result.chainFirstProcessedNanoseconds = chain.firstProcessedNanoseconds;
    result.chainActivationSequence = chain.activationSequence;
    result.chainFirstProcessedSequence = chain.firstProcessedSequence;
    result.chainCallbacksToFirstProcess = chain.callbacksToFirstProcess;
    const auto inputChainSnapshot = g_runtime.inputChain.snapshot();
    result.inputActivationNanoseconds = inputChainSnapshot.activationNanoseconds;
    result.inputFirstProcessedNanoseconds = inputChainSnapshot.firstProcessedNanoseconds;
    result.inputFirstProcessedSequence = inputChainSnapshot.firstProcessedSequence;
    result.inputCallbacksToFirstProcess = inputChainSnapshot.callbacksToFirstProcess;
    result.editorStage = editorStageName(static_cast<EditorStage>(
        g_runtime.editorStage.load(std::memory_order_acquire)));
    result.editorResultCode = g_runtime.editorResultCode.load(std::memory_order_acquire);
    result.editorRequestGeneration = g_runtime.editorRequestGeneration.load(std::memory_order_acquire);
    if (std::unique_lock<std::recursive_mutex> editorSnapshot(g_runtime.editorMutex,
                                                              std::try_to_lock);
        editorSnapshot.owns_lock()) {
        result.editorIdentity = g_runtime.editorIdentity;
        result.editorError = g_runtime.editorError;
    }
    result.installed = g_runtime.master.ready() && g_runtime.dsp.ready();
    result.audioOutputCallbackInstalled = g_runtime.stream.ready();
    result.audioOutputObserved = g_runtime.outputObserved.load(std::memory_order_acquire);
    result.audioOutputWritebackObserved =
        g_runtime.outputWriteObserved.load(std::memory_order_acquire);
    result.audioOutputCallback.callObserved =
        g_runtime.outputCalls.load(std::memory_order_relaxed) != 0;
    result.audioOutputCallback.bufferWriteObserved = result.audioOutputWritebackObserved;
    result.audioOutputCallback.callCount =
        g_runtime.outputCalls.load(std::memory_order_relaxed);
    result.audioOutputCallback.frameCount =
        g_runtime.outputFrames.load(std::memory_order_relaxed);
    result.audioOutputCallback.threadId =
        g_runtime.outputThread.load(std::memory_order_relaxed);
    result.audioOutputCallback.firstBufferAddress =
        g_runtime.outputFirstBuffer.load(std::memory_order_relaxed);
    result.audioOutputCallback.lastBufferAddress =
        g_runtime.outputLastBuffer.load(std::memory_order_relaxed);
    result.audioOutputCallback.beforeHash =
        g_runtime.outputBeforeHash.load(std::memory_order_relaxed);
    result.audioOutputCallback.afterHash =
        g_runtime.outputAfterHash.load(std::memory_order_relaxed);
    result.audioBufferPointerObserved =
        g_runtime.effects[0].ownerObserved.load(std::memory_order_acquire) ||
        g_runtime.effects[1].ownerObserved.load(std::memory_order_acquire);
    result.audioBufferWritebackObserved =
        g_runtime.effects[0].outputWritten.load(std::memory_order_acquire) ||
        g_runtime.effects[1].outputWritten.load(std::memory_order_acquire);
    result.audioBufferSequenceCount =
        static_cast<std::size_t>(g_runtime.callbackSequence.load(std::memory_order_relaxed));
    result.masterProcess.callCount = g_runtime.masterCalls.load(std::memory_order_relaxed);
    result.masterProcess.callObserved = result.masterProcess.callCount != 0;
    result.masterProcess.bufferWriteObserved = g_runtime.bufferWriteObserved.load(std::memory_order_relaxed);
    result.masterProcess.frameCount = g_runtime.frames.load(std::memory_order_relaxed);
    result.masterProcess.channelCount = g_runtime.channels.load(std::memory_order_relaxed);
    result.masterProcess.sampleRate = g_runtime.rate.load(std::memory_order_relaxed);
    result.masterProcess.threadId = g_runtime.masterThread.load(std::memory_order_relaxed);
    result.masterProcess.firstSequence = g_runtime.masterFirstSequence.load(std::memory_order_relaxed);
    result.masterProcess.lastSequence = g_runtime.masterLastSequence.load(std::memory_order_relaxed);
    result.masterProcess.firstBufferAddress = g_runtime.masterFirstBuffer.load(std::memory_order_relaxed);
    result.masterProcess.lastBufferAddress = g_runtime.masterLastBuffer.load(std::memory_order_relaxed);
    result.effectsChainProcessDsp.callCount = g_runtime.dspCalls.load(std::memory_order_relaxed);
    result.effectsChainProcessDsp.callObserved = result.effectsChainProcessDsp.callCount != 0;
    result.effectsChainProcessDsp.threadId = g_runtime.dspThread.load(std::memory_order_relaxed);
    result.effectsChainProcessDsp.firstSequence = g_runtime.dspFirstSequence.load(std::memory_order_relaxed);
    result.effectsChainProcessDsp.lastSequence = g_runtime.dspLastSequence.load(std::memory_order_relaxed);
    result.effectsChainProcessDsp.firstBufferAddress = g_runtime.dspFirstBuffer.load(std::memory_order_relaxed);
    result.effectsChainProcessDsp.lastBufferAddress = g_runtime.dspLastBuffer.load(std::memory_order_relaxed);
    result.effectsChainInsideMaster = g_runtime.dspInsideMaster.load(std::memory_order_relaxed);
    result.effectsChainAfterMasterObserved =
        g_runtime.dspAfterMasterObserved.load(std::memory_order_relaxed);
    result.sameBufferObserved = g_runtime.sameBufferObserved.load(std::memory_order_relaxed);
    result.globalChainEnabled = g_runtime.selectionPublished.load(std::memory_order_acquire) ||
        g_runtime.chain.snapshot().activeSlot >= 0;
    result.globalChainProcessBlocks = g_runtime.globalChainProcessBlocks.load(std::memory_order_relaxed);
    result.trackChainProcessBlocks = g_runtime.trackChainProcessBlocks.load(std::memory_order_relaxed);
    result.trackChainProcessedBlocks = g_runtime.trackChainProcessedBlocks.load(std::memory_order_relaxed);
    result.trackDispatchMisses = g_runtime.trackDispatchMisses.load(std::memory_order_relaxed);
    result.trackLastDspSelf = g_runtime.trackLastDspSelf.load(std::memory_order_relaxed);
    result.trackDispatchSelf0 = g_runtime.trackDispatchSelf0.load(std::memory_order_relaxed);
    result.trackDispatchSelf1 = g_runtime.trackDispatchSelf1.load(std::memory_order_relaxed);
    result.trackBindingsPublished = g_runtime.trackBindingsPublished.load(std::memory_order_relaxed);
    result.trackRuntimeProcessed = g_runtime.trackRuntimeProcessed.load(std::memory_order_acquire);
    result.trackRuntimeWriteObserved = g_runtime.trackRuntimeWriteObserved.load(std::memory_order_acquire);
    result.trackContextObserved = g_runtime.trackContextObserved.load(std::memory_order_acquire);
    result.trackContextStable = g_runtime.trackContextStable.load(std::memory_order_acquire);
    result.trackScopeUnresolved = g_runtime.trackScopeUnresolved.load(std::memory_order_acquire);
    result.effectsChainIndexAccessorFound = result.effectsChainProcessDsp.moduleLoaded &&
        result.effectsChainProcessDsp.exportFound && g_runtime.effectsChainIndex != nullptr;
    result.effectsChainIndexObserved = g_runtime.effectsChainIndexObserved.load(std::memory_order_acquire);
    result.observedEffectsChainIndex = g_runtime.observedEffectsChainIndex.load(std::memory_order_relaxed);
    result.effectsChainContextCount =
        g_runtime.effectsChainContextCount.load(std::memory_order_relaxed);
    {
        const auto runtimeIndex = g_runtime.lastTrackRuntimeIndex.load(std::memory_order_acquire);
        if (runtimeIndex >= 0 && runtimeIndex < 32 &&
            !g_runtime.trackRuntimes[runtimeIndex].trackKey.empty())
            result.trackContextKey = g_runtime.trackRuntimes[runtimeIndex].trackKey;
        else
            result.trackContextKey = g_runtime.currentTrackKey;
        result.trackRuntimeError = g_runtime.trackRuntimeError;
        result.trackRuntimeEvidence.clear();
        for (const auto &runtime : g_runtime.trackRuntimes) {
            if (runtime.trackKey.empty()) continue;
            TrackRuntimeEvidence evidence;
            evidence.trackKey = runtime.trackKey;
            evidence.trackId = runtime.trackId;
            evidence.processBlocks = runtime.processBlocks.load(std::memory_order_relaxed);
            evidence.processedBlocks = runtime.processedBlocks.load(std::memory_order_relaxed);
            evidence.bypassBlocks = runtime.bypassBlocks.load(std::memory_order_relaxed);
            evidence.errorBlocks = runtime.errorBlocks.load(std::memory_order_relaxed);
            evidence.configuredEffects = runtime.count.load(std::memory_order_acquire);
            evidence.configured = runtime.configured.load(std::memory_order_acquire);
            evidence.processed = runtime.processed.load(std::memory_order_acquire);
            evidence.writeObserved = runtime.writeObserved.load(std::memory_order_acquire);
            result.trackRuntimeEvidence.push_back(std::move(evidence));
        }
    }
    result.crossThreadObserved = result.masterProcess.threadId != 0 &&
        result.effectsChainProcessDsp.threadId != 0 &&
        result.masterProcess.threadId != result.effectsChainProcessDsp.threadId;
    const bool selectionPublished = g_runtime.selectionPublished.load(std::memory_order_acquire);
    result.runtimeProcessorReady = chain.activeSlot >= 0 && chain.activeSlot < 2 &&
        (g_runtime.selectionMode.load(std::memory_order_acquire)
             ? g_runtime.selectionSlots[chain.activeSlot].count > 0 &&
                   g_runtime.selectionSlots[chain.activeSlot].effects[0]->ready.load(std::memory_order_acquire)
             : (!selectionPublished && g_runtime.effects[chain.activeSlot].ready.load(std::memory_order_acquire)));
    result.runtimeProcessCount = g_runtime.effectCalls.load(std::memory_order_relaxed);
    result.runtimeConfigurationMismatchBlocks =
        g_runtime.configurationMismatchBlocks.load(std::memory_order_relaxed);
    const auto activeRate = chain.activeSlot >= 0 && chain.activeSlot < 2
        ? (g_runtime.selectionMode.load(std::memory_order_acquire)
               ? g_runtime.selectionSlots[chain.activeSlot].effects[0]->configuredRate.load(std::memory_order_acquire)
               : g_runtime.effects[chain.activeSlot].configuredRate.load(std::memory_order_acquire))
        : 0;
    const auto activeBlock = chain.activeSlot >= 0 && chain.activeSlot < 2
        ? (g_runtime.selectionMode.load(std::memory_order_acquire)
               ? g_runtime.selectionSlots[chain.activeSlot].effects[0]->configuredBlock.load(std::memory_order_acquire)
               : g_runtime.effects[chain.activeSlot].configuredBlock.load(std::memory_order_acquire))
        : std::size_t{0};
    result.runtimeConfigurationMatches = chain.activeSlot >= 0 && chain.activeSlot < 2 &&
        activeRate == g_runtime.rate.load(std::memory_order_relaxed) &&
        g_runtime.frames.load(std::memory_order_relaxed) <= activeBlock;
    result.runtimeProcessObserved = g_runtime.effectProcessed.load(std::memory_order_relaxed);
    result.runtimeBufferWriteObserved = g_runtime.effectWriteObserved.load(std::memory_order_relaxed);
    const auto active = chain.activeSlot >= 0 && chain.activeSlot < 2 ? chain.activeSlot : 0;
    if (g_runtime.selectionMode.load(std::memory_order_acquire)) {
        result.runtimeEffectName = g_runtime.selectionSlots[active].effects[0]->name;
        const auto runtimeEditorError = g_runtime.selectionSlots[active].effects[0]->getEditorError();
        result.runtimeEffectError = !runtimeEditorError.empty()
            ? runtimeEditorError
            : g_runtime.selectionSlots[active].effects[0]->error;
    } else if (!selectionPublished) {
        result.runtimeEffectName = g_runtime.effects[active].name;
        result.runtimeEffectError = g_runtime.effects[active].error;
    } else {
        result.runtimeEffectName.clear();
        result.runtimeEffectError.clear();
    }
    result.totalBypass = chain.bypassed;
    result.chainFaulted = chain.faulted;
    result.chainActiveSlot = chain.activeSlot;
    result.chainPreparedSlots = chain.preparedSlots;
    result.chainProcessBlocks = chain.processBlocks;
    result.chainProcessedBlocks = chain.processedBlocks;
    result.chainBypassBlocks = chain.bypassBlocks;
    result.chainErrorBlocks = chain.errorBlocks;
    result.chainFallbackBlocks = chain.fallbackBlocks;
    result.chainSwitchRequests = chain.switchRequests;
    result.chainSwitchPrepared = chain.switchPrepared;
    result.chainRetiredSlots = chain.retiredSlots;
    result.chainLastSwitchNanoseconds = chain.lastSwitchNanoseconds;
    result.chainMaxSwitchNanoseconds = chain.maxSwitchNanoseconds;
    result.chainLastReaderDrainNanoseconds = chain.lastReaderDrainNanoseconds;
    result.chainMaxReaderDrainNanoseconds = chain.maxReaderDrainNanoseconds;
    result.chainReaderDrainTimeouts = chain.readerDrainTimeouts;
    result.chainSequenceGaps = chain.sequenceGaps;
    result.chainLastSequence = chain.lastSequence;
    result.chainRampSamples = chain.rampSamples;
    result.chainRampRemaining = chain.rampRemaining;
    result.lastProcessNanoseconds = chain.lastProcessNanoseconds;
    result.maxProcessNanoseconds = chain.maxProcessNanoseconds;
    result.totalProcessNanoseconds = chain.totalProcessNanoseconds;
    result.chainSwitchCount = chain.switchCount;
    result.runtimeEffectInstances = 0;
    if (g_runtime.selectionMode.load(std::memory_order_acquire)) {
        if (chain.activeSlot >= 0 && chain.activeSlot < 2)
            result.runtimeEffectInstances = g_runtime.selectionSlots[chain.activeSlot].count;
    } else if (!selectionPublished) {
        for (const auto &effect : g_runtime.effects)
            if (effect.ready.load(std::memory_order_acquire)) ++result.runtimeEffectInstances;
    }
    result.reconfigurationPassed = g_runtime.reconfigurationPassed.load(std::memory_order_acquire);
    result.reconfigurationFailed = g_runtime.reconfigurationFailed.load(std::memory_order_acquire);
    result.reconfigurationValidated = g_runtime.reconfigurationValidated.load(std::memory_order_acquire);
    result.audioLayerInputLevelObserved =
        g_runtime.hostInputLevelObserved.load(std::memory_order_acquire);
    result.audioLayerStreamRunning = g_runtime.hostStreamRunning.load(std::memory_order_acquire);
    result.audioLayerBufferSize = g_runtime.hostBufferSize.load(std::memory_order_relaxed);
    result.inputCapturePathLocated =
        g_runtime.inputCapturePathLocated.load(std::memory_order_acquire);
    result.inputCaptureObserved = input.captureObserved;
    result.inputRouteEnabled = input.enabled;
    result.inputAfterOriginalBlocks = g_runtime.inputAfterOriginalBlocks.load(std::memory_order_acquire);
    result.inputPostOriginalHash = g_runtime.inputPostOriginalHash.load(std::memory_order_relaxed);
    result.inputPostRouteHash = g_runtime.inputPostRouteHash.load(std::memory_order_relaxed);
    result.inputOrderSamplesObserved = g_runtime.inputOrderSamplesObserved.load(std::memory_order_acquire);
    if (result.inputOrderSamplesObserved) {
        result.inputOrderCaptureSample = g_runtime.inputOrderCaptureSample.load(std::memory_order_relaxed);
        result.inputOrderGeneratedSample = g_runtime.inputOrderGeneratedSample.load(std::memory_order_relaxed);
        result.inputOrderOutputSample = g_runtime.inputOrderOutputSample.load(std::memory_order_relaxed);
    }
    const auto inputChain = g_runtime.inputChain.snapshot();
    result.inputProcessorReady = input.enabled && inputChain.activeSlot >= 0 &&
        !inputChain.bypassed && !inputChain.faulted;
    result.inputRoute = input::routeName(input.route);
    if (input.route == input::Route::Disabled)
        result.inputRouteReason = "p4_route_disabled";
    else if (!result.inputProcessorReady)
        result.inputRouteReason = "p4_input_processor_unavailable";
    else if (!result.inputCapturePathLocated)
        result.inputRouteReason = "p4_capture_tap_unresolved";
    else if (!input.captureObserved)
        result.inputRouteReason = "p4_capture_tap_waiting";
    else
        result.inputRouteReason = "p4_capture_tap_active";
    result.inputCaptureBlocks = input.captureBlocks;
    result.inputProcessedBlocks = input.inputProcessedBlocks;
    result.inputBusMixedBlocks = input.busMixedBlocks;
    result.inputBypassBlocks = input.bypassBlocks;
    result.inputErrorBlocks = input.errorBlocks;
    result.inputDroppedBlocks = input.droppedBlocks;
    result.inputFrameCount = input.frameCount;
    result.inputChannelCount = input.channelCount;
    result.inputSampleRate = input.sampleRate;
    result.inputLastPeak = input.lastPeak;
    result.inputMaxPeak = input.maxPeak;
    result.inputLastRms = input.lastRms;
    result.inputInterleavedFormatObserved = input.interleavedFormatObserved;
    result.inputInterleavedObserved = input.interleavedInputObserved;
    result.inputInterleavedOutputWritten = input.interleavedOutputWritten;
    result.inputInterleavedBlocks = input.interleavedBlocks;
    result.inputInterleavedFormatErrors = input.interleavedFormatErrors;
    result.inputInterleavedMissingBlocks = input.interleavedMissingBlocks;
    result.inputInterleavedInputChannelCount = input.interleavedInputChannelCount;
    result.inputInterleavedOutputChannelCount = input.interleavedOutputChannelCount;
    result.inputFirstCaptureAddress = input.firstCaptureAddress;
    result.inputLastCaptureAddress = input.lastCaptureAddress;
    result.inputFirstCaptureOwner = input.firstCaptureOwner;
    result.inputLastCaptureOwner = input.lastCaptureOwner;
    result.inputFirstOutputAddress = input.firstOutputAddress;
    result.inputLastOutputAddress = input.lastOutputAddress;
    result.inputCaptureFormat = input.interleavedFormatObserved ? "interleaved_float32" : "unresolved";
    result.inputCaptureChannelLayout = input.interleavedInputChannelCount == 1
        ? "mono" : (input.interleavedInputChannelCount == 2 ? "stereo" : "unresolved");
    result.inputConfigurationObserved =
        g_runtime.inputObservedRate.load(std::memory_order_acquire) > 0;
    result.inputConfiguredInputChannels =
        g_runtime.inputObservedChannels.load(std::memory_order_relaxed);
    result.inputConfiguredOutputChannels =
        g_runtime.inputObservedOutputChannels.load(std::memory_order_relaxed);
    result.inputConfiguredSampleRate = static_cast<double>(
        g_runtime.inputObservedRate.load(std::memory_order_relaxed));
    result.inputConfigurationErrors =
        g_runtime.inputConfigurationErrors.load(std::memory_order_relaxed);
    std::atomic_store(&previous, std::make_shared<const State>(result));
    return result;
}

bool configureSelectedChain(const std::vector<Vst3SelectionEntry> &selection, std::string *error) noexcept {
    if (error) error->clear();
    if (!g_runtime.master.ready() || selection.size() > SelectionSlot::kMaxEffects) {
        if (error) *error = !g_runtime.master.ready() ? "hook_install_failed" : "runtime_vst3_chain_full";
        return false;
    }
    const auto rate = callbackSampleRate();
    if (selection.empty()) {
        g_runtime.chain.setBypassed(true);
        g_runtime.chain.deactivate();
        g_runtime.selectionMode.store(false, std::memory_order_release);
        g_runtime.selectionPublished.store(true, std::memory_order_release);
        return true;
    }
    const auto old = g_runtime.chain.snapshot().activeSlot;
    const int previousSlot = old >= 0 ? old : g_runtime.retainedSelectionSlot;
    const auto *previous = previousSlot >= 0 ? &g_runtime.selectionSlots[previousSlot] : nullptr;
    if (old >= 0 && g_runtime.selectionConfiguredRate.load() != static_cast<int>(rate))
        g_runtime.chain.deactivate();
    if (previous && previous->matches(selection, rate, 16384)) {
        const auto slot = static_cast<unsigned>(previousSlot);
        if (!g_runtime.chain.activate(slot)) {
            if (error) *error = "runtime_vst3_chain_activate_failed";
            return false;
        }
        g_runtime.selectionMode.store(true, std::memory_order_release);
        g_runtime.selectionConfiguredRate.store(static_cast<int>(rate), std::memory_order_release);
        g_runtime.selectionPublished.store(true, std::memory_order_release);
        g_runtime.chain.clearFault();
        g_runtime.chain.setBypassed(false);
        g_runtime.retainedSelectionSlot = static_cast<int>(slot);
        g_runtime.selectionSlots[slot].markActive();
        g_runtime.globalPreloaded = g_runtime.selectionPool.hasPreloaded();
        refreshInputParameterMirrors();
        return true;
    }
    const auto target = previousSlot == 0 ? 1U : 0U;
    if (!g_runtime.chain.prepareSlot(target,
            {&g_runtime.selectionSlots[target], &SelectionSlot::processCallback})) {
        if (error) *error = "runtime_vst3_chain_prepare_failed";
        return false;
    }
    try {
        if (!g_runtime.selectionSlots[target].prepare(selection, rate, 16384, g_runtime.selectionPool, error)) return false;
    } catch (...) {
        g_runtime.selectionSlots[target].shutdown();
        if (error) *error = "runtime_vst3_initialize_exception";
        return false;
    }
    // Prepare before the handoff. Activation only flips atomics at a block
    // boundary; the previous slot remains owned by the runtime until that
    // slot is safely reused on a later request. This keeps UI/control calls
    // out of reader-drain waits while preserving processor lifetime.
    g_runtime.selectionMode.store(!selection.empty(), std::memory_order_release);
    g_runtime.selectionConfiguredRate.store(static_cast<int>(rate), std::memory_order_release);
    g_runtime.selectionPublished.store(true, std::memory_order_release);
    g_runtime.selectionSlots[target].markActive();
    g_runtime.globalPreloaded = g_runtime.selectionPool.hasPreloaded();
    g_runtime.chain.clearFault();
    if (!selection.empty()) {
        g_runtime.chain.activate(target);
        g_runtime.retainedSelectionSlot = static_cast<int>(target);
    } else {
        g_runtime.chain.deactivate();
        // Keep the last prepared slot available for a future re-enable while
        // bypassing it immediately. The empty selection is the explicit
        // direct-bypass state and exposes no active processor instance.
        g_runtime.chain.setBypassed(true);
    }
    g_runtime.chain.setBypassed(selection.empty());
    g_runtime.effects[0].shutdown();
    g_runtime.effects[1].shutdown();
    refreshInputParameterMirrors();
    return true;
}

void reconfigureInputRouterIfNeeded() noexcept {
    if (!g_runtime.inputRouter.snapshot().enabled) return;
    const auto observedRate = g_runtime.inputObservedRate.load(std::memory_order_acquire);
    const auto observedInput = g_runtime.inputObservedChannels.load(std::memory_order_acquire);
    const auto observedOutput = g_runtime.inputObservedOutputChannels.load(std::memory_order_acquire);
    if (observedRate <= 0 || observedInput == 0 || observedOutput == 0 ||
        observedRate == g_runtime.inputConfiguredRate.load(std::memory_order_acquire))
        return;
    g_runtime.inputRouter.setEnabled(false);
    lockInputProcessing();
    const InputProcessingGuard release;
    const auto active = g_runtime.inputChain.snapshot().activeSlot;
    bool configured = active >= 0;
    g_runtime.inputChain.setBypassed(true);
    if (configured) {
        auto &slot = g_runtime.inputSelectionSlots[active];
        for (std::size_t index = 0; index < slot.count; ++index)
            configured = slot.effects[index] &&
                slot.effects[index]->reconfigure(static_cast<double>(observedRate), 16384) && configured;
    }
    if (configured) {
        g_runtime.inputConfiguredRate.store(observedRate, std::memory_order_release);
        g_runtime.inputConfiguredChannels.store(observedInput, std::memory_order_release);
        g_runtime.inputConfiguredOutputChannels.store(observedOutput, std::memory_order_release);
        g_runtime.inputConfigurationPending.store(false, std::memory_order_release);
        g_runtime.inputChain.clearFault();
        g_runtime.inputChain.setBypassed(false);
    } else {
        g_runtime.inputConfigurationPending.store(true, std::memory_order_release);
        g_runtime.inputConfigurationErrors.fetch_add(1, std::memory_order_relaxed);
    }
    g_runtime.inputRouter.setEnabled(configured);
}

void shutdown() noexcept {
    g_qtDispatchStopping.store(true, std::memory_order_release);
    g_trackTopologyInvalidated.store(false, std::memory_order_release);
    stopSelectionWorker();
    g_inputMonitor.exchange.off();
    for (int index = 0; index < 2; ++index) {
        while (!g_inputMonitor.exchange.writable(index)) std::this_thread::yield();
        g_inputMonitor.monitorSlots[index].selection.shutdown();
        g_inputMonitor.monitorSlots[index].pool.effects.clear();
    }
    g_inputMonitor.warmEffects.clear();
    g_inputMonitor.retained = -1;
    g_inputMonitor.nativeSuppressed.store(false);
    std::lock_guard<std::recursive_mutex> editorLock(g_runtime.editorMutex);
    const auto openEditor = std::atomic_exchange(&g_openEditorEffect, std::shared_ptr<RuntimeEffect>{});
    if (openEditor) {
        EditorCallbackScope callbackScope;
        openEditor->closeEditor();
    }
    const TrackDispatchUpdate trackUpdate;
    state::clearRuntimeTrackContext();
    g_runtime.chain.setBypassed(true);
    g_runtime.chain.deactivate();
    g_runtime.inputRouter.setEnabled(false);
    g_runtime.inputRouter.setRoute(input::Route::Disabled);
    lockInputProcessing();
    g_runtime.inputChain.setBypassed(true);
    g_runtime.inputChain.deactivate();
    g_runtime.inputSelectionSlots[0].shutdown();
    g_runtime.inputSelectionSlots[1].shutdown();
    g_runtime.inputSelectionPool.effects.clear();
    g_runtime.retainedInputSelectionSlot = -1;
    g_runtime.inputRouter.setProcessor({});
    unlockInputProcessing();
    g_runtime.outputEvidenceClaimed.store(false, std::memory_order_release);
    g_runtime.outputBeforeHash.store(0, std::memory_order_relaxed);
    g_runtime.outputAfterHash.store(0, std::memory_order_relaxed);
    g_runtime.outputWriteObserved.store(false, std::memory_order_release);
    g_runtime.outputObserved.store(false, std::memory_order_release);
    g_runtime.outputCalls.store(0, std::memory_order_relaxed);
    g_runtime.outputFrames.store(0, std::memory_order_relaxed);
    g_runtime.outputThread.store(0, std::memory_order_relaxed);
    g_runtime.outputFirstBuffer.store(0, std::memory_order_relaxed);
    g_runtime.outputLastBuffer.store(0, std::memory_order_relaxed);
    remove(g_runtime.stream);
    removeListenerProbe();
    g_listenerProbeInstalled = false;
    remove(g_runtime.dsp);
    remove(g_runtime.master);
    remove(g_runtime.cursorMove);
    remove(g_runtime.cursorTrack);
    remove(g_runtime.scoreDuplicateTrack);
    remove(g_runtime.scoreCreateTrack);
    remove(g_runtime.scoreSwapTracks);
    g_runtime.effects[0].shutdown();
    g_runtime.effects[1].shutdown();
    g_runtime.selectionSlots[0].shutdown();
    g_runtime.selectionSlots[1].shutdown();
    g_runtime.selectionPool.effects.clear();
    {
        std::lock_guard<std::mutex> lock(g_runtime.selectionMutex);
        g_runtime.requestedSelection.clear();
        g_runtime.requestedTrackSelections.clear();
        g_runtime.publishedBindings.clear();
        g_runtime.currentTrackKey.clear();
    }
    {
        std::lock_guard<std::mutex> lock(g_runtime.selectionRequestMutex);
        g_runtime.pendingTrackSelections.clear();
    }
    g_runtime.trackContextObserved.store(false, std::memory_order_release);
    g_runtime.trackContextStable.store(false, std::memory_order_release);
    g_runtime.trackScopeUnresolved.store(true, std::memory_order_release);
    g_runtime.effectsChainIndexObserved.store(false, std::memory_order_release);
    g_runtime.observedEffectsChainIndex.store(-1, std::memory_order_relaxed);
    g_runtime.effectsChainContextCount.store(0, std::memory_order_relaxed);
    for (auto &observation : g_runtime.effectsChainObservations) {
        observation.index.store(-1, std::memory_order_relaxed);
        observation.self.store(nullptr, std::memory_order_relaxed);
    }
    g_runtime.trackChainProcessBlocks.store(0, std::memory_order_relaxed);
    g_runtime.trackChainProcessedBlocks.store(0, std::memory_order_relaxed);
    g_runtime.trackDispatchMisses.store(0, std::memory_order_relaxed);
    g_runtime.trackLastDspSelf.store(0, std::memory_order_relaxed);
    g_runtime.trackDispatchSelf0.store(0, std::memory_order_relaxed);
    g_runtime.trackDispatchSelf1.store(0, std::memory_order_relaxed);
    g_runtime.trackChainBypassBlocks.store(0, std::memory_order_relaxed);
    g_runtime.trackChainErrorBlocks.store(0, std::memory_order_relaxed);
    g_runtime.trackBindingsPublished.store(0, std::memory_order_release);
    g_runtime.trackRuntimeProcessed.store(false, std::memory_order_release);
    g_runtime.trackRuntimeWriteObserved.store(false, std::memory_order_release);
    g_runtime.lastTrackRuntimeIndex.store(-1, std::memory_order_release);
    g_runtime.trackRuntimeError.clear();
    for (auto &dispatch : g_runtime.trackDispatch) {
        dispatch.self.store(nullptr, std::memory_order_release);
        dispatch.runtime.store(nullptr, std::memory_order_release);
    }
    for (auto &runtime : g_runtime.trackRuntimes) {
        runtime.shutdown();
        runtime.trackKey.clear();
        runtime.trackId.clear();
        runtime.requested.clear();
    }
    g_runtime.globalChainProcessBlocks.store(0, std::memory_order_relaxed);
    g_runtime.selectionMode.store(false, std::memory_order_release);
    g_runtime.selectionPublished.store(false, std::memory_order_release);
    g_runtime.globalPreloaded = false;
    g_runtime.inputPreloaded = false;
    g_runtime.retainedSelectionSlot = -1;
    g_runtime.preloadBusy.store(false, std::memory_order_release);
    g_runtime.preloadAttempts.clear();
    g_runtime.pendingSelection.clear();
    g_runtime.appliedSelection.clear();
    g_runtime.projectGlobalRestored = false;
}

std::uint64_t outputHash(const void *output, unsigned long frames) noexcept {
    if (!output || frames == 0) return 0;
    // The PortAudio callback ABI does not expose channel count here. Hash only
    // one frame-sized span, which is valid for both mono and interleaved output
    // without assuming a stereo layout.
    const auto samples = (std::min)(static_cast<std::size_t>(frames),
                                    static_cast<std::size_t>(8192));
    const auto *values = static_cast<const float *>(output);
    std::uint64_t hash = 1469598103934665603ULL;
    for (std::size_t index = 0; index < samples; ++index) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, values + index, sizeof(bits));
        hash ^= bits;
        hash *= 1099511628211ULL;
    }
    return hash;
}

bool readDrainProbeSnapshot(void *owner, unsigned long frames,
                            input::drainprobe::Snapshot &snapshot) noexcept {
    const auto identity = asioprobe::currentCallback();
    portaudio::Configuration config;
    if (!identity.rateValidated || !input::drain::supportedDestinationRate(identity.actualRate) || !owner ||
        frames == 0 || frames > portaudio::kMaxFrames ||
        !portaudio::configuration(g_runtime.audioModule, owner, config) ||
        config.sampleRate != 44100 || config.outputChannels != 2) return false;
    const auto *stream = portaudio::read<const void *>(owner, 8);
    const auto *base = static_cast<const std::uint8_t *>(g_runtime.audioModule);
    if (!stream || stream != portaudio::read<const void *>(base, 0x2F2620) ||
        portaudio::read<const void *>(stream, 0x28) != owner ||
        portaudio::read<std::uint32_t>(stream, 0x178) < frames ||
        portaudio::read<std::uint32_t>(stream, 0x178) > 8192) return false;
    const auto *streamInterface = portaudio::read<const void *>(stream, 0x10);
    if (!streamInterface || portaudio::read<const void *>(streamInterface, 0) != base + 0x6DF60 ||
        portaudio::read<const void *>(streamInterface, 8) != base + 0x6FE90 ||
        portaudio::read<const void *>(streamInterface, 16) != base + 0x700A0) return false;
    return input::drainprobe::readSnapshot(base, owner,
        {identity.generation, identity.generation, identity.rateRevision,
         identity.actualRate, identity.rateValidated}, snapshot);
}

#ifdef GPVST3_P13_PROBE_BUILD
void observeDrainSource(const float *input, const void *format, std::int64_t frames) noexcept {
    auto *r = g_currentDrainRecord;
    if (!r) return;
    ++r->sourceObserverCalls;
    const auto *base = static_cast<const std::uint8_t *>(g_runtime.audioModule);
    const auto *parent = portaudio::read<const void *>(r->before.owner, 0);
    r->parentGain = parent ? portaudio::read<float>(parent, 0x5C) : 0;
    if (!input || format != base + 0x2507E8 || frames <= 0 || frames > 32768 ||
        r->rseCalls == 0 || r->rseRequestedFrames != frames || !parent ||
        portaudio::read<const void *>(parent, 0) != base + 0x18F7D8 || r->parentGain != 1.0f ||
        portaudio::read<std::uint8_t>(parent, 0x60) != 0) { r->rseValid = false; return; }
    for (std::size_t i = 0; i < std::size_t(frames) * 2; ++i) {
        ++r->rseComparedSamples;
        if (!std::isfinite(input[i]) || !std::isfinite(g_drainRseSum[i])) { r->rseValid = false; continue; }
        if (std::memcmp(input + i, g_drainRseSum.data() + i, sizeof(float)) != 0) ++r->rseMismatchSamples;
        r->rseEnergy += double(g_drainRseSum[i]) * g_drainRseSum[i];
    }
}

std::int64_t rseProbeHook(void *self, const float *input, std::uint32_t inputChannels,
    float *output, std::uint32_t outputChannels, std::int64_t frames, const void *timePoint) {
    using Fill = std::int64_t (*)(void *, const float *, std::uint32_t, float *,
        std::uint32_t, std::int64_t, const void *);
    const auto original = reinterpret_cast<Fill>(g_runtime.rseProbe.trampoline);
    const auto result = original(self, input, inputChannels, output, outputChannels, frames, timePoint);
    auto *r = g_currentDrainRecord;
    if (!r) return result;
    ++r->rseCalls;
    if (!self || !output || inputChannels != 2 || outputChannels != 2 || frames <= 0 || frames > 32768 ||
        result < 0 || result > frames || portaudio::read<const void *>(self, 0) != g_rseProbeBase + 0x23AD98) {
        r->rseValid = false;
        return result;
    }
    if (r->rseCalls == 1) {
        r->rseRequestedFrames = frames;
        std::fill_n(g_drainRseSum.data(), std::size_t(frames) * 2, 0.0f);
    } else if (r->rseRequestedFrames != frames) { r->rseValid = false; return result; }
    for (std::size_t i = 0; i < std::size_t(result) * 2; ++i) {
        if (!std::isfinite(output[i])) r->rseValid = false;
        g_drainRseSum[i] += output[i];
    }
    return result;
}

class DrainProbeScope final {
public:
    DrainProbeScope(void *owner, unsigned long frames, std::uint64_t sequence, unsigned long status) noexcept
        : owner_(owner), previous_(g_currentDrainRecord) {
        if ((!g_drainProbeRequested && !g_overlayProbeRequested) || g_drainProbeCount.load(std::memory_order_acquire) >= kDrainProbeCapacity ||
            steadyNanoseconds() < g_inputProbeStart) return;
        // Start on the first actually suppressed production callback. This
        // observer never requests a mode change or changes the native route.
        if (g_overlayTransitionProbe && !g_inputMonitor.nativeSuppressed.load(std::memory_order_acquire)) return;
        if (g_drainProbeBusy.test_and_set(std::memory_order_acquire)) {
            g_drainProbeOverlaps.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        held_ = true;
        index_ = g_drainProbeCount.load(std::memory_order_relaxed);
        if (index_ >= kDrainProbeCapacity) return;
        record_ = &g_drainProbeRecords[index_].record;
        record_->sequence = sequence;
        record_->timestamp = steadyNanoseconds();
        record_->frames = static_cast<std::uint32_t>(frames);
        record_->thread = GetCurrentThreadId();
        record_->statusFlags = status;
        record_->valid = g_rseProbeInstalled.load(std::memory_order_acquire) &&
            readDrainProbeSnapshot(owner_, frames, record_->before);
        if (record_->valid && !g_overlayProbeRequested) {
            record_->srcObserved = asioprobe::beginOutputSrc(
                record_->before.outputSrc, &observeDrainSource);
            record_->valid = record_->srcObserved;
        }
        if (record_->valid) g_currentDrainRecord = record_;
    }
    ~DrainProbeScope() {
        if (record_ && record_->srcObserved) asioprobe::finishOutputSrc();
        g_currentDrainRecord = previous_;
        if (held_) g_drainProbeBusy.clear(std::memory_order_release);
    }
    void finish(int result, const float *output) noexcept {
        if (!record_) return;
        auto &r = *record_;
        r.originalResult = result;
        if (r.srcObserved) r.src = asioprobe::finishOutputSrc();
        r.srcObserved = false;
        const bool afterValid = readDrainProbeSnapshot(owner_, r.frames, r.after);
        r.valid = r.valid && afterValid && result == 0 && output && r.statusFlags == 0 &&
            input::drainprobe::sameTopology(r.before, r.after) &&
            input::drainprobe::consumedSamples(r.before, r.after, r.frames, r.consumed) &&
            r.src.calls <= 1 && r.src.inputFrames >= 0 && r.src.outputFrames >= 0 &&
            r.listenerValid && r.listenerCalls == r.listenerSunk &&
            // This binary calls the listener once before each output SRC.
            r.listenerCalls == r.src.calls && g_drainProbeOverlaps.load(std::memory_order_relaxed) == 0;
        if (r.valid && output) {
            for (std::size_t i = 0; i < std::size_t(r.frames) * 2; ++i) {
                if (std::isfinite(output[i])) r.outputEnergy += double(output[i]) * output[i];
                else ++r.nonFiniteSamples;
            }
        }
        r.valid = r.valid && r.nonFiniteSamples == 0 && r.callerChangedSamples == 0 &&
            r.rseValid && r.rseMismatchSamples == 0 && r.sourceObserverCalls == r.src.calls;
        if (g_overlayProbeRequested) {
            if (g_overlayTransitionProbe && r.valid && !r.overlayCommitted && output &&
                r.frames > 0 && r.frames <= portaudio::kMaxFrames) {
                for (std::size_t i = 0; i < std::size_t(r.frames) * 2; ++i) {
                    ++r.overlayComparedSamples;
                    if (std::memcmp(output + i, g_overlayProbeBefore.data() + i, sizeof(float))) ++r.overlayMismatchSamples;
                }
            }
            r.valid = r.valid && r.overlaySuppressed &&
                (r.overlayCommitted || (g_overlayTransitionProbe && r.productionDrain.state == input::drain::State::Preparing)) &&
                r.overlayComparedSamples == r.frames * 2 && r.overlayMismatchSamples == 0;
        }
        if (index_ == 0) {
            if (!r.valid || !g_drainTracker.configure(r.before.config)) g_drainProbeInvalid = true;
        } else {
            const auto &first = g_drainProbeRecords[0].record;
            if (r.thread != first.thread || !input::drainprobe::sameTopology(first.before, r.before)) r.valid = false;
        }
        if (!r.valid) g_drainProbeInvalid = true;
        // Retrospective observation only. This experiment never publishes an
        // overlay; Ready records the first eligible future callback boundary.
        if (!g_drainProbeInvalid) {
            bool okay = g_drainTracker.beginCallback(r.before.config, r.sequence,
                r.frames, r.before.ring.queued, true);
            if (okay && r.src.calls) okay = g_drainTracker.srcCompleted(
                static_cast<std::uint64_t>(r.src.inputFrames), static_cast<std::uint64_t>(r.src.outputFrames));
            if (okay) okay = g_drainTracker.endCallback(r.after.config, r.after.ring.queued, r.consumed);
            if (!okay) g_drainProbeInvalid = true;
        }
        r.drain = g_drainTracker.snapshot();
        if (g_overlayTransitionProbe)
            r.valid = r.valid && r.productionDrain.state == r.drain.state &&
                r.productionDrain.phase == r.drain.phase &&
                r.overlayCommitted == (r.drain.state == input::drain::State::Ready);
        r.valid = r.valid && !g_drainProbeInvalid;
        r.ended = steadyNanoseconds();
        g_currentDrainRecord = previous_;
        g_drainProbeRecords[index_].published.store(true, std::memory_order_release);
        g_drainProbeCount.store(index_ + 1, std::memory_order_release);
        record_ = nullptr;
    }
private:
    void *owner_ = nullptr;
    DrainProbeRecord *previous_ = nullptr, *record_ = nullptr;
    std::size_t index_ = 0;
    bool held_ = false;
};

#endif

struct MonitorAudioState {
    input::drain::Tracker tracker;
    input::drainprobe::Snapshot initial;
    std::uint64_t token = 0, slotToken = 0;
    std::uint64_t requestedGeneration = 0, requestedRevision = 0;
    std::uint64_t suppressedGeneration = 0, suppressedRevision = 0;
    bool configured = false, faulted = false;
    std::array<float, portaudio::kMaxFrames * 2> capture{};
    std::array<float, 32768 * 2> sink{};
} g_monitorAudio;

bool finishMonitorOutput(float *samples, std::size_t count) noexcept {
    bool clipped = false;
    for (std::size_t i = 0; i < count; ++i) {
        // AMAudio applies this final range before returning. The overlay is
        // added afterwards, so restore the same device-output contract. The
        // router already rejected non-finite sums before any output write.
        if (samples[i] > 1.0f) { samples[i] = 1.0f; clipped = true; }
        else if (samples[i] < -1.0f) { samples[i] = -1.0f; clipped = true; }
    }
    return clipped;
}

class MonitorCallback final {
public:
    MonitorCallback(const void *capture, void *output, unsigned long frames,
                    void *owner, std::uint64_t sequence, unsigned long status) noexcept
        : output_(output), owner_(owner), frames_(frames), sequence_(sequence) {
        g_inputMonitor.exchange.acquire(lease_);
        if (!g_inputMonitor.nativeListenerKnown.load(std::memory_order_acquire) ||
            !g_inputMonitor.nativeListenerEnabled.load(std::memory_order_acquire)) {
            // The saved mode is a preference, never permission to open input.
            // No borrowed capture, listener sink or VST3 processor is touched.
            held_ = !g_inputMonitor.processing.test_and_set(std::memory_order_acquire);
            if (held_) {
                g_monitorAudio.configured = false;
                g_monitorAudio.slotToken = 0;
                g_inputMonitor.nativeSuppressed.store(false, std::memory_order_release);
                g_inputMonitor.configurationToken.store(0, std::memory_order_release);
            }
            return;
        }
        requested_ = lease_.suppressNative();
        if (!requested_ && !lease_.watchStream()) return;
        held_ = !g_inputMonitor.processing.test_and_set(std::memory_order_acquire);
        suppress_ = requested_ && g_inputMonitor.nativeSuppressed.load(std::memory_order_acquire);
        if (!held_) {
            if (requested_) g_inputMonitor.errors.fetch_add(1);
            return;
        }
        const auto identity = asioprobe::currentCallback();
        // A suppression decision belongs to one validated native stream.
        // An unwrapped/new/invalidated stream may have different objects and
        // topology; it must not inherit the previous stream's listener sink.
        suppress_ = suppress_ && identity.rateValidated &&
            identity.generation == g_monitorAudio.suppressedGeneration &&
            identity.rateRevision == g_monitorAudio.suppressedRevision;
        g_inputMonitor.nativeSuppressed.store(suppress_, std::memory_order_release);
        if (lease_.index() >= 0) slot_ = &g_inputMonitor.monitorSlots[lease_.index()];
        const bool changedStream = slot_ ? (identity.generation != slot_->generation ||
            identity.rateRevision != slot_->revision || identity.actualRate != slot_->rate) :
            (identity.generation != g_monitorAudio.suppressedGeneration ||
             identity.rateRevision != g_monitorAudio.suppressedRevision);
        if (identity.rateValidated && changedStream &&
            (identity.generation != g_monitorAudio.requestedGeneration ||
             identity.rateRevision != g_monitorAudio.requestedRevision)) {
            g_monitorAudio.requestedGeneration = identity.generation;
            g_monitorAudio.requestedRevision = identity.rateRevision;
            g_inputMonitor.reconfigureRequested.store(true);
            g_inputMonitor.statusChanged.store(true);
            if (const auto handle = g_inputMonitor.event.load()) SetEvent(handle);
        }
        if (!slot_) return;
        portaudio::Configuration captureConfig;
        configurationRejected_ = !(capture && output && frames > 0 && frames <= portaudio::kMaxFrames &&
            identity.rateValidated && identity.generation == slot_->generation &&
            identity.rateRevision == slot_->revision && identity.actualRate == slot_->rate &&
            portaudio::configuration(g_runtime.audioModule, owner, captureConfig) &&
            captureConfig.inputChannels >= 1 && captureConfig.inputChannels <= 2 &&
            readDrainProbeSnapshot(owner, frames, before_));
        // PortAudio status flags describe a preceding over/underrun. The
        // current callback still owns valid buffers; don't latch permanent
        // monitor silence on one scheduling hiccup.
        if (status) g_inputMonitor.statusBlocks.fetch_add(1, std::memory_order_relaxed);
        valid_ = !configurationRejected_;
        if (!valid_) return;
        inputChannels_ = captureConfig.inputChannels;
        before_.config.epoch = g_monitorAudio.token;
        for (std::size_t i = 0; i < std::size_t(frames) * inputChannels_; ++i) {
            const auto sample = static_cast<const float *>(capture)[i];
            if (!std::isfinite(sample)) { valid_ = false; return; }
            g_monitorAudio.capture[i] = sample;
        }
        if (g_monitorAudio.slotToken != lease_.token()) {
            const bool continuous = suppress_ && g_monitorAudio.configured && !g_monitorAudio.faulted &&
                input::drainprobe::sameTopology(g_monitorAudio.initial, before_);
            g_monitorAudio.slotToken = lease_.token();
            if (!continuous) {
                g_monitorAudio.tracker.~Tracker();
                new (&g_monitorAudio.tracker) input::drain::Tracker;
                g_inputMonitor.callbackFault.store(0);
                g_inputMonitor.listenerFaultFlags.store(0);
                g_monitorAudio.token = lease_.token();
                before_.config.epoch = g_monitorAudio.token;
                g_monitorAudio.initial = before_;
                g_monitorAudio.configured = g_monitorAudio.tracker.configure(before_.config);
                g_monitorAudio.faulted = !g_monitorAudio.configured;
            }
        }
        valid_ = g_monitorAudio.configured && !g_monitorAudio.faulted &&
            input::drainprobe::sameTopology(g_monitorAudio.initial, before_) &&
            g_monitorAudio.tracker.beginCallback(before_.config, sequence, frames, before_.ring.queued, true);
        if (!valid_) return;
        failureStage_ = 2;
        observing_ = before_.outputSrc && asioprobe::beginOutputSrc(before_.outputSrc
#ifdef GPVST3_P13_PROBE_BUILD
            , g_overlayProbeRequested ? &observeDrainSource : nullptr
#endif
        );
        valid_ = observing_ || !before_.config.usesOutputRing;
        if (valid_) {
            failureStage_ = 3;
            suppress_ = true;
            g_monitorAudio.suppressedGeneration = identity.generation;
            g_monitorAudio.suppressedRevision = identity.rateRevision;
            g_inputMonitor.nativeSuppressed.store(true, std::memory_order_release);
            g_inputMonitor.actualRate.store(static_cast<int>(slot_->rate));
            g_inputMonitor.frames.store(frames);
            g_inputMonitor.driverFrames.store(portaudio::read<std::uint32_t>(before_.stream, 0x178));
            g_inputMonitor.channels.store(inputChannels_);
            g_inputMonitor.configurationToken.store(lease_.token(), std::memory_order_release);
        }
    }
    ~MonitorCallback() {
        if (observing_) asioprobe::finishOutputSrc();
        if (held_) g_inputMonitor.processing.clear(std::memory_order_release);
    }
    bool suppress() const noexcept { return suppress_; }
    bool requested() const noexcept { return requested_; }
    bool legacy() const noexcept {
        return lease_.legacy() && g_inputMonitor.nativeListenerKnown.load(std::memory_order_acquire) &&
            g_inputMonitor.nativeListenerEnabled.load(std::memory_order_acquire);
    }
    bool ownsSink() const noexcept { return held_; }
    void listener(bool valid) noexcept { ++listenerCalls_; listenerValid_ &= valid; }
    void finish(int result) noexcept {
        if (!g_inputMonitor.nativeListenerKnown.load(std::memory_order_acquire) ||
            !g_inputMonitor.nativeListenerEnabled.load(std::memory_order_acquire)) {
            if (held_) {
                g_monitorAudio.configured = false;
                g_monitorAudio.slotToken = 0;
                g_inputMonitor.nativeSuppressed.store(false, std::memory_order_release);
                g_inputMonitor.configurationToken.store(0, std::memory_order_release);
            }
            return;
        }
        if (!requested_) {
            if (g_inputMonitor.exchange.current(lease_.token()))
                g_inputMonitor.nativeSuppressed.store(false, std::memory_order_release);
            return;
        }
        if (!held_) return;
        if (!slot_) {
            // A failed preparation after prior activation can retain a muted
            // exchange without a slot. Its unobserved drain history cannot
            // be reused on resume; an empty chain has its own dry slot.
            g_monitorAudio.configured = false;
            g_inputMonitor.publishCallbackPhase(lease_.token(), suppress_ ? 5 : 6);
            return;
        }
        asioprobe::SrcObservation src;
        if (observing_) { src = asioprobe::finishOutputSrc(); observing_ = false; }
#ifdef GPVST3_P13_PROBE_BUILD
        if (g_overlayProbeRequested && g_currentDrainRecord) {
            g_currentDrainRecord->src = src;
            g_currentDrainRecord->monitorToken = lease_.token();
            g_currentDrainRecord->overlaySuppressed = suppress_ && held_;
        }
#endif
        observedSrcCalls_ = src.calls;
        input::drainprobe::Snapshot after;
        std::uint64_t consumed = 0;
        if (!valid_) { fail(); return; }
        if (result != 0) { failureStage_ = 4; fail(); return; }
        if (!listenerValid_) { failureStage_ = 5; fail(); return; }
        if (listenerCalls_ != (before_.config.usesOutputRing ? src.calls : 1)) {
            failureStage_ = 12; fail(); return;
        }
        if (src.calls > 1 || src.inputFrames < 0 || src.outputFrames < 0) { failureStage_ = 6; fail(); return; }
        if (!readDrainProbeSnapshot(owner_, frames_, after)) {
            configurationRejected_ = true;
            failureStage_ = 7; fail(); return;
        }
        after.config.epoch = g_monitorAudio.token;
        if (!input::drainprobe::sameTopology(before_, after) ||
            !input::drainprobe::consumedSamples(before_, after, frames_, consumed)) {
            configurationRejected_ = true;
            failureStage_ = 8; fail(); return;
        }
        if (src.calls && !g_monitorAudio.tracker.srcCompleted(
                static_cast<std::uint64_t>(src.inputFrames), static_cast<std::uint64_t>(src.outputFrames))) {
            failureStage_ = 9; fail(); return;
        }
        if (!g_monitorAudio.tracker.endCallback(after.config, after.ring.queued, consumed)) {
            failureStage_ = 10; fail(); return;
        }
#ifdef GPVST3_P13_PROBE_BUILD
        if (g_overlayProbeRequested && g_currentDrainRecord)
            g_currentDrainRecord->productionDrain = g_monitorAudio.tracker.snapshot();
#endif
        if (g_monitorAudio.tracker.snapshot().state != input::drain::State::Ready) {
            g_inputMonitor.publishCallbackPhase(lease_.token(), 3);
            return;
        }
#ifdef GPVST3_P13_PROBE_BUILD
        const auto inputProcessStarted = g_inputTimingTicket ? steadyNanoseconds() : 0;
#endif
        const auto processed = slot_->router.processInterleaved({g_monitorAudio.capture.data(), output_,
            std::size_t(frames_), inputChannels_, 2, slot_->rate, std::size_t(frames_), owner_, sequence_});
#ifdef GPVST3_P13_PROBE_BUILD
        if (g_inputTimingTicket) input::timingprobe::Recorder::inputProcessed(*g_inputTimingTicket,
            inputProcessStarted, steadyNanoseconds(), processed.processed && !processed.error);
#endif
        if (!processed.processed || processed.error) { failureStage_ = 11; fail(); return; }
#ifdef GPVST3_P13_PROBE_BUILD
        if (g_overlayProbeRequested && g_currentDrainRecord) {
            auto &r = *g_currentDrainRecord;
            r.overlayCommitted = true;
            // This diagnostic is explicitly paired with the unity-gain fixture.
            // Independent expected samples detect RSE entering the input chain
            // and any overwrite, duplicate input or post-mix limiting.
            for (std::size_t f = 0; f < frames_; ++f) for (std::size_t c = 0; c < 2; ++c) {
                const auto i = f * 2 + c;
                const float contribution = g_monitorAudio.capture[f * inputChannels_ + (inputChannels_ == 1 ? 0 : c)] * slot_->gain;
                const float expected = g_overlayProbeBefore[i] + contribution;
                const auto *actual = static_cast<const float *>(output_) + i;
                ++r.overlayComparedSamples;
                if (std::memcmp(&expected, actual, sizeof(float)) != 0) ++r.overlayMismatchSamples;
                if (!std::isfinite(expected) || !std::isfinite(*actual)) ++r.nonFiniteSamples;
                r.preOverlayEnergy += double(g_overlayProbeBefore[i]) * g_overlayProbeBefore[i];
                r.overlayEnergy += double(contribution) * contribution;
            }
        }
#endif
        const bool clipped = finishMonitorOutput(static_cast<float *>(output_), std::size_t(frames_) * 2);
        if (clipped) g_inputMonitor.clipped.fetch_add(1);
        g_inputMonitor.blocks.fetch_add(1);
        g_inputMonitor.processFrames.store(frames_);
        g_inputMonitor.publishCallbackPhase(lease_.token(), 4);
    }
private:
    void fail() noexcept {
        g_monitorAudio.faulted = true;
        if (configurationRejected_) {
            g_inputMonitor.configurationToken.store(0, std::memory_order_release);
            g_inputMonitor.processFrames.store(0);
            g_inputMonitor.configurationRejectedBlocks.fetch_add(1);
            g_inputMonitor.publishCallbackPhase(lease_.token(), suppress_ ? 5 : 6);
            return;
        }
        int noFault = 0;
        if (g_inputMonitor.callbackFault.compare_exchange_strong(noFault, failureStage_)) {
            g_inputMonitor.faultListenerCalls.store(listenerCalls_);
            g_inputMonitor.faultSrcCalls.store(observedSrcCalls_);
        }
        g_inputMonitor.errors.fetch_add(1);
        g_inputMonitor.publishCallbackPhase(lease_.token(), suppress_ ? 5 : 6);
    }
    input::MonitorExchange::Lease lease_;
    InputMonitorSlot *slot_ = nullptr;
    input::drainprobe::Snapshot before_;
    void *output_, *owner_;
    unsigned long frames_;
    std::uint64_t sequence_;
    std::uint32_t listenerCalls_ = 0, observedSrcCalls_ = 0;
    std::size_t inputChannels_ = 0;
    bool requested_ = false, held_ = false, suppress_ = false, valid_ = false;
    bool observing_ = false, listenerValid_ = true, configurationRejected_ = false;
    int failureStage_ = 1;
};
thread_local MonitorCallback *g_monitorCallback = nullptr;

std::int64_t listenerProbeHook(void *self, const float *input, std::uint32_t inputChannels,
    float *output, std::uint32_t outputChannels, std::int64_t frames, const void *timePoint) {
    using FillBuffer = std::int64_t (*)(void *, const float *, std::uint32_t, float *,
        std::uint32_t, std::int64_t, const void *);
    const auto original = reinterpret_cast<FillBuffer>(g_runtime.listenerProbe.trampoline);
    if (g_monitorCallback && g_monitorCallback->suppress()) {
        // The native input SRC initially produces zero frames while filling
        // its history. Forward that legal call once into the sink; zero frames
        // must neither fault the monitor nor advance its drain counters.
        const bool valid = g_monitorCallback->ownsSink() && self &&
            frames >= 0 && frames <= 32768 && (frames == 0 || (input && output)) &&
            inputChannels == 2 && outputChannels == 2 &&
            portaudio::read<const void *>(self, 0) == g_listenerExecutableBase + 0x25ABAF8;
        if (!valid) {
            g_inputMonitor.listenerRequestedFrames.store(frames);
            g_inputMonitor.listenerFaultFlags.store((!g_monitorCallback->ownsSink() ? 1 : 0) |
                (!self ? 2 : 0) | (!input ? 4 : 0) | (!output ? 8 : 0) |
                (frames < 0 || frames > 32768 ? 16 : 0) | (inputChannels != 2 ? 32 : 0) |
                (outputChannels != 2 ? 64 : 0) |
                (self && portaudio::read<const void *>(self, 0) != g_listenerExecutableBase + 0x25ABAF8 ? 128 : 0));
            g_monitorCallback->listener(false); return 0;
        }
#ifdef GPVST3_P13_PROBE_BUILD
        auto *record = g_overlayProbeRequested ? g_currentDrainRecord : nullptr;
        if (record && frames > 0) {
            ++record->listenerCalls;
            std::copy_n(output, std::size_t(frames) * 2, g_listenerCallerBefore.data());
        }
#endif
        std::fill_n(g_monitorAudio.sink.data(), std::size_t(frames) * 2, 0.0f);
        const auto result = original(self, input, inputChannels, g_monitorAudio.sink.data(),
            outputChannels, frames, timePoint);
        if (result != frames) {
            g_inputMonitor.listenerReturnedFrames.store(result);
            g_inputMonitor.listenerRequestedFrames.store(frames);
            g_inputMonitor.listenerFaultFlags.store(256);
        }
        g_monitorCallback->listener(result == frames);
#ifdef GPVST3_P13_PROBE_BUILD
        if (record && frames > 0) {
            const auto *state = portaudio::read<const void *>(self, 8);
            if (result == frames && state && portaudio::read<std::uint8_t>(state, 0x50) != 0)
                ++record->listenerSunk;
            else record->listenerValid = false;
            if (state) {
                record->nativePreGain = portaudio::read<float>(state, 0x58);
                const auto inputPeak = portaudio::read<float>(state, 0x198);
                const auto outputPeak = portaudio::read<float>(state, 0x19C);
                if (std::isfinite(inputPeak) && std::isfinite(outputPeak)) {
                    ++record->listenerPeakSamples;
                    record->inputPeak = inputPeak; record->outputPeak = outputPeak;
                } else ++record->nonFiniteSamples;
            }
            for (std::size_t i = 0; i < std::size_t(frames) * 2; ++i) {
                if (std::memcmp(output + i, g_listenerCallerBefore.data() + i, sizeof(float)) != 0)
                    ++record->callerChangedSamples;
                if (std::isfinite(input[i]) && std::isfinite(g_monitorAudio.sink[i]) && std::isfinite(output[i])) {
                    record->captureEnergy += double(input[i]) * input[i];
                    record->nativeEnergy += double(g_monitorAudio.sink[i]) * g_monitorAudio.sink[i];
                    record->callerEnergy += double(output[i]) * output[i];
                } else ++record->nonFiniteSamples;
            }
        }
#endif
        return result;
    }
#ifdef GPVST3_P13_PROBE_BUILD
    auto *drainRecord = g_currentDrainRecord;
    if (drainRecord) ++drainRecord->listenerCalls;
    if (!drainRecord && (!g_listenerProbe.enabled() || g_listenerProbe.claimed() >= input::probe::kCapacity))
        return original(self, input, inputChannels, output, outputChannels, frames, timePoint);
    input::probe::Recorder::Ticket ticket;
    const auto started = steadyNanoseconds();
    // The external callback establishes caller ownership. Independent unit
    // invocations are forwarded unchanged and never assumed to own scratch.
    const bool valid = g_probeOuterSequence != 0 && self && frames > 0 && frames <= 32768 &&
        inputChannels >= 1 && inputChannels <= 2 && outputChannels >= 1 && outputChannels <= 2 &&
        portaudio::read<const void *>(self, 0) == g_listenerExecutableBase + 0x25ABAF8;
    if (valid && started >= g_inputProbeStart) {
        input::probe::Metadata m;
        m.sequence = g_listenerProbeSequence.fetch_add(1, std::memory_order_relaxed) + 1;
        m.parentSequence = g_probeOuterSequence;
        m.timestampNanoseconds = started;
        m.frames = static_cast<std::size_t>(frames);
        m.sampleRate = 44100; // Verified GP internal SRC destination; not hardware rate.
        m.inputChannels = inputChannels;
        m.outputChannels = outputChannels;
        m.thread = GetCurrentThreadId();
        m.inputAddress = reinterpret_cast<std::uintptr_t>(input);
        m.outputAddress = reinterpret_cast<std::uintptr_t>(output);
        m.ownerAddress = reinterpret_cast<std::uintptr_t>(self);
        m.configurationValid = true;
        ticket = g_listenerProbe.claim(m);
        if (ticket) {
            auto &extra = g_listenerProbeExtra[ticket.index()];
            const auto *state = portaudio::read<const void *>(self, 8);
            extra.state = reinterpret_cast<std::uintptr_t>(state);
            if (state) {
                extra.enabled = portaudio::read<std::uint8_t>(state, 0x50) != 0;
                const auto peak = portaudio::read<float>(state, 0x198);
                if (std::isfinite(peak)) extra.inputPeakBefore = peak;
                else extra.peakNonFiniteMask |= 1;
            }
            if (output) {
                const auto count = (std::min)(std::size_t(frames), input::probe::kSampleFrames) * outputChannels;
                for (std::size_t i = 0; i < count; ++i) {
                    if (std::isfinite(output[i])) extra.outputBefore[i] = output[i];
                    else extra.outputBeforeNonFiniteMask |= std::uint32_t{1} << i;
                }
            }
            g_listenerProbe.captureInput(ticket, input, true);
        }
    }
    struct SinkGuard {
        bool held = false;
        ~SinkGuard() { if (held) g_listenerSinkBusy.clear(std::memory_order_release); }
    } sinkGuard;
    float *originalOutput = output;
    const auto *listenerState = valid ? portaudio::read<const void *>(self, 8) : nullptr;
    const bool listenerEnabled = listenerState && portaudio::read<std::uint8_t>(listenerState, 0x50) != 0;
    bool sunk = false;
    if (((drainRecord && g_drainProbeRequested) || (ticket && g_listenerSinkRequested && !g_drainProbeRequested)) &&
        valid && input && output && inputChannels == 2 && outputChannels == 2 && listenerEnabled) {
        sinkGuard.held = !g_listenerSinkBusy.test_and_set(std::memory_order_acquire);
        if (ticket) g_listenerProbeExtra[ticket.index()].sinkBusy = !sinkGuard.held;
        if (sinkGuard.held) {
            if (drainRecord) std::copy_n(output, static_cast<std::size_t>(frames) * outputChannels,
                                         g_listenerCallerBefore.data());
            std::fill_n(g_listenerSink.data(), static_cast<std::size_t>(frames) * outputChannels, 0.0f);
            originalOutput = g_listenerSink.data();
            sunk = true;
            if (ticket) g_listenerProbeExtra[ticket.index()].sinkApplied = true;
        }
    }
    const auto originalStarted = ticket ? steadyNanoseconds() : 0;
    const auto result = original(self, input, inputChannels, originalOutput, outputChannels, frames, timePoint);
    if (drainRecord) {
        if (sunk && result == frames && listenerState == portaudio::read<const void *>(self, 8) &&
            portaudio::read<std::uint8_t>(listenerState, 0x50) != 0) ++drainRecord->listenerSunk;
        else drainRecord->listenerValid = false;
        if (sunk && result == frames) {
            for (std::size_t i = 0; i < std::size_t(frames) * outputChannels; ++i) {
                if (std::memcmp(output + i, g_listenerCallerBefore.data() + i, sizeof(float)) != 0)
                    ++drainRecord->callerChangedSamples;
                if (std::isfinite(input[i]) && std::isfinite(g_listenerSink[i]) && std::isfinite(output[i])) {
                    drainRecord->captureEnergy += double(input[i]) * input[i];
                    drainRecord->nativeEnergy += double(g_listenerSink[i]) * g_listenerSink[i];
                    drainRecord->callerEnergy += double(output[i]) * output[i];
                } else ++drainRecord->nonFiniteSamples;
            }
        }
    }
    if (ticket) {
        const auto originalEnded = steadyNanoseconds();
        auto &extra = g_listenerProbeExtra[ticket.index()];
        if (extra.sinkApplied && result == frames) {
            const auto count = (std::min)(std::size_t(frames), input::probe::kSampleFrames) * outputChannels;
            for (std::size_t i = 0; i < count; ++i) {
                if (std::isfinite(g_listenerSink[i])) extra.sinkOutput[i] = g_listenerSink[i];
                else extra.sinkNonFiniteMask |= std::uint32_t{1} << i;
            }
        }
        const auto *state = portaudio::read<const void *>(self, 8);
        extra.stateStable = state && reinterpret_cast<std::uintptr_t>(state) == extra.state;
        if (extra.stateStable) {
            const auto inputPeak = portaudio::read<float>(state, 0x198);
            const auto outputPeak = portaudio::read<float>(state, 0x19C);
            if (std::isfinite(inputPeak)) extra.inputPeakAfter = inputPeak;
            else extra.peakNonFiniteMask |= 2;
            if (std::isfinite(outputPeak)) extra.outputPeakAfter = outputPeak;
            else extra.peakNonFiniteMask |= 4;
        }
        const auto ended = steadyNanoseconds();
        g_listenerProbe.complete(ticket, {originalEnded, ended, originalEnded - originalStarted,
            ended - started, result == frames ? 0 : -1}, output, result == frames);
    }
    return result;
#else
    return original(self, input, inputChannels, output, outputChannels, frames, timePoint);
#endif
}

int streamCallbackChunk(const void *input, void *output, unsigned long frames,
                       const void *timeInfo, unsigned long status, void *userData) {
#ifdef GPVST3_P13_PROBE_BUILD
    InputTimingScope timingScope(frames, status);
#endif
    const auto original = reinterpret_cast<StreamCallback>(g_runtime.stream.trampoline);
    const auto outputSequence = g_runtime.outputCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    const void *monitorInput = input;
#ifdef GPVST3_P13_PROBE_BUILD
    // Measurement only: capture channel 1 is the external source; channel 2
    // is the physical right-output return. Record both raw channels but feed
    // only the source, duplicated to stereo, to both monitoring paths. The
    // return must never feed back into the listener. Normal builds do not
    // contain this experiment or change the user's channel mapping.
    std::array<float, portaudio::kMaxFrames * 2> measurementInput{};
    bool measurementMappingValid = false;
    if (g_monitorLatencyProbe) {
        portaudio::Configuration config;
        const auto identity = asioprobe::currentCallback();
        const bool validFormat = frames > 0 && frames <= portaudio::kMaxFrames &&
            portaudio::configuration(g_runtime.audioModule, userData, config) && config.inputChannels <= 2;
        // During the normal UI transition the native gain remains zero. For a
        // supported buffer, unknown topology fails silent instead of forwarding
        // a return channel. The collector requires confirmed mapping before
        // it raises the native monitor gain.
        if (validFormat) monitorInput = measurementInput.data();
        if (validFormat && identity.rateValidated && input && config.inputChannels == 2) {
            const auto *stream = portaudio::read<const void *>(userData, 8);
            const auto *buffers = portaudio::read<const std::uint8_t *>(stream, 0x180);
            const auto *base = static_cast<const std::uint8_t *>(g_runtime.audioModule);
            measurementMappingValid = stream && buffers &&
                stream == portaudio::read<const void *>(base, 0x2F2620) &&
                portaudio::read<const void *>(stream, 0x28) == userData &&
                portaudio::read<std::int32_t>(stream, 0x198) == 2 &&
                portaudio::read<std::int32_t>(stream, 0x19C) == 2 &&
                portaudio::read<std::int32_t>(buffers, 0) == 1 && portaudio::read<std::int32_t>(buffers, 4) == 0 &&
                portaudio::read<std::int32_t>(buffers + 24, 0) == 1 && portaudio::read<std::int32_t>(buffers + 24, 4) == 1 &&
                portaudio::read<std::int32_t>(buffers + 48, 0) == 0 && portaudio::read<std::int32_t>(buffers + 48, 4) == 0 &&
                portaudio::read<std::int32_t>(buffers + 72, 0) == 0 && portaudio::read<std::int32_t>(buffers + 72, 4) == 1;
            if (measurementMappingValid)
                for (std::size_t i = 0; i < frames; ++i)
                    measurementInput[i * 2] = measurementInput[i * 2 + 1] = static_cast<const float *>(input)[i * 2];
        }
        g_monitorLatencyMappingValid.store(measurementMappingValid);
    }
    struct OuterProbeScope {
        std::uint64_t previous;
        explicit OuterProbeScope(std::uint64_t sequence) : previous(g_probeOuterSequence) { g_probeOuterSequence = sequence; }
        ~OuterProbeScope() { g_probeOuterSequence = previous; }
    } outerProbeScope(outputSequence);
    input::probe::Recorder::Ticket probeTicket;
    input::pcmprobe::Recorder::Ticket pcmTicket;
    asioprobe::CallbackIdentity callbackIdentity;
    std::uint64_t probeStarted = 0;
    std::uint64_t probeOriginalEnded = 0;
    std::uint64_t probeOriginalStarted = 0;
    bool probeConfigurationValid = false;
    bool probeSilenced = false;
    std::size_t probeOutputChannels = 0;
    std::array<float, input::probe::kSampleFrames * input::probe::kSampleChannels> probeOutput{};
    if ((g_inputProbe.enabled() && g_inputProbe.claimed() < input::probe::kCapacity) ||
        (g_inputPcmProbe.enabled() &&
         g_inputPcmProbe.framesReserved() < input::pcmprobe::kFrameCapacity &&
         g_inputPcmProbe.claimed() < input::pcmprobe::kRecordCapacity)) {
        probeStarted = steadyNanoseconds();
        if (probeStarted >= g_inputProbeStart) {
            portaudio::Configuration config;
            probeConfigurationValid = frames > 0 && frames <= portaudio::kMaxFrames &&
                portaudio::configuration(g_runtime.audioModule, userData, config);
            probeOutputChannels = config.outputChannels;
            probeSilenced = g_inputProbeSilence && probeConfigurationValid && input && output;
            input::probe::Metadata metadata;
            metadata.sequence = outputSequence;
            callbackIdentity = asioprobe::currentCallback();
            metadata.streamGeneration = callbackIdentity.generation;
            metadata.rateRevision = callbackIdentity.rateRevision;
            metadata.actualSampleRate = callbackIdentity.actualRate;
            metadata.actualRateValidated = callbackIdentity.rateValidated;
            metadata.timestampNanoseconds = probeStarted;
            metadata.status = status;
            metadata.frames = frames;
            metadata.sampleRate = config.sampleRate;
            metadata.inputChannels = config.inputChannels;
            metadata.outputChannels = config.outputChannels;
            metadata.inputDevice = config.inputDevice;
            metadata.outputDevice = config.outputDevice;
            if (probeConfigurationValid) {
                // AMAudio.dll 8.1.1.17 embeds PortAudio 396fe4b6. The ASIO
                // callback reads this same active stream global and frame
                // field. No inference from preferences or callback frames.
                const auto *stream = portaudio::read<const void *>(userData, 8);
                const auto *streamInterface = portaudio::read<const void *>(stream, 0x10);
                const auto *base = static_cast<const std::uint8_t *>(g_runtime.audioModule);
                if (stream == portaudio::read<const void *>(g_runtime.audioModule, 0x2F2620) &&
                    streamInterface &&
                    portaudio::read<const void *>(streamInterface, 0) == base + 0x6DF60 &&
                    portaudio::read<const void *>(streamInterface, 8) == base + 0x6FE90 &&
                    portaudio::read<const void *>(streamInterface, 16) == base + 0x700A0 &&
                    portaudio::read<const void *>(stream, 0x28) == userData) {
                    const auto driverFrames = portaudio::read<std::uint32_t>(stream, 0x178);
                    if (driverFrames > 0 && driverFrames <= 65536) {
                        metadata.hostApiType = 3; // paASIO (PortAudio PaHostApiTypeId).
                        metadata.driverFrames = driverFrames;
                        metadata.driverInputLatency = portaudio::read<std::int32_t>(stream, 0x190);
                        metadata.driverOutputLatency = portaudio::read<std::int32_t>(stream, 0x194);
                        const auto inputCount = portaudio::read<std::int32_t>(stream, 0x198);
                        const auto outputCount = portaudio::read<std::int32_t>(stream, 0x19C);
                        const auto *buffers = portaudio::read<const std::uint8_t *>(stream, 0x180);
                        const auto *channels = portaudio::read<const std::uint8_t *>(stream, 0x188);
                        bool validChannels = buffers && channels && inputCount == int(config.inputChannels) &&
                            outputCount == int(config.outputChannels);
                        for (int i = 0; validChannels && i < inputCount + outputCount; ++i) {
                            const bool isInput = i < inputCount;
                            const auto selector = portaudio::read<std::int32_t>(buffers + i * 24, 4);
                            validChannels = selector >= 0 && selector < 65536 &&
                                portaudio::read<std::int32_t>(buffers + i * 24, 0) == int(isInput) &&
                                portaudio::read<std::int32_t>(channels + i * 52, 0) == selector &&
                                portaudio::read<std::int32_t>(channels + i * 52, 4) == int(isInput);
                            if (validChannels) {
                                if (isInput) metadata.driverInputSelectors[std::size_t(i)] = selector;
                                else metadata.driverOutputSelectors[std::size_t(i - inputCount)] = selector;
                            }
                        }
                        metadata.driverChannelsValidated = validChannels;
                    }
                }
            }
            metadata.thread = GetCurrentThreadId();
            metadata.inputAddress = reinterpret_cast<std::uintptr_t>(input);
            metadata.outputAddress = reinterpret_cast<std::uintptr_t>(output);
            metadata.ownerAddress = reinterpret_cast<std::uintptr_t>(userData);
            metadata.configurationValid = probeConfigurationValid;
            if (probeConfigurationValid && input && output) {
                metadata.pointersAlias = metadata.inputAddress <= metadata.outputAddress
                    ? metadata.outputAddress - metadata.inputAddress < frames * config.inputChannels * sizeof(float)
                    : metadata.inputAddress - metadata.outputAddress < frames * config.outputChannels * sizeof(float);
            }
            metadata.experiment = probeSilenced;
            metadata.monitorReferenceIsInput1 = measurementMappingValid;
            // Backend/driver frames remain unknown when the ASIO identity
            // gate failed. A generation is known only inside a bound proxy.
            probeTicket = g_inputProbe.claim(metadata);
            g_inputProbe.captureInput(probeTicket, input, probeConfigurationValid);
            pcmTicket = g_inputPcmProbe.begin(metadata, input, probeConfigurationValid);
        }
    }
#endif
    const auto outputAddress = reinterpret_cast<std::uintptr_t>(output);
    auto firstBuffer = g_runtime.outputFirstBuffer.load(std::memory_order_relaxed);
    if (firstBuffer == 0)
        g_runtime.outputFirstBuffer.compare_exchange_strong(firstBuffer, outputAddress,
                                                              std::memory_order_relaxed);
    g_runtime.outputLastBuffer.store(outputAddress, std::memory_order_relaxed);
    // Output hashes prove the callback writeback once. They are not part of
    // audio processing, so stop scanning the realtime buffer after that
    // evidence has been claimed.
    const bool needOutputHashes =
#ifdef GPVST3_P13_PROBE_BUILD
        !g_inputProbe.enabled() &&
#endif
        !g_runtime.outputEvidenceClaimed.load(std::memory_order_relaxed);
    const auto before = needOutputHashes ? outputHash(output, frames) : 0;
    MonitorCallback monitor(monitorInput, output, frames, userData, outputSequence, status);
    struct MonitorContext {
        MonitorCallback *previous;
        explicit MonitorContext(MonitorCallback &current) : previous(g_monitorCallback) { g_monitorCallback = &current; }
        ~MonitorContext() { g_monitorCallback = previous; }
    } monitorContext(monitor);
#ifdef GPVST3_P13_PROBE_BUILD
    DrainProbeScope drainScope(userData, frames, outputSequence, status);
    if (probeTicket || pcmTicket) probeOriginalStarted = steadyNanoseconds();
    const auto result = original(probeTicket && probeSilenced ? g_inputProbeZeros.data() : monitorInput,
                                 output, frames, timeInfo, status, userData);
    if (probeTicket || pcmTicket) probeOriginalEnded = steadyNanoseconds();
    if (probeTicket && probeConfigurationValid && output && result == 0)
        std::copy_n(static_cast<const float *>(output),
            (std::min)(std::size_t(frames), input::probe::kSampleFrames) * probeOutputChannels,
            probeOutput.data());
    if (g_overlayProbeRequested && g_currentDrainRecord && output && frames <= portaudio::kMaxFrames)
        std::copy_n(static_cast<const float *>(output), std::size_t(frames) * 2, g_overlayProbeBefore.data());
    monitor.finish(result);
    drainScope.finish(result, static_cast<const float *>(output));
    if (probeTicket || pcmTicket) {
        const auto overlayEnded = steadyNanoseconds();
        const auto completedIdentity = asioprobe::currentCallback();
        const bool sameCallbackIdentity = completedIdentity.generation == callbackIdentity.generation &&
            completedIdentity.rateRevision == callbackIdentity.rateRevision &&
            completedIdentity.rateValidated == callbackIdentity.rateValidated &&
            completedIdentity.actualRate == callbackIdentity.actualRate;
        if (pcmTicket) g_inputPcmProbe.complete(pcmTicket,
            {probeOriginalEnded, overlayEnded,
             probeOriginalEnded - probeOriginalStarted, overlayEnded - probeStarted, result},
            output, probeConfigurationValid && result == 0 && sameCallbackIdentity);
    }
#else
    const auto result = original(input, output, frames, timeInfo, status, userData);
    monitor.finish(result);
#endif
    const auto inputState = g_runtime.inputRouter.snapshot();
    portaudio::Configuration configuration;
    const bool configurationValid = frames > 0 &&
        frames <= portaudio::kMaxFrames &&
        portaudio::configuration(g_runtime.audioModule, userData, configuration);
    if (configurationValid) {
        g_runtime.inputObservedRate.store(static_cast<int>(configuration.sampleRate),
                                           std::memory_order_relaxed);
        g_runtime.inputObservedChannels.store(configuration.inputChannels,
                                               std::memory_order_relaxed);
        g_runtime.inputObservedOutputChannels.store(configuration.outputChannels,
                                                    std::memory_order_relaxed);
        g_runtime.inputConfiguredChannels.store(configuration.inputChannels,
                                                std::memory_order_release);
        g_runtime.inputConfiguredOutputChannels.store(configuration.outputChannels,
                                                       std::memory_order_release);
    } else {
        g_runtime.inputConfigurationErrors.fetch_add(1, std::memory_order_relaxed);
    }
    const bool preparedConfiguration = configurationValid &&
        static_cast<int>(configuration.sampleRate) ==
            g_runtime.inputConfiguredRate.load(std::memory_order_acquire);
    if (
        monitor.legacy() && !monitor.suppress() &&
        preparedConfiguration && inputState.enabled &&
        inputState.route != input::Route::Disabled) {
        g_runtime.inputCapturePathLocated.store(true, std::memory_order_release);
        g_runtime.inputCaptureCalls.fetch_add(1, std::memory_order_relaxed);
        const input::InterleavedView view{
            input, output, static_cast<std::size_t>(frames), configuration.inputChannels,
            configuration.outputChannels, configuration.sampleRate,
            static_cast<std::size_t>(frames),
            userData, static_cast<std::uint64_t>(outputSequence),
            input::InterleavedSampleFormat::Float32};
        const bool needInputOrderHash =
            !g_runtime.inputOrderEvidenceClaimed.load(std::memory_order_relaxed);
        const auto postOriginal = needInputOrderHash ? outputHash(output, frames) : 0;
        const bool observeSamples = input && output &&
            !g_runtime.inputOrderSamplesObserved.load(std::memory_order_acquire) &&
            g_runtime.trackChainProcessedBlocks.load(std::memory_order_relaxed) > 0 &&
            g_runtime.globalChainProcessBlocks.load(std::memory_order_relaxed) > 0;
        const float captureSample = observeSamples ? static_cast<const float *>(input)[0] : 0.0F;
        const float generatedSample = observeSamples ? static_cast<const float *>(output)[0] : 0.0F;
        if (processExternalInputInterleaved(view)) {
            const auto postRoute = needInputOrderHash ? outputHash(output, frames) : 0;
            g_runtime.inputAfterOriginalBlocks.fetch_add(1, std::memory_order_release);
            bool expected = false;
            if (needInputOrderHash && postOriginal != postRoute &&
                g_runtime.inputOrderEvidenceClaimed.compare_exchange_strong(expected, true)) {
                g_runtime.inputPostOriginalHash.store(postOriginal, std::memory_order_relaxed);
                g_runtime.inputPostRouteHash.store(postRoute, std::memory_order_relaxed);
            }
            // Copy one non-silent sample from this same callback. Never retain
            // borrowed device pointers; publish the tuple only after all stores.
            expected = false;
            if (observeSamples && std::abs(generatedSample) > 0.000001F &&
                g_runtime.inputOrderSamplesClaimed.compare_exchange_strong(expected, true)) {
                g_runtime.inputOrderCaptureSample.store(captureSample, std::memory_order_relaxed);
                g_runtime.inputOrderGeneratedSample.store(generatedSample, std::memory_order_relaxed);
                g_runtime.inputOrderOutputSample.store(static_cast<const float *>(output)[0], std::memory_order_relaxed);
                g_runtime.inputOrderSamplesObserved.store(true, std::memory_order_release);
            }
        }
    }
    const auto after = needOutputHashes ? outputHash(output, frames) : 0;
    g_runtime.outputObserved.store(output != nullptr && frames != 0, std::memory_order_release);
    if (needOutputHashes && before != after) {
        bool expected = false;
        if (g_runtime.outputEvidenceClaimed.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel)) {
        // Keep the first changed pair so a later silent block cannot overwrite
        // the evidence that the device callback actually wrote its output.
            g_runtime.outputBeforeHash.store(before, std::memory_order_relaxed);
            g_runtime.outputAfterHash.store(after, std::memory_order_relaxed);
            g_runtime.outputWriteObserved.store(true, std::memory_order_release);
        }
    }
    g_runtime.outputFrames.store(frames, std::memory_order_relaxed);
    g_runtime.outputThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
#ifdef GPVST3_P13_PROBE_BUILD
    timingScope.result = result;
    if (probeTicket) {
        const auto ended = steadyNanoseconds();
        // Never sample pre-callback output, or legacy-router output as native.
        g_inputProbe.complete(probeTicket,
            {probeOriginalEnded, ended, probeOriginalEnded - probeOriginalStarted,
             ended - probeStarted, result}, probeOutput.data(),
            probeConfigurationValid && output && result == 0);
    }
#endif
    return result;
}

int streamCallbackHook(const void *input, void *output, unsigned long frames,
                       const void *timeInfo, unsigned long status, void *userData) {
    if (frames <= portaudio::kMaxFrames)
        return streamCallbackChunk(input, output, frames, timeInfo, status, userData);
    portaudio::Configuration config;
    const auto identity = asioprobe::currentCallback();
    // GP clamps each native call to 2048 frames. Keep that ABI capacity and
    // advance the borrowed buffers so larger ASIO buffers have no stale tail.
    if (frames > 8192 || !output ||
        !portaudio::configuration(g_runtime.audioModule, userData, config))
        return reinterpret_cast<StreamCallback>(g_runtime.stream.trampoline)(
            input, output, frames, timeInfo, status, userData);
    for (unsigned long offset = 0; offset < frames;) {
        const auto count = (std::min)(static_cast<unsigned long>(portaudio::kMaxFrames), frames - offset);
        double times[3]{};
        if (timeInfo) {
            std::memcpy(times, timeInfo, sizeof(times));
            if (identity.rateValidated && identity.actualRate > 0) {
                // currentTime belongs to this driver invocation. GP derives
                // the render timestamp from DAC-current, so only advance the
                // ADC/DAC sample positions for subsequent native sub-blocks.
                times[0] += double(offset) / identity.actualRate;
                times[2] += double(offset) / identity.actualRate;
            }
        }
        const auto result = streamCallbackChunk(
            input ? static_cast<const float *>(input) + offset * config.inputChannels : nullptr,
            static_cast<float *>(output) + offset * config.outputChannels, count,
            timeInfo ? times : nullptr, status, userData);
        offset += count;
        if (result != 0) {
            std::fill_n(static_cast<float *>(output) + offset * config.outputChannels,
                (frames - offset) * config.outputChannels, 0.0f);
            return result;
        }
    }
    return 0;
}

void setTotalBypass(bool bypassed) noexcept {
    g_runtime.chain.setBypassed(bypassed);
    g_runtime.inputRouter.setBypassed(bypassed);
}

#ifdef GPVST3_P13_PROBE_BUILD
QJsonObject inputStreamProbeSnapshot() { return asioprobe::snapshot(); }

QJsonObject inputTimingProbeSnapshot() {
    const auto value = g_inputTimingProbe.snapshot(steadyNanoseconds());
    const auto histogram = [](const input::timingprobe::HistogramSnapshot &samples) {
        const auto percentile = [&](unsigned percent) {
            const auto upper = samples.percentileUpper(percent);
            return upper ? QJsonValue(qint64(upper)) : QJsonValue();
        };
        QJsonArray buckets;
        for (const auto count : samples.buckets) buckets.append(qint64(count));
        return QJsonObject{{"count", QString::number(samples.count)},
            {"bucket_width_ns", qint64(input::timingprobe::kBucketNanoseconds)},
            {"overflow_at_ns", qint64(input::timingprobe::kHistogramBuckets * input::timingprobe::kBucketNanoseconds)},
            {"overflow", QString::number(samples.overflow)}, {"buckets", buckets},
            {"p50_exclusive_upper_ns", percentile(50)}, {"p95_exclusive_upper_ns", percentile(95)},
            {"maximum_ns", QString::number(samples.maximum)}};
    };
    return {{"schema", 1}, {"enabled", value.enabled}, {"stopped", value.stopped}, {"acceptance", "not_evaluated"},
        {"scope", "hook_entry_to_exit_including_experimental_instrumentation; input_process_is_router_only; not_complete_driver_deadline_or_xrun_measurement"},
        {"coherent_snapshot", value.coherent},
        {"window_start_ns", QString::number(value.windowStart)}, {"window_end_ns", QString::number(value.windowEnd)},
        {"maximum_window_ns", QString::number(input::timingprobe::kWindowNanoseconds)},
        {"maximum_callbacks", QString::number(input::timingprobe::kCallbackLimit)},
        {"time_bound_reached", value.timeBoundReached}, {"callback_bound_reached", value.callbackBoundReached},
        {"first_started_ns", QString::number(value.firstStarted)}, {"last_ended_ns", QString::number(value.lastEnded)},
        {"admitted_callbacks", QString::number(value.admitted)}, {"completed_callbacks", QString::number(value.completed)},
        {"overlapping_callbacks_skipped", QString::number(value.overlappingCallbacks)},
        {"status_flag_callbacks", QString::number(value.statusCallbacks)}, {"status_flags_or", QString::number(value.statusFlags)},
        {"callback_result_errors", QString::number(value.resultErrors)}, {"invalid_clock_callbacks", QString::number(value.invalidClocks)},
        {"budget_scope", "callback_frames_divided_by_per_callback_validated_ASIO_rate; excludes_driver_work_outside_this_hook"},
        {"budget_validated_callbacks", QString::number(value.validatedBudgets)},
        {"budget_unvalidated_callbacks", QString::number(value.unvalidatedBudgets)},
        {"callback_budget_exceedances", QString::number(value.callbackBudgetExceeded)},
        {"input_process_budget_exceedances", QString::number(value.inputBudgetExceeded)},
        {"input_process_calls", QString::number(value.inputCalls)}, {"input_process_failures", QString::number(value.inputFailures)},
        {"validated_identity_changes", QString::number(value.identityChanges)},
        {"minimum_validated_rate", value.minimumRate}, {"maximum_validated_rate", value.maximumRate},
        {"minimum_validated_frames", qint64(value.minimumFrames)}, {"maximum_validated_frames", qint64(value.maximumFrames)},
        {"callback", histogram(value.callback)}, {"input_process", histogram(value.inputProcess)}};
}

QJsonObject stopInputTimingProbe() {
    g_inputTimingProbe.stop();
    asioprobe::stopTiming();
    QJsonObject hook, driver;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    do {
        const bool hookIdle = g_inputTimingProbe.stop();
        const bool driverIdle = asioprobe::stopTiming();
        hook = inputTimingProbeSnapshot();
        driver = asioprobe::timingSnapshot();
        hook.insert("quiesced", hookIdle);
        driver.insert("quiesced", driverIdle);
        if (hookIdle && driverIdle && hook.value("coherent_snapshot").toBool() && driver.value("coherent_snapshot").toBool()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    return {{"callback_timing", hook}, {"driver_callback_timing", driver}};
}

QJsonObject exportInputPcmProbe(double actualRate, int rateResult) {
    using namespace input::pcmprobe;
    static QJsonObject completed;
    if (!completed.isEmpty()) return completed;
    QJsonObject result{{"enabled", g_inputPcmProbe.enabled()}, {"complete", false},
        {"claimed", qint64(g_inputPcmProbe.claimed())},
        {"published", qint64(g_inputPcmProbe.published())},
        {"frames_reserved", qint64(g_inputPcmProbe.framesReserved())}};
    if (!g_inputPcmProbe.enabled() ||
        (g_inputPcmProbe.framesReserved() < kFrameCapacity && g_inputPcmProbe.claimed() < kRecordCapacity))
        return result;
    const auto count = g_inputPcmProbe.claimed();
    if (g_inputPcmProbe.published() != count) return result;

    const QDir directory(state::dataDirectory());
    QSaveFile captureFile(directory.filePath("p13-pcm-capture.f32"));
    QSaveFile outputFile(directory.filePath("p13-pcm-output.f32"));
    if (!captureFile.open(QIODevice::WriteOnly) || !outputFile.open(QIODevice::WriteOnly)) {
        result.insert("error", "pcm_file_open_failed");
        return result;
    }
    QCryptographicHash captureHash(QCryptographicHash::Sha256), outputHash(QCryptographicHash::Sha256);
    QJsonArray records;
    qint64 captureOffset = 0, outputOffset = 0;
    for (std::size_t i = 0; i < count; ++i) {
        Snapshot value;
        if (!g_inputPcmProbe.snapshot(i, value)) {
            result.insert("error", "pcm_unpublished_record");
            return result;
        }
        const auto &record = value.record;
        const auto &m = record.metadata;
        const auto captureBytes = qint64(value.captureSamples * sizeof(float));
        const auto outputBytes = qint64(value.postOriginalSamples * sizeof(float));
        if ((captureBytes && captureFile.write(reinterpret_cast<const char *>(value.capture), captureBytes) != captureBytes) ||
            (outputBytes && outputFile.write(reinterpret_cast<const char *>(value.postOriginal), outputBytes) != outputBytes)) {
            result.insert("error", "pcm_file_write_failed");
            return result;
        }
        if (captureBytes) captureHash.addData(reinterpret_cast<const char *>(value.capture), int(captureBytes));
        if (outputBytes) outputHash.addData(reinterpret_cast<const char *>(value.postOriginal), int(outputBytes));
        records.append(QJsonObject{{"index", qint64(i)}, {"sequence", QString::number(m.sequence)},
            {"timestamp_ns", QString::number(m.timestampNanoseconds)},
            {"frame_offset", qint64(record.frameOffset)}, {"saved_frames", qint64(record.savedFrames)},
            {"callback_frames", qint64(m.frames)}, {"truncated", record.truncated},
            {"changes", qint64(record.changes)}, {"status_flags", QString::number(m.status)},
            {"original_result", record.completion.callbackResult}, {"configuration_valid", m.configurationValid},
            {"owner", QString::number(m.ownerAddress, 16)}, {"generation", QString::number(m.streamGeneration)},
            {"rate_revision", QString::number(m.rateRevision)},
            {"actual_rate_validated", m.actualRateValidated}, {"actual_callback_rate", m.actualSampleRate},
            {"monitor_reference_is_input1", m.monitorReferenceIsInput1},
            {"thread", qint64(m.thread)}, {"requested_sample_rate", m.sampleRate},
            {"host_api_type", m.hostApiType}, {"driver_frames", qint64(m.driverFrames)},
            {"driver_channels_validated", m.driverChannelsValidated},
            {"driver_input_selectors", QJsonArray{m.driverInputSelectors[0], m.driverInputSelectors[1]}},
            {"driver_output_selectors", QJsonArray{m.driverOutputSelectors[0], m.driverOutputSelectors[1]}},
            {"input_device", m.inputDevice}, {"output_device", m.outputDevice},
            {"input_channels", qint64(m.inputChannels)}, {"output_channels", qint64(m.outputChannels)},
            {"capture_byte_offset", captureOffset}, {"capture_bytes", captureBytes},
            {"output_byte_offset", outputOffset}, {"output_bytes", outputBytes},
            {"capture_status", int(record.capture.status)}, {"output_status", int(record.postOriginal.status)},
            {"capture_nonfinite", qint64(record.capture.nonFiniteCount)},
            {"output_nonfinite", qint64(record.postOriginal.nonFiniteCount)}});
        captureOffset += captureBytes;
        outputOffset += outputBytes;
    }
    if (!captureFile.commit() || !outputFile.commit()) {
        result.insert("error", "pcm_file_commit_failed");
        return result;
    }
    result.insert("schema", 1);
    result.insert("complete", true);
    result.insert("format", "float32_little_endian_interleaved_per_record");
    result.insert("capture_file", "p13-pcm-capture.f32");
    result.insert("output_file", "p13-pcm-output.f32");
    result.insert("output_stage", "after_original_and_input_overlay_before_legacy_router");
    result.insert("capture_stage", "raw_device_capture_before_measurement_mapping");
    result.insert("monitor_latency_measurement", g_monitorLatencyProbe);
    result.insert("capture_sha256", QString::fromLatin1(captureHash.result().toHex()));
    result.insert("output_sha256", QString::fromLatin1(outputHash.result().toHex()));
    result.insert("busy_drops", QString::number(g_inputPcmProbe.busyDrops()));
    result.insert("abandoned", QString::number(g_inputPcmProbe.abandoned()));
    result.insert("actual_asio_sample_rate", actualRate > 0 ? QJsonValue(actualRate) : QJsonValue());
    result.insert("actual_rate_query_result", rateResult);
    result.insert("actual_rate_query_source", "ASIOGetSampleRate_thunk_0x6A540_ASIOError");
    result.insert("rate_scope", "control_thread_observation_after_recording_not_generation_bound");
    result.insert("acceptance", "not_evaluated");
    result.insert("records", records);
    QSaveFile manifest(directory.filePath("p13-pcm.json"));
    const auto bytes = QJsonDocument(result).toJson();
    if (!manifest.open(QIODevice::WriteOnly) || manifest.write(bytes) != bytes.size() || !manifest.commit()) {
        result.insert("complete", false);
        result.insert("error", "pcm_manifest_commit_failed");
        return result;
    }
    completed = result;
    return result;
}

QJsonObject drainProbeSnapshot() {
    static QJsonObject completed;
    if (!completed.isEmpty()) return completed;
    QJsonArray records;
    QJsonArray topology;
    std::size_t valid = 0, ready = 0;
    const auto count = g_drainProbeCount.load(std::memory_order_acquire);
    for (std::size_t i = 0; i < count; ++i) {
        if (!g_drainProbeRecords[i].published.load(std::memory_order_acquire)) continue;
        const auto &r = g_drainProbeRecords[i].record;
        if (r.valid) ++valid;
        if (r.valid && r.drain.state == input::drain::State::Ready) ++ready;
        if (i == 0) {
            for (std::size_t channel = 0; channel < 2; ++channel) {
                const auto &t = r.before.config.channel[channel];
                const auto &c = r.before.channels[channel];
                topology.append(QJsonObject{{"channel", int(channel)},
                    {"convolver_count", int(t.convolverCount)}, {"up", int(t.upFactor)}, {"down", int(t.downFactor)},
                    {"input_len", int(t.inputLen)}, {"previous_input_len", int(t.previousInputLen)},
                    {"block_len2", int(t.blockLen2)}, {"latency", int(t.latency)},
                    {"kernel_len", c.kernelLength}, {"block_len_bits", c.blockLengthBits},
                    {"input_delay", int(t.inputDelay)}, {"up_shift", int(t.upShift)}, {"down_shift", int(t.downShift)},
                    {"consume_latency", t.consumesLatency}, {"latency_fraction", c.latencyFraction},
                    {"in_data_left", c.inDataLeft}, {"latency_left", c.latencyLeft},
                    {"buffer_left", c.bufferLeft}, {"write_position", c.writePosition}, {"read_position", c.readPosition}});
            }
        }
        records.append(QJsonObject{{"index", qint64(i)}, {"sequence", QString::number(r.sequence)},
            {"timestamp_ns", QString::number(r.timestamp)}, {"callback_ns", QString::number(r.ended-r.timestamp)},
            {"thread", qint64(r.thread)}, {"frames", int(r.frames)}, {"valid", r.valid},
            {"status_flags", qint64(r.statusFlags)}, {"caller_changed_samples", qint64(r.callerChangedSamples)},
            {"nonfinite_samples", qint64(r.nonFiniteSamples)}, {"capture_energy", r.captureEnergy},
            {"native_energy", r.nativeEnergy}, {"caller_energy", r.callerEnergy}, {"output_energy", r.outputEnergy},
            {"rse_calls", int(r.rseCalls)}, {"rse_valid", r.rseValid}, {"rse_energy", r.rseEnergy},
            {"parent_gain", std::isfinite(r.parentGain) ? QJsonValue(r.parentGain) : QJsonValue()},
            {"rse_compared_samples", qint64(r.rseComparedSamples)}, {"rse_mismatch_samples", qint64(r.rseMismatchSamples)},
            {"source_observer_calls", int(r.sourceObserverCalls)},
            {"monitor_token", QString::number(r.monitorToken)},
            {"production_drain_state", int(r.productionDrain.state)}, {"production_drain_phase", int(r.productionDrain.phase)},
            {"overlay_suppressed", r.overlaySuppressed}, {"overlay_committed", r.overlayCommitted},
            {"overlay_compared_samples", int(r.overlayComparedSamples)}, {"overlay_mismatch_samples", int(r.overlayMismatchSamples)},
            {"pre_overlay_energy", r.preOverlayEnergy}, {"overlay_energy", r.overlayEnergy},
            {"listener_peak_samples", int(r.listenerPeakSamples)}, {"listener_input_peak", r.inputPeak},
            {"listener_output_peak", r.outputPeak},
            {"native_pre_gain", std::isfinite(r.nativePreGain) ? QJsonValue(r.nativePreGain) : QJsonValue()},
            {"generation", QString::number(r.before.context.generation)},
            {"rate_revision", QString::number(r.before.context.rateRevision)},
            {"actual_rate", r.before.context.actualRate}, {"rate_validated", r.before.context.rateValidated},
            {"before_error", int(r.before.error)}, {"after_error", int(r.after.error)},
            {"output_src", QString::number(reinterpret_cast<std::uintptr_t>(r.before.outputSrc), 16)},
            {"ring_storage", QString::number(reinterpret_cast<std::uintptr_t>(r.before.ring.storage), 16)},
            {"queued_before", qint64(r.before.ring.queued)}, {"queued_after", qint64(r.after.ring.queued)},
            {"read_before", qint64(r.before.ring.readIndex)}, {"read_after", qint64(r.after.ring.readIndex)},
            {"write_before", qint64(r.before.ring.writeIndex)}, {"write_after", qint64(r.after.ring.writeIndex)},
            {"consumed_samples", qint64(r.consumed)}, {"src_calls", int(r.src.calls)},
            {"src_input_frames", qint64(r.src.inputFrames)}, {"src_output_frames", qint64(r.src.outputFrames)},
            {"listener_calls", int(r.listenerCalls)}, {"listener_sunk", int(r.listenerSunk)},
            {"listener_valid", r.listenerValid}, {"original_result", r.originalResult},
            {"drain_state", int(r.drain.state)}, {"drain_phase", int(r.drain.phase)}, {"drain_error", int(r.drain.error)},
            {"convolver_target", qint64(r.drain.convolverTarget)}, {"convolver_frames", qint64(r.drain.convolverFrames)},
            {"interpolator_target", qint64(r.drain.interpolatorTarget)}, {"interpolator_frames", qint64(r.drain.interpolatorFrames)},
            {"old_ring_samples", qint64(r.drain.oldRingSamples)}});
    }
    QJsonObject result{{"requested", g_drainProbeRequested || g_overlayProbeRequested},
        {"overlay_probe", g_overlayProbeRequested}, {"transition_probe", g_overlayTransitionProbe}, {"capacity", int(kDrainProbeCapacity)},
        {"rse_probe_installed", g_rseProbeInstalled.load(std::memory_order_acquire)},
        {"complete", records.size() == int(kDrainProbeCapacity)}, {"published", records.size()},
        {"valid_records", qint64(valid)}, {"ready_records", qint64(ready)},
        {"overlaps", qint64(g_drainProbeOverlaps.load())}, {"initial_topology", topology},
        {"records", records}, {"acceptance", "not_evaluated"},
        {"scope", g_overlayProbeRequested ? "production_overlay_observation; unity_gain_fixture_expected; original_listener_called_and_internal_peaks_read; UI_meter_and_recording_not_proven" :
            "retrospective_counted_drain_experiment; no_overlay; no_shared_state_reset; native_DSP_runs; suppression_ends_after_window"}};
    if (result.value("complete").toBool()) completed = result;
    return result;
}

QJsonObject inputProbeSnapshot() {
    using namespace input::probe;
    // This function runs on the Qt/control thread. GP's ASIO extension can
    // retain PaStreamInfo.sampleRate=44100 while leaving the driver's rate
    // unchanged. Query the driver here, never in the realtime callback.
    double actualAsioRate = std::numeric_limits<double>::quiet_NaN();
    int asioRateResult = -1;
    if (!asioprobe::currentRate(actualAsioRate, asioRateResult) &&
        g_inputProbe.enabled() && g_runtime.audioModule &&
        portaudio::read<const void *>(g_runtime.audioModule, 0x2F2620)) {
        using GetAsioSampleRate = int (*)(double *);
        const auto getRate = reinterpret_cast<GetAsioSampleRate>(
            static_cast<std::uint8_t *>(g_runtime.audioModule) + 0x6A540);
        asioRateResult = getRate(&actualAsioRate);
        if (asioRateResult != 0 || !std::isfinite(actualAsioRate) ||
            actualAsioRate < 8000 || actualAsioRate > 768000) actualAsioRate = 0;
    }
    const auto sampleJson = [](const Samples &samples) {
        const char *status = "unvalidated";
        switch (samples.status) {
        case SampleStatus::Disabled: status = "disabled"; break;
        case SampleStatus::Missing: status = "missing"; break;
        case SampleStatus::InvalidFormat: status = "invalid_format"; break;
        case SampleStatus::Captured: status = "captured"; break;
        default: break;
        }
        QJsonArray values;
        for (std::size_t i = 0; i < samples.frames * samples.channels; ++i)
            values.append(samples.values[i]);
        return QJsonObject{{"status", status}, {"frames", samples.frames},
            {"channels", samples.channels}, {"non_finite_mask", qint64(samples.nonFiniteMask)},
            {"values", values}};
    };
    QJsonArray records;
    std::vector<double> callbackTimes, originalTimes;
    int deadlineMisses = 0;
    int statusFlags = 0;
    for (std::size_t index = 0; index < g_inputProbe.claimed(); ++index) {
        Record record;
        if (!g_inputProbe.snapshot(index, record)) continue;
        const auto &m = record.metadata;
        const auto &c = record.completion;
        callbackTimes.push_back(static_cast<double>(c.callbackNanoseconds));
        originalTimes.push_back(static_cast<double>(c.originalNanoseconds));
        // A non-ASIO or unobserved current rate cannot establish a deadline.
        if (m.hostApiType == 3 && actualAsioRate > 0 &&
            c.callbackNanoseconds > m.frames * 1e9 / actualAsioRate) ++deadlineMisses;
        if (m.status != 0) ++statusFlags;
        records.append(QJsonObject{
            {"index", qint64(index)}, {"sequence", QString::number(m.sequence)},
            {"timestamp_ns", QString::number(m.timestampNanoseconds)},
            {"stream_generation", m.streamGeneration ? QJsonValue(QString::number(m.streamGeneration)) : QJsonValue()},
            {"status_flags", qint64(m.status)}, {"frames", qint64(m.frames)},
            {"sample_rate", m.sampleRate}, {"sample_rate_source", "PaStreamInfo_requested_rate"},
            {"input_channels", qint64(m.inputChannels)},
            {"output_channels", qint64(m.outputChannels)}, {"input_device", m.inputDevice},
            {"output_device", m.outputDevice},
            {"host_api_type", m.hostApiType < 0 ? QJsonValue() : QJsonValue(m.hostApiType)},
            {"driver_frames", m.driverFrames ? QJsonValue(int(m.driverFrames)) : QJsonValue()},
            {"driver_input_latency_samples", m.driverInputLatency < 0 ? QJsonValue() : QJsonValue(m.driverInputLatency)},
            {"driver_output_latency_samples", m.driverOutputLatency < 0 ? QJsonValue() : QJsonValue(m.driverOutputLatency)},
            {"thread", qint64(m.thread)}, {"configuration_valid", m.configurationValid},
            {"pointers_alias", m.pointersAlias}, {"silence_experiment", m.experiment},
            {"input_address", QString::number(m.inputAddress, 16)},
            {"output_address", QString::number(m.outputAddress, 16)},
            {"owner_address", QString::number(m.ownerAddress, 16)},
            {"original_ns", QString::number(c.originalNanoseconds)},
            {"callback_ns", QString::number(c.callbackNanoseconds)},
            {"original_result", c.callbackResult}, {"capture", sampleJson(record.capture)},
            {"post_original", sampleJson(record.postOriginal)}});
    }
    const auto distribution = [](std::vector<double> values) {
        if (values.empty()) return QJsonObject{};
        std::sort(values.begin(), values.end());
        return QJsonObject{{"p50_ns", values[(values.size() - 1) / 2]},
            {"p95_ns", values[static_cast<std::size_t>(std::ceil(values.size() * 0.95)) - 1]},
            {"max_ns", values.back()}};
    };
    QJsonArray listenerRecords;
    for (std::size_t index = 0; index < g_listenerProbe.claimed(); ++index) {
        Record record;
        if (!g_listenerProbe.snapshot(index, record)) continue;
        const auto &extra = g_listenerProbeExtra[index]; // Covered by recorder release/acquire.
        const auto &m = record.metadata;
        QJsonArray before, sink;
        for (std::size_t i = 0; i < (std::min)(m.frames, kSampleFrames) * m.outputChannels; ++i) {
            before.append(extra.outputBefore[i]);
            if (extra.sinkApplied) sink.append(extra.sinkOutput[i]);
        }
        listenerRecords.append(QJsonObject{{"index", qint64(index)},
            {"sequence", QString::number(m.sequence)}, {"callback_sequence", QString::number(m.parentSequence)},
            {"timestamp_ns", QString::number(m.timestampNanoseconds)}, {"frames", qint64(m.frames)},
            {"input_channels", qint64(m.inputChannels)}, {"output_channels", qint64(m.outputChannels)},
            {"internal_sample_rate", m.sampleRate}, {"thread", qint64(m.thread)},
            {"unit_address", QString::number(m.ownerAddress, 16)}, {"state_address", QString::number(extra.state, 16)},
            {"unit_enabled_at_entry", extra.enabled}, {"state_pointer_stable", extra.stateStable},
            {"input_peak_before", extra.inputPeakBefore}, {"input_peak_after", extra.inputPeakAfter},
            {"output_peak_after", extra.outputPeakAfter}, {"peak_nonfinite_mask", qint64(extra.peakNonFiniteMask)},
            {"output_before", before}, {"output_before_nonfinite_mask", qint64(extra.outputBeforeNonFiniteMask)},
            {"sink_applied", extra.sinkApplied}, {"sink_busy", extra.sinkBusy},
            {"sink_output", sink}, {"sink_nonfinite_mask", qint64(extra.sinkNonFiniteMask)},
            {"input", sampleJson(record.capture)}, {"output_after", sampleJson(record.postOriginal)},
            {"original_ns", QString::number(record.completion.originalNanoseconds)},
            {"observed_ns", QString::number(record.completion.callbackNanoseconds)},
            {"returned_full_frames", record.completion.callbackResult == 0}});
    }
    const bool listenerComplete = !g_listenerProbeRequested ||
        (g_listenerProbeInstalled && listenerRecords.size() == int(kCapacity));
    const auto pcm = exportInputPcmProbe(actualAsioRate, asioRateResult);
    const bool pcmComplete = !g_inputPcmProbe.enabled() || pcm.value("complete").toBool();
    const auto drain = drainProbeSnapshot();
    const bool drainComplete = (!g_drainProbeRequested && !g_overlayProbeRequested) || drain.value("complete").toBool();
    return {{"schema", 1}, {"scope", "P13-0_experiment"}, {"acceptance", "not_evaluated"},
        {"input_monitor", inputMonitorSnapshot()},
        {"monitor_latency_mapping_valid", g_monitorLatencyMappingValid.load()},
        {"enabled", g_inputProbe.enabled()}, {"capacity", int(kCapacity)},
        {"claimed", qint64(g_inputProbe.claimed())}, {"published", records.size()},
        {"complete", records.size() == int(kCapacity) && listenerComplete && pcmComplete && drainComplete}, {"silence_requested", g_inputProbeSilence},
        {"drain_probe", drain},
        {"stream_lifecycle_probe", asioprobe::snapshot()},
        {"pcm_probe", QJsonObject{{"enabled", g_inputPcmProbe.enabled()}, {"complete", pcm.value("complete")},
            {"manifest", "p13-pcm.json"}, {"claimed", pcm.value("claimed")},
            {"published", pcm.value("published")}, {"error", pcm.value("error")}}},
        {"listener_probe", QJsonObject{{"requested", g_listenerProbeRequested},
            {"sink_requested", g_listenerSinkRequested},
            {"installed", g_listenerProbeInstalled}, {"published", listenerRecords.size()},
            {"complete", listenerRecords.size() == int(kCapacity)}, {"records", listenerRecords},
            {"scope", "bounded_experiment; output_before/after are caller buffers; optional sink diverts only listener output for recorded calls; native DSP and meters still run; shared SRC/ring history is not cleared; peak getters can reset concurrently"}}},
        {"actual_asio_sample_rate", actualAsioRate > 0 ? QJsonValue(actualAsioRate) : QJsonValue()},
        {"actual_asio_sample_rate_query_result", asioRateResult},
        {"actual_asio_sample_rate_query_source", "ASIOGetSampleRate_thunk_0x6A540_ASIOError"},
        {"actual_rate_observation_scope", "current_driver_control_thread_query_not_atomic_with_records"},
        {"callback_timing", distribution(std::move(callbackTimes))},
        {"original_timing", distribution(std::move(originalTimes))},
        {"conditional_callback_budget_exceedances", actualAsioRate > 0 ? QJsonValue(deadlineMisses) : QJsonValue()},
        {"callback_budget_scope", "current_control_rate_assumed_constant_during_window; not_complete_device_deadline"},
        {"callbacks_with_status_flags", statusFlags},
        {"timing_includes_probe_overhead", true},
        {"timing_excludes_recorder_publication", true},
        {"notes", "Requested stream rate can differ from actual ASIO rate. Driver frames are read independently; generation is known only for bound lifecycle proxies. Short samples are not full PCM or physical loopback latency evidence."},
        {"records", records}};
}
#endif

void prepareIndependentInputBackend(state::InputMonitorMode mode) {
    if (mode != state::InputMonitorMode::LowLatencyOverlay) return;
    if (!g_runtime.stream.ready()) prepare(g_verification, true);
    if (g_initial.hostSupported && g_runtime.stream.ready()) {
        if (!g_listenerProbeInstalled) g_listenerProbeInstalled = installListenerProbe();
        // The ordinary host reset preserves its device configuration and
        // obtains a lifecycle-owned stream. No second stream is opened.
        if (asioprobe::install(g_runtime.audioModule)) {
            const auto stream = asioprobe::currentStream();
            if (!stream.bound && stream.binding != asioprobe::BindingState::HostLimited)
                asioprobe::requestNativeReset();
        }
    }
}

bool requestInputVst3Selection(const std::vector<Vst3SelectionEntry> &selection,
                                std::string *error) noexcept {
    if (error) error->clear();
    if (!onQtThread() || selection.size() > SelectionSlot::kMaxEffects) {
        if (error) *error = "input_selection_invalid";
        return false;
    }
    state::InputMonitorMode mode;
    {
        std::lock_guard<std::mutex> requestLock(g_runtime.selectionRequestMutex);
        mode = g_runtime.pendingInputSettings.mode;
    }
    prepareIndependentInputBackend(mode);
    startSelectionWorker();
    {
        std::lock_guard<std::mutex> lock(g_runtime.selectionRequestMutex);
        g_runtime.pendingInputSelection = selection;
        g_runtime.inputSelectionRequestPending = true;
        ++g_runtime.inputRequestGeneration;
    }
    wakeSelectionWorker();
    return true;
}

bool requestInputMonitorSettings(const state::InputMonitorSettings &settings,
                                  std::string *error) noexcept {
    if (error) error->clear();
    if (!onQtThread()) { if (error) *error = "input_control_thread_required"; return false; }
    QJsonObject chain;
    QString detail;
    if (!state::loadChain(chain) || !state::setInputMonitorSettings(chain, settings, &detail)) {
        if (error) *error = detail.isEmpty() ? "input_settings_load_failed" : detail.toStdString();
        return false;
    }
    prepareIndependentInputBackend(settings.mode);
    if (!state::writeChain(chain)) { if (error) *error = "input_settings_save_failed"; return false; }
    startSelectionWorker();
    {
        std::lock_guard<std::mutex> lock(g_runtime.selectionRequestMutex);
        g_runtime.pendingInputSettings = settings;
        g_runtime.inputSelectionRequestPending = true;
        ++g_runtime.inputRequestGeneration;
    }
    wakeSelectionWorker();
    return true;
}

void setNativeInputState(bool known, bool enabled) noexcept {
    const bool wasEnabled = g_inputMonitor.nativeListenerEnabled.exchange(known && enabled, std::memory_order_acq_rel);
    const bool wasKnown = g_inputMonitor.nativeListenerKnown.exchange(known, std::memory_order_acq_rel);
    if (wasEnabled == (known && enabled) && wasKnown == known) return;
    g_inputMonitor.statusChanged.store(true, std::memory_order_release);
    if (const auto handle = g_inputMonitor.event.load(std::memory_order_acquire)) SetEvent(handle);
}

bool activeInputVst3States(std::vector<Vst3SelectionEntry> &result) noexcept {
    result.clear();
    std::unique_lock<std::mutex> lock(g_runtime.selectionMutex, std::try_to_lock);
    if (!lock.owns_lock()) return false;
    std::lock_guard<std::mutex> requestLock(g_runtime.selectionRequestMutex);
    if (g_runtime.inputSelectionRequestPending) return false;
    // Selection readiness is independent of the host's monitoring switch.
    // An Off monitor can retain prepared processors and their editor state.
    if (g_inputMonitor.retained < 0) return true;
    const auto &slot = g_inputMonitor.monitorSlots[g_inputMonitor.retained].selection;
    for (std::size_t i = 0; i < slot.count; ++i)
        if (slot.effects[i] && containsIdentity(g_inputMonitor.desired, slot.effects[i]->identity))
            result.push_back({slot.effects[i]->identity.module, slot.effects[i]->identity.classId});
    return true;
}

QJsonObject inputMonitorSnapshot() {
    std::unique_lock<std::mutex> lock(g_runtime.selectionMutex, std::try_to_lock);
    if (!lock.owns_lock()) return {{"state", "preparing"}};
    state::InputMonitorSettings requested;
    bool pending = false;
    {
        std::lock_guard<std::mutex> requestLock(g_runtime.selectionRequestMutex);
        requested = g_runtime.pendingInputSettings;
        pending = g_runtime.inputSelectionRequestPending;
    }
    const char *modes[]{"off", "legacy", "low_latency_overlay"};
    const char *phases[]{"off", "legacy", "preparing", "draining", "active", "muted", "host_limited"};
    const auto callbackPhase = g_inputMonitor.callbackPhase.load();
    const int reportedPhase = callbackPhase != 0 && input::MonitorExchange::versionOf(callbackPhase) == g_inputMonitor.exchange.version()
        ? int(callbackPhase & 7) : g_inputMonitor.phase.load();
    const auto phase = !g_inputMonitor.exchange.suppressed() &&
        requested.mode != state::InputMonitorMode::LowLatencyOverlay
        ? (requested.mode == state::InputMonitorMode::Legacy ? 1 : 0) : reportedPhase;
    // Measurements belong to the callback configuration that produced them.
    // A retained slot alone does not validate a new/unsupported stream.
    const auto configurationToken = g_inputMonitor.configurationToken.load(std::memory_order_acquire);
    const bool configurationValidated = !pending && configurationToken != 0 &&
        g_inputMonitor.exchange.current(configurationToken) && phase >= 3 && phase <= 5;
    const QString details[]{QStringLiteral("输入扩展已关闭"), QStringLiteral("兼容输入路由"),
        QStringLiteral("正在准备输入插件"), QStringLiteral("正在排空原生监听缓存"),
        QStringLiteral("独立输入监听已生效"), QStringLiteral("输入监听已静音；GP 播放继续"),
        QStringLiteral("当前宿主输入边界尚未通过验证")};
    std::uint64_t latency = 0;
    const int index = g_inputMonitor.retained;
    if (index >= 0)
        for (const auto &effect : g_inputMonitor.monitorSlots[index].selection.effects)
            if (effect && effect->processor) latency += effect->processor->getLatencySamples();
    const bool hostWaiting = requested.mode == state::InputMonitorMode::LowLatencyOverlay &&
        (!g_inputMonitor.nativeListenerKnown.load() || !g_inputMonitor.nativeListenerEnabled.load());
    return {{"state", hostWaiting ? "waiting_for_input" : pending ? "preparing" : phases[phase]},
        {"native_listener_known", g_inputMonitor.nativeListenerKnown.load()},
        {"native_listener_enabled", g_inputMonitor.nativeListenerEnabled.load()},
        {"dry_monitoring", g_inputMonitor.desired.empty()},
        {"mode", modes[static_cast<int>(requested.mode)]}, {"gain", requested.gain},
        {"detail", details[phase]}, {"error", QString::fromStdString(g_inputMonitor.error)},
        {"configuration_validated", configurationValidated},
        {"sample_rate", configurationValidated ? g_inputMonitor.actualRate.load() : 0},
        {"buffer_frames", configurationValidated ? qint64(g_inputMonitor.frames.load()) : 0},
        {"driver_buffer_frames", configurationValidated ? qint64(g_inputMonitor.driverFrames.load()) : 0},
        {"process_frames", configurationValidated ? qint64(g_inputMonitor.processFrames.load()) : 0},
        {"input_channels", configurationValidated ? qint64(g_inputMonitor.channels.load()) : 0},
        {"prepared_capacity", qint64(portaudio::kMaxFrames)},
        {"plugin_latency_samples", QString::number(latency)},
        {"input_native_effect_bypass", g_inputMonitor.nativeSuppressed.load()},
        {"processed_blocks", QString::number(g_inputMonitor.blocks.load())},
        {"callback_fault_stage", g_inputMonitor.callbackFault.load()},
        {"listener_fault_flags", g_inputMonitor.listenerFaultFlags.load()},
        {"listener_returned_frames", qint64(g_inputMonitor.listenerReturnedFrames.load())},
        {"listener_requested_frames", qint64(g_inputMonitor.listenerRequestedFrames.load())},
        {"fault_listener_calls", int(g_inputMonitor.faultListenerCalls.load())},
        {"fault_src_calls", int(g_inputMonitor.faultSrcCalls.load())},
        {"error_blocks", QString::number(g_inputMonitor.errors.load())},
        {"status_flag_blocks", QString::number(g_inputMonitor.statusBlocks.load())},
        {"configuration_rejected_blocks", QString::number(g_inputMonitor.configurationRejectedBlocks.load())},
        {"clipped_blocks", QString::number(g_inputMonitor.clipped.load())}};
}

std::vector<Vst3SelectionEntry> captureInputVst3States() {
    std::unique_lock<std::recursive_mutex> editorLock(g_runtime.editorMutex, std::try_to_lock);
    if (!editorLock.owns_lock()) return {};
    std::unique_lock<std::mutex> lock(g_runtime.selectionMutex, std::try_to_lock);
    if (!lock.owns_lock() || g_inputMonitor.retained < 0) return {};
    std::vector<Vst3SelectionEntry> result;
    auto &slot = g_inputMonitor.monitorSlots[g_inputMonitor.retained].selection;
    for (std::size_t i = 0; i < slot.count; ++i) result.push_back(slot.effects[i]->captureState());
    return result;
}

bool openInputVst3Editor(const Vst3SelectionEntry &entry, void *parentWindow) noexcept {
    if (!parentWindow || g_inEditorCallback || !onQtThread()) return false;
    std::unique_lock<std::recursive_mutex> editorLock(g_runtime.editorMutex, std::try_to_lock);
    if (!editorLock.owns_lock()) return false;
    std::unique_lock<std::mutex> lock(g_runtime.selectionMutex, std::try_to_lock);
    if (!lock.owns_lock() || g_inputMonitor.retained < 0) return false;
    auto &slot = g_inputMonitor.monitorSlots[g_inputMonitor.retained].selection;
    for (std::size_t i = 0; i < slot.count; ++i) {
        const auto effect = slot.effects[i];
        if (effect->identity.module != entry.module || effect->identity.classId != entry.classId) continue;
        lock.unlock();
        EditorCallbackScope callbackScope;
        closeVst3Editors();
        g_runtime.editorRequestGeneration.fetch_add(1);
        g_runtime.editorIdentity = "input\n" + entry.module + "\n" + entry.classId;
        const bool opened = effect->openEditor(static_cast<HWND>(parentWindow));
        g_runtime.editorStage.store(effect->editorStage.load());
        g_runtime.editorResultCode.store(effect->editorResultCode.load());
        g_runtime.editorError = effect->getEditorError();
        if (opened) std::atomic_store(&g_openEditorEffect, effect);
        return opened;
    }
    return false;
}

bool setVst3Selection(const std::vector<Vst3SelectionEntry> &selection, std::string *error) noexcept {
    std::lock_guard<std::recursive_mutex> editorLock(g_runtime.editorMutex);
    if (error) error->clear();
    g_runtime.selectionRequestId.fetch_add(1, std::memory_order_acq_rel);
    g_runtime.selectionQueuedNanoseconds.store(steadyNanoseconds(), std::memory_order_release);
    g_runtime.selectionStatus.store(2, std::memory_order_release);
    // P7 controls are called on the Qt control thread. Install before taking
    // selectionMutex because prepare() also locks it to restore a saved chain.
    if (!g_runtime.master.ready() && !selection.empty()) {
        const auto prepared = prepare(g_verification, true);
        if (!prepared.installed) {
            if (error) *error = prepared.reason;
            return false;
        }
    }
    std::lock_guard<std::mutex> lock(g_runtime.selectionMutex);
    if (!g_runtime.master.ready() && selection.empty()) {
        g_runtime.requestedSelection.clear();
        return true;
    }
    if (!configureSelectedChain(selection, error)) {
        g_runtime.selectionStatus.store(4, std::memory_order_release);
        return false;
    }
    g_runtime.requestedSelection = selection;
    g_runtime.selectionPreparedNanoseconds.store(steadyNanoseconds(), std::memory_order_release);
    g_runtime.selectionCommittedNanoseconds.store(steadyNanoseconds(), std::memory_order_release);
    g_runtime.audioGeneration.fetch_add(1, std::memory_order_acq_rel);
    g_runtime.selectionAppliedGeneration.fetch_add(1, std::memory_order_acq_rel);
    g_runtime.selectionStatus.store(3, std::memory_order_release);
    return true;
}

bool setGlobalVst3Selection(const std::vector<Vst3SelectionEntry> &selection,
                            std::string *error) noexcept {
    return setVst3Selection(selection, error);
}

bool requestGlobalVst3Selection(const std::vector<Vst3SelectionEntry> &selection,
                                std::string *error) noexcept {
    if (error) error->clear();
    if (selection.size() > SelectionSlot::kMaxEffects) {
        if (error) *error = "runtime_vst3_chain_full";
        return false;
    }
    if (!selection.empty() && !g_runtime.master.ready()) {
        updateAudioLayerState();
        const auto prepared = prepare(g_verification, true);
        if (!prepared.installed) { if (error) *error = prepared.reason; return false; }
    }
    startSelectionWorker();
    {
        std::lock_guard<std::mutex> lock(g_runtime.selectionRequestMutex);
        g_runtime.pendingSelection = selection;
        g_runtime.selectionRequestPending = true;
        ++g_runtime.selectionRequestGeneration;
        g_runtime.selectionRequestId.fetch_add(1, std::memory_order_acq_rel);
        g_runtime.selectionQueuedNanoseconds.store(steadyNanoseconds(), std::memory_order_release);
        g_runtime.selectionStatus.store(1, std::memory_order_release);
    }
    // Bypass is an audio atomic and must not wait for the worker to load a
    // factory. The old slot remains alive for warm reuse while the next
    // callback observes bypass immediately.
    if (selection.empty()) {
        g_runtime.chain.setBypassed(true);
        g_runtime.inputChain.setBypassed(true);
        g_runtime.inputRouter.setBypassed(true);
    }
    wakeSelectionWorker();
    return true;
}

void saveVst3States() {
    std::unique_lock<std::recursive_mutex> editorLock(g_runtime.editorMutex, std::try_to_lock);
    if (!editorLock.owns_lock()) return;
    std::unique_lock<std::mutex> lock(g_runtime.selectionMutex, std::try_to_lock);
    if (!lock.owns_lock()) return;
    if (g_inputMonitor.retained >= 0) {
        std::vector<Vst3SelectionEntry> entries;
        auto &slot = g_inputMonitor.monitorSlots[g_inputMonitor.retained].selection;
        for (std::size_t i = 0; i < slot.count; ++i) entries.push_back(slot.effects[i]->captureState());
        persistRuntimeEntries(entries, state::ScopeKind::Input);
    }
    const auto active = g_runtime.chain.snapshot().activeSlot;
    if (active >= 0 && g_runtime.selectionMode.load(std::memory_order_acquire)) {
        std::vector<Vst3SelectionEntry> entries;
        const auto &slot = g_runtime.selectionSlots[active];
        for (std::size_t index = 0; index < slot.count; ++index)
            entries.push_back(slot.effects[index]->captureState());
        persistRuntimeEntries(entries, state::ScopeKind::Global);
    }
    const TrackDispatchUpdate update;
    for (auto &runtime : g_runtime.trackRuntimes)
        if (!runtime.trackKey.empty()) saveTrackRuntime(runtime);
}

void setVst3Catalog(const QJsonArray &catalog) {
    if (!g_initial.hostSupported) return;
    std::vector<Vst3SelectionEntry> entries;
    for (const auto &value : catalog) {
        const auto entry = value.toObject();
        const auto status = entry.value("recognition_status").toString();
        if ((!status.isEmpty() && status != "ready") || entry.value("class_id").toString().isEmpty() ||
            !(entry.value("identified").toBool() || entry.value("compatible").toBool())) continue;
        entries.push_back({entry.value("module").toString().toStdString(), entry.value("class_id").toString().toStdString()});
    }
    {
        std::lock_guard<std::mutex> lock(g_runtime.catalogMutex);
        if (g_runtime.catalogReady && sameSelection(entries, g_runtime.catalogEntries)) return;
    }
    {
        std::lock_guard<std::mutex> lock(g_runtime.catalogMutex);
        g_runtime.catalogEntries = entries;
        g_runtime.catalogReady = true;
    }
    // Catalog delivery updates metadata; background preloading and explicit
    // activation are both serialized by the selection worker.
    if (!g_inputMonitor.intentLoaded) {
        QJsonObject chain;
        if (state::loadChain(chain)) {
            g_inputMonitor.intentLoaded = true;
            state::InputMonitorSettings settings;
            state::readInputMonitorSettings(chain, settings);
            const auto selection = enabledSelectionFromScope(state::ScopeKind::Input);
            {
                std::lock_guard<std::mutex> lock(g_runtime.selectionRequestMutex);
                if (g_runtime.inputRequestGeneration == 0) {
                    g_runtime.pendingInputSettings = settings;
                    g_runtime.pendingInputSelection = selection;
                }
            }
            if (chain.value("input").toObject().contains("monitor_mode"))
                requestInputMonitorSettings(settings);
        }
    }
    refreshTrackContextImpl();
}

void preloadSavedSelections() noexcept {
    if (!onQtThread() || !g_initial.hostSupported || g_qtDispatchStopping.load(std::memory_order_acquire) ||
        qEnvironmentVariable("GPVST3_ENABLE_P2_HOOK") == "0" || !state::pluginEnabled()) return;
    QJsonObject chain;
    if (!state::loadChain(chain)) return;
    const auto bindings = gp_audio::snapshot();
    // A catalog is metadata only. Without an active document there is no
    // project graph and therefore no scope whose processors may be prepared.
    if (bindings.empty() || !std::any_of(bindings.begin(), bindings.end(),
            [](const gp_audio::Binding &binding) {
                return binding.activeDocument && !binding.documentId.empty();
            })) {
        std::lock_guard<std::mutex> requestLock(g_runtime.selectionRequestMutex);
        g_runtime.pendingPreloads.clear();
        g_runtime.preloadAttempts.clear();
        g_runtime.globalPreloaded = false;
        g_runtime.inputPreloaded = false;
        return;
    }
    std::vector<Vst3SelectionEntry> catalog;
    {
        std::lock_guard<std::mutex> lock(g_runtime.catalogMutex);
        if (!g_runtime.catalogReady) return;
        catalog = g_runtime.catalogEntries;
    }
    // Only entries explicitly enabled in the opened project are prepared.
    // Disabled catalog items remain metadata and are instantiated only after
    // an explicit enable request. This keeps sidecar intent independent from
    // runtime state and avoids hidden full-catalog construction.
    const auto selections = [&](const QJsonArray &effects) {
        std::vector<Vst3SelectionEntry> entries;
        for (const auto &value : effects) {
            const auto effect = value.toObject();
            if (!effect.value("enabled").toBool()) continue;
            const auto module = effect.value("module").toString();
            const auto classId = effect.value("class_id").toString();
            if (module.isEmpty() || classId.isEmpty()) continue;
            const auto decode = [&](const char *field) {
                const auto bytes = QByteArray::fromBase64(effect.value(field).toString().toLatin1());
                return std::vector<unsigned char>(bytes.cbegin(), bytes.cend());
            };
            entries.push_back({module.toStdString(), classId.toStdString(),
                               decode("component_state"), decode("controller_state")});
        }
        // Drop stale sidecar entries that are absent from the current catalog;
        // an unavailable module must not block preparation of valid scopes.
        entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const auto &entry) {
            return std::none_of(catalog.begin(), catalog.end(), [&](const auto &known) {
                return known.module == entry.module && known.classId == entry.classId;
            });
        }), entries.end());
        return entries;
    };
    std::unordered_map<std::string, std::vector<Vst3SelectionEntry>> desired;
    if (!catalog.empty()) {
        const auto global = selections(state::scopeEffects(chain, state::ScopeKind::Global));
        if (!global.empty()) desired[""] = global;
    }
    for (const auto &binding : bindings) {
        if (!binding.activeDocument || binding.trackKey.empty() || catalog.empty()) continue;
        const auto track = selections(state::scopeEffects(chain, state::ScopeKind::Track,
            QString::fromStdString(binding.scoreKey), QString::fromStdString(binding.trackKey)));
        if (!track.empty()) desired[binding.trackKey] = track;
    }
    if (desired.empty()) {
        std::lock_guard<std::mutex> requestLock(g_runtime.selectionRequestMutex);
        g_runtime.pendingPreloads.clear();
        g_runtime.preloadAttempts.clear();
        g_runtime.globalPreloaded = false;
        g_runtime.inputPreloaded = false;
        return;
    }
    // Install dormant dispatch before the worker starts. Later UI requests
    // never block in prepare() behind a preload's call into Qt.
    if (!g_runtime.master.ready() && !prepare(g_verification, true).installed) return;
    updateAudioLayerState();
    const double rate = callbackSampleRate();
    startSelectionWorker();
    {
        std::lock_guard<std::mutex> lock(g_runtime.selectionRequestMutex);
        for (auto it = g_runtime.preloadAttempts.begin(); it != g_runtime.preloadAttempts.end();) {
            if (desired.find(it->first) == desired.end()) {
                g_runtime.pendingPreloads.erase(it->first);
                it = g_runtime.preloadAttempts.erase(it);
            } else ++it;
        }
        for (auto &item : desired) {
            const auto previous = g_runtime.preloadAttempts.find(item.first);
            if (previous != g_runtime.preloadAttempts.end() && previous->second.rate == rate &&
                sameSelection(previous->second.selection, item.second)) continue;
            const auto generation = previous == g_runtime.preloadAttempts.end() ? 1 : previous->second.generation + 1;
            Runtime::PreloadRequest request{std::move(item.second), rate, generation};
            g_runtime.preloadAttempts[item.first] = request;
            g_runtime.pendingPreloads[item.first] = std::move(request);
        }
    }
    wakeSelectionWorker();
}

bool consumeSelectionStateChanges() noexcept {
    if (vst3SelectionPending()) return false;
    return g_selectionStateChanged.exchange(false, std::memory_order_acq_rel);
}

bool vst3SelectionPending() noexcept {
    std::lock_guard<std::mutex> lock(g_runtime.selectionRequestMutex);
    return g_runtime.selectionWorkerBusy.load(std::memory_order_acquire) ||
        g_runtime.selectionRequestPending || g_runtime.inputSelectionRequestPending || !g_runtime.pendingTrackSelections.empty() ||
        g_runtime.trackContextRequestPending || !g_runtime.pendingPreloads.empty();
}

bool consumeTrackTopologyInvalidation() noexcept {
    return g_trackTopologyInvalidated.exchange(false, std::memory_order_acq_rel);
}

bool setTrackVst3Selection(const std::string &trackKey,
                           const std::vector<Vst3SelectionEntry> &selection,
                           std::string *error) noexcept {
    // Compatibility alias: all callers receive queue acceptance immediately.
    // Completion and failure are published by the same scope worker as UI.
    return requestTrackVst3Selection(trackKey, selection, error);
}
bool requestTrackVst3SelectionAtGeneration(const std::string &trackKey,
                                            std::uint64_t requestedSelectionGeneration,
                                            const std::vector<Vst3SelectionEntry> &selection,
                                            std::string *error) noexcept {
    if (error) error->clear();
    if (trackKey.empty()) {
        if (error) *error = "track_scope_unresolved";
        return false;
    }
    if (selection.size() > SelectionSlot::kMaxEffects) {
        if (error) *error = "runtime_vst3_chain_full";
        return false;
    }
    const auto currentGeneration = state::runtimeSelectionGeneration();
    if (requestedSelectionGeneration != 0 && (requestedSelectionGeneration != currentGeneration ||
        trackKey != state::currentTrackKey().toStdString())) {
        if (error) *error = "stale_selection_generation";
        return false;
    }
    // Install the host hook before handing plug-in initialization to the worker.
    if (!selection.empty() && !g_runtime.dsp.ready()) {
        const auto prepared = prepare(g_verification, true);
        if (!prepared.installed) {
            if (error) *error = prepared.reason;
            return false;
        }
    }
    // Track runtimes already have an independent fixed table. Queueing the
    // request through the same worker keeps processor construction off Qt;
    // the current binding is validated before accepting it.
    const auto bindings = gp_audio::snapshot();
    if (std::none_of(bindings.begin(), bindings.end(), [&](const gp_audio::Binding &binding) {
            return binding.chain && binding.activeDocument && binding.trackKey == trackKey;
        })) {
        if (error) *error = "track_scope_unresolved";
        return false;
    }
    startSelectionWorker();
    {
        std::lock_guard<std::mutex> lock(g_runtime.selectionRequestMutex);
        g_runtime.pendingTrackSelections[trackKey] = selection;
        g_runtime.pendingTrackGenerations[trackKey] += 1;
        // The accepted binding may be newer than the worker's runtime table.
        // Publish that exact snapshot before consuming the selection.
        g_runtime.pendingBindings = bindings;
        g_runtime.pendingDiscovered = bindings.size();
        g_runtime.trackContextRequestPending = true;
        g_runtime.selectionRequestId.fetch_add(1, std::memory_order_acq_rel);
        g_runtime.selectionQueuedNanoseconds.store(steadyNanoseconds(), std::memory_order_release);
        g_runtime.selectionStatus.store(1, std::memory_order_release);
    }
    if (selection.empty()) {
        const auto keyHash = stableTrackKeyHash(trackKey);
        for (auto &runtime : g_runtime.trackRuntimes)
            if (runtime.keyHash.load(std::memory_order_acquire) == keyHash)
                runtime.bypassRequested.store(true, std::memory_order_release);
    }
    wakeSelectionWorker();
    return true;
}

bool requestTrackVst3Selection(const std::string &trackKey,
                               const std::vector<Vst3SelectionEntry> &selection,
                               std::string *error) noexcept {
    return requestTrackVst3SelectionAtGeneration(trackKey, 0,
                                                  selection, error);
}

std::vector<Vst3SelectionEntry> captureGlobalVst3States() {
    return captureVst3States();
}

std::vector<Vst3SelectionEntry> captureTrackVst3States(const std::string &trackKey) {
    std::unique_lock<std::recursive_mutex> editorLock(g_runtime.editorMutex, std::try_to_lock);
    if (!editorLock.owns_lock()) return {};
    std::unique_lock<std::mutex> lock(g_runtime.selectionMutex, std::try_to_lock);
    if (!lock.owns_lock()) return {};
    for (auto &runtime : g_runtime.trackRuntimes) {
        if (runtime.trackKey != trackKey) continue;
        const auto snapshot = runtime.chain.snapshot();
        const int active = snapshot.activeSlot;
        if (active < 0 || snapshot.bypassed || snapshot.faulted ||
            runtime.bypassRequested.load(std::memory_order_acquire) ||
            !runtime.configured.load(std::memory_order_acquire) ||
            runtime.count.load(std::memory_order_acquire) == 0)
            return {};
        std::vector<Vst3SelectionEntry> result;
        auto &slot = runtime.trackSlots[active];
        for (std::size_t index = 0; index < slot.count; ++index)
            result.push_back(slot.effects[index]->captureState());
        runtime.requested = result;
        g_runtime.requestedTrackSelections[trackKey] = result;
        return result;
    }
    // This API is used by the UI to display the live chain.  Do not fall back
    // to the requested sidecar selection: that would make a failed or pending
    // track request appear enabled before a runtime slot is active.
    return {};
}

bool activeTrackVst3States(const std::string &trackKey,
                           std::vector<Vst3SelectionEntry> &result) noexcept {
    result.clear();
    std::unique_lock<std::recursive_mutex> editorLock(g_runtime.editorMutex, std::try_to_lock);
    if (!editorLock.owns_lock()) return false;
    std::unique_lock<std::mutex> lock(g_runtime.selectionMutex, std::try_to_lock);
    if (!lock.owns_lock()) return false;
    for (const auto &runtime : g_runtime.trackRuntimes) {
        if (runtime.trackKey != trackKey) continue;
        const auto snapshot = runtime.chain.snapshot();
        const int active = snapshot.activeSlot;
        if (active < 0 || snapshot.bypassed || snapshot.faulted ||
            runtime.bypassRequested.load(std::memory_order_acquire) ||
            !runtime.configured.load(std::memory_order_acquire) ||
            runtime.count.load(std::memory_order_acquire) == 0)
            return true;
        const auto &slot = runtime.trackSlots[active];
        for (std::size_t index = 0; index < slot.count; ++index)
            if (slot.effects[index]) result.push_back({slot.effects[index]->identity.module,
                                                       slot.effects[index]->identity.classId});
        return true;
    }
    return true;
}

std::vector<Vst3SelectionEntry> captureVst3States() {
    std::unique_lock<std::recursive_mutex> editorLock(g_runtime.editorMutex, std::try_to_lock);
    if (!editorLock.owns_lock()) return {};
    std::unique_lock<std::mutex> lock(g_runtime.selectionMutex, std::try_to_lock);
    if (!lock.owns_lock()) return {};
    std::vector<Vst3SelectionEntry> result;
    const auto active = g_runtime.chain.snapshot().activeSlot;
    if (active < 0 || !g_runtime.selectionMode.load(std::memory_order_acquire)) return result;
    auto &slot = g_runtime.selectionSlots[active];
    for (std::size_t i = 0; i < slot.count; ++i) result.push_back(slot.effects[i]->captureState());
    return result;
}

bool openVst3Editor(const Vst3SelectionEntry &entry, void *parentWindow) noexcept {
    if (g_inEditorCallback || !onQtThread()) return false;
    std::unique_lock<std::recursive_mutex> editorLock(g_runtime.editorMutex, std::try_to_lock);
    if (!editorLock.owns_lock()) {
        g_runtime.editorStage.store(static_cast<int>(EditorStage::BusyWait), std::memory_order_release);
        return false;
    }
    std::unique_lock<std::mutex> lock(g_runtime.selectionMutex, std::try_to_lock);
    if (!lock.owns_lock()) return false;
    if (!parentWindow || !g_runtime.selectionMode.load(std::memory_order_acquire)) return false;
    const int active = g_runtime.chain.snapshot().activeSlot;
    if (active < 0 || active >= 2) return false;
    auto &slot = g_runtime.selectionSlots[active];
    for (std::size_t index = 0; index < slot.count; ++index) {
        if (slot.effects[index]->name.empty()) continue;
        // The immutable selection is held beside each slot; matching the
        // module/class pair is performed against the requested list because
        // RuntimeEffect deliberately stores only the loaded display name.
        if (slot.effects[index]->identity.module == entry.module &&
            slot.effects[index]->identity.classId == entry.classId) {
            const auto effect = slot.effects[index];
            lock.unlock();
            EditorCallbackScope callbackScope;
            closeVst3Editors();
            g_runtime.editorRequestGeneration.fetch_add(1, std::memory_order_acq_rel);
            g_runtime.editorIdentity = entry.module + "\n" + entry.classId;
            g_runtime.editorStage.store(static_cast<int>(EditorStage::Requested), std::memory_order_release);
            const bool opened = effect->openEditor(static_cast<HWND>(parentWindow));
            g_runtime.editorStage.store(effect->editorStage.load(std::memory_order_acquire), std::memory_order_release);
            g_runtime.editorResultCode.store(effect->editorResultCode.load(std::memory_order_acquire), std::memory_order_release);
            g_runtime.editorError = effect->getEditorError();
            if (!opened) return false;
            std::atomic_store(&g_openEditorEffect, effect);
            return true;
        }
    }
    g_runtime.editorIdentity = entry.module + "\n" + entry.classId;
    g_runtime.editorStage.store(static_cast<int>(EditorStage::Failed), std::memory_order_release);
    g_runtime.editorResultCode.store(static_cast<long>(Steinberg::kNoInterface), std::memory_order_release);
    g_runtime.editorError = "editor_instance_unavailable";
    return false;
}

bool openTrackVst3Editor(const std::string &trackKey, const Vst3SelectionEntry &entry,
                         void *parentWindow) noexcept {
    if (!parentWindow || g_inEditorCallback || !onQtThread()) return false;
    std::unique_lock<std::recursive_mutex> editorLock(g_runtime.editorMutex, std::try_to_lock);
    if (!editorLock.owns_lock()) {
        g_runtime.editorStage.store(static_cast<int>(EditorStage::BusyWait), std::memory_order_release);
        return false;
    }
    std::unique_lock<std::mutex> lock(g_runtime.selectionMutex, std::try_to_lock);
    if (!lock.owns_lock()) return false;
    for (auto &runtime : g_runtime.trackRuntimes) {
        if (runtime.trackKey != trackKey) continue;
        const int active = runtime.chain.snapshot().activeSlot;
        if (active < 0) return false;
        auto &slot = runtime.trackSlots[active];
        for (std::size_t index = 0; index < slot.count; ++index) {
            const auto effect = slot.effects[index];
            if (effect->identity.module == entry.module && effect->identity.classId == entry.classId) {
                lock.unlock();
                EditorCallbackScope callbackScope;
                closeVst3Editors();
                g_runtime.editorRequestGeneration.fetch_add(1, std::memory_order_acq_rel);
                g_runtime.editorIdentity = entry.module + "\n" + entry.classId;
                const bool opened = effect->openEditor(static_cast<HWND>(parentWindow));
                g_runtime.editorStage.store(effect->editorStage.load(std::memory_order_acquire), std::memory_order_release);
                g_runtime.editorResultCode.store(effect->editorResultCode.load(std::memory_order_acquire), std::memory_order_release);
                g_runtime.editorError = effect->getEditorError();
                if (!opened) return false;
                std::atomic_store(&g_openEditorEffect, effect);
                return true;
            }
        }
    }
    g_runtime.editorIdentity = entry.module + "\n" + entry.classId;
    g_runtime.editorStage.store(static_cast<int>(EditorStage::Failed), std::memory_order_release);
    g_runtime.editorResultCode.store(static_cast<long>(Steinberg::kNoInterface), std::memory_order_release);
    g_runtime.editorError = "editor_instance_unavailable";
    return false;
}

void scaleVst3Editor(void *host, double factor) noexcept {
    if (!onQtThread()) return;
    const auto keepAlive = std::atomic_load(&g_openEditorEffect);
    if (!keepAlive || !host || !std::isfinite(factor) || factor <= 0.0) return;
    std::unique_lock<std::recursive_mutex> editorLock(g_runtime.editorMutex, std::try_to_lock);
    if (!editorLock.owns_lock()) return;
    Steinberg::IPtr<Steinberg::IPlugView> editor;
    Steinberg::IPtr<Steinberg::IPlugFrame> plugFrame;
    {
        std::lock_guard<std::mutex> lock(keepAlive->editorThreadMutex);
        if (keepAlive->editorParent != static_cast<HWND>(host) || !keepAlive->editor) return;
        editor = keepAlive->editor;
        plugFrame = keepAlive->plugFrame;
    }
    EditorCallbackScope callbackScope;
    try {
        if (auto scale = FUnknownPtr<Steinberg::IPlugViewContentScaleSupport>(editor.get())) {
            if (!succeeded(scale->setContentScaleFactor(static_cast<float>(factor)))) return;
            ViewRect size{};
            if (succeeded(editor->getSize(&size)) && plugFrame)
                plugFrame->resizeView(editor.get(), &size);
        }
    } catch (...) {
        keepAlive->setEditorError("editor_scale_exception");
    }
}

void closeVst3Editors() noexcept {
    if (!onQtThread()) return;
    std::unique_lock<std::recursive_mutex> editorLock(g_runtime.editorMutex, std::try_to_lock);
    if (!editorLock.owns_lock()) return;
    const auto effect = std::atomic_exchange(&g_openEditorEffect, std::shared_ptr<RuntimeEffect>{});
    if (!effect) return;
    EditorCallbackScope callbackScope;
    effect->closeEditor();
    g_runtime.editorStage.store(static_cast<int>(EditorStage::Removed), std::memory_order_release);
    g_runtime.editorResultCode.store(static_cast<long>(Steinberg::kResultOk), std::memory_order_release);
    g_runtime.editorError.clear();
    if (const auto notifier = g_selectionNotifier.load(std::memory_order_acquire)) notifier();
}

bool processExternalInput(const input::CaptureView &capture,
                          const input::GeneratedView &generated,
                          const input::OutputView &output) noexcept {
    g_runtime.inputCapturePathLocated.store(true, std::memory_order_release);
    g_runtime.inputCaptureCalls.fetch_add(1, std::memory_order_relaxed);
    if (g_runtime.inputProcessing.test_and_set(std::memory_order_acquire)) return false;
    const auto completed = g_runtime.inputRouter.process(capture, generated, output).completed;
    g_runtime.inputProcessing.clear(std::memory_order_release);
    return completed;
}

bool processExternalInputInterleaved(const input::InterleavedView &view) noexcept {
    if (g_runtime.inputProcessing.test_and_set(std::memory_order_acquire)) return false;
    if (static_cast<int>(view.sampleRate) != g_runtime.inputConfiguredRate.load(std::memory_order_acquire) ||
        view.inputChannelCount != g_runtime.inputConfiguredChannels.load(std::memory_order_acquire) ||
        view.outputChannelCount != g_runtime.inputConfiguredOutputChannels.load(std::memory_order_acquire)) {
        g_runtime.inputConfigurationErrors.fetch_add(1, std::memory_order_relaxed);
        g_runtime.inputProcessing.clear(std::memory_order_release);
        return false;
    }
    const auto completed = g_runtime.inputRouter.processInterleaved(view).completed;
    g_runtime.inputProcessing.clear(std::memory_order_release);
    return completed;
}

} // namespace gpvst3::hook
