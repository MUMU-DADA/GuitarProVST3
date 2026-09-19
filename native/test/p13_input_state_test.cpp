#include "state_manager.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QTemporaryDir>
#include <iostream>
#include <limits>

using namespace gpvst3::state;

namespace {
bool check(bool value, const char *message) {
    if (!value) std::cerr << "FAIL: " << message << '\n';
    return value;
}

QJsonObject effect(const char *module, const char *id, const char *state) {
    return {{"module", module}, {"class_id", id}, {"enabled", true},
            {"component_state", state}, {"parameters", QJsonObject{{"gain", 0.25}}}};
}

bool saveRaw(const QJsonObject &chain) {
    QFile file(sidecarPath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const auto bytes = QJsonDocument(chain).toJson();
    return file.write(bytes) == bytes.size();
}

bool settingsEqual(const InputMonitorSettings &a, InputMonitorMode mode, double gain) {
    return a.mode == mode && a.gain == gain;
}
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    if (!check(directory.isValid(), "temporary directory")) return 1;
    qputenv("GPVST3_DATA_DIR", directory.path().toUtf8());

    QJsonObject chain;
    if (!check(loadChain(chain) && !chain.contains("input"), "input is optional in existing schema 2")) return 1;
    const auto initial = chain;
    InputMonitorSettings settings{InputMonitorMode::Legacy, 4.0};
    QString error = "stale";
    if (!check(readInputMonitorSettings(chain, settings, &error) &&
               settingsEqual(settings, InputMonitorMode::Off, 0.5) && error.isEmpty() && chain == initial,
               "missing settings safely default without modifying JSON")) return 1;
    if (!check(scopeEffects(chain, ScopeKind::Input).isEmpty(), "new input does not inherit effects")) return 1;

    const auto sharedGlobal = effect("C:/VST3/shared.vst3", "SAME", "global-state");
    const auto sharedTrack = effect("C:/VST3/shared.vst3", "SAME", "track-state");
    auto sharedInput = effect("C:/VST3/shared.vst3", "SAME", "input-state");
    sharedInput.insert("parameters", QJsonObject{{"gain", 0.875}});
    setScopeEffects(chain, ScopeKind::Global, {sharedGlobal});
    setScopeEffects(chain, ScopeKind::Track, {sharedTrack}, "score-a", "track-a", 0);
    const auto global = chain.value("global"), scores = chain.value("scores"), legacy = chain.value("effects");
    chain.insert("input", QJsonObject{{"future", QJsonObject{{"opaque", "preserve"}}}, {"device_hint", "studio"}});
    if (!check(setInputMonitorSettings(chain, {InputMonitorMode::LowLatencyOverlay, 0.75}), "set requested input settings")) return 1;
    setScopeEffects(chain, ScopeKind::Input, {effect("C:/VST3/first.vst3", "FIRST", "first-state"), sharedInput},
                    "ignored-score", "ignored-track", 77, "ignored name");
    const auto input = chain.value("input").toObject();
    const auto inputEffects = scopeEffects(chain, ScopeKind::Input);
    if (!check(chain.value("global") == global && chain.value("scores") == scores && chain.value("effects") == legacy,
               "input writes do not modify global, track or legacy views")) return 1;
    if (!check(input.value("future").toObject().value("opaque") == "preserve" && input.value("device_hint") == "studio" &&
               input.value("monitor_mode") == "low_latency_overlay" && input.value("input_gain").toDouble() == 0.75,
               "input effects preserve metadata and monitor settings")) return 1;
    if (!check(inputEffects.size() == 2 && inputEffects.at(1).toObject().value("component_state") == "input-state" &&
               inputEffects.at(1).toObject().value("parameters").toObject().value("gain").toDouble() == 0.875 &&
               inputEffects.at(0).toObject().value("order").toInt() == 0 && inputEffects.at(1).toObject().value("order").toInt() == 1,
               "same plugin state, parameter gain and order are independent")) return 1;
    if (!check(!input.contains("active") && !input.contains("generation"), "runtime state is not persisted")) return 1;
    if (!check(writeChain(chain) && loadChain(chain) && chain.value("input") == input,
               "independent input configuration survives disk compaction and reload")) return 1;
    auto migratedAliases = chain;
    const auto savedScopes = migratedAliases.value("scores");
    const auto savedGlobal = migratedAliases.value("global");
    migratedAliases.insert("effects", QJsonArray{QJsonObject{{"plugin_path", "old.vst3"},
        {"class_uid", "OLD"}, {"bypass", false}}});
    if (!check(setInputMonitorSettings(migratedAliases, {InputMonitorMode::LowLatencyOverlay, 0.75}) &&
               writeChain(migratedAliases) && loadChain(migratedAliases) &&
               migratedAliases.value("scores") == savedScopes && migratedAliases.value("global") == savedGlobal &&
               migratedAliases.value("input") == input,
               "legacy aliases cannot erase schema-2 track state when saving input settings")) return 1;

    const InputMonitorMode modes[]{InputMonitorMode::Off, InputMonitorMode::Legacy, InputMonitorMode::LowLatencyOverlay};
    for (const auto mode : modes) {
        for (const double gain : {0.0, 0.5, 4.0}) {
            auto changed = chain;
            if (!check(setInputMonitorSettings(changed, {mode, gain}, &error) && error.isEmpty() &&
                       readInputMonitorSettings(changed, settings, &error) && settingsEqual(settings, mode, gain) &&
                       scopeEffects(changed, ScopeKind::Input) == inputEffects && changed.value("global") == global &&
                       changed.value("scores") == scores && changed.value("effects") == legacy,
                       "all monitor modes and inclusive gain endpoints round trip independently")) return 1;
        }
    }
    for (const double gain : {-0.0001, 4.0001, std::numeric_limits<double>::infinity(),
                              -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        auto invalid = chain;
        const auto before = QJsonDocument(invalid).toJson(QJsonDocument::Compact);
        if (!check(!setInputMonitorSettings(invalid, {InputMonitorMode::Legacy, gain}, &error) && !error.isEmpty() &&
                   QJsonDocument(invalid).toJson(QJsonDocument::Compact) == before,
                   "invalid requested gain is rejected without partial JSON mutation")) return 1;
    }
    auto invalid = chain;
    if (!check(!setInputMonitorSettings(invalid, {static_cast<InputMonitorMode>(-1), 0.5}, &error) && invalid == chain,
               "unknown requested mode is rejected atomically")) return 1;
    for (const auto &bad : QJsonArray{QJsonValue::Null, true, 17, "overlay", "OFF"}) {
        invalid = chain;
        auto invalidInput = input;
        invalidInput.insert("monitor_mode", bad);
        invalid.insert("input", invalidInput);
        const auto before = invalid;
        settings = {InputMonitorMode::Legacy, 4.0};
        if (!check(!readInputMonitorSettings(invalid, settings, &error) && !error.isEmpty() &&
                   settingsEqual(settings, InputMonitorMode::Off, 0.5) && invalid == before,
                   "invalid stored mode yields complete safe defaults and explicit error")) return 1;
    }
    for (const auto &bad : QJsonArray{QJsonValue::Null, true, "0.5", -0.1, 4.1}) {
        invalid = chain;
        auto invalidInput = input;
        invalidInput.insert("input_gain", bad);
        invalid.insert("input", invalidInput);
        settings = {InputMonitorMode::Legacy, 4.0};
        if (!check(!readInputMonitorSettings(invalid, settings, &error) &&
                   settingsEqual(settings, InputMonitorMode::Off, 0.5), "invalid stored gain fails closed")) return 1;
    }
    invalid.insert("input", false);
    const auto malformed = invalid;
    if (!check(!readInputMonitorSettings(invalid, settings) && settingsEqual(settings, InputMonitorMode::Off, 0.5) &&
               !setInputMonitorSettings(invalid, {}) && invalid == malformed,
               "malformed input scope is rejected without destruction")) return 1;
    QJsonObject partial{{"input", QJsonObject{{"monitor_mode", "legacy"}}}};
    if (!check(readInputMonitorSettings(partial, settings) && settingsEqual(settings, InputMonitorMode::Legacy, 0.5),
               "missing gain uses default")) return 1;
    partial.insert("input", QJsonObject{{"input_gain", 2.0}});
    if (!check(readInputMonitorSettings(partial, settings) && settingsEqual(settings, InputMonitorMode::Off, 2.0),
               "missing mode uses off")) return 1;

    // Context, document lifecycle, Save As and new scores cannot own input.
    std::vector<HostTrackIdentity> bindings{{"document", "score-a", "native-a", 0}};
    if (!check(reconcileTrackIdentities(bindings), "bind first score")) return 1;
    setRuntimeTrackContext("score-a", bindings.front().runtimeKey, 0, "native-a");
    if (!check(loadChain(chain) && chain.value("input") == input, "track selection preserves input")) return 1;
    bindings.front().scoreKey = "C:/scores/save-as.gp";
    if (!check(reconcileTrackIdentities(bindings), "save as")) return 1;
    bindings.front().scoreKey = "d51d43c4-caac-44ba-8fae-8006dc84ed71";
    if (!check(reconcileTrackIdentities(bindings), "new score")) return 1;
    clearRuntimeTrackContext(); resetTrackIdentities();
    if (!check(loadChain(chain) && chain.value("input") == input && scopeEffects(chain, ScopeKind::Input) == inputEffects,
               "score switch, close and identity reset retain application input")) return 1;

    // Compaction removes catalog noise only within its own scope, including
    // duplicate identities. Empty input effects retain monitor configuration.
    auto noisyInput = inputEffects;
    noisyInput.append(QJsonObject{{"module", "unused.vst3"}, {"class_id", "UNUSED"}, {"enabled", false}});
    auto duplicate = sharedInput;
    duplicate.insert("module", "c:/vst3/SHARED.vst3");
    duplicate.insert("future_effect", "keep");
    noisyInput.append(duplicate);
    setScopeEffects(chain, ScopeKind::Input, noisyInput);
    if (!check(scopeEffects(chain, ScopeKind::Input).size() == 2 &&
               scopeEffects(chain, ScopeKind::Input).at(1).toObject().value("future_effect") == "keep",
               "input compaction keeps configured state and merges duplicates locally")) return 1;
    setScopeEffects(chain, ScopeKind::Input, {});
    if (!check(scopeEffects(chain, ScopeKind::Input).isEmpty() && readInputMonitorSettings(chain, settings) &&
               settingsEqual(settings, InputMonitorMode::LowLatencyOverlay, 0.75) &&
               chain.value("input").toObject().value("future") == input.value("future"),
               "empty input chain retains settings and unknown fields")) return 1;

    QJsonObject old{{"schema", 1}, {"score_id", "legacy-score"}, {"track", 2}, {"effects", QJsonArray{sharedGlobal}}};
    if (!check(saveRaw(old) && loadChain(chain) && !chain.contains("input") && scopeEffects(chain, ScopeKind::Input).isEmpty(),
               "legacy migration never copies global effects into input")) return 1;
    old.insert("input", input);
    if (!check(saveRaw(old) && loadChain(chain) && chain.value("input") == input && writeChain(chain) && loadChain(chain) &&
               chain.value("input") == input, "legacy migration preserves independent input and metadata")) return 1;
    // Force the read-time repair path independently of writeChain's compaction.
    chain.insert("future_padding", QString(1024 * 1024 + 1, QLatin1Char('x')));
    auto expanded = input;
    expanded.insert("effects", noisyInput);
    chain.insert("input", expanded);
    if (!check(saveRaw(chain) && loadChain(chain) && scopeEffects(chain, ScopeKind::Input).size() == 2 &&
               chain.value("input").toObject().value("future") == input.value("future"),
               "large-sidecar repair independently compacts input without deleting metadata")) return 1;
    chain.remove("future_padding");
    chain.insert("input", input);
    if (!check(writeChain(chain) && disableAllEffectsAtStartup() && loadChain(chain), "startup processes input effects")) return 1;
    for (const auto &entry : scopeEffects(chain, ScopeKind::Input)) {
        const auto object = entry.toObject();
        if (!check(!object.value("enabled").toBool() && object.value("bypass").toBool() &&
                   object.value("desired_enabled").toBool() && !object.value("component_state").toString().isEmpty(),
                   "startup bypass preserves independent plugin state and requested activation")) return 1;
    }
    if (!check(readInputMonitorSettings(chain, settings) && settingsEqual(settings, InputMonitorMode::LowLatencyOverlay, 0.75) &&
               migrateDesiredEnabledIntent() && loadChain(chain), "startup never converts request into active state")) return 1;
    for (const auto &entry : scopeEffects(chain, ScopeKind::Input)) {
        const auto object = entry.toObject();
        if (!check(object.value("enabled").toBool() && !object.value("bypass").toBool() && !object.contains("desired_enabled"),
                   "input activation-intent migration matches independent scope semantics")) return 1;
    }
    std::cout << "PASS: P13 independent input state, settings validation, migration, lifecycle and compaction.\n";
    return 0;
}
