#include "qt_ui.h"
#include "state_manager.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QTemporaryDir>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QDialog>
#include <QtWidgets/QLabel>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QToolBar>
#include <QtWidgets/QVBoxLayout>
#include <QtGui/QMouseEvent>

#include <iostream>

namespace {
bool check(bool value, const char *message) {
    if (!value) std::cerr << "FAIL: " << message << '\n';
    return value;
}
std::vector<gpvst3::ui::Vst3SelectionEntry> selected;
bool selectGlobal(const std::vector<gpvst3::ui::Vst3SelectionEntry> &entries, std::string *) noexcept {
    selected = entries;
    return true;
}
bool selectTrack(const std::string &, const std::vector<gpvst3::ui::Vst3SelectionEntry> &entries,
                 std::string *) noexcept {
    selected = entries;
    return true;
}
bool requestGlobal(const std::vector<gpvst3::ui::Vst3SelectionEntry> &entries, std::string *) noexcept {
    selected = entries;
    return true;
}
bool requestTrack(const std::string &, const std::vector<gpvst3::ui::Vst3SelectionEntry> &entries,
                  std::string *) noexcept {
    selected = entries;
    return true;
}
}

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QTemporaryDir directory;
    if (!check(directory.isValid(), "temporary directory")) return 1;
    qputenv("GPVST3_DATA_DIR", directory.path().toUtf8());
    qputenv("GPVST3_SCORE_PATH", "C:/scores/p9-ui.gp");
    qputenv("GPVST3_TRACK", "1");
    QApplication app(argc, argv);

    QMainWindow window;
    window.setObjectName("p9TestMainWindow");
    auto *toolbar = new QToolBar(&window);
    toolbar->setObjectName("gpvst3TitleToolBar");
    window.addToolBar(toolbar);
    auto *sound = new QWidget(&window);
    sound->setObjectName("soundsContainer");
    auto *layout = new QVBoxLayout(sound);
    auto *trackAnchor = new QLabel(QStringLiteral("音源效果器"), sound);
    trackAnchor->setObjectName("gpNativeInstrumentEffects");
    layout->addWidget(trackAnchor);
    auto *globalAnchor = new QLabel(QStringLiteral("母带后期处理"), sound);
    globalAnchor->setObjectName("gpMasterPostProcessing");
    layout->addWidget(globalAnchor);
    window.setCentralWidget(sound);
    window.show();

    QJsonArray catalog{QJsonObject{{"module", "C:/VST3/A.vst3"}, {"class_id", "A"},
                                   {"name", "A very long effect name"}, {"vendor", "P9"},
                                   {"compatible", true}, {"recognition_status", "ready"}}};
    if (!check(gpvst3::state::writeChain(QJsonObject{{"effects", QJsonArray{}}}), "write sidecar")) return 1;
    gpvst3::ui::setVst3SelectionControl(selectGlobal);
    gpvst3::ui::setVst3TrackSelectionControl(selectTrack);
    gpvst3::ui::setVst3SelectionRequestControl(requestGlobal);
    gpvst3::ui::setVst3TrackSelectionRequestControl(requestTrack);
    gpvst3::ui::setVst3Catalog(catalog);
    gpvst3::ui::showEffectChainPanel(true);
    QCoreApplication::processEvents();

    auto aboutButtons = toolbar->findChildren<QPushButton *>("gpvst3AboutButton");
    if (!check(aboutButtons.size() == 1, "title toolbar has one About button")) return 1;
    aboutButtons.front()->click();
    QCoreApplication::processEvents();
    auto *about = qobject_cast<QDialog *>(qApp->property("gpvst3AboutDialog").value<QWidget *>());
    if (!check(about && about->isVisible() && !about->isModal() &&
               about->findChild<QLabel *>("gpvst3AboutDetails") &&
               about->findChild<QLabel *>("gpvst3AboutDetails")->text().contains("MIT License"),
               "About dialog content and nonmodal behavior")) return 1;
    auto *startupEnabled = about->findChild<QCheckBox *>("gpvst3PluginEnabledCheckBox");
    auto *openConfig = about->findChild<QPushButton *>("gpvst3OpenConfigButton");
    if (!check(startupEnabled && openConfig && startupEnabled->isChecked(),
               "About dialog exposes startup switch and configuration entry")) return 1;
    startupEnabled->setChecked(false);
    if (!check(!gpvst3::state::pluginEnabled(), "startup switch disables plugin for next launch")) return 1;
    startupEnabled->setChecked(true);
    if (!check(gpvst3::state::pluginEnabled(), "startup switch re-enables plugin")) return 1;
    about->hide();
    aboutButtons.front()->click();
    if (!check(qApp->property("gpvst3AboutDialog").value<QWidget *>() == about && about->isVisible(),
               "repeated About click reuses one dialog")) return 1;

    gpvst3::ui::setVst3ScanState("scan_failed", 4, 9, false,
                                 QStringLiteral("C:/private/module.vst3\nerror_code=E_P9"));
    auto *entry = sound->findChild<QPushButton *>("gpvst3SoundEffectChainButton");
    auto *status = sound->findChild<QLabel *>("gpvst3GlobalStatus");
    if (!check(entry && entry->text().contains(QStringLiteral("点击重试")) &&
               !entry->toolTip().contains("private") && status &&
               !status->toolTip().contains("private") &&
               !status->text().contains("error_code") &&
               !entry->text().contains("error_code"),
               "scan diagnostics stay out of visible text and tooltips")) return 1;

    auto *global = sound->findChild<QListWidget *>("gpvst3GlobalChainList");
    auto *available = sound->findChild<QListWidget *>("gpvst3GlobalAvailableList");
    if (!check(global && available, "global compact lists mounted")) return 1;
    auto *toggle = sound->findChild<QCheckBox *>("gpvst3GlobalEnabled_A");
    if (!check(toggle, "global row toggle present")) return 1;
    toggle->setChecked(true);
    QCoreApplication::processEvents();
    if (!check(toggle->parentWidget()->property("gpvst3EntryId").toString().endsWith("\nA"),
               "compact row keeps stable entry identity")) return 1;

    for (const int width : {260, 320, 420}) {
        window.resize(width, 800);
        QCoreApplication::processEvents();
        auto *name = sound->findChild<QPushButton *>("gpvst3GlobalName_A");
        if (!check(name && name->width() >= 24 &&
                   sound->findChildren<QPushButton *>("gpvst3GlobalEditor_A").isEmpty(),
                   "narrow sidebar row keeps name control without a dedicated GUI button")) return 1;
    }
    gpvst3::ui::shutdownEditors();
    std::cout << "PASS: P9 About toolbar idempotence/reuse, neutral scan feedback and compact narrow UI.\n";
    return 0;
}
