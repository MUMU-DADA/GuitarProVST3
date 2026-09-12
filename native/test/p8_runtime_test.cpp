// Exercise production runtime code with a real test VST3 and deterministic
// buffers. Only host discovery and editor window placement are substituted.
#include "../modules/gp_hook.cpp"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <QtCore/QCoreApplication>
#include <QtCore/QThread>
#include <QtCore/QTimer>
#include <QtWidgets/QWidget>

namespace {
std::vector<gpvst3::gp_audio::Binding> testBindings;
int testRate = 44100;
int readRate(const void *) { return testRate; }
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
}
namespace gpvst3::gp_audio {
std::size_t refresh() noexcept { return testBindings.size(); }
std::vector<Binding> snapshot() { return testBindings; }
bool currentTrack(Binding &binding) noexcept { if (testBindings.empty()) return false; binding = testBindings[0]; return true; }
const char *bindingSource() noexcept { return "fixture"; }
}
namespace gpvst3::ui {
void resizeNativeEditor(void *, int, int) {}
double nativeEditorScale(void *) { return 1; }
}
extern "C" __declspec(dllexport) int gpvst3_run_runtime_tests(const char *fixture) {
    using namespace gpvst3;
    using namespace gpvst3::hook;
    try {
        require(fixture && *fixture && qApp, "test VST3 path and real Qt host required");
        std::vector<Vst3SelectionEntry> entries;
        for (const auto &id : {"41302010605080701122334455667788", "42302010605080701122334455667788", "43302010605080701122334455667788"})
            entries.push_back({fixture, id});
        QJsonArray effects;
        for (const auto &entry : entries) effects.append(QJsonObject{{"module", fixture}, {"class_id", QString::fromStdString(entry.classId)}, {"enabled", true}});
        QJsonObject saved;
        state::setScopeEffects(saved, state::ScopeKind::Global, effects);
        state::setScopeEffects(saved, state::ScopeKind::Track, effects, "score", "track", 0);
        require(state::writeChain(saved), "write initial state");
        gp_audio::Binding binding;
        binding.chain = reinterpret_cast<void *>(1); binding.trackId = "track"; binding.trackKey = "track";
        binding.scoreKey = "score"; binding.documentId = "document"; binding.trackIndex = 0; binding.selectedTrack = true;
        testBindings.push_back(binding);
        g_initial.hostSupported = true;
        // No patch is installed by this fixture; calls enter the production
        // control API after substituting discovery and the rate accessor.
        g_runtime.master.installed = true; g_runtime.dsp.installed = true;
        g_runtime.sampleRate = &readRate; g_runtime.audioCore = reinterpret_cast<void *>(1);
        refreshTrackContext();
        std::string error;
        require(setTrackVst3Selection("track", entries, &error), "prepare track processors");
        qputenv("GPVST3_TEST_INITIALIZE_DELAY_MS", "150");
        int uiTicks = 0;
        QTimer heartbeat;
        QObject::connect(&heartbeat, &QTimer::timeout, [&uiTicks] { ++uiTicks; });
        heartbeat.start(5);
        const auto requestStarted = std::chrono::steady_clock::now();
        require(requestGlobalVst3Selection(entries, &error), "queue global processors");
        const auto requestElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - requestStarted).count();
        require(requestElapsed < 100, "global VST3 request returned without waiting for initialization");
        const auto requestDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (vst3SelectionPending() && std::chrono::steady_clock::now() < requestDeadline) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            QThread::msleep(1);
        }
        heartbeat.stop();
        qunsetenv("GPVST3_TEST_INITIALIZE_DELAY_MS");
        require(!vst3SelectionPending() && uiTicks >= 10,
                "Qt event processing continued while VST3 processors initialized");
        auto &track = g_runtime.trackRuntimes[0];
        auto *trackProcessor = track.trackSlots[track.chain.snapshot().activeSlot].effects[0].get();
        auto *globalProcessor = g_runtime.selectionSlots[g_runtime.chain.snapshot().activeSlot].effects[0].get();
        require(trackProcessor != globalProcessor, "scope processors are independent");
        // Live input must use an independently prepared copy of the same
        // selected global chain. This proves the capture path is not silently
        // routed through the playback instance or a default plug-in.
        g_runtime.stream.installed = true;
        require(configureInputRouter(), "prepare live input router");
        require(configureInputSelection(entries, &error), "prepare selected live input chain");
        g_runtime.inputRouter.setStreamRunning(true);
        require(g_runtime.inputSelectionSlots[g_runtime.inputChain.snapshot().activeSlot].effects[0].get() !=
                    globalProcessor,
                "live input uses an independent VST3 instance");
        float capture[512]{}, inputOutput[512]{};
        std::fill(std::begin(capture), std::end(capture), 0.4F);
        const input::InterleavedView inputView{
            capture, inputOutput, 256, 2, 2, 44100.0, 256,
            reinterpret_cast<void *>(0x44), 1, input::InterleavedSampleFormat::Float32};
        require(processExternalInputInterleaved(inputView), "process guitar input through selected VST3 chain");
        require(std::abs(inputOutput[200 * 2] - 0.5125F) < 0.000001F,
                "guitar input follows the selected VST3 chain order");
        require(g_runtime.inputRouter.snapshot().inputProcessedBlocks > 0 &&
                    g_runtime.inputRouter.snapshot().interleavedOutputWritten,
                "guitar input writes the processed block back to the host output");
        require(globalProcessor->queueParameter(1, 0.25),
                "global editor parameter publishes to the selected chain");
        require(processExternalInputInterleaved(inputView),
                "guitar input accepts a mirrored global parameter");
        require(std::abs(inputOutput[200 * 2] - 0.575F) < 0.000001F,
                "global parameter edit reaches the independent live input instance");
        require(trackProcessor->queueParameter(1, 0.75),
                "track editor parameter publishes to the track chain");
        require(processExternalInputInterleaved(inputView),
                "guitar input remains available after a track parameter edit");
        require(std::abs(inputOutput[200 * 2] - 0.575F) < 0.000001F,
                "track parameter edit does not leak into the live input instance");
        require(globalProcessor->queueParameter(1, 0.125) && trackProcessor->queueParameter(1, 0.125),
                "restore independent playback and input parameter values");
        require(processExternalInputInterleaved(inputView), "restore the live input parameter value");
        require(std::abs(inputOutput[200 * 2] - 0.5125F) < 0.000001F,
                "restored global parameter reaches the live input instance");
        float left[256], right[256]; float *channels[]{left, right};
        auto block = [&](int rate) { return audio::BlockView{nullptr, nullptr, nullptr, channels, 2, 256, double(rate), 256}; };
        auto reset = [&] { std::fill_n(left, 256, 0.4F); std::fill_n(right, 256, 0.4F); };
        for (const int rate : {44100, 48000, 96000, 44100}) {
            testRate = rate;
            if (track.configuredRate.load() != rate) {
                reset(); bool processed = true;
                require(track.processBlock(block(rate), &processed) && !processed && left[0] == 0.4F,
                    "changed rate bypasses until reconfigured");
                require(!track.chain.faulted(), "rate transition must not latch an audio fault");
            }
            refreshTrackContext();
            require(track.configuredRate.load() == rate && globalProcessor->configuredRate.load() == rate,
                "both scopes adopt the new callback sample rate");
            require(track.trackSlots[track.chain.snapshot().activeSlot].effects[0].get() == trackProcessor &&
                g_runtime.selectionSlots[g_runtime.chain.snapshot().activeSlot].effects[0].get() == globalProcessor,
                "rate reconfiguration preserves processor identity");
            reset(); bool processed = false;
            require(track.processBlock(block(rate), &processed) && processed && std::abs(left[0] - 0.5125F) < 0.000001F,
                "track actual sample after reconfiguration");
            reset(); require(g_runtime.chain.process(block(rate)).completed && std::abs(left[0] - 0.5125F) < 0.000001F,
                "global actual sample after reconfiguration");
        }
        auto missing = entries; missing[1].module = "C:/missing/P8 Missing.vst3";
        require(!setTrackVst3Selection("track", missing, &error) && error == "runtime_vst3_not_found", "missing explicit selection rejected");
        reset(); require(track.processBlock(block(testRate)) && std::abs(left[0] - 0.5125F) < 0.000001F,
            "failed selection preserves the previous live track chain");
        QWidget editorHost;
        editorHost.setAttribute(Qt::WA_NativeWindow);
        editorHost.resize(500, 300);
        editorHost.show();
        QCoreApplication::processEvents();
        require(openVst3Editor(entries[0], reinterpret_cast<void *>(editorHost.winId())),
                "VST3 editor opens on the Qt thread");
        QCoreApplication::processEvents();
        require(openVst3Editor(entries[0], reinterpret_cast<void *>(editorHost.winId())),
                "reopening the same VST3 editor is idempotent");
        closeVst3Editors();
        QCoreApplication::processEvents();
        editorHost.close();
        const auto externalEditorPath = qEnvironmentVariable("GPVST3_TEST_EXTERNAL_EDITOR");
        if (!externalEditorPath.isEmpty()) {
            RuntimeEffect externalEffect;
            require(externalEffect.initialize(44100.0, 16384, fs::u8path(externalEditorPath.toStdString()), {}),
                    "third-party VST3 editor processor initializes");
            QWidget externalHost;
            externalHost.setAttribute(Qt::WA_NativeWindow);
            externalHost.resize(640, 420);
            externalHost.show();
            QCoreApplication::processEvents();
            require(externalEffect.openEditor(reinterpret_cast<HWND>(externalHost.winId())),
                    "third-party VST3 editor opens on the Qt thread");
            QCoreApplication::processEvents();
            externalEffect.closeEditor();
            externalHost.close();
        }
        // An invalid state rejects the new selection while preserving the
        // currently active processor chain.
        auto corrupted = entries; corrupted[1].componentState = {1, 2, 3};
        require(!setGlobalVst3Selection(corrupted, &error) &&
                error.rfind("runtime_vst3_state_restore_failed", 0) == 0,
                "invalid component state is rejected");
        reset(); require(g_runtime.chain.process(block(testRate)).completed &&
                         std::abs(left[0] - 0.5125F) < 0.000001F,
                         "failed global selection preserves the previous live chain");
        require(setTrackVst3Selection("track", entries, &error), "restore explicit track selection");
        auto &slot = track.trackSlots[track.chain.snapshot().activeSlot];
        slot.effects[1]->forceError = true;
        reset(); require(!track.processBlock(block(testRate)), "failed process is detected");
        refreshTrackContext();
        reset(); require(track.processBlock(block(testRate)) && std::abs(left[0] - 0.775F) < 0.000001F,
            "remaining track processors continue after failure isolation");
        reset(); require(g_runtime.chain.process(block(testRate)).completed && std::abs(left[0] - 0.5125F) < 0.000001F,
            "a track failure does not change the independent global chain");
        require(consumeSelectionStateChanges(), "failure publishes UI reload notification");
        g_runtime.master.installed = false; g_runtime.dsp.installed = false;
        shutdown();
        std::cout << "PASS: P8 real VST3 buffers, async selection with Qt heartbeat, editor open/reopen/close, scope/state preservation and process failure isolation.\n";
        return 0;
    } catch (const std::exception &error) {
        g_runtime.master.installed = false; g_runtime.dsp.installed = false;
        shutdown(); std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
