#pragma once

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace gpvst3::input::probe {

// P13-0 observation only: no routing, native-input suppression or feasibility
// claim is implied by these records. Construct/configure before callbacks can
// enter; the recorder cannot be reset or configured again during its lifetime.
constexpr std::size_t kCapacity = 512;
constexpr std::size_t kSampleFrames = 16;
constexpr std::size_t kSampleChannels = 2;

struct Config {
    bool enabled = false;
    bool captureSamples = true;
};

struct Metadata {
    std::uint64_t sequence = 0;
    std::uint64_t parentSequence = 0;
    std::uint64_t timestampNanoseconds = 0;
    std::uint64_t streamGeneration = 0;
    std::uint64_t rateRevision = 0;
    double actualSampleRate = 0;
    bool actualRateValidated = false;
    std::uint64_t status = 0;
    std::size_t frames = 0;
    double sampleRate = 0.0;
    std::size_t inputChannels = 0;
    std::size_t outputChannels = 0;
    std::int32_t inputDevice = -1;
    std::int32_t outputDevice = -1;
    std::int32_t hostApiType = -1; // Unknown is distinct from a proven ASIO backend.
    std::uint32_t driverFrames = 0; // Unknown; never substitute callback frames.
    std::int32_t driverInputLatency = -1;
    std::int32_t driverOutputLatency = -1;
    std::array<std::int32_t, 2> driverInputSelectors{{-1, -1}};
    std::array<std::int32_t, 2> driverOutputSelectors{{-1, -1}};
    bool driverChannelsValidated = false;
    std::uint32_t thread = 0;
    std::uintptr_t inputAddress = 0;
    std::uintptr_t outputAddress = 0;
    std::uintptr_t ownerAddress = 0;
    bool configurationValid = false;
    bool pointersAlias = false;
    bool experiment = false;
    bool monitorReferenceIsInput1 = false;
};

struct Completion {
    std::uint64_t originalEndNanoseconds = 0;
    std::uint64_t callbackEndNanoseconds = 0;
    std::uint64_t originalNanoseconds = 0;
    std::uint64_t callbackNanoseconds = 0;
    std::int32_t callbackResult = 0;
};

enum class SampleStatus : std::uint8_t {
    Disabled,
    Unvalidated,
    Missing,
    InvalidFormat,
    Captured,
};

struct Samples {
    // Interleaved, with the original channel count; entries beyond frames *
    // channels remain zero. Non-finite values are zeroed and marked by bit.
    std::array<float, kSampleFrames * kSampleChannels> values{};
    std::uint32_t nonFiniteMask = 0;
    std::uint8_t frames = 0;
    std::uint8_t channels = 0;
    SampleStatus status = SampleStatus::Unvalidated;
};

struct Record {
    Metadata metadata{};
    Completion completion{};
    Samples capture{};
    Samples postOriginal{};
};

class Recorder final {
public:
    class Ticket final {
    public:
        Ticket() = default;
        Ticket(const Ticket &) = delete;
        Ticket &operator=(const Ticket &) = delete;
        Ticket(Ticket &&other) noexcept
            : owner_(std::exchange(other.owner_, nullptr)), index_(other.index_) {}
        Ticket &operator=(Ticket &&other) noexcept {
            if (this != &other) {
                // An abandoned ticket remains unpublished and is never reused.
                owner_ = std::exchange(other.owner_, nullptr);
                index_ = other.index_;
            }
            return *this;
        }
        explicit operator bool() const noexcept { return owner_ != nullptr; }
        std::size_t index() const noexcept { return index_; }
    private:
        friend class Recorder;
        Ticket(Recorder *owner, std::size_t index) noexcept : owner_(owner), index_(index) {}
        Recorder *owner_ = nullptr;
        std::size_t index_ = kCapacity;
    };

    Recorder() = default;
    Recorder(const Recorder &) = delete;
    Recorder &operator=(const Recorder &) = delete;

    // Control thread only, before any callback or reader. Even configure({})
    // consumes this one-time operation; there is deliberately no online reset.
    bool configure(Config config) noexcept {
        if (configured_.test_and_set(std::memory_order_relaxed)) return false;
        captureSamples_ = config.captureSamples;
        enabled_.store(config.enabled, std::memory_order_release);
        return true;
    }

    bool enabled() const noexcept { return enabled_.load(std::memory_order_acquire); }

    // Any callback thread may claim a unique, permanently owned slot. A ticket
    // itself has one writer and may not be used concurrently from two threads.
    Ticket claim(const Metadata &metadata) noexcept {
        if (!enabled() || next_.load(std::memory_order_relaxed) >= kCapacity) return {};
        const auto index = next_.fetch_add(1, std::memory_order_relaxed);
        if (index >= kCapacity) return {};
        slots_[index].record.metadata = metadata;
        return Ticket(this, index);
    }

    bool captureInput(Ticket &ticket, const void *input, bool validatedInput) noexcept {
        if (ticket.owner_ != this) return false;
        auto &record = slots_[ticket.index_].record;
        record.capture = samples(input, record.metadata.frames, record.metadata.inputChannels,
            validatedInput && record.metadata.configurationValid);
        return true;
    }

    // Call only after the original callback returned. validPostOriginalOutput
    // must come from the host contract; a non-null pointer alone is insufficient.
    // Passing false never dereferences output, including for aliasing pointers.
    bool complete(Ticket &ticket, const Completion &completion, const void *output,
                  bool validPostOriginalOutput) noexcept {
        if (ticket.owner_ != this) return false;
        auto &slot = slots_[ticket.index_];
        slot.record.completion = completion;
        slot.record.postOriginal = samples(output, slot.record.metadata.frames,
            slot.record.metadata.outputChannels,
            validPostOriginalOutput && slot.record.metadata.configurationValid);
        ticket.owner_ = nullptr;
        slot.published.store(true, std::memory_order_release);
        published_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    std::size_t claimed() const noexcept {
        const auto count = next_.load(std::memory_order_relaxed);
        return count < kCapacity ? count : kCapacity;
    }
    std::size_t published() const noexcept { return published_.load(std::memory_order_relaxed); }

    // Slots may publish out of order or remain unpublished if their callback
    // never completes. Scan indices, not a prefix of published() records.
    bool snapshot(std::size_t index, Record &record) const noexcept {
        if (index >= kCapacity || !slots_[index].published.load(std::memory_order_acquire))
            return false;
        record = slots_[index].record;
        return true;
    }

private:
    Samples samples(const void *source, std::size_t frames, std::size_t channels,
                    bool validated) const noexcept {
        Samples result;
        if (!captureSamples_) { result.status = SampleStatus::Disabled; return result; }
        if (!validated) return result;
        if (!source) { result.status = SampleStatus::Missing; return result; }
        if (frames == 0 || channels == 0 || channels > kSampleChannels) {
            result.status = SampleStatus::InvalidFormat;
            return result;
        }
        result.frames = static_cast<std::uint8_t>(frames < kSampleFrames ? frames : kSampleFrames);
        result.channels = static_cast<std::uint8_t>(channels);
        result.status = SampleStatus::Captured;
        const auto *values = static_cast<const float *>(source);
        const auto count = static_cast<std::size_t>(result.frames) * channels;
        for (std::size_t index = 0; index < count; ++index) {
            const auto value = values[index];
            if (std::isfinite(value)) result.values[index] = value;
            else result.nonFiniteMask |= std::uint32_t{1} << index;
        }
        return result;
    }

    struct Slot {
        Record record{};
        std::atomic<bool> published{false};
    };
    static_assert(std::atomic<std::size_t>::is_always_lock_free,
                  "The diagnostic callback requires lock-free counters");
    static_assert(std::atomic<bool>::is_always_lock_free,
                  "The diagnostic callback requires lock-free publication");
    std::array<Slot, kCapacity> slots_{};
    std::atomic_flag configured_ = ATOMIC_FLAG_INIT;
    std::atomic<bool> enabled_{false};
    std::atomic<std::size_t> next_{0};
    std::atomic<std::size_t> published_{0};
    bool captureSamples_ = true;
};

} // namespace gpvst3::input::probe
