// Test-only Qt driver. It supplies no audio/track provider or MCP service.
// The release DLL must discover and process the score by itself.
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QLibrary>
#include <QtCore/QTimer>
#include <QtCore/QSet>
#include <QtGui/QFileOpenEvent>
#include <QtGui/QGenericPlugin>
#include <QtWidgets/QAction>
#include <QtWidgets/QAbstractButton>
#include <QtWidgets/QApplication>
#include <QtWidgets/QWidget>

class P8StandaloneDriver final : public QGenericPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID QGenericPluginFactoryInterface_iid FILE "p8-test-driver.json")
public:
    QObject *create(const QString &name, const QString &) override {
        if (name != "gpvst3_test_driver" || QFileInfo(QCoreApplication::applicationFilePath()).baseName() != "GuitarPro") return nullptr;
        const QString directory = qEnvironmentVariable("GPVST3_DATA_DIR");
        const QString score = qEnvironmentVariable("GPVST3_TEST_SCORE");
        const QString runtimeTest = qEnvironmentVariable("GPVST3_TEST_RUNTIME_DLL");
        if (runtimeTest.isEmpty() && (QFileInfo(score).absolutePath() != QDir(directory).absolutePath() || !QFileInfo::exists(score))) return nullptr;
        auto *timer = new QTimer;
        timer->setInterval(500);
        connect(timer, &QTimer::timeout, timer, [directory, score, runtimeTest, tick=0, opened=false, played=false]() mutable {
            ++tick;
            if (QFileInfo::exists(QDir(directory).filePath("driver-stop"))) { QCoreApplication::quit(); return; }
            if (tick < 5 || QApplication::activeModalWidget()) return;
            if (!runtimeTest.isEmpty() && !opened) {
                opened = true;
                QLibrary library(runtimeTest);
                using Run = int (*)(const char *);
                auto run = reinterpret_cast<Run>(library.resolve("gpvst3_run_runtime_tests"));
                const auto fixture = qEnvironmentVariable("GPVST3_TEST_RUNTIME_FIXTURE").toUtf8();
                const int result = run ? run(fixture.constData()) : -1;
                QFile status(QDir(directory).filePath("runtime-test.json"));
                if (status.open(QIODevice::WriteOnly)) status.write(QJsonDocument(QJsonObject{{"result",result},{"driver","in_guitar_pro"}}).toJson());
                QCoreApplication::quit();
                return;
            }
            if (!opened) {
                opened = true;
                QFileOpenEvent event(score);
                QCoreApplication::sendEvent(qApp, &event);
                return;
            }
            QJsonArray candidates;
            QJsonArray controls;
            QAction *play = nullptr;
            QSet<QAction *> actions;
            for (auto *window : QApplication::allWidgets()) {
                if (auto *button = qobject_cast<QAbstractButton *>(window))
                    controls.append(QJsonObject{{"object",button->objectName()},{"text",button->text()},{"tooltip",button->toolTip()}});
                for (auto *action : window->actions()) actions.insert(action);
                for (auto *action : window->findChildren<QAction *>()) actions.insert(action);
            }
            for (auto *action : actions) {
                    controls.append(QJsonObject{{"object",action->objectName()},{"text",action->text()},{"shortcut",action->shortcut().toString()}});
                    if (action->shortcut() != QKeySequence(Qt::Key_Space)) continue;
                    candidates.append(QJsonObject{{"object",action->objectName()},{"text",action->text()},{"enabled",action->isEnabled()}});
                    if (action->isEnabled()) play = action;
            }
            if (!played && tick >= 9 && candidates.size() == 1 && play) { played = true; play->trigger(); }
            QFile status(QDir(directory).filePath("driver.json"));
            if (status.open(QIODevice::WriteOnly)) status.write(QJsonDocument(QJsonObject{{"opened",opened},{"played",played},{"play_actions",candidates},{"controls",controls}}).toJson());
        });
        timer->start();
        return timer;
    }
};
#include "p8_standalone_driver.moc"
