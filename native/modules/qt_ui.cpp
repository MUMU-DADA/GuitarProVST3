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
#include <QtWidgets/QCheckBox>
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

RealtimeBypassControl g_realtimeBypassControl = nullptr;
QJsonArray g_vst3Catalog;

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
        if (list_->count()) list_->setCurrentRow(0);
        syncRealtimeBypass();
    }

    void syncRealtimeBypass() {
        if (!g_realtimeBypassControl || list_->count() == 0) return;
        bool allBypassed = true;
        for (int row = 0; row < list_->count(); ++row)
            allBypassed = allBypassed && list_->item(row)->checkState() == Qt::Checked;
        g_realtimeBypassControl(allBypassed);
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
        list_->setCurrentItem(item); syncRealtimeBypass(); dirty_ = true;
    }

    void removeEffect() {
        const int row = list_->currentRow(); if (row < 0) return;
        delete list_->takeItem(row);
        QJsonArray effects = sidecar_.value("effects").toArray(); effects.removeAt(row); sidecar_.insert("effects", effects);
        dirty_ = true; if (list_->count()) list_->setCurrentRow((std::min)(row, list_->count() - 1));
        if (list_->count()) syncRealtimeBypass();
        else if (g_realtimeBypassControl) g_realtimeBypassControl(true);
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
        syncRealtimeBypass();
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

class P7Panel final : public QWidget {
public:
    P7Panel() {
        setAttribute(Qt::WA_DeleteOnClose);
        setWindowFlag(Qt::Tool);
        setWindowTitle(QStringLiteral("音源 · VST3 效果器"));
        resize(420, 360);
        auto *root = new QVBoxLayout(this);
        root->addWidget(new QLabel(QStringLiteral("VST3 效果器"), this));
        list_ = new QListWidget(this);
        list_->setSelectionMode(QAbstractItemView::NoSelection);
        root->addWidget(list_, 1);
        status_ = new QLabel(this);
        status_->setWordWrap(true);
        root->addWidget(status_);
        loadChain();
    }

private:
    static QString key(const QJsonObject &effect) {
        return effect.value("module").toString() + QStringLiteral("\n") +
               effect.value("class_id").toString();
    }

    static QString displayName(const QJsonObject &entry, const QHash<QString, int> &counts) {
        const auto name = entry.value("name").toString();
        return counts.value(name) > 1
                   ? QStringLiteral("%1 (%2)").arg(name, entry.value("vendor").toString())
                   : name;
    }

    void loadChain() {
        QString error;
        if (!state::loadChain(sidecar_, &error))
            status_->setText(QStringLiteral("状态恢复失败：%1").arg(error));
        auto saved = sidecar_.value("effects").toArray();
        QHash<QString, int> savedRows;
        for (int index = 0; index < saved.size(); ++index) {
            const auto effect = saved.at(index).toObject();
            if (!effect.value("module").toString().isEmpty()) savedRows.insert(key(effect), index);
        }

        QHash<QString, int> counts;
        for (const auto &value : g_vst3Catalog) {
            const auto entry = value.toObject();
            if (entry.value("compatible").toBool())
                counts[entry.value("name").toString()]++;
        }
        QSet<QString> listed;
        for (const auto &value : g_vst3Catalog) {
            const auto entry = value.toObject();
            if (!entry.value("compatible").toBool()) continue;
            QJsonObject effect = savedRows.contains(entry.value("module").toString() +
                                                   QStringLiteral("\n") + entry.value("class_id").toString())
                                     ? saved.at(savedRows.value(entry.value("module").toString() +
                                                                QStringLiteral("\n") + entry.value("class_id").toString())).toObject()
                                     : QJsonObject{};
            effect.insert("module", entry.value("module"));
            effect.insert("class_id", entry.value("class_id"));
            effect.insert("name", entry.value("name"));
            effect.insert("vendor", entry.value("vendor"));
            if (!effect.contains("enabled")) effect.insert("enabled", false);
            appendRow(effect, displayName(entry, counts));
            listed.insert(entry.value("module").toString() + QStringLiteral("\n") + entry.value("class_id").toString());
        }
        // Keep missing or previously discovered entries in the sidecar visible,
        // but never enable them implicitly after a failed scan.
        for (const auto &value : saved) {
            const auto effect = value.toObject();
            if (effect.value("module").toString().isEmpty() || listed.contains(key(effect))) continue;
            auto missing = effect;
            missing.insert("enabled", false);
            appendRow(missing, effect.value("name").toString(QStringLiteral("缺失插件")));
        }
        sidecar_.insert("effects", effects_);
        if (list_->count() == 0)
            status_->setText(QStringLiteral("未发现可用的 x64 VST3 audio effect。"));
    }

    void appendRow(const QJsonObject &effect, const QString &label) {
        const int index = effects_.size();
        effects_.append(effect);
        auto *item = new QListWidgetItem(list_);
        auto *row = new QWidget(list_);
        auto *layout = new QHBoxLayout(row);
        layout->setContentsMargins(2, 2, 2, 2);
        auto *check = new QCheckBox(row);
        check->setChecked(effect.value("enabled").toBool());
        auto *name = new QPushButton(label, row);
        name->setFlat(true);
        name->setCursor(Qt::PointingHandCursor);
        layout->addWidget(check);
        layout->addWidget(name, 1);
        item->setSizeHint(row->sizeHint());
        list_->setItemWidget(item, row);
        connect(check, &QCheckBox::toggled, this, [this, index](bool enabled) {
            auto effect = effects_.at(index).toObject();
            effect.insert("enabled", enabled);
            effect.insert("bypass", !enabled);
            effects_.replace(index, effect);
            sidecar_.insert("effects", effects_);
            if (!state::writeChain(sidecar_))
                status_->setText(QStringLiteral("状态保存失败：%1").arg(state::sidecarPath()));
            else
                status_->setText(enabled ? QStringLiteral("已启用：点击名称打开原生 GUI")
                                          : QStringLiteral("已停用：%1").arg(effect.value("name").toString()));
        });
        connect(name, &QPushButton::clicked, this, [this, index] { openEditor(index); });
    }

    void openEditor(int index) {
        const auto effect = effects_.at(index).toObject();
        if (!effect.value("enabled").toBool()) {
            status_->setText(QStringLiteral("请先勾选启用插件，再打开其 GUI。"));
            return;
        }
        // The locked GP build has no verified HWND/IPlugView insertion ABI.
        // Keep the click target and report the boundary instead of showing a
        // project-owned parameter editor.
        status_->setText(QStringLiteral("原生 GUI 暂不可用：Guitar Pro 私有 IPlugView/HWND ABI 未验证（host_limited）。"));
    }

    void closeEvent(QCloseEvent *event) override {
        sidecar_.insert("effects", effects_);
        state::writeChain(sidecar_);
        QWidget::closeEvent(event);
    }

    QListWidget *list_ = nullptr;
    QLabel *status_ = nullptr;
    QJsonObject sidecar_;
    QJsonArray effects_;
};

} // namespace

const char *state() noexcept { return "panel_ready_p7"; }

void setRealtimeBypassControl(RealtimeBypassControl control) noexcept {
    g_realtimeBypassControl = control;
}

void setVst3Catalog(const QJsonArray &catalog) { g_vst3Catalog = catalog; }

void showEffectChainPanel() {
    if (!qApp || qApp->property("gpvst3P5Panel").value<QWidget *>()) return;
    QJsonObject chain;
    state::loadChain(chain);
    bool legacy = false;
    for (const auto &value : chain.value("effects").toArray()) {
        if (value.toObject().contains("plugin_path")) { legacy = true; break; }
    }
    // Keep the old isolated P5 fixture readable when no catalog is supplied;
    // a real P7 host always has a catalog and therefore uses the two-state UI.
    legacy = legacy && g_vst3Catalog.isEmpty();
    QWidget *panel = legacy ? static_cast<QWidget *>(new ChainPanel)
                            : static_cast<QWidget *>(new P7Panel);
    const bool useP7Panel = !legacy;
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
    QObject::connect(timer, &QTimer::timeout, timer, [timer, panel, useP7Panel] {
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
        if (!useP7Panel) {
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
                    QObject::connect(dock, &QObject::destroyed, qApp, [] {
                        qApp->setProperty("gpvst3P5Panel", QVariant());
                    });
                    dock->show();
                }
                dockReady = true;
            }
        }
        if (!panel->parentWidget()) {
            panel->show(); panel->raise(); panel->activateWindow();
        }
        timer->setProperty("soundEntryReady", soundEntryReady);
        timer->setProperty("dockReady", dockReady);
        if (soundEntryReady) timer->deleteLater();
    });
    timer->start();
}

}
