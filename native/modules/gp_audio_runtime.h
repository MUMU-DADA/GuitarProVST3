#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class QObject;

namespace gpvst3::gp_audio {

using RefreshNotifier = void (*)() noexcept;
QObject *nativeInputAction() noexcept; // Qt thread, existing lifecycle registry

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
// Register only a Qt event observer before asynchronous host verification so
// unparented services created during startup are not lost. No native ABI calls.
void observeHostObjects() noexcept;
void initialize() noexcept;
void initialize(bool hostSupported, bool qtCoreSupported) noexcept;
void shutdown() noexcept;
void setRefreshNotifier(RefreshNotifier notifier) noexcept;
void markDirty() noexcept;
void markTopologyDirty() noexcept;
void markExplicitTopologyDirty() noexcept;
bool consumeTopologyDirty() noexcept;
std::uint64_t topologyEventCount() noexcept;
void markSelectionDirty() noexcept;
void notifyCursorChanged(void *cursor) noexcept;
bool refreshNeeded() noexcept;
bool refreshIncomplete() noexcept;
bool checkStructureChanged() noexcept;

// Refreshes the immutable chain binding snapshot. Must run on the Qt/control
// thread. Returns the number of verified bindings published.
std::size_t refresh() noexcept;
std::size_t refreshIfNeeded() noexcept;
bool refreshSelectionContext() noexcept;

// P12 diagnostics. Binding generation changes only when document/track/chain
// topology changes; selection generation changes only when the active
// document or selected track changes. These counters are monotonic for the
// lifetime of the process and are safe to sample from any thread.
std::uint64_t selectionGeneration() noexcept;
std::uint64_t bindingGeneration() noexcept;
std::uint64_t contextPublishLatencyNanoseconds() noexcept;
std::uint64_t droppedRefreshCount() noexcept;
bool hasActiveDocument() noexcept;

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
