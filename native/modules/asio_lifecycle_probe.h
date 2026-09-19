#pragma once

#include <cstdint>
#include <QtCore/QJsonObject>

namespace gpvst3::hook::asioprobe {

struct CallbackIdentity {
    std::uint64_t generation = 0;
    std::uint64_t rateRevision = 0;
    double actualRate = 0;
    bool rateValidated = false;
};

enum class BindingState : std::uint8_t {
    Uninstalled, Unbound, Preparing, Running, Suspended, Retired, DrainOverdue, WrongThread, HostLimited
};
enum class BindingLimit : std::uint8_t {
    None, ProxyCapacityExhausted, UnsupportedCallbacks
};
struct StreamIdentity {
    CallbackIdentity callback;
    BindingState binding = BindingState::Uninstalled;
    BindingLimit limit = BindingLimit::None;
    bool bound = false;
    bool lifetimeProtected = false;
    std::uint64_t driverFrames = 0;
};

// Call only after the full AMAudio module hash gate, on the control thread.
// Bound registrations seal/drain before native Close/Dispose. After the 2 s
// diagnostic deadline they keep the caller parked until readers reach zero:
// GP ignores close errors, so returning early would let it free borrowed data.
// A callback that never returns consequently requires process restart.
bool install(void *audioModule) noexcept;
// Only identifies callbacks received through a non-reused ASIO proxy.
CallbackIdentity currentCallback() noexcept;
// Control-thread-only copied identity for preparing an input slot. It grants no
// stream-pointer lifetime. Publish prepared work with this generation/revision
// and recheck against currentCallback() on the audio thread before using it.
StreamIdentity currentStream() noexcept;
// Control-thread-only: returns false when the observer is not installed.
// Queries serialize with intercepted lifecycle calls; never call from a driver
// callback, because native Stop/Close can be waiting for that callback to exit.
bool currentRate(double &rate, int &result) noexcept;
bool requestNativeReset() noexcept;
struct SrcObservation {
    std::uint32_t calls = 0;
    std::int64_t inputFrames = 0;
    std::int64_t outputFrames = 0;
};
using SrcInputObserver = void (*)(const float *, const void *, std::int64_t) noexcept;
bool beginOutputSrc(const void *src, SrcInputObserver observer = nullptr) noexcept;
SrcObservation finishOutputSrc() noexcept;
QJsonObject snapshot();
#ifdef GPVST3_P13_PROBE_BUILD
void configureTiming(std::uint64_t start) noexcept;
bool stopTiming() noexcept;
QJsonObject timingSnapshot();
#endif

} // namespace gpvst3::hook::asioprobe
