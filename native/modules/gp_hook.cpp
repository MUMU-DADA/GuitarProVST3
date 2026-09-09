#include "gp_hook.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef max
#undef min

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <filesystem>

#include "audio_adapter.h"
#include "pluginterfaces/base/funknownimpl.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstpluginterfacesupport.h"

namespace gpvst3::hook {
namespace {

constexpr char kMasterProcess[] =
    "?process@Master@rse@gp@@QEAAXAEAVAudioBuffer@audio@am@@AEBV?$vector@VTick@audio@am@@V?$allocator@VTick@audio@am@@@std@@@std@@AEBV?$vector@PEAVMusician@rse@gp@@V?$allocator@PEAVMusician@rse@gp@@@std@@@8@AEBV?$shared_ptr@VBackingTrack@rse@gp@@@8@@Z";
constexpr char kEffectsChainProcessDsp[] =
    "?processDSP@EffectsChain@rse@gp@@QEAAXAEAVIAudioBuffer@audio@am@@AEAV?$array@VAudioBuffer@audio@am@@$02@std@@AEBV?$vector@VTick@audio@am@@V?$allocator@VTick@audio@am@@@std@@@8@@Z";
constexpr char kRawData[] = "?rawData@AudioBuffer@audio@am@@UEBAAEBV?$array@PEAM$01@std@@XZ";
constexpr char kFrameCount[] = "?frameCount@AudioBuffer@audio@am@@UEBA_JXZ";
constexpr char kChannelCount[] = "?channelCount@AudioBuffer@audio@am@@UEBAIXZ";
constexpr char kAudioCoreInstance[] = "?Instance@AudioCore@audio@am@@SAAEAV123@XZ";
constexpr char kSampleRate[] = "?samplingRate@AudioCore@audio@am@@QEBAHXZ";

// The locked 8.1.1.17 entry points both begin with three complete, position
// independent mov instructions. Refuse installation if memory differs.
constexpr std::size_t kPatchBytes = 15;
constexpr std::uint8_t kMasterPrologue[kPatchBytes]{
    0x4C, 0x89, 0x44, 0x24, 0x18, 0x48, 0x89, 0x54, 0x24, 0x10, 0x48, 0x89, 0x4C, 0x24, 0x08};
constexpr std::uint8_t kDspPrologue[kPatchBytes]{
    0x48, 0x89, 0x5C, 0x24, 0x20, 0x48, 0x89, 0x54, 0x24, 0x10, 0x48, 0x89, 0x4C, 0x24, 0x08};

struct RawData { float *channels[2]; };
using MasterProcess = void (*)(void *, void *, void *, void *, void *);
using DspProcess = void (*)(void *, void *, void *, void *);
using RawDataFn = const RawData &(*)(const void *);
using FrameCountFn = std::size_t (*)(const void *);
using ChannelCountFn = unsigned (*)(const void *);
using AudioCoreInstanceFn = void *(*)();
using SampleRateFn = int (*)(const void *);

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
    bool ready = false;
    std::atomic_flag processing = ATOMIC_FLAG_INIT;

    ~RuntimeEffect() { shutdown(); }

    void shutdown() noexcept {
        ready = false;
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

    bool initialize() noexcept {
        shutdown();
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
        setup.maxSamplesPerBlock = 16384;
        setup.sampleRate = 44100.0;
        if (!succeeded(processor->setupProcessing(setup)) || !succeeded(component->setActive(true)) ||
            !succeeded(processor->setProcessing(true)) || !scratch.prepare(2, 16384)) {
            error = "runtime_vst3_processing_setup_failed";
            return false;
        }
        ready = true;
        error.clear();
        return true;
    }

    bool process(const RawData &raw, std::size_t frames, std::size_t channels,
                 double sampleRate) noexcept {
        if (!ready || !processor || frames == 0 || frames > scratch.frameCapacity() ||
            channels == 0 || channels > scratch.channelCount()) return false;
        for (std::size_t channel = 0; channel < channels; ++channel)
            if (!raw.channels[channel]) return false;
        if (processing.test_and_set(std::memory_order_acquire)) return false;
        const float *inputs[2]{raw.channels[0], raw.channels[1]};
        float *outputs[2]{raw.channels[0], raw.channels[1]};
        const audio::BlockView block{inputs, nullptr, outputs, nullptr, channels, frames,
                                     sampleRate, frames};
        const auto result = audio::process(*processor, block, scratch, false);
        processing.clear(std::memory_order_release);
        return result.processed;
    }
};

struct Patch {
    void *target = nullptr;
    void *trampoline = nullptr;
    std::uint8_t original[kPatchBytes]{};
    bool installed = false;
};

struct Runtime {
    Patch master;
    Patch dsp;
    RawDataFn rawData = nullptr;
    FrameCountFn frameCount = nullptr;
    ChannelCountFn channelCount = nullptr;
    SampleRateFn sampleRate = nullptr;
    void *audioCore = nullptr;
    std::atomic<std::size_t> masterCalls{0};
    std::atomic<std::size_t> dspCalls{0};
    std::atomic<bool> bufferWriteObserved{false};
    std::atomic<bool> dspInsideMaster{false};
    std::atomic<std::size_t> frames{0};
    std::atomic<std::size_t> channels{0};
    std::atomic<int> rate{0};
    std::atomic<unsigned long> masterThread{0};
    std::atomic<unsigned long> dspThread{0};
    RuntimeEffect effect;
    std::atomic<std::size_t> effectCalls{0};
    std::atomic<bool> effectProcessed{false};
    std::atomic<bool> effectWriteObserved{false};
};

Runtime g_runtime;
State g_initial;
thread_local bool g_inMasterHook = false;

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
    const auto before = bufferHash(buffer);
    if (g_runtime.masterCalls.fetch_add(1, std::memory_order_relaxed) == 0) {
        g_runtime.masterThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
        g_runtime.frames.store(g_runtime.frameCount(buffer), std::memory_order_relaxed);
        g_runtime.channels.store(g_runtime.channelCount(buffer), std::memory_order_relaxed);
        if (g_runtime.sampleRate && g_runtime.audioCore)
            g_runtime.rate.store(g_runtime.sampleRate(g_runtime.audioCore), std::memory_order_relaxed);
    }
    original(self, buffer, ticks, musicians, backingTrack);
    if (g_runtime.effect.ready && g_runtime.rawData && g_runtime.frameCount && g_runtime.channelCount) {
        const auto &raw = g_runtime.rawData(buffer);
        const auto frames = g_runtime.frameCount(buffer);
        const auto channels = (std::min)(g_runtime.channelCount(buffer), 2U);
        const auto effectBefore = bufferHash(buffer);
        const auto rate = g_runtime.rate.load(std::memory_order_relaxed);
        if (g_runtime.effect.process(raw, frames, channels, rate > 0 ? rate : 44100.0)) {
            g_runtime.effectCalls.fetch_add(1, std::memory_order_relaxed);
            g_runtime.effectProcessed.store(true, std::memory_order_relaxed);
            if (effectBefore != bufferHash(buffer))
                g_runtime.effectWriteObserved.store(true, std::memory_order_relaxed);
        }
    }
    if (before != bufferHash(buffer)) g_runtime.bufferWriteObserved.store(true, std::memory_order_relaxed);
    g_inMasterHook = false;
}

void dspProcessHook(void *self, void *buffer, void *scratch, void *ticks) {
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

bool install(Patch &patch, void *target, void *detour, const std::uint8_t *expected) noexcept {
    if (!target || patch.installed || std::memcmp(target, expected, kPatchBytes) != 0) return false;
    auto *trampoline = static_cast<std::uint8_t *>(VirtualAlloc(nullptr, kPatchBytes + 12,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) return false;
    std::memcpy(patch.original, target, kPatchBytes);
    std::memcpy(trampoline, target, kPatchBytes);
    writeJump(trampoline + kPatchBytes, static_cast<std::uint8_t *>(target) + kPatchBytes);
    FlushInstructionCache(GetCurrentProcess(), trampoline, kPatchBytes + 12);
    DWORD protection = 0;
    if (!VirtualProtect(target, kPatchBytes, PAGE_EXECUTE_READWRITE, &protection)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }
    // Publish the original destination before making the entry point callable.
    patch.target = target;
    patch.trampoline = trampoline;
    writeJump(static_cast<std::uint8_t *>(target), detour);
    std::memset(static_cast<std::uint8_t *>(target) + 12, 0x90, kPatchBytes - 12);
    FlushInstructionCache(GetCurrentProcess(), target, kPatchBytes);
    DWORD unused = 0;
    VirtualProtect(target, kPatchBytes, protection, &unused);
    patch.installed = true;
    return true;
}

void remove(Patch &patch) noexcept {
    if (!patch.installed) return;
    DWORD protection = 0;
    if (!VirtualProtect(patch.target, kPatchBytes, PAGE_EXECUTE_READWRITE, &protection)) return;
    std::memcpy(patch.target, patch.original, kPatchBytes);
    FlushInstructionCache(GetCurrentProcess(), patch.target, kPatchBytes);
    DWORD unused = 0;
    VirtualProtect(patch.target, kPatchBytes, protection, &unused);
    patch.installed = false;
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
        g_runtime.sampleRate = reinterpret_cast<SampleRateFn>(GetProcAddress(amaudio, kSampleRate));
        const auto coreInstance = reinterpret_cast<AudioCoreInstanceFn>(GetProcAddress(amaudio, kAudioCoreInstance));
        if (coreInstance) g_runtime.audioCore = coreInstance();
        const bool master = install(g_runtime.master, GetProcAddress(gprse, kMasterProcess),
                                    reinterpret_cast<void *>(&masterProcessHook), kMasterPrologue);
        const bool dsp = master && install(g_runtime.dsp, GetProcAddress(gprse, kEffectsChainProcessDsp),
                                          reinterpret_cast<void *>(&dspProcessHook), kDspPrologue);
        result.installed = master && dsp;
        if (!result.installed) {
            remove(g_runtime.master);
            remove(g_runtime.dsp);
        }
        if (result.installed && result.runtimeEffectEnabled) {
            result.runtimeProcessorReady = g_runtime.effect.initialize();
            result.runtimeEffectError = g_runtime.effect.error;
            result.runtimeEffectName = g_runtime.effect.name;
            result.observationOnly = !result.runtimeProcessorReady;
        } else {
            g_runtime.effect.shutdown();
        }
        result.reason = result.installed ? "runtime_observation_active" : "hook_install_failed";
    }
    g_initial = result;
    return result;
}

State snapshot() noexcept {
    State result = g_initial;
    result.installed = g_runtime.master.installed && g_runtime.dsp.installed;
    result.masterProcess.callCount = g_runtime.masterCalls.load(std::memory_order_relaxed);
    result.masterProcess.callObserved = result.masterProcess.callCount != 0;
    result.masterProcess.bufferWriteObserved = g_runtime.bufferWriteObserved.load(std::memory_order_relaxed);
    result.masterProcess.frameCount = g_runtime.frames.load(std::memory_order_relaxed);
    result.masterProcess.channelCount = g_runtime.channels.load(std::memory_order_relaxed);
    result.masterProcess.sampleRate = g_runtime.rate.load(std::memory_order_relaxed);
    result.masterProcess.threadId = g_runtime.masterThread.load(std::memory_order_relaxed);
    result.effectsChainProcessDsp.callCount = g_runtime.dspCalls.load(std::memory_order_relaxed);
    result.effectsChainProcessDsp.callObserved = result.effectsChainProcessDsp.callCount != 0;
    result.effectsChainProcessDsp.threadId = g_runtime.dspThread.load(std::memory_order_relaxed);
    result.effectsChainInsideMaster = g_runtime.dspInsideMaster.load(std::memory_order_relaxed);
    result.runtimeProcessorReady = g_runtime.effect.ready;
    result.runtimeProcessCount = g_runtime.effectCalls.load(std::memory_order_relaxed);
    result.runtimeProcessObserved = g_runtime.effectProcessed.load(std::memory_order_relaxed);
    result.runtimeBufferWriteObserved = g_runtime.effectWriteObserved.load(std::memory_order_relaxed);
    result.runtimeEffectName = g_runtime.effect.name;
    result.runtimeEffectError = g_runtime.effect.error;
    return result;
}

void shutdown() noexcept {
    remove(g_runtime.dsp);
    remove(g_runtime.master);
    g_runtime.effect.shutdown();
}

} // namespace gpvst3::hook
