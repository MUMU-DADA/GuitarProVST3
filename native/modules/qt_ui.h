#pragma once

namespace gpvst3::ui {

// P5 exposes the chain editor as a Qt tool window. Guitar Pro does not export
// a stable widget insertion API, so the editor remains independent of GP's
// private widget hierarchy.
const char *state() noexcept;
void showEffectChainPanel();

}
