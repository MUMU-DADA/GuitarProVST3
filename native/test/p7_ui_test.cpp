#include "qt_ui.h"
#include "state_manager.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QTemporaryDir>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>
#include <QtCore/QTimer>

#include <iostream>

namespace {
bool check(bool value, const char *message) {
    if (!value) std::cerr << "FAIL: " << message << '\n';
    return value;
}
bool rejectSelection(const std::vector<gpvst3::ui::Vst3SelectionEntry> &, std::string *error) noexcept {
    if (error) *error = "host_unsupported";
    return false;
}
}

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QTemporaryDir directory;
    if (!check(directory.isValid(), "temporary directory")) return 1;
    qputenv("GPVST3_DATA_DIR", directory.path().toUtf8());
    QApplication application(argc, argv);

    const QJsonArray catalog{
        QJsonObject{{"module", "C:/VST3/One.vst3"}, {"class_id", "ONE"},
                    {"name", "One"}, {"vendor", "Test"}, {"compatible", true}},
        QJsonObject{{"module", "C:/VST3/Two.vst3"}, {"class_id", "TWO"},
                    {"name", "Two"}, {"vendor", "Test"}, {"compatible", true}}};
    if (!check(gpvst3::state::writeChain(QJsonObject{{"effects", QJsonArray{}}}),
               "write empty P7 sidecar")) return 1;
    gpvst3::ui::setVst3Catalog(catalog);
    gpvst3::ui::showEffectChainPanel();
    QCoreApplication::processEvents();
    auto *panel = qApp->property("gpvst3P5Panel").value<QWidget *>();
    if (!check(panel != nullptr, "P7 panel created")) return 1;
    if (!check(!panel->isVisible(), "P7 panel does not pop up at startup")) return 1;
    if (!check(panel->findChildren<QCheckBox *>().size() == 2, "catalog rows created")) return 1;
    panel->show();
    QCoreApplication::processEvents();
    auto *editorHost = panel->findChild<QWidget *>("gpvst3NativeEditorHost");
    if (!check(editorHost != nullptr && !editorHost->isVisible(),
               "native editor host does not intercept selector clicks before opening")) return 1;
    panel->hide();
    for (auto *button : panel->findChildren<QPushButton *>()) {
        if (!check(button->text() != QStringLiteral("添加") &&
                       button->text() != QStringLiteral("删除") &&
                       button->text() != QStringLiteral("保存") &&
                       !button->text().contains(QStringLiteral("旁路")),
                   "P7 removed legacy controls")) return 1;
    }
    auto *first = panel->findChildren<QCheckBox *>().first();
    first->setChecked(true);
    QCoreApplication::processEvents();
    QJsonObject saved;
    if (!check(gpvst3::state::loadChain(saved), "load P7 sidecar")) return 1;
    const auto effect = saved.value("effects").toArray().first().toObject();
    if (!check(effect.value("enabled").toBool() && !effect.value("bypass").toBool(),
               "checked means enabled")) return 1;
    gpvst3::ui::setVst3SelectionControl(&rejectSelection);
    auto *second = panel->findChild<QCheckBox *>("gpvst3Enabled_TWO");
    second->setChecked(true);
    auto *status = panel->findChild<QLabel *>("gpvst3Status");
    if (!check(!second->isChecked() && first->isChecked() &&
                   status->text().contains(QStringLiteral("兼容性校验")) &&
                   status->toolTip() == QStringLiteral("host_unsupported"),
               "failed selection rolls back and reports the actual host failure")) return 1;
    if (!check(gpvst3::state::loadChain(saved) &&
                   !saved.value("effects").toArray().at(1).toObject().value("enabled").toBool(),
               "rejected selection is not persisted as enabled")) return 1;
    gpvst3::ui::setVst3SelectionControl(nullptr);
    panel->findChildren<QPushButton *>().first()->click();
    QCoreApplication::processEvents();
    bool hostLimited = false;
    for (auto *label : panel->findChildren<QLabel *>())
        hostLimited |= label->text().contains(QStringLiteral("host_limited"));
    if (!check(hostLimited, "native editor boundary is explicit")) return 1;

    QWidget soundHost;
    soundHost.setObjectName(QStringLiteral("soundsContainer"));
    soundHost.setLayout(new QVBoxLayout);
    soundHost.show();
    for (auto *timer : qApp->findChildren<QTimer *>()) timer->setInterval(0);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    if (!check(soundHost.findChild<QPushButton *>("gpvst3SoundEffectChainButton") != nullptr,
               "persistent sound-section entry")) return 1;
    delete soundHost.findChild<QPushButton *>("gpvst3SoundEffectChainButton");
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    if (!check(soundHost.findChild<QPushButton *>("gpvst3SoundEffectChainButton") != nullptr,
               "entry returns after sidebar rebuild")) return 1;
    auto *entry = soundHost.findChild<QPushButton *>("gpvst3SoundEffectChainButton");
    entry->click();
    QCoreApplication::processEvents();
    auto *opened = qApp->property("gpvst3P5Panel").value<QWidget *>();
    if (!check(opened != nullptr && opened->isVisible(),
               "entry opens the existing panel")) return 1;
    panel->close();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    entry->click();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    auto *reopened = qApp->property("gpvst3P5Panel").value<QWidget *>();
    if (!check(reopened != nullptr && reopened->isVisible(),
               "entry reopens the panel after close")) return 1;
    reopened->close();
    std::cout << "PASS: P7 catalog UI, enabled semantics and host-limited editor evidence.\n";
    return 0;
}
