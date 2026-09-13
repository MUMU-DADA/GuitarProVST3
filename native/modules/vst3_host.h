#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace gpvst3::vst3 {

// Lifecycle diagnostics for explicit development probes or one user-selected
// bundle. Static discovery never populates processor/lifecycle success flags.
struct ClassState {
    std::string module;
    std::string classId;
    std::string name;
    std::string vendor;
    std::string category;
    std::string version;
    std::string sdkVersion;
    bool componentCreated = false;
    bool componentInitialized = false;
    bool processorReady = false;
    bool controllerCreated = false;
    bool controllerInitialized = false;
    bool active = false;
    bool processing = false;
    bool stateRoundTrip = false;
    bool controllerStateRoundTrip = false;
    bool bypassParameter = false;
    bool bypassRoundTrip = false;
    int parameterCount = 0;
    std::size_t componentStateBytes = 0;
    std::size_t controllerStateBytes = 0;
    unsigned int latencySamples = 0;
    unsigned int tailSamples = 0;
    bool processProbePassed = false;
    unsigned int processProbeFrames = 0;
    std::string error;
    bool effectIdentified = false;
};

// Static identification is separate from tested runtime compatibility. A
// pending file candidate has an empty classId, never a fabricated class UID.
struct CatalogEntry {
    std::string module;
    std::string classId;
    std::string name;
    std::string vendor;
    std::string category;
    bool compatible = false;
    std::string error;
    bool identified = false;
    std::string source;
    std::string recognitionStatus = "idle";
    std::string recognitionSource;
    int recognitionAttempts = 0;
    std::string recognitionError;
    long long recognitionRetryAfter = 0;
    long long recognitionDeadlineAt = 0;
    std::string recognitionIgnoredReason;
};

struct State {
    std::string status = "pending_p1";
    bool hostSupported = false;
    bool ready = false;
    bool workerThread = false;
    bool scanPending = false;
    int modulesDiscovered = 0;
    int modulesLoaded = 0;
    int classesEnumerated = 0;
    int instancesCreated = 0;
    int lifecyclesPassed = 0;
    int processCalls = 0;
    int processProbesPassed = 0;
    std::vector<ClassState> classes;
    std::vector<std::string> errors;
    bool staticScan = false;
    bool cacheHit = false;
    int filesChecked = 0;
    int metadataReads = 0;
    int cacheReused = 0;
    int modulesChecked = 0;
    int scanGeneration = 0;
    long long elapsedMs = 0;
    std::string cacheStatus;
    std::string currentModule;
    std::vector<CatalogEntry> catalog;
    // Static file discovery and active factory recognition are reported
    // separately. Recognition always runs on the control worker.
    bool recognitionPending = false;
    bool recognitionWorker = false;
    int recognitionAttempted = 0;
    int recognitionCompleted = 0;
    int recognitionFailed = 0;
    int recognitionTimedOut = 0;
    int recognitionWorkersStarted = 0;
    int recognitionWorkersDetached = 0;
    std::string recognitionCurrentModule;
    std::string recognitionStatus = "idle";
};

using RecognitionControl = State (*)(const std::string &, bool) noexcept;

// hostSupported gates all module loading. An unverified Guitar Pro build must
// remain bypassed and must not load third-party code.
State prepare(bool hostSupported = true) noexcept;
// Explicit user selection only. Never called by static discovery or cache refresh.
State identifyBundle(const std::string &module, bool hostSupported) noexcept;

// Read the small local cache first, then check files on one worker. poll()
// delivers changed progress/catalog snapshots without waiting for the worker.
State beginAsync(bool hostSupported = true, bool retryTimedOut = false) noexcept;
bool poll(State &completed) noexcept;
bool pollNeeded() noexcept;
void shutdownScan() noexcept;
void setRecognitionControl(RecognitionControl control) noexcept;

std::vector<CatalogEntry> effectCatalog(const State &state);

}
