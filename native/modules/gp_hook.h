#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "input_router.h"
#include "host_lock.h"

namespace gpvst3::hook {

struct Vst3SelectionEntry {
    std::string module;
    std::string classId;
    std::vector<unsigned char> componentState;
    std::vector<unsigned char> controllerState;
};

struct EntryPointObservation {
    bool moduleLoaded = false;
    bool exportFound = false;
    bool callObserved = false;
    bool bufferWriteObserved = false;
    std::size_t callCount = 0;
    std::size_t frameCount = 0;
    std::size_t channelCount = 0;
    double sampleRate = 0.0;
    unsigned long threadId = 0;
    std::uint64_t firstSequence = 0;
    std::uint64_t lastSequence = 0;
    std::uintptr_t firstBufferAddress = 0;
    std::uintptr_t lastBufferAddress = 0;
    std::uint64_t beforeHash = 0;
    std::uint64_t afterHash = 0;
};

struct State {
    bool installed = false;
    bool enabled = false;
    bool hostSupported = false;
    bool observationOnly = true;
    bool audioBufferAccessorsFound = false;
    bool audioBufferLockAccessorsFound = false;
    bool audioBufferPointerObserved = false;
    bool audioBufferWritebackObserved = false;
    bool audioOutputCallbackInstalled = false;
    bool audioOutputObserved = false;
    bool audioOutputWritebackObserved = false;
    bool effectsChainInsideMaster = false;
    bool effectsChainAfterMasterObserved = false;
    bool crossThreadObserved = false;
    bool sameBufferObserved = false;
    bool runtimeEffectEnabled = false;
    bool runtimeProcessorReady = false;
    bool runtimeProcessObserved = false;
    bool runtimeBufferWriteObserved = false;
    std::size_t runtimeProcessCount = 0;
    std::size_t runtimeConfigurationMismatchBlocks = 0;
    bool runtimeConfigurationMatches = false;
    std::string runtimeEffectName;
    std::string runtimeEffectError;
    bool totalBypass = true;
    bool chainFaulted = false;
    int chainActiveSlot = -1;
    std::size_t chainPreparedSlots = 0;
    std::size_t chainProcessBlocks = 0;
    std::size_t chainProcessedBlocks = 0;
    std::size_t chainBypassBlocks = 0;
    std::size_t chainErrorBlocks = 0;
    std::size_t chainFallbackBlocks = 0;
    std::uint64_t lastProcessNanoseconds = 0;
    std::uint64_t maxProcessNanoseconds = 0;
    std::uint64_t totalProcessNanoseconds = 0;
    std::size_t chainSwitchCount = 0;
    bool globalChainEnabled = false;
    std::size_t globalChainProcessBlocks = 0;
    std::size_t trackChainProcessBlocks = 0;
    bool trackContextObserved = false;
    bool trackContextStable = false;
    bool trackScopeUnresolved = true;
    std::string trackContextKey;
    std::size_t audioBufferSequenceCount = 0;
    std::size_t runtimeEffectInstances = 0;
    std::size_t reconfigurationPassed = 0;
    std::size_t reconfigurationFailed = 0;
    bool reconfigurationValidated = false;
    bool audioLayerInputLevelAccessorFound = false;
    bool audioLayerInputLevelObserved = false;
    bool audioLayerStreamRunning = false;
    std::size_t audioLayerBufferSize = 0;
    bool inputCapturePathLocated = false;
    bool inputCaptureObserved = false;
    bool inputRouteEnabled = false;
    bool inputProcessorReady = false;
    std::string inputRoute = "disabled";
    std::string inputRouteReason = "p4_capture_tap_unresolved";
    std::size_t inputCaptureBlocks = 0;
    std::size_t inputProcessedBlocks = 0;
    std::size_t inputBusMixedBlocks = 0;
    std::size_t inputBypassBlocks = 0;
    std::size_t inputErrorBlocks = 0;
    std::size_t inputDroppedBlocks = 0;
    std::size_t inputFrameCount = 0;
    std::size_t inputChannelCount = 0;
    double inputSampleRate = 0.0;
    float inputLastPeak = 0.0F;
    float inputMaxPeak = 0.0F;
    float inputLastRms = 0.0F;
    bool inputInterleavedFormatObserved = false;
    bool inputInterleavedObserved = false;
    bool inputInterleavedOutputWritten = false;
    std::size_t inputInterleavedBlocks = 0;
    std::size_t inputInterleavedFormatErrors = 0;
    std::size_t inputInterleavedMissingBlocks = 0;
    std::size_t inputInterleavedInputChannelCount = 0;
    std::size_t inputInterleavedOutputChannelCount = 0;
    std::uintptr_t inputFirstCaptureAddress = 0;
    std::uintptr_t inputLastCaptureAddress = 0;
    std::uintptr_t inputFirstCaptureOwner = 0;
    std::uintptr_t inputLastCaptureOwner = 0;
    std::uintptr_t inputFirstOutputAddress = 0;
    std::uintptr_t inputLastOutputAddress = 0;
    std::string inputCaptureFormat = "unresolved";
    std::string inputCaptureChannelLayout = "unresolved";
    std::string inputCaptureOwnership = "borrowed_for_callback";
    bool inputConfigurationObserved = false;
    std::size_t inputConfiguredInputChannels = 0;
    std::size_t inputConfiguredOutputChannels = 0;
    double inputConfiguredSampleRate = 0.0;
    std::size_t inputConfigurationErrors = 0;
    std::string reason = "p2_observation_only";
    EntryPointObservation masterProcess;
    EntryPointObservation effectsChainProcessDsp;
    EntryPointObservation audioOutputCallback;
};

// The first P7 selection can enable the locked hook without a development
// environment switch. An explicit GPVST3_ENABLE_P2_HOOK=0 still disables it.
// Empty startup remains unpatched; all paths require host hash/prologue checks.
State prepare(const host::Verification &verification, bool enableForSelection = false) noexcept;
State snapshot() noexcept;
void shutdown() noexcept;

// Thread-safe control used by the Qt panel. It only changes an atomic bypass
// flag; processor creation and destruction remain on the worker thread.
void setTotalBypass(bool bypassed) noexcept;
// Queues the current P7 checked list for control-thread preparation. The
// audio callback only sees the atomically published chain and never touches
// these strings or creates plug-in instances.
bool setVst3Selection(const std::vector<Vst3SelectionEntry> &selection,
                      std::string *error = nullptr) noexcept;
std::vector<Vst3SelectionEntry> captureVst3States();
// P8 scope-aware aliases. Global uses the verified Master post-processing
// chain; track selection remains bypassed until a stable host track context is
// observed in processDSP.
bool setGlobalVst3Selection(const std::vector<Vst3SelectionEntry> &selection,
                            std::string *error = nullptr) noexcept;
bool setTrackVst3Selection(const std::string &trackKey,
                           const std::vector<Vst3SelectionEntry> &selection,
                           std::string *error = nullptr) noexcept;
std::vector<Vst3SelectionEntry> captureGlobalVst3States();
std::vector<Vst3SelectionEntry> captureTrackVst3States(const std::string &trackKey);
// Open the editor owned by the currently active processing instance. The
// caller supplies a native Windows child HWND created on the Qt UI thread.
bool openVst3Editor(const Vst3SelectionEntry &entry, void *parentWindow) noexcept;
void closeVst3Editors() noexcept;
void scaleVst3Editor(void *host, double scale) noexcept;

// Called by the eventual AudioLayer/PortAudio capture adapter. The function
// owns no buffers and is safe to call from the audio callback after prepare().
bool processExternalInput(const input::CaptureView &capture,
                          const input::GeneratedView &generated,
                          const input::OutputView &output) noexcept;
bool processExternalInputInterleaved(const input::InterleavedView &view) noexcept;

}
