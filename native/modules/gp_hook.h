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
    std::size_t frameCount = 0;
    std::size_t channelCount = 0;
    double sampleRate = 0.0;
    unsigned long threadId = 0;
};

struct State {
    bool installed = false;
    bool hostSupported = false;
    bool observationOnly = true;
    bool audioBufferAccessorsFound = false;
    std::string reason = "p2_observation_only";
    EntryPointObservation masterProcess;
    EntryPointObservation effectsChainProcessDsp;
};

// This pass only resolves symbols and records what can be observed safely.
// Patching a private C++ call site remains disabled until a runtime trace has
// proved the exact object layout, ownership and processing order.
State prepare(const host::Verification &verification) noexcept;

}
