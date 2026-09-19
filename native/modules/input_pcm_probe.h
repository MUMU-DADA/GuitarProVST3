#pragma once

#if !defined(GPVST3_P13_PROBE_BUILD)
#error P13 PCM observation is test-only and must not enter production builds.
#endif

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>

#include "input_probe.h"

namespace gpvst3::input::pcmprobe {

constexpr std::size_t kFrameCapacity = 262144;
constexpr std::size_t kRecordCapacity = 8192;
constexpr std::size_t kMaxCallbackFrames = 2048;
constexpr std::size_t kMaxChannels = 2;

using Metadata = probe::Metadata;
using Completion = probe::Completion;

struct Config {
    bool enabled = false;
    std::size_t frameCapacity = kFrameCapacity;
    std::size_t recordCapacity = kRecordCapacity;
};

enum class SampleStatus : std::uint8_t {
    Unvalidated, Missing, InvalidFormat, Captured, NonFinite,
};

enum Change : std::uint32_t {
    NoChange = 0,
    SequenceDiscontinuity = 1U << 0,
    OwnerChanged = 1U << 1,
    ChannelsChanged = 1U << 2,
    GenerationChanged = 1U << 3,
    SampleRateChanged = 1U << 4,
    DeviceChanged = 1U << 5,
    ThreadChanged = 1U << 6,
    MissingOwner = 1U << 7,
    DriverChannelsChanged = 1U << 8,
    ActualRateChanged = 1U << 9,
};

struct Samples {
    SampleStatus status = SampleStatus::Unvalidated;
    std::size_t frames = 0;
    std::size_t channels = 0;
    std::size_t nonFiniteCount = 0;
    std::size_t firstNonFinite = (std::numeric_limits<std::size_t>::max)();
};

struct Record {
    Metadata metadata{};
    Completion completion{};
    std::size_t frameOffset = 0;
    std::size_t savedFrames = 0;
    bool truncated = false;
    std::uint32_t changes = NoChange;
    Samples capture{};
    Samples postOriginal{};
};

struct Snapshot {
    Record record{};
    // Recorder-owned interleaved PCM, immutable until Recorder destruction.
    // Invalid/unvalidated sides have nullptr and zero sample counts. NonFinite
    // sides retain the original IEEE values and MUST NOT be treated as valid PCM.
    const float *capture = nullptr;
    const float *postOriginal = nullptr;
    std::size_t captureSamples = 0;
    std::size_t postOriginalSamples = 0;
};

class Recorder final {
public:
    class Ticket final {
    public:
        Ticket() = default;
        ~Ticket() { abandon(); }
        Ticket(const Ticket &) = delete;
        Ticket &operator=(const Ticket &) = delete;
        Ticket(Ticket &&other) noexcept
            : owner_(std::exchange(other.owner_, nullptr)), index_(other.index_) {}
        Ticket &operator=(Ticket &&other) noexcept {
            if (this != &other) {
                abandon();
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
        void abandon() noexcept {
            if (owner_) {
                auto *owner = std::exchange(owner_, nullptr);
                owner->abandoned_.fetch_add(1, std::memory_order_relaxed);
                owner->busy_.clear(std::memory_order_release);
            }
        }
        Recorder *owner_ = nullptr;
        std::size_t index_ = kRecordCapacity;
    };

    Recorder() = default;
    Recorder(const Recorder &) = delete;
    Recorder &operator=(const Recorder &) = delete;

    // Control thread, once, before any readers or callbacks. Disabled or failed
    // configuration also consumes this operation. No online reset or reuse.
    // Recorder must outlive its tickets and readers; teardown drains them first.
    bool configure(const Config &config = {}) noexcept {
        if (configured_.test_and_set(std::memory_order_relaxed)) return false;
        if (config.frameCapacity == 0 || config.frameCapacity > kFrameCapacity ||
            config.recordCapacity == 0 || config.recordCapacity > kRecordCapacity)
            return false;
        if (!config.enabled) return true;
        try {
            auto recordSlots = std::make_unique<Slot[]>(config.recordCapacity);
            auto capture = std::make_unique<float[]>(config.frameCapacity * kMaxChannels);
            auto output = std::make_unique<float[]>(config.frameCapacity * kMaxChannels);
            slots_ = std::move(recordSlots);
            capture_ = std::move(capture);
            output_ = std::move(output);
        } catch (...) {
            return false;
        }
        config_ = config;
        enabled_.store(true, std::memory_order_release);
        return true;
    }

    bool enabled() const noexcept { return enabled_.load(std::memory_order_acquire); }

    // Call before the original callback. One writer owns the entire ticket;
    // overlapping/reentrant callbacks skip immediately, without touching their
    // buffers. Copied capture remains valid even if original overwrites input.
    Ticket begin(const Metadata &metadata, const void *capture, bool validatedInput) noexcept {
        if (!enabled()) return {};
        if (busy_.test_and_set(std::memory_order_acquire)) {
            busyDrops_.fetch_add(1, std::memory_order_relaxed);
            return {};
        }
        const auto index = claimed_.load(std::memory_order_relaxed);
        const auto offset = framesReserved_.load(std::memory_order_relaxed);
        if (index == config_.recordCapacity || offset == config_.frameCapacity) {
            capacityDrops_.fetch_add(1, std::memory_order_relaxed);
            busy_.clear(std::memory_order_release);
            return {};
        }
        auto &record = slots_[index].record;
        record.metadata = metadata;
        record.frameOffset = offset;
        if (metadata.frames > 0 && metadata.frames <= kMaxCallbackFrames) {
            const auto remaining = config_.frameCapacity - offset;
            record.savedFrames = metadata.frames < remaining ? metadata.frames : remaining;
            record.truncated = record.savedFrames != metadata.frames;
        }
        record.changes = changes(metadata);
        record.capture = copy(capture, capture_.get() + offset * kMaxChannels,
            metadata, metadata.inputChannels, record.savedFrames, validatedInput);
        framesReserved_.store(offset + record.savedFrames, std::memory_order_relaxed);
        claimed_.store(index + 1, std::memory_order_relaxed);
        return Ticket(this, index);
    }

    // Call after the original returns. Output validity is a host-contract
    // decision, separate from pointer presence and callback result metadata.
    bool complete(Ticket &ticket, const Completion &completion, const void *output,
                  bool validatedOutput) noexcept {
        if (ticket.owner_ != this) return false;
        auto &slot = slots_[ticket.index_];
        auto &record = slot.record;
        record.completion = completion;
        record.postOriginal = copy(output, output_.get() + record.frameOffset * kMaxChannels,
            record.metadata, record.metadata.outputChannels, record.savedFrames, validatedOutput);
        previous_ = record.metadata;
        havePrevious_ = true;
        ticket.owner_ = nullptr;
        slot.published.store(true, std::memory_order_release);
        published_.fetch_add(1, std::memory_order_relaxed);
        busy_.clear(std::memory_order_release);
        return true;
    }

    // Control thread only. Failure leaves result untouched. Scan all claimed
    // indices: abandoned tickets are permanent unpublished holes, not zero PCM.
    bool snapshot(std::size_t index, Snapshot &result) const noexcept {
        if (!enabled() || index >= config_.recordCapacity ||
            !slots_[index].published.load(std::memory_order_acquire)) return false;
        Snapshot observed;
        observed.record = slots_[index].record;
        const auto offset = observed.record.frameOffset * kMaxChannels;
        observed.captureSamples = observed.record.capture.frames * observed.record.capture.channels;
        observed.postOriginalSamples = observed.record.postOriginal.frames * observed.record.postOriginal.channels;
        if (observed.captureSamples) observed.capture = capture_.get() + offset;
        if (observed.postOriginalSamples) observed.postOriginal = output_.get() + offset;
        result = observed;
        return true;
    }

    std::size_t claimed() const noexcept { return claimed_.load(std::memory_order_relaxed); }
    std::size_t published() const noexcept { return published_.load(std::memory_order_relaxed); }
    std::size_t framesReserved() const noexcept { return framesReserved_.load(std::memory_order_relaxed); }
    std::uint64_t busyDrops() const noexcept { return busyDrops_.load(std::memory_order_relaxed); }
    std::uint64_t capacityDrops() const noexcept { return capacityDrops_.load(std::memory_order_relaxed); }
    std::uint64_t abandoned() const noexcept { return abandoned_.load(std::memory_order_relaxed); }

private:
    static Samples copy(const void *source, float *target, const Metadata &metadata,
                        std::size_t channels, std::size_t frames, bool validated) noexcept {
        Samples result;
        if (!validated || !metadata.configurationValid) return result;
        if (metadata.frames == 0 || metadata.frames > kMaxCallbackFrames ||
            channels == 0 || channels > kMaxChannels ||
            !std::isfinite(metadata.sampleRate) || metadata.sampleRate <= 0.0) {
            result.status = SampleStatus::InvalidFormat;
            return result;
        }
        if (!source) {
            result.status = SampleStatus::Missing;
            return result;
        }
        result.status = SampleStatus::Captured;
        result.frames = frames;
        result.channels = channels;
        const auto *samples = static_cast<const float *>(source);
        for (std::size_t i = 0; i < frames * channels; ++i) {
            target[i] = samples[i];
            if (!std::isfinite(target[i])) {
                if (result.nonFiniteCount == 0) result.firstNonFinite = i;
                ++result.nonFiniteCount;
            }
        }
        if (result.nonFiniteCount) result.status = SampleStatus::NonFinite;
        return result;
    }

    std::uint32_t changes(const Metadata &current) const noexcept {
        std::uint32_t result = NoChange;
        if (current.sequence == 0 || (havePrevious_ &&
            (previous_.sequence == (std::numeric_limits<std::uint64_t>::max)() ||
             current.sequence != previous_.sequence + 1))) result |= SequenceDiscontinuity;
        if (current.ownerAddress == 0) result |= MissingOwner;
        if (havePrevious_) {
            if (current.ownerAddress != previous_.ownerAddress) result |= OwnerChanged;
            if (current.inputChannels != previous_.inputChannels ||
                current.outputChannels != previous_.outputChannels) result |= ChannelsChanged;
            if (current.streamGeneration != previous_.streamGeneration) result |= GenerationChanged;
            if (current.sampleRate != previous_.sampleRate) result |= SampleRateChanged;
            if (current.inputDevice != previous_.inputDevice ||
                current.outputDevice != previous_.outputDevice ||
                current.hostApiType != previous_.hostApiType) result |= DeviceChanged;
            if (current.thread != previous_.thread) result |= ThreadChanged;
            if (current.driverChannelsValidated != previous_.driverChannelsValidated ||
                current.driverInputSelectors != previous_.driverInputSelectors ||
                current.driverOutputSelectors != previous_.driverOutputSelectors) result |= DriverChannelsChanged;
            if (current.rateRevision != previous_.rateRevision ||
                current.actualRateValidated != previous_.actualRateValidated ||
                current.actualSampleRate != previous_.actualSampleRate) result |= ActualRateChanged;
        }
        return result;
    }

    struct Slot {
        Record record{};
        std::atomic<bool> published{false};
    };
    static_assert(std::atomic<bool>::is_always_lock_free &&
                  std::atomic<std::size_t>::is_always_lock_free &&
                  std::atomic<std::uint64_t>::is_always_lock_free,
                  "PCM diagnostic publication/counters require lock-free atomics");
    Config config_{};
    std::unique_ptr<Slot[]> slots_;
    std::unique_ptr<float[]> capture_;
    std::unique_ptr<float[]> output_;
    Metadata previous_{};
    bool havePrevious_ = false;
    std::atomic_flag configured_ = ATOMIC_FLAG_INIT;
    std::atomic_flag busy_ = ATOMIC_FLAG_INIT;
    std::atomic<bool> enabled_{false};
    std::atomic<std::size_t> claimed_{0}, published_{0}, framesReserved_{0};
    std::atomic<std::uint64_t> busyDrops_{0}, capacityDrops_{0}, abandoned_{0};
};

} // namespace gpvst3::input::pcmprobe
