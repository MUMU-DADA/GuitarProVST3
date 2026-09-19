#include "bootstrap.h"

#include "effect_chain.h"
#include "gp_audio_runtime.h"
#include "gp_hook.h"
#include "host_lock.h"
#include "qt_ui.h"
#include "state_manager.h"
#include "vst3_host.h"

#include <QtCore/QJsonArray>
#include <QtCore/QString>
#include <QtCore/QTimer>
#include <QtCore/QCoreApplication>
#include <QtCore/QMetaObject>
#include <QtCore/QPointer>
#include <atomic>
#ifdef GPVST3_P13_PROBE_BUILD
#include <QtCore/QJsonDocument>
#include <QtCore/QDir>
#include <QtCore/QSaveFile>
#endif

namespace {

QPointer<QTimer> g_scanTimer;
QPointer<QTimer> g_trackFallbackTimer;
QJsonObject g_scanStatus;
std::atomic<bool> g_trackRefreshQueued{false};
std::atomic<bool> g_stopping{false};
int g_trackFallbackAttempts = 0;
bool g_trackFallbackScoreOpen = false;

void dispatchTrackRefresh();
void ensureScanTimer();
void publishCurrentTrackContext() noexcept {
    gpvst3::gp_audio::Binding selected;
    if (gpvst3::gp_audio::currentTrack(selected))
        gpvst3::state::setRuntimeTrackContext(QString::fromStdString(selected.scoreKey),
            QString::fromStdString(selected.trackKey), selected.trackIndex,
            QString::fromStdString(selected.trackId));
    else gpvst3::state::clearRuntimeTrackContext();
}
void scheduleTrackRefresh() noexcept {
    if (g_stopping.load(std::memory_order_acquire)) return;
    auto *application = QCoreApplication::instance();
    if (!application) return;
    bool expected = false;
    if (!g_trackRefreshQueued.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;
    if (!QMetaObject::invokeMethod(application, [application] {
            QTimer::singleShot(0, application, [] { dispatchTrackRefresh(); });
        }, Qt::QueuedConnection)) g_trackRefreshQueued.store(false, std::memory_order_release);
}

void notifyTrackContextComplete() noexcept {
    if (auto *application = QCoreApplication::instance())
        QMetaObject::invokeMethod(application, [] {
            if (gpvst3::hook::consumeSelectionStateChanges()) gpvst3::ui::reloadVst3Selections();
            gpvst3::ui::syncVst3Selection();
            gpvst3::ui::refreshVst3TrackContext();
            gpvst3::hook::preloadSavedSelections();
        }, Qt::QueuedConnection);
}

void dispatchTrackRefresh() {
    g_trackRefreshQueued.store(false, std::memory_order_release);
    if (g_stopping.load(std::memory_order_acquire)) return;
    const bool scoreOpen = gpvst3::gp_audio::hasActiveDocument();
    const bool topologyEvent = gpvst3::gp_audio::consumeTopologyDirty();
    if (topologyEvent && QCoreApplication::instance()) {
        // Guitar Pro applies Score mutations asynchronously after the public
        // method returns. Give the host three bounded settling points so an
        // inserted/removed track is collected after its Conductor update,
        // while keeping the steady state fully event-driven.
        for (const int delay : {50, 200, 500})
            QTimer::singleShot(delay, QCoreApplication::instance(), [] {
                gpvst3::gp_audio::markDirty();
            });
    }
    if (g_trackFallbackTimer && scoreOpen &&
        ((topologyEvent && !g_trackFallbackTimer->isActive()) || !g_trackFallbackScoreOpen)) {
        g_trackFallbackAttempts = 0;
        g_trackFallbackTimer->start();
    }
    g_trackFallbackScoreOpen = scoreOpen;
    if (gpvst3::hook::editorCallbackActive() && !gpvst3::gp_audio::refreshNeeded()) {
        // An open editor must not trigger a full native graph walk, but the
        // MCP bridge can still provide the selected-track flags needed to
        // switch the track scope before opening the next editor.
        gpvst3::gp_audio::refreshSelectionContext();
        publishCurrentTrackContext();
        gpvst3::ui::refreshVst3TrackContext();
        return;
    }
    gpvst3::gp_audio::refreshSelectionContext();
    publishCurrentTrackContext();
    // A Score mutation may complete without emitting a Qt lifecycle event.
    // During the bounded settling window the check is a cheap pointer/lifetime
    // comparison over the cached score. Steady-state cursor events skip it.
    if (topologyEvent || (g_trackFallbackTimer && g_trackFallbackTimer->isActive()))
        gpvst3::gp_audio::checkStructureChanged();
    // Publish completion before enqueueing another context refresh; otherwise
    // syncSelection can keep seeing our newly queued work as still busy.
    if (gpvst3::hook::consumeSelectionStateChanges()) gpvst3::ui::reloadVst3Selections();
    gpvst3::ui::syncVst3Selection();
    gpvst3::hook::refreshTrackContext();
    gpvst3::ui::refreshVst3TrackContext();
    // Project graph preparation is event-driven and score-scoped. Catalog
    // completion alone must never instantiate every discovered module.
    gpvst3::hook::preloadSavedSelections();
    const bool scoreOpenAfterRefresh = gpvst3::gp_audio::hasActiveDocument();
    if (g_trackFallbackTimer && scoreOpenAfterRefresh && !g_trackFallbackScoreOpen) {
        g_trackFallbackAttempts = 0;
        g_trackFallbackTimer->start();
    }
    g_trackFallbackScoreOpen = scoreOpenAfterRefresh;
}

void ensureScanTimer() {
    auto *application = QCoreApplication::instance();
    if (!application) return;
    if (!g_scanTimer) {
        g_scanTimer = new QTimer(application);
        g_scanTimer->setInterval(100);
        QObject::connect(g_scanTimer, &QTimer::timeout, g_scanTimer, [] {
            gpvst3::bootstrap::pollVst3(g_scanStatus);
            if (!gpvst3::vst3::pollNeeded()) g_scanTimer->stop();
        });
    }
    if (gpvst3::vst3::pollNeeded()) g_scanTimer->start();
}

QJsonObject classStatus(const gpvst3::vst3::ClassState &value) {
    return QJsonObject{
        {"module", QString::fromUtf8(value.module.data())},
        {"class_id", QString::fromUtf8(value.classId.data())},
        {"name", QString::fromUtf8(value.name.data())},
        {"vendor", QString::fromUtf8(value.vendor.data())},
        {"category", QString::fromUtf8(value.category.data())},
        {"version", QString::fromUtf8(value.version.data())},
        {"sdk_version", QString::fromUtf8(value.sdkVersion.data())},
        {"component_created", value.componentCreated},
        {"component_initialized", value.componentInitialized},
        {"processor_ready", value.processorReady},
        {"controller_created", value.controllerCreated},
        {"controller_initialized", value.controllerInitialized},
        {"active", value.active},
        {"processing", value.processing},
        {"state_round_trip", value.stateRoundTrip},
        {"controller_state_round_trip", value.controllerStateRoundTrip},
        {"bypass_parameter", value.bypassParameter},
        {"bypass_round_trip", value.bypassRoundTrip},
        {"parameter_count", value.parameterCount},
        {"component_state_bytes", static_cast<qint64>(value.componentStateBytes)},
        {"controller_state_bytes", static_cast<qint64>(value.controllerStateBytes)},
        {"latency_samples", static_cast<qint64>(value.latencySamples)},
        {"tail_samples", static_cast<qint64>(value.tailSamples)},
        {"process_probe_passed", value.processProbePassed},
        {"process_probe_frames", static_cast<qint64>(value.processProbeFrames)},
        {"error", QString::fromUtf8(value.error.data())}
    };
}

QJsonObject vst3Status(const gpvst3::vst3::State &value) {
    QJsonArray classes;
    for (const auto &item : value.classes) classes.append(classStatus(item));
    QJsonArray errors;
    for (const auto &item : value.errors) errors.append(QString::fromUtf8(item.data()));
    return QJsonObject{
        {"status", QString::fromUtf8(value.status.data())},
        {"host_supported", value.hostSupported},
        {"ready", value.ready},
        {"worker_thread", value.workerThread},
        {"scan_pending", value.scanPending},
        {"scan_mode", value.staticScan ? "static_files" : "explicit_lifecycle_probe"},
        {"cache_hit", value.cacheHit}, {"cache_status", QString::fromStdString(value.cacheStatus)},
        {"files_checked", value.filesChecked}, {"metadata_reads", value.metadataReads},
        {"cache_reused", value.cacheReused}, {"modules_checked", value.modulesChecked},
        {"scan_generation", value.scanGeneration}, {"elapsed_ms", static_cast<qint64>(value.elapsedMs)},
        {"current_module", QString::fromStdString(value.currentModule)},
        {"modules_discovered", value.modulesDiscovered},
        {"recognition_pending", value.recognitionPending},
        {"recognition_worker", value.recognitionWorker},
        {"recognition_attempted", value.recognitionAttempted},
        {"recognition_completed", value.recognitionCompleted},
        {"recognition_failed", value.recognitionFailed},
        {"recognition_timed_out", value.recognitionTimedOut},
        {"recognition_workers_started", value.recognitionWorkersStarted},
        {"recognition_workers_detached", value.recognitionWorkersDetached},
        {"recognition_current_module", QString::fromStdString(value.recognitionCurrentModule)},
        {"recognition_status", QString::fromStdString(value.recognitionStatus)},
        {"modules_loaded", value.modulesLoaded},
        {"classes_enumerated", value.classesEnumerated},
        {"instances_created", value.instancesCreated},
        {"lifecycles_passed", value.lifecyclesPassed},
        {"process_calls", value.processCalls},
        {"process_probes_passed", value.processProbesPassed},
        {"classes", classes},
        {"errors", errors}
    };
}

QJsonArray vst3Catalog(const gpvst3::vst3::State &value) {
    QJsonArray result;
    for (const auto &entry : gpvst3::vst3::effectCatalog(value)) {
        result.append(QJsonObject{
            {"module", QString::fromUtf8(entry.module.data())},
            {"class_id", QString::fromUtf8(entry.classId.data())},
            {"name", QString::fromUtf8(entry.name.data())},
            {"vendor", QString::fromUtf8(entry.vendor.data())},
            {"category", QString::fromUtf8(entry.category.data())},
            {"compatible", entry.compatible},
            {"identified", entry.identified},
            {"source", QString::fromStdString(entry.source)},
            {"error", QString::fromUtf8(entry.error.data())},
            {"recognition_status", QString::fromStdString(entry.recognitionStatus)},
            {"recognition_source", QString::fromStdString(entry.recognitionSource)},
            {"recognition_attempts", entry.recognitionAttempts},
            {"recognition_error", QString::fromStdString(entry.recognitionError)},
            {"recognition_retry_after", static_cast<qint64>(entry.recognitionRetryAfter)},
            {"recognition_deadline_at", static_cast<qint64>(entry.recognitionDeadlineAt)},
            {"recognition_ignored_reason", QString::fromStdString(entry.recognitionIgnoredReason)}});
    }
    return result;
}

void scanFeedback(const gpvst3::vst3::State &scan) {
    QStringList details;
    for (const auto &error : scan.errors) details.append(QString::fromStdString(error));
    if (scan.recognitionTimedOut > 0)
        details.prepend(QStringLiteral("识别超时：%1 个（已从当前列表隐藏）").arg(scan.recognitionTimedOut));
    if (!scan.currentModule.empty()) details.prepend(QString::fromStdString(scan.currentModule));
    if (!scan.recognitionCurrentModule.empty())
        details.prepend(QStringLiteral("识别：") + QString::fromStdString(scan.recognitionCurrentModule));
    const auto phase = scan.recognitionPending || scan.recognitionWorker
        ? QStringLiteral("recognition") : QString::fromStdString(scan.status);
    gpvst3::ui::setVst3ScanState(phase, scan.modulesChecked,
                                scan.modulesDiscovered, scan.cacheHit, details.join('\n'));
}

void refreshCatalog(bool retryTimedOut = false) {
    const auto pending = gpvst3::vst3::beginAsync(gpvst3::hook::snapshot().hostSupported, retryTimedOut);
    scanFeedback(pending);
    ensureScanTimer();
}

QJsonArray identifyBundle(const QString &module, QString *error) {
    // Selection is explicit, but the version/configuration gate still precedes third-party code.
    const auto host = gpvst3::host::verify();
    if (!host.supported || qEnvironmentVariable("GPVST3_ENABLE_P2_HOOK") == "0") {
        if (error) *error = host.supported ? QStringLiteral("realtime_disabled_by_environment") : QStringLiteral("host_unsupported");
        return {};
    }
    const auto state = gpvst3::vst3::identifyBundle(module.toStdString(), true);
    QJsonArray result;
    for (const auto &value : vst3Catalog(state)) if (value.toObject().value("identified").toBool()) result.append(value);
    if (result.isEmpty() && error) *error = state.errors.empty() ? QStringLiteral("未识别到音频效果器")
        : QString::fromStdString(state.errors.front());
    return result;
}

QJsonObject hookStatus(const gpvst3::hook::State &value) {
    QJsonArray instances;
    for (const auto &entry : value.instances) instances.append(QJsonObject{
        {"scope", QString::fromStdString(entry.scope)}, {"track_key", QString::fromStdString(entry.trackKey)},
        {"module", QString::fromStdString(entry.module)}, {"class_id", QString::fromStdString(entry.classId)},
        {"instance_id", QString::number(entry.instanceId)}, {"processed_blocks", qint64(entry.processedBlocks)},
        {"sample_rate", entry.sampleRate}, {"active", entry.active}, {"preloaded", entry.preloaded}});
    const auto entryStatus = [](const gpvst3::hook::EntryPointObservation &entry) {
        return QJsonObject{
            {"module_loaded", entry.moduleLoaded},
            {"export_found", entry.exportFound},
            {"call_observed", entry.callObserved},
            {"buffer_write_observed", entry.bufferWriteObserved},
            {"call_count", static_cast<qint64>(entry.callCount)},
            {"frame_count", static_cast<qint64>(entry.frameCount)},
            {"channel_count", static_cast<qint64>(entry.channelCount)},
            {"sample_rate", entry.sampleRate},
            {"thread_id", static_cast<qint64>(entry.threadId)},
            {"first_sequence", static_cast<qint64>(entry.firstSequence)},
            {"last_sequence", static_cast<qint64>(entry.lastSequence)},
            {"first_buffer_address", QString::number(static_cast<qulonglong>(entry.firstBufferAddress), 16)},
            {"last_buffer_address", QString::number(static_cast<qulonglong>(entry.lastBufferAddress), 16)},
            {"before_hash", QString::number(static_cast<qulonglong>(entry.beforeHash), 16)},
            {"after_hash", QString::number(static_cast<qulonglong>(entry.afterHash), 16)}};
    };
    QJsonArray trackRuntimeEvidence;
    for (const auto &track : value.trackRuntimeEvidence) {
        trackRuntimeEvidence.append(QJsonObject{
            {"track_key", QString::fromStdString(track.trackKey)},
            {"track_id", QString::fromStdString(track.trackId)},
            {"process_blocks", static_cast<qint64>(track.processBlocks)},
            {"processed_blocks", static_cast<qint64>(track.processedBlocks)},
            {"bypass_blocks", static_cast<qint64>(track.bypassBlocks)},
            {"error_blocks", static_cast<qint64>(track.errorBlocks)},
            {"configured_effects", static_cast<qint64>(track.configuredEffects)},
            {"configured", track.configured},
            {"processed", track.processed},
            {"write_observed", track.writeObserved}});
    }
    return QJsonObject{
        {"installed", value.installed},
        {"enabled", value.enabled},
        {"host_supported", value.hostSupported},
        {"observation_only", value.observationOnly},
        {"audio_buffer_accessors_found", value.audioBufferAccessorsFound},
        {"audio_buffer_lock_accessors_found", value.audioBufferLockAccessorsFound},
        {"audio_buffer_pointer_observed", value.audioBufferPointerObserved},
        {"audio_buffer_writeback_observed", value.audioBufferWritebackObserved},
        {"audio_output_callback_installed", value.audioOutputCallbackInstalled},
        {"audio_output_observed", value.audioOutputObserved},
        {"audio_output_writeback_observed", value.audioOutputWritebackObserved},
        {"effects_chain_inside_master", value.effectsChainInsideMaster},
        {"effects_chain_after_master_observed", value.effectsChainAfterMasterObserved},
        {"cross_thread_observed", value.crossThreadObserved},
        {"same_buffer_observed", value.sameBufferObserved},
        {"runtime_effect_enabled", value.runtimeEffectEnabled},
        {"runtime_processor_ready", value.runtimeProcessorReady},
        {"runtime_process_observed", value.runtimeProcessObserved},
        {"runtime_buffer_write_observed", value.runtimeBufferWriteObserved},
        {"preload_pending", value.preloadPending},
        {"global_preloaded", value.globalPreloaded},
        {"input_preloaded", value.inputPreloaded},
        {"track_preloaded", static_cast<qint64>(value.trackPreloaded)},
        {"warm_cache_limit", static_cast<qint64>(value.warmCacheLimit)},
        {"warm_cache_evictions", static_cast<qint64>(value.warmCacheEvictions)},
        {"preload_completed", qint64(value.preloadCompleted)}, {"preload_failed", qint64(value.preloadFailed)},
        {"preload_error", QString::fromStdString(value.preloadError)}, {"instances", instances},
        {"runtime_process_count", static_cast<qint64>(value.runtimeProcessCount)},
        {"runtime_configuration_mismatch_blocks", static_cast<qint64>(value.runtimeConfigurationMismatchBlocks)},
        {"runtime_configuration_matches", value.runtimeConfigurationMatches},
        {"runtime_effect_name", QString::fromUtf8(value.runtimeEffectName.data())},
        {"runtime_effect_error", QString::fromUtf8(value.runtimeEffectError.data())},
        {"selection_request_id", static_cast<qint64>(value.selectionRequestId)},
        {"selection_queued_nanoseconds", static_cast<qint64>(value.selectionQueuedNanoseconds)},
        {"selection_worker_started_nanoseconds", static_cast<qint64>(value.selectionWorkerStartedNanoseconds)},
        {"selection_prepared_nanoseconds", static_cast<qint64>(value.selectionPreparedNanoseconds)},
        {"selection_committed_nanoseconds", static_cast<qint64>(value.selectionCommittedNanoseconds)},
        {"selection_applied_generation", static_cast<qint64>(value.selectionAppliedGeneration)},
        {"selection_generation", static_cast<qint64>(value.selectionGeneration)},
        {"binding_generation", static_cast<qint64>(value.bindingGeneration)},
        {"context_publish_latency_nanoseconds", static_cast<qint64>(value.contextPublishLatencyNanoseconds)},
        {"dropped_refresh_count", static_cast<qint64>(value.droppedRefreshCount)},
        {"score_open", value.scoreOpen},
        {"selection_event_source", QString::fromStdString(value.selectionEventSource)},
        {"selection_hook_installed", value.selectionHookInstalled},
        {"selection_hook_gate_passed", value.selectionHookGatePassed},
        {"topology_hook_installed", value.topologyHookInstalled},
        {"topology_hook_gate_passed", value.topologyHookGatePassed},
        {"topology_duplicate_calls", static_cast<qint64>(value.topologyDuplicateCalls)},
        {"topology_swap_calls", static_cast<qint64>(value.topologySwapCalls)},
        {"topology_event_count", static_cast<qint64>(value.topologyEventCount)},
        {"audio_generation", static_cast<qint64>(value.audioGeneration)},
        {"selection_status", QString::fromStdString(value.selectionStatus)},
        {"editor_stage", QString::fromStdString(value.editorStage)},
        {"editor_identity", QString::fromStdString(value.editorIdentity)},
        {"editor_error", QString::fromStdString(value.editorError)},
        {"editor_result_code", static_cast<qint64>(value.editorResultCode)},
        {"editor_request_generation", static_cast<qint64>(value.editorRequestGeneration)},
        {"chain_activation_nanoseconds", static_cast<qint64>(value.chainActivationNanoseconds)},
        {"chain_first_processed_nanoseconds", static_cast<qint64>(value.chainFirstProcessedNanoseconds)},
        {"chain_activation_sequence", static_cast<qint64>(value.chainActivationSequence)},
        {"chain_first_processed_sequence", static_cast<qint64>(value.chainFirstProcessedSequence)},
        {"chain_callbacks_to_first_process", static_cast<qint64>(value.chainCallbacksToFirstProcess)},
        {"input_activation_nanoseconds", static_cast<qint64>(value.inputActivationNanoseconds)},
        {"input_first_processed_nanoseconds", static_cast<qint64>(value.inputFirstProcessedNanoseconds)},
        {"input_first_processed_sequence", static_cast<qint64>(value.inputFirstProcessedSequence)},
        {"input_callbacks_to_first_process", static_cast<qint64>(value.inputCallbacksToFirstProcess)},
        {"total_bypass", value.totalBypass},
        {"chain_faulted", value.chainFaulted},
        {"chain_active_slot", value.chainActiveSlot},
        {"chain_prepared_slots", static_cast<qint64>(value.chainPreparedSlots)},
        {"chain_process_blocks", static_cast<qint64>(value.chainProcessBlocks)},
        {"chain_processed_blocks", static_cast<qint64>(value.chainProcessedBlocks)},
        {"chain_bypass_blocks", static_cast<qint64>(value.chainBypassBlocks)},
        {"chain_error_blocks", static_cast<qint64>(value.chainErrorBlocks)},
        {"chain_fallback_blocks", static_cast<qint64>(value.chainFallbackBlocks)},
        {"chain_switch_requests", static_cast<qint64>(value.chainSwitchRequests)},
        {"chain_switch_prepared", static_cast<qint64>(value.chainSwitchPrepared)},
        {"chain_retired_slots", static_cast<qint64>(value.chainRetiredSlots)},
        {"chain_last_switch_nanoseconds", static_cast<qint64>(value.chainLastSwitchNanoseconds)},
        {"chain_max_switch_nanoseconds", static_cast<qint64>(value.chainMaxSwitchNanoseconds)},
        {"chain_last_reader_drain_nanoseconds", static_cast<qint64>(value.chainLastReaderDrainNanoseconds)},
        {"chain_max_reader_drain_nanoseconds", static_cast<qint64>(value.chainMaxReaderDrainNanoseconds)},
        {"chain_reader_drain_timeouts", static_cast<qint64>(value.chainReaderDrainTimeouts)},
        {"chain_sequence_gaps", static_cast<qint64>(value.chainSequenceGaps)},
        {"chain_last_sequence", static_cast<qint64>(value.chainLastSequence)},
        {"chain_ramp_samples", static_cast<qint64>(value.chainRampSamples)},
        {"chain_ramp_remaining", static_cast<qint64>(value.chainRampRemaining)},
        {"last_process_nanoseconds", static_cast<qint64>(value.lastProcessNanoseconds)},
        {"max_process_nanoseconds", static_cast<qint64>(value.maxProcessNanoseconds)},
        {"total_process_nanoseconds", static_cast<qint64>(value.totalProcessNanoseconds)},
        {"chain_switch_count", static_cast<qint64>(value.chainSwitchCount)},
        {"global_chain_enabled", value.globalChainEnabled},
        {"global_chain_process_blocks", static_cast<qint64>(value.globalChainProcessBlocks)},
        {"track_chain_process_blocks", static_cast<qint64>(value.trackChainProcessBlocks)},
        {"track_chain_processed_blocks", static_cast<qint64>(value.trackChainProcessedBlocks)},
        {"track_dispatch_misses", static_cast<qint64>(value.trackDispatchMisses)},
        {"track_last_dsp_self", QString::number(static_cast<qulonglong>(value.trackLastDspSelf), 16)},
        {"track_dispatch_self0", QString::number(static_cast<qulonglong>(value.trackDispatchSelf0), 16)},
        {"track_dispatch_self1", QString::number(static_cast<qulonglong>(value.trackDispatchSelf1), 16)},
        {"track_bindings_published", static_cast<qint64>(value.trackBindingsPublished)},
        {"track_runtime_processed", value.trackRuntimeProcessed},
        {"track_runtime_write_observed", value.trackRuntimeWriteObserved},
        {"track_runtime_error", QString::fromStdString(value.trackRuntimeError)},
        {"track_binding_source", QString::fromStdString(value.trackBindingSource)},
        {"track_runtime_evidence", trackRuntimeEvidence},
        {"track_context_observed", value.trackContextObserved},
        {"track_context_stable", value.trackContextStable},
        {"track_scope_unresolved", value.trackScopeUnresolved},
        {"effects_chain_index_accessor_found", value.effectsChainIndexAccessorFound},
        {"effects_chain_index_observed", value.effectsChainIndexObserved},
        {"observed_effects_chain_index", value.observedEffectsChainIndex},
        {"effects_chain_context_count", static_cast<qint64>(value.effectsChainContextCount)},
        {"track_context_key", QString::fromStdString(value.trackContextKey)},
        {"audio_buffer_sequence_count", static_cast<qint64>(value.audioBufferSequenceCount)},
        {"runtime_effect_instances", static_cast<qint64>(value.runtimeEffectInstances)},
        {"reconfiguration_passed", static_cast<qint64>(value.reconfigurationPassed)},
        {"reconfiguration_failed", static_cast<qint64>(value.reconfigurationFailed)},
        {"reconfiguration_validated", value.reconfigurationValidated},
        {"audio_layer_input_level_accessor_found", value.audioLayerInputLevelAccessorFound},
        {"audio_layer_input_level_observed", value.audioLayerInputLevelObserved},
        {"audio_layer_stream_running", value.audioLayerStreamRunning},
        {"audio_layer_buffer_size", static_cast<qint64>(value.audioLayerBufferSize)},
        {"input_capture_path_located", value.inputCapturePathLocated},
        {"input_capture_observed", value.inputCaptureObserved},
        {"input_route_enabled", value.inputRouteEnabled},
        {"input_monitor", gpvst3::hook::inputMonitorSnapshot()},
        {"input_after_original_blocks", qint64(value.inputAfterOriginalBlocks)},
        {"input_post_original_hash", QString::number(value.inputPostOriginalHash, 16)},
        {"input_post_route_hash", QString::number(value.inputPostRouteHash, 16)},
        {"input_order_samples_observed", value.inputOrderSamplesObserved},
        {"input_order_capture_sample", value.inputOrderCaptureSample},
        {"input_order_generated_sample", value.inputOrderGeneratedSample},
        {"input_order_output_sample", value.inputOrderOutputSample},
        {"input_processor_ready", value.inputProcessorReady},
        {"input_route", QString::fromUtf8(value.inputRoute.data())},
        {"input_route_reason", QString::fromUtf8(value.inputRouteReason.data())},
        {"input_capture_blocks", static_cast<qint64>(value.inputCaptureBlocks)},
        {"input_processed_blocks", static_cast<qint64>(value.inputProcessedBlocks)},
        {"input_bus_mixed_blocks", static_cast<qint64>(value.inputBusMixedBlocks)},
        {"input_bypass_blocks", static_cast<qint64>(value.inputBypassBlocks)},
        {"input_error_blocks", static_cast<qint64>(value.inputErrorBlocks)},
        {"input_dropped_blocks", static_cast<qint64>(value.inputDroppedBlocks)},
        {"input_frame_count", static_cast<qint64>(value.inputFrameCount)},
        {"input_channel_count", static_cast<qint64>(value.inputChannelCount)},
        {"input_sample_rate", value.inputSampleRate},
        {"input_last_peak", value.inputLastPeak},
        {"input_max_peak", value.inputMaxPeak},
        {"input_last_rms", value.inputLastRms},
        {"input_interleaved_format_observed", value.inputInterleavedFormatObserved},
        {"input_interleaved_observed", value.inputInterleavedObserved},
        {"input_interleaved_output_written", value.inputInterleavedOutputWritten},
        {"input_interleaved_blocks", static_cast<qint64>(value.inputInterleavedBlocks)},
        {"input_interleaved_format_errors", static_cast<qint64>(value.inputInterleavedFormatErrors)},
        {"input_interleaved_missing_blocks", static_cast<qint64>(value.inputInterleavedMissingBlocks)},
        {"input_interleaved_input_channel_count", static_cast<qint64>(value.inputInterleavedInputChannelCount)},
        {"input_interleaved_output_channel_count", static_cast<qint64>(value.inputInterleavedOutputChannelCount)},
        {"input_first_capture_address", QString::number(static_cast<qulonglong>(value.inputFirstCaptureAddress), 16)},
        {"input_last_capture_address", QString::number(static_cast<qulonglong>(value.inputLastCaptureAddress), 16)},
        {"input_first_capture_owner", QString::number(static_cast<qulonglong>(value.inputFirstCaptureOwner), 16)},
        {"input_last_capture_owner", QString::number(static_cast<qulonglong>(value.inputLastCaptureOwner), 16)},
        {"input_first_output_address", QString::number(static_cast<qulonglong>(value.inputFirstOutputAddress), 16)},
        {"input_last_output_address", QString::number(static_cast<qulonglong>(value.inputLastOutputAddress), 16)},
        {"input_capture_format", QString::fromUtf8(value.inputCaptureFormat.data())},
        {"input_capture_channel_layout", QString::fromUtf8(value.inputCaptureChannelLayout.data())},
        {"input_capture_ownership", QString::fromUtf8(value.inputCaptureOwnership.data())},
        {"input_configuration_observed", value.inputConfigurationObserved},
        {"input_configured_input_channels", static_cast<qint64>(value.inputConfiguredInputChannels)},
        {"input_configured_output_channels", static_cast<qint64>(value.inputConfiguredOutputChannels)},
        {"input_configured_sample_rate", value.inputConfiguredSampleRate},
        {"input_configuration_errors", static_cast<qint64>(value.inputConfigurationErrors)},
        {"reason", QString::fromUtf8(value.reason.data())},
        {"master_process", entryStatus(value.masterProcess)},
        {"effects_chain_processDSP", entryStatus(value.effectsChainProcessDsp)},
        {"audio_output_callback", entryStatus(value.audioOutputCallback)}};
}

} // namespace

namespace gpvst3::bootstrap {

void shutdown() noexcept {
    g_stopping.store(true, std::memory_order_release);
#ifdef GPVST3_P13_PROBE_BUILD
    if (qEnvironmentVariable("GPVST3_P13_PROBE") == "1") {
        try {
            QSaveFile file(QDir(state::dataDirectory()).filePath("p13-runtime-final.json"));
            auto final = hook::stopInputTimingProbe();
            final.insert("schema", 1);
            final.insert("acceptance", "not_evaluated");
            final.insert("input_monitor", hook::inputMonitorSnapshot());
            const auto bytes = QJsonDocument(final).toJson();
            if (file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size()) file.commit();
        } catch (...) {}
    }
    if (qEnvironmentVariable("GPVST3_P13_STREAM_PROBE") == "1") {
        try {
            QSaveFile file(QDir(state::dataDirectory()).filePath("p13-stream-lifecycle-final.json"));
            const auto bytes = QJsonDocument(hook::inputStreamProbeSnapshot()).toJson();
            if (file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size()) file.commit();
        } catch (...) {}
    }
#endif
    if (g_scanTimer) g_scanTimer->stop();
    if (g_trackFallbackTimer) g_trackFallbackTimer->stop();
    gp_audio::setRefreshNotifier(nullptr);
    hook::setSelectionNotifier(nullptr);
    hook::setTrackContextNotifier(nullptr);
}

QJsonObject initialize() {
    return initialize(host::verify());
}

QJsonObject initialize(const host::Verification &verification) {
    g_stopping.store(false, std::memory_order_release);
    const auto host = verification;
    const bool enabled = state::pluginEnabled();
    if (enabled) state::migrateDesiredEnabledIntent();
    if (enabled) {
        gpvst3::gp_audio::initialize(host.supported, host.qtCoreSupported);
        gpvst3::gp_audio::setRefreshNotifier(&scheduleTrackRefresh);
        hook::prepare(host);
        hook::setSelectionNotifier(&scheduleTrackRefresh);
        hook::setTrackContextNotifier(&notifyTrackContextComplete);
        // Native score discovery can walk Guitar Pro's object graph. Defer
        // the first walk until the startup callback has returned to Qt so the
        // host can paint and accept input before that control-thread work.
        if (auto *application = QCoreApplication::instance())
            QTimer::singleShot(75, application, [] {
                if (!g_stopping.load(std::memory_order_acquire)) hook::refreshTrackContext();
            });
        ui::setRealtimeBypassControl(&hook::setTotalBypass);
        ui::setVst3SelectionControl(&hook::setGlobalVst3Selection);
        ui::setVst3SelectionRequestControl(&hook::requestGlobalVst3Selection);
        ui::setVst3BusyControl(&hook::vst3SelectionPending);
        ui::setVst3TrackSelectionControl(&hook::setTrackVst3Selection);
        ui::setVst3TrackSelectionRequestControl(&hook::requestTrackVst3Selection);
        ui::setVst3TrackGenerationRequestControl(&hook::requestTrackVst3SelectionAtGeneration);
        ui::setVst3StateControl(&hook::captureGlobalVst3States);
        ui::setVst3TrackControls(&hook::captureTrackVst3States, &hook::openTrackVst3Editor,
                                 &hook::activeTrackVst3States);
        ui::setVst3EditorControl(&hook::openVst3Editor, &hook::closeVst3Editors, &hook::scaleVst3Editor);
        ui::setVst3InputControls(&hook::requestInputVst3Selection, &hook::captureInputVst3States,
                                &hook::openInputVst3Editor, &hook::requestInputMonitorSettings,
                                &hook::inputMonitorSnapshot, &hook::activeInputVst3States,
                                &hook::setNativeInputState, &gp_audio::nativeInputAction);
    } else {
        ui::setRealtimeBypassControl(nullptr);
        ui::setVst3SelectionControl(nullptr);
        ui::setVst3SelectionRequestControl(nullptr);
        ui::setVst3TrackSelectionControl(nullptr);
        ui::setVst3TrackSelectionRequestControl(nullptr);
        ui::setVst3TrackGenerationRequestControl(nullptr);
        ui::setVst3StateControl(nullptr);
        ui::setVst3TrackControls(nullptr, nullptr);
        ui::setVst3EditorControl(nullptr, nullptr, nullptr);
        ui::setVst3InputControls(nullptr, nullptr, nullptr, nullptr, nullptr);
    }
    const auto hookState = hook::snapshot();
#ifdef GPVST3_P13_PROBE_BUILD
    if (enabled && qEnvironmentVariable("GPVST3_P13_PROBE") == "1") {
        // A bounded experiment window, never a production maintenance poll.
        auto *probeTimer = new QTimer(QCoreApplication::instance());
        probeTimer->setInterval(1000);
        QObject::connect(probeTimer, &QTimer::timeout, probeTimer, [probeTimer, remaining = 90]() mutable {
            if (g_stopping.load(std::memory_order_acquire)) {
                probeTimer->stop();
                probeTimer->deleteLater();
                return;
            }
            const auto probe = gpvst3::hook::inputProbeSnapshot();
            QSaveFile file(QDir(gpvst3::state::dataDirectory()).filePath("p13-input-probe.json"));
            bool written = false;
            if (file.open(QIODevice::WriteOnly)) {
                const auto bytes = QJsonDocument(probe).toJson();
                if (file.write(bytes) == bytes.size()) written = file.commit();
                else file.cancelWriting();
            }
            if (--remaining <= 0 || (written && (probe.value("complete").toBool() || !probe.value("enabled").toBool()))) {
                probeTimer->stop();
                probeTimer->deleteLater();
            }
        });
        probeTimer->start();
    }
#endif
    vst3::State vst3;
    if (enabled) {
        vst3::setRecognitionControl(&vst3::identifyBundle);
        ui::setVst3DiscoveryControl([] { refreshCatalog(true); }, &identifyBundle);
        vst3 = qEnvironmentVariable("GPVST3_RUN_LIFECYCLE_PROBE") == "1"
            ? vst3::prepare(host.supported) : vst3::beginAsync(host.supported);
        g_scanStatus = QJsonObject{};
        g_scanStatus.insert("vst3_host", vst3Status(vst3));
        g_scanStatus.insert("vst3_catalog", vst3Catalog(vst3));
        ensureScanTimer();
        g_trackFallbackTimer = new QTimer(QCoreApplication::instance());
        // Rebuild settling is intentionally short and bounded. A 250 ms
        // window catches Guitar Pro's asynchronous Conductor update without
        // reintroducing a steady-state polling loop.
        g_trackFallbackTimer->setInterval(250);
        QObject::connect(g_trackFallbackTimer, &QTimer::timeout, g_trackFallbackTimer, [] {
            const auto state = gpvst3::hook::snapshot();
            if (!gpvst3::gp_audio::hasActiveDocument() || g_trackFallbackAttempts >= 8) {
                g_trackFallbackTimer->stop();
                return;
            }
            ++g_trackFallbackAttempts;
            // Some Guitar Pro builds expose cursor changes only through the
            // native score model. During the bounded settling window, one
            // full refresh every two seconds is the documented fallback.
            gpvst3::gp_audio::markDirty();
            gpvst3::gp_audio::checkStructureChanged();
            if (gpvst3::gp_audio::refreshIncomplete()) gpvst3::gp_audio::markDirty();
            scheduleTrackRefresh();
        });
        g_trackFallbackAttempts = 0;
        g_trackFallbackScoreOpen = gpvst3::gp_audio::hasActiveDocument();
        g_trackFallbackTimer->start();
    } else {
        vst3.status = "disabled_by_user";
        ui::setVst3DiscoveryControl(nullptr, nullptr);
        ui::setVst3Catalog({});
        ui::setVst3ScanState(QStringLiteral("disabled"), 0, 0, false, {});
    }
    const auto catalog = vst3Catalog(vst3);
    if (enabled) {
        hook::setVst3Catalog(catalog);
        if (hook::consumeSelectionStateChanges()) ui::reloadVst3Selections();
    }
    ui::setVst3Catalog(catalog);
    scanFeedback(vst3);
    effects::Chain chain;
    chain.setBypassed(!hookState.runtimeProcessorReady);

    QJsonObject fileResults;
    for (auto it = host.files.cbegin(); it != host.files.cend(); ++it)
        fileResults.insert(it.key(), it.value());

    return QJsonObject{
        {"schema", 1},
        {"status", !enabled ? "disabled_by_user" : (host.supported ? "loaded" : "host_unsupported")},
        {"loaded", true},
        {"bypassed", chain.bypassed()},
        {"host", "Guitar Pro 8.1.1.17"},
        {"platform", "Windows x64"},
        {"qt_target", "5.15.3"},
        {"host_supported", host.supported},
        {"host_files", fileResults},
        {"vst3_host", vst3Status(vst3)},
        {"vst3_catalog", catalog},
        {"audio_adapter", QJsonObject{
            {"status", "planar_float32"},
            {"block_view", "pointer_view"},
            {"scratch_prepared_off_thread", true},
            {"realtime_process", "vst3_process_probe"}}},
        {"gp_hook", hookStatus(hookState)},
        {"qt_ui", ui::state()},
        {"state_manager", QJsonObject{{"status", "sidecar_json_p7_enabled"},
                                        {"path", state::sidecarPath()}}},
        {"reason", !enabled ? "plugin_disabled_by_user"
                   : hookState.runtimeProcessorReady
                       ? "P2 runtime VST3 effect processing enabled by environment switch"
                       : (host.supported ? "P0 bootstrap complete; processing remains bypassed"
                                          : "Host files do not match the P0 lock")}
    };
}

bool pollVst3(QJsonObject &status) {
    vst3::State completed;
    if (!vst3::poll(completed)) return false;
    const auto catalog = vst3Catalog(completed);
    hook::setVst3Catalog(catalog);
    if (hook::consumeSelectionStateChanges()) ui::reloadVst3Selections();
    ui::setVst3Catalog(catalog);
    scanFeedback(completed);
    status.insert("vst3_host", vst3Status(completed));
    status.insert("vst3_catalog", catalog);
    state::writeStatus(status);
    if (!vst3::pollNeeded() && g_scanTimer) g_scanTimer->stop();
    return true;
}

QJsonObject hookSnapshot() {
    return hookStatus(hook::snapshot());
}

}
