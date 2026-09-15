// Exercise production runtime code with a real test VST3 and deterministic
// buffers. Only host discovery and editor window placement are substituted.
#include "../modules/gp_hook.cpp"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <QtCore/QCoreApplication>
#include <QtCore/QThread>
#include <QtWidgets/QWidget>

namespace {
std::vector<gpvst3::gp_audio::Binding> testBindings;
int testRate = 44100;
int readRate(const void *) { return testRate; }
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
}
namespace gpvst3::gp_audio {
std::size_t refresh() noexcept { return testBindings.size(); }
std::size_t refreshIfNeeded() noexcept { return testBindings.size(); }
std::vector<Binding> snapshot() { return testBindings; }
bool currentTrack(Binding &binding) noexcept { if (testBindings.empty()) return false; binding = testBindings[0]; return true; }
const char *bindingSource() noexcept { return "fixture"; }
void setRefreshNotifier(RefreshNotifier) noexcept {}
void markDirty() noexcept {}
void markSelectionDirty() noexcept {}
bool refreshNeeded() noexcept { return true; }
bool refreshIncomplete() noexcept { return false; }
bool checkStructureChanged() noexcept { return false; }
bool refreshSelectionContext() noexcept { return false; }
}
namespace gpvst3::ui {
void resizeNativeEditor(void *, int, int) {}
double nativeEditorScale(void *) { return 1; }
}

namespace {
void verifyPreloadRetention(const std::vector<gpvst3::hook::Vst3SelectionEntry> &entries,
                            const QJsonArray &catalog) {
    using namespace gpvst3;
    using namespace gpvst3::hook;
    auto &track = g_runtime.trackRuntimes[0];
    std::string error;
    const auto wait = [&] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (vst3SelectionPending() && std::chrono::steady_clock::now() < deadline) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            QThread::msleep(1);
        }
        require(!vst3SelectionPending(), "preload/selection worker drains");
    };
    const auto select = [&](const std::vector<Vst3SelectionEntry> &global,
                            const std::vector<Vst3SelectionEntry> &selectedTrack) {
        require(requestGlobalVst3Selection(global, &error), "queue global lifecycle selection");
        require(requestTrackVst3Selection("track", selectedTrack, &error), "queue track lifecycle selection");
        wait();
    };
    std::array<std::weak_ptr<RuntimeEffect>, 9> retained;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        retained[i] = g_runtime.selectionPool.effects[i];
        retained[3 + i] = track.pool.effects[i];
        retained[6 + i] = g_runtime.inputSelectionPool.effects[i];
    }
    float left[256], right[256]; float *channels[]{left, right};
    const audio::BlockView block{nullptr, nullptr, nullptr, channels, 2, 256, 44100.0, 256};
    const auto reset = [&] { std::fill_n(left, 256, 0.4F); std::fill_n(right, 256, 0.4F); };
    select({entries[0]}, {entries[0]});
    require(retained[0].lock()->queueParameter(1, 0.375) && retained[3].lock()->queueParameter(1, 0.625),
            "edit independently retained global and track states");
    reset(); require(g_runtime.chain.process(block).completed, "apply global edit");
    reset(); require(track.processBlock(block), "apply track edit");
    const auto globalSaved = captureGlobalVst3States();
    const auto trackSaved = captureTrackVst3States("track");
    require(globalSaved.size() == 1 && trackSaved.size() == 1, "capture edited states");
    const auto created = g_nextInstanceId.load();
    for (const auto index : {1U, 2U, 0U, 2U, 1U, 0U}) {
        select({}, {});
        const auto globalBlocks = retained[0].lock()->processedBlocks.load();
        const auto trackBlocks = retained[3].lock()->processedBlocks.load();
        reset(); require(g_runtime.chain.process(block).bypassed && left[0] == 0.4F,
                         "disabled global leaves playback samples untouched");
        bool processed = true;
        reset(); require(track.processBlock(block, &processed) && !processed && left[0] == 0.4F,
                         "disabled track bypasses without running its processor");
        require(retained[0].lock()->processedBlocks.load() == globalBlocks &&
                    retained[3].lock()->processedBlocks.load() == trackBlocks,
                "disabled processors retain DSP state without processing blocks");
        select(index == 0 ? globalSaved : std::vector<Vst3SelectionEntry>{entries[index]},
               index == 0 ? trackSaved : std::vector<Vst3SelectionEntry>{entries[index]});
        require(g_runtime.selectionSlots[g_runtime.chain.snapshot().activeSlot].effects[0] == retained[index].lock() &&
                    track.trackSlots[track.chain.snapshot().activeSlot].effects[0] == retained[3 + index].lock() &&
                    g_runtime.inputSelectionSlots[g_runtime.inputChain.snapshot().activeSlot].effects[0] == retained[6 + index].lock(),
                "A-disable-B-disable-C-disable-A reuses every scope's original instance");
        require(g_nextInstanceId.load() == created, "switching constructs no additional instance");
    }
    reset(); require(g_runtime.chain.process(block).completed && std::abs(left[200] - 0.775F) < 0.000001F,
                     "re-enabled global preserves its edited parameter");
    reset(); require(track.processBlock(block) && std::abs(left[200] - 1.025F) < 0.000001F,
                     "re-enabled track preserves its independent parameter");
    require(retained[0].lock()->queueParameter(1, 0.125) && retained[3].lock()->queueParameter(1, 0.125),
            "restore parameters after retention regression");
    select({entries[2], entries[0], entries[1]}, {entries[2], entries[0], entries[1]});
    reset(); require(g_runtime.chain.process(block).completed && std::abs(left[200] - 0.3875F) < 0.000001F,
                     "reordered retained instances process audio in the new order");
    select(entries, entries);

    // Preloading must also cover catalogs larger than the active-chain limit.
    auto largerCatalog = catalog;
    largerCatalog.insert(1, QJsonObject{{"module", "C:/missing/Preload Missing.vst3"},
        {"class_id", QString::fromStdString(entries[0].classId)}, {"identified", true}});
    auto largerEntries = entries;
    for (int copy = 0; copy < 2; ++copy) {
        const auto fixture = fs::u8path(entries[0].module);
        const auto copyPath = fs::u8path(state::dataDirectory().toStdString()) /
            ("preload-copy-" + std::to_string(copy)) / fixture.filename();
        fs::create_directories(copyPath.parent_path());
        fs::copy(fixture, copyPath, fs::copy_options::recursive);
        for (auto entry : entries) {
            entry.module = copyPath.u8string();
            largerEntries.push_back(entry);
            largerCatalog.append(QJsonObject{{"module", QString::fromStdString(entry.module)},
                {"class_id", QString::fromStdString(entry.classId)}, {"identified", true}});
        }
    }
    const auto failed = g_runtime.preloadFailed;
    setVst3Catalog(largerCatalog);
    preloadSavedSelections();
    require(requestGlobalVst3Selection(entries, &error), "explicit enable during catalog preloading");
    wait();
    require(g_runtime.selectionPool.effects.size() == 9 && track.pool.effects.size() == 9 &&
                g_runtime.inputSelectionPool.effects.size() == 9 && g_runtime.preloadFailed == failed + 3,
            "all nine plugins preload per scope despite an earlier failure and active selection");
    require(g_runtime.selectionSlots[g_runtime.chain.snapshot().activeSlot].effects[0] == retained[0].lock(),
            "background preloading preserves the active chain");
    const auto afterPreload = g_nextInstanceId.load();
    select({largerEntries.back()}, {largerEntries.back()});
    require(g_nextInstanceId.load() == afterPreload, "plugin beyond chain capacity was already preloaded");
    select(entries, entries);
}
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
        auto disabledEffect = effects[1].toObject();
        disabledEffect.insert("enabled", false);
        disabledEffect.insert("configured", true);
        const QJsonArray savedEffects{effects[0], disabledEffect};
        state::setScopeEffects(saved, state::ScopeKind::Global, savedEffects);
        state::setScopeEffects(saved, state::ScopeKind::Track, savedEffects, "score", "track", 0);
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
        qunsetenv("GPVST3_P4_ROUTE");
        require(configuredInputRoute() == input::Route::Disabled, "input monitoring is opt-in");
        refreshTrackContext();
        std::string error;
        require(state::disableAllEffectsAtStartup(), "preload preserves startup bypass");
        QJsonArray preloadCatalog;
        for (const auto &value : effects) {
            auto effect = value.toObject(); effect.insert("identified", true); preloadCatalog.append(effect);
        }
        setVst3Catalog(preloadCatalog);
        g_runtime.stream.installed = true;
        require(configureInputRouter(), "prepare dormant live input router");
        preloadSavedSelections();
        const auto preloadDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (vst3SelectionPending() && std::chrono::steady_clock::now() < preloadDeadline) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            QThread::msleep(1);
        }
        require(!vst3SelectionPending() && g_runtime.globalPreloaded && !g_runtime.inputPreloaded &&
                    g_runtime.trackRuntimes[0].preloaded.load(),
                "global and track catalog preloads without enabling input monitoring");
        require(g_runtime.selectionPool.effects.size() == entries.size() &&
                    g_runtime.trackRuntimes[0].pool.effects.size() == entries.size() &&
                    g_runtime.inputSelectionPool.effects.empty(),
                "all catalog plugins preload including disabled and never-saved entries");
        const auto *preloadedGlobal = g_runtime.selectionPool.effects[0].get();
        const auto *preloadedTrack = g_runtime.trackRuntimes[0].pool.effects[0].get();
        require(g_runtime.chain.snapshot().activeSlot < 0 && g_runtime.inputChain.snapshot().activeSlot < 0 &&
                    g_runtime.trackRuntimes[0].chain.snapshot().activeSlot < 0,
                "preloaded chains remain bypassed before explicit enable");
        float dormantLeft[256]{}, dormantRight[256]{};
        float *dormantChannels[]{dormantLeft, dormantRight};
        const audio::BlockView dormantBlock{nullptr, nullptr, nullptr, dormantChannels, 2, 256, 44100.0, 256};
        for (int i = 0; i < 20; ++i) {
            require(g_runtime.chain.process(dormantBlock).bypassed, "global preload never processes before enable");
            require(g_runtime.inputChain.process(dormantBlock).bypassed, "input preload never processes before enable");
            bool processed = true;
            g_runtime.trackRuntimes[0].processBlock(dormantBlock, &processed);
            require(!processed, "track preload never processes before enable");
        }
        require(preloadedGlobal != preloadedTrack, "preloaded scopes own independent processors");
        const auto preloadedEvidence = snapshot();
        require(preloadedEvidence.instances.size() == 6 && std::all_of(
                    preloadedEvidence.instances.begin(), preloadedEvidence.instances.end(), [](const auto &instance) {
                        return !instance.active && instance.preloaded && instance.processedBlocks == 0;
                    }), "diagnostics expose every dormant instance");
        const auto preloadCompleted = g_runtime.preloadCompleted;
        for (int i = 0; i < 20; ++i) preloadSavedSelections();
        require(!vst3SelectionPending() && g_runtime.preloadCompleted == preloadCompleted,
                "unchanged preload requests are deduplicated");
        require(requestTrackVst3Selection("track", entries, &error), "activate preloaded track selection");
        while (vst3SelectionPending() && std::chrono::steady_clock::now() < preloadDeadline) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            QThread::msleep(1);
        }
        require(g_runtime.trackRuntimes[0].chain.snapshot().activeSlot >= 0 &&
                    g_runtime.trackRuntimes[0].trackSlots[g_runtime.trackRuntimes[0].chain.snapshot().activeSlot].effects[0].get() == preloadedTrack,
                "track enable reuses the preloaded processor");
        require(requestGlobalVst3Selection(entries, &error), "activate preloaded global selection");
        while (vst3SelectionPending() && std::chrono::steady_clock::now() < preloadDeadline) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            QThread::msleep(1);
        }
        require(g_runtime.selectionSlots[g_runtime.chain.snapshot().activeSlot].effects[0].get() == preloadedGlobal,
                "global enable reuses the preloaded processor");
        require(!g_runtime.inputRouter.snapshot().enabled && g_runtime.inputChain.snapshot().activeSlot == -1 &&
                    g_runtime.inputSelectionPool.effects.empty(),
                "global enable does not create or enable a microphone monitor");
        require(!g_runtime.globalPreloaded && !g_runtime.inputPreloaded && !g_runtime.trackRuntimes[0].preloaded.load(),
                "preload diagnostics distinguish active from prepared slots");
        require(setTrackVst3Selection("track", entries, &error), "prepare track processors");
        qputenv("GPVST3_TEST_INITIALIZE_DELAY_MS", "150");
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
        qunsetenv("GPVST3_TEST_INITIALIZE_DELAY_MS");
        require(!vst3SelectionPending(),
                "VST3 processors finish asynchronous initialization");
        auto &track = g_runtime.trackRuntimes[0];
        auto *trackProcessor = track.trackSlots[track.chain.snapshot().activeSlot].effects[0].get();
        auto *globalProcessor = g_runtime.selectionSlots[g_runtime.chain.snapshot().activeSlot].effects[0].get();
        require(trackProcessor != globalProcessor, "scope processors are independent");
        // Live input must use an independently prepared copy of the same
        // selected global chain. This proves the capture path is not silently
        // routed through the playback instance or a default plug-in.
        qputenv("GPVST3_P4_ROUTE", "input_insert");
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
        auto waitMaintenance = [&] {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (vst3SelectionPending() && std::chrono::steady_clock::now() < deadline) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
                QThread::msleep(1);
            }
            require(!vst3SelectionPending(), "track maintenance worker completes");
        };
        verifyPreloadRetention(entries, preloadCatalog);
        for (const int rate : {44100, 48000, 96000, 44100}) {
            testRate = rate;
            if (track.configuredRate.load() != rate) {
                reset(); bool processed = true;
                require(track.processBlock(block(rate), &processed) && !processed && left[0] == 0.4F,
                    "changed rate bypasses until reconfigured");
                require(!track.chain.faulted(), "rate transition must not latch an audio fault");
            }
            refreshTrackContext();
            waitMaintenance();
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
            require(externalEffect.initialize(
                        44100.0, 16384, fs::u8path(externalEditorPath.toStdString()), {}),
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
        waitMaintenance();
        reset(); require(track.processBlock(block(testRate)) && std::abs(left[0] - 0.775F) < 0.000001F,
            "remaining track processors continue after failure isolation");
        reset(); require(g_runtime.chain.process(block(testRate)).completed && std::abs(left[0] - 0.5125F) < 0.000001F,
            "a track failure does not change the independent global chain");
        require(consumeSelectionStateChanges(), "failure publishes UI reload notification");
        g_runtime.master.installed = false; g_runtime.dsp.installed = false;
        shutdown();
        std::cout << "PASS: P8 real VST3 buffers, async selection, editor open/reopen/close, scope/state preservation and process failure isolation.\n";
        return 0;
    } catch (const std::exception &error) {
        g_runtime.master.installed = false; g_runtime.dsp.installed = false;
        shutdown(); std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
