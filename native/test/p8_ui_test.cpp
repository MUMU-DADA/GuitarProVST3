#include "qt_ui.h"
#include "state_manager.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QTemporaryDir>
#include <QtWidgets/QApplication>
#include <QtWidgets/QFrame>
#include <QtWidgets/QLabel>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QVBoxLayout>
#include <QtCore/QTimer>
#include <iostream>

namespace {
bool check(bool value, const char *message) {
    if (!value) std::cerr << "FAIL: " << message << '\n';
    return value;
}
}

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QTemporaryDir dir;
    if (!check(dir.isValid(), "temporary directory")) return 1;
    qputenv("GPVST3_DATA_DIR", dir.path().toUtf8());
    qputenv("GPVST3_SCORE_PATH", "C:/scores/p8-ui.gp");
    qputenv("GPVST3_TRACK", "1");
    QApplication app(argc, argv);
    const QJsonArray catalog{
        QJsonObject{{"module", "C:/VST3/A.vst3"}, {"class_id", "A"},
                    {"name", "A very long effect name for a narrow Guitar Pro sidebar"},
                    {"vendor", "Test Vendor"}, {"compatible", true}},
        QJsonObject{{"module", "C:/VST3/B.vst3"}, {"class_id", "B"}, {"name", "B"}, {"vendor", "Test"}, {"compatible", true}},
        QJsonObject{{"module", "C:/VST3/C.vst3"}, {"class_id", "C"}, {"name", "C"}, {"vendor", "Test"}, {"compatible", true}}};
    if (!check(gpvst3::state::writeChain(QJsonObject{{"effects", QJsonArray{}}}), "seed state")) return 1;
    QJsonObject chain; gpvst3::state::loadChain(chain);
    gpvst3::state::setScopeEffects(chain, gpvst3::state::ScopeKind::Track,
        QJsonArray{QJsonObject{{"module", "C:/VST3/B.vst3"}, {"class_id", "B"}, {"name", "B"}, {"enabled", true}}},
        "C:/scores/p8-ui.gp", "C:/scores/p8-ui.gp#track-1", 1, "Guitar");
    gpvst3::state::setScopeEffects(chain, gpvst3::state::ScopeKind::Global,
        QJsonArray{QJsonObject{{"module", "C:/VST3/C.vst3"}, {"class_id", "C"}, {"name", "C"}, {"enabled", true}},
                   QJsonObject{{"module", "C:/VST3/A.vst3"}, {"class_id", "A"}, {"name", "A"}, {"enabled", true}}});
    if (!check(gpvst3::state::writeChain(chain), "write scoped state")) return 1;
    gpvst3::ui::setVst3Catalog(catalog);
    gpvst3::ui::showEffectChainPanel(true);
    QCoreApplication::processEvents();
    auto *panel = qApp->property("gpvst3P5Panel").value<QWidget *>();
    if (!check(panel && panel->objectName() == "gpvst3P7Panel", "P8 panel")) return 1;
    auto *tabs = panel->findChild<QTabWidget *>();
    if (!check(tabs && tabs->count() == 2 && tabs->tabText(0) == QStringLiteral("当前音轨") &&
               tabs->tabText(1) == QStringLiteral("全局 Master"), "scope tabs")) return 1;
    auto *track = panel->findChild<QListWidget *>("gpvst3TrackChainList");
    auto *global = panel->findChild<QListWidget *>("gpvst3GlobalChainList");
    auto *available = panel->findChild<QListWidget *>("gpvst3AvailableList");
    if (!check(track && global && available, "scope list object names")) return 1;
    if (!check(track->count() == 1 && available->count() == 2 && track->itemWidget(track->item(0)) != nullptr,
               "track catalog rows")) return 1;
    tabs->setCurrentIndex(1); QCoreApplication::processEvents();
    if (!check(global->count() == 2, "global enabled rows")) return 1;
    if (!check(global->itemWidget(global->item(0))->property("gpvst3EntryId").toString().endsWith("\nC"),
               "enabled entries stay first in saved order")) return 1;
    auto *nameButton = global->findChild<QPushButton *>("gpvst3Name_A");
    if (!check(nameButton && nameButton->sizePolicy().horizontalPolicy() == QSizePolicy::Ignored &&
               nameButton->toolTip().contains(QStringLiteral("A very long effect name")),
               "long plugin names keep a left aligned, discoverable tooltip")) return 1;

    QWidget soundHost;
    soundHost.setObjectName(QStringLiteral("soundsContainer"));
    auto *soundLayout = new QVBoxLayout(&soundHost);
    auto *nativeSource = new QLabel(QStringLiteral("音源效果器"), &soundHost);
    nativeSource->setObjectName(QStringLiteral("gpNativeInstrumentEffects"));
    auto *nativeMaster = new QLabel(QStringLiteral("母带后期处理"), &soundHost);
    nativeMaster->setObjectName(QStringLiteral("gpMasterPostProcessing"));
    soundLayout->addWidget(nativeSource);
    soundLayout->addWidget(nativeMaster);
    soundHost.show();
    for (auto *timer : qApp->findChildren<QTimer *>()) timer->setInterval(0);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    auto *trackSection = soundHost.findChild<QWidget *>("gpvst3TrackVst3Section");
    auto *globalSection = soundHost.findChild<QWidget *>("gpvst3GlobalVst3Section");
    if (!check(trackSection && trackSection->parentWidget() == &soundHost &&
               globalSection && globalSection->parentWidget() == &soundHost,
               "host sections are mounted as owned wrappers")) return 1;
    if (!check(trackSection->findChild<QFrame *>("gpvst3TrackVst3Divider") &&
               globalSection->findChild<QFrame *>("gpvst3GlobalVst3Divider"),
               "track and global sections have structural dividers")) return 1;
    if (!check(soundLayout->indexOf(nativeSource) < soundLayout->indexOf(trackSection) &&
               soundLayout->indexOf(nativeMaster) < soundLayout->indexOf(globalSection),
               "sections follow their native host anchors")) return 1;
    if (!check(soundHost.findChild<QLabel *>("gpNativeInstrumentEffects") == nativeSource &&
               soundHost.findChild<QLabel *>("gpMasterPostProcessing") == nativeMaster,
               "native source and master controls remain present")) return 1;
    panel->close();
    QJsonObject saved; gpvst3::state::loadChain(saved);
    const auto reordered = gpvst3::state::scopeEffects(saved, gpvst3::state::ScopeKind::Global);
    if (!check(reordered.size() == 3 && reordered.at(0).toObject().value("class_id") == "C" &&
               reordered.at(1).toObject().value("class_id") == "A", "drag order persisted")) return 1;
    std::cout << "PASS: P8 scope tabs, independent state and drag order.\n";
    return 0;
}
