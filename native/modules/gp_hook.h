#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "input_router.h"
#include "host_lock.h"
#include <QtCore/QJsonArray>

namespace gpvst3::hook {

void setVst3Catalog(const QJsonArray &catalog);
void saveVst3States();

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

struct TrackRuntimeEvidence {
    std::string trackKey;
    std::string trackId;
    std::size_t processBlocks = 0;
    std::size_t processedBlocks = 0;
    std::size_t bypassBlocks = 0;
    std::size_t errorBlocks = 0;
    std::size_t configuredEffects = 0;
    bool configured = false;
    bool processed = false;
    bool writeObserved = false;
};

struct State {
    std::string trackBindingSource;
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
    // Selection/activation timeline. Timestamps use steady_clock nanoseconds
    // and are intended for ordering and latency diagnostics only.
    std::uint64_t selectionRequestId = 0;
    std::uint64_t selectionQueuedNanoseconds = 0;
    std::uint64_t selectionWorkerStartedNanoseconds = 0;
    std::uint64_t selectionPreparedNanoseconds = 0;
    std::uint64_t selectionCommittedNanoseconds = 0;
    std::uint64_t selectionAppliedGeneration = 0;
    std::uint64_t audioGeneration = 0;
    std::string selectionStatus;
    std::uint64_t chainActivationNanoseconds = 0;
    std::uint64_t chainFirstProcessedNanoseconds = 0;
    std::uint64_t chainActivationSequence = 0;
    std::uint64_t chainFirstProcessedSequence = 0;
    std::size_t chainCallbacksToFirstProcess = 0;
    std::uint64_t inputActivationNanoseconds = 0;
    std::uint64_t inputFirstProcessedNanoseconds = 0;
    std::uint64_t inputFirstProcessedSequence = 0;
    std::size_t inputCallbacksToFirstProcess = 0;
    std::string editorStage;
    std::string editorIdentity;
    std::string editorError;
    long editorResultCode = 0;
    std::uint64_t editorRequestGeneration = 0;
    bool totalBypass = true;
    bool chainFaulted = false;
    int chainActiveSlot = -1;
    std::size_t chainPreparedSlots = 0;
    std::size_t chainProcessBlocks = 0;
    std::size_t chainProcessedBlocks = 0;
    std::size_t chainBypassBlocks = 0;
    std::size_t chainErrorBlocks = 0;
    std::size_t chainFallbackBlocks = 0;
    std::size_t chainSwitchRequests = 0;
    std::size_t chainSwitchPrepared = 0;
    std::size_t chainRetiredSlots = 0;
    std::uint64_t chainLastSwitchNanoseconds = 0;
    std::uint64_t chainMaxSwitchNanoseconds = 0;
    std::uint64_t chainLastReaderDrainNanoseconds = 0;
    std::uint64_t chainMaxReaderDrainNanoseconds = 0;
    std::size_t chainReaderDrainTimeouts = 0;
    std::size_t chainSequenceGaps = 0;
    std::uint64_t chainLastSequence = 0;
    std::size_t chainRampSamples = 128;
    std::size_t chainRampRemaining = 0;
    std::uint64_t lastProcessNanoseconds = 0;
    std::uint64_t maxProcessNanoseconds = 0;
    std::uint64_t totalProcessNanoseconds = 0;
    std::size_t chainSwitchCount = 0;
    bool globalChainEnabled = false;
    std::size_t globalChainProcessBlocks = 0;
    std::size_t trackChainProcessBlocks = 0;
    std::size_t trackChainProcessedBlocks = 0;
    std::size_t trackBindingsPublished = 0;
    bool trackRuntimeProcessed = false;
    bool trackRuntimeWriteObserved = false;
    std::string trackRuntimeError;
    std::vector<TrackRuntimeEvidence> trackRuntimeEvidence;
    bool trackContextObserved = false;
    bool trackContextStable = false;
    bool trackScopeUnresolved = true;
    bool effectsChainIndexAccessorFound = false;
    bool effectsChainIndexObserved = false;
    int observedEffectsChainIndex = -1;
    std::size_t effectsChainContextCount = 0;
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
    std::size_t inputAfterOriginalBlocks = 0;
    std::uint64_t inputPostOriginalHash = 0;
    std::uint64_t inputPostRouteHash = 0;
    bool inputOrderSamplesObserved = false;
    float inputOrderCaptureSample = 0.0F;
    float inputOrderGeneratedSample = 0.0F;
    float inputOrderOutputSample = 0.0F;
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
// Refresh the verified GuitarProMCP EffectsChain -> track binding table on
// the Qt/control thread. The audio callback only consumes its atomics.
void refreshTrackContext() noexcept;
using SelectionNotifier = void (*)() noexcept;
using TrackContextNotifier = void (*)() noexcept;
void setSelectionNotifier(SelectionNotifier notifier) noexcept;
void setTrackContextNotifier(TrackContextNotifier notifier) noexcept;
bool editorCallbackActive() noexcept;
// Control-thread notification after a failed restored/running entry was
// persisted as disabled; the UI reloads the actual accepted selection.
bool consumeSelectionStateChanges() noexcept;
bool vst3SelectionPending() noexcept;

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
// Nonblocking UI request path. The request is coalesced and prepared on the
// runtime control worker; the synchronous API above remains available to
// fixtures and maintenance callers.
bool requestGlobalVst3Selection(const std::vector<Vst3SelectionEntry> &selection,
                                std::string *error = nullptr) noexcept;
bool setTrackVst3Selection(const std::string &trackKey,
                           const std::vector<Vst3SelectionEntry> &selection,
                           std::string *error = nullptr) noexcept;
bool requestTrackVst3Selection(const std::string &trackKey,
                               const std::vector<Vst3SelectionEntry> &selection,
                               std::string *error = nullptr) noexcept;
std::vector<Vst3SelectionEntry> captureGlobalVst3States();
std::vector<Vst3SelectionEntry> captureTrackVst3States(const std::string &trackKey);
// Open the editor owned by the currently active processing instance. The
// caller supplies a native Windows child HWND created on the Qt UI thread.
bool openVst3Editor(const Vst3SelectionEntry &entry, void *parentWindow) noexcept;
bool openTrackVst3Editor(const std::string &trackKey, const Vst3SelectionEntry &entry,
                         void *parentWindow) noexcept;
void closeVst3Editors() noexcept;
void scaleVst3Editor(void *host, double scale) noexcept;

// Called by the eventual AudioLayer/PortAudio capture adapter. The function
// owns no buffers and is safe to call from the audio callback after prepare().
bool processExternalInput(const input::CaptureView &capture,
                          const input::GeneratedView &generated,
                          const input::OutputView &output) noexcept;
bool processExternalInputInterleaved(const input::InterleavedView &view) noexcept;

}
