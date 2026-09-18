// Exercise production runtime code with a real test VST3 and deterministic
// buffers. Only host discovery and editor window placement are substituted.
#include "../modules/gp_hook.cpp"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <QtCore/QCoreApplication>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QThread>
#include <QtCore/QTimer>
#include <QtWidgets/QWidget>

namespace {
std::vector<gpvst3::gp_audio::Binding> testBindings;
int testRate = 44100;
std::atomic<int> selectionNotifications{0};
void notifySelection() noexcept { selectionNotifications.fetch_add(1, std::memory_order_relaxed); }
int readRate(const void *) { return testRate; }
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }

void verifyDormantContext() {
    using namespace gpvst3::hook;
    const auto createdBefore = g_nextInstanceId.load();
    std::atomic<bool> completed{false};
    std::thread worker([&] {
        refreshTrackContextWorkerImpl(testBindings, testBindings.size());
        completed.store(true, std::memory_order_release);
    });
    QElapsedTimer clock;
    clock.start();
    // Simulate Qt being occupied by the first enable request. A dormant
    // context refresh must release editorMutex without requesting Qt work.
    while (!completed.load(std::memory_order_acquire) && clock.elapsed() < 500)
        QThread::msleep(1);
    const bool completedWithoutQt = completed.load(std::memory_order_acquire);
    // Drain a regressed implementation before joining, so the assertion can
    // report the lock dependency instead of leaving a blocked test worker.
    while (!completed.load(std::memory_order_acquire)) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    worker.join();
    require(completedWithoutQt, "dormant context completes without Qt dispatch before first enable");
    require(g_nextInstanceId.load() == createdBefore && g_runtime.selectionPool.effects.empty() &&
                g_runtime.trackRuntimes[0].pool.effects.empty() && !g_runtime.projectGlobalRestored,
            "uninstalled hooks defer both global and track plugin restoration");
    require(g_runtime.trackRuntimes[0].trackKey == "track" &&
                g_runtime.trackBindingsPublished.load() == 1,
            "dormant context still publishes the track scope needed by first enable");
    std::vector<Vst3SelectionEntry> live;
    require(activeTrackVst3States("track", live) && live.empty() &&
                captureTrackVst3States("track").empty(),
            "saved enabled intent is not an active track before preparation");
    std::unique_lock<std::recursive_mutex> prepareLock(g_runtime.editorMutex, std::try_to_lock);
    require(prepareLock.owns_lock(), "Qt prepare can acquire runtime ownership after dormant context");
    g_runtime.requestedTrackSelections.clear();
    g_runtime.publishedBindings.clear();
}

void verifyColdSelection(const std::string &trackKey,
                         const std::vector<gpvst3::hook::Vst3SelectionEntry> &entries) {
    using namespace gpvst3::hook;
    QElapsedTimer clock;
    clock.start();
    qint64 lastTick = 0, maximumGap = 0;
    QJsonArray longGaps;
    qint64 maximumEvents = 0, maximumPending = 0;
    int ticks = 0;
    QTimer heartbeat;
    heartbeat.setTimerType(Qt::PreciseTimer);
    QObject::connect(&heartbeat, &QTimer::timeout, [&] {
        const auto now = clock.elapsed();
        if (now - lastTick >= 80)
            longGaps.append(QJsonObject{{"at_ms", now}, {"gap_ms", now - lastTick}});
        maximumGap = std::max(maximumGap, now - lastTick);
        lastTick = now;
        ++ticks;
    });
    heartbeat.start(10);
    // Only the first class is warm. Two new instances must initialize here.
    const auto createdBefore = g_nextInstanceId.load();
    qputenv("GPVST3_TEST_INITIALIZE_DELAY_MS", "150");
    qputenv("GPVST3_TEST_STATE_DELAY_MS", "150");
    // Cover both VST3 controller layouts: track uses a combined component;
    // global creates independent controllers with slow initialize/state calls.
    if (trackKey.empty()) qputenv("GPVST3_TEST_SEPARATE_CONTROLLER", "1");
    std::string error;
    const bool accepted = trackKey.empty() ? requestGlobalVst3Selection(entries, &error)
        : requestTrackVst3Selection(trackKey, entries, &error);
    const auto requestElapsed = clock.elapsed();
    while (clock.elapsed() < 5000) {
        auto start = clock.elapsed();
        const bool pending = vst3SelectionPending();
        maximumPending = std::max(maximumPending, clock.elapsed() - start);
        if (!pending) break;
        start = clock.elapsed();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        maximumEvents = std::max(maximumEvents, clock.elapsed() - start);
        QThread::msleep(1);
    }
    maximumGap = std::max(maximumGap, clock.elapsed() - lastTick);
    heartbeat.stop();
    qunsetenv("GPVST3_TEST_INITIALIZE_DELAY_MS");
    qunsetenv("GPVST3_TEST_STATE_DELAY_MS");
    qunsetenv("GPVST3_TEST_SEPARATE_CONTROLLER");
    const auto instancesCreated = g_nextInstanceId.load() - createdBefore;
    const auto scope = trackKey.empty() ? QStringLiteral("global") : QStringLiteral("track");
    QFile evidence(gpvst3::state::dataDirectory() + "/cold-selection-" + scope + ".json");
    require(evidence.open(QIODevice::WriteOnly), "write cold selection evidence");
    evidence.write(QJsonDocument(QJsonObject{{"scope", scope}, {"request_ms", requestElapsed},
        {"total_ms", clock.elapsed()}, {"max_qt_gap_ms", maximumGap}, {"qt_ticks", ticks},
        {"instances_created", qint64(instancesCreated)}, {"long_gaps", longGaps},
        {"max_process_events_ms", maximumEvents}, {"max_pending_ms", maximumPending}}).toJson());
    evidence.close();
    require(accepted && !vst3SelectionPending(), "cold selection completes");
    require(instancesCreated == 2 && clock.elapsed() >= 1800,
            "cold selection executes both delayed initializers and saved-state restores");
    require(requestElapsed < 100, "cold selection request returns immediately");
    require(ticks >= 10 && maximumGap < 100, "Qt remains responsive throughout cold initialization");
}

bool syncTrackSelection(const std::string &trackKey,
                        const std::vector<gpvst3::hook::Vst3SelectionEntry> &selection,
                        std::string *error) {
    if (!gpvst3::hook::requestTrackVst3Selection(trackKey, selection, error)) return false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (gpvst3::hook::vst3SelectionPending() && std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    if (gpvst3::hook::vst3SelectionPending()) {
        if (error) *error = "selection_worker_timeout";
        return false;
    }
    for (const auto &runtime : gpvst3::hook::g_runtime.trackRuntimes)
        if (runtime.trackKey == trackKey && !runtime.error.empty()) {
            if (error) *error = runtime.error;
            return false;
        }
    return true;
}

void verifyTrackCancellationDuringPrepare(
    const std::vector<gpvst3::hook::Vst3SelectionEntry> &sourceEntries) {
    using namespace gpvst3;
    using namespace gpvst3::hook;
    require(sourceEntries.size() >= 2, "cancellation regression needs two fixture classes");
    QJsonArray cases;
    for (const bool replaceWhilePreparing : {false, true}) {
        // Each case uses a cold path; a warm A would miss the cancellation window.
        const auto source = std::filesystem::u8path(sourceEntries[0].module);
        const auto copyPath = std::filesystem::u8path(state::dataDirectory().toStdString()) /
            (replaceWhilePreparing ? "cancel-replace-fixture" : "cancel-only-fixture") / source.filename();
        std::error_code copyError;
        std::filesystem::create_directories(copyPath.parent_path(), copyError);
        require(!copyError, "create cancellation fixture directory");
        std::filesystem::copy(source, copyPath,
            std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing,
            copyError);
        require(!copyError, "copy cold cancellation fixture");

        auto requestA = sourceEntries[0];
        auto requestB = sourceEntries[1];
        requestA.module = copyPath.u8string();
        requestB.module = copyPath.u8string();
        std::string error;
        require(syncTrackSelection("track", {}, &error),
                "clear track before cancellation race");
        std::vector<Vst3SelectionEntry> disabled;
        require(activeTrackVst3States("track", disabled) && disabled.empty(),
                "track is bypassed before cancellation race");

        struct DelayCleanup {
            ~DelayCleanup() {
                qunsetenv("GPVST3_TEST_INITIALIZE_DELAY_MS");
                qunsetenv("GPVST3_TEST_STATE_DELAY_MS");
            }
        } cleanup;
        qputenv("GPVST3_TEST_INITIALIZE_DELAY_MS", "150");
        qputenv("GPVST3_TEST_STATE_DELAY_MS", "150");
        QElapsedTimer clock;
        clock.start();
        qint64 lastTick = 0;
        qint64 maximumGap = 0;
        int ticks = 0;
        QTimer heartbeat;
        heartbeat.setTimerType(Qt::PreciseTimer);
        QObject::connect(&heartbeat, &QTimer::timeout, [&] {
            const auto now = clock.elapsed();
            maximumGap = std::max(maximumGap, now - lastTick);
            lastTick = now;
            ++ticks;
        });
        heartbeat.start(10);

        const auto instancesBefore = g_nextInstanceId.load(std::memory_order_acquire);
        const auto generationBefore = g_runtime.audioGeneration.load(std::memory_order_acquire);
        const auto workerStartBefore = g_runtime.selectionWorkerStartedNanoseconds.load(
            std::memory_order_acquire);
        require(requestTrackVst3Selection("track", {requestA}, &error),
                "queue slow A selection");

        // RuntimeEffect construction occurs inside prepare(), after the worker's
        // initial generation check. This catches cancellation during preparation,
        // including a worker waiting to create the Qt-affine component.
        bool aPreparing = false;
        const auto overlapDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < overlapDeadline) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            if (g_runtime.selectionWorkerBusy.load(std::memory_order_acquire) &&
                g_runtime.selectionStatus.load(std::memory_order_acquire) == 2 &&
                g_runtime.selectionWorkerStartedNanoseconds.load(std::memory_order_acquire) > workerStartBefore &&
                g_nextInstanceId.load(std::memory_order_acquire) > instancesBefore) {
                aPreparing = true;
                break;
            }
            QThread::msleep(1);
        }
        require(aPreparing, "slow A selection entered production prepare");
        require(g_runtime.audioGeneration.load(std::memory_order_acquire) == generationBefore,
                "cancellation overlaps A before it can commit");

        // The empty request marks the runtime bypassed immediately. In the second
        // case B supersedes that pending entry while A still owns prepare().
        require(requestTrackVst3Selection("track", {}, &error),
                "queue cancellation during slow A selection");
        require(g_runtime.trackRuntimes[0].bypassRequested.load(std::memory_order_acquire),
                "cancellation immediately requests audio bypass");
        if (replaceWhilePreparing)
            require(requestTrackVst3Selection("track", {requestB}, &error),
                    "queue B selection after cancellation");

        const auto drainDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        while (vst3SelectionPending() && std::chrono::steady_clock::now() < drainDeadline) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            QThread::msleep(1);
        }
        maximumGap = std::max(maximumGap, clock.elapsed() - lastTick);
        heartbeat.stop();
        qunsetenv("GPVST3_TEST_INITIALIZE_DELAY_MS");
        qunsetenv("GPVST3_TEST_STATE_DELAY_MS");
        require(!vst3SelectionPending(), "cancellation race worker drains");
        require(ticks >= 10 && maximumGap < 100,
                "Qt remains responsive during cancellation race preparation");

        const auto generationDelta = g_runtime.audioGeneration.load(std::memory_order_acquire) -
            generationBefore;
        cases.append(QJsonObject{{"replace_while_preparing", replaceWhilePreparing},
            {"total_ms", clock.elapsed()}, {"max_qt_gap_ms", maximumGap}, {"qt_ticks", ticks},
            {"instances_created", qint64(g_nextInstanceId.load() - instancesBefore)},
            {"audio_generations_committed", qint64(generationDelta)}});
        require(generationDelta == 1,
                "stale A prepare does not publish an extra audio generation");
        auto &runtime = g_runtime.trackRuntimes[0];
        if (!replaceWhilePreparing) {
            require(runtime.chain.snapshot().activeSlot == -1 &&
                        runtime.count.load(std::memory_order_acquire) == 0 &&
                        runtime.bypassRequested.load(std::memory_order_acquire) &&
                        !runtime.configured.load(std::memory_order_acquire) &&
                        activeTrackVst3States("track", disabled) && disabled.empty() &&
                        runtime.requested.empty(),
                    "cancelled cold A remains disabled after preparation completes");
            require(syncTrackSelection("track", {requestB}, &error),
                    "B can be enabled after cold A cancellation completes");
            require(g_runtime.audioGeneration.load(std::memory_order_acquire) == generationBefore + 2,
                    "B commits once after completed cancellation");
        }
        const auto active = runtime.chain.snapshot().activeSlot;
        require(active >= 0 && runtime.count.load(std::memory_order_acquire) == 1 &&
                    !runtime.bypassRequested.load(std::memory_order_acquire) &&
                    runtime.trackSlots[active].count == 1 &&
                    runtime.trackSlots[active].effects[0] &&
                    runtime.trackSlots[active].effects[0]->identity.module == requestB.module &&
                    runtime.trackSlots[active].effects[0]->identity.classId == requestB.classId &&
                    sameSelection(runtime.requested, {requestB}),
                "latest B selection is the only active track runtime");
        std::vector<Vst3SelectionEntry> applied;
        require(activeTrackVst3States("track", applied) && applied.size() == 1 &&
                    applied[0].classId == requestB.classId && applied[0].classId != requestA.classId,
                "B is the final applied track identity after A cancellation");
        const auto evidence = snapshot();
        const auto trackEvidence = std::find_if(evidence.trackRuntimeEvidence.begin(), evidence.trackRuntimeEvidence.end(),
            [](const auto &value) { return value.trackKey == "track"; });
        require(trackEvidence != evidence.trackRuntimeEvidence.end() && trackEvidence->configuredEffects == 1,
                "runtime evidence reports exactly one configured track effect after cancellation");
        float left[256], right[256];
        std::fill_n(left, 256, 0.4F);
        std::fill_n(right, 256, 0.4F);
        float *channels[]{left, right};
        const audio::BlockView block{nullptr, nullptr, nullptr, channels, 2, 256, double(testRate), 256};
        bool processed = false;
        require(runtime.processBlock(block, &processed) && processed &&
                    std::abs(left[200] - 0.2F) < 0.000001F &&
                    runtime.trackSlots[active].effects[0]->processedBlocks.load() > 0,
                "only B processes the post-cancellation audio block");
    }
    QFile evidence(state::dataDirectory() + "/cancel-selection-track.json");
    require(evidence.open(QIODevice::WriteOnly), "write cancellation selection evidence");
    evidence.write(QJsonDocument(cases).toJson());
    evidence.close();
    std::string error;
    require(syncTrackSelection("track", sourceEntries, &error),
            "restore track selection after cancellation regression");
}
}
namespace gpvst3::gp_audio {
std::size_t refresh() noexcept { return testBindings.size(); }
std::size_t refreshIfNeeded() noexcept { return testBindings.size(); }
std::vector<Binding> snapshot() { return testBindings; }
bool currentTrack(Binding &binding) noexcept { if (testBindings.empty()) return false; binding = testBindings[0]; return true; }
const char *bindingSource() noexcept { return "fixture"; }
void setRefreshNotifier(RefreshNotifier) noexcept {}
void markDirty() noexcept {}
void markTopologyDirty() noexcept {}
void markExplicitTopologyDirty() noexcept {}
bool consumeTopologyDirty() noexcept { return false; }
std::uint64_t topologyEventCount() noexcept { return 0; }
void markSelectionDirty() noexcept {}
bool refreshNeeded() noexcept { return true; }
bool refreshIncomplete() noexcept { return false; }
bool checkStructureChanged() noexcept { return false; }
bool refreshSelectionContext() noexcept { return false; }
std::uint64_t selectionGeneration() noexcept { return 1; }
std::uint64_t bindingGeneration() noexcept { return 1; }
std::uint64_t contextPublishLatencyNanoseconds() noexcept { return 0; }
std::uint64_t droppedRefreshCount() noexcept { return 0; }
bool hasActiveDocument() noexcept { return !testBindings.empty(); }
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
        std::vector<Vst3SelectionEntry> disabled;
        require(track.chain.snapshot().activeSlot == -1 && track.bypassRequested.load() &&
                    track.count.load() == 0 && activeTrackVst3States("track", disabled) && disabled.empty(),
                "disabled track has no active slot or enabled UI identity");
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
        std::vector<Vst3SelectionEntry> applied;
        require(activeTrackVst3States("track", applied) && applied.size() == 1 &&
                    applied[0].classId == entries[index].classId,
                "enabling another track plugin publishes its actual identity");
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

    // P12 keeps catalog metadata separate from the project graph. Adding
    // unconfigured modules must not instantiate them during preload.
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
    require(g_runtime.selectionPool.effects.size() == entries.size() && track.pool.effects.size() == entries.size() &&
                g_runtime.inputSelectionPool.effects.size() == entries.size() && g_runtime.preloadFailed == failed,
            "catalog refresh does not preload unconfigured or input-routed modules");
    require(g_runtime.selectionSlots[g_runtime.chain.snapshot().activeSlot].effects[0] == retained[0].lock(),
            "background preloading preserves the active chain");
    const auto afterPreload = g_nextInstanceId.load();
    select({largerEntries.back()}, {largerEntries.back()});
    require(g_nextInstanceId.load() > afterPreload, "explicit enable creates a previously metadata-only plugin");
    select(entries, entries);
}
}

extern "C" __declspec(dllexport) int gpvst3_run_runtime_tests(const char *fixture) {
    using namespace gpvst3;
    using namespace gpvst3::hook;
    try {
        require(fixture && *fixture && qApp, "test VST3 path and real Qt host required");
        // The driver loads us during Guitar Pro startup. Let the host finish
        // its initial widget/layout work before measuring a checkbox request.
        QEventLoop hostSettling;
        QTimer::singleShot(2000, &hostSettling, &QEventLoop::quit);
        hostSettling.exec();
        std::vector<Vst3SelectionEntry> entries;
        for (const auto &id : {"41302010605080701122334455667788", "42302010605080701122334455667788", "43302010605080701122334455667788"})
            entries.push_back({fixture, id});
        // Keep default sound values, but force cold enable through both state
        // restoration paths, not only initialize(). The first class stays warm.
        for (std::size_t index = 1; index < entries.size(); ++index) {
            const double value = index == 1 ? 0.5 : 0.25;
            entries[index].componentState.resize(sizeof(value));
            std::memcpy(entries[index].componentState.data(), &value, sizeof(value));
            entries[index].controllerState = entries[index].componentState;
        }
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
        qunsetenv("GPVST3_DISABLE_PROJECT_RESTORE");
        verifyDormantContext();
        qputenv("GPVST3_DISABLE_PROJECT_RESTORE", "1");
        // No patch is installed by this fixture; calls enter the production
        // control API after substituting discovery and the rate accessor.
        g_runtime.master.installed = true; g_runtime.dsp.installed = true;
        g_runtime.sampleRate = &readRate; g_runtime.audioCore = reinterpret_cast<void *>(1);
        qunsetenv("GPVST3_P4_ROUTE");
        require(configuredInputRoute() == input::Route::Disabled, "input monitoring is opt-in");
        refreshTrackContext();
        std::string error;
        QJsonArray preloadCatalog;
        for (const auto &value : effects) {
            auto effect = value.toObject(); effect.insert("identified", true); preloadCatalog.append(effect);
        }
        setVst3Catalog(preloadCatalog);
        require(!requestTrackVst3SelectionAtGeneration("track", 2, entries, &error) &&
                    error == "stale_selection_generation",
                "stale track selection generation is rejected before enqueue");
        auto newBinding = binding;
        newBinding.chain = reinterpret_cast<void *>(2);
        newBinding.trackKey = "new-track";
        newBinding.trackId = "new-track";
        newBinding.trackIndex = 1;
        newBinding.selectedTrack = false;
        testBindings.push_back(newBinding);
        require(syncTrackSelection("new-track", {entries[0]}, &error),
                "first selection prepares a track before its runtime table was published");
        std::vector<Vst3SelectionEntry> newLive;
        require(activeTrackVst3States("new-track", newLive) && newLive.size() == 1 &&
                    newLive[0].classId == entries[0].classId,
                "first selection is applied rather than silently dropped");
        require(syncTrackSelection("new-track", {}, &error) &&
                    activeTrackVst3States("new-track", newLive) && newLive.empty(),
                "new track selection can be disabled");
        testBindings.pop_back();
        refreshTrackContext();
        g_runtime.stream.installed = true;
        require(configureInputRouter(), "prepare dormant live input router");
        setSelectionNotifier(&notifySelection);
        const auto notificationsBeforePreload = selectionNotifications.load();
        preloadSavedSelections();
        const auto preloadDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (vst3SelectionPending() && std::chrono::steady_clock::now() < preloadDeadline) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            QThread::msleep(1);
        }
        // The context worker and preload worker are independent queues. A
        // fixture may observe the first preload before its track runtime slot
        // is published; replaying the idempotent project-scoped request here
        // verifies the same retry behavior used after a host rebuild.
        refreshTrackContext();
        preloadSavedSelections();
        while (vst3SelectionPending() && std::chrono::steady_clock::now() < preloadDeadline) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            QThread::msleep(1);
        }
        require(!vst3SelectionPending() && g_runtime.globalPreloaded && !g_runtime.inputPreloaded &&
                    g_runtime.trackRuntimes[0].preloaded.load(),
                "global and track catalog preloads without enabling input monitoring");
        require(selectionNotifications.load() > notificationsBeforePreload,
                "preload completion wakes a UI indicator deferred by pending work");
        require(g_runtime.selectionPool.effects.size() == 1 &&
                    g_runtime.trackRuntimes[0].pool.effects.size() == 1 &&
                    g_runtime.inputSelectionPool.effects.empty(),
                "only explicitly enabled project plugins preload; disabled catalog entries stay metadata-only");
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
        require(preloadedEvidence.instances.size() == 2 && std::all_of(
                    preloadedEvidence.instances.begin(), preloadedEvidence.instances.end(), [](const auto &instance) {
                        return !instance.active && instance.preloaded && instance.processedBlocks == 0;
                    }), "diagnostics expose only configured dormant instances");
        const auto preloadCompleted = g_runtime.preloadCompleted;
        for (int i = 0; i < 20; ++i) preloadSavedSelections();
        const auto dedupeDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (vst3SelectionPending() && std::chrono::steady_clock::now() < dedupeDeadline) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            QThread::msleep(1);
        }
        require(!vst3SelectionPending() && g_runtime.preloadCompleted == preloadCompleted,
                "unchanged preload requests are deduplicated");
        const auto noScoreInstances = g_nextInstanceId.load();
        testBindings.clear();
        preloadSavedSelections();
        require(g_nextInstanceId.load() == noScoreInstances && !vst3SelectionPending(),
                "no open score keeps catalog metadata-only and does not enqueue preload");
        testBindings.push_back(binding);
        verifyColdSelection("track", entries);
        while (vst3SelectionPending() && std::chrono::steady_clock::now() < preloadDeadline) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            QThread::msleep(1);
        }
        require(g_runtime.trackRuntimes[0].chain.snapshot().activeSlot >= 0 &&
                    g_runtime.trackRuntimes[0].trackSlots[g_runtime.trackRuntimes[0].chain.snapshot().activeSlot].effects[0].get() == preloadedTrack,
                "track enable reuses the preloaded processor");
        verifyColdSelection({}, entries);
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
        require(syncTrackSelection("track", entries, &error), "prepare track processors");
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
        verifyTrackCancellationDuringPrepare(entries);
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
        require(!syncTrackSelection("track", missing, &error) && error == "runtime_vst3_not_found", "missing explicit selection rejected");
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
        require(syncTrackSelection("track", entries, &error), "restore explicit track selection");
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
        setSelectionNotifier(nullptr);
        qunsetenv("GPVST3_DISABLE_PROJECT_RESTORE");
        std::cout << "PASS: P8 real VST3 buffers, async selection, editor open/reopen/close, scope/state preservation and process failure isolation.\n";
        return 0;
    } catch (const std::exception &error) {
        g_runtime.master.installed = false; g_runtime.dsp.installed = false;
        shutdown(); std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
