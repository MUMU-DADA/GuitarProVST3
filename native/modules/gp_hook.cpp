#include "gp_hook.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
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
#include <mutex>
#include <condition_variable>
#include <string>
#include <vector>
#include <unordered_map>
#include <thread>
#include <chrono>

#include "audio_adapter.h"
#include "effect_chain.h"
#include "gp_audio_runtime.h"
#include "state_manager.h"
#include "vst3_parameters.h"
#include "qt_ui.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include <memory>
#include "portaudio_capture_abi.h"
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
void reconfigureInputRouterIfNeeded() noexcept;
bool configureSelectedChain(const std::vector<Vst3SelectionEntry> &selection, std::string *error) noexcept;

namespace {

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
constexpr std::uint8_t kMasterPrologue[kMasterPatchBytes]{
    0x4C, 0x89, 0x44, 0x24, 0x18, 0x48, 0x89, 0x54, 0x24, 0x10, 0x48, 0x89, 0x4C, 0x24, 0x08};
constexpr std::uint8_t kDspPrologue[kDspPatchBytes]{
    0x48, 0x89, 0x5C, 0x24, 0x20, 0x48, 0x89, 0x54, 0x24, 0x10, 0x48, 0x89, 0x4C, 0x24, 0x08};
constexpr std::uint8_t kStreamPrologue[kStreamPatchBytes]{
    0x48, 0x89, 0x5C, 0x24, 0x20, 0x55, 0x56, 0x57,
    0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
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

struct RuntimeEffect;
bool succeeded(tresult result) noexcept;

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

class RuntimePlugFrame final
    : public Steinberg::U::Implements<Steinberg::U::Directly<Steinberg::IPlugFrame>> {
public:
    explicit RuntimePlugFrame(HWND hostWindow) : hostWindow_(hostWindow) {}
    Steinberg::tresult PLUGIN_API resizeView(Steinberg::IPlugView *view,
                                             ViewRect *newSize) override {
        if (!newSize || !hostWindow_) return Steinberg::kInvalidArgument;
        if (resizing_) return Steinberg::kResultTrue;
        resizing_ = true;
        const int width = (std::max)(1, newSize->getWidth());
        const int height = (std::max)(1, newSize->getHeight());
        gpvst3::ui::resizeNativeEditor(reinterpret_cast<void *>(hostWindow_), width, height);
        const bool resized = view && succeeded(view->onSize(newSize));
        resizing_ = false;
        return resized ? Steinberg::kResultTrue : Steinberg::kResultFalse;
    }

private:
    HWND hostWindow_ = nullptr;
    bool resizing_ = false;
};

bool succeeded(tresult result) noexcept {
    return result == Steinberg::kResultOk || result == Steinberg::kResultTrue;
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

    tresult PLUGIN_API isPlugInterfaceSupported(const TUID) override {
        return Steinberg::kResultTrue;
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

struct RuntimeEffect {
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
    vst3::ParameterChanges parameterChanges;
    Vst3SelectionEntry identity;
    FUnknownPtr<Steinberg::Vst::IConnectionPoint> componentConnection;
    FUnknownPtr<Steinberg::Vst::IConnectionPoint> controllerConnection;
    std::atomic<std::size_t> parameterEdits{0};
    HWND editorParent = nullptr;
    std::atomic<bool> editorAttached{false};
    std::string editorError;
    RuntimeHostApplication host;
    audio::PlanarBuffer scratch;
    std::string name;
    std::string error;
    std::atomic<bool> ready{false};
    std::atomic<bool> outputWritten{false};
    std::atomic<bool> ownerObserved{false};
    std::atomic<int> configuredRate{0};
    std::atomic<std::size_t> configuredBlock{0};
    std::atomic<unsigned int> reportedLatencySamples{0};
    bool forceError = false;
    std::atomic_flag processing = ATOMIC_FLAG_INIT;

    ~RuntimeEffect() { shutdown(); }

    void shutdown() noexcept {
        ready.store(false, std::memory_order_release);
        closeEditor();
        if (componentConnection && controllerConnection) {
            componentConnection->disconnect(controllerConnection);
            controllerConnection->disconnect(componentConnection);
        }
        componentConnection = nullptr;
        controllerConnection = nullptr;
        if (controller) controller->setComponentHandler(nullptr);
        if (controller && separateControllerInitialized) controller->terminate();
        separateControllerInitialized = false;
        componentHandler = nullptr;
        controller = nullptr;
        plugFrame = nullptr;
        parameterChanges.clear();
        editorError.clear();
        outputWritten.store(false, std::memory_order_release);
        ownerObserved.store(false, std::memory_order_release);
        configuredRate.store(0, std::memory_order_release);
        configuredBlock.store(0, std::memory_order_release);
        reportedLatencySamples.store(0, std::memory_order_release);
        if (processor) processor->setProcessing(false);
        if (component) {
            component->setActive(false);
            component->terminate();
        }
        processor = nullptr;
        component = nullptr;
        factory = nullptr;
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
        IComponent *rawComponent = nullptr;
        if (!succeeded(factory->createInstance(selected.cid, IComponent::iid,
                                               reinterpret_cast<void **>(&rawComponent))) || !rawComponent) {
            error = "runtime_vst3_component_failed";
            return false;
        }
        component = Steinberg::owned(rawComponent);
        auto *hostUnknown = static_cast<Steinberg::FUnknown *>(
            static_cast<Steinberg::Vst::IHostApplication *>(&host));
        if (auto factory3 = FUnknownPtr<Steinberg::IPluginFactory3>(factory.get()))
            factory3->setHostContext(hostUnknown);
        if (!succeeded(component->initialize(hostUnknown))) {
            error = "runtime_vst3_component_initialize_failed";
            return false;
        }
        for (int direction = Steinberg::Vst::kInput; direction <= Steinberg::Vst::kOutput; ++direction) {
            const auto count = component->getBusCount(Steinberg::Vst::kAudio, direction);
            for (Steinberg::int32 index = 0; index < count; ++index) {
                Steinberg::Vst::BusInfo bus{};
                if (succeeded(component->getBusInfo(Steinberg::Vst::kAudio, direction, index, bus)) &&
                    ((bus.flags & Steinberg::Vst::BusInfo::kDefaultActive) || index == 0))
                    component->activateBus(Steinberg::Vst::kAudio, direction, index, true);
            }
        }
        Steinberg::MemoryStream componentState;
        const bool componentStateReady = succeeded(component->getState(&componentState));
        if (componentStateReady) {
            componentState.seek(0, Steinberg::IBStream::kIBSeekSet, nullptr);
            component->setState(&componentState);
        }
        processor = FUnknownPtr<IAudioProcessor>(component.get());
        if (!processor) {
            error = "runtime_vst3_processor_missing";
            return false;
        }
        // Initialize the optional controller before activating the component.
        // This is the same lifecycle order used by the metadata probe and by
        // controllers that reject initialization after processing starts.
        TUID controllerClassId{};
        if (succeeded(component->getControllerClassId(controllerClassId))) {
            IEditController *rawController = nullptr;
            if (succeeded(factory->createInstance(controllerClassId, IEditController::iid,
                                                  reinterpret_cast<void **>(&rawController))) &&
                rawController)
                controller = Steinberg::owned(rawController);
        }
        const bool separateController = static_cast<bool>(controller);
        if (!controller) controller = FUnknownPtr<IEditController>(component.get());
        if (!controller) {
            editorError = "runtime_vst3_controller_create_failed";
        } else {
            auto handler = Steinberg::owned(new RuntimeComponentHandler(this));
            // Single-component plug-ins already initialized their controller
            // through IComponent. Reinitializing returns kResultFalse.
            const auto controllerInit = separateController ? controller->initialize(hostUnknown)
                                                           : Steinberg::kResultOk;
            separateControllerInitialized = separateController && succeeded(controllerInit);
            if (succeeded(controllerInit)) {
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
                    if (componentStateReady) {
                        componentState.seek(0, Steinberg::IBStream::kIBSeekSet, nullptr);
                        controller->setComponentState(&componentState);
                    }
                } else {
                    editorError = "runtime_vst3_component_handler_failed";
                }
            } else {
                controller = nullptr;
                editorError = "runtime_vst3_controller_initialize_failed_" +
                    std::to_string(static_cast<long>(controllerInit));
            }
        }
        if (!parameterChanges.prepare(controller.get())) {
            error = "runtime_vst3_parameter_setup_failed";
            return false;
        }
        if (saved && !restoreState(*saved)) return false;
        Steinberg::Vst::ProcessSetup setup{};
        setup.processMode = Steinberg::Vst::kRealtime;
        setup.symbolicSampleSize = Steinberg::Vst::kSample32;
        setup.maxSamplesPerBlock = static_cast<Steinberg::int32>(maxSamplesPerBlock);
        setup.sampleRate = sampleRate;
        if (!succeeded(processor->setupProcessing(setup))) {
            error = "runtime_vst3_processing_setup_failed";
            return false;
        }
        if (!succeeded(component->setActive(true)) || !succeeded(processor->setProcessing(true)) ||
            !scratch.prepare(2, maxSamplesPerBlock)) {
            error = "runtime_vst3_processing_setup_failed";
            return false;
        }
        ready.store(true, std::memory_order_release);
        configuredRate.store(static_cast<int>(sampleRate), std::memory_order_release);
        configuredBlock.store(maxSamplesPerBlock, std::memory_order_release);
        reportedLatencySamples.store(processor->getLatencySamples(), std::memory_order_release);
        error.clear();
        return true;
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
            if (!restored(component->setState(&stream), "component")) return false;
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
            if (!restored(controller ? controller->setState(&stream) : Steinberg::kNoInterface,
                          "controller")) return false;
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
        return result;
    }

    bool queueParameter(ParamID id, ParamValue value) noexcept {
        if (!std::isfinite(value) || !parameterChanges.publish(id, value)) return false;
        parameterEdits.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    bool openEditor(HWND parentWindow) noexcept {
        if (!ready.load(std::memory_order_acquire) || !controller || !parentWindow) {
            editorError = editorError.empty() ? "editor_host_unavailable" : editorError;
            return false;
        }
        if (editor && editorAttached.load(std::memory_order_acquire) && editorParent == parentWindow) {
            editor->onFocus(true);
            return true;
        }
        closeEditor();
        Steinberg::IPlugView *rawView = controller->createView("editor");
        if (!rawView) {
            editorError = "editor_view_unavailable";
            return false;
        }
        editor = Steinberg::owned(rawView);
        if (!succeeded(editor->isPlatformTypeSupported(Steinberg::kPlatformTypeHWND))) {
            editor = nullptr;
            editorError = "editor_hwnd_unsupported";
            return false;
        }
        auto frame = Steinberg::owned(new RuntimePlugFrame(parentWindow));
        if (!frame || !succeeded(editor->setFrame(frame.get()))) {
            editor = nullptr;
            editorError = "editor_frame_failed";
            return false;
        }
        if (auto scale = FUnknownPtr<Steinberg::IPlugViewContentScaleSupport>(editor.get()))
            scale->setContentScaleFactor(static_cast<float>(gpvst3::ui::nativeEditorScale(reinterpret_cast<void *>(parentWindow))));
        ViewRect rect{};
        if (!succeeded(editor->getSize(&rect))) {
            rect = ViewRect(0, 0, 420, 260);
        }
        gpvst3::ui::resizeNativeEditor(reinterpret_cast<void *>(parentWindow), rect.getWidth(), rect.getHeight());
        if (!succeeded(editor->attached(reinterpret_cast<void *>(parentWindow),
                                        Steinberg::kPlatformTypeHWND))) {
            editor->setFrame(nullptr);
            editor = nullptr;
            editorError = "editor_attach_failed";
            return false;
        }
        plugFrame = std::move(frame);
        editorParent = parentWindow;
        editor->onSize(&rect);
        editorAttached.store(true, std::memory_order_release);
        editorError.clear();
        return true;
    }

    void closeEditor() noexcept {
        editorParent = nullptr;
        if (editor) {
            if (editorAttached.exchange(false, std::memory_order_acq_rel))
                editor->removed();
            editor->setFrame(nullptr);
            editor = nullptr;
        }
        plugFrame = nullptr;
    }

    bool reconfigure(double sampleRate, std::size_t maxSamplesPerBlock) noexcept {
        if (!ready.load(std::memory_order_acquire) || !processor || maxSamplesPerBlock == 0)
            return false;
        processor->setProcessing(false);
        component->setActive(false);
        Steinberg::Vst::ProcessSetup setup{};
        setup.processMode = Steinberg::Vst::kRealtime;
        setup.symbolicSampleSize = Steinberg::Vst::kSample32;
        setup.maxSamplesPerBlock = static_cast<Steinberg::int32>(maxSamplesPerBlock);
        setup.sampleRate = sampleRate;
        if (!succeeded(processor->setupProcessing(setup)) || !scratch.prepare(2, maxSamplesPerBlock) ||
            !succeeded(component->setActive(true)) || !succeeded(processor->setProcessing(true))) {
            error = "runtime_vst3_reconfigure_failed";
            ready.store(false, std::memory_order_release);
            return false;
        }
        error.clear();
        configuredRate.store(static_cast<int>(sampleRate), std::memory_order_release);
        configuredBlock.store(maxSamplesPerBlock, std::memory_order_release);
        reportedLatencySamples.store(processor->getLatencySamples(), std::memory_order_release);
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
        parameterChanges.drain();
        const auto result = audio::process(*processor, block, scratch, false, &parameterChanges);
        parameterChanges.clear();
        processing.clear(std::memory_order_release);
        if (result.outputWritten) outputWritten.store(true, std::memory_order_release);
        if (result.ownerPointerObserved) ownerObserved.store(true, std::memory_order_release);
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
    if (flags & Steinberg::Vst::kParamValuesChanged) {
        for (int i = 0; i < owner_->controller->getParameterCount(); ++i) {
            Steinberg::Vst::ParameterInfo info{};
            if (succeeded(owner_->controller->getParameterInfo(i, info)))
                owner_->queueParameter(info.id, owner_->controller->getParamNormalized(info.id));
        }
        return Steinberg::kResultOk;
    }
    return Steinberg::kNotImplemented;
}

// A selected P7 list is prepared as one immutable callback context. Each
// processor writes into the next preallocated planar buffer; the final one
// writes to the host buffer. Rebuilding happens off the audio callback.
struct SelectionSlot {
    static constexpr std::size_t kMaxEffects = 8;
    std::shared_ptr<RuntimeEffect> effects[kMaxEffects];
    audio::PlanarBuffer pipeline[2];
    std::size_t count = 0;
    std::atomic<int> failedIndex{-1};

    void shutdown() noexcept {
        for (auto &effect : effects) effect.reset();
        count = 0;
        failedIndex.store(-1, std::memory_order_release);
    }

    bool prepare(const std::vector<Vst3SelectionEntry> &entries, double rate,
                 std::size_t maxBlock, const SelectionSlot *previous, std::string *error) {
        // Preserve a warm, disabled instance even when the slot being reused
        // is also the last active slot. Copy ownership before clearing it.
        std::array<std::shared_ptr<RuntimeEffect>, kMaxEffects> reusable;
        const auto previousCount = previous ? previous->count : 0;
        for (std::size_t i = 0; i < previousCount; ++i) reusable[i] = previous->effects[i];
        shutdown();
        if (entries.empty()) return true;
        if (entries.size() > kMaxEffects || !pipeline[0].prepare(2, maxBlock) ||
            !pipeline[1].prepare(2, maxBlock)) {
            if (error) *error = "runtime_vst3_chain_buffer_failed";
            return false;
        }
        for (std::size_t index = 0; index < entries.size(); ++index) {
            for (std::size_t old = 0; old < previousCount; ++old) {
                const auto &effect = reusable[old];
                if (effect->identity.module == entries[index].module &&
                    effect->identity.classId == entries[index].classId &&
                    effect->identity.componentState == entries[index].componentState &&
                    effect->identity.controllerState == entries[index].controllerState &&
                    effect->configuredRate.load() == static_cast<int>(rate))
                    effects[index] = effect;
            }
            if (!effects[index]) {
                effects[index] = std::make_shared<RuntimeEffect>();
                if (!effects[index]->initialize(rate, maxBlock, fs::u8path(entries[index].module),
                                                entries[index].classId, &entries[index])) {
                    if (error) *error = effects[index]->error;
                    shutdown();
                    return false;
                }
            }
        }
        count = entries.size();
        return true;
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

// One independently prepared VST3 chain per host track.  The object is fixed
// in the runtime table so the audio callback never follows a map or allocates;
// control-thread reconfiguration drains the chain before replacing instances.
struct TrackRuntime {
    SelectionSlot trackSlots[2];
    effects::Chain chain;
    std::size_t count = 0;
    std::string trackKey;
    std::string trackId;
    std::string scoreKey;
    int trackIndex = -1;
    std::string error;
    std::vector<Vst3SelectionEntry> failedSelection;
    std::atomic<int> configuredRate{0};
    std::vector<Vst3SelectionEntry> requested;
    std::atomic<bool> configured{false};
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
        count = 0;
        configuredRate.store(0, std::memory_order_release);
        processBlocks.store(0, std::memory_order_relaxed);
        processedBlocks.store(0, std::memory_order_relaxed);
        bypassBlocks.store(0, std::memory_order_relaxed);
        errorBlocks.store(0, std::memory_order_relaxed);
        configured.store(false, std::memory_order_release);
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
        const auto old = chain.snapshot().activeSlot;
        const auto target = old == 0 ? 1U : 0U;
        const auto *previous = old >= 0 ? &trackSlots[old] : nullptr;
        // Reuse only a slot that has been retired at a block boundary. The
        // chain admission check drains readers before the slot's shared
        // processor instances are replaced.
        if (!chain.prepareSlot(target, {&trackSlots[target], &SelectionSlot::processCallback})) {
            if (error) *error = "runtime_vst3_track_chain_prepare_failed";
            return false;
        }
        if (!trackSlots[target].prepare(entries, rate, maxBlock, previous, error)) return false;
        count = entries.size();
        if (entries.empty()) {
            chain.clearFault();
            chain.setBypassed(true);
            configured.store(true, std::memory_order_release);
            return true;
        }
        if (!chain.prepareSlot(target, {&trackSlots[target], &SelectionSlot::processCallback}) ||
            !chain.activate(target)) {
            if (error) *error = "runtime_vst3_track_chain_activate_failed";
            trackSlots[target].shutdown();
            return false;
        }
        chain.clearFault();
        chain.setBypassed(false);
        configured.store(true, std::memory_order_release);
        configuredRate.store(static_cast<int>(rate), std::memory_order_release);
        return true;
    }

    bool processBlock(const audio::BlockView &block, bool *actuallyProcessed = nullptr) noexcept {
        if (actuallyProcessed) *actuallyProcessed = false;
        processBlocks.fetch_add(1, std::memory_order_relaxed);
        if (!configured.load(std::memory_order_acquire) || count == 0 ||
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
};

struct Runtime {
    Patch master;
    Patch dsp;
    Patch stream;
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
    std::atomic<std::uint64_t> audioCallbackBlocks{0};
    std::atomic<std::uint64_t> firstAudioCallbackNanoseconds{0};
    std::atomic<std::uint64_t> audioCallbackProcessingNanoseconds{0};
    std::atomic<std::uint64_t> audioCallbackMaxNanoseconds{0};
    std::atomic<std::uint64_t> audioCallbackDeadlineNanoseconds{0};
    std::atomic<std::size_t> audioCallbackDeadlineOverruns{0};
    std::atomic<std::size_t> audioCallbackExtraCopyOperations{0};
    std::atomic<std::size_t> audioCallbackSampleIndex{0};
    std::array<std::atomic<std::uint64_t>, 256> audioCallbackSamples{};
    std::atomic<std::size_t> deviceInputLatencySamples{0};
    std::atomic<std::size_t> deviceOutputLatencySamples{0};
    std::atomic<bool> bufferWriteObserved{false};
    std::atomic<bool> dspInsideMaster{false};
    std::atomic<std::size_t> frames{0};
    std::atomic<std::size_t> channels{0};
    std::atomic<int> rate{0};
    std::atomic<unsigned long> masterThread{0};
    std::atomic<unsigned long> dspThread{0};
    RuntimeEffect effects[2];
    SelectionSlot selectionSlots[2];
    std::recursive_mutex editorMutex;
    std::mutex selectionMutex;
    std::mutex selectionRequestMutex;
    std::mutex catalogMutex;
    std::atomic<bool> selectionWorkerBusy{false};
    int retainedSelectionSlot = -1;
    std::vector<Vst3SelectionEntry> requestedSelection;
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
    RuntimeEffect inputEffect;
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
    std::condition_variable selectionCondition;
    bool selectionWorkerStop = false;
    bool selectionRequestPending = false;
    std::uint64_t selectionRequestGeneration = 0;
    std::uint64_t selectionAppliedGeneration = 0;
    std::vector<Vst3SelectionEntry> pendingSelection;
    std::vector<Vst3SelectionEntry> appliedSelection;
    std::unordered_map<std::string, std::vector<Vst3SelectionEntry>> pendingTrackSelections;
};

Runtime g_runtime;
State g_initial;
host::Verification g_verification;
std::shared_ptr<RuntimeEffect> g_openEditorEffect; // Owned and accessed on Qt.
std::atomic<bool> g_selectionStateChanged{false};
thread_local bool g_inMasterHook = false;
thread_local bool g_inEditorCallback = false;

struct EditorCallbackScope {
    const bool previous = g_inEditorCallback;
    EditorCallbackScope() noexcept { g_inEditorCallback = true; }
    ~EditorCallbackScope() { g_inEditorCallback = previous; }
};

double callbackSampleRate() noexcept;
void rejectSavedEntry(const Vst3SelectionEntry &entry, const std::string &error,
                      state::ScopeKind scope, const std::string &score = {},
                      const std::string &track = {}, int trackIndex = -1);

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

bool catalogIsReady() noexcept {
    std::lock_guard<std::mutex> lock(g_runtime.catalogMutex);
    return g_runtime.catalogReady;
}

void selectionWorkerLoop() {
    for (;;) {
        std::vector<Vst3SelectionEntry> selection;
        std::string trackKey;
        std::uint64_t generation = 0;
        {
            std::unique_lock<std::mutex> lock(g_runtime.selectionRequestMutex);
            g_runtime.selectionCondition.wait(lock, [] {
                return g_runtime.selectionWorkerStop || g_runtime.selectionRequestPending ||
                       !g_runtime.pendingTrackSelections.empty();
            });
            if (g_runtime.selectionWorkerStop && !g_runtime.selectionRequestPending &&
                g_runtime.pendingTrackSelections.empty()) return;
            if (g_runtime.selectionRequestPending) {
                selection = std::move(g_runtime.pendingSelection);
                generation = g_runtime.selectionRequestGeneration;
                g_runtime.selectionRequestPending = false;
            } else {
                auto it = g_runtime.pendingTrackSelections.begin();
                trackKey = it->first;
                selection = std::move(it->second);
                g_runtime.pendingTrackSelections.erase(it);
            }
            g_runtime.selectionWorkerBusy.store(true, std::memory_order_release);
        }
        {
            // Only the worker waits for runtime ownership. Qt's observation,
            // capture and editor paths try this lock and defer while busy.
            std::lock_guard<std::recursive_mutex> editorLock(g_runtime.editorMutex);
            std::lock_guard<std::mutex> lock(g_runtime.selectionMutex);
            if (trackKey.empty()) {
                std::string error;
                const bool accepted = configureSelectedChain(selection, &error);
                if (accepted) {
                    g_runtime.requestedSelection = selection;
                    g_runtime.appliedSelection = selection;
                    g_runtime.selectionAppliedGeneration = generation;
                } else {
                    for (const auto &entry : selection)
                        if (!containsIdentity(g_runtime.appliedSelection, entry))
                            rejectSavedEntry(entry, "runtime_vst3_selection_prepare_failed",
                                             state::ScopeKind::Global);
                    g_runtime.requestedSelection = g_runtime.appliedSelection;
                }
            } else {
                auto runtime = std::find_if(std::begin(g_runtime.trackRuntimes),
                                            std::end(g_runtime.trackRuntimes),
                                            [&](const TrackRuntime &value) {
                                                return value.trackKey == trackKey;
                                            });
                if (runtime != std::end(g_runtime.trackRuntimes)) {
                    std::string error;
                    if (runtime->prepare(selection, callbackSampleRate(), 16384, &error)) {
                        runtime->requested = selection;
                        g_runtime.requestedTrackSelections[trackKey] = selection;
                        runtime->error.clear();
                    } else {
                        for (const auto &entry : selection)
                            if (!containsIdentity(runtime->requested, entry))
                                rejectSavedEntry(entry, "runtime_vst3_selection_prepare_failed",
                                                 state::ScopeKind::Track, runtime->scoreKey,
                                                 runtime->trackKey, runtime->trackIndex);
                        runtime->error = error;
                        runtime->failedSelection = selection;
                    }
                }
            }
            g_selectionStateChanged.store(true, std::memory_order_release);
        }
        g_runtime.selectionWorkerBusy.store(false, std::memory_order_release);
    }
}

void startSelectionWorker() {
    std::lock_guard<std::mutex> lock(g_runtime.selectionRequestMutex);
    if (g_runtime.selectionWorker.joinable()) return;
    g_runtime.selectionWorkerStop = false;
    g_runtime.selectionWorker = std::thread(selectionWorkerLoop);
}

void stopSelectionWorker() noexcept {
    {
        std::lock_guard<std::mutex> lock(g_runtime.selectionRequestMutex);
        if (!g_runtime.selectionWorker.joinable()) return;
        g_runtime.selectionWorkerStop = true;
        g_runtime.selectionRequestPending = false;
        g_runtime.pendingSelection.clear();
        g_runtime.pendingTrackSelections.clear();
    }
    g_runtime.selectionCondition.notify_all();
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

std::vector<Vst3SelectionEntry> savedSelection(const QJsonArray &effects) {
    std::vector<Vst3SelectionEntry> result;
    std::lock_guard<std::mutex> lock(g_runtime.catalogMutex);
    for (const auto &value : effects) {
        const auto effect = value.toObject();
        if (!effect.value("enabled").toBool()) continue;
        const auto module = effect.value("module").toString().toStdString();
        const auto classId = effect.value("class_id").toString().toStdString();
        if (std::none_of(g_runtime.catalogEntries.begin(), g_runtime.catalogEntries.end(),
            [&](const Vst3SelectionEntry &entry) { return entry.module == module && entry.classId == classId; })) continue;
        const auto component = QByteArray::fromBase64(effect.value("component_state").toString().toLatin1());
        const auto controller = QByteArray::fromBase64(effect.value("controller_state").toString().toLatin1());
        result.push_back({module, classId, {component.begin(), component.end()}, {controller.begin(), controller.end()}});
    }
    return result;
}

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

// Restore healthy entries even when one saved processor rejects its state.
// Explicit UI requests still use the atomic, all-or-nothing selection API.
template <typename Configure>
std::vector<Vst3SelectionEntry> restoreSavedEntries(const std::vector<Vst3SelectionEntry> &requested,
        state::ScopeKind scope, const std::string &score, const std::string &track, int trackIndex, Configure configure) {
    std::string error;
    if (configure(requested, &error)) return requested;
    std::vector<Vst3SelectionEntry> accepted;
    for (const auto &entry : requested) {
        auto candidate = accepted;
        candidate.push_back(entry);
        if (configure(candidate, &error)) accepted = std::move(candidate);
        else rejectSavedEntry(entry, error, scope, score, track, trackIndex);
    }
    if (accepted.empty()) configure(accepted, &error);
    return accepted;
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

void refreshTrackContextImpl() noexcept {
    if (g_inEditorCallback) return;
    if (!g_initial.hostSupported) return;
    const auto discovered = gpvst3::gp_audio::refresh();
    const auto bindings = gpvst3::gp_audio::snapshot();
    gpvst3::gp_audio::Binding selectedBinding;
    const bool haveSelectedBinding = gpvst3::gp_audio::currentTrack(selectedBinding);
    if (haveSelectedBinding) {
        gpvst3::state::setRuntimeTrackContext(
            QString::fromStdString(selectedBinding.scoreKey),
            QString::fromStdString(selectedBinding.trackKey),
            selectedBinding.trackIndex,
            QString::fromStdString(selectedBinding.trackId));
    } else {
        gpvst3::state::clearRuntimeTrackContext();
    }
    std::unique_lock<std::recursive_mutex> editorLock(g_runtime.editorMutex, std::try_to_lock);
    if (!editorLock.owns_lock()) return;
    std::unique_lock<std::mutex> lock(g_runtime.selectionMutex, std::try_to_lock);
    if (!lock.owns_lock()) return;
    g_runtime.currentTrackKey = haveSelectedBinding ? selectedBinding.trackKey : "";
    const int rate = static_cast<int>(callbackSampleRate());
    const auto sameBindings = bindings.size() == g_runtime.publishedBindings.size() &&
        std::equal(bindings.begin(), bindings.end(), g_runtime.publishedBindings.begin(),
            [](const gp_audio::Binding &a, const gp_audio::Binding &b) {
                return a.chain == b.chain && a.trackKey == b.trackKey && a.trackId == b.trackId &&
                       a.scoreKey == b.scoreKey && a.activeDocument == b.activeDocument;
            });
    bool pending = !sameBindings || (g_runtime.selectionMode.load() &&
        (g_runtime.selectionConfiguredRate.load() != rate || g_runtime.chain.faulted()));
    for (const auto &runtime : g_runtime.trackRuntimes) {
        if (runtime.trackKey.empty()) continue;
        const auto requested = g_runtime.requestedTrackSelections.find(runtime.trackKey);
        pending |= catalogIsReady() && requested == g_runtime.requestedTrackSelections.end();
        pending |= requested != g_runtime.requestedTrackSelections.end() && !sameSelection(runtime.requested, requested->second) &&
            (runtime.error.empty() || !sameSelection(runtime.failedSelection, requested->second));
        pending |= runtime.count > 0 && runtime.configuredRate.load() != rate;
        pending |= runtime.chain.faulted();
    }
    if (!pending) return;
    const TrackDispatchUpdate update;
    clearTrackDispatch();
    g_runtime.trackRuntimeError.clear();
    bool stable = true;
    std::size_t published = 0;
    std::array<bool, 32> used{};
    QJsonObject savedChain;
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
    if (catalogIsReady()) state::loadChain(savedChain);
    for (const auto &binding : bindings) {
        if (!binding.chain || !binding.activeDocument) continue;
        const auto runtimeIndex = trackRuntimeIndexFor(binding.trackKey);
        if (runtimeIndex < 0) { stable = false; continue; }
        used[static_cast<std::size_t>(runtimeIndex)] = true;
        auto &runtime = g_runtime.trackRuntimes[runtimeIndex];
        runtime.trackKey = binding.trackKey;
        runtime.trackId = binding.trackId;
        if (!runtime.scoreKey.empty() && runtime.scoreKey != binding.scoreKey) saveTrackRuntime(runtime);
        runtime.scoreKey = binding.scoreKey;
        runtime.trackIndex = binding.trackIndex;
        const bool restoring = catalogIsReady() && !g_runtime.requestedTrackSelections.count(runtime.trackKey);
        if (restoring)
            g_runtime.requestedTrackSelections[runtime.trackKey] = savedSelection(state::scopeEffects(
                savedChain, state::ScopeKind::Track, QString::fromStdString(binding.scoreKey), QString::fromStdString(binding.trackKey)));
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
        if (requested != g_runtime.requestedTrackSelections.end() && !queued &&
            (!runtime.configured.load(std::memory_order_acquire) ||
             !sameSelection(runtime.requested, requested->second)) &&
            (runtime.error.empty() || !sameSelection(runtime.failedSelection, requested->second))) {
            std::string error;
            if (restoring) {
                requested->second = restoreSavedEntries(requested->second, state::ScopeKind::Track,
                    runtime.scoreKey, runtime.trackKey, runtime.trackIndex,
                    [&](const std::vector<Vst3SelectionEntry> &entries, std::string *failure) {
                        return runtime.prepare(entries, callbackSampleRate(), 16384, failure);
                    });
                runtime.requested = requested->second;
                runtime.error.clear();
            } else if (runtime.prepare(requested->second, callbackSampleRate(), 16384, &error)) {
                runtime.requested = requested->second;
                runtime.error.clear();
            } else {
                runtime.error = error;
                runtime.failedSelection = requested->second;
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

void updateAudioLayerState() noexcept;
double callbackSampleRate() noexcept {
    if (g_runtime.sampleRate && g_runtime.audioCore) {
        const auto rate = g_runtime.sampleRate(g_runtime.audioCore);
        if (rate > 0) return static_cast<double>(rate);
    }
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
    const auto before = bufferHash(buffer);
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
        const auto effectBefore = bufferHash(buffer);
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
        if (effectBefore != bufferHash(buffer) && !chainResult.bypassed)
            g_runtime.effectWriteObserved.store(true, std::memory_order_relaxed);
    }
    if (before != bufferHash(buffer)) g_runtime.bufferWriteObserved.store(true, std::memory_order_relaxed);
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
    if (!configured || !*configured || std::strcmp(configured, "disabled") == 0)
        return input::Route::Disabled;
    if (std::strcmp(configured, "input_insert") == 0)
        return input::Route::InputInsert;
    if (std::strcmp(configured, "bus_mix") == 0)
        return input::Route::BusMix;
    return input::Route::Disabled;
}

void updateAudioLayerState() noexcept {
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
    g_runtime.inputEffect.shutdown();
    if (route == input::Route::Disabled) return true;
    const double initialRate = callbackSampleRate();
    if (!g_runtime.inputRouter.prepare(2, portaudio::kMaxFrames) ||
        !g_runtime.inputEffect.initialize(initialRate, 16384))
        return false;
    g_runtime.inputConfiguredRate.store(static_cast<int>(initialRate), std::memory_order_release);
    g_runtime.inputConfiguredChannels.store(g_runtime.inputChannelCount, std::memory_order_release);
    g_runtime.inputConfiguredOutputChannels.store(g_runtime.outputChannelCount, std::memory_order_release);
    g_runtime.inputRouter.setProcessor(
        {&g_runtime.inputEffect, &RuntimeEffect::processCallback});
    const auto *bypass = std::getenv("GPVST3_TOTAL_BYPASS");
    g_runtime.inputRouter.setBypassed(bypass && std::strcmp(bypass, "1") == 0);
    g_runtime.inputRouter.setEnabled(true);
    updateAudioLayerState();
    return true;
}

bool inputFeatureEnabled() noexcept {
    const char *enabled = std::getenv("GPVST3_ENABLE_P4_INPUT");
    return enabled && std::strcmp(enabled, "1") == 0;
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

    const auto *dispatch = findTrackDispatch(self);
    auto *trackRuntime = dispatch ? dispatch->runtime.load(std::memory_order_acquire) : nullptr;
    if (!trackRuntime || !g_runtime.rawData || !g_runtime.frameCount ||
        !g_runtime.channelCount || !buffer) {
        g_runtime.trackScopeUnresolved.store(true, std::memory_order_release);
        return;
    }
    const auto frames = g_runtime.frameCount(buffer);
    const auto channels = g_runtime.channelCount(buffer);
    if (frames == 0 || frames > 16384 || channels == 0 || channels > 2) {
        g_runtime.trackScopeUnresolved.store(true, std::memory_order_release);
        return;
    }
    const auto &raw = g_runtime.rawData(buffer);
    if (!raw.channels[0] || (channels > 1 && !raw.channels[1])) {
        g_runtime.trackScopeUnresolved.store(true, std::memory_order_release);
        return;
    }
    const auto rate = callbackSampleRate();
    const float *inputs[2]{raw.channels[0], raw.channels[1]};
    float *outputs[2]{raw.channels[0], raw.channels[1]};
    const audio::BlockView block{inputs, nullptr, outputs, nullptr,
                                 channels, frames, rate, frames, buffer,
                                 sequence, true};
    const auto before = bufferHash(buffer);
    bool actuallyProcessed = false;
    const bool completed = trackRuntime->processBlock(block, &actuallyProcessed);
    const auto after = bufferHash(buffer);
    if (completed && trackRuntime->count != 0) {
        g_runtime.trackChainProcessBlocks.fetch_add(1, std::memory_order_relaxed);
        if (actuallyProcessed) {
            g_runtime.trackChainProcessedBlocks.fetch_add(1, std::memory_order_relaxed);
            g_runtime.trackRuntimeProcessed.store(true, std::memory_order_release);
        }
        if (before != after) {
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

void writeJump(std::uint8_t *bytes, const void *destination) noexcept {
    bytes[0] = 0x48;
    bytes[1] = 0xB8;
    const auto address = reinterpret_cast<std::uint64_t>(destination);
    std::memcpy(bytes + 2, &address, sizeof(address));
    bytes[10] = 0xFF;
    bytes[11] = 0xE0;
}

bool install(Patch &patch, void *target, void *detour, const std::uint8_t *expected,
             std::size_t size) noexcept {
    if (!target || !detour || !expected || patch.installed || size < 12 ||
        size > sizeof(patch.original) ||
        std::memcmp(target, expected, size) != 0) return false;
    auto *trampoline = static_cast<std::uint8_t *>(VirtualAlloc(nullptr, size + 12,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) return false;
    std::memcpy(patch.original, target, size);
    std::memcpy(trampoline, target, size);
    writeJump(trampoline + size, static_cast<std::uint8_t *>(target) + size);
    FlushInstructionCache(GetCurrentProcess(), trampoline, size + 12);
    DWORD protection = 0;
    if (!VirtualProtect(target, size, PAGE_EXECUTE_READWRITE, &protection)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }
    // Publish the original destination before making the entry point callable.
    patch.target = target;
    patch.trampoline = trampoline;
    patch.size = size;
    writeJump(static_cast<std::uint8_t *>(target), detour);
    std::memset(static_cast<std::uint8_t *>(target) + 12, 0x90, size - 12);
    FlushInstructionCache(GetCurrentProcess(), target, size);
    DWORD unused = 0;
    VirtualProtect(target, size, protection, &unused);
    patch.installed = true;
    return true;
}

void remove(Patch &patch) noexcept {
    if (!patch.installed) return;
    DWORD protection = 0;
    if (!VirtualProtect(patch.target, patch.size, PAGE_EXECUTE_READWRITE, &protection)) return;
    std::memcpy(patch.target, patch.original, patch.size);
    FlushInstructionCache(GetCurrentProcess(), patch.target, patch.size);
    DWORD unused = 0;
    VirtualProtect(patch.target, patch.size, protection, &unused);
    patch.installed = false;
    patch.size = 0;
    // An audio call may still be returning through this trampoline. Its tiny
    // allocation is retained until process exit instead of freeing live code.
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

bool configureSelectedChain(const std::vector<Vst3SelectionEntry> &selection,
                            std::string *error = nullptr) noexcept;

State prepare(const host::Verification &verification, bool enableForSelection) noexcept {
    std::lock_guard<std::recursive_mutex> editorLock(g_runtime.editorMutex);
    g_verification = verification;
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
        const bool master = install(g_runtime.master, GetProcAddress(gprse, kMasterProcess),
                                    reinterpret_cast<void *>(&masterProcessHook), kMasterPrologue,
                                    kMasterPatchBytes);
        const bool dsp = master && install(g_runtime.dsp, GetProcAddress(gprse, kEffectsChainProcessDsp),
                                          reinterpret_cast<void *>(&dspProcessHook), kDspPrologue,
                                          kDspPatchBytes);
        auto *streamTarget = amaudio
            ? reinterpret_cast<std::uint8_t *>(amaudio) + kStreamCallbackRva
            : nullptr;
        result.audioOutputCallback.exportFound = streamTarget != nullptr;
        install(
            g_runtime.stream, streamTarget, reinterpret_cast<void *>(&streamCallbackHook),
            kStreamPrologue, kStreamPatchBytes);
        result.installed = master && dsp;
        if (!result.installed) {
            remove(g_runtime.master);
            remove(g_runtime.dsp);
            remove(g_runtime.stream);
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
        {
            std::lock_guard<std::mutex> lock(g_runtime.selectionMutex);
            if (result.installed && !g_runtime.requestedSelection.empty() &&
                configureSelectedChain(g_runtime.requestedSelection))
                g_runtime.appliedSelection = g_runtime.requestedSelection;
        }
        if (result.installed && inputFeatureEnabled()) configureInputRouter();
        else {
            g_runtime.inputRouter.setEnabled(false);
            g_runtime.inputRouter.setRoute(input::Route::Disabled);
            g_runtime.inputEffect.shutdown();
        }
        result.reason = result.installed ? "runtime_observation_active" : "hook_install_failed";
    }
    g_initial = result;
    return result;
}

State snapshot() noexcept {
    static std::shared_ptr<const State> previous = std::make_shared<const State>();
    std::unique_lock<std::mutex> runtimeLock(g_runtime.selectionMutex, std::try_to_lock);
    if (!runtimeLock.owns_lock()) return *std::atomic_load(&previous);
    State result = g_initial;
    result.trackBindingSource = gp_audio::bindingSource();
    updateAudioLayerState();
    reconfigureInputRouterIfNeeded();
    const auto chain = g_runtime.chain.snapshot();
    const auto input = g_runtime.inputRouter.snapshot();
    result.installed = g_runtime.master.installed && g_runtime.dsp.installed;
    result.audioOutputCallbackInstalled = g_runtime.stream.installed;
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
            evidence.configuredEffects = runtime.count;
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
        result.runtimeEffectError = !g_runtime.selectionSlots[active].effects[0]->editorError.empty()
            ? g_runtime.selectionSlots[active].effects[0]->editorError
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
    result.chainDeadlineNanoseconds = chain.deadlineNanoseconds;
    result.chainDeadlineExceededBlocks = chain.deadlineExceededBlocks;
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
    result.inputProcessorReady = input.enabled && g_runtime.inputEffect.ready.load(std::memory_order_acquire);
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
    result.inputInterleavedCopyOperations = input.interleavedCopyOperations;
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
    result.audioCallbackBlocks = g_runtime.audioCallbackBlocks.load(std::memory_order_relaxed);
    result.firstAudioCallbackNanoseconds =
        g_runtime.firstAudioCallbackNanoseconds.load(std::memory_order_relaxed);
    result.audioCallbackProcessingNanoseconds =
        g_runtime.audioCallbackProcessingNanoseconds.load(std::memory_order_relaxed);
    result.audioCallbackMaxNanoseconds =
        g_runtime.audioCallbackMaxNanoseconds.load(std::memory_order_relaxed);
    result.audioCallbackDeadlineNanoseconds =
        g_runtime.audioCallbackDeadlineNanoseconds.load(std::memory_order_relaxed);
    result.audioCallbackDeadlineOverruns =
        g_runtime.audioCallbackDeadlineOverruns.load(std::memory_order_relaxed);
    result.audioCallbackExtraCopyOperations =
        g_runtime.audioCallbackExtraCopyOperations.load(std::memory_order_relaxed);
    std::vector<std::uint64_t> callbackSamples;
    const auto sampleCount = (std::min)(g_runtime.audioCallbackSampleIndex.load(std::memory_order_relaxed),
                                        g_runtime.audioCallbackSamples.size());
    callbackSamples.reserve(sampleCount);
    for (std::size_t index = 0; index < sampleCount; ++index) {
        const auto value = g_runtime.audioCallbackSamples[index].load(std::memory_order_relaxed);
        if (value != 0) callbackSamples.push_back(value);
    }
    std::sort(callbackSamples.begin(), callbackSamples.end());
    const auto percentile = [&callbackSamples](double fraction) -> std::uint64_t {
        if (callbackSamples.empty()) return 0;
        const auto index = (std::min)(callbackSamples.size() - 1,
            static_cast<std::size_t>(fraction * static_cast<double>(callbackSamples.size() - 1)));
        return callbackSamples[index];
    };
    result.audioCallbackP95Nanoseconds = percentile(0.95);
    result.audioCallbackP99Nanoseconds = percentile(0.99);
    result.deviceInputLatencySamples =
        g_runtime.deviceInputLatencySamples.load(std::memory_order_relaxed);
    result.deviceOutputLatencySamples =
        g_runtime.deviceOutputLatencySamples.load(std::memory_order_relaxed);
    std::size_t vst3Latency = g_runtime.inputEffect.reportedLatencySamples.load(std::memory_order_relaxed);
    const auto activeGlobal = chain.activeSlot;
    if (activeGlobal >= 0 && activeGlobal < 2) {
        if (g_runtime.selectionMode.load(std::memory_order_acquire)) {
            const auto &slot = g_runtime.selectionSlots[activeGlobal];
            for (std::size_t index = 0; index < slot.count; ++index)
                if (slot.effects[index])
                    vst3Latency += slot.effects[index]->reportedLatencySamples.load(std::memory_order_relaxed);
        } else if (!selectionPublished) {
            vst3Latency += g_runtime.effects[activeGlobal].reportedLatencySamples.load(std::memory_order_relaxed);
        }
    }
    result.vst3LatencySamples = vst3Latency;
    result.adapterLatencySamples = 0;
    result.roundtripLatencySamples = result.deviceInputLatencySamples +
        result.deviceOutputLatencySamples + result.vst3LatencySamples;
    result.roundtripLatencyMeasured = false;
    result.roundtripLatencyStatus = result.audioCallbackBlocks == 0
        ? "unavailable" : "reported_components_only";
    std::atomic_store(&previous, std::make_shared<const State>(result));
    return result;
}

bool configureSelectedChain(const std::vector<Vst3SelectionEntry> &selection, std::string *error) noexcept {
    if (error) error->clear();
    if (!g_runtime.master.installed || selection.size() > SelectionSlot::kMaxEffects) {
        if (error) *error = !g_runtime.master.installed ? "hook_install_failed" : "runtime_vst3_chain_full";
        return false;
    }
    const auto rate = callbackSampleRate();
    const auto old = g_runtime.chain.snapshot().activeSlot;
    const auto target = old == 0 ? 1U : 0U;
    if (!g_runtime.chain.prepareSlot(target,
            {&g_runtime.selectionSlots[target], &SelectionSlot::processCallback})) {
        if (error) *error = "runtime_vst3_chain_prepare_failed";
        return false;
    }
    const int previousSlot = old >= 0 ? old : g_runtime.retainedSelectionSlot;
    const auto *previous = previousSlot >= 0 && g_runtime.selectionPublished.load(std::memory_order_acquire)
        ? &g_runtime.selectionSlots[previousSlot] : nullptr;
    try {
        if (!g_runtime.selectionSlots[target].prepare(selection, rate, 16384, previous, error)) return false;
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
    if (g_runtime.inputProcessing.test_and_set(std::memory_order_acquire)) return;
    g_runtime.inputRouter.setEnabled(false);
    const bool configured = g_runtime.inputEffect.reconfigure(static_cast<double>(observedRate), 16384);
    if (configured) {
        g_runtime.inputConfiguredRate.store(observedRate, std::memory_order_release);
        g_runtime.inputConfiguredChannels.store(observedInput, std::memory_order_release);
        g_runtime.inputConfiguredOutputChannels.store(observedOutput, std::memory_order_release);
        g_runtime.inputConfigurationPending.store(false, std::memory_order_release);
    } else {
        g_runtime.inputConfigurationPending.store(true, std::memory_order_release);
        g_runtime.inputConfigurationErrors.fetch_add(1, std::memory_order_relaxed);
    }
    g_runtime.inputRouter.setEnabled(configured);
    g_runtime.inputProcessing.clear(std::memory_order_release);
}

void shutdown() noexcept {
    stopSelectionWorker();
    std::lock_guard<std::recursive_mutex> editorLock(g_runtime.editorMutex);
    const TrackDispatchUpdate trackUpdate;
    state::clearRuntimeTrackContext();
    g_runtime.chain.setBypassed(true);
    g_runtime.chain.deactivate();
    g_runtime.inputRouter.setEnabled(false);
    g_runtime.inputRouter.setRoute(input::Route::Disabled);
    g_runtime.inputEffect.shutdown();
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
    g_runtime.audioCallbackBlocks.store(0, std::memory_order_relaxed);
    g_runtime.firstAudioCallbackNanoseconds.store(0, std::memory_order_relaxed);
    g_runtime.audioCallbackProcessingNanoseconds.store(0, std::memory_order_relaxed);
    g_runtime.audioCallbackMaxNanoseconds.store(0, std::memory_order_relaxed);
    g_runtime.audioCallbackDeadlineNanoseconds.store(0, std::memory_order_relaxed);
    g_runtime.audioCallbackDeadlineOverruns.store(0, std::memory_order_relaxed);
    g_runtime.audioCallbackExtraCopyOperations.store(0, std::memory_order_relaxed);
    g_runtime.audioCallbackSampleIndex.store(0, std::memory_order_relaxed);
    for (auto &sample : g_runtime.audioCallbackSamples) sample.store(0, std::memory_order_relaxed);
    g_runtime.deviceInputLatencySamples.store(0, std::memory_order_relaxed);
    g_runtime.deviceOutputLatencySamples.store(0, std::memory_order_relaxed);
    remove(g_runtime.stream);
    remove(g_runtime.dsp);
    remove(g_runtime.master);
    g_runtime.effects[0].shutdown();
    g_runtime.effects[1].shutdown();
    g_runtime.selectionSlots[0].shutdown();
    g_runtime.selectionSlots[1].shutdown();
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
    g_runtime.pendingSelection.clear();
    g_runtime.appliedSelection.clear();
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

int streamCallbackHook(const void *input, void *output, unsigned long frames,
                       const void *timeInfo, unsigned long status, void *userData) {
    const auto started = std::chrono::steady_clock::now();
    const auto original = reinterpret_cast<StreamCallback>(g_runtime.stream.trampoline);
    const auto outputAddress = reinterpret_cast<std::uintptr_t>(output);
    auto firstBuffer = g_runtime.outputFirstBuffer.load(std::memory_order_relaxed);
    if (firstBuffer == 0)
        g_runtime.outputFirstBuffer.compare_exchange_strong(firstBuffer, outputAddress,
                                                              std::memory_order_relaxed);
    g_runtime.outputLastBuffer.store(outputAddress, std::memory_order_relaxed);
    g_runtime.outputCalls.fetch_add(1, std::memory_order_relaxed);
    const auto before = outputHash(output, frames);
    const auto result = original(input, output, frames, timeInfo, status, userData);
    const auto inputState = g_runtime.inputRouter.snapshot();
    double callbackRate = callbackSampleRate();
    const auto finish = [&] {
        const auto after = outputHash(output, frames);
        g_runtime.outputObserved.store(output != nullptr && frames != 0, std::memory_order_release);
        if (before != after) {
            bool expected = false;
            if (g_runtime.outputEvidenceClaimed.compare_exchange_strong(expected, true)) {
                g_runtime.outputBeforeHash.store(before, std::memory_order_relaxed);
                g_runtime.outputAfterHash.store(after, std::memory_order_relaxed);
                g_runtime.outputWriteObserved.store(true, std::memory_order_release);
            }
        }
        g_runtime.outputFrames.store(frames, std::memory_order_relaxed);
        g_runtime.outputThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
        const auto elapsed = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started).count());
        const auto deadline = callbackRate > 0.0
            ? static_cast<std::uint64_t>((static_cast<double>(frames) / callbackRate) * 1.0e9) : 0;
        g_runtime.audioCallbackBlocks.fetch_add(1, std::memory_order_relaxed);
        auto firstCallback = g_runtime.firstAudioCallbackNanoseconds.load(std::memory_order_relaxed);
        if (firstCallback == 0) {
            const auto sinceEpoch = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
            g_runtime.firstAudioCallbackNanoseconds.compare_exchange_strong(firstCallback, sinceEpoch,
                                                                            std::memory_order_relaxed);
        }
        g_runtime.audioCallbackProcessingNanoseconds.store(elapsed, std::memory_order_relaxed);
        g_runtime.audioCallbackDeadlineNanoseconds.store(deadline, std::memory_order_relaxed);
        auto maximum = g_runtime.audioCallbackMaxNanoseconds.load(std::memory_order_relaxed);
        while (maximum < elapsed && !g_runtime.audioCallbackMaxNanoseconds.compare_exchange_weak(
                   maximum, elapsed, std::memory_order_relaxed)) {}
        const auto sample = g_runtime.audioCallbackSampleIndex.fetch_add(1, std::memory_order_relaxed);
        g_runtime.audioCallbackSamples[sample % g_runtime.audioCallbackSamples.size()].store(
            elapsed, std::memory_order_relaxed);
        if (deadline != 0 && elapsed > deadline)
            g_runtime.audioCallbackDeadlineOverruns.fetch_add(1, std::memory_order_relaxed);
        return result;
    };
    // A disabled or bypassed input chain leaves the native callback's output
    // in place. Do not parse stream internals or copy capture data in this path.
    if (!inputState.enabled || inputState.bypassed ||
        inputState.route == input::Route::Disabled) return finish();
    portaudio::Configuration configuration;
    const bool configurationValid = frames > 0 &&
        frames <= portaudio::kMaxFrames &&
        portaudio::configuration(g_runtime.audioModule, userData, configuration);
    if (configurationValid) {
        callbackRate = configuration.sampleRate;
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
        const auto latencySamples = [callbackRate](double latency) noexcept {
            return std::isfinite(latency) && latency > 0.0
                ? static_cast<std::size_t>(latency * callbackRate + 0.5) : std::size_t{0};
        };
        g_runtime.deviceInputLatencySamples.store(latencySamples(configuration.inputSuggestedLatency),
                                                  std::memory_order_relaxed);
        g_runtime.deviceOutputLatencySamples.store(latencySamples(configuration.outputSuggestedLatency),
                                                   std::memory_order_relaxed);
    } else {
        g_runtime.inputConfigurationErrors.fetch_add(1, std::memory_order_relaxed);
    }
    const bool preparedConfiguration = configurationValid &&
        static_cast<int>(configuration.sampleRate) ==
            g_runtime.inputConfiguredRate.load(std::memory_order_acquire);
    if (preparedConfiguration && inputState.enabled &&
        inputState.route != input::Route::Disabled) {
        g_runtime.inputCapturePathLocated.store(true, std::memory_order_release);
        g_runtime.inputCaptureCalls.fetch_add(1, std::memory_order_relaxed);
        const input::InterleavedView view{
            input, output, static_cast<std::size_t>(frames), configuration.inputChannels,
            configuration.outputChannels, configuration.sampleRate,
            static_cast<std::size_t>(frames),
            userData, static_cast<std::uint64_t>(g_runtime.outputCalls.load(std::memory_order_relaxed)),
            input::InterleavedSampleFormat::Float32};
        const auto postOriginal = outputHash(output, frames);
        const bool observeSamples = input && output &&
            !g_runtime.inputOrderSamplesObserved.load(std::memory_order_acquire) &&
            g_runtime.trackChainProcessedBlocks.load(std::memory_order_relaxed) > 0 &&
            g_runtime.globalChainProcessBlocks.load(std::memory_order_relaxed) > 0;
        const float captureSample = observeSamples ? static_cast<const float *>(input)[0] : 0.0F;
        const float generatedSample = observeSamples ? static_cast<const float *>(output)[0] : 0.0F;
        if (processExternalInputInterleaved(view)) {
            const auto postRoute = outputHash(output, frames);
            g_runtime.inputAfterOriginalBlocks.fetch_add(1, std::memory_order_release);
            bool expected = false;
            if (postOriginal != postRoute && g_runtime.inputOrderEvidenceClaimed.compare_exchange_strong(expected, true)) {
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
    const auto copies = g_runtime.inputRouter.snapshot().interleavedCopyOperations;
    if (copies >= inputState.interleavedCopyOperations)
        g_runtime.audioCallbackExtraCopyOperations.fetch_add(copies - inputState.interleavedCopyOperations,
                                                            std::memory_order_relaxed);
    return finish();
}

void setTotalBypass(bool bypassed) noexcept {
    g_runtime.chain.setBypassed(bypassed);
    g_runtime.inputRouter.setBypassed(bypassed);
}

bool setVst3Selection(const std::vector<Vst3SelectionEntry> &selection, std::string *error) noexcept {
    std::lock_guard<std::recursive_mutex> editorLock(g_runtime.editorMutex);
    if (error) error->clear();
    // P7 controls are called on the Qt control thread. Install before taking
    // selectionMutex because prepare() also locks it to restore a saved chain.
    if (!g_runtime.master.installed && !selection.empty()) {
        const auto prepared = prepare(g_verification, true);
        if (!prepared.installed) {
            if (error) *error = prepared.reason;
            return false;
        }
    }
    std::lock_guard<std::mutex> lock(g_runtime.selectionMutex);
    if (!g_runtime.master.installed && selection.empty()) {
        g_runtime.requestedSelection.clear();
        return true;
    }
    if (!configureSelectedChain(selection, error)) return false;
    g_runtime.requestedSelection = selection;
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
    if (!g_runtime.master.installed && !selection.empty()) {
        const auto prepared = prepare(g_verification, true);
        if (!prepared.installed) {
            if (error) *error = prepared.reason;
            return false;
        }
    }
    startSelectionWorker();
    {
        std::lock_guard<std::mutex> lock(g_runtime.selectionRequestMutex);
        g_runtime.pendingSelection = selection;
        g_runtime.selectionRequestPending = true;
        ++g_runtime.selectionRequestGeneration;
    }
    g_runtime.selectionCondition.notify_one();
    return true;
}

void saveVst3States() {
    std::unique_lock<std::recursive_mutex> editorLock(g_runtime.editorMutex, std::try_to_lock);
    if (!editorLock.owns_lock()) return;
    std::unique_lock<std::mutex> lock(g_runtime.selectionMutex, std::try_to_lock);
    if (!lock.owns_lock()) return;
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
    saveVst3States();
    {
        std::lock_guard<std::mutex> lock(g_runtime.catalogMutex);
        g_runtime.catalogEntries = entries;
        g_runtime.catalogReady = true;
    }
    {
        std::lock_guard<std::mutex> lock(g_runtime.selectionMutex);
        g_runtime.requestedTrackSelections.clear();
    }
    QJsonObject chain;
    state::loadChain(chain);
    const auto global = savedSelection(state::scopeEffects(chain, state::ScopeKind::Global));
    bool anyEnabled = !global.empty();
    const auto scores = chain.value("scores").toObject();
    for (const auto &score : scores) {
        const auto tracks = score.toObject().value("tracks").toObject();
        for (const auto &track : tracks)
            anyEnabled |= !savedSelection(track.toObject().value("effects").toArray()).empty();
    }
    if (anyEnabled && !g_runtime.master.installed) {
        const auto prepared = prepare(g_verification, true);
        if (!prepared.installed) return;
    }
    if (g_runtime.master.installed && (!global.empty() || g_runtime.selectionPublished.load())) {
        startSelectionWorker();
        {
            std::lock_guard<std::mutex> lock(g_runtime.selectionRequestMutex);
            g_runtime.pendingSelection = global;
            g_runtime.selectionRequestPending = true;
            ++g_runtime.selectionRequestGeneration;
        }
        g_runtime.selectionCondition.notify_one();
    }
    refreshTrackContextImpl();
}

bool consumeSelectionStateChanges() noexcept {
    if (vst3SelectionPending()) return false;
    return g_selectionStateChanged.exchange(false, std::memory_order_acq_rel);
}

bool vst3SelectionPending() noexcept {
    std::lock_guard<std::mutex> lock(g_runtime.selectionRequestMutex);
    return g_runtime.selectionWorkerBusy.load(std::memory_order_acquire) ||
        g_runtime.selectionRequestPending || !g_runtime.pendingTrackSelections.empty();
}

bool setTrackVst3Selection(const std::string &trackKey,
                           const std::vector<Vst3SelectionEntry> &selection,
                           std::string *error) noexcept {
    std::lock_guard<std::recursive_mutex> editorLock(g_runtime.editorMutex);
    if (error) error->clear();
    if (trackKey.empty()) {
        if (error) *error = "track_scope_unresolved";
        return false;
    }
    if (selection.size() > SelectionSlot::kMaxEffects) {
        if (error) *error = "runtime_vst3_chain_full";
        return false;
    }
    if (!g_runtime.dsp.installed && !selection.empty()) {
        const auto prepared = prepare(g_verification, true);
        if (!prepared.installed) {
            if (error) *error = prepared.reason;
            return false;
        }
    }
    refreshTrackContextImpl();
    const auto bindings = gp_audio::snapshot();
    if (std::none_of(bindings.begin(), bindings.end(), [&](const gp_audio::Binding &binding) {
            return binding.chain && binding.activeDocument && binding.trackKey == trackKey;
        })) {
        if (error) *error = "track_scope_unresolved";
        return false;
    }
    std::vector<Vst3SelectionEntry> previous;
    {
        std::lock_guard<std::mutex> lock(g_runtime.selectionMutex);
        previous = g_runtime.requestedTrackSelections[trackKey];
        g_runtime.requestedTrackSelections[trackKey] = selection;
        for (auto &runtime : g_runtime.trackRuntimes)
            if (runtime.trackKey == trackKey) runtime.error.clear();
    }
    // Reconcile immediately when the MCP binding is already available. The
    // periodic control timer repeats this after score/Conductor rebuilds.
    refreshTrackContextImpl();
    std::string targetError;
    for (const auto &runtime : g_runtime.trackRuntimes)
        if (runtime.trackKey == trackKey) targetError = runtime.error;
    if (!targetError.empty()) {
        if (error) *error = targetError;
        {
            std::lock_guard<std::mutex> lock(g_runtime.selectionMutex);
            g_runtime.requestedTrackSelections[trackKey] = previous;
        }
        refreshTrackContextImpl();
        return false;
    }
    return true;
}

bool requestTrackVst3Selection(const std::string &trackKey,
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
    if (!g_runtime.dsp.installed && !selection.empty()) {
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
    }
    g_runtime.selectionCondition.notify_one();
    return true;
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
        const int active = runtime.chain.snapshot().activeSlot;
        if (active < 0) break;
        std::vector<Vst3SelectionEntry> result;
        auto &slot = runtime.trackSlots[active];
        for (std::size_t index = 0; index < slot.count; ++index)
            result.push_back(slot.effects[index]->captureState());
        runtime.requested = result;
        g_runtime.requestedTrackSelections[trackKey] = result;
        return result;
    }
    const auto it = g_runtime.requestedTrackSelections.find(trackKey);
    return it == g_runtime.requestedTrackSelections.end() ? std::vector<Vst3SelectionEntry>{} : it->second;
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
    if (g_inEditorCallback) return false;
    std::unique_lock<std::recursive_mutex> editorLock(g_runtime.editorMutex, std::try_to_lock);
    if (!editorLock.owns_lock()) return false;
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
            closeVst3Editors();
            g_openEditorEffect = effect;
            EditorCallbackScope callbackScope;
            return effect->openEditor(static_cast<HWND>(parentWindow));
        }
    }
    return false;
}

bool openTrackVst3Editor(const std::string &trackKey, const Vst3SelectionEntry &entry,
                         void *parentWindow) noexcept {
    if (!parentWindow || g_inEditorCallback) return false;
    std::unique_lock<std::recursive_mutex> editorLock(g_runtime.editorMutex, std::try_to_lock);
    if (!editorLock.owns_lock()) return false;
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
                closeVst3Editors();
                g_openEditorEffect = effect;
                EditorCallbackScope callbackScope;
                return effect->openEditor(static_cast<HWND>(parentWindow));
            }
        }
    }
    return false;
}

void scaleVst3Editor(void *host, double factor) noexcept {
    std::unique_lock<std::recursive_mutex> editorLock(g_runtime.editorMutex, std::try_to_lock);
    if (!editorLock.owns_lock()) return;
    const auto keepAlive = g_openEditorEffect;
    if (keepAlive) {
        EditorCallbackScope callbackScope;
        auto &effect = *keepAlive;
        if (effect.editorParent != host || !effect.editor) return;
        if (auto scale = FUnknownPtr<Steinberg::IPlugViewContentScaleSupport>(effect.editor.get())) {
            if (!succeeded(scale->setContentScaleFactor(static_cast<float>(factor)))) return;
            ViewRect size{};
            if (succeeded(effect.editor->getSize(&size)) && effect.plugFrame)
                effect.plugFrame->resizeView(effect.editor.get(), &size);
        }
    }
}

void closeVst3Editors() noexcept {
    std::unique_lock<std::recursive_mutex> editorLock(g_runtime.editorMutex, std::try_to_lock);
    if (!editorLock.owns_lock()) return;
    // The visible editor pins its processor independently of the two DSP
    // slots. No plug-in UI call may hold the worker's selection mutex:
    // attached/removed can pump Qt events and re-enter observation/capture.
    const auto effect = std::move(g_openEditorEffect);
    if (effect) {
        EditorCallbackScope callbackScope;
        effect->closeEditor();
    }
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
