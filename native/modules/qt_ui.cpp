#include "qt_ui.h"

#include "state_manager.h"

#include <algorithm>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QVariant>
#include <QtCore/QTimer>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QApplication>
#include <QtGui/QCloseEvent>
#include <QtWidgets/QAction>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QDockWidget>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QMenuBar>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>

namespace gpvst3::ui {
namespace {

constexpr int kPathRole = Qt::UserRole;
constexpr int kUidRole = Qt::UserRole + 1;

class ChainPanel final : public QWidget {
public:
    ChainPanel() {
        setAttribute(Qt::WA_DeleteOnClose);
        setWindowFlag(Qt::Tool);
        setWindowTitle(QStringLiteral("音源 · VST3 效果器链"));
        resize(720, 460);

        auto *root = new QVBoxLayout(this);
        auto *header = new QHBoxLayout;
        header->addWidget(new QLabel(QStringLiteral("VST3 效果器链"), this));
        search_ = new QLineEdit(this);
        search_->setPlaceholderText(QStringLiteral("搜索插件路径或 class UID"));
        header->addWidget(search_, 1);
        root->addLayout(header);

        auto *scope = new QFormLayout;
        scoreId_ = new QLineEdit(this);
        track_ = new QSpinBox(this);
        track_->setRange(0, 9999);
        bus_ = new QLineEdit(this);
        scope->addRow(QStringLiteral("曲谱标识"), scoreId_);
        scope->addRow(QStringLiteral("Track"), track_);
        scope->addRow(QStringLiteral("Bus"), bus_);
        root->addLayout(scope);

        auto *body = new QHBoxLayout;
        list_ = new QListWidget(this);
        list_->setSelectionMode(QAbstractItemView::SingleSelection);
        body->addWidget(list_, 1);

        auto *right = new QVBoxLayout;
        auto *chainButtons = new QHBoxLayout;
        add_ = new QPushButton(QStringLiteral("添加"), this);
        remove_ = new QPushButton(QStringLiteral("删除"), this);
        up_ = new QPushButton(QStringLiteral("上移"), this);
        down_ = new QPushButton(QStringLiteral("下移"), this);
        bypass_ = new QPushButton(QStringLiteral("切换旁路"), this);
        for (auto *button : {add_, remove_, up_, down_, bypass_}) chainButtons->addWidget(button);
        right->addLayout(chainButtons);
        pluginPath_ = new QLineEdit(this);
        classUid_ = new QLineEdit(this);
        auto *select = new QPushButton(QStringLiteral("重新选择插件…"), this);
        right->addWidget(new QLabel(QStringLiteral("插件路径"), this));
        right->addWidget(pluginPath_);
        right->addWidget(new QLabel(QStringLiteral("class UID"), this));
        right->addWidget(classUid_);
        right->addWidget(select);
        missing_ = new QLabel(this);
        missing_->setStyleSheet(QStringLiteral("color:#b00020"));
        right->addWidget(missing_);

        parameters_ = new QTableWidget(0, 2, this);
        parameters_->setHorizontalHeaderLabels({QStringLiteral("参数 ID"), QStringLiteral("值")});
        parameters_->horizontalHeader()->setStretchLastSection(true);
        parameters_->verticalHeader()->setVisible(false);
        right->addWidget(new QLabel(QStringLiteral("参数"), this));
        right->addWidget(parameters_, 1);
        auto *parameterButtons = new QHBoxLayout;
        addParameter_ = new QPushButton(QStringLiteral("添加参数"), this);
        removeParameter_ = new QPushButton(QStringLiteral("删除参数"), this);
        parameterButtons->addWidget(addParameter_);
        parameterButtons->addWidget(removeParameter_);
        right->addLayout(parameterButtons);
        body->addLayout(right, 2);
        root->addLayout(body, 1);

        auto *footer = new QHBoxLayout;
        status_ = new QLabel(this);
        footer->addWidget(status_, 1);
        auto *save = new QPushButton(QStringLiteral("保存"), this);
        auto *close = new QPushButton(QStringLiteral("关闭"), this);
        footer->addWidget(save);
        footer->addWidget(close);
        root->addLayout(footer);

        connect(search_, &QLineEdit::textChanged, this, [this] { filterItems(); });
        connect(list_, &QListWidget::currentRowChanged, this, [this](int) { syncSelected(); loadSelected(); });
        connect(add_, &QPushButton::clicked, this, [this] { addEffect(); });
        connect(remove_, &QPushButton::clicked, this, [this] { removeEffect(); });
        connect(up_, &QPushButton::clicked, this, [this] { moveEffect(-1); });
        connect(down_, &QPushButton::clicked, this, [this] { moveEffect(1); });
        connect(bypass_, &QPushButton::clicked, this, [this] { toggleBypass(); });
        connect(select, &QPushButton::clicked, this, [this] { choosePlugin(); });
        connect(addParameter_, &QPushButton::clicked, this, [this] { addParameter(); });
        connect(removeParameter_, &QPushButton::clicked, this, [this] { removeParameter(); });
        connect(save, &QPushButton::clicked, this, [this] { saveChain(); });
        connect(close, &QPushButton::clicked, this, &QWidget::close);
        connect(pluginPath_, &QLineEdit::editingFinished, this, [this] { updateSelectedMetadata(); });
        connect(classUid_, &QLineEdit::editingFinished, this, [this] { updateSelectedMetadata(); });
        connect(parameters_, &QTableWidget::cellChanged, this, [this](int, int) { dirty_ = true; });
        loadChain();
    }

private:
    static QString chooseVst3Path(QWidget *parent, const QString &title) {
        const QString file = QFileDialog::getOpenFileName(parent, title, QString(),
                                                           QStringLiteral("VST3 插件 (*.vst3)"));
        if (!file.isEmpty()) return file;
        return QFileDialog::getExistingDirectory(parent, title);
    }

    static QJsonObject effectObject(QListWidgetItem *item) {
        return QJsonObject{{"plugin_path", item->data(kPathRole).toString()},
                           {"class_uid", item->data(kUidRole).toString()},
                           {"parameters", QJsonObject{}}, {"state_chunk", QString{}},
                           {"bypass", item->checkState() == Qt::Checked}};
    }

    void loadChain() {
        QJsonObject chain;
        QString error;
        const bool valid = state::loadChain(chain, &error);
        scoreId_->setText(chain.value("score_id").toString());
        track_->setValue(chain.value("track").toInt());
        bus_->setText(chain.value("bus").toString(QStringLiteral("master")));
        list_->clear();
        for (const auto &value : chain.value("effects").toArray()) {
            const auto object = value.toObject();
            auto *item = new QListWidgetItem(object.value("plugin_path").toString(), list_);
            item->setData(kPathRole, object.value("plugin_path").toString());
            item->setData(kUidRole, object.value("class_uid").toString());
            const bool missing = object.value("plugin_path").toString().isEmpty() ||
                                 !QFileInfo::exists(object.value("plugin_path").toString());
            item->setCheckState(object.value("bypass").toBool() || missing ? Qt::Checked : Qt::Unchecked);
        }
        sidecar_ = chain;
        dirty_ = false;
        status_->setText(valid ? QStringLiteral("状态已恢复：%1").arg(state::sidecarPath())
                               : QStringLiteral("状态恢复失败，已使用空链：%1").arg(error));
        if (list_->count()) {
            list_->setCurrentRow(0);
        }
    }

    void filterItems() {
        const QString needle = search_->text().trimmed();
        for (int i = 0; i < list_->count(); ++i) {
            auto *item = list_->item(i);
            const QString haystack = item->text() + " " + item->data(kUidRole).toString();
            item->setHidden(!needle.isEmpty() && !haystack.contains(needle, Qt::CaseInsensitive));
        }
    }

    void loadSelected() {
        parameters_->blockSignals(true);
        parameters_->setRowCount(0);
        const int row = list_->currentRow();
        const auto effects = sidecar_.value("effects").toArray();
        if (row < 0 || row >= effects.size()) {
            pluginPath_->clear(); classUid_->clear(); missing_->clear();
            parameters_->blockSignals(false); return;
        }
        const auto object = effects.at(row).toObject();
        pluginPath_->setText(object.value("plugin_path").toString());
        classUid_->setText(object.value("class_uid").toString());
        updateMissing(pluginPath_->text());
        const auto parameters = object.value("parameters").toObject();
        for (auto it = parameters.begin(); it != parameters.end(); ++it) {
            const int target = parameters_->rowCount();
            parameters_->insertRow(target);
            parameters_->setItem(target, 0, new QTableWidgetItem(it.key()));
            parameters_->setItem(target, 1, new QTableWidgetItem(it.value().toVariant().toString()));
        }
        parameters_->blockSignals(false);
    }

    void updateMissing(const QString &path) {
        missing_->setText(path.isEmpty() || !QFileInfo::exists(path)
                              ? QStringLiteral("插件缺失或路径未设置：自动旁路") : QString());
    }

    void updateSelectedMetadata() {
        const int row = list_->currentRow(); if (row < 0) return;
        auto *item = list_->item(row);
        item->setText(pluginPath_->text()); item->setData(kPathRole, pluginPath_->text());
        item->setData(kUidRole, classUid_->text()); updateMissing(pluginPath_->text()); dirty_ = true;
    }

    void addEffect() {
        const QString path = chooseVst3Path(this, QStringLiteral("选择 VST3 插件"));
        if (path.isEmpty()) return;
        auto *item = new QListWidgetItem(path, list_);
        item->setData(kPathRole, path); item->setData(kUidRole, QString()); item->setCheckState(Qt::Unchecked);
        QJsonArray effects = sidecar_.value("effects").toArray(); effects.append(effectObject(item)); sidecar_.insert("effects", effects);
        list_->setCurrentItem(item); dirty_ = true;
    }

    void removeEffect() {
        const int row = list_->currentRow(); if (row < 0) return;
        delete list_->takeItem(row);
        QJsonArray effects = sidecar_.value("effects").toArray(); effects.removeAt(row); sidecar_.insert("effects", effects);
        dirty_ = true; if (list_->count()) list_->setCurrentRow((std::min)(row, list_->count() - 1));
    }

    void moveEffect(int delta) {
        const int row = list_->currentRow(), target = row + delta;
        if (row < 0 || target < 0 || target >= list_->count()) return;
        auto *item = list_->takeItem(row); list_->insertItem(target, item);
        QJsonArray effects = sidecar_.value("effects").toArray(); const auto value = effects.takeAt(row); effects.insert(target, value); sidecar_.insert("effects", effects);
        list_->setCurrentRow(target); dirty_ = true;
    }

    void toggleBypass() {
        const int row = list_->currentRow(); if (row < 0) return;
        auto *item = list_->item(row); const bool bypass = item->checkState() != Qt::Checked; item->setCheckState(bypass ? Qt::Checked : Qt::Unchecked);
        QJsonArray effects = sidecar_.value("effects").toArray(); auto object = effects.at(row).toObject(); object.insert("bypass", bypass); effects.replace(row, object); sidecar_.insert("effects", effects);
        dirty_ = true;
    }

    QJsonObject readParameters() const {
        QJsonObject result;
        for (int row = 0; row < parameters_->rowCount(); ++row) {
            const auto *key = parameters_->item(row, 0), *value = parameters_->item(row, 1);
            if (key && value && !key->text().trimmed().isEmpty()) result.insert(key->text().trimmed(), value->text());
        }
        return result;
    }

    void syncSelected() {
        const int row = list_->currentRow(); if (row < 0) return;
        QJsonArray effects = sidecar_.value("effects").toArray(); if (row >= effects.size()) return;
        auto object = effects.at(row).toObject(); object.insert("plugin_path", pluginPath_->text()); object.insert("class_uid", classUid_->text()); object.insert("parameters", readParameters()); object.insert("bypass", list_->item(row)->checkState() == Qt::Checked); effects.replace(row, object); sidecar_.insert("effects", effects);
    }

    void choosePlugin() {
        const QString path = chooseVst3Path(this, QStringLiteral("重新选择 VST3 插件"));
        if (!path.isEmpty()) { pluginPath_->setText(path); updateSelectedMetadata(); }
    }

    void addParameter() {
        const int row = parameters_->rowCount(); parameters_->insertRow(row); parameters_->setItem(row, 0, new QTableWidgetItem(QStringLiteral("param_%1").arg(row))); parameters_->setItem(row, 1, new QTableWidgetItem(QStringLiteral("0"))); dirty_ = true;
    }

    void removeParameter() {
        const int row = parameters_->currentRow(); if (row >= 0) { parameters_->removeRow(row); dirty_ = true; }
    }

    void saveChain() {
        syncSelected(); sidecar_.insert("score_id", scoreId_->text()); sidecar_.insert("track", track_->value()); sidecar_.insert("bus", bus_->text());
        if (!state::writeChain(sidecar_)) { status_->setText(QStringLiteral("保存失败：%1").arg(state::sidecarPath())); return; }
        dirty_ = false; status_->setText(QStringLiteral("已保存：%1").arg(state::sidecarPath()));
    }

    void closeEvent(QCloseEvent *event) override { if (dirty_) saveChain(); QWidget::closeEvent(event); }

    QLineEdit *search_ = nullptr, *pluginPath_ = nullptr, *classUid_ = nullptr, *scoreId_ = nullptr, *bus_ = nullptr;
    QListWidget *list_ = nullptr; QSpinBox *track_ = nullptr; QLabel *missing_ = nullptr, *status_ = nullptr;
    QTableWidget *parameters_ = nullptr; QPushButton *add_ = nullptr, *remove_ = nullptr, *up_ = nullptr, *down_ = nullptr, *bypass_ = nullptr, *addParameter_ = nullptr, *removeParameter_ = nullptr;
    QJsonObject sidecar_; bool dirty_ = false;
};

} // namespace

const char *state() noexcept { return "panel_ready_p5"; }

void showEffectChainPanel() {
    if (!qApp || qApp->property("gpvst3P5Panel").value<QWidget *>()) return;
    auto *panel = new ChainPanel;
    qApp->setProperty("gpvst3P5Panel", QVariant::fromValue(static_cast<QWidget *>(panel)));
    QObject::connect(panel, &QObject::destroyed, qApp, [] { qApp->setProperty("gpvst3P5Panel", QVariant()); });

    // GP exposes no supported insertion ABI for the Sound inspector. When
    // its main window is available, host the same editor in a right-side dock
    // and add a menu entry; otherwise keep the tool window usable standalone.
    auto *timer = new QTimer(qApp);
    timer->setInterval(200);
    QObject::connect(panel, &QObject::destroyed, timer, [timer] {
        timer->stop();
        timer->deleteLater();
    });
    QObject::connect(timer, &QTimer::timeout, timer, [timer, panel] {
        bool soundEntryReady = timer->property("soundEntryReady").toBool();
        bool dockReady = timer->property("dockReady").toBool();
        for (QWidget *widget : QApplication::allWidgets()) {
            if (widget->objectName() != QStringLiteral("soundsContainer") || !widget->layout()) continue;
            if (!widget->findChild<QPushButton *>("gpvst3SoundEffectChainButton")) {
                auto *button = new QPushButton(QStringLiteral("VST3 效果器链"), widget);
                button->setObjectName(QStringLiteral("gpvst3SoundEffectChainButton"));
                widget->layout()->addWidget(button);
                QObject::connect(button, &QPushButton::clicked, button, [panel] {
                    panel->show(); panel->raise(); panel->activateWindow();
                });
                soundEntryReady = true;
            }
            break;
        }
        for (QWidget *widget : QApplication::topLevelWidgets()) {
            if (QByteArray(widget->metaObject()->className()) != "gp::gui::MainWindow") continue;
            auto *window = qobject_cast<QMainWindow *>(widget);
            if (!window) continue;
            auto *dock = window->findChild<QDockWidget *>("gpvst3EffectChainDock");
            if (!dock) {
                dock = new QDockWidget(QStringLiteral("VST3 效果器链"), window);
                dock->setObjectName(QStringLiteral("gpvst3EffectChainDock"));
                dock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
                dock->setWidget(panel);
                window->addDockWidget(Qt::RightDockWidgetArea, dock);
                auto *action = window->menuBar()->addAction(QStringLiteral("VST3 效果器链"));
                action->setObjectName(QStringLiteral("gpvst3EffectChainAction"));
                QObject::connect(action, &QAction::triggered, dock, [dock] {
                    dock->setVisible(!dock->isVisible());
                });
                QObject::connect(dock, &QObject::destroyed, qApp, [] { qApp->setProperty("gpvst3P5Panel", QVariant()); });
                dock->show();
            }
            dockReady = true;
        }
        if (!panel->parentWidget()) {
            panel->show(); panel->raise(); panel->activateWindow();
        }
        timer->setProperty("soundEntryReady", soundEntryReady);
        timer->setProperty("dockReady", dockReady);
        if (soundEntryReady && dockReady) timer->deleteLater();
    });
    timer->start();
}

}
