#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gpvst3::gp_audio {

using RefreshNotifier = void (*)() noexcept;

struct Binding {
    void *chain = nullptr;
    int trackIndex = -1;
    int soundIndex = -1;
    std::string trackKey;
    std::string trackId;
    std::string documentId;
    std::string scoreKey;
    bool activeDocument = true;
    bool selectedTrack = false;
};

// Installs a lightweight Qt event observer so native ConductorController
// objects created outside the QWidget tree become available to the control
// thread.  No native model calls are made from the audio callback.
void initialize() noexcept;
void shutdown() noexcept;
void setRefreshNotifier(RefreshNotifier notifier) noexcept;
void markDirty() noexcept;
// Marks only the active-document/selected-track context dirty. This schedules
// the same coalesced control pass without forcing a native track/effects-chain
// collection for every document-view show/hide event.
void markSelectionDirty() noexcept;
bool refreshNeeded() noexcept;

// Refreshes the immutable chain binding snapshot. Must run on the Qt/control
// thread. Returns the number of verified bindings published.
std::size_t refresh() noexcept;
std::size_t refreshIfNeeded() noexcept;
// Refresh only MCP-provided document/selection flags. This bounded fallback
// avoids traversing the host QWidget/native object tree when a score cursor
// changes without a lifecycle event.
bool refreshSelectionContext() noexcept;
bool refreshIncomplete() noexcept;
// Cheap two-second fallback signature for score/controller topology. A
// changed signature schedules the full collector; an unchanged signature
// never traverses the widget tree.
bool checkStructureChanged() noexcept;

// Returns the track selected by the active Guitar Pro document according to
// the MCP bridge. This is a control-thread snapshot; the audio callback uses
// only the immutable dispatch table published by refresh().
bool currentTrack(Binding &binding) noexcept;
const char *bindingSource() noexcept;

// Audio-thread lookup. The returned strings are never accessed by the audio
// path; callers only use the returned binding pointer and its prebuilt key.
const Binding *lookup(void *chain) noexcept;

std::vector<Binding> snapshot();

}
