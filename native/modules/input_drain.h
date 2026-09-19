#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace gpvst3::input::drain {

// Counts native pipeline history; it neither suppresses native audio nor
// activates an overlay. The caller must first prove the matching AMAudio
// binary, object lifetimes, unique producer/consumer and callback serialization.
// All methods have one owner thread, with no concurrent readers. Publish a copy
// of Snapshot outside this object if another thread needs the result.
struct ConvolverTopology {
    std::uint32_t upFactor = 0;
    std::uint32_t downFactor = 0;
    std::uint32_t blockLen2 = 0;
    std::uint32_t previousInputLen = 0;
    std::uint32_t inputLen = 0;
    std::uint32_t latency = 0;
    std::uint32_t inputDelay = 0;
    std::uint32_t upShift = 0;
    std::uint32_t downShift = 0;
    bool consumesLatency = false;
};

struct ChannelTopology : ConvolverTopology {
    std::uint32_t convolverCount = 0;
    std::uint32_t interpolatorSourceRate = 0;
    std::uint32_t interpolatorDestinationRate = 0;
    // 44100 -> 176400 uses two 2x convolvers without an interpolator.
    ConvolverTopology second{};
};

inline bool supportedDestinationRate(double rate) noexcept {
    return rate == 44100 || rate == 48000 || rate == 88200 ||
        rate == 96000 || rate == 176400 || rate == 192000;
}

struct Config {
    // A new stream, SRC/ring object, topology or drain request needs a fresh
    // object and epoch. Zero is never a verified epoch.
    std::uint64_t epoch = 0;
    std::uint32_t channels = 0;
    std::uint32_t sourceRate = 0;
    std::uint32_t destinationRate = 0;
    std::uint32_t ringCapacitySamples = 0;
    std::uint32_t maxSrcInputFrames = 0;
    std::uint32_t maxCallbackFrames = 0;
    // At 44100 Hz GP omits output SRC and the shared output ring entirely.
    bool usesOutputRing = true;
    std::array<ChannelTopology, 2> channel{};
};

enum class State : std::uint8_t { Invalid, Preparing, Ready };
enum class Phase : std::uint8_t { Invalid, Convolver, Interpolator, Ring, NextCallback, Ready };
enum class Error : std::uint8_t {
    None, Unconfigured, InvalidConfig, Reconfigured, ConfigChanged,
    CallbackOrder, SuppressionLost, InvalidCount, DuplicateSrc,
    RingDiscontinuity, InvalidSrc, MissingSrc, InvalidRingEvidence,
};

struct Snapshot {
    State state = State::Invalid;
    Phase phase = Phase::Invalid;
    Error error = Error::Unconfigured;
    std::uint64_t convolverTarget = 0;
    std::uint64_t interpolatorTarget = 0;
    std::uint64_t convolverFrames = 0;
    std::uint64_t interpolatorFrames = 0;
    std::uint64_t oldRingSamples = 0;
};

class Tracker final {
public:
    Tracker() = default;
    Tracker(const Tracker &) = delete;
    Tracker &operator=(const Tracker &) = delete;

    // One shot, before observations. Invalid is sticky after configuration;
    // retries use a new Tracker. No allocation, locks, host reads or clock.
    bool configure(const Config &config) noexcept {
        if (configured_) return fail(Error::Reconfigured);
        configured_ = true;
        if (!validConfig(config)) return fail(Error::InvalidConfig);
        config_ = config;
        for (std::size_t i = 0; i < config.channels; ++i) {
            const auto &c = config.channel[i];
            const auto a = std::uint64_t{3} * (c.inputLen / 2);
            // The second phase consumes clean output of the first convolver.
            // Its worst startup discard is L1, so n source frames supply at
            // least max(0, 2*n-L1) clean frames to the following stage.
            const auto followingHistory = c.convolverCount == 2
                ? std::uint64_t{3} * (c.second.inputLen / 2)
                : (c.interpolatorSourceRate ? std::uint64_t{256} : 0);
            const auto b = followingHistory ? (followingHistory + c.latency + 1) / 2 : 0;
            if (a > snapshot_.convolverTarget) snapshot_.convolverTarget = a;
            if (b > snapshot_.interpolatorTarget) snapshot_.interpolatorTarget = b;
        }
        snapshot_.state = State::Preparing;
        snapshot_.phase = config.usesOutputRing ? Phase::Convolver : Phase::NextCallback;
        snapshot_.error = Error::None;
        return true;
    }

    // observed must describe the ACTUAL current objects, not merely echo config.
    // Every callback, including those skipping SRC, must be observed in order.
    // nativeSuppressed means the same callback snapshot suppresses the complete
    // native listener contribution before it reaches the shared output SRC.
    // Ready is promoted only here, after validating the next callback's entry.
    bool beginCallback(const Config &observed, std::uint64_t sequence,
                       std::uint64_t callbackFrames, std::uint64_t queuedBefore,
                       bool nativeSuppressed) noexcept {
        if (!usable()) return false;
        if (!sameConfig(config_, observed)) return fail(Error::ConfigChanged);
        if (open_ || sequence == 0 ||
            (seenCallback_ && (lastSequence_ == (std::numeric_limits<std::uint64_t>::max)() ||
                               sequence != lastSequence_ + 1)))
            return fail(Error::CallbackOrder);
        if (!nativeSuppressed) return fail(Error::SuppressionLost);
        if (callbackFrames > config_.maxCallbackFrames || !validQueue(queuedBefore))
            return fail(Error::InvalidCount);
        if (!config_.usesOutputRing && queuedBefore != 0) return fail(Error::InvalidRingEvidence);
        if (seenCallback_ && queuedBefore != lastQueuedAfter_)
            return fail(Error::RingDiscontinuity);
        open_ = true;
        seenCallback_ = true;
        lastSequence_ = sequence;
        callbackFrames_ = callbackFrames;
        queuedBefore_ = queuedBefore;
        appended_ = 0;
        srcSeen_ = false;
        if (snapshot_.phase == Phase::NextCallback) {
            snapshot_.phase = Phase::Ready;
            snapshot_.state = State::Ready;
        }
        return true;
    }

    // Observe the one actual output StreamSampleRateConverter::process return.
    // Never substitute ASIO frames, input-SRC frames or nominal block length.
    // An absent call is legal; a second call violates this binary's contract.
    // All output from a call crossing either phase threshold is treated as old:
    // surplus input cannot advance the next phase in the same call.
    bool srcCompleted(std::uint64_t actualInputFrames,
                      std::uint64_t returnedOutputFrames) noexcept {
        if (!usable()) return false;
        if (!open_) return fail(Error::CallbackOrder);
        if (!config_.usesOutputRing) return fail(Error::InvalidSrc);
        if (srcSeen_) return fail(Error::DuplicateSrc);
        srcSeen_ = true;
        if (queuedBefore_ >= callbackFrames_ * config_.channels ||
            actualInputFrames > config_.maxSrcInputFrames ||
            returnedOutputFrames > config_.ringCapacitySamples / config_.channels ||
            (actualInputFrames == 0 && returnedOutputFrames != 0))
            return fail(Error::InvalidSrc);
        // The bounds above make multiplication/addition safe. Reject an
        // overflowing physical ring even if the subsequent read would hide it.
        appended_ = returnedOutputFrames * config_.channels;
        if (appended_ > config_.ringCapacitySamples - queuedBefore_)
            return fail(Error::InvalidRingEvidence);
        if (snapshot_.phase == Phase::Convolver) {
            advance(snapshot_.convolverFrames, snapshot_.convolverTarget, actualInputFrames);
            if (snapshot_.convolverFrames == snapshot_.convolverTarget) {
                if (snapshot_.interpolatorTarget) snapshot_.phase = Phase::Interpolator;
                else captureFrontier_ = true;
            }
        } else if (snapshot_.phase == Phase::Interpolator) {
            interpolatorSawOutput_ = interpolatorSawOutput_ || returnedOutputFrames != 0;
            advance(snapshot_.interpolatorFrames, snapshot_.interpolatorTarget, actualInputFrames);
            if (snapshot_.interpolatorFrames == snapshot_.interpolatorTarget) {
                // The following stage must produce output over the bounded
                // phase; missing output evidence is not successful drain.
                if (!interpolatorSawOutput_) return fail(Error::InvalidSrc);
                captureFrontier_ = true;
            }
        }
        return true;
    }

    // Called only AFTER the complete original callback (SRC output appended,
    // ring read finished). consumedSamples is independently observed real ring
    // consumption, not an assumed device frame count. Verify it against this
    // binary's min(available, requested) rule and both ring snapshots.
    bool endCallback(const Config &observed, std::uint64_t queuedAfter,
                     std::uint64_t consumedSamples) noexcept {
        if (!usable()) return false;
        if (!open_) return fail(Error::CallbackOrder);
        if (!sameConfig(config_, observed)) return fail(Error::ConfigChanged);
        if (!config_.usesOutputRing) {
            if (srcSeen_ || queuedAfter || consumedSamples) return fail(Error::InvalidRingEvidence);
            open_ = false;
            lastQueuedAfter_ = 0;
            return true;
        }
        const auto available = queuedBefore_ + appended_;
        const auto requested = callbackFrames_ * config_.channels;
        if (queuedBefore_ < requested && !srcSeen_) return fail(Error::MissingSrc);
        const auto expectedConsumed = available < requested ? available : requested;
        if (!validQueue(queuedAfter) || consumedSamples != expectedConsumed ||
            queuedAfter != available - expectedConsumed)
            return fail(Error::InvalidRingEvidence);
        open_ = false;
        lastQueuedAfter_ = queuedAfter;
        if (captureFrontier_) {
            // This callback can itself emit old samples. Only its remaining
            // queue defines the frontier; never subtract its consumption again.
            captureFrontier_ = false;
            snapshot_.oldRingSamples = queuedAfter;
            snapshot_.phase = Phase::Ring;
        } else if (snapshot_.phase == Phase::Ring) {
            snapshot_.oldRingSamples = consumedSamples >= snapshot_.oldRingSamples
                ? 0 : snapshot_.oldRingSamples - consumedSamples;
        }
        if (snapshot_.phase == Phase::Ring && snapshot_.oldRingSamples == 0)
            snapshot_.phase = Phase::NextCallback;
        // Never transition state to Ready at a callback's end, even for Q=0.
        return true;
    }

    Snapshot snapshot() const noexcept { return snapshot_; }

private:
    static bool sameConvolver(const ConvolverTopology &a, const ConvolverTopology &b) noexcept {
        return a.upFactor == b.upFactor &&
            a.downFactor == b.downFactor && a.blockLen2 == b.blockLen2 &&
            a.previousInputLen == b.previousInputLen && a.inputLen == b.inputLen &&
            a.latency == b.latency && a.inputDelay == b.inputDelay &&
            a.upShift == b.upShift && a.downShift == b.downShift && a.consumesLatency == b.consumesLatency;
    }

    static bool sameChannel(const ChannelTopology &a, const ChannelTopology &b) noexcept {
        return sameConvolver(a, b) && sameConvolver(a.second, b.second) &&
            a.convolverCount == b.convolverCount &&
            a.interpolatorSourceRate == b.interpolatorSourceRate &&
            a.interpolatorDestinationRate == b.interpolatorDestinationRate;
    }

    static bool sameConfig(const Config &a, const Config &b) noexcept {
        if (a.epoch != b.epoch || a.channels != b.channels || a.sourceRate != b.sourceRate ||
            a.destinationRate != b.destinationRate || a.ringCapacitySamples != b.ringCapacitySamples ||
            a.maxSrcInputFrames != b.maxSrcInputFrames || a.maxCallbackFrames != b.maxCallbackFrames ||
            a.usesOutputRing != b.usesOutputRing)
            return false;
        for (std::size_t i = 0; i < a.channels; ++i)
            if (!sameChannel(a.channel[i], b.channel[i])) return false;
        return true;
    }

    static bool validConfig(const Config &c) noexcept {
        if (c.epoch == 0 || c.channels == 0 || c.channels > 2 ||
            c.sourceRate != 44100 || !supportedDestinationRate(c.destinationRate) ||
            c.ringCapacitySamples != 32768 || c.maxSrcInputFrames == 0 ||
            c.maxSrcInputFrames > 32768 || c.maxCallbackFrames == 0 ||
            c.maxCallbackFrames > 32768) return false;
        const auto validConvolver = [](const ConvolverTopology &t) {
            const auto block = std::uint64_t{t.inputLen} / 2;
            return !(t.upFactor != 2 || t.downFactor != 1 ||
                !t.consumesLatency || t.inputDelay != 0 || t.upShift != 1 || t.downShift != 0 ||
                t.blockLen2 < 2 || (t.blockLen2 & (t.blockLen2 - 1)) != 0 ||
                t.inputLen == 0 || (t.inputLen & 1) != 0 || t.previousInputLen > block ||
                block + t.previousInputLen != std::uint64_t{t.blockLen2} / 2 ||
                t.latency < t.inputLen || t.latency > std::uint64_t{t.inputLen} + t.blockLen2);
        };
        if (c.usesOutputRing != (c.destinationRate != c.sourceRate)) return false;
        for (std::size_t i = 0; i < c.channels; ++i) {
            const auto &t = c.channel[i];
            if (!c.usesOutputRing) {
                if (t.convolverCount || t.interpolatorSourceRate || t.interpolatorDestinationRate) return false;
                continue;
            }
            const auto stages = c.destinationRate == 176400 ? 2U : 1U;
            const bool interpolates = c.destinationRate != 88200 && c.destinationRate != 176400;
            if (t.convolverCount != stages || !validConvolver(t) ||
                (stages == 2 && !validConvolver(t.second)) ||
                t.interpolatorSourceRate != (interpolates ? 88200U : 0U) ||
                t.interpolatorDestinationRate != (interpolates ? c.destinationRate : 0U))
                return false;
        }
        return true;
    }

    bool validQueue(std::uint64_t count) const noexcept {
        return count <= config_.ringCapacitySamples && count % config_.channels == 0;
    }
    bool usable() const noexcept { return snapshot_.state != State::Invalid; }
    bool fail(Error error) noexcept {
        snapshot_.state = State::Invalid;
        snapshot_.phase = Phase::Invalid;
        snapshot_.error = error;
        open_ = false;
        captureFrontier_ = false;
        return false;
    }
    static void advance(std::uint64_t &count, std::uint64_t target, std::uint64_t amount) noexcept {
        // Saturating to a small proven target avoids lifetime counter overflow.
        count += amount < target - count ? amount : target - count;
    }

    Config config_{};
    Snapshot snapshot_{};
    std::uint64_t lastSequence_ = 0;
    std::uint64_t lastQueuedAfter_ = 0;
    std::uint64_t callbackFrames_ = 0;
    std::uint64_t queuedBefore_ = 0;
    std::uint64_t appended_ = 0;
    bool configured_ = false;
    bool seenCallback_ = false;
    bool open_ = false;
    bool srcSeen_ = false;
    bool captureFrontier_ = false;
    bool interpolatorSawOutput_ = false;
};

} // namespace gpvst3::input::drain
