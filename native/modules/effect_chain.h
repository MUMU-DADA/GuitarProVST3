#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace gpvst3::audio {
struct BlockView;
}

namespace gpvst3::effects {

// A fixed-size chain handoff. Slot preparation and activation happen on a
// worker/control thread. process() only reads atomics and invokes an already
// prepared callback, so the audio thread never allocates or waits on a mutex.
class Chain final {
public:
    using ProcessFn = bool (*)(void *, const audio::BlockView &) noexcept;

    struct SlotConfig {
        void *context = nullptr;
        ProcessFn process = nullptr;
    };

    struct ProcessResult {
        bool completed = false;
        bool bypassed = false;
        bool error = false;
        std::uint64_t elapsedNanoseconds = 0;
    };

    struct Snapshot {
        bool bypassed = true;
        bool faulted = false;
        int activeSlot = -1;
        std::size_t preparedSlots = 0;
        std::size_t processBlocks = 0;
        std::size_t processedBlocks = 0;
        std::size_t bypassBlocks = 0;
        std::size_t errorBlocks = 0;
        std::size_t fallbackBlocks = 0;
        std::uint64_t lastProcessNanoseconds = 0;
        std::uint64_t maxProcessNanoseconds = 0;
        std::uint64_t totalProcessNanoseconds = 0;
        std::uint64_t deadlineNanoseconds = 0;
        std::size_t deadlineExceededBlocks = 0;
        std::size_t switchCount = 0;
        std::size_t switchRequests = 0;
        std::size_t switchPrepared = 0;
        std::size_t retiredSlots = 0;
        std::uint64_t lastSwitchNanoseconds = 0;
        std::uint64_t maxSwitchNanoseconds = 0;
        std::uint64_t lastReaderDrainNanoseconds = 0;
        std::uint64_t maxReaderDrainNanoseconds = 0;
        std::size_t readerDrainTimeouts = 0;
        std::size_t sequenceGaps = 0;
        std::uint64_t lastSequence = 0;
        std::size_t rampSamples = 128;
        std::size_t rampRemaining = 0;
    };

    Chain() = default;
    Chain(const Chain &) = delete;
    Chain &operator=(const Chain &) = delete;

    // The target slot must be inactive. It is retired and drained before its
    // callback is replaced, which keeps a processor instance alive while an
    // audio call is using it.
    bool prepareSlot(std::size_t index, SlotConfig config) noexcept;
    bool activate(std::size_t index) noexcept;
    void deactivate() noexcept;

    void setBypassed(bool value) noexcept;
    // Configure a short block-boundary gain transition used when a prepared
    // slot becomes active. The default is intentionally small enough for a
    // realtime block while avoiding a hard step at the handoff.
    void setRampSamples(std::size_t samples) noexcept;
    // Optional control-thread deadline override. A zero value makes process()
    // derive the current block duration from sampleRate/frameCount.
    void setDeadlineNanoseconds(std::uint64_t nanoseconds) noexcept;
    bool bypassed() const noexcept { return bypassed_.load(std::memory_order_acquire); }
    void clearFault() noexcept;
    bool faulted() const noexcept { return faulted_.load(std::memory_order_acquire); }

    ProcessResult process(const audio::BlockView &block) noexcept;
    Snapshot snapshot() const noexcept;

private:
    struct Slot {
        std::atomic<bool> accepting{false};
        std::atomic<bool> configured{false};
        std::atomic<std::size_t> readers{0};
        SlotConfig config{};
    };

    static constexpr std::size_t kSlotCount = 2;

    bool acquire(Slot &slot, int index) noexcept;
    void waitForReaders(Slot &slot) noexcept;
    void applyRamp(const audio::BlockView &block) noexcept;

    Slot slots_[kSlotCount];
    std::atomic<int> activeSlot_{-1};
    std::atomic<bool> requestedBypass_{true};
    std::atomic<bool> bypassed_{true};
    std::atomic<bool> faulted_{false};
    std::atomic<std::size_t> processBlocks_{0};
    std::atomic<std::size_t> processedBlocks_{0};
    std::atomic<std::size_t> bypassBlocks_{0};
    std::atomic<std::size_t> errorBlocks_{0};
    std::atomic<std::size_t> fallbackBlocks_{0};
    std::atomic<std::uint64_t> lastProcessNanoseconds_{0};
    std::atomic<std::uint64_t> maxProcessNanoseconds_{0};
    std::atomic<std::uint64_t> totalProcessNanoseconds_{0};
    std::atomic<std::uint64_t> deadlineNanoseconds_{0};
    std::atomic<std::uint64_t> deadlineOverrideNanoseconds_{0};
    std::atomic<std::size_t> deadlineExceededBlocks_{0};
    std::atomic<std::size_t> switchCount_{0};
    std::atomic<std::size_t> switchRequests_{0};
    std::atomic<std::size_t> switchPrepared_{0};
    std::atomic<std::size_t> retiredSlots_{0};
    std::atomic<std::uint64_t> lastSwitchNanoseconds_{0};
    std::atomic<std::uint64_t> maxSwitchNanoseconds_{0};
    std::atomic<std::uint64_t> lastReaderDrainNanoseconds_{0};
    std::atomic<std::uint64_t> maxReaderDrainNanoseconds_{0};
    std::atomic<std::size_t> readerDrainTimeouts_{0};
    std::atomic<std::size_t> sequenceGaps_{0};
    std::atomic<std::uint64_t> lastSequence_{0};
    std::atomic<std::size_t> rampSamples_{128};
    std::atomic<std::size_t> rampRemaining_{0};
};

}
