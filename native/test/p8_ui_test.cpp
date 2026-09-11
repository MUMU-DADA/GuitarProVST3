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

namespace {
void doubleClick(QWidget *widget) {
    QMouseEvent event(QEvent::MouseButtonDblClick, QPointF(2, 2), Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(widget, &event);
    QCoreApplication::processEvents();
}
bool check(bool value, const char *message) {
    if (!value) std::cerr << "FAIL: " << message << '\n';
    return value;
}
std::vector<gpvst3::ui::Vst3SelectionEntry> globalSelection, trackSelection;
std::string selectedTrack, editorScope;
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
    result->setObjectName("soundsContainer");
    auto *layout = new QVBoxLayout(result.get());
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
    QCoreApplication::processEvents();
    auto *track = soundHost->findChild<QListWidget *>("gpvst3TrackChainList");
    auto *global = soundHost->findChild<QListWidget *>("gpvst3GlobalChainList");
    auto *trackSection = soundHost->findChild<QWidget *>("gpvst3TrackVst3Section");
    auto *globalSection = soundHost->findChild<QWidget *>("gpvst3GlobalVst3Section");
    if (!check(track && global && trackSection && globalSection && trackSection->isAncestorOf(track) &&
               globalSection->isAncestorOf(global) && track->isVisible() && global->isVisible(),
               "actual lists are visible inside their separate native sections")) return 1;
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
    QCoreApplication::processEvents();
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
        QCoreApplication::processEvents();
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
    if (!check(track->count() == 0 && global->count() == 1, "track change leaves global content active")) return 1;
    qputenv("GPVST3_TRACK", "1");
    gpvst3::ui::refreshVst3TrackContext();
    if (!check(track->count() == 2 && global->count() == 1, "return restores the correct track")) return 1;
    soundHost.reset();
    soundHost = host();
    gpvst3::ui::showEffectChainPanel();
    gpvst3::ui::showEffectChainPanel();
    QCoreApplication::processEvents();
    if (!check(soundHost->findChildren<QListWidget *>("gpvst3TrackChainList").size() == 1 &&
               soundHost->findChild<QListWidget *>("gpvst3TrackChainList")->count() == 2 &&
               soundHost->findChildren<QListWidget *>("gpvst3GlobalChainList").size() == 1 &&
               soundHost->findChild<QListWidget *>("gpvst3GlobalChainList")->count() == 1,
               "sidebar destruction and repeated attach preserve both chains without duplicates")) return 1;
    gpvst3::ui::shutdownEditors();
    std::cout << "PASS: P8 actual section content, independent controls, real reorder, narrow/DPI and sidebar rebuild.\n";
}
