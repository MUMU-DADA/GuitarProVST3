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
using Vst3SelectionRequestControl = bool (*)(const std::vector<Vst3SelectionEntry> &, std::string *) noexcept;
using Vst3TrackSelectionControl = bool (*)(const std::string &, const std::vector<Vst3SelectionEntry> &, std::string *) noexcept;
using Vst3TrackSelectionRequestControl = bool (*)(const std::string &, const std::vector<Vst3SelectionEntry> &, std::string *) noexcept;
using Vst3SelectionMatchControl = bool (*)(const std::string &, const std::vector<Vst3SelectionEntry> &) noexcept;
using Vst3StateControl = std::vector<Vst3SelectionEntry> (*)();
using Vst3TrackStateControl = std::vector<Vst3SelectionEntry> (*)(const std::string &);
using Vst3TrackEditorControl = bool (*)(const std::string &, const Vst3SelectionEntry &, void *) noexcept;
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
void setVst3SelectionRequestControl(Vst3SelectionRequestControl control) noexcept;
void setVst3BusyControl(bool (*control)() noexcept) noexcept;
void setVst3TrackSelectionControl(Vst3TrackSelectionControl control) noexcept;
void setVst3TrackSelectionRequestControl(Vst3TrackSelectionRequestControl control) noexcept;
void setVst3SelectionMatchControl(Vst3SelectionMatchControl control) noexcept;
void setVst3StateControl(Vst3StateControl control) noexcept;
void setVst3TrackControls(Vst3TrackStateControl state, Vst3TrackEditorControl editor) noexcept;
void setVst3EditorControl(Vst3EditorControl open, Vst3EditorCloseControl close,
                         Vst3EditorScaleControl scale = nullptr) noexcept;
void setVst3DiscoveryControl(Vst3RefreshControl refresh, Vst3IdentifyControl identify) noexcept;
void syncVst3Selection();
void refreshVst3TrackContext();
void reloadVst3Selections();
void setVst3Catalog(const QJsonArray &catalog);
void setVst3ScanState(const QString &state, int checked = 0, int total = 0,
                     bool cached = false, const QString &detail = {});
void resizeNativeEditor(void *host, int width, int height);
double nativeEditorScale(void *host);
void shutdownEditors();
void showEffectChainPanel(bool show = true);

}
