#include "qt_ui.h"
#include "state_manager.h"

#include <QtCore/QTemporaryDir>
#include <QtCore/QDir>
#include <QtCore/QPointer>
#include <QtGui/QMouseEvent>
#include <QtWidgets/QApplication>
#include <QtWidgets/QAction>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QToolButton>
#include <iostream>

namespace {
using namespace gpvst3;
using Entries = std::vector<ui::Vst3SelectionEntry>;
Entries inputSelection, globalSelection;
Entries pendingInputSelection;
state::InputMonitorSettings settings;
QString runtimeState = QStringLiteral("inactive"), editorScope;
int inputRequests = 0, trackRequests = 0, closeCalls = 0, monitorRequests = 0;
bool rejectMonitor = false, rejectSelection = false, workerBusy = false, snapshotBusy = false;
bool driverKnown = true;
bool activeSnapshotBusy = false, nativeKnown = false, nativeEnabled = false;
int nativeInputUpdates = 0, inputEditorCalls = 0;
bool reentrantEditorClose = false, checkboxSetterActive = false, checkboxDestroyedInSetter = false;

bool selectionBusy() noexcept { return workerBusy; }
bool requestInput(const Entries &entries, std::string *error) noexcept {
    if (rejectSelection) {
        if (error) *error = "runtime_vst3_chain_full";
        return false;
    }
    if (workerBusy) pendingInputSelection = entries;
    else inputSelection = entries;
    ++inputRequests;
    return true;
}
bool requestGlobal(const Entries &entries, std::string *) noexcept {
    globalSelection = entries;
    return true;
}
bool requestTrack(const std::string &, const Entries &, std::string *) noexcept {
    ++trackRequests;
    return true;
}
Entries captureInput() { return inputSelection; }
bool activeInput(Entries &entries) noexcept {
    if (activeSnapshotBusy) return false;
    entries = inputSelection;
    return true;
}
void nativeInput(bool known, bool enabled) noexcept {
    nativeKnown = known;
    nativeEnabled = enabled;
    ++nativeInputUpdates;
}
bool editorInput(const ui::Vst3SelectionEntry &, void *) noexcept {
    editorScope = QStringLiteral("input");
    ++inputEditorCalls;
    return true;
}
bool editorGlobal(const ui::Vst3SelectionEntry &, void *) noexcept {
    editorScope = QStringLiteral("global");
    return true;
}
bool editorTrack(const std::string &, const ui::Vst3SelectionEntry &, void *) noexcept {
    editorScope = QStringLiteral("track");
    return true;
}
void closeEditors() noexcept {
    ++closeCalls;
    if (!reentrantEditorClose) return;
    reentrantEditorClose = false;
    // Real plug-in removed()/getState() callbacks may run a nested Qt event
    // loop while the selection-completion notification rebuilds the rows.
    ui::reloadVst3Selections();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QApplication::processEvents();
}
bool requestMonitor(const state::InputMonitorSettings &value, std::string *error) noexcept {
    ++monitorRequests;
    if (rejectMonitor) {
        if (error) *error = "mock_rejection";
        return false;
    }
    settings = value;
    runtimeState = value.mode == state::InputMonitorMode::LowLatencyOverlay
        ? QStringLiteral("preparing") : QStringLiteral("inactive");
    QJsonObject chain;
    return state::loadChain(chain) && state::setInputMonitorSettings(chain, value) && state::writeChain(chain);
}
QJsonObject snapshot() {
    if (snapshotBusy) return {{"state", "preparing"}};
    const char *mode = settings.mode == state::InputMonitorMode::LowLatencyOverlay ? "low_latency_overlay"
        : settings.mode == state::InputMonitorMode::Legacy ? "legacy" : "off";
    QJsonObject result{{"mode", mode}, {"gain", settings.gain}, {"state", runtimeState},
        {"native_listener_known", nativeKnown}, {"native_listener_enabled", nativeEnabled},
        {"detail", "mock_detail"}, {"sample_rate", 192000}, {"buffer_frames", 64},
        {"configuration_validated", runtimeState == "active" || runtimeState == "muted"},
        {"input_channels", 1}, {"input_native_effect_bypass", runtimeState == "active" || runtimeState == "muted"},
        {"clipped_blocks", "3"},
        {"plugin_latency_samples", "35"}, {"error_blocks", "2"}, {"configuration_rejected_blocks", "7"}};
    if (driverKnown) result.insert("driver_buffer_frames", 128);
    return result;
}
bool check(bool value, const char *message) {
    if (!value) std::cerr << "FAIL: " << message << '\n';
    return value;
}
#define CHECK(condition, message) do { if (!check(condition, message)) return 1; } while (false)
void pumpEvents() {
    QApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QApplication::processEvents();
}
void doubleClick(QWidget *widget) {
    if (!widget) return;
    QMouseEvent event(QEvent::MouseButtonDblClick, QPointF(2, 2), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(widget, &event);
    pumpEvents();
}
QWidget *nativeEditorWindow() {
    for (auto *widget : QApplication::topLevelWidgets())
        if (widget->objectName() == QStringLiteral("gpvst3NativeEditorWindow")) return widget;
    return nullptr;
}
QWidget *soundRack(QMainWindow &window) {
    auto *rack = new QWidget(&window);
    rack->setObjectName(QStringLiteral("soundRack"));
    auto *layout = new QVBoxLayout(rack);
    for (const auto *name : {"soundsContainer", "gpNativeInstrumentEffects", "gpMasterPostProcessing"}) {
        auto *anchor = new QWidget(rack);
        anchor->setObjectName(name);
        new QVBoxLayout(anchor);
        layout->addWidget(anchor);
    }
    window.setCentralWidget(rack);
    return rack;
}
QJsonObject savedEffect(state::ScopeKind scope, const QString &id) {
    QJsonObject chain;
    if (!state::loadChain(chain)) return {};
    for (const auto &value : state::scopeEffects(chain, scope))
        if (value.toObject().value("class_id").toString() == id) return value.toObject();
    return {};
}
}

int main(int argc, char **argv) {
    using namespace gpvst3;
    if (qEnvironmentVariable("GPVST3_UI_RENDER_PLATFORM") == "windows")
        qputenv("QT_QPA_PLATFORM", "windows");
    else qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication app(argc, argv);
    QTemporaryDir temporary;
    CHECK(temporary.isValid(), "isolated state directory");
    qputenv("GPVST3_DATA_DIR", temporary.path().toUtf8());
    qputenv("GPVST3_SCORE_PATH", "C:/p13-ui.gp");
    qputenv("GPVST3_TRACK", "1");
    QJsonArray catalog;
    for (const auto *id : {"A", "B"})
        catalog.append(QJsonObject{{"module", QString("C:/%1.vst3").arg(id)}, {"class_id", id},
            {"name", QString(id) == "A" ? "Archetype Mateus Asato" : "Archetype Nolly X"},
            {"vendor", "Test"}, {"identified", true}, {"compatible", true}});
    auto effect = catalog[0].toObject();
    effect.insert("enabled", true);
    effect.insert("component_state", "SQ==");
    auto disabled = catalog[1].toObject();
    disabled.insert("enabled", false);
    disabled.insert("component_state", "Qg==");
    QJsonObject chain;
    state::setScopeEffects(chain, state::ScopeKind::Input, {effect, disabled});
    effect.insert("component_state", "Rw==");
    state::setScopeEffects(chain, state::ScopeKind::Global, {effect});
    state::setScopeEffects(chain, state::ScopeKind::Track, {effect}, "C:/p13-ui.gp", "C:/p13-ui.gp#track-1", 1);
    CHECK(state::writeChain(chain), "seed three independent scopes");

    // These fake controls verify Qt routing and persistence. They do not load
    // VST3 code or establish ASIO/native-monitor correctness.
    ui::setVst3BusyControl(selectionBusy);
    ui::setVst3SelectionControl(requestGlobal);
    ui::setVst3TrackSelectionControl(requestTrack);
    ui::setVst3EditorControl(editorGlobal, closeEditors);
    ui::setVst3TrackControls(nullptr, editorTrack);
    inputSelection = {{"C:/A.vst3", "A", {'I'}, {}}};
    ui::setVst3InputControls(requestInput, captureInput, editorInput, requestMonitor, snapshot, activeInput, nativeInput);
    CHECK(!nativeKnown && !nativeEnabled, "missing native Line-In action defaults to unknown/off");
    ui::setVst3Catalog(catalog);
    QMainWindow window;
    // Deliberately hostile host colors reproduce the detached panel bug.
    window.setStyleSheet(QStringLiteral("QWidget { background:#000000; color:#ffffff; } QPushButton { color:#444a64; }"));
    auto *nativeAction = new QAction;
    nativeAction->setObjectName(QStringLiteral("actionActivatedLineIn"));
    nativeAction->setCheckable(true);
    window.addAction(nativeAction);
    auto *rack = soundRack(window);
    window.show();
    ui::showEffectChainPanel();
    pumpEvents();
    CHECK(nativeKnown && !nativeEnabled && !nativeAction->isChecked(),
          "parentless Line-In registered in widget actions is observed Off without changing host state");
    auto *input = qApp->property("gpvst3InputPanel").value<QWidget *>();
    CHECK(input && input->property("gpvst3Scope") == "input" && input->parentWidget() == &window,
          "input window belongs to the main window, independently of the score sidebar");
    CHECK(inputRequests == 0 && monitorRequests == 0, "opening the UI does not activate input runtime");
    auto *button = rack->findChild<QPushButton *>("gpvst3InputEffectChainButton");
    CHECK(button, "input entry exists");
    button->click();
    pumpEvents();
    CHECK(input->isVisible(), "input entry opens the independent panel");
    auto *list = input->findChild<QListWidget *>("gpvst3InputChainList");
    auto *monitor = input->findChild<QCheckBox *>("gpvst3InputLowLatencyEnabled");
    auto *gain = input->findChild<QDoubleSpinBox *>("gpvst3InputGain");
    auto *status = input->findChild<QLabel *>("gpvst3InputMonitorStatus");
    auto *details = input->findChild<QLabel *>("gpvst3InputMonitorDetails");
    auto *format = input->findChild<QLabel *>("gpvst3InputMonitorFormat");
    CHECK(list && list->count() == 1 && monitor && gain && status, "saved input chain and monitor controls");
    CHECK(gain->minimum() == 0.0 && gain->maximum() == 4.0 && gain->value() == 0.5, "persisted gain range and default");
    CHECK(details && format && details->isHidden() && input->testAttribute(Qt::WA_StyledBackground),
          "detached input panel paints its own background and collapses diagnostics");
    CHECK(input->findChild<QPushButton *>("gpvst3InputName_A")->palette().color(QPalette::ButtonText).lightness() > 180,
          "input plug-in text overrides dark host button text");

    // The UI has saved S1. A later editor edit and gain request leave the
    // runtime at S2 while the list still has S1. Adding B must retain S2.
    inputSelection = {{"C:/A.vst3", "A", {'0'}, {}}};
    doubleClick(input->findChild<QPushButton *>("gpvst3InputName_A"));
    auto *editorWindow = nativeEditorWindow();
    CHECK(editorWindow && editorScope == "input", "input editor opens the input controller");
    inputSelection[0].componentState = {'1'};
    editorWindow->close();
    CHECK(savedEffect(state::ScopeKind::Input, "A").value("component_state") == "MQ==", "UI saved S1");
    doubleClick(input->findChild<QPushButton *>("gpvst3InputName_A"));
    inputSelection[0].componentState = {'2'};
    gain->setValue(0.75);
    const int trackRequestsBeforeInput = trackRequests;
    auto *inputB = input->findChild<QCheckBox *>("gpvst3InputEnabled_B");
    CHECK(inputB, "available input B row");
    inputB->setChecked(true);
    pumpEvents();
    CHECK(inputSelection.size() == 2 && inputSelection[0].componentState == std::vector<unsigned char>{'2'} &&
          inputSelection[1].componentState == std::vector<unsigned char>{'B'},
          "existing A keeps live S2 while newly enabled B keeps saved state");
    CHECK(savedEffect(state::ScopeKind::Input, "A").value("component_state") == "Mg==", "fresh S2 is persisted");
    CHECK(inputRequests == 1 && globalSelection.empty() && trackRequests == trackRequestsBeforeInput,
          "input edits never call global or track requests");
    CHECK(savedEffect(state::ScopeKind::Global, "A").value("component_state") == "Rw==",
          "input state capture preserves global opaque state");
    rack->findChild<QCheckBox *>("gpvst3GlobalEnabled_A")->setChecked(false);
    pumpEvents();
    CHECK(inputRequests == 1, "global toggle never republishes input");

    rejectSelection = true;
    input->findChild<QCheckBox *>("gpvst3InputEnabled_B")->setChecked(false);
    pumpEvents();
    CHECK(input->findChild<QCheckBox *>("gpvst3InputEnabled_B")->isChecked() && inputSelection.size() == 2 &&
          savedEffect(state::ScopeKind::Input, "B").value("enabled").toBool(),
          "rejected input selection restores the checkbox and preserves saved intent");
    CHECK(list->model()->moveRow({}, 1, {}, 0), "input supports order edits");
    pumpEvents();
    CHECK(list->itemWidget(list->item(0))->property("gpvst3EntryId").toString().endsWith("\nA") &&
          inputSelection[0].classId == "A", "rejected order restores visible and active input order");
    CHECK(state::loadChain(chain) && state::scopeEffects(chain, state::ScopeKind::Input)[0].toObject().value("class_id") == "A",
          "rejected order preserves persisted input order");
    rejectSelection = false;
    CHECK(list->model()->moveRow({}, 1, {}, 0), "retry input order edit");
    pumpEvents();
    CHECK(inputSelection[0].classId == "B" && state::loadChain(chain) &&
          state::scopeEffects(chain, state::ScopeKind::Input)[0].toObject().value("class_id") == "B",
          "accepted input order reaches runtime and persistence");
    monitor->setChecked(true);
    CHECK(monitor->isChecked() && !nativeAction->isChecked() && status->text().contains(QStringLiteral("LINE-IN 已关闭")),
          "low-latency preference does not enable native input");
    nativeAction->setChecked(true);
    CHECK(settings.mode == state::InputMonitorMode::LowLatencyOverlay && status->text().contains(QStringLiteral("准备")),
          "requested mode and preparing state remain distinct");
    ui::reloadVst3Selections();
    CHECK(!input->findChild<QLabel *>("gpvst3InputStatus")->text().contains(QStringLiteral("已生效")),
          "saved input selection alone is not described as active monitoring");
    gain->setValue(4.0);
    CHECK(settings.gain == 4.0, "maximum gain reaches the monitor request");
    rejectMonitor = true;
    gain->setValue(1.0);
    CHECK(gain->value() == 4.0 && settings.gain == 4.0, "rejected gain restores the control");
    monitor->setChecked(false);
    CHECK(monitor->isChecked() && settings.mode == state::InputMonitorMode::LowLatencyOverlay,
          "rejected mode restores the checkbox");
    rejectMonitor = false;
    snapshotBusy = true;
    ui::syncVst3Selection();
    CHECK(monitor->isChecked() && gain->value() == 4.0, "busy snapshot does not reset accepted settings");
    snapshotBusy = false;
    runtimeState = QStringLiteral("muted");
    ui::syncVst3Selection();
    CHECK(details->text().contains("192000") && details->text().contains(QStringLiteral("回调：64 帧")) &&
          details->text().contains(QStringLiteral("驱动缓冲：128 帧")) &&
          details->text().contains(QStringLiteral("插件报告延迟：35 样本")) &&
          details->text().contains(QStringLiteral("输入通道：1")) &&
          details->text().contains(QStringLiteral("原生输入监听：已旁通")) &&
          details->text().contains(QStringLiteral("混音削波：3 块，请降低输入增益")) &&
          status->property("gpvst3RuntimeState") == "muted", "callback, driver and reported plugin latency are distinct");
    driverKnown = false;
    ui::syncVst3Selection();
    CHECK(details->text().contains(QStringLiteral("驱动缓冲：未知")), "missing driver evidence remains unknown");
    runtimeState = QStringLiteral("host_limited");
    driverKnown = true;
    ui::syncVst3Selection();
    CHECK(!details->text().contains("192000") && !details->text().contains(QStringLiteral("回调：64 帧")) &&
          !details->text().contains(QStringLiteral("输入通道：1")) &&
          details->text().contains(QStringLiteral("原生输入监听：未旁通")) &&
          details->text().contains(QStringLiteral("驱动缓冲：未知")) &&
          details->text().contains(QStringLiteral("输入处理错误：2 次")) &&
          details->text().contains(QStringLiteral("配置未验证跳过：7 块")),
          "unvalidated stream hides historical format and separates configuration rejection from DSP errors");
    settings.mode = state::InputMonitorMode::Legacy;
    runtimeState = QStringLiteral("legacy");
    ui::syncVst3Selection();
    gain->setValue(0.6);
    CHECK(!monitor->isChecked() && settings.mode == state::InputMonitorMode::Legacy,
          "gain edit preserves legacy mode rather than switching it off");

    doubleClick(input->findChild<QPushButton *>("gpvst3InputName_A"));
    const int closes = closeCalls, requests = inputRequests;
    qputenv("GPVST3_TRACK", "2");
    ui::refreshVst3TrackContext();
    pumpEvents();
    CHECK(closeCalls == closes && inputRequests == requests &&
          qApp->property("gpvst3InputPanel").value<QWidget *>() == input,
          "track switch neither closes input editor nor rebuilds input");
    CHECK(editorWindow->isVisible(), "input editor remains visible");
    ui::closeInputEditorForRuntimeChange();
    CHECK(!editorWindow->isVisible() && closeCalls == closes + 1, "retirement closes only the old input editor");
    const int closedInput = closeCalls;
    ui::closeInputEditorForRuntimeChange();
    CHECK(closeCalls == closedInput, "input editor retirement is idempotent");
    doubleClick(input->findChild<QPushButton *>("gpvst3InputName_A"));
    workerBusy = true;
    doubleClick(input->findChild<QPushButton *>("gpvst3InputName_A"));
    ui::closeInputEditorForRuntimeChange();
    CHECK(!editorWindow->isVisible(), "old controller is closed while replacement is pending");
    workerBusy = false;
    ui::syncVst3Selection();
    pumpEvents();
    CHECK(editorWindow->isVisible() && editorScope == "input", "queued editor open survives old controller retirement");

    rack->findChild<QCheckBox *>("gpvst3GlobalEnabled_A")->setChecked(true);
    pumpEvents();
    doubleClick(rack->findChild<QPushButton *>("gpvst3GlobalName_A"));
    CHECK(editorScope == "global", "global editor opened for isolation check");
    const int globalCloses = closeCalls;
    ui::closeInputEditorForRuntimeChange();
    CHECK(editorWindow->isVisible() && closeCalls == globalCloses, "input retirement preserves global editor");
    rack->findChild<QCheckBox *>("gpvst3Enabled_A")->setChecked(true);
    pumpEvents();
    doubleClick(rack->findChild<QPushButton *>("gpvst3Name_A"));
    CHECK(editorScope == "track", "track editor opened for isolation check");
    const int trackCloses = closeCalls;
    ui::closeInputEditorForRuntimeChange();
    CHECK(editorWindow->isVisible() && closeCalls == trackCloses, "input retirement preserves track editor");

    delete window.takeCentralWidget();
    rack = soundRack(window);
    ui::showEffectChainPanel();
    ui::showEffectChainPanel();
    pumpEvents();
    CHECK(qApp->property("gpvst3InputPanel").value<QWidget *>() == input && inputRequests == requests && input->isVisible(),
          "sidebar recreation preserves input window and runtime intent");
    CHECK(rack->findChildren<QPushButton *>("gpvst3InputEffectChainButton").size() == 1,
          "input entry is reattached exactly once");

    // A native action replacement temporarily closes the gate. Rebinding
    // reads the replacement's actual state and never toggles it for the user.
    const int monitorRequestsBeforeRebind = monitorRequests;
    delete nativeAction;
    CHECK(!nativeKnown && !nativeEnabled, "destroyed native action closes input immediately");
    auto *nativeService = new QObject(qApp);
    nativeService->setObjectName(QStringLiteral("nativeInputService"));
    nativeAction = new QAction(nativeService);
    nativeAction->setObjectName(QStringLiteral("actionActivatedLineIn"));
    nativeAction->setCheckable(true);
    pumpEvents();
    CHECK(nativeKnown && !nativeEnabled && monitorRequests == monitorRequestsBeforeRebind,
          "native action replacement is observed independently of saved monitoring preference");
    CHECK(!window.findChild<QAction *>(QStringLiteral("actionActivatedLineIn")),
          "service-owned native action is outside the main-window subtree");
    nativeAction->setChecked(true);
    CHECK(nativeKnown && nativeEnabled, "replacement native action remains connected");

    input->findChild<QCheckBox *>("gpvst3InputEnabled_B")->setChecked(false);
    pumpEvents();
    ui::syncVst3Selection();
    workerBusy = true;
    input->findChild<QCheckBox *>("gpvst3InputEnabled_B")->click();
    pumpEvents();
    CHECK(input->findChild<QCheckBox *>("gpvst3InputEnabled_B")->checkState() == Qt::PartiallyChecked,
          "input enable waits for the same acknowledged runtime state as track enable");
    auto *loading = input->findChild<QWidget *>("gpvst3InputEffectRow_B")->findChild<QWidget *>("gpvst3RowLoading");
    CHECK(loading && !loading->isHidden(), "pending input row visibly loads");
    const int editorCallsBeforePending = inputEditorCalls;
    doubleClick(input->findChild<QPushButton *>("gpvst3InputName_B"));
    CHECK(inputEditorCalls == editorCallsBeforePending, "pending input editor waits for prepare completion");
    input->findChild<QCheckBox *>("gpvst3InputEnabled_B")->click();
    pumpEvents();
    workerBusy = false;
    inputSelection = pendingInputSelection;
    ui::syncVst3Selection();
    CHECK(input->findChild<QCheckBox *>("gpvst3InputEnabled_B")->checkState() == Qt::Unchecked &&
          inputEditorCalls == editorCallsBeforePending,
          "partial input checkbox cancels the pending enable and queued editor");
    workerBusy = true;
    input->findChild<QCheckBox *>("gpvst3InputEnabled_B")->click();
    pumpEvents();
    doubleClick(input->findChild<QPushButton *>("gpvst3InputName_B"));
    inputSelection = pendingInputSelection;
    activeSnapshotBusy = true;
    workerBusy = false;
    ui::syncVst3Selection();
    CHECK(input->findChild<QCheckBox *>("gpvst3InputEnabled_B")->checkState() == Qt::PartiallyChecked,
          "busy active input snapshot retains pending checkbox");
    activeSnapshotBusy = false;
    ui::syncVst3Selection();
    pumpEvents();
    CHECK(input->findChild<QCheckBox *>("gpvst3InputEnabled_B")->checkState() == Qt::Checked &&
          inputEditorCalls == editorCallsBeforePending + 1,
          "acknowledged input preparation completes checkbox and opens editor once");
    rejectSelection = true;
    input->findChild<QCheckBox *>("gpvst3InputEnabled_B")->click();
    pumpEvents();
    CHECK(editorWindow->isVisible(), "rejected input disable preserves the active editor");
    rejectSelection = false;
    input->findChild<QCheckBox *>("gpvst3InputEnabled_B")->click();
    pumpEvents();
    ui::syncVst3Selection();
    CHECK(!editorWindow->isVisible(), "accepted input disable closes its editor");

    workerBusy = true;
    input->findChild<QCheckBox *>("gpvst3InputEnabled_B")->click();
    pumpEvents();
    CHECK(state::loadChain(chain), "read pending input state for failure simulation");
    auto failedEffects = state::scopeEffects(chain, state::ScopeKind::Input);
    for (int index = 0; index < failedEffects.size(); ++index) {
        auto failed = failedEffects[index].toObject();
        if (failed.value("class_id") != "B") continue;
        failed.insert("enabled", false);
        failed.insert("last_error", "mock_prepare_failed");
        failedEffects[index] = failed;
    }
    state::setScopeEffects(chain, state::ScopeKind::Input, failedEffects);
    CHECK(state::writeChain(chain), "backend publishes rejected prepared input state");
    workerBusy = false;
    ui::syncVst3Selection();
    pumpEvents();
    CHECK(input->findChild<QCheckBox *>("gpvst3InputEnabled_B")->checkState() == Qt::Unchecked &&
          input->property("gpvst3SelectionState") == "failed_reverted",
          "input asynchronous failure restores actual check state and retry feedback");
    input->findChild<QCheckBox *>("gpvst3InputEnabled_B")->click();
    pumpEvents();
    ui::syncVst3Selection();
    pumpEvents();
    CHECK(input->findChild<QCheckBox *>("gpvst3InputEnabled_B")->checkState() == Qt::Checked &&
          savedEffect(state::ScopeKind::Input, "B").value("last_error").toString().isEmpty(),
          "input enable retry clears the previous failure");
    input->findChild<QCheckBox *>("gpvst3InputEnabled_B")->click();
    pumpEvents();
    ui::syncVst3Selection();
    pumpEvents();
    monitor->setChecked(true);
    runtimeState = "active";
    ui::syncVst3Selection();
    input->resize(420, 640);
    pumpEvents();
    const auto screenshotRoot = qEnvironmentVariable("GPVST3_UI_SCREENSHOT_DIR");
    if (!screenshotRoot.isEmpty()) {
        const auto imagePath = QDir(screenshotRoot).filePath(QStringLiteral("input-ui-%1.png").arg(qEnvironmentVariable("QT_SCALE_FACTOR")));
        CHECK(input->grab().save(imagePath), "input UI screenshot exported");
    }
    input->findChild<QCheckBox *>("gpvst3InputEnabled_A")->click();
    pumpEvents();
    ui::syncVst3Selection();
    CHECK(list->count() == 0 && status->text().contains(QStringLiteral("原声直通")),
          "empty input chain presents dry monitoring as a valid active state");
    nativeAction->setChecked(false);
    CHECK(monitor->isChecked() && status->text().contains(QStringLiteral("LINE-IN 已关闭")),
          "native input Off stops listening while preserving low-latency preference");
    for (int width : {260, 320, 420}) {
        input->setFixedWidth(width);
        pumpEvents();
        CHECK(gain->geometry().right() < input->width() && monitor->width() <= input->width() &&
              status->width() <= input->width(), "monitor controls fit a narrow input panel");
    }
    doubleClick(rack->findChild<QPushButton *>("gpvst3GlobalName_A"));
    CHECK(editorScope == "global" && editorWindow->isVisible(), "global editor ready for reentrant-close regression");
    auto *globalToggle = rack->findChild<QCheckBox *>("gpvst3GlobalEnabled_A");
    const QPointer<QCheckBox> watchedToggle(globalToggle);
    QObject::connect(globalToggle, &QObject::destroyed, &app, [] {
        checkboxDestroyedInSetter |= checkboxSetterActive;
    });
    reentrantEditorClose = true;
    checkboxSetterActive = true;
    globalToggle->setProperty("checked", false);
    checkboxSetterActive = false;
    CHECK(!checkboxDestroyedInSetter && watchedToggle,
          "checkbox survives Qt setChecked accessibility updates despite nested editor callbacks");
    pumpEvents();
    CHECK(!editorWindow->isVisible() && !rack->findChild<QCheckBox *>("gpvst3GlobalEnabled_A")->isChecked() &&
          !savedEffect(state::ScopeKind::Global, "A").value("enabled").toBool(),
          "reentrant global disable retains the requested state and safely closes its editor");

    input->findChild<QCheckBox *>("gpvst3InputEnabled_A")->click();
    pumpEvents();
    ui::syncVst3Selection();
    pumpEvents();
    doubleClick(input->findChild<QPushButton *>("gpvst3InputName_A"));
    CHECK(editorScope == "input" && editorWindow->isVisible(), "input editor ready for reentrant-close regression");
    auto *inputToggle = input->findChild<QCheckBox *>("gpvst3InputEnabled_A");
    const QPointer<QCheckBox> watchedInputToggle(inputToggle);
    QObject::connect(inputToggle, &QObject::destroyed, &app, [] {
        checkboxDestroyedInSetter |= checkboxSetterActive;
    });
    reentrantEditorClose = true;
    checkboxSetterActive = true;
    inputToggle->setProperty("checked", false);
    checkboxSetterActive = false;
    CHECK(!checkboxDestroyedInSetter && watchedInputToggle,
          "input checkbox survives setChecked despite reentrant editor callbacks");
    pumpEvents();
    CHECK(!editorWindow->isVisible() && inputSelection.empty() &&
          !input->findChild<QCheckBox *>("gpvst3InputEnabled_A")->isChecked() &&
          !savedEffect(state::ScopeKind::Input, "A").value("enabled").toBool(),
          "reentrant input disable closes the editor and preserves disabled intent");

    workerBusy = true;
    inputToggle = input->findChild<QCheckBox *>("gpvst3InputEnabled_A");
    const int requestsBeforeRapidCancel = inputRequests;
    inputToggle->click();
    inputToggle->click();
    pumpEvents();
    CHECK(inputRequests == requestsBeforeRapidCancel + 2 && pendingInputSelection.empty() &&
          !savedEffect(state::ScopeKind::Input, "A").value("enabled").toBool(),
          "enable followed by cancel before the next event turn retains the final disabled intent");
    workerBusy = false;
    inputSelection = pendingInputSelection;
    ui::syncVst3Selection();
    pumpEvents();
    CHECK(!input->findChild<QCheckBox *>("gpvst3InputEnabled_A")->isChecked(),
          "rapid enable/cancel completes unchecked without resurrecting saved selection");
    ui::shutdownEditors();
    std::cout << "PASS: P13 input UI scope/state isolation, request rollback, status evidence, editor retirement and sidebar lifecycle.\n";
    return 0;
}
