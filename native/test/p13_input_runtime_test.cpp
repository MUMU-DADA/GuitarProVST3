// Production input runtime with synthetic lifecycle identities and real VST3
// processors. No host DLL, ASIO driver, listener hook or audio device is used.
#define GPVST3_P13_PROBE_BUILD 1
#include "p8_runtime_test.cpp" // Reuse host discovery and editor placement stubs.
#include <QtCore/QTemporaryDir>
#include <QtWidgets/QApplication>

namespace {
gpvst3::hook::asioprobe::StreamIdentity inputTestStream;
int inputTestInstalls = 0, inputTestResets = 0;
}
namespace gpvst3::hook::asioprobe {
bool install(void *) noexcept { ++inputTestInstalls; return false; }
CallbackIdentity currentCallback() noexcept { return inputTestStream.callback; }
StreamIdentity currentStream() noexcept { return inputTestStream; }
bool currentRate(double &rate, int &result) noexcept {
    rate = inputTestStream.callback.actualRate; result = 0; return inputTestStream.bound;
}
bool requestNativeReset() noexcept { ++inputTestResets; return false; }
bool beginOutputSrc(const void *, SrcInputObserver) noexcept { return false; }
SrcObservation finishOutputSrc() noexcept { return {}; }
QJsonObject snapshot() { return {{"fixture", "synthetic_lifecycle_no_host"}}; }
void configureTiming(std::uint64_t) noexcept {}
bool stopTiming() noexcept { return true; }
QJsonObject timingSnapshot() { return {{"coherent_snapshot", true}}; }
}

namespace {
using namespace gpvst3;
using namespace gpvst3::hook;
using Mode = state::InputMonitorMode;

void waitInput() {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    const auto pending = [] {
        return vst3SelectionPending() || g_inputMonitor.statusChanged.load() ||
            g_inputMonitor.reconfigureRequested.load();
    };
    while (pending() && std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    require(!pending(), "input worker drains within deadline");
}

void requestInput(const std::vector<Vst3SelectionEntry> &selection,
                  state::InputMonitorSettings settings) {
    std::string error;
    // These are separate UI requests, not one atomic operation. Settle the
    // settings first so a fast selection failure cannot be followed by an
    // unrelated successful settings request for its restored previous chain.
    require(requestInputMonitorSettings(settings, &error), "input settings accepted");
    waitInput();
    require(requestInputVst3Selection(selection, &error), "input selection accepted");
    waitInput();
}

Vst3SelectionEntry stateValue(Vst3SelectionEntry entry, double value) {
    entry.componentState.resize(sizeof(value));
    std::memcpy(entry.componentState.data(), &value, sizeof(value));
    entry.controllerState = entry.componentState;
    return entry;
}

double savedValue(const Vst3SelectionEntry &entry) {
    require(entry.componentState.size() == sizeof(double), "fixture saved state contains value");
    double value = 0;
    std::memcpy(&value, entry.componentState.data(), sizeof(value));
    return value;
}

InputMonitorSlot &inputSlot() {
    require(g_inputMonitor.retained >= 0, "input slot retained");
    return g_inputMonitor.monitorSlots[g_inputMonitor.retained];
}

void checkInputSamples(double inputGain, double monitorGain) {
    float capture[64], left[64], right[64];
    std::fill_n(capture, 64, 0.4F);
    std::fill_n(left, 64, 0.2F);
    std::fill_n(right, 64, -0.1F);
    const float *inputs[]{capture};
    float *outputs[]{left, right};
    const auto result = inputSlot().router.process(
        {inputs, 1, 64, 192000.0, 64}, {}, {outputs, 2});
    require(result.processed && !result.error, "independent input overlay processes fixture");
    require(std::abs(left[63] - float(0.2 + 0.4 * inputGain * monitorGain)) < 0.000001F &&
            std::abs(right[63] - float(-0.1 + 0.4 * inputGain * monitorGain)) < 0.000001F,
            "monitor gain scales input contribution and preserves both GP channels");
}

int zeroListenerCalls = 0;
std::int64_t zeroListener(void *, const float *input, std::uint32_t inputChannels,
                          float *output, std::uint32_t outputChannels, std::int64_t frames, const void *) {
    require(input == nullptr && output == g_monitorAudio.sink.data() && frames == 0 &&
            inputChannels == 2 && outputChannels == 2, "zero-frame call preserves input and uses owned sink");
    ++zeroListenerCalls;
    return frames;
}

std::atomic<int> latencyNotifications{0};
std::atomic<std::uint64_t> notifiedLatency{0};
void captureLatencyNotification() noexcept {
    notifiedLatency.store(inputMonitorSnapshot().value("plugin_latency_samples").toString().toULongLong());
    latencyNotifications.fetch_add(1);
}

void verifyInputLatencyReporting(const char *fixture) {
    requestInput({}, {Mode::Off, 0.25});
    // A distinct module identity prevents the warm cache from supplying
    // instances created before the reporting fixture was enabled.
    const auto latencyPath = fs::u8path(state::dataDirectory().toStdString()) / "latency" /
        fs::u8path(fixture).filename();
    std::error_code error;
    fs::create_directories(latencyPath.parent_path(), error);
    require(!error, "create latency fixture directory");
    fs::copy(fs::u8path(fixture), latencyPath, fs::copy_options::recursive, error);
    require(!error, "copy independent latency reporting fixture");
    qputenv("GPVST3_TEST_LATENCY_REPORTING", "1");
    const Vst3SelectionEntry offset{latencyPath.u8string(), "41302010605080701122334455667788"};
    const Vst3SelectionEntry gain{latencyPath.u8string(), "42302010605080701122334455667788"};
    requestInput({offset, gain}, {Mode::LowLatencyOverlay, 0.25});
    qunsetenv("GPVST3_TEST_LATENCY_REPORTING");
    const auto first = inputSlot().selection.effects[0];
    const auto second = inputSlot().selection.effects[1];
    require(first->processor->getLatencySamples() == 17 && second->processor->getLatencySamples() == 29 &&
                inputMonitorSnapshot().value("plugin_latency_samples").toString() == "46",
            "two serial input plugins report the sum of their distinct nonzero delays");
    const auto requestGeneration = g_runtime.inputRequestGeneration;
    const auto instancesCreated = g_nextInstanceId.load();
    setSelectionNotifier(&captureLatencyNotification);
    const auto changeLatency = [&](const std::shared_ptr<RuntimeEffect> &effect, double value,
                                   std::uint64_t expected) {
        const auto previousNotifications = latencyNotifications.load();
        require(effect->controller->setParamNormalized(2, value) == Steinberg::kResultOk,
                "plugin emits an accepted kLatencyChanged notification through its actual handler");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while ((latencyNotifications.load() == previousNotifications || notifiedLatency.load() != expected) &&
               std::chrono::steady_clock::now() < deadline) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            QThread::msleep(1);
        }
        require(latencyNotifications.load() > previousNotifications && notifiedLatency.load() == expected,
                "latency notification wakes the worker and delivers the updated serial total to the UI observer");
        waitInput();
        require(inputMonitorSnapshot().value("plugin_latency_samples").toString().toULongLong() == expected &&
                    g_runtime.inputRequestGeneration == requestGeneration && g_nextInstanceId.load() == instancesCreated &&
                    inputSlot().selection.effects[0] == first && inputSlot().selection.effects[1] == second,
                "latency-only updates neither reconfigure nor rebuild the input chain");
    };
    changeLatency(second, 0.1, 117);
    changeLatency(first, 0.0, 100);
    changeLatency(second, 0.0, 0);
    setSelectionNotifier(nullptr);
    std::cout << "PASS: serial input latency reports 17+29=46, then plugin kLatencyChanged delivers 117, 100 and 0 samples without rebuilding.\n";
}

void verifyInputRuntime(const char *fixture) {
    // Startup host discovery remains disabled. The worker sees only this
    // copied identity; callback/lifetime correctness is tested separately.
    inputTestStream.callback = {40, 2, 32000, true};
    inputTestStream.binding = asioprobe::BindingState::Running;
    inputTestStream.limit = asioprobe::BindingLimit::None;
    inputTestStream.bound = true;
    inputTestStream.lifetimeProtected = true;
    inputTestStream.driverFrames = 64;
    setNativeInputState(true, true);
    g_runtime.stream.installed = true;
    g_listenerProbeInstalled = true;
    const Vst3SelectionEntry gain{fixture, "42302010605080701122334455667788"};
    const auto initial = stateValue(gain, 0.5);
    requestInput({}, {Mode::LowLatencyOverlay, 0.5});
    require(!g_inputMonitor.exchange.suppressed() && g_inputMonitor.phase == 6 &&
                g_inputMonitor.error == "input_host_contract_unavailable",
            "unsupported stream rejects even a dry chain without claiming readiness");
    requestInput({initial}, {Mode::LowLatencyOverlay, 0.5});
    require(!g_inputMonitor.exchange.suppressed() && g_inputMonitor.exchange.watching() &&
                g_inputMonitor.phase == 6 && g_inputMonitor.desired.empty() &&
                g_runtime.pendingInputSelection.size() == 1,
            "first unsupported stream preserves native audio and pending plugin intent");
    inputTestStream.callback = {41, 3, 192000, true};
    {
        MonitorCallback callback(nullptr, nullptr, 64, nullptr, 99, 0);
        require(!callback.requested() && !callback.suppress(),
                "first recovery waits for preparation before suppressing native audio");
        callback.finish(0);
    }
    waitInput();
    require(g_inputMonitor.exchange.activeIndex() >= 0 && g_inputMonitor.error.empty(),
            "valid copied stream identity prepares and publishes independent input");
    const auto first = inputSlot().selection.effects[0];
    require(first->configuredRate == 192000 && inputSlot().generation == 41 && inputSlot().revision == 3,
            "input uses actual ASIO rate and copied generation/revision");
    require(first->processor->getLatencySamples() == 0 &&
                inputMonitorSnapshot().value("plugin_latency_samples").toString() == "0",
            "default gain fixture retains its zero-latency reporting contract");
    require(g_runtime.selectionPool.effects.empty() && g_runtime.inputSelectionPool.effects.empty(),
            "independent input does not construct global or legacy mirrors");
    checkInputSamples(0.5, 0.5);

    RuntimeEffect otherScope;
    const auto otherState = stateValue(gain, 0.25);
    require(otherScope.initialize(192000, 2048, fs::u8path(fixture), gain.classId, &otherState),
            "same plugin initializes independently in another scope");
    require(first.get() != &otherScope && first->queueParameter(1, 0.75), "edit input parameter");
    checkInputSamples(0.75, 0.5);
    require(savedValue(otherScope.captureState()) == 0.25 &&
                savedValue(captureInputVst3States().at(0)) == 0.75,
            "same identity has independent live parameter and saved state");

    std::string error;
    require(requestInputMonitorSettings({Mode::LowLatencyOverlay, 0.25}, &error), "gain update accepted");
    waitInput();
    require(inputSlot().selection.effects[0] == first && savedValue(g_inputMonitor.desired.at(0)) == 0.75,
            "monitor gain update retains processor and newest parameter state");
    checkInputSamples(0.75, 0.25);
    requestInput({initial}, {Mode::LowLatencyOverlay, 0.25});
    require(inputSlot().selection.effects[0] == first && savedValue(g_inputMonitor.desired.at(0)) == 0.75,
            "ordinary selection with old bytes cannot overwrite live input state");

    // An obsolete callback cannot replace a newer control status in the UI.
    {
        input::MonitorExchange::Lease oldCallback;
        g_inputMonitor.exchange.acquire(oldCallback);
        require(requestInputMonitorSettings({Mode::Off, 0.25}, &error), "Off accepted");
        waitInput();
        g_inputMonitor.publishCallbackPhase(oldCallback.token(), 4);
        waitInput();
        require(inputMonitorSnapshot().value("state") == "off", "stale active callback cannot undo Off state");
    }
    require(!g_inputMonitor.exchange.suppressed(), "Off releases suppression");

    // Preparing then failing before the first callback cannot mute the native
    // route. Once a callback has actually suppressed it, failure stays muted.
    auto missing = initial;
    missing.module = "C:/missing/P13 Missing.vst3";
    // Stream preparation can finish before its first callback. The callback's
    // last observed generation is deliberately older than the prepared slot.
    ++inputTestStream.callback.generation;
    requestInput({initial}, {Mode::LowLatencyOverlay, 0.25});
    requestInput({initial, missing}, {Mode::LowLatencyOverlay, 0.25});
    require(!g_inputMonitor.exchange.suppressed() && g_inputMonitor.phase == 6 &&
                g_inputMonitor.error == "runtime_vst3_not_found" && g_inputMonitor.desired.size() == 1 &&
                g_runtime.pendingInputSelection.size() == 1,
            "first prepare failure restores native route and rejects only new identity");
    const auto failedRequestGeneration = g_runtime.inputRequestGeneration;
    const auto failedRouteLegacy = g_inputMonitor.exchange.legacyFallback();
    for (int i = 0; i < 4; ++i) {
        {
            MonitorCallback callback(nullptr, nullptr, 64, nullptr, 200 + i, 0);
            require(!callback.requested() && !callback.suppress() && callback.legacy() == failedRouteLegacy,
                    "callback after plugin preparation failure preserves original native route");
            callback.finish(0);
        }
        waitInput();
    }
    require(!g_inputMonitor.exchange.watching() && !g_inputMonitor.exchange.suppressed() &&
                g_runtime.inputRequestGeneration == failedRequestGeneration &&
                g_inputMonitor.error == "runtime_vst3_not_found" && g_inputMonitor.phase == 6,
            "plugin preparation failure cannot retry the old chain or erase its error on the next callback");
    requestInput({initial}, {Mode::LowLatencyOverlay, 0.25});
    g_inputMonitor.nativeSuppressed = true;
    requestInput({initial, missing}, {Mode::LowLatencyOverlay, 0.25});
    require(g_inputMonitor.exchange.suppressed() && g_inputMonitor.exchange.activeIndex() == -1 &&
                g_inputMonitor.phase == 5 && g_inputMonitor.desired.size() == 1,
            "failure after actual activation mutes input and preserves its desired chain");
    require(savedValue(captureInputVst3States().at(0)) == 0.75,
            "failed replacement preserves latest input processor state");
    requestInput({}, {Mode::LowLatencyOverlay, 0.25});
    require(g_inputMonitor.exchange.suppressed() && g_inputMonitor.phase == 3 &&
                g_inputMonitor.error.empty() && g_inputMonitor.desired.empty(),
            "clearing an active input chain publishes a dry monitor processor");
    checkInputSamples(1.0, 0.25);
    setNativeInputState(true, false);
    {
        const auto processed = g_inputMonitor.blocks.load();
        MonitorCallback callback(reinterpret_cast<void *>(1), reinterpret_cast<void *>(1), 64,
            reinterpret_cast<void *>(1), 999, 0);
        require(!callback.requested() && !callback.suppress(), "host Off rejects even invalid borrowed audio before reading it");
        callback.finish(0);
        require(g_inputMonitor.blocks == processed && !g_inputMonitor.nativeSuppressed &&
            inputMonitorSnapshot().value("state") == "waiting_for_input", "host Off has zero input processing despite saved low-latency mode");
    }
    setNativeInputState(true, true);
    g_inputMonitor.nativeSuppressed = true;
    g_monitorAudio.suppressedGeneration = inputTestStream.callback.generation;
    g_monitorAudio.suppressedRevision = inputTestStream.callback.rateRevision;
    {
        MonitorCallback callback(nullptr, nullptr, 64, nullptr, 100, 0);
        require(callback.suppress(), "intentional mute keeps validated original stream suppression");
        std::vector<std::uint8_t> image(0x25ABB00);
        const auto *savedBase = g_listenerExecutableBase;
        const auto savedOriginal = g_runtime.listenerProbe.trampoline;
        g_listenerExecutableBase = image.data();
        const void *unit[]{image.data() + 0x25ABAF8};
        g_runtime.listenerProbe.trampoline = reinterpret_cast<void *>(&zeroListener);
        g_monitorCallback = &callback;
        g_inputMonitor.listenerFaultFlags = 0;
        require(listenerProbeHook(unit, nullptr, 2, nullptr, 2, 0, nullptr) == 0 &&
                zeroListenerCalls == 1 && g_inputMonitor.listenerFaultFlags == 0,
                "cold SRC zero frames forward exactly once without monitor fault");
        require(listenerProbeHook(unit, nullptr, 2, nullptr, 2, -1, nullptr) == 0 &&
                zeroListenerCalls == 1 && (g_inputMonitor.listenerFaultFlags.load() & 16),
                "negative listener frame count still rejected before native call");
        g_inputMonitor.listenerFaultFlags = 0;
        g_monitorCallback = nullptr;
        g_runtime.listenerProbe.trampoline = savedOriginal;
        g_listenerExecutableBase = savedBase;
        callback.finish(0);
    }
    ++inputTestStream.callback.generation;
    {
        MonitorCallback callback(nullptr, nullptr, 64, nullptr, 101, 0);
        require(!callback.suppress() && !g_inputMonitor.nativeSuppressed && g_inputMonitor.reconfigureRequested,
                "new stream cannot inherit an old stream's unvalidated native suppression");
        callback.finish(0);
    }
    requestInput({}, {Mode::Off, 0.25});
    g_inputMonitor.nativeSuppressed = false;

    requestInput({initial}, {Mode::LowLatencyOverlay, 0.25});
    const auto oldRateEffect = inputSlot().selection.effects[0];
    const auto oldGeneration = inputSlot().generation;
    ++inputTestStream.callback.generation;
    ++inputTestStream.callback.rateRevision;
    require(requestInputMonitorSettings({Mode::LowLatencyOverlay, 0.25}, &error), "new stream rebuild accepted");
    waitInput();
    require(inputSlot().generation == oldGeneration + 1 && inputSlot().revision == 4 &&
                inputSlot().selection.effects[0] == oldRateEffect,
            "new stream identity rebinds same-rate processor without losing state");
    {
        input::MonitorExchange::Lease validated;
        g_inputMonitor.exchange.acquire(validated);
        g_inputMonitor.actualRate = 192000;
        g_inputMonitor.frames = 64;
        g_inputMonitor.driverFrames = 64;
        g_inputMonitor.processFrames = 64;
        g_inputMonitor.channels = 1;
        g_inputMonitor.configurationToken = validated.token();
        require(inputMonitorSnapshot().value("configuration_validated").toBool() &&
                    inputMonitorSnapshot().value("sample_rate").toInt() == 192000,
                "diagnostic measurements require their current validated callback token");
    }
    const auto processingErrorsBeforeRateChange = g_inputMonitor.errors.load();
    const auto rejectedBlocksBeforeRateChange = g_inputMonitor.configurationRejectedBlocks.load();
    g_inputMonitor.callbackFault = 0;
    inputTestStream.callback.actualRate = 32000;
    ++inputTestStream.callback.rateRevision;
    {
        MonitorCallback callback(nullptr, nullptr, 64, nullptr, 102, 0);
        require(!callback.suppress(), "unsupported new rate cannot inherit suppression");
        callback.finish(0);
    }
    waitInput();
    const auto unsupportedSnapshot = inputMonitorSnapshot();
    require(!unsupportedSnapshot.value("configuration_validated").toBool() &&
                unsupportedSnapshot.value("sample_rate").toInt() == 0 &&
                unsupportedSnapshot.value("buffer_frames").toInt() == 0 &&
                unsupportedSnapshot.value("driver_buffer_frames").toInt() == 0 &&
                unsupportedSnapshot.value("process_frames").toInt() == 0 &&
                g_inputMonitor.errors.load() == processingErrorsBeforeRateChange &&
                g_inputMonitor.configurationRejectedBlocks.load() == rejectedBlocksBeforeRateChange + 1 &&
                g_inputMonitor.callbackFault.load() == 0,
            "rejected stream clears current format evidence and counts separately from processing faults");
    require(!g_inputMonitor.exchange.suppressed() && g_inputMonitor.phase == 6 &&
                g_inputMonitor.exchange.watching() &&
                g_inputMonitor.error == "input_host_contract_unavailable" &&
                inputSlot().selection.effects[0] == oldRateEffect,
            "unsupported rate cannot publish or reconfigure active processor");
    const auto rejectedGeneration = g_runtime.inputRequestGeneration;
    {
        MonitorCallback callback(nullptr, nullptr, 64, nullptr, 103, 0);
        require(!callback.requested() && !callback.suppress(),
                "rejected stream remains audible through its original native route");
        callback.finish(0);
    }
    waitInput();
    require(g_runtime.inputRequestGeneration == rejectedGeneration,
            "unchanged rejected identity cannot repeatedly requeue preparation");
    inputTestStream.callback.actualRate = 192000;
    ++inputTestStream.callback.generation;
    ++inputTestStream.callback.rateRevision;
    {
        MonitorCallback callback(nullptr, nullptr, 64, nullptr, 104, 0);
        require(!callback.requested() && !callback.suppress(),
                "passive recovery observes a new stream without premature suppression");
        callback.finish(0);
    }
    waitInput();
    require(g_runtime.inputRequestGeneration > rejectedGeneration &&
                g_inputMonitor.exchange.activeIndex() >= 0 && g_inputMonitor.error.empty() &&
                inputSlot().generation == inputTestStream.callback.generation &&
                inputSlot().revision == inputTestStream.callback.rateRevision &&
                inputSlot().selection.effects[0] == oldRateEffect,
            "new supported stream automatically recovers without another user request");

    // No callback runs between the Off request and this preparation failure.
    // A stale observation of the preceding active callback cannot reactivate
    // suppression after the user has already turned the mode off.
    g_inputMonitor.nativeSuppressed = true;
    requestInput({initial, missing}, {Mode::Off, 0.25});
    require(!g_inputMonitor.exchange.suppressed() && !g_inputMonitor.exchange.watching() &&
                inputMonitorSnapshot().value("state") == "off",
            "failed replacement after Off cannot republish a muted low-latency mode");
    g_inputMonitor.nativeSuppressed = false;
    requestInput({initial}, {Mode::LowLatencyOverlay, 0.25});

    // Force a cold plugin to fail after another request supersedes it. The
    // obsolete failure must not mute, reject, or replace the newer selection.
    const auto coldPath = fs::u8path(state::dataDirectory().toStdString()) / "cold" /
        fs::u8path(fixture).filename();
    std::error_code copyError;
    fs::create_directories(coldPath.parent_path(), copyError);
    require(!copyError, "create isolated cold fixture directory");
    fs::copy(fs::u8path(fixture), coldPath, fs::copy_options::recursive, copyError);
    require(!copyError, "copy isolated cold VST3");
    auto corrupt = initial;
    corrupt.module = coldPath.u8string();
    corrupt.componentState = {1, 2, 3};
    qputenv("GPVST3_TEST_INITIALIZE_DELAY_MS", "100");
    const auto instancesBefore = g_nextInstanceId.load();
    require(requestInputVst3Selection({corrupt}, &error), "cold invalid request queued");
    const auto startedDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (g_nextInstanceId.load() == instancesBefore && std::chrono::steady_clock::now() < startedDeadline) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
        QThread::msleep(1);
    }
    require(g_nextInstanceId.load() > instancesBefore && g_runtime.selectionWorkerBusy,
            "cold invalid request entered preparation");
    require(requestInputVst3Selection({initial}, &error) &&
                requestInputMonitorSettings({Mode::Off, 0.25}, &error), "superseding Off selection queued");
    waitInput();
    qunsetenv("GPVST3_TEST_INITIALIZE_DELAY_MS");
    require(!g_inputMonitor.exchange.suppressed() && g_inputMonitor.error.empty() &&
                inputMonitorSnapshot().value("state") == "off" && g_inputMonitor.desired.size() == 1 &&
                g_inputMonitor.desired.at(0).module == fixture,
            "superseded prepare failure cannot undo newest mode or selection");

    // Pause Qt after the old failure has updated its control state but before
    // its queued disk rejection runs. Installing the formerly missing plugin
    // and retrying the same identity makes that disk write obsolete too.
    // Discard dormant fixtures on Qt first so teardown cannot be the queued
    // dispatch this regression is deliberately holding back.
    g_inputMonitor.exchange.off();
    for (auto &slot : g_inputMonitor.monitorSlots) {
        slot.selection.shutdown();
        slot.pool.effects.clear();
    }
    g_inputMonitor.retained = -1;
    g_inputMonitor.desired.clear();
    g_inputMonitor.phase = 0;
    const auto installedPath = fs::u8path(state::dataDirectory().toStdString()) / "installed-later" /
        fs::u8path(fixture).filename();
    auto installedLater = initial;
    installedLater.module = installedPath.u8string();
    QJsonObject saved;
    require(state::loadChain(saved), "load isolated saved input state");
    state::setScopeEffects(saved, state::ScopeKind::Input, QJsonArray{QJsonObject{
        {"module", QString::fromStdString(installedLater.module)}, {"class_id", QString::fromStdString(installedLater.classId)},
        {"enabled", true}, {"identified", true}}});
    require(state::writeChain(saved), "write enabled retry identity");
    require(requestInputVst3Selection({installedLater}, &error), "queue missing plugin before install");
    const auto failureDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (g_inputMonitor.phase.load() != 6 && std::chrono::steady_clock::now() < failureDeadline)
        QThread::msleep(1);
    require(g_inputMonitor.phase == 6 && g_runtime.selectionWorkerBusy,
            "old missing-plugin failure awaits its Qt rejection");
    fs::create_directories(installedPath.parent_path(), copyError);
    require(!copyError, "create retry plugin directory");
    fs::copy(fs::u8path(fixture), installedPath, fs::copy_options::recursive, copyError);
    require(!copyError, "install formerly missing plugin for newer request");
    require(requestInputVst3Selection({installedLater}, &error), "retry installed identity before old disk rejection");
    waitInput();
    require(g_inputMonitor.error.empty() && g_inputMonitor.desired.size() == 1 &&
                g_inputMonitor.desired.at(0).module == installedLater.module,
            "newly installed plugin retry succeeds");
    require(state::loadChain(saved) &&
                state::scopeEffects(saved, state::ScopeKind::Input).at(0).toObject().value("enabled").toBool(),
            "obsolete Qt rejection cannot disable successfully retried saved identity");
}

void verifyInputWarmCache(const char *fixture) {
    inputTestStream.callback = {80, 1, 192000, true};
    Vst3SelectionEntry latest;
    for (int i = 0; i < 20; ++i) {
        const auto path = fs::u8path(state::dataDirectory().toStdString()) /
            ("cache-" + std::to_string(i)) / fs::u8path(fixture).filename();
        fs::create_directories(path.parent_path());
        fs::copy(fs::u8path(fixture), path, fs::copy_options::recursive);
        latest = stateValue({path.u8string(), "42302010605080701122334455667788"}, 0.375);
        requestInput({latest}, {Mode::LowLatencyOverlay, 0.25});
        require(g_inputMonitor.error.empty() && inputSlot().selection.count == 1 &&
            g_inputMonitor.warmEffects.size() <= EffectPool::kMaxWarmInstances,
            "cycling beyond warm-cache capacity never rejects a valid input identity");
    }
    const auto previous = inputSlot().selection.effects[0];
    requestInput({}, {Mode::LowLatencyOverlay, 0.25});
    checkInputSamples(1.0, 0.25);
    requestInput({latest}, {Mode::LowLatencyOverlay, 0.25});
    require(inputSlot().selection.effects[0] == previous &&
        savedValue(captureInputVst3States().at(0)) == 0.375,
        "recent input effect survives disable and re-enable with identical instance and state");
}
}

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QTemporaryDir data;
    qputenv("GPVST3_DATA_DIR", data.path().toUtf8());
    qputenv("GPVST3_DISABLE_PROJECT_RESTORE", "1");
    int result = 1;
    try {
        require(argc == 2 && data.isValid(), "isolated data directory and fixture path");
        verifyInputRuntime(argv[1]);
        verifyInputLatencyReporting(argv[1]);
        verifyInputWarmCache(argv[1]);
        float output[]{-4.0f, -1.0f, -0.125f, 0.0f, 0.125f, 1.0f, 4.0f};
        require(finishMonitorOutput(output, 7) && output[0] == -1 && output[6] == 1 &&
            output[2] == -0.125f && output[4] == 0.125f && !finishMonitorOutput(output, 7),
            "final monitor output restores native saturation and preserves all in-range samples");
        result = 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
    }
    g_runtime.stream.installed = false;
    shutdown();
    if (result == 0)
        std::cout << "PASS: P13 independent input runtime with real VST3 and synthetic lifecycle identity; no host/audio hardware acceptance.\n";
    return result;
}
