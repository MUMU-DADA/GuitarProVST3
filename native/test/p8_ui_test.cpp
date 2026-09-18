#include "qt_ui.h"
#include "state_manager.h"
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QFrame>
#include <QtWidgets/QLabel>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>
#include <QtGui/QMouseEvent>
#include <iostream>
#include <memory>
#include <unordered_map>

namespace {
void pumpEvents() {
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();
}
void doubleClick(QWidget *widget) {
    QMouseEvent event(QEvent::MouseButtonDblClick, QPointF(2, 2), Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(widget, &event);
    pumpEvents();
}
bool check(bool value, const char *message) {
    if (!value) std::cerr << "FAIL: " << message << '\n';
    return value;
}
std::vector<gpvst3::ui::Vst3SelectionEntry> globalSelection, trackSelection;
std::string selectedTrack, editorScope;
using Entries = std::vector<gpvst3::ui::Vst3SelectionEntry>;
std::unordered_map<std::string, Entries> activeTracks, queuedTracks;
bool workerBusy = false, snapshotBusy = false, rejectRequest = false;
std::uint64_t requestedGeneration = 0;
int generationRequests = 0, stateCaptures = 0;
bool busyControl() noexcept { return workerBusy; }
bool activeTrackControl(const std::string &track, Entries &entries) noexcept {
    if (snapshotBusy) return false;
    entries = activeTracks[track];
    return true;
}
Entries captureTrackControl(const std::string &track) {
    ++stateCaptures;
    return activeTracks[track];
}
bool generationTrackControl(const std::string &track, std::uint64_t generation,
                            const Entries &entries, std::string *error) noexcept {
    ++generationRequests;
    requestedGeneration = generation;
    if (rejectRequest || generation != gpvst3::state::runtimeSelectionGeneration()) {
        if (error) *error = rejectRequest ? "runtime_vst3_selection_prepare_failed" : "stale_selection_generation";
        return false;
    }
    selectedTrack = track;
    trackSelection = entries;
    queuedTracks[track] = entries;
    workerBusy = true;
    if (entries.empty()) activeTracks[track].clear();
    return true;
}
void finishTrackRequest(const std::string &track, bool success = true) {
    if (success) activeTracks[track] = queuedTracks[track];
    else {
        QJsonObject chain;
        gpvst3::state::loadChain(chain);
        const auto score = gpvst3::state::currentScoreKey();
        auto effects = gpvst3::state::scopeEffects(chain, gpvst3::state::ScopeKind::Track,
                                                  score, QString::fromStdString(track));
        for (int i = 0; i < effects.size(); ++i) {
            auto effect = effects[i].toObject();
            for (const auto &requested : queuedTracks[track]) {
                if (effect.value("class_id").toString().toStdString() != requested.classId) continue;
                effect.insert("enabled", false);
                effect.insert("bypass", true);
                effect.insert("last_error", "runtime_vst3_selection_prepare_failed");
            }
            effects[i] = effect;
        }
        gpvst3::state::setScopeEffects(chain, gpvst3::state::ScopeKind::Track, effects,
            score, QString::fromStdString(track), gpvst3::state::runtimeTrackIndex());
        gpvst3::state::writeChain(chain);
    }
    queuedTracks.erase(track);
    workerBusy = false;
    gpvst3::ui::reloadVst3Selections();
    pumpEvents();
    gpvst3::ui::syncVst3Selection();
    pumpEvents();
}
bool globalControl(const std::vector<gpvst3::ui::Vst3SelectionEntry> &entries, std::string *) noexcept {
    globalSelection = entries; return true;
}
bool trackControl(const std::string &track, const std::vector<gpvst3::ui::Vst3SelectionEntry> &entries,
                  std::string *) noexcept {
    selectedTrack = track; trackSelection = entries; return true;
}
bool globalEditor(const gpvst3::ui::Vst3SelectionEntry &, void *) noexcept {
    editorScope = "global"; return true;
}
bool trackEditor(const std::string &track, const gpvst3::ui::Vst3SelectionEntry &, void *) noexcept {
    editorScope = track; return true;
}
std::unique_ptr<QWidget> host() {
    auto result = std::make_unique<QWidget>();
    // Mirror the real GP hierarchy: soundsContainer is a host-owned native
    // soft-source slot inside SoundRack.  The VST3 entry must be mounted on
    // the rack so adding it cannot replace the slot's native child.
    result->setObjectName("soundRack");
    auto *layout = new QVBoxLayout(result.get());
    auto *nativeContainer = new QWidget(result.get());
    nativeContainer->setObjectName("soundsContainer");
    auto *nativeLayout = new QVBoxLayout(nativeContainer);
    auto *nativeSource = new QLabel(QStringLiteral("Native soft source"), nativeContainer);
    nativeSource->setObjectName("nativeSoftSource");
    nativeLayout->addWidget(nativeSource);
    layout->addWidget(nativeContainer);
    for (const auto &name : {"gpNativeInstrumentEffects", "gpMasterPostProcessing"}) {
        auto *anchor = new QLabel(name, result.get());
        anchor->setObjectName(name);
        layout->addWidget(anchor);
    }
    result->show();
    return result;
}
QString identity(QListWidget *list, int row) {
    return list->itemWidget(list->item(row))->property("gpvst3EntryId").toString();
}
}

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QTemporaryDir dir;
    if (!check(dir.isValid(), "temporary directory")) return 1;
    qputenv("GPVST3_DATA_DIR", dir.path().toUtf8());
    qputenv("GPVST3_SCORE_PATH", "C:/scores/p8-ui.gp");
    qputenv("GPVST3_TRACK", "1");
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication app(argc, argv);
    QJsonArray catalog;
    for (const auto &id : {"A", "B", "C"}) catalog.append(QJsonObject{
        {"module", QString("C:/VST3/%1.vst3").arg(id)}, {"class_id", id},
        {"name", QString("%1 very long effect name for a narrow Guitar Pro sidebar").arg(id)},
        {"vendor", "Test Vendor"}, {"compatible", true}, {"recognition_status", "ready"}});
    for (const auto &status : {"queued", "running", "failed", "timeout"}) catalog.append(QJsonObject{
        {"module", QString("C:/VST3/%1.vst3").arg(status)}, {"class_id", status},
        {"name", status}, {"compatible", true}, {"recognition_status", status}});
    auto enabled = [&](int index) { auto value = catalog[index].toObject(); value["enabled"] = true; return value; };
    QJsonObject chain{{"effects", QJsonArray{}}};
    gpvst3::state::setScopeEffects(chain, gpvst3::state::ScopeKind::Track, QJsonArray{enabled(1)},
        "C:/scores/p8-ui.gp", "C:/scores/p8-ui.gp#track-1", 1, "Guitar");
    gpvst3::state::setScopeEffects(chain, gpvst3::state::ScopeKind::Global,
        QJsonArray{enabled(2), enabled(0)});
    if (!check(gpvst3::state::writeChain(chain), "write scoped state")) return 1;
    gpvst3::ui::setVst3SelectionControl(globalControl);
    gpvst3::ui::setVst3TrackSelectionControl(trackControl);
    gpvst3::ui::setVst3EditorControl(globalEditor, nullptr);
    gpvst3::ui::setVst3TrackControls(nullptr, trackEditor);
    gpvst3::ui::setVst3Catalog(catalog);
    auto soundHost = host();
    gpvst3::ui::showEffectChainPanel(true);
    pumpEvents();
    auto *track = soundHost->findChild<QListWidget *>("gpvst3TrackChainList");
    auto *global = soundHost->findChild<QListWidget *>("gpvst3GlobalChainList");
    auto *trackSection = soundHost->findChild<QWidget *>("gpvst3TrackVst3Section");
    auto *globalSection = soundHost->findChild<QWidget *>("gpvst3GlobalVst3Section");
    auto *nativeSource = soundHost->findChild<QLabel *>("nativeSoftSource");
    auto *entry = soundHost->findChild<QPushButton *>("gpvst3SoundEffectChainButton");
    if (!check(track && global && trackSection && globalSection && trackSection->isAncestorOf(track) &&
               globalSection->isAncestorOf(global) && track->isVisible() && global->isVisible(),
               "actual lists are visible inside their separate native sections")) return 1;
    if (!check(nativeSource && entry && nativeSource->parentWidget()->objectName() == "soundsContainer" &&
               entry->parentWidget() == soundHost.get(),
               "native soft-source slot remains intact while the VST3 entry is mounted on SoundRack")) return 1;
    auto *layout = soundHost->layout();
    if (!check(layout->indexOf(trackSection) == layout->indexOf(soundHost->findChild<QWidget *>("gpNativeInstrumentEffects")) + 1 &&
               layout->indexOf(globalSection) == layout->indexOf(soundHost->findChild<QWidget *>("gpMasterPostProcessing")) + 1 &&
               trackSection->findChild<QFrame *>("gpvst3TrackVst3Divider")->isVisible() &&
               globalSection->findChild<QFrame *>("gpvst3GlobalVst3Divider")->isVisible(),
               "sections and visible dividers immediately follow the untouched native controls")) return 1;
    if (!check(track->count() == 1 && global->count() == 2 &&
               soundHost->findChild<QListWidget *>("gpvst3AvailableList")->count() == 2 &&
               soundHost->findChild<QListWidget *>("gpvst3GlobalAvailableList")->count() == 1,
               "both scopes are populated and non-ready entries stay hidden")) return 1;
    if (!check(identity(global, 0).endsWith("\nC") && identity(global, 1).endsWith("\nA"), "saved global order")) return 1;
    if (!check(global->model()->moveRow({}, 1, {}, 0), "enabled list supports a real model move")) return 1;
    if (!check(globalSelection.size() == 2 && globalSelection[0].classId == "A" && globalSelection[1].classId == "C",
               "a list move publishes the exact processor order")) return 1;
    trackSection->findChild<QCheckBox *>("gpvst3Enabled_A")->setChecked(true);
    globalSection->findChild<QCheckBox *>("gpvst3GlobalEnabled_C")->setChecked(false);
    pumpEvents();
    if (!check(trackSelection.size() == 2 && selectedTrack == "C:/scores/p8-ui.gp#track-1" &&
               globalSelection.size() == 1 && globalSelection[0].classId == "A" &&
               track->count() == 2 && global->count() == 1,
               "simultaneous controls publish only their fixed scope")) return 1;
    doubleClick(trackSection->findChild<QPushButton *>("gpvst3Name_A"));
    if (!check(editorScope == selectedTrack, "name opens the track processor editor")) return 1;
    doubleClick(globalSection->findChild<QPushButton *>("gpvst3GlobalName_A"));
    if (!check(editorScope == "global", "name opens the independent global editor")) return 1;
    for (const int width : {260, 320, 420}) {
        soundHost->setFixedWidth(width);
        soundHost->resize(width, 1100);
        pumpEvents();
        for (auto *section : {trackSection, globalSection}) {
            const QString prefix = section == trackSection ? "gpvst3" : "gpvst3Global"; auto *name = section->findChild<QPushButton *>(prefix + "Name_A");
            auto *toggle = section->findChild<QCheckBox *>(prefix + "Enabled_A");
            if (!check(name && name->text().startsWith("A") && name->width() >= 60 &&
                       name->toolTip().contains("Test Vendor") && name->toolTip().contains("C:/VST3/A.vst3") &&
                       toggle->geometry().right() < name->geometry().left() &&
                       section->findChildren<QPushButton *>(prefix + "Editor_A").isEmpty(),
                       "narrow/DPI layout preserves prefix, identity and removes dedicated GUI controls")) return 1;
        }
    }
    qputenv("GPVST3_TRACK", "2");
    gpvst3::ui::refreshVst3TrackContext();
    pumpEvents();
    if (!check(track->count() == 0 && global->count() == 1, "track change leaves global content active")) return 1;
    qputenv("GPVST3_TRACK", "1");
    gpvst3::ui::refreshVst3TrackContext();
    pumpEvents();
    if (!check(track->count() == 2 && global->count() == 1, "return restores the correct track")) return 1;
    soundHost.reset();
    soundHost = host();
    gpvst3::ui::showEffectChainPanel();
    gpvst3::ui::showEffectChainPanel();
    pumpEvents();
    if (!check(soundHost->findChildren<QListWidget *>("gpvst3TrackChainList").size() == 1 &&
               soundHost->findChild<QListWidget *>("gpvst3TrackChainList")->count() == 2 &&
               soundHost->findChildren<QListWidget *>("gpvst3GlobalChainList").size() == 1 &&
               soundHost->findChild<QListWidget *>("gpvst3GlobalChainList")->count() == 1,
               "sidebar destruction and repeated attach preserve both chains without duplicates")) return 1;
    gpvst3::ui::shutdownEditors();
    soundHost.reset();

    // Exercise the production callback combination, including a separate
    // non-blocking active snapshot and generation-checked asynchronous queue.
    const QString score = "C:/scores/p8-ui.gp", key1 = score + "#track-1", key2 = score + "#track-2";
    const auto key1String = key1.toStdString(), key2String = key2.toStdString();
    gpvst3::state::setRuntimeTrackContext(score, key1, 1, "track-one");
    gpvst3::state::loadChain(chain);
    gpvst3::state::setScopeEffects(chain, gpvst3::state::ScopeKind::Track, QJsonArray{enabled(1)}, score, key1, 1);
    if (!check(gpvst3::state::writeChain(chain), "seed stale saved enabled intent")) return 1;
    gpvst3::ui::setVst3TrackSelectionRequestControl(trackControl);
    gpvst3::ui::setVst3TrackGenerationRequestControl(generationTrackControl);
    gpvst3::ui::setVst3BusyControl(busyControl);
    gpvst3::ui::setVst3TrackControls(captureTrackControl, trackEditor, activeTrackControl);
    snapshotBusy = true;
    soundHost = host();
    gpvst3::ui::showEffectChainPanel();
    pumpEvents();
    trackSection = soundHost->findChild<QWidget *>("gpvst3TrackVst3Section");
    track = soundHost->findChild<QListWidget *>("gpvst3TrackChainList");
    const auto toggle = [&](const char *id) {
        return trackSection->findChild<QCheckBox *>(QString("gpvst3Enabled_") + id);
    };
    const auto rowLoading = [&](const QString &rowName) {
        const auto *row = soundHost->findChild<QWidget *>(rowName);
        const auto *loading = row ? row->findChild<QWidget *>("gpvst3RowLoading") : nullptr;
        return loading && loading->isVisible();
    };
    const auto savedEnabled = [&](const char *id) {
        QJsonObject saved;
        gpvst3::state::loadChain(saved);
        for (const auto &value : gpvst3::state::scopeEffects(saved, gpvst3::state::ScopeKind::Track, score, key1)) {
            const auto effect = value.toObject();
            if (effect.value("class_id").toString() == id) return effect.value("enabled").toBool();
        }
        return false;
    };
    if (!check(toggle("B")->checkState() == Qt::Unchecked && track->count() == 0 &&
               savedEnabled("B") && generationRequests == 0 && stateCaptures == 0,
               "first busy snapshot never shows saved intent active or invokes plugin state capture")) return 1;
    snapshotBusy = false;
    gpvst3::ui::reloadVst3Selections();
    pumpEvents();
    if (!check(toggle("B")->checkState() == Qt::Unchecked && savedEnabled("B"),
               "reliable empty runtime stays unchecked and does not erase saved intent")) return 1;
    soundHost.reset();
    if (!check(savedEnabled("B"), "panel destruction preserves saved intent while runtime is empty")) return 1;
    soundHost = host();
    gpvst3::ui::showEffectChainPanel();
    pumpEvents();
    trackSection = soundHost->findChild<QWidget *>("gpvst3TrackVst3Section");
    track = soundHost->findChild<QListWidget *>("gpvst3TrackChainList");
    const auto capturesBeforeClick = stateCaptures;
    toggle("A")->click();
    pumpEvents();
    if (!check(trackSelection.size() == 1 && trackSelection[0].classId == "A" &&
               toggle("A")->checkState() == Qt::PartiallyChecked &&
               toggle("B")->checkState() == Qt::Unchecked && !savedEnabled("B") && savedEnabled("A") &&
               stateCaptures == capturesBeforeClick,
               "new request excludes hidden saved selection and stays visibly pending without state capture")) return 1;
    auto *pendingRow = soundHost->findChild<QWidget *>("gpvst3EffectRow_A");
    if (!check(pendingRow && pendingRow->findChild<QWidget *>("gpvst3RowLoading") &&
               pendingRow->findChild<QWidget *>("gpvst3RowLoading")->isVisible() &&
               pendingRow->findChild<QWidget *>("gpvst3RowLoading")->testAttribute(Qt::WA_TransparentForMouseEvents) &&
               !rowLoading("gpvst3GlobalEffectRow_A"),
               "track selection animates the requested plugin row")) return 1;
    snapshotBusy = true;
    gpvst3::ui::reloadVst3Selections();
    pumpEvents();
    if (!check(toggle("A")->checkState() == Qt::PartiallyChecked,
               "busy snapshot retains a cancellable pending selection")) return 1;
    toggle("A")->click();
    pumpEvents();
    if (!check(trackSelection.empty() && toggle("A")->checkState() == Qt::Unchecked && !savedEnabled("A") &&
               !rowLoading("gpvst3EffectRow_A"),
               "clicking a pending row sends the empty selection and persists cancellation")) return 1;
    snapshotBusy = false;
    finishTrackRequest(key1String);
    toggle("A")->click();
    pumpEvents();
    finishTrackRequest(key1String);
    if (!check(toggle("A")->checkState() == Qt::Checked && track->count() == 1,
               "only a completed active runtime is checked")) return 1;
    pendingRow = soundHost->findChild<QWidget *>("gpvst3EffectRow_A");
    if (!check(pendingRow && (!pendingRow->findChild<QWidget *>("gpvst3RowLoading") ||
                             !pendingRow->findChild<QWidget *>("gpvst3RowLoading")->isVisible()),
               "completed track selection stops row animation")) return 1;
    toggle("B")->click();
    pumpEvents();
    if (!check(toggle("A")->checkState() == Qt::Checked && toggle("B")->checkState() == Qt::PartiallyChecked &&
               !rowLoading("gpvst3EffectRow_A") && rowLoading("gpvst3EffectRow_B") &&
               !rowLoading("gpvst3GlobalEffectRow_A"),
               "adding B animates only B while active A and the global chain stay settled")) return 1;
    toggle("B")->click();
    pumpEvents();
    finishTrackRequest(key1String);
    if (!check(toggle("A")->checkState() == Qt::Checked && toggle("B")->checkState() == Qt::Unchecked &&
               !rowLoading("gpvst3EffectRow_A") && !rowLoading("gpvst3EffectRow_B"),
               "cancelling B preserves active A and clears the loading background")) return 1;
    // A second edit while B is still preparing must retain B's row feedback
    // even when that edit removes the already-active A from the newest chain.
    toggle("B")->click();
    pumpEvents();
    toggle("A")->click();
    pumpEvents();
    if (!check(toggle("A")->checkState() == Qt::Unchecked &&
               toggle("B")->checkState() == Qt::PartiallyChecked &&
               !rowLoading("gpvst3EffectRow_A") && rowLoading("gpvst3EffectRow_B"),
               "editing a settled row preserves the pending row background")) return 1;
    toggle("B")->click();
    pumpEvents();
    finishTrackRequest(key1String);
    toggle("A")->click();
    pumpEvents();
    finishTrackRequest(key1String);
    if (!check(toggle("A")->checkState() == Qt::Checked &&
               !rowLoading("gpvst3EffectRow_A") && !rowLoading("gpvst3EffectRow_B"),
               "settling the replacement request clears retained row feedback")) return 1;
    // A rejected replacement must leave the already queued B request and its
    // background feedback intact.
    toggle("B")->click();
    pumpEvents();
    rejectRequest = true;
    toggle("A")->click();
    pumpEvents();
    rejectRequest = false;
    if (!check(toggle("A")->checkState() == Qt::Checked &&
               toggle("B")->checkState() == Qt::PartiallyChecked &&
               rowLoading("gpvst3EffectRow_B"),
               "rejected replacement preserves the queued row background")) return 1;
    toggle("B")->click();
    pumpEvents();
    finishTrackRequest(key1String);
    if (!check(toggle("A")->checkState() == Qt::Checked &&
               !rowLoading("gpvst3EffectRow_B"),
               "accepted replacement settles after rejection")) return 1;
    snapshotBusy = true;
    gpvst3::ui::reloadVst3Selections();
    pumpEvents();
    if (!check(toggle("A")->checkState() == Qt::Checked && savedEnabled("A"),
               "busy snapshot preserves last reliable live state")) return 1;
    toggle("A")->click();
    pumpEvents();
    if (!check(trackSelection.empty(), "active A can always submit an empty selection")) return 1;
    toggle("B")->click();
    pumpEvents();
    if (!check(trackSelection.size() == 1 && trackSelection[0].classId == "B" &&
               toggle("A")->checkState() == Qt::Unchecked && toggle("B")->checkState() == Qt::PartiallyChecked,
               "A off then B on uses the newest pending selection")) return 1;
    snapshotBusy = false;
    finishTrackRequest(key1String);
    if (!check(toggle("B")->checkState() == Qt::Checked && !savedEnabled("A") && savedEnabled("B"),
               "B commits after replacing A")) return 1;
    toggle("B")->click();
    pumpEvents();
    finishTrackRequest(key1String);
    toggle("C")->click();
    pumpEvents();
    finishTrackRequest(key1String, false);
    if (!check(toggle("C")->checkState() == Qt::Unchecked && !savedEnabled("C") &&
               !rowLoading("gpvst3EffectRow_C"),
               "failed worker request returns to unchecked and can be retried")) return 1;
    rejectRequest = true;
    toggle("C")->click();
    pumpEvents();
    if (!check(toggle("C")->checkState() == Qt::Unchecked && !savedEnabled("C"),
               "queue rejection restores checkbox and saved intent")) return 1;
    rejectRequest = false;
    toggle("C")->click();
    pumpEvents();
    finishTrackRequest(key1String);
    if (!check(toggle("C")->checkState() == Qt::Checked, "retry commits successfully")) return 1;
    const auto beforeGeneration = gpvst3::state::runtimeSelectionGeneration();
    gpvst3::state::setRuntimeTrackContext(score, key1, 1, "track-one-rebound");
    gpvst3::ui::refreshVst3TrackContext();
    pumpEvents();
    toggle("C")->click();
    pumpEvents();
    if (!check(requestedGeneration > beforeGeneration && trackSelection.empty(),
               "same track key with a new generation remains editable")) return 1;
    finishTrackRequest(key1String);
    toggle("A")->click();
    pumpEvents();
    finishTrackRequest(key1String);
    activeTracks[key2String] = {{"C:/VST3/B.vst3", "B"}};
    gpvst3::state::setRuntimeTrackContext(score, key2, 2, "track-two");
    gpvst3::ui::refreshVst3TrackContext();
    pumpEvents();
    if (!check(toggle("B")->checkState() == Qt::Checked && toggle("A")->checkState() == Qt::Unchecked,
               "track change reads an independent runtime snapshot")) return 1;
    gpvst3::state::setRuntimeTrackContext(score, key1, 1, "track-one-rebound");
    gpvst3::ui::refreshVst3TrackContext();
    pumpEvents();
    if (!check(toggle("A")->checkState() == Qt::Checked && toggle("B")->checkState() == Qt::Unchecked,
               "returning to a track restores its own active runtime")) return 1;
    gpvst3::ui::shutdownEditors();
    std::cout << "PASS: P8 scope/layout/reorder and generation-aware track active/pending/cancel/failure/retry state.\n";
}
