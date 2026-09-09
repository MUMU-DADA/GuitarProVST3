#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace gpvst3::vst3 {

// P1 host results are plain data. The scan and plug-in lifecycle run on a
// worker thread; Qt only serializes this snapshot afterwards.
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
    std::string error;
};

struct State {
    std::string status = "pending_p1";
    bool ready = false;
    bool workerThread = false;
    int modulesDiscovered = 0;
    int modulesLoaded = 0;
    int classesEnumerated = 0;
    int instancesCreated = 0;
    int lifecyclesPassed = 0;
    std::vector<ClassState> classes;
    std::vector<std::string> errors;
};

// hostSupported gates all module loading. An unverified Guitar Pro build must
// remain bypassed and must not load third-party code.
State prepare(bool hostSupported = true) noexcept;

}
