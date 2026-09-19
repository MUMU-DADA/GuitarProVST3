#include "asio_lifecycle_probe.h"

#include <Windows.h>
#include <MinHook.h>
#include <QtCore/QJsonArray>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>
#ifdef GPVST3_P13_PROBE_BUILD
#include "input_timing_probe_json.h"
#endif

namespace gpvst3::hook::asioprobe {
namespace {

constexpr std::size_t kGenerations = 64;
constexpr int kPaInternalError = -9986;
constexpr int kAsioInvalidMode = -997;
constexpr auto kDrainTimeout = std::chrono::milliseconds(2000);
using Switch = void (*)(std::int32_t, std::int32_t);
using RateChanged = void (*)(double);
using Message = std::int32_t (*)(std::int32_t, std::int32_t, void *, double *);
using TimeSwitch = void *(*)(void *, std::int32_t, std::int32_t);
struct Callbacks { Switch buffer; RateChanged rate; Message message; TimeSwitch time; };
static_assert(sizeof(Callbacks) == 32);
using CreateBuffers = std::int32_t (*)(void *, std::int32_t, std::int32_t, Callbacks *);
using StreamCall = std::int32_t (*)(void *);
using DisposeBuffers = std::int32_t (*)();
using SrcProcess = std::int64_t (*)(void *, const float *, const void *, float *, const void *, std::int64_t);

struct Context {
    Callbacks original{};
    std::uint64_t driverFrames = 0;
    const void *bufferInfos = nullptr;
    std::atomic<bool> accepting{false};
    std::atomic<bool> audioAccepting{false};
    std::atomic<bool> created{false};
    std::atomic<bool> started{false};
    std::atomic<std::uint64_t> readers{0}, callbacks{0}, rejected{0};
    std::atomic<std::uint64_t> revision{1}, queriedRevision{0}, rateBits{0};
    std::atomic<std::uint64_t> rateEvents{0}, resetEvents{0};
#ifdef GPVST3_P13_PROBE_BUILD
    std::atomic<std::uint64_t> overloadEvents{0}, resyncEvents{0};
#endif
    std::atomic<std::uintptr_t> stream{0};
    std::atomic<int> rateResult{-1}, createResult{-1}, startResult{-1};
    std::atomic<int> stopResult{-1}, abortResult{-1}, closeResult{-1};
    std::atomic<int> disposeResult{-1};
    std::atomic<std::uint64_t> readersAtStop{0}, readersAtClose{0};
    std::atomic<std::uint64_t> readersAtDispose{0}, drainTimeouts{0};
    bool closing = false, disposing = false;
};
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

struct Runtime {
    std::recursive_mutex control;
    std::array<Context, kGenerations> contexts{};
    std::atomic<std::size_t> count{0};
    std::atomic<Context *> current{nullptr};
    std::atomic<bool> installed{false};
    std::atomic<bool> drainOverdue{false};
    bool installAttempted = false;
    BindingLimit bindingLimit = BindingLimit::None;
    std::atomic<std::uint64_t> exhausted{0}, unattachedControls{0};
    std::uint8_t *module = nullptr;
    CreateBuffers create = nullptr;
    StreamCall start = nullptr, stop = nullptr, abort = nullptr, close = nullptr;
    DisposeBuffers dispose = nullptr;
    SrcProcess srcProcess = nullptr;
    std::atomic<std::uint64_t> requestedResets{0};
    std::atomic<std::uint64_t> rejectedControls{0};
    const char *error = "not_requested";
    std::array<std::uintptr_t, 4> lastCallbacks{};
};

// Driver callbacks can arrive late. Contexts, proxy addresses and original
// trampolines are never reused or freed during the process lifetime.
Runtime &runtime() { static auto *value = new Runtime; return *value; }
thread_local Context *callbackContext = nullptr;
thread_local const void *observedOutputSrc = nullptr;
thread_local SrcObservation srcObservation;
thread_local SrcInputObserver srcInputObserver = nullptr;
#ifdef GPVST3_P13_PROBE_BUILD
input::timingprobe::Recorder driverTiming;
std::uint64_t timingNow() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
input::timingprobe::Identity timingIdentity() noexcept {
    const auto identity = currentCallback();
    return {identity.generation, identity.rateRevision, identity.actualRate, identity.rateValidated};
}
class DriverTimingScope final {
public:
    DriverTimingScope(std::uint64_t start, std::uint64_t frames) noexcept {
        driverTiming.begin(ticket_, start, frames, 0, timingIdentity());
    }
    ~DriverTimingScope() { if (ticket_.active) driverTiming.finish(ticket_, timingNow(), 0, timingIdentity()); }
private:
    input::timingprobe::Recorder::Ticket ticket_;
};
#endif

std::int64_t srcHook(void *self, const float *input, const void *inputFormat,
                     float *output, const void *outputFormat, std::int64_t frames) {
    if (self == observedOutputSrc && observedOutputSrc && srcInputObserver)
        srcInputObserver(input, inputFormat, frames);
    const auto result = runtime().srcProcess(self, input, inputFormat, output, outputFormat, frames);
    if (self == observedOutputSrc && observedOutputSrc) {
        if (srcObservation.calls != (std::numeric_limits<std::uint32_t>::max)()) ++srcObservation.calls;
        srcObservation.inputFrames = frames;
        srcObservation.outputFrames = result;
    }
    return result;
}

class Borrow final {
public:
    Borrow(const Borrow &) = delete;
    Borrow &operator=(const Borrow &) = delete;
    explicit Borrow(Context &context, bool audio = false) noexcept : context_(context), previous_(callbackContext) {
        if (!context_.accepting.load(std::memory_order_seq_cst)) return;
        if (audio && !context_.audioAccepting.load(std::memory_order_seq_cst)) return;
        context_.readers.fetch_add(1, std::memory_order_seq_cst);
        if (!context_.accepting.load(std::memory_order_seq_cst) ||
            (audio && !context_.audioAccepting.load(std::memory_order_seq_cst))) {
            context_.readers.fetch_sub(1, std::memory_order_seq_cst);
            return;
        }
        accepted_ = true;
        callbackContext = &context_;
    }
    ~Borrow() {
        if (accepted_) {
            callbackContext = previous_;
            context_.readers.fetch_sub(1, std::memory_order_seq_cst);
        } else context_.rejected.fetch_add(1, std::memory_order_relaxed);
    }
    explicit operator bool() const noexcept { return accepted_; }
private:
    Context &context_;
    Context *previous_;
    bool accepted_ = false;
};

template <std::size_t Index> void bufferProxy(std::int32_t index, std::int32_t direct) {
#ifdef GPVST3_P13_PROBE_BUILD
    const auto started = timingNow();
#endif
    auto &context = runtime().contexts[Index];
    Borrow borrow(context, true);
    if (!borrow) return;
#ifdef GPVST3_P13_PROBE_BUILD
    DriverTimingScope timing(started, context.driverFrames);
#endif
    context.callbacks.fetch_add(1, std::memory_order_relaxed);
    context.original.buffer(index, direct);
}
template <std::size_t Index> void rateProxy(double rate) {
    auto &context = runtime().contexts[Index];
    Borrow borrow(context);
    if (!borrow) return;
    context.revision.fetch_add(1, std::memory_order_seq_cst);
    context.rateEvents.fetch_add(1, std::memory_order_relaxed);
    context.original.rate(rate);
}
template <std::size_t Index> std::int32_t messageProxy(std::int32_t selector, std::int32_t value,
                                                     void *message, double *optional) {
    auto &context = runtime().contexts[Index];
    Borrow borrow(context);
    if (!borrow) return 0;
    // kAsioResetRequest / kAsioBufferSizeChange / kAsioResyncRequest.
    if (selector == 3 || selector == 4 || selector == 5) {
        context.revision.fetch_add(1, std::memory_order_seq_cst);
        context.resetEvents.fetch_add(1, std::memory_order_relaxed);
    }
#ifdef GPVST3_P13_PROBE_BUILD
    if (selector == 5) context.resyncEvents.fetch_add(1, std::memory_order_relaxed);
    if (selector == 15) context.overloadEvents.fetch_add(1, std::memory_order_relaxed);
#endif
    return context.original.message(selector, value, message, optional);
}
template <std::size_t Index> void *timeProxy(void *time, std::int32_t index, std::int32_t direct) {
#ifdef GPVST3_P13_PROBE_BUILD
    const auto started = timingNow();
#endif
    auto &context = runtime().contexts[Index];
    Borrow borrow(context, true);
    if (!borrow) return nullptr;
#ifdef GPVST3_P13_PROBE_BUILD
    DriverTimingScope timing(started, context.driverFrames);
#endif
    context.callbacks.fetch_add(1, std::memory_order_relaxed);
    return context.original.time(time, index, direct);
}
template <std::size_t... Index> constexpr auto proxyTable(std::index_sequence<Index...>) {
    return std::array<Callbacks, sizeof...(Index)>{{
        {&bufferProxy<Index>, &rateProxy<Index>, &messageProxy<Index>, &timeProxy<Index>}...}};
}
auto proxies = proxyTable(std::make_index_sequence<kGenerations>{});

bool rejectCallbackControl() noexcept {
    if (!callbackContext) return false;
    runtime().rejectedControls.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void seal(Context &context) noexcept {
    context.accepting.store(false, std::memory_order_seq_cst);
    context.audioAccepting.store(false, std::memory_order_seq_cst);
    context.started.store(false, std::memory_order_release);
    context.revision.fetch_add(1, std::memory_order_seq_cst);
}

// Only called while holding control. A callback never takes control, including
// reentrant requests to intercepted lifecycle functions, which fail beforehand.
void drain(Context &context) noexcept {
    const auto deadline = std::chrono::steady_clock::now() + kDrainTimeout;
    bool timedOut = false;
    while (context.readers.load(std::memory_order_seq_cst) != 0) {
        if (!timedOut && std::chrono::steady_clock::now() >= deadline) {
            context.drainTimeouts.fetch_add(1, std::memory_order_relaxed);
            runtime().drainOverdue.store(true, std::memory_order_release);
            timedOut = true;
            // Do not return to the caller while borrowed GP userData still
            // exists: GP ignores Close failure and can destroy its owner. The
            // host thread remains parked until the callback exits; a permanently
            // stuck third-party callback requires process restart. This is not
            // a callback/control-lock cycle: callbacks never acquire control.
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    runtime().drainOverdue.store(false, std::memory_order_release);
}

void queryRate(Context &context, std::uint64_t expectedRevision) {
    context.queriedRevision.store(0, std::memory_order_release);
    const auto revision = context.revision.load(std::memory_order_seq_cst);
    if (revision != expectedRevision) return;
    double rate = std::numeric_limits<double>::quiet_NaN();
    const auto query = reinterpret_cast<std::int32_t (*)(double *)>(runtime().module + 0x6A540);
    const int result = query(&rate);
    context.rateResult.store(result, std::memory_order_relaxed);
    if (result != 0 || !std::isfinite(rate) || rate < 8000 || rate > 768000 ||
        revision != context.revision.load(std::memory_order_seq_cst)) return;
    std::uint64_t bits = 0;
    std::memcpy(&bits, &rate, sizeof(bits));
    context.rateBits.store(bits, std::memory_order_relaxed);
    context.queriedRevision.store(revision, std::memory_order_release);
}

Context *forStream(void *stream) {
    auto &state = runtime();
    auto *context = state.current.load(std::memory_order_acquire);
    if (!stream || !context || !context->created.load(std::memory_order_acquire)) return nullptr;
    const auto address = reinterpret_cast<std::uintptr_t>(stream);
    if (context->stream.load(std::memory_order_acquire) == 0) {
        // A successfully opened stream may be closed without ever starting.
        // Match the buffers actually registered by this context before binding.
        const void *buffers = nullptr;
        std::memcpy(&buffers, static_cast<const std::uint8_t *>(stream) + 0x180, sizeof(buffers));
        if (!context->bufferInfos || buffers != context->bufferInfos) return nullptr;
        context->stream.store(address, std::memory_order_release);
    }
    return context->stream.load(std::memory_order_acquire) == address ? context : nullptr;
}

std::int32_t createHook(void *buffers, std::int32_t channels, std::int32_t frames, Callbacks *callbacks) {
    auto &state = runtime();
    if (rejectCallbackControl()) return kAsioInvalidMode;
    std::lock_guard<std::recursive_mutex> lock(state.control);
    if (!state.installed.load(std::memory_order_acquire))
        return state.create(buffers, channels, frames, callbacks);
    const auto index = state.count.load(std::memory_order_relaxed);
    // Open replaces the SDK default rate callback from the host-specific
    // stream info at 0x6E853 / 0x6E95F. GP supplies its Qt reset dispatcher.
    const auto rateCallback = callbacks && callbacks->rate == reinterpret_cast<RateChanged>(state.module + 0x9850)
        ? reinterpret_cast<RateChanged>(state.module + 0x9850)
        : reinterpret_cast<RateChanged>(state.module + 0x6DBC0);
    const Callbacks expected{reinterpret_cast<Switch>(state.module + 0x6D610), rateCallback,
        reinterpret_cast<Message>(state.module + 0x6DBD0),
        reinterpret_cast<TimeSwitch>(state.module + 0x6D6C0)};
    if (callbacks) std::memcpy(state.lastCallbacks.data(), callbacks, sizeof(Callbacks));
    if (index == kGenerations || !callbacks || std::memcmp(callbacks, &expected, sizeof(expected)) != 0) {
        state.exhausted.fetch_add(1, std::memory_order_relaxed);
        state.bindingLimit = index == kGenerations ? BindingLimit::ProxyCapacityExhausted : BindingLimit::UnsupportedCallbacks;
        state.current.store(nullptr, std::memory_order_release);
        return state.create(buffers, channels, frames, callbacks);
    }
    state.bindingLimit = BindingLimit::None;
    auto &context = state.contexts[index];
    context.original = *callbacks;
    context.driverFrames = frames > 0 ? static_cast<std::uint64_t>(frames) : 0;
    context.bufferInfos = buffers;
    // Driver negotiation callbacks are needed during createBuffers. Audio
    // callbacks cannot touch the native stream before Open finishes and Start
    // begins: failed Open frees its stream before calling disposeBuffers.
    context.audioAccepting.store(false, std::memory_order_seq_cst);
    context.accepting.store(true, std::memory_order_seq_cst);
    state.count.store(index + 1, std::memory_order_release);
    state.current.store(&context, std::memory_order_release);
    const auto result = state.create(buffers, channels, frames, &proxies[index]);
    context.createResult.store(result, std::memory_order_relaxed);
    context.created.store(result == 0, std::memory_order_release);
    if (result != 0) context.accepting.store(false, std::memory_order_seq_cst);
    return result;
}

std::int32_t startHook(void *stream) {
    auto &state = runtime();
    if (rejectCallbackControl()) return kPaInternalError;
    std::lock_guard<std::recursive_mutex> lock(state.control);
    if (!state.installed.load(std::memory_order_acquire)) return state.start(stream);
    auto *context = forStream(stream);
    std::uint64_t startRevision = 0;
    if (context) {
        if (!context->accepting.load(std::memory_order_seq_cst)) return kPaInternalError;
        startRevision = context->revision.fetch_add(1, std::memory_order_seq_cst) + 1;
        context->started.store(false, std::memory_order_release);
        queryRate(*context, startRevision);
        context->audioAccepting.store(true, std::memory_order_seq_cst);
    } else state.unattachedControls.fetch_add(1, std::memory_order_relaxed);
    const auto result = state.start(stream);
    if (context) {
        context->startResult.store(result, std::memory_order_relaxed);
        // A notification during Start belongs to an invalidated configuration.
        // Do not let a later query in this same Start silently bless its reset.
        if (result == 0) queryRate(*context, startRevision);
        else context->audioAccepting.store(false, std::memory_order_seq_cst);
        context->started.store(result == 0, std::memory_order_release);
    }
    return result;
}

std::int32_t stopOrAbort(void *stream, bool aborting) {
    auto &state = runtime();
    if (rejectCallbackControl()) return kPaInternalError;
    std::lock_guard<std::recursive_mutex> lock(state.control);
    if (!state.installed.load(std::memory_order_acquire))
        return (aborting ? state.abort : state.stop)(stream);
    auto *context = forStream(stream);
    if (context) {
        context->started.store(false, std::memory_order_release);
        context->revision.fetch_add(1, std::memory_order_seq_cst);
    } else state.unattachedControls.fetch_add(1, std::memory_order_relaxed);
    const auto result = (aborting ? state.abort : state.stop)(stream);
    if (context) {
        (aborting ? context->abortResult : context->stopResult).store(result, std::memory_order_relaxed);
        context->readersAtStop.store(context->readers.load(std::memory_order_seq_cst), std::memory_order_relaxed);
    }
    return result;
}
std::int32_t stopHook(void *stream) { return stopOrAbort(stream, false); }
std::int32_t abortHook(void *stream) { return stopOrAbort(stream, true); }
std::int32_t closeHook(void *stream) {
    auto &state = runtime();
    if (rejectCallbackControl()) return kPaInternalError;
    std::lock_guard<std::recursive_mutex> lock(state.control);
    if (!state.installed.load(std::memory_order_acquire)) return state.close(stream);
    auto *context = forStream(stream);
    if (context) {
        if (context->closing) return kPaInternalError;
        seal(*context);
        context->readersAtClose.store(context->readers.load(std::memory_order_seq_cst), std::memory_order_relaxed);
        drain(*context);
        context->closing = true;
    } else state.unattachedControls.fetch_add(1, std::memory_order_relaxed);
    // Every admitted proxy has returned and new entry is sealed. Never return an
    // early timeout: GP discards its stream handle even on Close failure, and
    // can then destroy callback userData or the ASIO host API behind our back.
    const auto result = state.close(stream);
    if (context) {
        context->closing = false;
        context->closeResult.store(result, std::memory_order_relaxed);
    }
    return result;
}

std::int32_t disposeHook() {
    auto &state = runtime();
    if (rejectCallbackControl()) return kAsioInvalidMode;
    std::lock_guard<std::recursive_mutex> lock(state.control);
    if (!state.installed.load(std::memory_order_acquire)) return state.dispose();
    // Open's error cleanup disposes successfully created buffers without calling
    // Close. No stream dereference is valid here: cleanup already freed it.
    auto *context = state.current.load(std::memory_order_acquire);
    if (context) {
        if (context->disposing) return kAsioInvalidMode;
        seal(*context);
        context->readersAtDispose.store(context->readers.load(std::memory_order_seq_cst), std::memory_order_relaxed);
        drain(*context);
        context->disposing = true;
    }
    const auto result = state.dispose();
    if (context) {
        context->disposing = false;
        context->disposeResult.store(result, std::memory_order_relaxed);
    }
    return result;
}

} // namespace

bool install(void *audioModule) noexcept {
    auto &state = runtime();
    if (rejectCallbackControl()) return false;
    std::lock_guard<std::recursive_mutex> lock(state.control);
    if (state.installed.load(std::memory_order_acquire)) return true;
    if (!audioModule || state.installAttempted) return false;
    state.installAttempted = true;
    state.module = static_cast<std::uint8_t *>(audioModule);
    struct Entry { std::uintptr_t rva; void *detour; void **original; const char *bytes; std::size_t size; };
    const Entry entries[]{
        {0x6A310, reinterpret_cast<void *>(&createHook), reinterpret_cast<void **>(&state.create),
            "\x48\x83\xEC\x38\x4C\x8B\xD1\x48\x8B\x0D", 10},
        {0x6A370, reinterpret_cast<void *>(&disposeHook), reinterpret_cast<void **>(&state.dispose),
            "\x48\x8B\x0D\x41\x7E\x28\x00\x48\x85\xC9", 10},
        {0x6FE90, reinterpret_cast<void *>(&startHook), reinterpret_cast<void **>(&state.start),
            "\x40\x55\x56\x41\x56\x48\x83\xEC\x40\x45\x33\xF6", 12},
        {0x700A0, reinterpret_cast<void *>(&stopHook), reinterpret_cast<void **>(&state.stop),
            "\x48\x89\x5C\x24\x10\x56\x48\x83\xEC\x20\x48\x8B\xD9", 13},
        {0x6DCB0, reinterpret_cast<void *>(&abortHook), reinterpret_cast<void **>(&state.abort),
            "\x48\x89\x5C\x24\x10\x56\x48\x83\xEC\x20\x48\x89\x7C\x24\x30", 15},
        {0x6DF60, reinterpret_cast<void *>(&closeHook), reinterpret_cast<void **>(&state.close),
            "\x40\x53\x48\x83\xEC\x20\x48\x8B\xD9\x48\x83\xC1\x68", 13},
        {0x51890, reinterpret_cast<void *>(&srcHook), reinterpret_cast<void **>(&state.srcProcess),
            "\x48\x89\x5C\x24\x10\x48\x89\x6C\x24\x18\x48\x89\x74\x24\x20", 15}
    };
    for (const auto &entry : entries) {
        if (std::memcmp(state.module + entry.rva, entry.bytes, entry.size) != 0) {
            state.error = "lifecycle_prologue_mismatch";
            return false;
        }
    }
    const auto initialized = MH_Initialize();
    if (initialized != MH_OK && initialized != MH_ERROR_ALREADY_INITIALIZED) {
        state.error = "minhook_initialize_failed";
        return false;
    }
    std::size_t created = 0;
    for (const auto &entry : entries) {
        if (MH_CreateHook(state.module + entry.rva, entry.detour, entry.original) != MH_OK) {
            // None is enabled yet. Remove only hooks created by this attempt;
            // the failed target may already belong to another component.
            for (std::size_t i = 0; i < created; ++i) MH_RemoveHook(state.module + entries[i].rva);
            state.error = "minhook_create_failed";
            return false;
        }
        ++created;
    }
    std::array<void *, std::size(entries)> targets{};
    for (std::size_t i = 0; i < targets.size(); ++i) targets[i] = state.module + entries[i].rva;
    MH_STATUS enabled = MH_ERROR_THREAD_BUSY;
    for (int attempt = 0; attempt < 16 && enabled == MH_ERROR_THREAD_BUSY; ++attempt) {
        enabled = MH_EnableHooksStrict(targets.data(), UINT(targets.size()));
        if (enabled == MH_ERROR_THREAD_BUSY) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (enabled != MH_OK) {
        // Refuse inaccessible threads and occupied prologues before writing.
        // Keep every trampoline on error: a complete pass-through detour may
        // already be entered when page restoration or thread resume fails.
        // Never follow this with the upstream best-effort disable path.
        state.error = MH_StatusToString(enabled);
        return false;
    }
    state.error = "";
    state.installed.store(true, std::memory_order_release);
    return true;
}

namespace {
CallbackIdentity identityOf(const Context *context) noexcept {
    CallbackIdentity result;
    if (!context) return result;
    result.generation = std::uint64_t(context - runtime().contexts.data()) + 1;
    result.rateRevision = context->revision.load(std::memory_order_seq_cst);
    if (!context->started.load(std::memory_order_acquire)) return result;
    const auto queried = context->queriedRevision.load(std::memory_order_acquire);
    const auto bits = context->rateBits.load(std::memory_order_relaxed);
    std::memcpy(&result.actualRate, &bits, sizeof(bits));
    result.rateValidated = context->started.load(std::memory_order_acquire) &&
        context->accepting.load(std::memory_order_seq_cst) && queried == result.rateRevision &&
        result.rateRevision == context->revision.load(std::memory_order_seq_cst) && result.actualRate > 0;
    if (!result.rateValidated) result.actualRate = 0;
    return result;
}
}

CallbackIdentity currentCallback() noexcept { return identityOf(callbackContext); }

StreamIdentity currentStream() noexcept {
    StreamIdentity result;
    if (rejectCallbackControl()) { result.binding = BindingState::WrongThread; return result; }
    auto &state = runtime();
    if (state.drainOverdue.load(std::memory_order_acquire)) {
        result.binding = BindingState::DrainOverdue;
        return result;
    }
    std::lock_guard<std::recursive_mutex> lock(state.control);
    if (!state.installed.load(std::memory_order_acquire)) return result;
    if (state.bindingLimit != BindingLimit::None) {
        result.binding = BindingState::HostLimited;
        result.limit = state.bindingLimit;
        return result;
    }
    result.binding = BindingState::Unbound;
    const auto *context = state.current.load(std::memory_order_acquire);
    if (context) {
        result.callback = identityOf(context);
        result.bound = context->created.load(std::memory_order_acquire) &&
            context->stream.load(std::memory_order_acquire) != 0;
        const bool accepting = context->accepting.load(std::memory_order_seq_cst);
        result.lifetimeProtected = accepting && result.bound;
        if (!accepting) result.binding = BindingState::Retired;
        else if (result.callback.rateValidated) result.binding = BindingState::Running;
        else if (context->startResult.load() == 0 && !context->started.load())
            result.binding = BindingState::Suspended;
        else result.binding = BindingState::Preparing;
    }
    if (state.drainOverdue.load(std::memory_order_acquire)) {
        result.binding = BindingState::DrainOverdue;
        result.lifetimeProtected = false;
        result.callback.actualRate = 0;
        result.callback.rateValidated = false;
    }
    return result;
}

bool currentRate(double &rate, int &result) noexcept {
    auto &state = runtime();
    if (rejectCallbackControl()) { rate = 0; result = kAsioInvalidMode; return true; }
    if (!state.installed.load(std::memory_order_acquire)) return false;
    if (state.drainOverdue.load(std::memory_order_acquire)) {
        rate = 0; result = kAsioInvalidMode; return true;
    }
    std::lock_guard<std::recursive_mutex> lock(state.control);
    rate = 0;
    result = -1;
    if (state.drainOverdue.load(std::memory_order_acquire)) { result = kAsioInvalidMode; return true; }
    void *stream = nullptr;
    std::memcpy(&stream, state.module + 0x2F2620, sizeof(stream));
    if (!stream) return true;
    rate = std::numeric_limits<double>::quiet_NaN();
    const auto query = reinterpret_cast<std::int32_t (*)(double *)>(state.module + 0x6A540);
    result = query(&rate);
    if (result != 0 || !std::isfinite(rate) || rate < 8000 || rate > 768000) rate = 0;
    return true;
}

bool requestNativeReset() noexcept {
    auto &state = runtime();
    if (rejectCallbackControl()) return false;
    if (!state.installed.load(std::memory_order_acquire)) return false;
    if (state.drainOverdue.load(std::memory_order_acquire)) return false;
    std::lock_guard<std::recursive_mutex> lock(state.control);
    if (state.drainOverdue.load(std::memory_order_acquire)) return false;
    void *stream = nullptr;
    std::memcpy(&stream, state.module + 0x2F2620, sizeof(stream));
    constexpr unsigned char dispatcher[]{0xE9, 0x4B, 0xFF, 0xFF, 0xFF};
    if (!stream || std::memcmp(state.module + 0x9850, dispatcher, sizeof(dispatcher)) != 0) return false;
    // GP's rate/reset callback queues its normal Qt stop/close/open/start.
    reinterpret_cast<void (*)()>(state.module + 0x9850)();
    state.requestedResets.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool beginOutputSrc(const void *src, SrcInputObserver observer) noexcept {
    if (!src || observedOutputSrc || !runtime().installed.load(std::memory_order_acquire)) return false;
    srcObservation = {};
    observedOutputSrc = src;
    srcInputObserver = observer;
    return true;
}
SrcObservation finishOutputSrc() noexcept {
    observedOutputSrc = nullptr;
    srcInputObserver = nullptr;
    return srcObservation;
}

QJsonObject snapshot() {
    if (rejectCallbackControl()) return {{"error", "callback_thread"}};
    auto &state = runtime();
    std::lock_guard<std::recursive_mutex> lock(state.control);
    QJsonArray generations;
    QJsonArray callbackRvas;
    for (const auto address : state.lastCallbacks)
        callbackRvas.append(QString::number(address - reinterpret_cast<std::uintptr_t>(state.module), 16));
    const auto count = state.count.load(std::memory_order_acquire);
    for (std::size_t i = 0; i < count; ++i) {
        const auto &context = state.contexts[i];
        const auto bits = context.rateBits.load(std::memory_order_relaxed);
        double rate = 0;
        std::memcpy(&rate, &bits, sizeof(bits));
        generations.append(QJsonObject{{"generation", qint64(i + 1)},
            {"stream", QString::number(context.stream.load(), 16)}, {"accepting", context.accepting.load()},
            {"started", context.started.load()}, {"callbacks", QString::number(context.callbacks.load())},
            {"rejected_callbacks", QString::number(context.rejected.load())},
            {"rate_revision", QString::number(context.revision.load())},
            {"queried_revision", QString::number(context.queriedRevision.load())},
            {"actual_rate", rate}, {"rate_result", context.rateResult.load()},
            {"create_result", context.createResult.load()}, {"start_result", context.startResult.load()},
            {"stop_result", context.stopResult.load()}, {"abort_result", context.abortResult.load()},
            {"close_result", context.closeResult.load()}, {"rate_events", QString::number(context.rateEvents.load())},
            {"dispose_result", context.disposeResult.load()},
            {"reset_events", QString::number(context.resetEvents.load())},
            {"readers_at_stop", QString::number(context.readersAtStop.load())},
            {"readers_at_close", QString::number(context.readersAtClose.load())},
            {"readers_at_dispose", QString::number(context.readersAtDispose.load())},
            {"drain_timeouts", QString::number(context.drainTimeouts.load())}});
    }
    return {{"installed", state.installed.load()}, {"error", state.error}, {"generations", generations},
        {"binding_limit", state.bindingLimit == BindingLimit::ProxyCapacityExhausted ? "proxy_capacity_exhausted" :
            state.bindingLimit == BindingLimit::UnsupportedCallbacks ? "unsupported_callbacks" : ""},
        {"last_callback_rvas", callbackRvas},
        {"requested_native_resets", QString::number(state.requestedResets.load())},
        {"drain_overdue", state.drainOverdue.load()},
        {"rejected_controls", QString::number(state.rejectedControls.load())},
        {"capacity", int(kGenerations)}, {"unattached_controls", QString::number(state.unattachedControls.load())},
        {"unwrapped_creates", QString::number(state.exhausted.load())},
        {"acceptance", "not_evaluated"},
        {"scope", "non_reused_callback_proxies; sealed_native_release; overdue_drain_never_releases_borrowed_memory"}};
}

#ifdef GPVST3_P13_PROBE_BUILD
void configureTiming(std::uint64_t start) noexcept { driverTiming.configure(true, start); }
bool stopTiming() noexcept { return driverTiming.stop(); }
QJsonObject timingSnapshot() {
    auto result = input::timingprobe::snapshotJson(driverTiming.snapshot(timingNow()),
        "ASIO_proxy_entry_through_original_driver_callback; excludes_final_proxy_reader_release_and_driver_scheduling",
        "ASIO_createBuffers_frames_divided_by_generation_validated_rate; execution_budget_exceedance_is_not_an_xrun_count");
    std::uint64_t overloads = 0, resyncs = 0;
    auto &state = runtime();
    const auto count = state.count.load(std::memory_order_acquire);
    for (std::size_t i = 0; i < count; ++i) {
        overloads += state.contexts[i].overloadEvents.load();
        resyncs += state.contexts[i].resyncEvents.load();
    }
    result.insert("asio_overload_notifications", QString::number(overloads));
    result.insert("asio_resync_notifications", QString::number(resyncs));
    result.insert("notification_scope", "all_observed_generations; driver_may_not_report_all_xruns");
    return result;
}
#endif

} // namespace gpvst3::hook::asioprobe
