#include <QtCore/QCoreApplication>
#include <QtCore/QFileInfo>
#include <QtCore/QTimer>
#include <QtGui/QImageIOPlugin>

#include "modules/bootstrap.h"
#include "modules/gp_hook.h"
#include "modules/qt_ui.h"
#include "modules/state_manager.h"

namespace {

void writeObservation() {
    gpvst3::state::writeRealtimeObservation(gpvst3::bootstrap::hookSnapshot());
    if (auto *application = QCoreApplication::instance())
        QTimer::singleShot(250, application, &writeObservation);
}

void stopObservation() {
    gpvst3::state::writeRealtimeObservation(gpvst3::bootstrap::hookSnapshot());
    gpvst3::hook::shutdown();
}

}

class GuitarProVst3Autoload final : public QImageIOPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.qt-project.Qt.QImageIOHandlerFactoryInterface" FILE "vst3-autoload.json")

public:
    GuitarProVst3Autoload() {
        if (!qApp || QFileInfo(QCoreApplication::applicationFilePath()).baseName().compare("GuitarPro", Qt::CaseInsensitive) != 0)
            return;
        if (qApp->property("gpvst3P0Scheduled").toBool()) return;
        qApp->setProperty("gpvst3P0Scheduled", true);
        auto *application = qApp;
        QTimer::singleShot(0, application, [application] {
            auto status = gpvst3::bootstrap::initialize();
            gpvst3::state::writeStatus(status);
            gpvst3::ui::showEffectChainPanel();
            gpvst3::ui::syncVst3Selection();
            auto *scanTimer = new QTimer(application);
            scanTimer->setInterval(100);
            QObject::connect(scanTimer, &QTimer::timeout, scanTimer,
                             [scanTimer, status]() mutable {
                                 if (!gpvst3::bootstrap::pollVst3(status)) return;
                                 scanTimer->stop();
                                 scanTimer->deleteLater();
                             });
            scanTimer->start();
            const auto hook = status.value("gp_hook").toObject();
            if (!hook.value("enabled").toBool()) return;
            writeObservation();
            qAddPostRoutine(&stopObservation);
        });
    }

    Capabilities capabilities(QIODevice *, const QByteArray &) const override { return {}; }
    QImageIOHandler *create(QIODevice *, const QByteArray &) const override { return nullptr; }
};

#include "vst3_autoload.moc"
