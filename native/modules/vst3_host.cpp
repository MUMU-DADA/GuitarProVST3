#include "vst3_host.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef max
#undef min

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <unordered_set>
#include <QtCore/QCoreApplication>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

#include "audio_adapter.h"
#include "pluginterfaces/base/funknownimpl.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstpluginterfacesupport.h"
#include "public.sdk/source/common/memorystream.h"

namespace gpvst3::vst3 {
namespace {

namespace fs = std::filesystem;
using Steinberg::FUnknownPtr;
using Steinberg::IPtr;
using Steinberg::TUID;
using Steinberg::tresult;
using Steinberg::Vst::IComponent;
using Steinberg::Vst::IEditController;
using Steinberg::Vst::IAudioProcessor;

constexpr int kMaxSamplesPerBlock = 256;
constexpr double kProbeSampleRate = 44100.0;


bool succeeded(tresult result) noexcept {
    return result == Steinberg::kResultOk || result == Steinberg::kResultTrue;
}

std::string narrow(const std::wstring &value) {
    if (value.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                           static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                        result.data(), length, nullptr, nullptr);
    return result;
}

std::string narrow(const Steinberg::char16 *value, std::size_t maxCharacters) {
    if (!value) return {};
    std::size_t length = 0;
    while (length < maxCharacters && value[length] != 0) ++length;
    return narrow(std::wstring(reinterpret_cast<const wchar_t *>(value), length));
}

std::string ascii(const char *value, std::size_t maxCharacters) {
    if (!value) return {};
    std::size_t length = 0;
    while (length < maxCharacters && value[length] != 0) ++length;
    return std::string(value, length);
}

std::string uidString(const TUID value) {
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

void rewind(Steinberg::IBStream &stream) noexcept {
    stream.seek(0, Steinberg::IBStream::kIBSeekSet, nullptr);
}

class HostApplication final
    : public Steinberg::U::Implements<Steinberg::U::Directly<
          Steinberg::Vst::IHostApplication, Steinberg::Vst::IPlugInterfaceSupport>> {
public:
    tresult PLUGIN_API getName(Steinberg::Vst::String128 name) override {
        if (!name) return Steinberg::kInvalidArgument;
        constexpr char text[] = "GuitarProVST3 P1 Host";
        int i = 0;
        for (; text[i] != 0 && i < 127; ++i) name[i] = static_cast<Steinberg::Vst::TChar>(text[i]);
        name[i] = 0;
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

struct LoadedModule {
    HMODULE handle = nullptr;
    ExitModuleProc exit = nullptr;
    ~LoadedModule() {
        if (exit) exit();
        if (handle) FreeLibrary(handle);
    }
    LoadedModule() = default;
    LoadedModule(const LoadedModule &) = delete;
    LoadedModule &operator=(const LoadedModule &) = delete;
};

fs::path moduleBinary(const fs::path &path) {
    std::error_code error;
    if (!fs::is_directory(path, error)) return path;
    const auto candidate = path / L"Contents" / L"x86_64-win" / path.filename();
    if (fs::is_regular_file(candidate, error)) return candidate;
    return {};
}

std::vector<fs::path> configuredRoots(bool &explicitConfiguration) {
    const char *configured = std::getenv("GPVST3_VST3_PATHS");
    if (!configured || !*configured) configured = std::getenv("GPVST3_VST3_ROOT");
    std::vector<fs::path> roots;
    if (configured && *configured) {
        explicitConfiguration = true;
        std::string value(configured);
        std::size_t begin = 0;
        while (begin <= value.size()) {
            const auto end = value.find(';', begin);
            const auto item = value.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
            if (!item.empty()) roots.emplace_back(fs::u8path(item));
            if (end == std::string::npos) break;
            begin = end + 1;
        }
        return roots;
    }

    explicitConfiguration = false;
    const auto addRoot = [&roots](const char *base, const char *suffix) {
        if (base && *base) roots.emplace_back(fs::u8path(std::string(base) + suffix));
    };
    addRoot(std::getenv("ProgramW6432"), "\\Common Files\\VST3");
    addRoot(std::getenv("ProgramFiles"), "\\Common Files\\VST3");
    addRoot(std::getenv("ProgramFiles(x86)"), "\\Common Files\\VST3");
    addRoot(std::getenv("LOCALAPPDATA"), "\\Programs\\Common\\VST3");
    return roots;
}

void discoverFrom(const fs::path &root, bool, std::vector<fs::path> &result,
                 std::unordered_set<std::wstring> &seen) {
    std::error_code error;
    if (!fs::exists(root, error)) return;
    if (fs::is_regular_file(root, error)) {
        if (root.extension() == L".vst3") {
            const auto key = fs::weakly_canonical(root, error).wstring();
            if (seen.insert(key).second) result.push_back(root);
        }
        return;
    }
    if (!fs::is_directory(root, error)) return;
    if (root.extension() == L".vst3") {
        {
            const auto key = fs::weakly_canonical(root, error).wstring();
            if (seen.insert(key).second) result.push_back(root);
        }
        return;
    }
    fs::directory_iterator iterator(root, fs::directory_options::skip_permission_denied, error);
    const fs::directory_iterator end;
    while (!error && iterator != end) {
        discoverFrom(iterator->path(), false, result, seen);
        iterator.increment(error);
    }
}

std::vector<fs::path> discover(State &state) {
    bool explicitConfiguration = false;
    const auto roots = configuredRoots(explicitConfiguration);
    std::vector<fs::path> modules;
    std::unordered_set<std::wstring> seen;
    for (const auto &root : roots)
        discoverFrom(root, explicitConfiguration, modules, seen);
    std::sort(modules.begin(), modules.end(), [](const fs::path &a, const fs::path &b) {
        return a.wstring() < b.wstring();
    });
    state.modulesDiscovered = static_cast<int>(modules.size());
    return modules;
}

void setClassError(ClassState &state, std::string message) {
    if (state.error.empty()) state.error = std::move(message);
}

bool probeProcess(IAudioProcessor &processor, IComponent &component, ClassState &state) {
    constexpr std::size_t probeFrames = 32;
    std::size_t channels = 2;
    Steinberg::Vst::BusInfo bus{};
    if (component.getBusCount(Steinberg::Vst::kAudio, Steinberg::Vst::kOutput) > 0 &&
        succeeded(component.getBusInfo(Steinberg::Vst::kAudio, Steinberg::Vst::kOutput, 0, bus)) &&
        bus.channelCount > 0)
        channels = static_cast<std::size_t>(bus.channelCount);
    else if (component.getBusCount(Steinberg::Vst::kAudio, Steinberg::Vst::kInput) > 0 &&
             succeeded(component.getBusInfo(Steinberg::Vst::kAudio, Steinberg::Vst::kInput, 0, bus)) &&
             bus.channelCount > 0)
        channels = static_cast<std::size_t>(bus.channelCount);

    audio::PlanarBuffer scratch;
    if (!scratch.prepare(channels, probeFrames)) {
        setClassError(state, "process_probe_buffer_failed");
        return false;
    }
    std::vector<std::vector<float>> input(channels, std::vector<float>(probeFrames, 0.0F));
    std::vector<std::vector<float>> output(channels, std::vector<float>(probeFrames, 0.0F));
    std::vector<const float *> inputPointers(channels);
    std::vector<float *> outputPointers(channels);
    for (std::size_t channel = 0; channel < channels; ++channel) {
        inputPointers[channel] = input[channel].data();
        outputPointers[channel] = output[channel].data();
    }
    const audio::BlockView block{
        inputPointers.data(), nullptr, outputPointers.data(), nullptr, channels, probeFrames,
        kProbeSampleRate, probeFrames};
    const auto result = audio::process(processor, block, scratch, false);
    state.processProbeFrames = static_cast<unsigned int>(probeFrames);
    state.processProbePassed = result.processed;
    if (!result.processed) setClassError(state, std::string("process_probe_") + result.error);
    return result.processed;
}

ClassState validateClass(const fs::path &modulePath, Steinberg::IPluginFactory &factory,
                         const Steinberg::PClassInfoW &info, HostApplication &host) {
    ClassState result;
    result.module = narrow(modulePath.wstring());
    result.classId = uidString(info.cid);
    result.name = narrow(info.name, Steinberg::PClassInfo::kNameSize);
    result.vendor = narrow(info.vendor, Steinberg::PClassInfo2::kVendorSize);
    result.category = ascii(info.category, Steinberg::PClassInfo::kCategorySize);
    result.version = narrow(info.version, Steinberg::PClassInfo2::kVersionSize);
    result.sdkVersion = narrow(info.sdkVersion, Steinberg::PClassInfo2::kVersionSize);

    // A bundle also exports controller and other service classes. They are
    // enumerated for identity, but only audio components enter this lifecycle
    // probe.
    if (result.category != "Audio Module Class") return result;

    IComponent *rawComponent = nullptr;
    if (!succeeded(factory.createInstance(info.cid, IComponent::iid,
                                          reinterpret_cast<void **>(&rawComponent))) || !rawComponent) {
        setClassError(result, "create_component_failed");
        return result;
    }
    IPtr<IComponent> component = Steinberg::owned(rawComponent);
    result.componentCreated = true;

    auto *hostUnknown = static_cast<Steinberg::FUnknown *>(
        static_cast<Steinberg::Vst::IHostApplication *>(&host));
    if (!succeeded(component->initialize(hostUnknown))) {
        setClassError(result, "component_initialize_failed");
        return result;
    }
    result.componentInitialized = true;

    // Activate default audio buses before setup. GP routing is supplied by the
    // hash-gated observation layer; the private write-back path stays disabled
    // until its runtime ABI is traced.
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
    const auto componentStateResult = component->getState(&componentState);
    if (succeeded(componentStateResult)) {
        result.componentStateBytes = static_cast<std::size_t>(componentState.getSize());
        rewind(componentState);
        result.stateRoundTrip = succeeded(component->setState(&componentState));
        rewind(componentState);
    }

    FUnknownPtr<IAudioProcessor> processor(component.get());
    if (!processor) {
        setClassError(result, "audio_processor_unavailable");
    } else {
        Steinberg::Vst::ProcessSetup setup{};
        setup.processMode = Steinberg::Vst::kRealtime;
        setup.symbolicSampleSize = Steinberg::Vst::kSample32;
        setup.maxSamplesPerBlock = kMaxSamplesPerBlock;
        setup.sampleRate = kProbeSampleRate;
        result.processorReady = succeeded(processor->setupProcessing(setup));
        if (!result.processorReady) setClassError(result, "setup_processing_failed");
        result.latencySamples = processor->getLatencySamples();
        result.tailSamples = processor->getTailSamples();
    }

    TUID controllerId{};
    IPtr<IEditController> controller;
    if (succeeded(component->getControllerClassId(controllerId))) {
        IEditController *rawController = nullptr;
        if (succeeded(factory.createInstance(controllerId, IEditController::iid,
                                              reinterpret_cast<void **>(&rawController))) && rawController) {
            controller = Steinberg::owned(rawController);
            result.controllerCreated = true;
            result.controllerInitialized = succeeded(controller->initialize(hostUnknown));
            if (!result.controllerInitialized) setClassError(result, "controller_initialize_failed");
        }
    }

    // Some effects expose the controller on the component itself and do not
    // export a second controller class (the VST3 single-component pattern).
    if (!controller) {
        controller = FUnknownPtr<IEditController>(component.get());
        if (controller) {
            result.controllerCreated = true;
            result.controllerInitialized = true; // component initialize covered it
        }
    }
    if (controller && result.controllerInitialized) {
        Steinberg::MemoryStream controllerState;
        if (succeeded(controller->getState(&controllerState))) {
            result.controllerStateBytes = static_cast<std::size_t>(controllerState.getSize());
            rewind(controllerState);
            result.controllerStateRoundTrip = succeeded(controller->setState(&controllerState));
        }
        if (succeeded(componentStateResult)) {
            rewind(componentState);
            controller->setComponentState(&componentState);
        }
        result.parameterCount = std::max(0, static_cast<int>(controller->getParameterCount()));
        for (int index = 0; index < result.parameterCount; ++index) {
            Steinberg::Vst::ParameterInfo parameter{};
            if (!succeeded(controller->getParameterInfo(index, parameter))) continue;
            if ((parameter.flags & Steinberg::Vst::ParameterInfo::kIsBypass) != 0) {
                result.bypassParameter = true;
                const auto before = controller->getParamNormalized(parameter.id);
                const bool setOn = succeeded(controller->setParamNormalized(parameter.id, 1.0));
                const bool setOff = succeeded(controller->setParamNormalized(parameter.id, before));
                result.bypassRoundTrip = setOn && setOff;
                break;
            }
        }
    }

    if (processor && result.processorReady) {
        result.active = succeeded(component->setActive(true));
        if (!result.active) setClassError(result, "set_active_failed");
        result.processing = result.active && succeeded(processor->setProcessing(true));
        if (result.active && !result.processing) setClassError(result, "set_processing_failed");
        if (result.processing) probeProcess(*processor, *component, result);
        if (result.processing) processor->setProcessing(false);
        if (result.active) component->setActive(false);
    }
    component->terminate();
    return result;
}

State scanInProcess(bool metadataOnly = false, const std::vector<fs::path> &selected = {}) {
    State result;
    result.workerThread = true;
    const auto modules = selected.empty() ? discover(result) : selected;
    result.modulesDiscovered = static_cast<int>(modules.size());
    const char *configuredPaths = std::getenv("GPVST3_VST3_PATHS");
    if (!configuredPaths || !*configuredPaths) configuredPaths = std::getenv("GPVST3_VST3_ROOT");
    const bool validateLifecycles = !metadataOnly && configuredPaths && *configuredPaths;
    auto host = Steinberg::owned(new HostApplication);
    std::unordered_set<std::string> seenClasses;
    for (const auto &packagePath : modules) {
        const auto binary = moduleBinary(packagePath);
        if (binary.empty()) {
            result.errors.push_back(narrow(packagePath.wstring()) + ":invalid_bundle");
            continue;
        }
        LoadedModule loaded;
        loaded.handle = LoadLibraryW(binary.wstring().c_str());
        if (!loaded.handle) {
            result.errors.push_back(narrow(packagePath.wstring()) + ":load_failed:" +
                                    std::to_string(GetLastError()));
            continue;
        }
        if (const auto init = reinterpret_cast<InitModuleProc>(GetProcAddress(loaded.handle, "InitDll"));
            init && !init()) {
            result.errors.push_back(narrow(packagePath.wstring()) + ":InitDll_failed");
            continue;
        }
        loaded.exit = reinterpret_cast<ExitModuleProc>(GetProcAddress(loaded.handle, "ExitDll"));
        using GetFactoryProc = Steinberg::IPluginFactory *(PLUGIN_API *)();
        const auto getFactory = reinterpret_cast<GetFactoryProc>(GetProcAddress(loaded.handle, "GetPluginFactory"));
        if (!getFactory) {
            result.errors.push_back(narrow(packagePath.wstring()) + ":missing_GetPluginFactory");
            continue;
        }
        auto factory = Steinberg::owned(getFactory());
        if (!factory) {
            result.errors.push_back(narrow(packagePath.wstring()) + ":null_factory");
            continue;
        }
        ++result.modulesLoaded;
        if (auto factory3 = FUnknownPtr<Steinberg::IPluginFactory3>(factory.get()))
            factory3->setHostContext(static_cast<Steinberg::FUnknown *>(
                static_cast<Steinberg::Vst::IHostApplication *>(host.get())));

        const auto count = factory->countClasses();
        for (Steinberg::int32 index = 0; index < count; ++index) {
            Steinberg::PClassInfoW info{};
            bool hasInfo = false;
            if (auto factory3 = FUnknownPtr<Steinberg::IPluginFactory3>(factory.get()))
                hasInfo = succeeded(factory3->getClassInfoUnicode(index, &info));
            if (!hasInfo) {
                Steinberg::PClassInfo2 info2{};
                if (auto factory2 = FUnknownPtr<Steinberg::IPluginFactory2>(factory.get()))
                    hasInfo = succeeded(factory2->getClassInfo2(index, &info2));
                if (hasInfo) info.fromAscii(info2);
            }
            if (!hasInfo) {
                Steinberg::PClassInfo info1{};
                if (succeeded(factory->getClassInfo(index, &info1))) {
                    Steinberg::PClassInfo2 info2{};
                    std::memcpy(info2.cid, info1.cid, sizeof(Steinberg::TUID));
                    info2.cardinality = info1.cardinality;
                    std::memcpy(info2.category, info1.category, sizeof(info1.category));
                    std::memcpy(info2.name, info1.name, sizeof(info1.name));
                    info.fromAscii(info2);
                    hasInfo = true;
                }
            }
            if (!hasInfo) {
                result.errors.push_back(narrow(packagePath.wstring()) + ":class_info_failed:" +
                                        std::to_string(index));
                continue;
            }
            ++result.classesEnumerated;
            ClassState classState;
            if (validateLifecycles) {
                classState = validateClass(packagePath, *factory, info, *host);
            } else {
                classState.module = narrow(packagePath.wstring());
                classState.classId = uidString(info.cid);
                classState.name = narrow(info.name, Steinberg::PClassInfo::kNameSize);
                classState.vendor = narrow(info.vendor, Steinberg::PClassInfo2::kVendorSize);
                classState.category = ascii(info.category, Steinberg::PClassInfo::kCategorySize);
                classState.version = narrow(info.version, Steinberg::PClassInfo2::kVersionSize);
                classState.sdkVersion = narrow(info.sdkVersion, Steinberg::PClassInfo2::kVersionSize);
                classState.error = classState.category == "Audio Module Class"
                                       ? "metadata_only_scan"
                                       : "non_audio_class";
                const std::string subcategories = ascii(info.subCategories, Steinberg::PClassInfo2::kSubCategoriesSize);
                const bool instrument = subcategories.find("Instrument") != std::string::npos;
                classState.effectIdentified = classState.category == "Audio Module Class" && !instrument;
                if (instrument) classState.error = "instrument_class_not_effect";
            }
            const auto classKey = classState.module + "\\n" + classState.classId;
            if (!seenClasses.insert(classKey).second) continue;
            if (classState.componentCreated) ++result.instancesCreated;
            if (classState.processorReady) ++result.processCalls;
            if (classState.processProbePassed) ++result.processProbesPassed;
            if (classState.active && classState.processing && classState.componentInitialized &&
                classState.processorReady && classState.processProbePassed)
                ++result.lifecyclesPassed;
            result.classes.push_back(std::move(classState));
        }
    }
    if (!validateLifecycles && result.classesEnumerated > 0)
        result.status = "catalog_ready";
    else if (result.lifecyclesPassed > 0)
        result.status = "ready";
    else if (result.classesEnumerated > 0)
        result.status = "lifecycle_failed";
    else if (result.modulesLoaded > 0)
        result.status = "no_classes";
    else
        result.status = "no_plugins";
    result.ready = result.status == "ready" || result.status == "catalog_ready";
    result.scanPending = false;
    return result;
}

} // namespace

State prepare(bool hostSupported) noexcept {
    if (!hostSupported) {
        State result;
        result.status = "host_unsupported";
        result.errors.push_back("host_hash_mismatch");
        return result;
    }
    try {
        return std::async(std::launch::async, [] { return scanInProcess(); }).get();
    } catch (const std::exception &error) {
        State result;
        result.status = "error";
        result.errors.push_back(error.what());
        return result;
    } catch (...) {
        State result;
        result.status = "error";
        result.errors.push_back("unknown_exception");
        return result;
    }
}

State identifyBundle(const std::string &module, bool hostSupported) noexcept {
    State result;
    result.hostSupported = hostSupported;
    if (!hostSupported || module.empty()) {
        result.status = "host_unsupported";
        result.errors.push_back("host_unsupported");
        return result;
    }
    try {
        result = scanInProcess(true, {fs::u8path(module)});
        result.hostSupported = hostSupported;
        return result;
    }
    catch (...) {
        result.status = "identification_failed";
        result.errors.push_back("identification_exception");
        return result;
    }
}

std::vector<CatalogEntry> effectCatalog(const State &state) {
    if (state.staticScan) return state.catalog;
    std::vector<CatalogEntry> result;
    std::unordered_set<std::string> seen;
    for (const auto &item : state.classes) {
        if (item.category != "Audio Module Class") continue;
        const auto key = item.module + "\n" + item.classId;
        if (!seen.insert(key).second) continue;
        result.push_back(CatalogEntry{item.module, item.classId, item.name, item.vendor,
                                      item.category, item.category == "Audio Module Class" && item.processorReady,
                                      item.error, item.effectIdentified || item.processorReady, "factory_on_request"});
    }
    std::sort(result.begin(), result.end(), [](const CatalogEntry &a, const CatalogEntry &b) {
        if (a.name != b.name) return a.name < b.name;
        if (a.vendor != b.vendor) return a.vendor < b.vendor;
        return a.module < b.module;
    });
    return result;
}

} // namespace gpvst3::vst3
