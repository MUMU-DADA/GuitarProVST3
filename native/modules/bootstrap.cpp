#include "bootstrap.h"

#include "effect_chain.h"
#include "gp_hook.h"
#include "host_lock.h"
#include "qt_ui.h"
#include "state_manager.h"
#include "vst3_host.h"

#include <QtCore/QJsonArray>
#include <QtCore/QString>

namespace {

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
        {"ready", value.ready},
        {"worker_thread", value.workerThread},
        {"modules_discovered", value.modulesDiscovered},
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

QJsonObject hookStatus(const gpvst3::hook::State &value) {
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
        {"runtime_process_count", static_cast<qint64>(value.runtimeProcessCount)},
        {"runtime_configuration_mismatch_blocks", static_cast<qint64>(value.runtimeConfigurationMismatchBlocks)},
        {"runtime_configuration_matches", value.runtimeConfigurationMatches},
        {"runtime_effect_name", QString::fromUtf8(value.runtimeEffectName.data())},
        {"runtime_effect_error", QString::fromUtf8(value.runtimeEffectError.data())},
        {"total_bypass", value.totalBypass},
        {"chain_faulted", value.chainFaulted},
        {"chain_active_slot", value.chainActiveSlot},
        {"chain_prepared_slots", static_cast<qint64>(value.chainPreparedSlots)},
        {"chain_process_blocks", static_cast<qint64>(value.chainProcessBlocks)},
        {"chain_processed_blocks", static_cast<qint64>(value.chainProcessedBlocks)},
        {"chain_bypass_blocks", static_cast<qint64>(value.chainBypassBlocks)},
        {"chain_error_blocks", static_cast<qint64>(value.chainErrorBlocks)},
        {"chain_fallback_blocks", static_cast<qint64>(value.chainFallbackBlocks)},
        {"last_process_nanoseconds", static_cast<qint64>(value.lastProcessNanoseconds)},
        {"max_process_nanoseconds", static_cast<qint64>(value.maxProcessNanoseconds)},
        {"total_process_nanoseconds", static_cast<qint64>(value.totalProcessNanoseconds)},
        {"chain_switch_count", static_cast<qint64>(value.chainSwitchCount)},
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
        {"reason", QString::fromUtf8(value.reason.data())},
        {"master_process", entryStatus(value.masterProcess)},
        {"effects_chain_processDSP", entryStatus(value.effectsChainProcessDsp)},
        {"audio_output_callback", entryStatus(value.audioOutputCallback)}};
}

} // namespace

namespace gpvst3::bootstrap {

QJsonObject initialize() {
    const auto host = host::verify();
    hook::prepare(host);
    ui::setRealtimeBypassControl(&hook::setTotalBypass);
    const auto hookState = hook::snapshot();
    const auto vst3 = vst3::prepare(host.supported);
    effects::Chain chain;
    chain.setBypassed(!hookState.runtimeProcessorReady);

    QJsonObject fileResults;
    for (auto it = host.files.cbegin(); it != host.files.cend(); ++it)
        fileResults.insert(it.key(), it.value());

    return QJsonObject{
        {"schema", 1},
        {"status", host.supported ? "loaded" : "host_unsupported"},
        {"loaded", true},
        {"bypassed", chain.bypassed()},
        {"host", "Guitar Pro 8.1.1.17"},
        {"platform", "Windows x64"},
        {"qt_target", "5.15.3"},
        {"host_supported", host.supported},
        {"host_files", fileResults},
        {"vst3_host", vst3Status(vst3)},
        {"audio_adapter", QJsonObject{
            {"status", "planar_float32"},
            {"block_view", "pointer_view"},
            {"scratch_prepared_off_thread", true},
            {"realtime_process", "vst3_process_probe"}}},
        {"gp_hook", hookStatus(hookState)},
        {"qt_ui", ui::state()},
        {"state_manager", QJsonObject{{"status", "sidecar_json_p5"},
                                        {"path", state::sidecarPath()}}},
        {"reason", hookState.runtimeProcessorReady
                       ? "P2 runtime VST3 effect processing enabled by environment switch"
                       : (host.supported ? "P0 bootstrap complete; processing remains bypassed"
                                          : "Host files do not match the P0 lock")}
    };
}

QJsonObject hookSnapshot() {
    return hookStatus(hook::snapshot());
}

}
