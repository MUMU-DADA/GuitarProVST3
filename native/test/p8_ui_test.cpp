#include "qt_ui.h"
#include "state_manager.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QTemporaryDir>
#include <QtWidgets/QApplication>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QTabWidget>
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
        QJsonObject{{"module", "C:/VST3/A.vst3"}, {"class_id", "A"}, {"name", "A"}, {"vendor", "Test"}, {"compatible", true}},
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
    panel->close();
    QJsonObject saved; gpvst3::state::loadChain(saved);
    const auto reordered = gpvst3::state::scopeEffects(saved, gpvst3::state::ScopeKind::Global);
    if (!check(reordered.size() == 3 && reordered.at(0).toObject().value("class_id") == "C" &&
               reordered.at(1).toObject().value("class_id") == "A", "drag order persisted")) return 1;
    std::cout << "PASS: P8 scope tabs, independent state and drag order.\n";
    return 0;
}
