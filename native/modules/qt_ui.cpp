#include "qt_ui.h"

#include "state_manager.h"

#include <algorithm>
#include <QtCore/QFileInfo>
#include <QtCore/QFile>
#include <QtCore/QCryptographicHash>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QVariant>
#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtCore/QSignalBlocker>
#include <functional>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QAbstractButton>
#include <QtWidgets/QBoxLayout>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtGui/QCloseEvent>
#include <QtGui/QFontMetrics>
#include <QtGui/QMouseEvent>
#include <QtGui/QDesktopServices>
#include <QtCore/QUrl>
#include <QtGui/QResizeEvent>
#include <QtWidgets/QAction>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QFrame>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QDockWidget>
#include <QtWidgets/QDialog>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QMenuBar>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>
#include <QtWidgets/QSizePolicy>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QToolBar>

namespace gpvst3::ui {
namespace {

RealtimeBypassControl g_realtimeBypassControl = nullptr;
Vst3SelectionControl g_vst3SelectionControl = nullptr;
Vst3SelectionRequestControl g_vst3SelectionRequestControl = nullptr;
Vst3TrackSelectionControl g_vst3TrackSelectionControl = nullptr;
Vst3TrackSelectionRequestControl g_vst3TrackSelectionRequestControl = nullptr;
Vst3StateControl g_vst3StateControl = nullptr;
Vst3TrackStateControl g_vst3TrackStateControl = nullptr;
Vst3TrackEditorControl g_vst3TrackEditorControl = nullptr;
Vst3EditorControl g_vst3EditorControl = nullptr;
Vst3EditorCloseControl g_vst3EditorCloseControl = nullptr;
Vst3EditorScaleControl g_vst3EditorScaleControl = nullptr;
Vst3RefreshControl g_refreshControl = nullptr;
Vst3IdentifyControl g_identifyControl = nullptr;
QJsonArray g_vst3Catalog;
QString g_scanButtonText = QStringLiteral("VST3");
QString g_scanMessage;
QString g_scanDetail;
void saveCurrentRuntimeState();
QString g_vst3ScanState = QStringLiteral("pending");
class P7Panel;
P7Panel *g_p7Panel = nullptr;
P7Panel *g_globalPanel = nullptr;
QPointer<QTimer> g_panelAttachTimer;
QPointer<QDialog> g_aboutDialog;
bool g_panelUsesP7 = true;
bool g_trackExpanded = true, g_globalExpanded = true;

constexpr int kPathRole = Qt::UserRole;
constexpr int kUidRole = Qt::UserRole + 1;

bool trackContextAvailable() {
    return state::runtimeTrackContextAvailable() ||
           !qEnvironmentVariable("GPVST3_TRACK").isEmpty();
}

int configuredTrackIndex() {
    const auto runtimeIndex = state::runtimeTrackIndex();
    if (runtimeIndex >= 0) return runtimeIndex;
    bool ok = false;
    const auto value = qEnvironmentVariable("GPVST3_TRACK").toInt(&ok);
    return ok ? value : -1;
}

QString configuredTrackLabel() {
    const auto index = configuredTrackIndex();
    return index < 0 ? QStringLiteral("当前音轨：无法确认当前音轨")
                     : QStringLiteral("当前音轨：Track %1").arg(index);
}

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

class NativeEditorWindow final : public QWidget {
public:
    explicit NativeEditorWindow(QWidget *owner) : QWidget(owner, Qt::Window | Qt::WindowTitleHint |
            Qt::WindowSystemMenuHint | Qt::WindowCloseButtonHint) {
        setObjectName(QStringLiteral("gpvst3NativeEditorWindow"));
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        host = new QWidget(this);
        host->setAttribute(Qt::WA_NativeWindow);
        host->setObjectName(QStringLiteral("gpvst3NativeEditorHost"));
        layout->addWidget(host);
        resize(420, 260);
    }
    bool event(QEvent *event) override {
        const bool result = QWidget::event(event);
        if (event->type() == QEvent::ScreenChangeInternal && host) QTimer::singleShot(0, this, [this] {
            if (g_vst3EditorScaleControl) g_vst3EditorScaleControl(reinterpret_cast<void *>(host->winId()), host->devicePixelRatioF());
        });
        return result;
    }
    QWidget *host = nullptr;
    QString openedKey;
    ~NativeEditorWindow() override {
        if (g_vst3EditorCloseControl) g_vst3EditorCloseControl();
    }
    void closeEvent(QCloseEvent *event) override {
        saveCurrentRuntimeState();
        if (g_vst3EditorCloseControl) g_vst3EditorCloseControl();
        openedKey.clear();
        QWidget::closeEvent(event);
    }
};
QPointer<NativeEditorWindow> g_editorWindow;

NativeEditorWindow *editorWindow() {
    if (g_editorWindow) return g_editorWindow;
    QWidget *owner = nullptr;
    for (auto *widget : QApplication::topLevelWidgets()) {
        if (QByteArray(widget->metaObject()->className()) == "gp::gui::MainWindow") { owner = widget; break; }
    }
    g_editorWindow = new NativeEditorWindow(owner);
    return g_editorWindow;
}

QMainWindow *mainWindow() {
    for (auto *widget : QApplication::topLevelWidgets()) {
        if (auto *window = qobject_cast<QMainWindow *>(widget)) {
            if (QByteArray(widget->metaObject()->className()) == "gp::gui::MainWindow") return window;
        }
    }
    for (auto *widget : QApplication::topLevelWidgets())
        if (auto *window = qobject_cast<QMainWindow *>(widget)) return window;
    return nullptr;
}

QDialog *aboutDialog() {
    if (g_aboutDialog) return g_aboutDialog;
    auto *owner = mainWindow();
    auto *dialog = new QDialog(owner);
    dialog->setObjectName(QStringLiteral("gpvst3AboutDialog"));
    dialog->setWindowTitle(QStringLiteral("关于 GuitarProVST3"));
    dialog->setModal(false);
    dialog->setAttribute(Qt::WA_DeleteOnClose, false);
    dialog->resize(420, 300);
    auto *layout = new QVBoxLayout(dialog);
    auto *title = new QLabel(QStringLiteral("GuitarProVST3"), dialog);
    QFont titleFont = title->font();
    titleFont.setPointSize(titleFont.pointSize() + 2);
    titleFont.setBold(true);
    title->setFont(titleFont);
    layout->addWidget(title);
    auto *details = new QLabel(
        QStringLiteral("版本：0.9.0\n"
                       "已验证宿主：Guitar Pro 8.1.1.17（Windows x64）\n"
                       "许可证：MIT License\n"
                       "第三方声明：VST3 SDK 及插件各自遵循其许可证。\n"
                       "诊断数据目录：%1")
            .arg(state::dataDirectory()), dialog);
    details->setObjectName(QStringLiteral("gpvst3AboutDetails"));
    details->setWordWrap(true);
    layout->addWidget(details);
    auto *enabled = new QCheckBox(QStringLiteral("启动时启用插件"), dialog);
    enabled->setObjectName(QStringLiteral("gpvst3PluginEnabledCheckBox"));
    enabled->setChecked(state::pluginEnabled());
    enabled->setToolTip(QStringLiteral("修改后重启 Guitar Pro 生效"));
    layout->addWidget(enabled);
    auto *openConfig = new QPushButton(QStringLiteral("打开配置"), dialog);
    openConfig->setObjectName(QStringLiteral("gpvst3OpenConfigButton"));
    openConfig->setToolTip(state::settingsPath());
    layout->addWidget(openConfig, 0, Qt::AlignLeft);
    QObject::connect(enabled, &QCheckBox::toggled, dialog, [enabled](bool checked) {
        if (state::setPluginEnabled(checked)) return;
        const QSignalBlocker blocker(enabled);
        enabled->setChecked(!checked);
        enabled->setToolTip(QStringLiteral("配置保存失败：请检查诊断数据目录的写入权限。"));
    });
    QObject::connect(openConfig, &QPushButton::clicked, dialog, [] {
        if (!QFile::exists(state::settingsPath())) state::setPluginEnabled(state::pluginEnabled());
        QDesktopServices::openUrl(QUrl::fromLocalFile(state::settingsPath()));
    });
    layout->addStretch(1);
    auto *close = new QPushButton(QStringLiteral("关闭"), dialog);
    close->setObjectName(QStringLiteral("gpvst3AboutCloseButton"));
    close->setDefault(true);
    layout->addWidget(close, 0, Qt::AlignRight);
    QObject::connect(close, &QPushButton::clicked, dialog, &QDialog::hide);
    g_aboutDialog = dialog;
    if (qApp) qApp->setProperty("gpvst3AboutDialog", QVariant::fromValue(static_cast<QWidget *>(dialog)));
    QObject::connect(dialog, &QObject::destroyed, qApp, [] {
        g_aboutDialog = nullptr;
        if (qApp) qApp->setProperty("gpvst3AboutDialog", QVariant());
    });
    return dialog;
}

void showAboutDialog() {
    auto *dialog = aboutDialog();
    if (!dialog) return;
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

QToolBar *findTitleToolBar(QMainWindow *window) {
    if (!window) return nullptr;
    const QStringList names{QStringLiteral("gpvst3TitleToolBar"), QStringLiteral("titleToolBar"),
                            QStringLiteral("gpTitleToolBar"), QStringLiteral("mainToolBar"),
                            QStringLiteral("toolBar")};
    for (const auto &name : names)
        if (auto *bar = window->findChild<QToolBar *>(name)) return bar;
    for (auto *bar : window->findChildren<QToolBar *>()) {
        const auto object = bar->objectName().toLower();
        if (object.contains(QStringLiteral("title")) || object.contains(QStringLiteral("header"))) return bar;
    }
    const auto bars = window->findChildren<QToolBar *>();
    return bars.isEmpty() ? nullptr : bars.front();
}

void ensureAboutEntry() {
    auto *window = mainWindow();
    if (!window) return;
    if (auto *bar = findTitleToolBar(window)) {
        if (!bar->findChild<QPushButton *>(QStringLiteral("gpvst3AboutButton"))) {
            auto *button = new QPushButton(QStringLiteral("关于"), bar);
            button->setObjectName(QStringLiteral("gpvst3AboutButton"));
            button->setToolTip(QStringLiteral("关于 GuitarProVST3"));
            button->setAccessibleName(QStringLiteral("关于 GuitarProVST3"));
            bar->addWidget(button);
            QObject::connect(button, &QPushButton::clicked, button, [] { showAboutDialog(); });
        }
        if (auto *stale = window->findChild<QAction *>(QStringLiteral("gpvst3AboutAction"))) {
            window->menuBar()->removeAction(stale);
            stale->deleteLater();
        }
        window->setProperty("gpvst3AboutMount", "title_toolbar");
        return;
    }
    if (!window->menuBar()) return;
    auto *action = window->findChild<QAction *>(QStringLiteral("gpvst3AboutAction"));
    if (!action) {
        action = window->menuBar()->addAction(QStringLiteral("关于 GuitarProVST3"));
        action->setObjectName(QStringLiteral("gpvst3AboutAction"));
        QObject::connect(action, &QAction::triggered, action, [] { showAboutDialog(); });
    }
    window->setProperty("gpvst3AboutMount", "menu_fallback");
}

class ElidedButton final : public QPushButton {
public:
    explicit ElidedButton(QWidget *parent = nullptr) : QPushButton(parent) {
        setFlat(true);
        setCursor(Qt::PointingHandCursor);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        setMinimumWidth(24);
        setStyleSheet(QStringLiteral("text-align: left; padding-left: 2px; padding-right: 2px;"));
    }

    void setFullText(const QString &value) {
        fullText_ = value;
        setToolTip(value);
        updateElidedText();
    }

    std::function<void()> onDoubleClick;

protected:
    void resizeEvent(QResizeEvent *event) override {
        QPushButton::resizeEvent(event);
        updateElidedText();
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override {
        if (event->button() == Qt::LeftButton && onDoubleClick) onDoubleClick();
        QPushButton::mouseDoubleClickEvent(event);
    }

private:
    void updateElidedText() {
        const auto width = qMax(1, this->width() - 8);
        setText(QFontMetrics(font()).elidedText(fullText_, Qt::ElideRight, width));
    }
    QString fullText_;
};

class P7Panel final : public QWidget {
public:
    explicit P7Panel(state::ScopeKind scope = state::ScopeKind::Track) : scope_(scope) {
        setObjectName(scope == state::ScopeKind::Track ? QStringLiteral("gpvst3P7Panel")
                                                       : QStringLiteral("gpvst3GlobalPanel"));
        setProperty("gpvst3Scope", scope == state::ScopeKind::Track ? "track" : "global");
        setWindowTitle(QStringLiteral("音源 · VST3 效果器"));
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
        setMinimumWidth(0);
        auto *root = new QVBoxLayout(this);
        root->setContentsMargins(0, 0, 0, 0);
        auto *heading = new QHBoxLayout;
        trackContext_ = new QLabel(scope == state::ScopeKind::Track ? configuredTrackLabel()
            : QStringLiteral("全局 Master"), this);
        trackContext_->setObjectName(QStringLiteral("gpvst3TrackContext"));
        heading->addWidget(trackContext_, 1);
        auto *close = new QPushButton(QStringLiteral("×"), this);
        close->setObjectName(scope == state::ScopeKind::Track ? QStringLiteral("gpvst3CloseSelectorButton")
                                                            : QStringLiteral("gpvst3GlobalCloseSelectorButton"));
        close->setToolTip(QStringLiteral("关闭选择区"));
        close->setFixedWidth(24);
        heading->addWidget(close);
        connect(close, &QPushButton::clicked, this, &QWidget::close);
        root->addLayout(heading);
        root->addWidget(new QLabel(QStringLiteral("正在使用（从上到下为效果顺序）"), this));
        list_ = new QListWidget(this);
        list_->setObjectName(scope == state::ScopeKind::Track ? QStringLiteral("gpvst3TrackChainList")
                                                             : QStringLiteral("gpvst3GlobalChainList"));
        list_->setSelectionMode(QAbstractItemView::SingleSelection);
        list_->setDragDropMode(QAbstractItemView::InternalMove);
        list_->setDefaultDropAction(Qt::MoveAction);
        list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        list_->setStyleSheet(QStringLiteral("QListWidget { padding:0; margin:0; } QListWidget::item { padding:0; margin:0; }"));
        list_->setMinimumWidth(0);
        list_->setMaximumHeight(150);
        root->addWidget(list_);
        root->addWidget(new QLabel(QStringLiteral("可用插件"), this));
        availableList_ = new QListWidget(this);
        availableList_->setObjectName(scope == state::ScopeKind::Track ? QStringLiteral("gpvst3AvailableList")
                                                                      : QStringLiteral("gpvst3GlobalAvailableList"));
        availableList_->setSelectionMode(QAbstractItemView::NoSelection);
        availableList_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        availableList_->setStyleSheet(list_->styleSheet());
        availableList_->setMinimumWidth(0);
        availableList_->setMaximumHeight(120);
        root->addWidget(availableList_);
        status_ = new QLabel(this);
        status_->setObjectName(scope == state::ScopeKind::Track ? QStringLiteral("gpvst3Status")
                                                               : QStringLiteral("gpvst3GlobalStatus"));
        status_->setWordWrap(true);
        root->addWidget(status_);
        connect(list_->model(), &QAbstractItemModel::rowsMoved, this,
                [this] { syncOrderFromList(); });
        list_->setContextMenuPolicy(Qt::ActionsContextMenu);
        for (const int direction : {-1, 1}) {
            auto *action = new QAction(direction < 0 ? QStringLiteral("上移效果器") : QStringLiteral("下移效果器"), list_);
            const auto prefix = scope_ == state::ScopeKind::Track ? QStringLiteral("gpvst3") : QStringLiteral("gpvst3Global");
            action->setObjectName(prefix + (direction < 0 ? "MoveUp" : "MoveDown"));
            action->setShortcut(QKeySequence(Qt::ALT | (direction < 0 ? Qt::Key_Up : Qt::Key_Down)));
            action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
            list_->addAction(action);
            connect(action, &QAction::triggered, this, [this, direction] {
                const int row = list_->currentRow(), target = row + direction;
                if (row < 0 || target < 0 || target >= list_->count()) return;
                if (list_->model()->moveRow({}, row, {}, direction > 0 ? target + 1 : target)) list_->setCurrentRow(target);
            });
        }
        loadChain();
        (scope == state::ScopeKind::Track ? g_p7Panel : g_globalPanel) = this;
    }

    ~P7Panel() override {
        saveRuntimeState();
        if (g_p7Panel == this) g_p7Panel = nullptr;
        if (g_globalPanel == this) g_globalPanel = nullptr;
    }

    void refreshCatalog() {
        saveRuntimeState();
        loadChain();
        if (scope_ == state::ScopeKind::Global || contextReady_) restoreSelection();
    }

    void reloadSavedSelection() {
        loadChain();
        setProperty("gpvst3SelectionState", "applied");
    }

    void refreshTrackContext() {
        if (scope_ == state::ScopeKind::Global) return;
        const auto nextKey = state::currentTrackKey();
        const auto changed = nextKey != trackKey_ || trackContextAvailable() != contextReady_;
        if (trackContext_) trackContext_->setText(configuredTrackLabel());
        if (!changed) return;
        saveRuntimeState();
        if (scope_ == state::ScopeKind::Track) {
            loadChain();
            if (contextReady_) restoreSelection();
        } else {
            bindTrackContext();
        }
    }

    void capture() { saveRuntimeState(); }
    void scanFeedback() {
        status_->setText(g_scanMessage);
        // Scan diagnostics stay in status.json/logs. Product UI keeps a
        // neutral tooltip so module paths and error codes never leak into a
        // row or a hover card.
        status_->setToolTip(QStringLiteral("VST3 插件清单状态"));
        if (g_scanMessage.isEmpty() && activeList()->count() == 0 && activeAvailableList()->count() == 0)
            status_->setText(QStringLiteral("未发现可用的 VST3 效果器。"));
    }

    void syncSelection() {
        bool anyEnabled = false;
        for (const auto &value : effects_)
            anyEnabled |= value.toObject().value("enabled").toBool();
        if (anyEnabled && (scope_ == state::ScopeKind::Global || contextReady_)) {
            selectionDirty_ = true;
            restoreSelection();
        }
    }

private:
    void restoreSelection() {
        if (publishSelection()) return;
        const auto error = status_->toolTip();
        const auto live = scope_ == state::ScopeKind::Track
            ? (g_vst3TrackStateControl ? g_vst3TrackStateControl(trackKey_.toStdString()) : std::vector<Vst3SelectionEntry>{})
            : (g_vst3StateControl ? g_vst3StateControl() : std::vector<Vst3SelectionEntry>{});
        for (int index = 0; index < effects_.size(); ++index) {
            auto effect = effects_[index].toObject();
            if (!effect.value("enabled").toBool() || !effect.value("identified").toBool()) continue;
            const bool active = std::any_of(live.begin(), live.end(), [&](const Vst3SelectionEntry &entry) {
                return entry.module == effect.value("module").toString().toStdString() &&
                       entry.classId == effect.value("class_id").toString().toStdString();
            });
            if (!active) {
                effect.insert("enabled", false);
                effect.insert("bypass", true);
                effect.insert("last_error", error);
                effects_[index] = effect;
            }
        }
        saveRuntimeState();
        loadChain();
    }

    static std::vector<unsigned char> stateBytes(const QJsonObject &effect, const char *field) {
        const auto bytes = QByteArray::fromBase64(effect.value(field).toString().toLatin1());
        return {bytes.begin(), bytes.end()};
    }

    bool publishSelection() {
        if (!g_vst3SelectionControl && !g_vst3TrackSelectionControl) return true; // isolated UI fixture
        std::vector<Vst3SelectionEntry> selection;
        for (const auto &value : effects_) {
            const auto effect = value.toObject();
            if (!effect.value("enabled").toBool() || !effect.value("identified").toBool()) continue;
            const auto module = effect.value("module").toString();
            const auto classId = effect.value("class_id").toString();
            if (module.isEmpty() || classId.isEmpty()) continue;
            selection.push_back({module.toStdString(), classId.toStdString(),
                                 stateBytes(effect, "component_state"), stateBytes(effect, "controller_state")});
        }
        selectionDirty_ = false;
        setProperty("gpvst3SelectionState", "requesting");
        std::string error;
        const bool accepted = scope_ == state::ScopeKind::Global
            ? (g_vst3SelectionRequestControl ? g_vst3SelectionRequestControl(selection, &error)
               : (g_vst3SelectionControl ? g_vst3SelectionControl(selection, &error) : true))
            : (g_vst3TrackSelectionRequestControl ? g_vst3TrackSelectionRequestControl(
                  trackKey_.toStdString(), selection, &error)
               : (g_vst3TrackSelectionControl ? g_vst3TrackSelectionControl(
                  trackKey_.toStdString(), selection, &error) : true));
        if (accepted) {
            setProperty("gpvst3SelectionState",
                        (g_vst3SelectionRequestControl || g_vst3TrackSelectionRequestControl)
                            ? "request_pending" : "applied");
            return true;
        }
        setProperty("gpvst3SelectionState", "failed_reverted");
        QString message = QStringLiteral("无法启用此插件：插件初始化失败。");
        if (error == "host_unsupported")
            message = QStringLiteral("当前 Guitar Pro 版本未通过兼容性校验，无法启用效果器。");
        else if (error == "realtime_disabled_by_environment")
            message = QStringLiteral("实时效果器已被启动配置禁用，请恢复默认配置后重启 Guitar Pro。");
        else if (error == "hook_install_failed" || error == "entry_points_not_found" ||
                 error == "buffer_accessors_not_found" || error == "gprse_not_loaded")
            message = QStringLiteral("无法接入 Guitar Pro 音频处理，请重启后重试。");
        else if (error == "runtime_vst3_not_found")
            message = QStringLiteral("找不到此插件，请重新安装插件后重启 Guitar Pro。");
        else if (error.rfind("runtime_vst3_state_restore_failed", 0) == 0)
            message = QStringLiteral("无法启用此插件：已保存的插件状态恢复失败。");
        else if (error == "runtime_vst3_chain_full")
            message = QStringLiteral("效果器链已满，请先停用其他插件。");
        status_->setText(message);
        status_->setToolTip(QString::fromStdString(error));
        return false;
    }

    void saveRuntimeState() {
        if (scope_ == state::ScopeKind::Track && !contextReady_) return;
        const auto runtimeStates = scope_ == state::ScopeKind::Track
            ? (g_vst3TrackStateControl ? g_vst3TrackStateControl(trackKey_.toStdString()) : std::vector<Vst3SelectionEntry>{})
            : (g_vst3StateControl ? g_vst3StateControl() : std::vector<Vst3SelectionEntry>{});
        for (const auto &saved : runtimeStates) {
            for (int i = 0; i < effects_.size(); ++i) {
                auto effect = effects_.at(i).toObject();
                if (effect.value("module").toString().toStdString() != saved.module ||
                    effect.value("class_id").toString().toStdString() != saved.classId) continue;
                effect.insert("component_state", QString::fromLatin1(QByteArray(
                    reinterpret_cast<const char *>(saved.componentState.data()),
                    static_cast<int>(saved.componentState.size())).toBase64()));
                effect.insert("controller_state", QString::fromLatin1(QByteArray(
                    reinterpret_cast<const char *>(saved.controllerState.data()),
                    static_cast<int>(saved.controllerState.size())).toBase64()));
                effects_.replace(i, effect);
            }
        }
        QJsonArray savedEffects;
        for (const auto &value : effects_)
            if (!value.toObject().value("module").toString().isEmpty() &&
                (!value.toObject().value("class_id").toString().isEmpty() ||
                 value.toObject().value("enabled").toBool())) savedEffects.append(value);
        state::loadChain(sidecar_);
        state::setScopeEffects(sidecar_, scope_, savedEffects, scoreKey_, trackKey_, trackIndex_, {});
        if (!state::writeChain(sidecar_) && status_)
            status_->setText(QStringLiteral("插件状态保存失败。"));
    }

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
        bindTrackContext();
        QString error;
        if (!state::loadChain(sidecar_, &error))
            status_->setText(QStringLiteral("状态恢复失败：%1").arg(error));
        auto saved = state::scopeEffects(sidecar_, scope_, scoreKey_, trackKey_);
        QHash<QString, int> savedRows;
        for (int index = 0; index < saved.size(); ++index) {
            const auto effect = saved.at(index).toObject();
            if (!effect.value("module").toString().isEmpty()) savedRows.insert(key(effect), index);
        }

        QHash<QString, int> counts;
        for (const auto &value : g_vst3Catalog) {
            const auto entry = value.toObject();
            if ((entry.value("identified").toBool() || entry.value("compatible").toBool()) &&
                !entry.value("class_id").toString().isEmpty() &&
                (entry.value("recognition_status").toString().isEmpty() ||
                 entry.value("recognition_status").toString() == QStringLiteral("ready")))
                counts[entry.value("name").toString()]++;
        }
        QSet<QString> listed;
        std::vector<QJsonValue> catalogEffects;
        QJsonArray preservedMissing;
        for (const auto &value : g_vst3Catalog) {
            const auto entry = value.toObject();
            const auto status = entry.value("recognition_status").toString();
            // Pending, failed and timed-out bundles remain in the cache for
            // diagnostics, but never become an interactive row in either
            // scope. A saved desired_enabled record is retained in sidecar
            // and will be reconciled when a later scan reaches ready.
            const bool identified = (entry.value("identified").toBool() || entry.value("compatible").toBool()) &&
                !entry.value("class_id").toString().isEmpty() &&
                (status.isEmpty() || status == QStringLiteral("ready"));
            if (!identified) continue;
            QJsonObject effect = savedRows.contains(entry.value("module").toString() +
                                                   QStringLiteral("\n") + entry.value("class_id").toString())
                                     ? saved.at(savedRows.value(entry.value("module").toString() +
                                                                QStringLiteral("\n") + entry.value("class_id").toString())).toObject()
                                     : QJsonObject{};
            if (effect.isEmpty()) for (const auto &savedValue : saved) {
                const auto pending = savedValue.toObject();
                if (pending.value("module") == entry.value("module") &&
                    pending.value("class_id").toString().isEmpty()) { effect = pending; break; }
            }
            effect.insert("module", entry.value("module"));
            effect.insert("class_id", entry.value("class_id"));
            effect.insert("name", entry.value("name"));
            effect.insert("vendor", entry.value("vendor"));
            effect.insert("identified", identified);
            if (!effect.contains("enabled")) effect.insert("enabled", false);
            catalogEffects.push_back(effect);
            listed.insert(entry.value("module").toString() + QStringLiteral("\n") + entry.value("class_id").toString());
        }
        // Saved identities and opaque state survive incomplete metadata and cache rebuilds.
        for (const auto &value : saved) {
            const auto effect = value.toObject();
            if (effect.value("module").toString().isEmpty() || effect.value("class_id").toString().isEmpty() ||
                listed.contains(key(effect))) continue;
            // Keep sidecar state opaque and ordered without exposing a stale
            // missing plug-in as a selectable entry.
            auto missing = effect;
            missing.insert("identified", false);
            preservedMissing.append(missing);
        }
        std::stable_sort(catalogEffects.begin(), catalogEffects.end(), [](const QJsonValue &a, const QJsonValue &b) {
            const auto left = a.toObject(), right = b.toObject();
            const bool le = left.value("enabled").toBool(), re = right.value("enabled").toBool();
            if (le != re) return le > re;
            if (le) return left.value("order").toInt() < right.value("order").toInt();
            const auto ln = left.value("name").toString(), rn = right.value("name").toString();
            return ln == rn ? left.value("vendor").toString() < right.value("vendor").toString() : ln < rn;
        });
        activeList()->clear();
        availableList_->clear();
        effects_ = {};
        for (const auto &value : catalogEffects) {
            const auto effect = value.toObject();
            const auto recognition = effect.value("recognition_status").toString();
            const auto suffix = effect.value("identified").toBool() ? QString{}
                : (recognition == QStringLiteral("failed") ? QStringLiteral(" · 识别失败")
                   : QStringLiteral(" · 后台识别中"));
            appendRow(effect, displayName(effect, counts) + suffix);
        }
        for (const auto &value : preservedMissing) effects_.append(value);
        const auto rowHeight = qMax(32, qRound(32 * devicePixelRatioF()));
        list_->setFixedHeight(qMin(150, qMax(1, list_->count()) * rowHeight + 4));
        availableList_->setFixedHeight(qMin(120, qMax(1, availableList_->count()) * rowHeight + 4));
        state::setScopeEffects(sidecar_, scope_, effects_, scoreKey_, trackKey_, trackIndex_);
        scanFeedback();
    }

    void appendRow(const QJsonObject &effect, const QString &label) {
        const int index = effects_.size();
        effects_.append(effect);
        auto *targetList = effect.value("enabled").toBool() ? activeList() : activeAvailableList();
        auto *item = new QListWidgetItem(targetList);
        auto *row = new QWidget(targetList);
        row->setMinimumHeight(qMax(32, qRound(32 * devicePixelRatioF())));
        auto *layout = new QHBoxLayout(row);
        layout->setContentsMargins(2, 1, 2, 1);
        layout->setSpacing(3);
        auto *check = new QCheckBox(row);
        const auto token = effect.value("class_id").toString();
        const auto prefix = scope_ == state::ScopeKind::Track ? QStringLiteral("gpvst3")
                                                              : QStringLiteral("gpvst3Global");
        row->setObjectName(prefix + QStringLiteral("EffectRow_") + token);
        check->setObjectName(prefix + QStringLiteral("Enabled_") + token);
        check->setFixedWidth(20);
        const bool trackContextReady = scope_ != state::ScopeKind::Track ||
            (trackContextAvailable() && g_vst3TrackSelectionControl);
        check->setEnabled(trackContextReady);
        check->setToolTip(trackContextReady ? QString{} :
            QStringLiteral("音轨效果器暂不可用：等待宿主音轨上下文（track_scope_unresolved）。"));
        check->setAccessibleName(QStringLiteral("启用 %1").arg(effect.value("name").toString()));
        check->setChecked(effect.value("enabled").toBool());
        auto *name = new ElidedButton(row);
        name->setObjectName(prefix + QStringLiteral("Name_") + token);
        name->setFullText(label);
        name->setAccessibleName(effect.value("name").toString());
        const auto fullIdentity = key(effect);
        name->setToolTip(QStringLiteral("%1\n厂商：%2\nentry_id：%3")
            .arg(effect.value("name").toString(), effect.value("vendor").toString(), fullIdentity));
        if (!effect.value("last_error").toString().isEmpty()) {
            // Keep the failure reason in the sidecar/structured diagnostics;
            // the compact row only offers a neutral retry affordance.
            row->setProperty("gpvst3Diagnostic", effect.value("last_error"));
            check->setToolTip(QStringLiteral("重新勾选可重试。"));
        }
        auto *vendor = new QLabel(effect.value("vendor").toString(), row);
        vendor->setObjectName(prefix + QStringLiteral("Vendor_") + token);
        vendor->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        vendor->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
        vendor->setToolTip(effect.value("vendor").toString());
        vendor->hide(); // Full vendor/identity remain available in the name tooltip.
        auto *handle = new QLabel(QStringLiteral("⋮⋮"), row);
        handle->setObjectName(prefix + QStringLiteral("DragHandle_") + token);
        handle->setAlignment(Qt::AlignCenter);
        handle->setFixedWidth(20);
        handle->setToolTip(QStringLiteral("拖动以调整处理顺序"));
        layout->addWidget(check);
        layout->addWidget(name, 1);
        layout->addWidget(vendor);
        layout->addWidget(handle);
        item->setSizeHint(QSize(0, row->minimumHeight()));
        targetList->setItemWidget(item, row);
        const auto identity = key(effect);
        row->setProperty("gpvst3EntryId", identity);
        connect(check, &QCheckBox::toggled, this, [this, identity, check](bool enabled) {
            saveRuntimeState();
            const int index = indexFor(identity); if (index < 0) return;
            auto effect = effects_.at(index).toObject();
            const auto previous = effect;
            const auto editorKey = (scope_ == state::ScopeKind::Global ? QStringLiteral("global") : trackKey_) + '\n' + key(effect);
            if (!enabled && g_editorWindow && g_editorWindow->openedKey == editorKey) g_editorWindow->close();
            effect.insert("enabled", enabled);
            effect.insert("bypass", !enabled);
            effect.insert("configured", true);
            effect.remove("last_error");
            effect.remove("desired_enabled");
            if (enabled) {
                int nextOrder = 0;
                for (const auto &value : effects_)
                    if (value.toObject().value("enabled").toBool())
                        nextOrder = qMax(nextOrder, value.toObject().value("order").toInt() + 1);
                effect.insert("order", nextOrder);
            }
            effects_.replace(index, effect);
            if (!publishSelection()) {
                effects_.replace(index, previous);
                const QSignalBlocker blocked(check);
                check->setChecked(previous.value("enabled").toBool());
                return;
            }
            saveRuntimeState();
            status_->setToolTip(QString());
            status_->setText(enabled
                ? (scope_ == state::ScopeKind::Track && !state::runtimeTrackContextAvailable()
                    ? QStringLiteral("已保存音轨链：等待宿主确认音轨上下文，当前保持旁路。")
                    : ((g_vst3SelectionRequestControl && scope_ == state::ScopeKind::Global) ||
                       (g_vst3TrackSelectionRequestControl && scope_ == state::ScopeKind::Track)
                        ? QStringLiteral("请求中：准备完成后自动生效。")
                        : QStringLiteral("已启用：双击名称打开原生 GUI")))
                : QStringLiteral("已停用：%1").arg(effect.value("name").toString()));
            // Rebuild after the current signal so enabled rows move immediately.
            QTimer::singleShot(0, this, [this] { loadChain(); });
        });
        name->onDoubleClick = [this, identity] {
            const int index = indexFor(identity); if (index >= 0) openEditor(index);
        };
    }

    void openEditor(int index) {
        const auto effect = effects_.at(index).toObject();
        if (!effect.value("enabled").toBool()) {
            status_->setText(QStringLiteral("请先勾选启用插件，再打开其 GUI。"));
            return;
        }
        auto *window = editorWindow();
        const auto editorKey = (scope_ == state::ScopeKind::Global ? QStringLiteral("global") : trackKey_) + '\n' + key(effect);
        if (window->openedKey == editorKey && window->isVisible()) {
            window->showNormal(); window->raise(); window->activateWindow();
            return;
        }
        saveRuntimeState();
        if (g_vst3EditorCloseControl) g_vst3EditorCloseControl();
        window->openedKey = editorKey;
        window->setWindowTitle(effect.value("name").toString() + QStringLiteral(" · VST3"));
        window->show();
        const gpvst3::hook::Vst3SelectionEntry selection{
            effect.value("module").toString().toStdString(),
            effect.value("class_id").toString().toStdString()};
        if (scope_ == state::ScopeKind::Global ? !g_vst3EditorControl : !g_vst3TrackEditorControl) {
            window->hide();
            status_->setText(QStringLiteral("原生 GUI 暂不可用：当前测试宿主未提供 IPlugView/HWND 桥接（host_limited）。"));
            return;
        }
        void *editorHost = reinterpret_cast<void *>(window->host->winId());
        const bool opened = scope_ == state::ScopeKind::Global
            ? g_vst3EditorControl(selection, editorHost)
            : g_vst3TrackEditorControl(trackKey_.toStdString(), selection, editorHost);
        if (!opened) {
            window->hide();
            status_->setText(QStringLiteral("原生 GUI 不可用：插件未提供可嵌入 editor 或初始化失败。"));
            return;
        }
        window->raise(); window->activateWindow();
        status_->setText(QStringLiteral("原生 GUI 已打开：%1").arg(effect.value("name").toString()));
    }

    void closeEvent(QCloseEvent *event) override {
        saveRuntimeState();
        (scope_ == state::ScopeKind::Track ? g_trackExpanded : g_globalExpanded) = false;
        QWidget::closeEvent(event);
    }

    QListWidget *activeList() const noexcept {
        return list_;
    }

    QListWidget *activeAvailableList() const noexcept {
        return availableList_;
    }

    int indexFor(const QString &identity) const {
        for (int i = 0; i < effects_.size(); ++i) if (key(effects_.at(i).toObject()) == identity) return i;
        return -1;
    }

    void syncOrderFromList() {
        const auto *target = activeList();
        QJsonArray ordered;
        for (int row = 0; row < target->count(); ++row) {
            auto *widget = target->itemWidget(target->item(row));
            const auto identity = widget ? widget->property("gpvst3EntryId").toString() : QString{};
            const auto index = indexFor(identity);
            if (index >= 0) ordered.append(effects_.at(index));
        }
        if (ordered.isEmpty() && !effects_.isEmpty()) return;
        QSet<QString> moved;
        for (const auto &value : ordered) moved.insert(key(value.toObject()));
        QJsonArray merged;
        for (int order = 0; order < ordered.size(); ++order) {
            auto effect = ordered.at(order).toObject();
            effect.insert("order", order);
            merged.append(effect);
        }
        int disabledOrder = ordered.size();
        for (const auto &value : effects_) {
            const auto effect = value.toObject();
            if (moved.contains(key(effect))) continue;
            auto disabled = effect;
            disabled.insert("order", disabledOrder++);
            merged.append(disabled);
        }
        effects_ = merged;
        saveRuntimeState();
        publishSelection();
    }

    QListWidget *list_ = nullptr, *availableList_ = nullptr;
    QLabel *trackContext_ = nullptr;
    QLabel *status_ = nullptr;
    QJsonObject sidecar_;
    QJsonArray effects_;
    const state::ScopeKind scope_;
    bool selectionDirty_ = false;
    QString scoreKey_, trackKey_;
    int trackIndex_ = -1;
    bool contextReady_ = false;

    void bindTrackContext() {
        scoreKey_ = state::currentScoreKey();
        trackKey_ = state::currentTrackKey();
        trackIndex_ = configuredTrackIndex();
        contextReady_ = trackContextAvailable();
    }
};

void saveCurrentRuntimeState() {
    if (g_p7Panel) g_p7Panel->capture();
    if (g_globalPanel) { g_globalPanel->capture(); return; }
    if (!g_vst3StateControl) return;
    QJsonObject chain;
    if (!state::loadChain(chain)) return;
    auto effects = chain.value("effects").toArray();
    for (const auto &saved : g_vst3StateControl()) for (int i = 0; i < effects.size(); ++i) {
        auto effect = effects.at(i).toObject();
        if (effect.value("module").toString().toStdString() != saved.module ||
            effect.value("class_id").toString().toStdString() != saved.classId) continue;
        effect.insert("component_state", QString::fromLatin1(QByteArray(reinterpret_cast<const char *>(saved.componentState.data()),
                       static_cast<int>(saved.componentState.size())).toBase64()));
        effect.insert("controller_state", QString::fromLatin1(QByteArray(reinterpret_cast<const char *>(saved.controllerState.data()),
                       static_cast<int>(saved.controllerState.size())).toBase64()));
        effects.replace(i, effect);
    }
    chain.insert("effects", effects);
    state::writeChain(chain);
}

QWidget *findSoundHost() {
    QWidget *named = nullptr;
    for (QWidget *widget : QApplication::allWidgets()) {
        if (widget->objectName() == QStringLiteral("soundsContainer") && widget->layout()) {
            named = widget;
            break;
        }
    }
    if (named) return named;

    // Some GP builds do not keep the old objectName. The visible RSE/MIDI
    // controls are more stable and identify the same audio section.
    for (QWidget *widget : QApplication::allWidgets()) {
        auto *control = qobject_cast<QAbstractButton *>(widget);
        if (!control || control->text().compare(QStringLiteral("RSE"), Qt::CaseInsensitive) != 0)
            continue;
        for (QWidget *candidate = control->parentWidget(); candidate;
             candidate = candidate->parentWidget()) {
            if (!candidate->layout()) continue;
            bool hasMidi = false;
            for (QAbstractButton *button : candidate->findChildren<QAbstractButton *>())
                hasMidi |= button->text().compare(QStringLiteral("MIDI"), Qt::CaseInsensitive) == 0;
            if (hasMidi) return candidate;
        }
    }
    return nullptr;
}

QWidget *findHostAnchor(QWidget *host, const QStringList &objectNames,
                        const QStringList &texts) {
    if (!host) return nullptr;
    const auto children = host->findChildren<QWidget *>();
    for (auto *widget : children) {
        if (objectNames.contains(widget->objectName())) return widget;
        if (auto *button = qobject_cast<QAbstractButton *>(widget)) {
            if (texts.contains(button->text(), Qt::CaseInsensitive)) return widget;
        }
        if (auto *label = qobject_cast<QLabel *>(widget)) {
            if (texts.contains(label->text(), Qt::CaseInsensitive)) return widget;
        }
    }
    return nullptr;
}

QWidget *findApplicationAnchor(const QStringList &objectNames, const QStringList &texts) {
    const auto children = QApplication::allWidgets();
    // Prefer an exact stable objectName over discovery order. Qt does not
    // guarantee the order returned by allWidgets(), and selecting a nested
    // title label before its containing effect chain can place our section at
    // the end of the page instead of immediately after the chain.
    for (const auto &name : objectNames) {
        if (name.isEmpty()) continue;
        for (auto *widget : children)
            if (widget->objectName() == name) return widget;
    }
    for (auto *widget : children) {
        if (auto *button = qobject_cast<QAbstractButton *>(widget))
            if (texts.contains(button->text(), Qt::CaseInsensitive)) return widget;
        if (auto *label = qobject_cast<QLabel *>(widget))
            if (texts.contains(label->text(), Qt::CaseInsensitive)) return widget;
    }
    return nullptr;
}

QLayout *layoutContaining(QWidget *host, QWidget *anchor) {
    if (!host || !anchor) return nullptr;
    if (auto *layout = host->layout(); layout && layout->indexOf(anchor) >= 0) return layout;
    for (auto *layout : host->findChildren<QLayout *>())
        if (layout->indexOf(anchor) >= 0) return layout;
    return nullptr;
}

QWidget *layoutHostFor(QWidget *anchor) {
    if (!anchor) return nullptr;
    // GP nests the score/track controls in layouts such as masteringLayout
    // and verticalLayout_2. Search those layouts before climbing to an outer
    // page; appending to the outer page would move our section past unrelated
    // controls while still leaving the native anchor untouched.
    for (auto *current = anchor->parentWidget(); current; current = current->parentWidget())
        if (layoutContaining(current, anchor)) return current;
    return nullptr;
}

bool placeSectionAfterAnchor(QWidget *host, QWidget *section, QWidget *anchor) {
    if (!host || !section || !anchor || !host->layout()) return false;
    auto *hostLayout = layoutContaining(host, anchor);
    if (!hostLayout) return false;
    int anchorIndex = -1;
    for (QWidget *candidate = anchor; candidate && candidate != host;
         candidate = candidate->parentWidget()) {
        anchorIndex = hostLayout->indexOf(candidate);
        if (anchorIndex >= 0) break;
    }
    if (anchorIndex < 0) return false;

    const int sectionIndex = hostLayout->indexOf(section);
    if (sectionIndex >= 0 && sectionIndex == anchorIndex + 1) return true;
    if (sectionIndex >= 0) hostLayout->removeWidget(section);

    // Removing an item before the anchor shifts the insertion index left by
    // one. Re-read the anchor index after removal so repeated sidebar
    // rebuilds remain idempotent.
    anchorIndex = -1;
    for (QWidget *candidate = anchor; candidate && candidate != host;
         candidate = candidate->parentWidget()) {
        anchorIndex = hostLayout->indexOf(candidate);
        if (anchorIndex >= 0) break;
    }
    if (anchorIndex < 0) return false;
    if (auto *box = qobject_cast<QBoxLayout *>(hostLayout))
        box->insertWidget(anchorIndex + 1, section);
    else if (auto *grid = qobject_cast<QGridLayout *>(hostLayout))
        grid->addWidget(section, grid->rowCount(), 0, 1, grid->columnCount());
    else
        hostLayout->addWidget(section);
    section->setProperty("gpvst3AnchorName", anchor->objectName());
    section->setProperty("gpvst3LayoutIndex", hostLayout->indexOf(section));
    return true;
}

QWidget *ensureHostSection(QWidget *host, const QString &sectionName,
                           const QString &dividerName, const QString &title,
                           const QStringList &anchorNames, const QStringList &anchorTexts) {
    if (!host || !host->layout()) return nullptr;
    if (anchorNames.isEmpty() || (anchorNames.size() == 1 && anchorNames.front().isEmpty())) return nullptr;
    if (auto *existing = host->findChild<QWidget *>(sectionName)) {
        if (existing->parentWidget() != host) return nullptr;
        auto *anchor = findHostAnchor(host, anchorNames, anchorTexts);
        if (!anchor || !placeSectionAfterAnchor(host, existing, anchor)) return nullptr;
        return existing;
    }
    auto *anchor = findHostAnchor(host, anchorNames, anchorTexts);
    if (!anchor) return nullptr;
    auto *section = new QWidget(host);
    section->setObjectName(sectionName);
    section->setProperty("gpvst3Owned", true);
    section->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    auto *layout = new QVBoxLayout(section);
    layout->setContentsMargins(0, 4, 0, 4);
    auto *divider = new QFrame(section);
    divider->setObjectName(dividerName);
    divider->setFrameShape(QFrame::HLine);
    divider->setFrameShadow(QFrame::Sunken);
    layout->addWidget(divider);
    auto *label = new QLabel(title, section);
    label->setObjectName(sectionName + QStringLiteral("Title"));
    label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    layout->addWidget(label);

    // Insert after the nearest host-level anchor without touching any native
    // item. If GP wraps the anchor, use the first ancestor represented in the
    // host layout; otherwise leave the section unmounted for this build.
    if (!placeSectionAfterAnchor(host, section, anchor)) {
        section->deleteLater();
        return nullptr;
    }
    return section;
}

} // namespace

const char *state() noexcept { return "panel_ready_p9"; }

void setRealtimeBypassControl(RealtimeBypassControl control) noexcept {
    g_realtimeBypassControl = control;
}

void setVst3SelectionControl(Vst3SelectionControl control) noexcept {
    g_vst3SelectionControl = control;
}

void setVst3SelectionRequestControl(Vst3SelectionRequestControl control) noexcept {
    g_vst3SelectionRequestControl = control;
}

void setVst3TrackSelectionControl(Vst3TrackSelectionControl control) noexcept {
    g_vst3TrackSelectionControl = control;
}

void setVst3TrackSelectionRequestControl(Vst3TrackSelectionRequestControl control) noexcept {
    g_vst3TrackSelectionRequestControl = control;
}

void setVst3StateControl(Vst3StateControl control) noexcept {
    g_vst3StateControl = control;
}

void setVst3TrackControls(Vst3TrackStateControl state, Vst3TrackEditorControl editor) noexcept {
    g_vst3TrackStateControl = state;
    g_vst3TrackEditorControl = editor;
}

void setVst3EditorControl(Vst3EditorControl open, Vst3EditorCloseControl close, Vst3EditorScaleControl scale) noexcept {
    g_vst3EditorControl = open;
    g_vst3EditorCloseControl = close;
    g_vst3EditorScaleControl = scale;
}

void setVst3Catalog(const QJsonArray &catalog) {
    if (g_vst3Catalog == catalog) return;
    g_vst3Catalog = catalog;
    if (g_p7Panel) {
        g_p7Panel->refreshCatalog();
    }
    if (g_globalPanel) g_globalPanel->refreshCatalog();
}

void setVst3DiscoveryControl(Vst3RefreshControl refresh, Vst3IdentifyControl identify) noexcept {
    g_refreshControl = refresh;
    g_identifyControl = identify;
}

void setVst3ScanState(const QString &state, int checked, int total, bool cached, const QString &detail) {
    g_vst3ScanState = state;
    g_scanButtonText = QStringLiteral("VST3");
    g_scanMessage.clear();
    g_scanDetail = detail;
    if (state == "scanning") {
        g_scanButtonText = cached ? QStringLiteral("VST3 · 正在更新…") :
            (total > 0 ? QStringLiteral("VST3 · 扫描中… %1/%2").arg(checked).arg(total)
                        : QStringLiteral("VST3 · 扫描中…"));
        g_scanMessage = cached ? QStringLiteral("正在检查插件变化，缓存清单可继续使用。")
            : QStringLiteral("首次扫描可能需要一些时间，完成后将自动显示插件。");
    } else if (state == "recognition") {
        g_scanButtonText = QStringLiteral("VST3 · 扫描中…");
        g_scanMessage = QStringLiteral("正在更新可用插件清单。");
    } else if (state == "scan_failed") {
        // A retry affordance is useful while keeping the text free of
        // module names, paths and error codes.
        g_scanButtonText = QStringLiteral("VST3 · 点击重试");
        g_scanMessage = QStringLiteral("点击 VST3 可刷新插件清单。");
    } else if (state == "partial_failure" || state == "failed" || state == "timeout") {
        g_scanMessage = QStringLiteral("点击 VST3 可刷新插件清单。");
    }
    if (qApp) for (auto *widget : QApplication::allWidgets()) {
        if (widget->objectName() == QStringLiteral("gpvst3SoundEffectChainButton")) {
            if (auto *button = qobject_cast<QPushButton *>(widget)) {
                button->setText(g_scanButtonText);
                button->setToolTip(QStringLiteral("打开或刷新 VST3 插件清单"));
            }
        }
    }
    if (g_p7Panel) g_p7Panel->scanFeedback();
    if (g_globalPanel) g_globalPanel->scanFeedback();
}

double nativeEditorScale(void *host) {
    auto *widget = QWidget::find(reinterpret_cast<WId>(host));
    return widget ? widget->devicePixelRatioF() : 1.0;
}

void resizeNativeEditor(void *host, int width, int height) {
    auto *widget = QWidget::find(reinterpret_cast<WId>(host));
    if (!widget) return;
    const auto scale = widget->devicePixelRatioF();
    const QSize size(qMax(1, qRound(width / scale)), qMax(1, qRound(height / scale)));
    widget->setFixedSize(size);
    if (widget->window() != widget) widget->window()->setFixedSize(size);
}

void shutdownEditors() {
    saveCurrentRuntimeState();
    if (g_editorWindow) { g_editorWindow->close(); delete g_editorWindow.data(); }
    else if (g_vst3EditorCloseControl) g_vst3EditorCloseControl();
}

void syncVst3Selection() {
    if (g_p7Panel) g_p7Panel->syncSelection();
    if (g_globalPanel) g_globalPanel->syncSelection();
}

void refreshVst3TrackContext() {
    if (g_p7Panel) g_p7Panel->refreshTrackContext();
}

void reloadVst3Selections() {
    if (g_p7Panel) g_p7Panel->reloadSavedSelection();
    if (g_globalPanel) g_globalPanel->reloadSavedSelection();
}

void showEffectChainPanel(bool show) {
    if (!qApp) return;
    if (show) g_trackExpanded = g_globalExpanded = true;
    QJsonObject chain;
    state::loadChain(chain);
    bool legacy = false;
    for (const auto &value : chain.value("effects").toArray()) {
        if (value.toObject().contains("plugin_path")) { legacy = true; break; }
    }
    // Keep the old isolated P5 fixture readable when no catalog is supplied;
    // a real P7 host always has a catalog and therefore uses the two-state UI.
    legacy = legacy && g_vst3Catalog.isEmpty();
    const bool useP7Panel = !legacy;

    // Keep the maintenance timer owned by qApp rather than by the selector.
    // GP destroys and rebuilds the sidebar widgets during score/track changes;
    // the next tick must recreate a hidden selector and reattach its entry.
    g_panelUsesP7 = useP7Panel;
    auto *timer = g_panelAttachTimer.data();
    if (!timer) {
        timer = new QTimer(qApp);
        timer->setInterval(500);
        g_panelAttachTimer = timer;
    }
    const auto attachPanel = [timer] {
        ensureAboutEntry();
        QWidget *panel = qApp->property("gpvst3P5Panel").value<QWidget *>();
        if (!panel) {
            panel = g_panelUsesP7 ? static_cast<QWidget *>(new P7Panel)
                                  : static_cast<QWidget *>(new ChainPanel);
            qApp->setProperty("gpvst3P5Panel", QVariant::fromValue(panel));
            QObject::connect(panel, &QObject::destroyed, qApp, [] {
                qApp->setProperty("gpvst3P5Panel", QVariant());
            });
        }
        const bool useP7Panel = g_panelUsesP7;
        if (useP7Panel && !g_globalPanel) {
            auto *global = new P7Panel(state::ScopeKind::Global);
            qApp->setProperty("gpvst3GlobalPanel", QVariant::fromValue(static_cast<QWidget *>(global)));
            QObject::connect(global, &QObject::destroyed, qApp, [] {
                qApp->setProperty("gpvst3GlobalPanel", QVariant());
            });
        }
        auto *soundHost = findSoundHost();
        bool soundEntryReady = soundHost != nullptr;
        QWidget *trackSection = nullptr;
        QWidget *globalSection = nullptr;
        if (useP7Panel && soundHost) {
            const auto trackAnchor = findApplicationAnchor(
                {QStringLiteral("gpNativeInstrumentEffects"), QStringLiteral("gpvst3NativeSourceEffects"),
                 QStringLiteral("rseEffectsChain"), QStringLiteral("instrumentEffects"),
                 QStringLiteral("soundRack")},
                {QStringLiteral("音源效果器"), QStringLiteral("音轨效果器"), QStringLiteral("RSE")});
            const auto globalAnchor = findApplicationAnchor(
                {QStringLiteral("gpMasterPostProcessing"), QStringLiteral("masterPostProcessing"),
                 QStringLiteral("gpvst3MasterEffects"), QStringLiteral("masterEffects"),
                 QStringLiteral("soundMastering"), QStringLiteral("soundMasteringTitle")},
                {QStringLiteral("母带后期处理"), QStringLiteral("Master Post Processing")});
            trackSection = ensureHostSection(layoutHostFor(trackAnchor), QStringLiteral("gpvst3TrackVst3Section"),
                QStringLiteral("gpvst3TrackVst3Divider"), QStringLiteral("音轨 VST3 效果器"),
                {trackAnchor ? trackAnchor->objectName() : QString{}}, {});
            globalSection = ensureHostSection(layoutHostFor(globalAnchor), QStringLiteral("gpvst3GlobalVst3Section"),
                QStringLiteral("gpvst3GlobalVst3Divider"), QStringLiteral("全局 Master VST3 效果器"),
                {globalAnchor ? globalAnchor->objectName() : QString{}}, {});
        }
        if (soundHost && !soundHost->findChild<QPushButton *>("gpvst3SoundEffectChainButton")) {
            auto *button = new QPushButton(g_scanButtonText, soundHost);
            button->setObjectName(QStringLiteral("gpvst3SoundEffectChainButton"));
            button->setToolTip(QStringLiteral("打开或刷新 VST3 插件清单"));
            soundHost->layout()->addWidget(button);
            QObject::connect(button, &QPushButton::clicked, button, [] {
                showEffectChainPanel();
                if (g_refreshControl) g_refreshControl();
            });
        }
        if (useP7Panel) {
            const auto mount = [](QWidget *content, QWidget *section, bool expanded) {
                if (!content) return;
                if (!section) {
                    content->hide();
                    content->setProperty("gpvst3AnchorReady", false);
                    return;
                }
                if (content->parentWidget() != section) {
                    content->setParent(section, Qt::Widget);
                    section->layout()->addWidget(content);
                }
                content->setProperty("gpvst3AnchorReady", true);
                content->setVisible(expanded);
                section->show();
            };
            mount(panel, trackSection, g_trackExpanded);
            mount(g_globalPanel, globalSection, g_globalExpanded);
        }
        bool dockReady = timer->property("dockReady").toBool();
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
        } else if (!soundEntryReady) {
            // Do not add a second title/menu entry when the host has not yet
            // exposed its sound section. The native sidebar button is the
            // single plugin entry once that section becomes available.
            for (QWidget *widget : QApplication::topLevelWidgets()) {
                if (QByteArray(widget->metaObject()->className()) != "gp::gui::MainWindow") continue;
                if (auto *stale = widget->findChild<QAction *>("gpvst3P7EffectChainAction")) {
                    if (auto *window = qobject_cast<QMainWindow *>(widget)) window->menuBar()->removeAction(stale);
                    stale->deleteLater();
                }
            }
        }
        if (!useP7Panel && !panel->parentWidget()) {
            panel->show(); panel->raise(); panel->activateWindow();
        }
        timer->setProperty("dockReady", dockReady);
    };
    if (!timer->property("gpvst3Connected").toBool()) {
        QObject::connect(timer, &QTimer::timeout, timer, attachPanel);
        timer->setProperty("gpvst3Connected", true);
    }
    // A user click must display a newly recreated selector in this event,
    // without waiting for another click or the sidebar maintenance timer.
    attachPanel();
    timer->start();
}

}
