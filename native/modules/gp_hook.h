#pragma once

#include <cstddef>
#include <string>

#include "host_lock.h"

namespace gpvst3::hook {

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
};

struct State {
    bool installed = false;
    bool enabled = false;
    bool hostSupported = false;
    bool observationOnly = true;
    bool audioBufferAccessorsFound = false;
    bool effectsChainInsideMaster = false;
    bool runtimeEffectEnabled = false;
    bool runtimeProcessorReady = false;
    bool runtimeProcessObserved = false;
    bool runtimeBufferWriteObserved = false;
    std::size_t runtimeProcessCount = 0;
    std::string runtimeEffectName;
    std::string runtimeEffectError;
    std::string reason = "p2_observation_only";
    EntryPointObservation masterProcess;
    EntryPointObservation effectsChainProcessDsp;
};

// Entry-point patching is enabled only by the explicit environment switch and
// the locked host hash/prologue checks. The optional runtime effect remains
// disabled unless GPVST3_ENABLE_P2_EFFECT=1 is also present.
State prepare(const host::Verification &verification) noexcept;
State snapshot() noexcept;
void shutdown() noexcept;

}
