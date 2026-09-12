#include "qt_ui.h"
#include "state_manager.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QTemporaryDir>
#include <QtWidgets/QApplication>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QToolBar>
#include <QtWidgets/QVBoxLayout>

#include <iostream>

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QTemporaryDir directory;
    if (!directory.isValid()) return 1;
    qputenv("GPVST3_DATA_DIR", directory.path().toUtf8());
    qputenv("GPVST3_SCORE_PATH", "C:/scores/p10-ui.gp");
    qputenv("GPVST3_TRACK", "2");
    QApplication app(argc, argv);
    QMainWindow window;
    auto *toolbar = new QToolBar(&window);
    toolbar->setObjectName("gpvst3TitleToolBar");
    window.addToolBar(toolbar);
    auto *sound = new QWidget(&window);
    sound->setObjectName("soundsContainer");
    auto *layout = new QVBoxLayout(sound);
    auto *track = new QLabel(QStringLiteral("音源效果器"), sound);
    track->setObjectName("gpNativeInstrumentEffects");
    layout->addWidget(track);
    auto *global = new QLabel(QStringLiteral("母带后期处理"), sound);
    global->setObjectName("gpMasterPostProcessing");
    layout->addWidget(global);
    window.setCentralWidget(sound);
    window.show();
    gpvst3::state::writeChain(QJsonObject{{"effects", QJsonArray{}}});
    gpvst3::ui::setVst3Catalog(QJsonArray{QJsonObject{{"module", "C:/VST3/Test.vst3"},
        {"class_id", "P10"}, {"name", "P10 Test"}, {"vendor", "P10"},
        {"compatible", true}, {"recognition_status", "ready"}}});
    gpvst3::ui::showEffectChainPanel(true);
    QCoreApplication::processEvents();
    auto *button = sound->findChild<QPushButton *>("gpvst3SoundEffectChainButton");
    if (!button || !button->text().contains("Track 2") || !button->text().contains("VST3") ||
        !button->toolTip().contains("Track 2") || button->accessibleName().isEmpty()) {
        std::cerr << "FAIL: title identity missing for Track 2\n";
        return 1;
    }
    qputenv("GPVST3_TRACK", "3");
    gpvst3::ui::refreshVst3TrackContext();
    QCoreApplication::processEvents();
    if (!button->text().contains("Track 3") || !button->property("gpvst3TrackIdentity").toString().contains("Track 3")) {
        std::cerr << "FAIL: title identity did not refresh after track switch\n";
        return 1;
    }
    std::cout << "PASS: P10 title track identity, plugin summary and track switch refresh.\n";
    return 0;
}
