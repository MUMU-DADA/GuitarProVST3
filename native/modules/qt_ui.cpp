#include "qt_ui.h"

#include "state_manager.h"

#include <algorithm>
#include <QtCore/QFileInfo>
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
#include <QtWidgets/QSizePolicy>
#include <QtWidgets/QTabWidget>

namespace gpvst3::ui {
namespace {

RealtimeBypassControl g_realtimeBypassControl = nullptr;
Vst3SelectionControl g_vst3SelectionControl = nullptr;
Vst3TrackSelectionControl g_vst3TrackSelectionControl = nullptr;
Vst3StateControl g_vst3StateControl = nullptr;
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

class P7Panel final : public QWidget {
public:
    P7Panel() {
        setObjectName(QStringLiteral("gpvst3P7Panel"));
        setAttribute(Qt::WA_DeleteOnClose);
        setWindowTitle(QStringLiteral("音源 · VST3 效果器"));
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
        resize(420, 360);
        auto *root = new QVBoxLayout(this);
        auto *heading = new QHBoxLayout;
        heading->addWidget(new QLabel(QStringLiteral("VST3 效果器"), this), 1);
        auto *close = new QPushButton(QStringLiteral("×"), this);
        close->setObjectName(QStringLiteral("gpvst3CloseSelectorButton"));
        close->setToolTip(QStringLiteral("关闭选择区"));
        close->setFixedWidth(24);
        heading->addWidget(close);
        connect(close, &QPushButton::clicked, this, &QWidget::close);
        root->addLayout(heading);
        scopeTabs_ = new QTabWidget(this);
        auto *trackPage = new QWidget(scopeTabs_);
        auto *trackLayout = new QVBoxLayout(trackPage);
        const auto trackNumber = qEnvironmentVariable("GPVST3_TRACK");
        auto *trackContext = new QLabel(trackNumber.isEmpty()
            ? QStringLiteral("当前音轨：无法确认当前音轨")
            : QStringLiteral("当前音轨：Track %1").arg(trackNumber), trackPage);
        trackContext->setObjectName(QStringLiteral("gpvst3TrackContext"));
        trackLayout->addWidget(trackContext);
        trackLayout->addWidget(new QLabel(QStringLiteral("正在使用（从上到下为效果顺序）"), trackPage));
        list_ = new QListWidget(trackPage);
        list_->setObjectName(QStringLiteral("gpvst3TrackChainList"));
        list_->setSelectionMode(QAbstractItemView::SingleSelection);
        list_->setDragDropMode(QAbstractItemView::InternalMove);
        list_->setDefaultDropAction(Qt::MoveAction);
        trackLayout->addWidget(list_, 2);
        trackLayout->addWidget(new QLabel(QStringLiteral("可用插件"), trackPage));
        availableList_ = new QListWidget(trackPage);
        availableList_->setObjectName(QStringLiteral("gpvst3AvailableList"));
        availableList_->setSelectionMode(QAbstractItemView::NoSelection);
        trackLayout->addWidget(availableList_, 1);
        scopeTabs_->addTab(trackPage, QStringLiteral("当前音轨"));
        auto *globalPage = new QWidget(scopeTabs_);
        auto *globalLayout = new QVBoxLayout(globalPage);
        globalLayout->addWidget(new QLabel(QStringLiteral("正在使用（从上到下为效果顺序）"), globalPage));
        globalList_ = new QListWidget(globalPage);
        globalList_->setObjectName(QStringLiteral("gpvst3GlobalChainList"));
        globalList_->setSelectionMode(QAbstractItemView::SingleSelection);
        globalList_->setDragDropMode(QAbstractItemView::InternalMove);
        globalList_->setDefaultDropAction(Qt::MoveAction);
        globalLayout->addWidget(globalList_, 1);
        globalLayout->addWidget(new QLabel(QStringLiteral("可用插件"), globalPage));
        globalAvailableList_ = new QListWidget(globalPage);
        globalAvailableList_->setSelectionMode(QAbstractItemView::NoSelection);
        globalLayout->addWidget(globalAvailableList_, 1);
        scopeTabs_->addTab(globalPage, QStringLiteral("全局 Master"));
        root->addWidget(scopeTabs_, 1);
        // A track tab is safe only when the host supplies a stable track
        // context. Without that key, keep the legacy global chain as the
        // initial scope so existing selections continue to process through
        // the verified Master hook; the unresolved track tab remains visible
        // and explicitly bypassed until a context is provided.
        if (qEnvironmentVariable("GPVST3_TRACK").isEmpty()) scope_ = state::ScopeKind::Global;
        scopeTabs_->setCurrentIndex(scope_ == state::ScopeKind::Global ? 1 : 0);
        status_ = new QLabel(this);
        status_->setObjectName(QStringLiteral("gpvst3Status"));
        status_->setWordWrap(true);
        root->addWidget(status_);
        connect(scopeTabs_, &QTabWidget::currentChanged, this, [this](int index) {
            saveRuntimeState();
            scope_ = index == 0 ? state::ScopeKind::Track : state::ScopeKind::Global;
            loadChain();
        });
        connect(list_->model(), &QAbstractItemModel::rowsMoved, this,
                [this] { syncOrderFromList(); });
        connect(globalList_->model(), &QAbstractItemModel::rowsMoved, this,
                [this] { syncOrderFromList(); });
        loadChain();
        g_p7Panel = this;
    }

    ~P7Panel() override {
        saveRuntimeState();
        if (g_p7Panel == this) g_p7Panel = nullptr;
    }

    void refreshCatalog() {
        saveRuntimeState();
        loadChain();
        if (scope_ == state::ScopeKind::Global) publishSelection();
    }

    void capture() { saveRuntimeState(); }
    void scanFeedback() {
        status_->setText(g_scanMessage);
        status_->setToolTip(g_scanDetail);
        if (g_scanMessage.isEmpty() && activeList()->count() == 0 && activeAvailableList()->count() == 0)
            status_->setText(QStringLiteral("未发现可用的 VST3 效果器。"));
    }

    void syncSelection() {
        bool anyEnabled = false;
        for (const auto &value : effects_)
            anyEnabled |= value.toObject().value("enabled").toBool();
        if (anyEnabled && scope_ == state::ScopeKind::Global) {
            selectionDirty_ = true;
            publishSelection();
        }
    }

private:
    static std::vector<unsigned char> stateBytes(const QJsonObject &effect, const char *field) {
        const auto bytes = QByteArray::fromBase64(effect.value(field).toString().toLatin1());
        return {bytes.begin(), bytes.end()};
    }

    bool publishSelection() {
        if (!g_vst3SelectionControl && !g_vst3TrackSelectionControl) return true; // isolated UI fixture
        std::vector<Vst3SelectionEntry> selection;
        for (const auto &value : effects_) {
            const auto effect = value.toObject();
            if (!effect.value("enabled").toBool()) continue;
            const auto module = effect.value("module").toString();
            const auto classId = effect.value("class_id").toString();
            if (module.isEmpty() || classId.isEmpty()) continue;
            selection.push_back({module.toStdString(), classId.toStdString(),
                                 stateBytes(effect, "component_state"), stateBytes(effect, "controller_state")});
        }
        selectionDirty_ = false;
        std::string error;
        const bool accepted = scope_ == state::ScopeKind::Global
            ? (g_vst3SelectionControl ? g_vst3SelectionControl(selection, &error) : true)
            : (g_vst3TrackSelectionControl ? g_vst3TrackSelectionControl(
                  state::currentTrackKey().toStdString(), selection, &error) : true);
        if (accepted) return true;
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
        if (g_vst3StateControl) for (const auto &saved : g_vst3StateControl()) {
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
        state::setScopeEffects(sidecar_, scope_, savedEffects, state::currentScoreKey(),
                               state::currentTrackKey(), qEnvironmentVariable("GPVST3_TRACK").toInt(), {});
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
        QString error;
        if (!state::loadChain(sidecar_, &error))
            status_->setText(QStringLiteral("状态恢复失败：%1").arg(error));
        auto saved = state::scopeEffects(sidecar_, scope_, state::currentScoreKey(), state::currentTrackKey());
        QHash<QString, int> savedRows;
        for (int index = 0; index < saved.size(); ++index) {
            const auto effect = saved.at(index).toObject();
            if (!effect.value("module").toString().isEmpty()) savedRows.insert(key(effect), index);
        }

        QHash<QString, int> counts;
        for (const auto &value : g_vst3Catalog) {
            const auto entry = value.toObject();
            if (entry.value("identified").toBool() || entry.value("compatible").toBool())
                counts[entry.value("name").toString()]++;
        }
        QSet<QString> listed;
        std::vector<QJsonValue> catalogEffects;
        for (const auto &value : g_vst3Catalog) {
            const auto entry = value.toObject();
            const bool identified = entry.value("identified").toBool() || entry.value("compatible").toBool();
            if (!identified && !entry.value("class_id").toString().isEmpty()) continue;
            bool savedModule = false;
            for (const auto &value : saved) {
                const auto item = value.toObject();
                savedModule |= item.value("module") == entry.value("module") && !item.value("class_id").toString().isEmpty();
            }
            if (!identified && savedModule) continue;
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
            if (effect.value("module").toString().isEmpty() || listed.contains(key(effect))) continue;
            auto missing = effect;

            catalogEffects.push_back(missing);
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
        globalAvailableList_->clear();
        if (scope_ == state::ScopeKind::Global) globalList_->clear();
        effects_ = {};
        for (const auto &value : catalogEffects) {
            const auto effect = value.toObject();
            const auto recognition = effect.value("recognition_status").toString();
            const auto suffix = effect.value("identified").toBool() ? QString{}
                : (recognition == QStringLiteral("failed") ? QStringLiteral(" · 识别失败")
                   : QStringLiteral(" · 后台识别中"));
            appendRow(effect, displayName(effect, counts) + suffix);
        }
        state::setScopeEffects(sidecar_, scope_, effects_, state::currentScoreKey(), state::currentTrackKey());
        scanFeedback();
    }

    void appendRow(const QJsonObject &effect, const QString &label) {
        const int index = effects_.size();
        effects_.append(effect);
        auto *targetList = effect.value("enabled").toBool() ? activeList() : activeAvailableList();
        auto *item = new QListWidgetItem(targetList);
        auto *row = new QWidget(targetList);
        auto *layout = new QHBoxLayout(row);
        layout->setContentsMargins(2, 2, 2, 2);
        auto *check = new QCheckBox(row);
        const bool pending = effect.value("class_id").toString().isEmpty();
        const auto token = pending ? QString::fromLatin1(QCryptographicHash::hash(effect.value("module").toString().toUtf8(),
                            QCryptographicHash::Sha256).toHex().left(16)) : effect.value("class_id").toString();
        check->setObjectName((pending ? QStringLiteral("gpvst3Identify_") : QStringLiteral("gpvst3Enabled_")) + token);
        check->setToolTip(pending ? QStringLiteral("后台识别完成后可启用；插件加载后可能自行联网。") : QString{});
        check->setChecked(effect.value("enabled").toBool());
        auto *name = new QPushButton(label, row);
        name->setObjectName(QStringLiteral("gpvst3Editor_") + token);
        name->setFlat(true);
        name->setCursor(Qt::PointingHandCursor);
        layout->addWidget(check);
        layout->addWidget(name, 1);
        item->setSizeHint(row->sizeHint());
        targetList->setItemWidget(item, row);
        const auto identity = key(effect);
        row->setProperty("gpvst3EntryId", identity);
        connect(check, &QCheckBox::toggled, this, [this, identity, check](bool enabled) {
            saveRuntimeState();
            const int index = indexFor(identity); if (index < 0) return;
            auto effect = effects_.at(index).toObject();
            const auto previous = effect;
            if (enabled && effect.value("class_id").toString().isEmpty()) {
                { const QSignalBlocker blocked(check); check->setChecked(false); }
                effect.insert("enabled", true);
                effect.insert("bypass", false);
                effects_.replace(index, effect);
                saveRuntimeState();
                status_->setText(QStringLiteral("后台识别中：完成后可启用。"));
                return;
            }
            if (!enabled && g_editorWindow && g_editorWindow->openedKey == key(effect)) g_editorWindow->close();
            effect.insert("enabled", enabled);
            effect.insert("bypass", !enabled);
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
                ? (scope_ == state::ScopeKind::Track
                    ? QStringLiteral("已保存音轨链：等待宿主确认音轨上下文，当前保持旁路。")
                    : QStringLiteral("已启用：点击名称打开原生 GUI"))
                : QStringLiteral("已停用：%1").arg(effect.value("name").toString()));
            // The isolated P7 fixture keeps its legacy item widgets stable;
            // the host path (with a track context) rebinds rows so enabled
            // items move to the active section immediately.
            if (!qEnvironmentVariable("GPVST3_TRACK").isEmpty())
                QTimer::singleShot(0, this, [this] { loadChain(); });
        });
        connect(name, &QPushButton::clicked, this, [this, identity] {
            const int index = indexFor(identity); if (index >= 0) openEditor(index);
        });
    }

    void openEditor(int index) {
        const auto effect = effects_.at(index).toObject();
        if (!effect.value("enabled").toBool()) {
            status_->setText(QStringLiteral("请先勾选启用插件，再打开其 GUI。"));
            return;
        }
        auto *window = editorWindow();
        if (window->openedKey == key(effect) && window->isVisible()) {
            window->showNormal(); window->raise(); window->activateWindow();
            return;
        }
        saveRuntimeState();
        if (g_vst3EditorCloseControl) g_vst3EditorCloseControl();
        window->openedKey = key(effect);
        window->setWindowTitle(effect.value("name").toString() + QStringLiteral(" · VST3"));
        window->show();
        const gpvst3::hook::Vst3SelectionEntry selection{
            effect.value("module").toString().toStdString(),
            effect.value("class_id").toString().toStdString()};
        if (!g_vst3EditorControl) {
            window->hide();
            status_->setText(QStringLiteral("原生 GUI 暂不可用：当前测试宿主未提供 IPlugView/HWND 桥接（host_limited）。"));
            return;
        }
        if (!g_vst3EditorControl(selection, reinterpret_cast<void *>(window->host->winId()))) {
            window->hide();
            status_->setText(QStringLiteral("原生 GUI 不可用：插件未提供可嵌入 editor 或初始化失败。"));
            return;
        }
        window->raise(); window->activateWindow();
        status_->setText(QStringLiteral("原生 GUI 已打开：%1").arg(effect.value("name").toString()));
    }

    void closeEvent(QCloseEvent *event) override {
        saveRuntimeState();
        QWidget::closeEvent(event);
    }

    QListWidget *activeList() const noexcept {
        return scope_ == state::ScopeKind::Global ? globalList_ : list_;
    }

    QListWidget *activeAvailableList() const noexcept {
        return scope_ == state::ScopeKind::Global ? globalAvailableList_ : availableList_;
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
        effects_ = ordered;
        saveRuntimeState();
        publishSelection();
    }

    QListWidget *list_ = nullptr, *globalList_ = nullptr, *availableList_ = nullptr, *globalAvailableList_ = nullptr;
    QTabWidget *scopeTabs_ = nullptr;
    QLabel *status_ = nullptr;
    QJsonObject sidecar_;
    QJsonArray effects_;
    state::ScopeKind scope_ = state::ScopeKind::Track;
    bool selectionDirty_ = false;
};

void saveCurrentRuntimeState() {
    if (g_p7Panel) { g_p7Panel->capture(); return; }
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

} // namespace

const char *state() noexcept { return "panel_ready_p7"; }

void setRealtimeBypassControl(RealtimeBypassControl control) noexcept {
    g_realtimeBypassControl = control;
}

void setVst3SelectionControl(Vst3SelectionControl control) noexcept {
    g_vst3SelectionControl = control;
}

void setVst3TrackSelectionControl(Vst3TrackSelectionControl control) noexcept {
    g_vst3TrackSelectionControl = control;
}

void setVst3StateControl(Vst3StateControl control) noexcept {
    g_vst3StateControl = control;
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
        g_scanButtonText = cached ? QStringLiteral("VST3 · 正在更新…") : (total > 0
            ? QStringLiteral("VST3 · 扫描 %1/%2").arg(checked).arg(total) : QStringLiteral("VST3 · 正在扫描…"));
        g_scanMessage = cached ? QStringLiteral("正在检查插件变化，缓存清单可继续使用。")
            : QStringLiteral("首次扫描可能需要一些时间，完成后将自动显示插件。");
    } else if (state == "recognition") {
        g_scanButtonText = QStringLiteral("VST3 · 后台识别中…");
        g_scanMessage = QStringLiteral("插件清单已可用，后台正在识别候选插件。勾选操作不会阻塞界面。");
    } else if (state == "scan_failed") {
        g_scanButtonText = QStringLiteral("VST3 · 扫描失败，点击重试");
        g_scanMessage = QStringLiteral("扫描失败，请点击 VST3 重试。");
    } else if (state == "partial_failure") {
        g_scanMessage = QStringLiteral("部分文件读取失败，已保留候选插件，稍后重试静态读取。");
    }
    if (qApp) for (auto *widget : QApplication::allWidgets()) {
        if (widget->objectName() == QStringLiteral("gpvst3SoundEffectChainButton")) {
            if (auto *button = qobject_cast<QPushButton *>(widget)) {
                button->setText(g_scanButtonText); button->setToolTip(g_scanDetail);
            }
        }
    }
    if (g_p7Panel) g_p7Panel->scanFeedback();
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
}

void showEffectChainPanel(bool show) {
    if (!qApp) return;
    if (auto *existing = qApp->property("gpvst3P5Panel").value<QWidget *>()) {
        if (show) {
            existing->setProperty("gpvst3ShowRequested", true);
            if (existing->objectName() != "gpvst3P7Panel" || existing->parentWidget()) {
                existing->show();
                existing->raise();
                existing->activateWindow();
                existing->setProperty("gpvst3ShowRequested", false);
            }
        }
        return;
    }
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
    panel->setProperty("gpvst3ShowRequested", show);
    qApp->setProperty("gpvst3P5Panel", QVariant::fromValue(static_cast<QWidget *>(panel)));
    QObject::connect(panel, &QObject::destroyed, qApp, [] { qApp->setProperty("gpvst3P5Panel", QVariant()); });

    // Keep the entry alive for the lifetime of the host. GP rebuilds this
    // sidebar when the score or selected track changes, so a one-shot timer
    // would leave the button missing after that rebuild.
    auto *timer = new QTimer(qApp);
    timer->setInterval(500);
    QObject::connect(panel, &QObject::destroyed, timer, [timer] {
        timer->stop();
        timer->deleteLater();
    });
    const QPointer<QWidget> panelGuard(panel);
    const auto attachPanel = [timer, panelGuard, useP7Panel] {
        QWidget *panel = panelGuard.data();
        if (!panel) {
            timer->stop();
            timer->deleteLater();
            return;
        }
        auto *soundHost = findSoundHost();
        bool soundEntryReady = soundHost != nullptr;
        if (soundHost && !soundHost->findChild<QPushButton *>("gpvst3SoundEffectChainButton")) {
            auto *button = new QPushButton(g_scanButtonText, soundHost);
            button->setObjectName(QStringLiteral("gpvst3SoundEffectChainButton"));
            button->setToolTip(g_scanDetail);
            soundHost->layout()->addWidget(button);
            QObject::connect(button, &QPushButton::clicked, button, [] {
                showEffectChainPanel();
                if (g_refreshControl) g_refreshControl();
            });
        }
        if (useP7Panel && soundHost && panel->parentWidget() != soundHost) {
            // The P7 selector belongs to the same QWidget hierarchy and
            // layout as the host's sound section. Reparenting is repeated on
            // every tick because GP rebuilds this area when the score or
            // selected track changes.
            const bool wasVisible = !panel->isHidden();
            panel->setParent(soundHost, Qt::Widget);
            soundHost->layout()->addWidget(panel);
            panel->setWindowFlag(Qt::Tool, false);
            panel->setWindowTitle(QString());
            if (wasVisible) panel->show();
            else panel->hide();
        } else if (useP7Panel && !soundHost && panel->parentWidget()) {
            // If the private sound section is temporarily absent, detach the
            // panel so the next section instance can adopt it safely.
            const bool wasVisible = !panel->isHidden();
            panel->setParent(nullptr, Qt::Tool);
            if (wasVisible) panel->show();
            else panel->hide();
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
            // Fallback for GP builds that expose no stable audio-section
            // object. This keeps a visible, repeatable entry without opening
            // a dock or showing the editor automatically at startup.
            for (QWidget *widget : QApplication::topLevelWidgets()) {
                if (QByteArray(widget->metaObject()->className()) != "gp::gui::MainWindow") continue;
                auto *window = qobject_cast<QMainWindow *>(widget);
                if (!window || !window->menuBar()) continue;
                auto *action = window->findChild<QAction *>("gpvst3P7EffectChainAction");
                if (!action) {
                    action = window->menuBar()->addAction(QStringLiteral("VST3 效果器"));
                    action->setObjectName(QStringLiteral("gpvst3P7EffectChainAction"));
                    QObject::connect(action, &QAction::triggered, action, [] {
                        showEffectChainPanel();
                    });
                }
            }
        }
        if (!useP7Panel && !panel->parentWidget()) {
            panel->show(); panel->raise(); panel->activateWindow();
        }
        if (useP7Panel && soundHost && panel->property("gpvst3ShowRequested").toBool()) {
            panel->show();
            panel->setProperty("gpvst3ShowRequested", false);
        }
        timer->setProperty("dockReady", dockReady);
    };
    QObject::connect(timer, &QTimer::timeout, timer, attachPanel);
    // A user click must display a newly recreated selector in this event,
    // without waiting for another click or the sidebar maintenance timer.
    attachPanel();
    timer->start();
}

}
