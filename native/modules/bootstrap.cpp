#include "bootstrap.h"

#include "effect_chain.h"
#include "gp_hook.h"
#include "host_lock.h"
#include "qt_ui.h"
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
        {"classes", classes},
        {"errors", errors}
    };
}

} // namespace

namespace gpvst3::bootstrap {

QJsonObject initialize() {
    const auto host = host::verify();
    const auto hook = hook::prepare(host);
    const auto vst3 = vst3::prepare(host.supported);
    effects::Chain chain;
    chain.setBypassed(true);

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
        {"audio_adapter", "shape_only_p0"},
        {"gp_hook", hook.reason},
        {"qt_ui", ui::state()},
        {"state_manager", "status_only_p0"},
        {"reason", host.supported ? "P0 bootstrap complete; processing remains bypassed" : "Host files do not match the P0 lock"}
    };
}

}
