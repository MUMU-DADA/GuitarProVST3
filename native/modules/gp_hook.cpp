#include "gp_hook.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef max
#undef min

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <filesystem>

#include "audio_adapter.h"
#include "effect_chain.h"
#include "pluginterfaces/base/funknownimpl.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstpluginterfacesupport.h"

namespace gpvst3::hook {

int streamCallbackHook(const void *input, void *output, unsigned long frames,
                       const void *timeInfo, unsigned long status, void *userData);

namespace {

constexpr char kMasterProcess[] =
    "?process@Master@rse@gp@@QEAAXAEAVAudioBuffer@audio@am@@AEBV?$vector@VTick@audio@am@@V?$allocator@VTick@audio@am@@@std@@@std@@AEBV?$vector@PEAVMusician@rse@gp@@V?$allocator@PEAVMusician@rse@gp@@@std@@@8@AEBV?$shared_ptr@VBackingTrack@rse@gp@@@8@@Z";
constexpr char kEffectsChainProcessDsp[] =
    "?processDSP@EffectsChain@rse@gp@@QEAAXAEAVIAudioBuffer@audio@am@@AEAV?$array@VAudioBuffer@audio@am@@$02@std@@AEBV?$vector@VTick@audio@am@@V?$allocator@VTick@audio@am@@@std@@@8@@Z";
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

namespace fs = std::filesystem;
using Steinberg::FUnknownPtr;
using Steinberg::IPtr;
using Steinberg::TUID;
using Steinberg::tresult;
using Steinberg::Vst::IComponent;
using Steinberg::Vst::IAudioProcessor;

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

    ~RuntimeEffect() { shutdown(); }

    void shutdown() noexcept {
        ready.store(false, std::memory_order_release);
        outputWritten.store(false, std::memory_order_release);
        ownerObserved.store(false, std::memory_order_release);
        configuredRate.store(0, std::memory_order_release);
        configuredBlock.store(0, std::memory_order_release);
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

    bool initialize(double sampleRate = 44100.0, std::size_t maxSamplesPerBlock = 16384) noexcept {
        shutdown();
        forceError = false;
        if (const char *forced = std::getenv("GPVST3_FORCE_P3_ERROR");
            forced && std::strcmp(forced, "1") == 0)
            forceError = true;
        const auto path = runtimeBinary(runtimeModulePath());
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
            selected = info;
            found = true;
            break;
        }
        if (!found) {
            error = "runtime_vst3_audio_class_missing";
            return false;
        }
        name = selected.name;
        IComponent *rawComponent = nullptr;
        if (!succeeded(factory->createInstance(selected.cid, IComponent::iid,
                                               reinterpret_cast<void **>(&rawComponent))) || !rawComponent) {
            error = "runtime_vst3_component_failed";
            return false;
        }
        component = Steinberg::owned(rawComponent);
        auto *hostUnknown = static_cast<Steinberg::FUnknown *>(
            static_cast<Steinberg::Vst::IHostApplication *>(&host));
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
        processor = FUnknownPtr<IAudioProcessor>(component.get());
        if (!processor) {
            error = "runtime_vst3_processor_missing";
            return false;
        }
        Steinberg::Vst::ProcessSetup setup{};
        setup.processMode = Steinberg::Vst::kRealtime;
        setup.symbolicSampleSize = Steinberg::Vst::kSample32;
        setup.maxSamplesPerBlock = static_cast<Steinberg::int32>(maxSamplesPerBlock);
        setup.sampleRate = sampleRate;
        if (!succeeded(processor->setupProcessing(setup)) || !succeeded(component->setActive(true)) ||
            !succeeded(processor->setProcessing(true)) || !scratch.prepare(2, maxSamplesPerBlock)) {
            error = "runtime_vst3_processing_setup_failed";
            return false;
        }
        ready.store(true, std::memory_order_release);
        configuredRate.store(static_cast<int>(sampleRate), std::memory_order_release);
        configuredBlock.store(maxSamplesPerBlock, std::memory_order_release);
        error.clear();
        return true;
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
        const auto result = audio::process(*processor, block, scratch, false);
        processing.clear(std::memory_order_release);
        if (result.outputWritten) outputWritten.store(true, std::memory_order_release);
        if (result.ownerPointerObserved) ownerObserved.store(true, std::memory_order_release);
        return result.processed;
    }

    static bool processCallback(void *context, const audio::BlockView &block) noexcept {
        return static_cast<RuntimeEffect *>(context)->processBlock(block);
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
    void *audioCore = nullptr;
    std::atomic<std::size_t> masterCalls{0};
    std::atomic<std::size_t> dspCalls{0};
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
    effects::Chain chain;
    std::atomic<std::size_t> effectCalls{0};
    std::atomic<bool> effectProcessed{false};
    std::atomic<bool> effectWriteObserved{false};
    std::atomic<std::size_t> configurationMismatchBlocks{0};
    std::atomic<std::size_t> reconfigurationPassed{0};
    std::atomic<std::size_t> reconfigurationFailed{0};
    std::atomic<bool> reconfigurationValidated{false};
    RuntimeEffect inputEffect;
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
};

Runtime g_runtime;
State g_initial;
thread_local bool g_inMasterHook = false;

void updateAudioLayerState() noexcept;

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
        const bool configurationMatches = active < 0 ||
            (g_runtime.effects[active].configuredRate.load(std::memory_order_acquire) == rate &&
             frames <= g_runtime.effects[active].configuredBlock.load(std::memory_order_acquire));
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
    g_runtime.inputRouter.setRoute(route);
    g_runtime.inputRouter.setEnabled(false);
    g_runtime.inputRouter.setStreamRunning(true);
    g_runtime.inputRouter.setProcessor({});
    g_runtime.inputEffect.shutdown();
    if (route == input::Route::Disabled) return true;
    if (!g_runtime.inputRouter.prepare(2, 16384) ||
        !g_runtime.inputEffect.initialize(44100.0, 16384))
        return false;
    g_runtime.inputRouter.setProcessor(
        {&g_runtime.inputEffect, &RuntimeEffect::processCallback});
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
    if (g_inMasterHook) g_runtime.dspInsideMaster.store(true, std::memory_order_relaxed);
    reinterpret_cast<DspProcess>(g_runtime.dsp.trampoline)(self, buffer, scratch, ticks);
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

State prepare(const host::Verification &verification) noexcept {
    State result;
    result.hostSupported = verification.supported;
    if (!verification.supported) {
        result.reason = "host_unsupported";
        g_initial = result;
        return result;
    }
    const auto gprse = GetModuleHandleW(L"GPRSE.dll");
    const auto amaudio = GetModuleHandleW(L"AMAudio.dll");
    result.masterProcess = observe(gprse, kMasterProcess);
    result.effectsChainProcessDsp = observe(gprse, kEffectsChainProcessDsp);
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
    result.enabled = enabled && std::strcmp(enabled, "1") == 0;
    const char *effectEnabled = std::getenv("GPVST3_ENABLE_P2_EFFECT");
    result.runtimeEffectEnabled = result.enabled && effectEnabled && std::strcmp(effectEnabled, "1") == 0;
    if (!gprse) result.reason = "gprse_not_loaded";
    else if (!result.masterProcess.exportFound || !result.effectsChainProcessDsp.exportFound)
        result.reason = "entry_points_not_found";
    else if (!result.audioBufferAccessorsFound) result.reason = "buffer_accessors_not_found";
    else if (!result.enabled) result.reason = "p2_observation_only_callsite_unverified";
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
    State result = g_initial;
    updateAudioLayerState();
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
    result.crossThreadObserved = result.masterProcess.threadId != 0 &&
        result.effectsChainProcessDsp.threadId != 0 &&
        result.masterProcess.threadId != result.effectsChainProcessDsp.threadId;
    result.runtimeProcessorReady = chain.activeSlot >= 0 && chain.activeSlot < 2 &&
        g_runtime.effects[chain.activeSlot].ready.load(std::memory_order_acquire);
    result.runtimeProcessCount = g_runtime.effectCalls.load(std::memory_order_relaxed);
    result.runtimeConfigurationMismatchBlocks =
        g_runtime.configurationMismatchBlocks.load(std::memory_order_relaxed);
    result.runtimeConfigurationMatches = chain.activeSlot >= 0 && chain.activeSlot < 2 &&
        g_runtime.effects[chain.activeSlot].configuredRate.load(std::memory_order_acquire) ==
            g_runtime.rate.load(std::memory_order_relaxed) &&
        g_runtime.frames.load(std::memory_order_relaxed) <=
            g_runtime.effects[chain.activeSlot].configuredBlock.load(std::memory_order_acquire);
    result.runtimeProcessObserved = g_runtime.effectProcessed.load(std::memory_order_relaxed);
    result.runtimeBufferWriteObserved = g_runtime.effectWriteObserved.load(std::memory_order_relaxed);
    const auto active = chain.activeSlot >= 0 && chain.activeSlot < 2 ? chain.activeSlot : 0;
    result.runtimeEffectName = g_runtime.effects[active].name;
    result.runtimeEffectError = g_runtime.effects[active].error;
    result.totalBypass = chain.bypassed;
    result.chainFaulted = chain.faulted;
    result.chainActiveSlot = chain.activeSlot;
    result.chainPreparedSlots = chain.preparedSlots;
    result.chainProcessBlocks = chain.processBlocks;
    result.chainProcessedBlocks = chain.processedBlocks;
    result.chainBypassBlocks = chain.bypassBlocks;
    result.chainErrorBlocks = chain.errorBlocks;
    result.chainFallbackBlocks = chain.fallbackBlocks;
    result.lastProcessNanoseconds = chain.lastProcessNanoseconds;
    result.maxProcessNanoseconds = chain.maxProcessNanoseconds;
    result.totalProcessNanoseconds = chain.totalProcessNanoseconds;
    result.chainSwitchCount = chain.switchCount;
    result.runtimeEffectInstances = 0;
    for (const auto &effect : g_runtime.effects)
        if (effect.ready.load(std::memory_order_acquire)) ++result.runtimeEffectInstances;
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
    return result;
}

void shutdown() noexcept {
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
    remove(g_runtime.stream);
    remove(g_runtime.dsp);
    remove(g_runtime.master);
    g_runtime.effects[0].shutdown();
    g_runtime.effects[1].shutdown();
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
    const auto after = outputHash(output, frames);
    g_runtime.outputObserved.store(output != nullptr && frames != 0, std::memory_order_release);
    if (before != after) {
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
    return result;
}

void setTotalBypass(bool bypassed) noexcept {
    g_runtime.chain.setBypassed(bypassed);
}

bool processExternalInput(const input::CaptureView &capture,
                          const input::GeneratedView &generated,
                          const input::OutputView &output) noexcept {
    g_runtime.inputCapturePathLocated.store(true, std::memory_order_release);
    return g_runtime.inputRouter.process(capture, generated, output).completed;
}

} // namespace gpvst3::hook
