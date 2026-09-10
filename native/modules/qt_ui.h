#pragma once

#include <QtCore/QJsonArray>
#include <QtCore/QString>
#include <string>
#include <vector>

#include "gp_hook.h"

namespace gpvst3::ui {

using RealtimeBypassControl = void (*)(bool) noexcept;
using Vst3SelectionEntry = gpvst3::hook::Vst3SelectionEntry;
using Vst3SelectionControl = bool (*)(const std::vector<Vst3SelectionEntry> &, std::string *) noexcept;
using Vst3StateControl = std::vector<Vst3SelectionEntry> (*)();
using Vst3EditorControl = bool (*)(const Vst3SelectionEntry &, void *) noexcept;
using Vst3EditorCloseControl = void (*)() noexcept;
using Vst3EditorScaleControl = void (*)(void *, double) noexcept;
using Vst3RefreshControl = void (*)();
using Vst3IdentifyControl = QJsonArray (*)(const QString &, QString *);

// The P7 selector belongs to the sound section; the native plug-in editor
// is a separate nonmodal window owned by the Guitar Pro main window.
const char *state() noexcept;
void setRealtimeBypassControl(RealtimeBypassControl control) noexcept;
void setVst3SelectionControl(Vst3SelectionControl control) noexcept;
void setVst3StateControl(Vst3StateControl control) noexcept;
void setVst3EditorControl(Vst3EditorControl open, Vst3EditorCloseControl close,
                         Vst3EditorScaleControl scale = nullptr) noexcept;
void setVst3DiscoveryControl(Vst3RefreshControl refresh, Vst3IdentifyControl identify) noexcept;
void syncVst3Selection();
void setVst3Catalog(const QJsonArray &catalog);
void setVst3ScanState(const QString &state, int checked = 0, int total = 0,
                     bool cached = false, const QString &detail = {});
void resizeNativeEditor(void *host, int width, int height);
double nativeEditorScale(void *host);
void shutdownEditors();
void showEffectChainPanel(bool show = true);

}
