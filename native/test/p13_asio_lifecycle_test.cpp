#define GPVST3_P13_PROBE_BUILD
#define MH_Initialize testInitialize
#define MH_CreateHook testCreateHook
#define MH_RemoveHook testRemoveHook
#define MH_EnableHooksStrict testEnableHooksStrict
#define MH_DisableHook testDisableHook
// Exercise the same private proxy and admission code used in the test DLL.
#include "../modules/asio_lifecycle_probe.cpp"

#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

namespace installation {
struct Hook { void *target; bool enabled = false; bool removed = false; };
std::vector<Hook> hooks;
int createCalls = 0, enableCalls = 0, removeCalls = 0, disableCalls = 0;
int failCreate = -1;
MH_STATUS enableResult = MH_OK;
bool touchedUnowned = false;
void reset() {
    hooks.clear();
    createCalls = enableCalls = removeCalls = disableCalls = 0;
    failCreate = -1;
    enableResult = MH_OK;
    touchedUnowned = false;
}
}

MH_STATUS WINAPI testInitialize() { return MH_OK; }
MH_STATUS WINAPI testCreateHook(void *target, void *, void **original) {
    if (installation::createCalls++ == installation::failCreate) return MH_ERROR_ALREADY_CREATED;
    installation::hooks.push_back({target});
    *original = target;
    return MH_OK;
}
MH_STATUS WINAPI testRemoveHook(void *target) {
    ++installation::removeCalls;
    for (auto &hook : installation::hooks) {
        if (hook.target != target) continue;
        hook.removed = true;
        return MH_OK;
    }
    installation::touchedUnowned = true;
    return MH_ERROR_NOT_CREATED;
}
MH_STATUS WINAPI testEnableHook(void *target) {
    ++installation::enableCalls;
    for (auto &hook : installation::hooks) {
        if (hook.target != target) continue;
        hook.enabled = true;
        return MH_OK;
    }
    installation::touchedUnowned = true;
    return MH_ERROR_NOT_CREATED;
}

MH_STATUS WINAPI testEnableHooksStrict(void *const *targets, UINT count) {
    ++installation::enableCalls;
    if (count != 7) installation::touchedUnowned = true;
    for (UINT i = 0; i < count; ++i) {
        bool owned = false;
        for (auto &hook : installation::hooks)
            if (hook.target == targets[i]) owned = true;
        if (!owned) installation::touchedUnowned = true;
    }
    if (installation::enableResult == MH_OK)
        for (auto &hook : installation::hooks) hook.enabled = true;
    return installation::enableResult;
}
MH_STATUS WINAPI testDisableHook(void *target) {
    ++installation::disableCalls;
    for (auto &hook : installation::hooks) {
        if (hook.target != target) continue;
        if (!hook.enabled) return MH_ERROR_DISABLED;
        hook.enabled = false;
        return MH_OK;
    }
    installation::touchedUnowned = true;
    return MH_ERROR_NOT_CREATED;
}

namespace {
using namespace gpvst3::hook::asioprobe;
std::atomic<bool> hold{false}, entered{false};
std::atomic<int> calls{0}, rateCalls{0}, messageCalls{0}, closeCalls{0}, disposeCalls{0};
CallbackIdentity observed;
double notifiedRate = 0;

void *fakeTime(void *time, std::int32_t index, std::int32_t direct) {
    if (index != 1 || direct != 1) std::abort();
    ++calls;
    observed = currentCallback();
    entered.store(true, std::memory_order_release);
    while (hold.load(std::memory_order_acquire)) std::this_thread::yield();
    return time;
}
void fakeBuffer(std::int32_t index, std::int32_t direct) { fakeTime(nullptr, index, direct); }
void fakeRate(double rate) { ++rateCalls; notifiedRate = rate; }
std::int32_t fakeMessage(std::int32_t selector, std::int32_t value, void *, double *) {
    ++messageCalls;
    return selector + value;
}
std::int32_t fakeClose(void *) { ++closeCalls; return 0; }
std::int32_t fakeDispose() { ++disposeCalls; return 0; }
std::int64_t fakeSrc(void *, const float *, const void *, float *, const void *, std::int64_t frames) {
    return frames < 0 ? frames : frames * 4;
}
std::atomic<int> srcObservations{0};
std::int64_t observedSrcFrames = 0;
void observeSrcInput(const float *, const void *, std::int64_t frames) noexcept {
    ++srcObservations;
    observedSrcFrames = frames;
}
int queryCalls = 0, notifyOnQuery = 0;
bool notifyOnStart = false;
int nativeStartResult = 0;
bool validatedDuringStart = false;
std::int32_t fakeQuery(double *rate) {
    *rate = 192000;
    if (++queryCalls == notifyOnQuery) rateProxy<6>(96000);
    return 0;
}
std::int32_t fakeStart(void *) {
    bufferProxy<6>(1, 1);
    validatedDuringStart = observed.rateValidated;
    if (notifyOnStart) messageProxy<6>(3, 0, nullptr, nullptr);
    return nativeStartResult;
}
std::int32_t creatingBuffers(void *, std::int32_t, std::int32_t, Callbacks *callbacks) {
    callbacks->buffer(1, 1);
    if (callbacks->time(nullptr, 1, 1) != nullptr) std::abort();
    return 0;
}

bool check(bool valid, const char *message) {
    if (!valid) std::cerr << "FAIL: " << message << '\n';
    return valid;
}

Context &prepareContext(std::size_t index, void *stream) {
    auto &state = runtime();
    auto &context = state.contexts[index];
    context.original = {fakeBuffer, fakeRate, fakeMessage, fakeTime};
    double rate = 192000;
    std::uint64_t bits = 0;
    std::memcpy(&bits, &rate, sizeof(bits));
    context.rateBits.store(bits);
    context.queriedRevision.store(1);
    context.stream.store(reinterpret_cast<std::uintptr_t>(stream));
    context.created.store(true);
    context.started.store(true);
    context.accepting.store(true);
    context.audioAccepting.store(true);
    state.drainOverdue.store(false);
    state.current.store(&context);
    state.installed.store(true);
    state.close = fakeClose;
    return context;
}

bool forwardingAndInvalidation() {
    int stream = 0, time = 0;
    auto &context = prepareContext(0, &stream);
    if (!check(timeProxy<0>(&time, 1, 1) == &time && observed.generation == 1 &&
               observed.rateValidated && observed.actualRate == 192000 && context.readers.load() == 0,
               "time callback forwards ABI and carries the exact generation/rate")) return false;
    bufferProxy<0>(1, 1);
    if (!check(calls.load() == 2 && currentCallback().generation == 0,
               "legacy callback forwards and TLS is restored")) return false;
    rateProxy<0>(96000);
    timeProxy<0>(&time, 1, 1);
    if (!check(rateCalls.load() == 1 && notifiedRate == 96000 && !observed.rateValidated &&
               observed.actualRate == 0 && observed.rateRevision == 2,
               "rate event invalidates old query before forwarding")) return false;
    if (!check(messageProxy<0>(3, 7, nullptr, nullptr) == 10 && messageCalls.load() == 1 &&
               context.revision.load() == 3, "reset event invalidates and preserves message return")) return false;
    if (!check(closeHook(&stream) == 0 && !context.accepting.load() &&
               context.readersAtClose.load() == 0, "close seals this registration")) return false;
    const auto previousCalls = calls.load();
    bufferProxy<0>(1, 1);
    if (!check(timeProxy<0>(&time, 1, 1) == nullptr, "late time callback rejected")) return false;
    rateProxy<0>(44100);
    if (!check(messageProxy<0>(3, 7, nullptr, nullptr) == 0 && context.rejected.load() == 4 &&
               calls.load() == previousCalls && rateCalls.load() == 1 && messageCalls.load() == 1,
               "late notifications cannot enter retired native callbacks")) return false;
    int nextStream = 0;
    prepareContext(1, &nextStream);
    timeProxy<1>(&time, 1, 1);
    if (!check(observed.generation == 2 && observed.rateValidated,
               "new proxy has independent identity")) return false;
    timeProxy<0>(&time, 1, 1);
    return check(observed.generation == 2 && context.rejected.load() == 5,
                 "old proxy cannot attach to a later registration");
}

bool closeWaitsForBorrowedCallback() {
    int stream = 0, time = 0;
    auto &context = prepareContext(2, &stream);
    hold.store(true);
    entered.store(false);
    std::thread reader([&] { timeProxy<2>(&time, 1, 1); });
    while (!entered.load(std::memory_order_acquire)) std::this_thread::yield();
    const auto previousClose = closeCalls.load();
    int result = -1;
    std::thread closer([&] { result = closeHook(&stream); });
    while (context.accepting.load()) std::this_thread::yield();
    const bool didNotRelease = closeCalls.load() == previousClose;
    const auto previousCallbacks = calls.load();
    timeProxy<2>(&time, 1, 1);
    const bool rejectedLate = calls.load() == previousCallbacks;
    hold.store(false, std::memory_order_release);
    reader.join();
    closer.join();
    return check(result == 0 && didNotRelease && rejectedLate && closeCalls.load() == previousClose + 1 &&
                 context.readers.load() == 0 && !context.accepting.load() && !runtime().drainOverdue.load(),
                 "Close seals new entry, waits for the admitted callback, then frees exactly once");
}

bool neverStartedIsRetired() {
    std::array<std::uint8_t, 0x188> stream{};
    int buffers = 0, otherBuffers = 0;
    auto &context = prepareContext(3, nullptr);
    context.started.store(false);
    context.bufferInfos = &buffers;
    const void *table = &otherBuffers;
    std::memcpy(stream.data() + 0x180, &table, sizeof(table));
    if (!check(forStream(stream.data()) == nullptr && context.stream.load() == 0,
               "pending registration cannot bind a different buffer table")) return false;
    table = &buffers;
    std::memcpy(stream.data() + 0x180, &table, sizeof(table));
    if (!check(closeHook(stream.data()) == 0 && !context.accepting.load() &&
               context.stream.load() == reinterpret_cast<std::uintptr_t>(stream.data()),
               "Close seals a successful CreateBuffers that never reached Start")) return false;
    const auto previousCalls = calls.load();
    bufferProxy<3>(1, 1);
    if (!check(calls.load() == previousCalls && context.rejected.load() == 1,
               "never-started registration rejects late callbacks after Close")) return false;
    auto &failedOpen = prepareContext(4, nullptr);
    failedOpen.started.store(false);
    runtime().dispose = fakeDispose;
    if (!check(disposeHook() == 0 && !failedOpen.accepting.load() && failedOpen.disposeResult.load() == 0,
               "Open failure disposal retires registration without dereferencing a freed stream")) return false;
    bufferProxy<4>(1, 1);
    return check(calls.load() == previousCalls && failedOpen.rejected.load() == 1,
                 "failed Open cannot deliver a callback into a later native stream");
}

bool audioCannotEnterIncompleteOpen() {
    auto &state = runtime();
    std::vector<std::uint8_t> module(0x71000);
    state.module = module.data();
    state.installed.store(true);
    state.create = creatingBuffers;
    state.count.store(12);
    const Callbacks expected{reinterpret_cast<Switch>(module.data() + 0x6D610),
        reinterpret_cast<RateChanged>(module.data() + 0x9850),
        reinterpret_cast<Message>(module.data() + 0x6DBD0),
        reinterpret_cast<TimeSwitch>(module.data() + 0x6D6C0)};
    auto callbacks = expected;
    int table = 0;
    const auto result = createHook(&table, 3, 64, &callbacks);
    auto &context = state.contexts[12];
    const bool passed = result == 0 && context.created.load() && context.accepting.load() &&
        !context.audioAccepting.load() && context.callbacks.load() == 0 && context.rejected.load() == 2;
    // The expected original addresses are intentionally not executable test
    // callbacks: reaching either would fail, exposing premature audio entry.
    state.dispose = fakeDispose;
    disposeHook();
    state.module = nullptr;
    return check(passed && !context.accepting.load(), "CreateBuffers never forwards audio before native Open is complete");
}

void heldRate(double) {
    entered.store(true, std::memory_order_release);
    while (hold.load(std::memory_order_acquire)) std::this_thread::yield();
}

bool disposeWaitsForNegotiation() {
    auto &context = prepareContext(7, nullptr);
    context.original.rate = heldRate;
    context.audioAccepting.store(false);
    runtime().dispose = fakeDispose;
    hold.store(true);
    entered.store(false);
    std::thread reader([] { rateProxy<7>(96000); });
    while (!entered.load()) std::this_thread::yield();
    const auto previousDisposes = disposeCalls.load();
    int result = -1;
    std::thread disposer([&] { result = disposeHook(); });
    while (context.accepting.load()) std::this_thread::yield();
    const bool heldDriver = disposeCalls.load() == previousDisposes;
    hold.store(false, std::memory_order_release);
    reader.join();
    disposer.join();
    return check(heldDriver && result == 0 && disposeCalls.load() == previousDisposes + 1,
                 "Dispose keeps driver alive until a create-time notification callback exits");
}

bool overdueDrainDoesNotReturnToUnsafeCaller() {
    auto &state = runtime();
    state.dispose = fakeDispose;
    for (int scenario = 0; scenario < 2; ++scenario) {
        int stream = 0;
        auto &context = prepareContext(8 + scenario, &stream);
        context.original.rate = heldRate;
        hold.store(true);
        entered.store(false);
        std::thread reader([&] { if (scenario == 0) rateProxy<8>(96000); else rateProxy<9>(96000); });
        while (!entered.load()) std::this_thread::yield();
        const auto previousClose = closeCalls.load();
        const auto previousDispose = disposeCalls.load();
        int result = 0;
        std::atomic<bool> returned{false};
        std::thread controller([&] {
            result = scenario == 0 ? closeHook(&stream) : disposeHook();
            returned.store(true, std::memory_order_release);
        });
        while (!state.drainOverdue.load(std::memory_order_acquire)) std::this_thread::yield();
        bool passed = !returned.load() && context.readers.load() == 1 && context.drainTimeouts.load() == 1 &&
            !context.accepting.load();
        passed = passed && closeCalls.load() == previousClose && disposeCalls.load() == previousDispose &&
            currentStream().binding == BindingState::DrainOverdue;
        hold.store(false, std::memory_order_release);
        reader.join();
        controller.join();
        passed = passed && returned.load() && result == 0 && !state.drainOverdue.load() &&
            closeCalls.load() == previousClose + (scenario == 0 ? 1 : 0) &&
            disposeCalls.load() == previousDispose + (scenario == 1 ? 1 : 0);
        if (!check(passed, "overdue drain parks unsafe caller until borrowed userData exits before native release")) return false;
    }
    return true;
}

std::atomic<bool> callControls{false};
bool callbackControlsRejected = false;
void *reentrantTime(void *time, std::int32_t, std::int32_t) {
    entered.store(true, std::memory_order_release);
    while (!callControls.load(std::memory_order_acquire)) std::this_thread::yield();
    int stream = 0;
    double rate = 1;
    int result = 0;
    callbackControlsRejected = closeHook(&stream) == kPaInternalError &&
        stopHook(&stream) == kPaInternalError && startHook(&stream) == kPaInternalError &&
        disposeHook() == kAsioInvalidMode && currentStream().binding == BindingState::WrongThread &&
        currentRate(rate, result) && result == kAsioInvalidMode && rate == 0 && !requestNativeReset();
    return time;
}

bool callbackReentryCannotDeadlockDrain() {
    int stream = 0, time = 0;
    auto &context = prepareContext(10, &stream);
    context.original.time = reentrantTime;
    entered.store(false);
    callControls.store(false);
    std::thread reader([&] { timeProxy<10>(&time, 1, 1); });
    while (!entered.load()) std::this_thread::yield();
    int result = -1;
    std::thread closer([&] { result = closeHook(&stream); });
    while (context.accepting.load()) std::this_thread::yield();
    callControls.store(true, std::memory_order_release);
    reader.join();
    closer.join();
    return check(callbackControlsRejected && result == 0 && !runtime().drainOverdue.load(),
                 "callback reentry fails before taking the control lock held by its drainer");
}

bool controlIdentityReflectsBinding() {
    int stream = 0;
    auto &context = prepareContext(11, &stream);
    auto identity = currentStream();
    if (!check(identity.binding == BindingState::Running && identity.bound && identity.lifetimeProtected &&
               identity.callback.generation == 12 && identity.callback.rateValidated &&
               identity.callback.actualRate == 192000, "control identity exposes a qualified bound generation")) return false;
    rateProxy<11>(96000);
    identity = currentStream();
    if (!check(identity.binding == BindingState::Preparing && !identity.callback.rateValidated &&
               identity.callback.rateRevision == 2, "rate notification immediately invalidates the preparation identity")) return false;
    closeHook(&stream);
    identity = currentStream();
    return check(identity.binding == BindingState::Retired && !identity.lifetimeProtected &&
                 !identity.callback.rateValidated && !context.accepting.load(),
                 "control identity cannot authorize a retired stream");
}

bool srcObservationOwnership() {
    auto &state = runtime();
    state.installed.store(true);
    state.srcProcess = fakeSrc;
    int inputSrc = 0, outputSrc = 0;
    if (!check(beginOutputSrc(&outputSrc, &observeSrcInput) && !beginOutputSrc(&inputSrc),
               "nested observer cannot replace active output SRC")) return false;
    srcHook(&inputSrc, nullptr, nullptr, nullptr, nullptr, 64);
    auto observation = finishOutputSrc();
    if (!check(observation.calls == 0 && srcObservations == 0, "input SRC is excluded from output observation")) return false;
    if (!beginOutputSrc(&outputSrc, &observeSrcInput)) return false;
    bool isolated = false;
    std::thread other([&] {
        if (!beginOutputSrc(&inputSrc)) return;
        srcHook(&inputSrc, nullptr, nullptr, nullptr, nullptr, 5);
        const auto local = finishOutputSrc();
        isolated = local.calls == 1 && local.inputFrames == 5 && local.outputFrames == 20;
    });
    other.join();
    if (!isolated) return false;
    srcHook(&outputSrc, nullptr, nullptr, nullptr, nullptr, 14);
    observation = finishOutputSrc();
    if (!check(observation.calls == 1 && observation.inputFrames == 14 && observation.outputFrames == 56 &&
               srcObservations == 1 && observedSrcFrames == 14,
               "actual SRC input/return observed with TLS isolation")) return false;
    if (!beginOutputSrc(&outputSrc)) return false;
    srcHook(&outputSrc, nullptr, nullptr, nullptr, nullptr, 14);
    srcHook(&outputSrc, nullptr, nullptr, nullptr, nullptr, -2);
    observation = finishOutputSrc();
    return check(observation.calls == 2 && observation.inputFrames == -2 && observation.outputFrames == -2,
                 "duplicate and invalid SRC returns remain visible for caller rejection");
}

bool installationFailuresAreInert() {
    std::vector<std::uint8_t> module(0x71000);
    const auto signature = [&](std::size_t rva, const char *bytes, std::size_t size) {
        std::memcpy(module.data() + rva, bytes, size);
    };
    signature(0x6A310, "\x48\x83\xEC\x38\x4C\x8B\xD1\x48\x8B\x0D", 10);
    signature(0x6A370, "\x48\x8B\x0D\x41\x7E\x28\x00\x48\x85\xC9", 10);
    signature(0x6FE90, "\x40\x55\x56\x41\x56\x48\x83\xEC\x40\x45\x33\xF6", 12);
    signature(0x700A0, "\x48\x89\x5C\x24\x10\x56\x48\x83\xEC\x20\x48\x8B\xD9", 13);
    signature(0x6DCB0, "\x48\x89\x5C\x24\x10\x56\x48\x83\xEC\x20\x48\x89\x7C\x24\x30", 15);
    signature(0x6DF60, "\x40\x53\x48\x83\xEC\x20\x48\x8B\xD9\x48\x83\xC1\x68", 13);
    signature(0x51890, "\x48\x89\x5C\x24\x10\x48\x89\x6C\x24\x18\x48\x89\x74\x24\x20", 15);
    auto &state = runtime();
    for (int failure = 0; failure < 7; ++failure) {
        installation::reset();
        state.installed.store(false);
        state.installAttempted = false;
        installation::failCreate = failure;
        if (!check(!install(module.data()) && !state.installed.load() &&
                   installation::removeCalls == failure && !installation::touchedUnowned &&
                   installation::enableCalls == 0,
                   "Create failure removes exactly the successfully owned, disabled hooks")) return false;
    }
    for (const auto failure : {MH_ERROR_THREAD_FREEZE, MH_ERROR_THREAD_BUSY, MH_ERROR_THREAD_RESUME,
                               MH_ERROR_MEMORY_PROTECT, MH_ERROR_CACHE_FLUSH, MH_ERROR_PATCH_ROLLBACK}) {
        installation::reset();
        state.installAttempted = false;
        installation::enableResult = failure;
        if (!check(!install(module.data()) && !state.installed.load() &&
                   installation::disableCalls == 0 && installation::removeCalls == 0 &&
                   !installation::touchedUnowned &&
                   installation::enableCalls == (failure == MH_ERROR_THREAD_BUSY ? 16 : 1),
                   "strict enable failure is inert, retains trampolines, and only retries occupied prologues")) return false;
        const auto attempts = installation::createCalls;
        if (!check(!install(module.data()) && installation::createCalls == attempts,
                   "failed installation is not retried over retained trampolines")) return false;
    }
    state.close = fakeClose;
    auto &context = state.contexts[5];
    context.created.store(true);
    context.accepting.store(true);
    int stream = 0;
    context.stream.store(reinterpret_cast<std::uintptr_t>(&stream));
    state.current.store(&context);
    const auto previousCloses = closeCalls.load();
    return check(closeHook(&stream) == 0 && closeCalls.load() == previousCloses + 1 &&
                 context.accepting.load(), "detour entered during failed install only forwards native behavior");
}

bool startNotificationsStayInvalid() {
    // Keep the same RVA-based query path as the DLL. Only the driver thunk is
    // replaced with a controlled x64 tail jump for deterministic interleavings.
    auto *module = static_cast<std::uint8_t *>(VirtualAlloc(nullptr, 0x71000,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!check(module != nullptr, "query-thunk test allocation")) return false;
    std::array<std::uint8_t, 12> jump{0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0};
    const auto destination = reinterpret_cast<std::uintptr_t>(&fakeQuery);
    std::memcpy(jump.data() + 2, &destination, sizeof(destination));
    std::memcpy(module + 0x6A540, jump.data(), jump.size());
    DWORD oldProtect = 0;
    if (!VirtualProtect(module, 0x71000, PAGE_EXECUTE_READ, &oldProtect)) {
        VirtualFree(module, 0, MEM_RELEASE);
        return check(false, "query-thunk test executable protection");
    }
    FlushInstructionCache(GetCurrentProcess(), module + 0x6A540, jump.size());
    auto &state = runtime();
    state.module = module;
    state.start = fakeStart;
    int stream = 0;
    bool passed = true;
    for (int scenario = 0; scenario < 5 && passed; ++scenario) {
        auto &context = prepareContext(6, &stream);
        context.revision.store(1);
        queryCalls = 0;
        notifyOnQuery = scenario == 1 ? 1 : scenario == 2 ? 2 : 0;
        notifyOnStart = scenario == 3;
        nativeStartResult = scenario == 4 ? -1 : 0;
        const auto result = startHook(&stream);
        bufferProxy<6>(1, 1);
        passed = check(result == nativeStartResult && !validatedDuringStart &&
                       observed.rateValidated == (scenario == 0),
                       "only a successful Start without intervening notifications validates rate");
        if (scenario == 1 || scenario == 3)
            passed = passed && check(queryCalls == 1, "Start does not requery and bless a pending reset");
    }
    state.module = nullptr;
    VirtualFree(module, 0, MEM_RELEASE);
    return passed;
}

Callbacks *forwardedTable = nullptr;
std::int32_t unwrappedCreate(void *, std::int32_t, std::int32_t, Callbacks *callbacks) {
    forwardedTable = callbacks;
    return 0;
}
bool exhaustedProxyIsExplicitlyHostLimited() {
    auto &state = runtime();
    std::vector<std::uint8_t> module(0x71000);
    state.module = module.data();
    state.create = unwrappedCreate;
    state.installed.store(true);
    state.count.store(kGenerations);
    Callbacks callbacks{reinterpret_cast<Switch>(state.module + 0x6D610),
        reinterpret_cast<RateChanged>(state.module + 0x9850),
        reinterpret_cast<Message>(state.module + 0x6DBD0),
        reinterpret_cast<TimeSwitch>(state.module + 0x6D6C0)};
    if (!check(createHook(nullptr, 2, 64, &callbacks) == 0 && forwardedTable == &callbacks,
               "capacity exhaustion preserves native callback registration")) return false;
    auto identity = currentStream();
    if (!check(identity.binding == BindingState::HostLimited &&
               identity.limit == BindingLimit::ProxyCapacityExhausted && !identity.bound &&
               !identity.lifetimeProtected && !identity.callback.rateValidated &&
               state.count.load() == kGenerations && state.current.load() == nullptr,
               "exhaustion cannot recycle a proxy or authorize overlay work")) return false;
    state.count.store(12);
    callbacks.buffer = nullptr;
    createHook(nullptr, 2, 64, &callbacks);
    identity = currentStream();
    return check(identity.binding == BindingState::HostLimited &&
                 identity.limit == BindingLimit::UnsupportedCallbacks && !identity.lifetimeProtected &&
                 state.count.load() == 12, "unsupported callback ABI has a distinct host limitation");
}
bool driverTimingAndNotifications() {
    int stream = 0, time = 0;
    auto &context = prepareContext(11, &stream);
    context.revision.store(1);
    context.driverFrames = 128;
    runtime().count.store(12);
    configureTiming(timingNow());
    bufferProxy<11>(1, 1);
    if (!check(timeProxy<11>(&time, 1, 1) == &time, "timed callback retains return value")) return false;
    const auto overloadBefore = context.overloadEvents.load();
    const auto resyncBefore = context.resyncEvents.load();
    if (!check(messageProxy<11>(15, 2, nullptr, nullptr) == 17 &&
               messageProxy<11>(5, 2, nullptr, nullptr) == 7,
               "overload/resync notifications retain native return values")) return false;
    stopTiming();
    bufferProxy<11>(1, 1);
    const auto timing = driverTiming.snapshot(timingNow());
    return check(timing.coherent && timing.stopped && timing.admitted == 2 && timing.completed == 2 &&
                 timing.validatedBudgets == 2 && timing.minimumFrames == 128 && timing.maximumRate == 192000 &&
                 context.overloadEvents.load() == overloadBefore + 1 && context.resyncEvents.load() == resyncBefore + 1,
                 "driver timing uses createBuffers frames and actual rate without changing forwarding");
}
} // namespace

int main() {
    if (!forwardingAndInvalidation() || !closeWaitsForBorrowedCallback() ||
        !neverStartedIsRetired() || !audioCannotEnterIncompleteOpen() || !startNotificationsStayInvalid() ||
        !disposeWaitsForNegotiation() || !overdueDrainDoesNotReturnToUnsafeCaller() ||
        !callbackReentryCannotDeadlockDrain() || !controlIdentityReflectsBinding() ||
        !srcObservationOwnership() || !installationFailuresAreInert() ||
        !exhaustedProxyIsExplicitlyHostLimited() || !driverTimingAndNotifications()) return 1;
    std::cout << "PASS: proxy ABI, generation isolation, rate invalidation, retirement, installation failure rollback.\n";
    return 0;
}
