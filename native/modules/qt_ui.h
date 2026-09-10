#pragma once

#include <QtCore/QJsonArray>
#include <QtCore/QString>
#include <string>
#include <vector>

#include "gp_hook.h"

namespace gpvst3::ui {

using RealtimeBypassControl = void (*)(bool) noexcept;
using Vst3SelectionEntry = gpvst3::hook::Vst3SelectionEntry;
using Vst3SelectionControl = bool (*)(const std::vector<Vst3SelectionEntry> &) noexcept;
using Vst3StateControl = std::vector<Vst3SelectionEntry> (*)();
using Vst3EditorControl = bool (*)(const Vst3SelectionEntry &, void *) noexcept;
using Vst3EditorCloseControl = void (*)() noexcept;

// P5 exposes the chain editor as a Qt tool window. Guitar Pro does not export
// a stable widget insertion API, so the editor remains independent of GP's
// private widget hierarchy.
const char *state() noexcept;
void setRealtimeBypassControl(RealtimeBypassControl control) noexcept;
void setVst3SelectionControl(Vst3SelectionControl control) noexcept;
void setVst3StateControl(Vst3StateControl control) noexcept;
void setVst3EditorControl(Vst3EditorControl open, Vst3EditorCloseControl close) noexcept;
void syncVst3Selection();
void setVst3Catalog(const QJsonArray &catalog);
void setVst3ScanState(const QString &state);
void showEffectChainPanel();

}
