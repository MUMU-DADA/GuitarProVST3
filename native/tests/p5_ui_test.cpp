#include "qt_ui.h"
#include "state_manager.h"

#include <QtWidgets/QApplication>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QPushButton>
#include <QtCore/QJsonArray>
#include <QtCore/QTemporaryDir>

#include <iostream>

namespace {
bool realtimeBypassed = false;

void captureBypass(bool bypassed) noexcept { realtimeBypassed = bypassed; }

bool check(bool value, const char *message) {
    if (!value) std::cerr << "FAIL: " << message << '\n';
    return value;
}
}

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QTemporaryDir directory;
    if (!directory.isValid()) return 1;
    qputenv("GPVST3_DATA_DIR", directory.path().toUtf8());
    QApplication application(argc, argv);
    const QJsonObject effect{{"plugin_path", "C:/missing.vst3"}, {"class_uid", "ABC"},
                             {"parameters", QJsonObject{{"1", 0.5}}}, {"state_chunk", "AQID"},
                             {"bypass", false}};
    const QJsonObject chain{{"score_id", "ui-score.gp"}, {"track", 2}, {"bus", "master"},
                            {"effects", QJsonArray{effect}}};
    if (!check(gpvst3::state::writeChain(chain), "write UI sidecar")) return 1;
    gpvst3::ui::setRealtimeBypassControl(&captureBypass);
    gpvst3::ui::showEffectChainPanel();
    QCoreApplication::processEvents();
    auto *panel = qApp->property("gpvst3P5Panel").value<QWidget *>();
    if (!check(panel != nullptr, "panel created")) return 1;
    if (!check(panel->windowTitle() == QStringLiteral("音源 · VST3 效果器链"), "panel title")) return 1;
    auto *list = panel->findChild<QListWidget *>();
    if (!check(list && list->count() == 1 && list->item(0)->checkState() == Qt::Checked,
               "restored missing effect is bypassed")) return 1;
    if (!check(panel->findChild<QLineEdit *>() != nullptr, "search and metadata controls")) return 1;
    bool hasBypass = false, hasSave = false;
    for (auto *button : panel->findChildren<QPushButton *>()) {
        hasBypass |= button->text().contains(QStringLiteral("旁路"));
        hasSave |= button->text() == QStringLiteral("保存");
    }
    if (!check(hasBypass && hasSave, "chain controls")) return 1;
    if (!check(realtimeBypassed, "missing effect starts realtime bypassed")) return 1;
    for (auto *button : panel->findChildren<QPushButton *>()) {
        if (button->text().contains(QStringLiteral("旁路"))) {
            button->click();
            break;
        }
    }
    if (!check(!realtimeBypassed, "bypass action reaches realtime control")) return 1;
    panel->close();
    QCoreApplication::processEvents();
    std::cout << "PASS: P5 Qt chain panel creation and sidecar restoration.\n";
    return 0;
}
