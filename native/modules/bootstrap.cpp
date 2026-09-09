#include "bootstrap.h"

#include "effect_chain.h"
#include "gp_hook.h"
#include "host_lock.h"
#include "qt_ui.h"
#include "vst3_host.h"

namespace gpvst3::bootstrap {

QJsonObject initialize() {
    const auto host = host::verify();
    const auto hook = hook::prepare(host);
    const auto vst3 = vst3::prepare();
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
        {"vst3_host", vst3.status},
        {"audio_adapter", "shape_only_p0"},
        {"gp_hook", hook.reason},
        {"qt_ui", ui::state()},
        {"state_manager", "status_only_p0"},
        {"reason", host.supported ? "P0 bootstrap complete; processing remains bypassed" : "Host files do not match the P0 lock"}
    };
}

}
