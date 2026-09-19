#pragma once

#include "input_drain.h"
#include "portaudio_capture_abi.h"

#include <array>
#include <cmath>
#include <cstdint>

namespace gpvst3::input::drainprobe {

// Only call inside the hash-gated AMAudio callback, after the ASIO/owner ABI
// checks. The caller owns the borrowed lifetime and excludes concurrent native
// close/reconfigure. Pointer comparisons below cannot establish that lifetime.
// No host functions, allocation, locks, Qt or remote-memory reads are used.
struct Context {
    std::uint64_t epoch = 0;
    std::uint64_t generation = 0;
    std::uint64_t rateRevision = 0;
    double actualRate = 0;
    bool rateValidated = false;
};

enum class Error : std::uint8_t {
    None, InvalidContext, MissingObject, UnexpectedVtable, InvalidTopology,
    InvalidDynamicBounds, InvalidRing,
};

struct Channel {
    const void *resampler = nullptr;
    const void *convolver = nullptr;
    const void *filter = nullptr;
    const void *interpolator = nullptr;
    std::int32_t kernelLength = 0;
    std::int32_t blockLengthBits = 0;
    std::int32_t inDataLeft = 0;
    std::int32_t latencyLeft = 0;
    std::int32_t bufferLeft = 0;
    std::int32_t writePosition = 0;
    std::int32_t readPosition = 0;
    double latencyFraction = 0;
};

struct Ring {
    std::uint64_t capacity = 0;
    std::uint64_t mask = 0;
    std::uint64_t queued = 0;
    const void *storage = nullptr;
    std::uint64_t readIndex = 0;
    std::uint64_t writeIndex = 0;
};

struct Snapshot {
    drain::Config config{};
    Context context{};
    const void *module = nullptr;
    const void *owner = nullptr;
    const void *stream = nullptr;
    const void *outputSrc = nullptr;
    const void *srcImpl = nullptr;
    std::array<Channel, 2> channels{};
    Ring ring{};
    Error error = Error::InvalidContext;
};

inline bool readSnapshot(const void *module, const void *owner,
                         const Context &context, Snapshot &result) noexcept {
    using hook::portaudio::read;
    result = {};
    result.context = context;
    result.module = module;
    result.owner = owner;
    const auto fail = [&](Error error) { result.error = error; return false; };
    if (!module || !owner || !context.epoch || !context.generation || !context.rateRevision ||
        !context.rateValidated || context.actualRate != 192000)
        return fail(Error::InvalidContext);
    const auto *base = static_cast<const std::uint8_t *>(module);
    result.stream = read<const void *>(owner, 8);
    result.outputSrc = read<const void *>(owner, 0x20);
    if (!result.stream || !result.outputSrc) return fail(Error::MissingObject);
    result.srcImpl = read<const void *>(result.outputSrc, 0);
    if (!result.srcImpl) return fail(Error::MissingObject);

    auto &config = result.config;
    config.epoch = context.epoch;
    config.channels = 2;
    config.sourceRate = 44100;
    config.destinationRate = 192000;
    config.ringCapacitySamples = 32768;
    config.maxSrcInputFrames = 32768;
    config.maxCallbackFrames = std::uint32_t(hook::portaudio::kMaxFrames);
    if (read<std::int32_t>(module, 0x2507F0) != 2)
        return fail(Error::InvalidTopology);
    for (std::size_t index = 0; index < result.channels.size(); ++index) {
        auto &channel = result.channels[index];
        auto &topology = config.channel[index];
        channel.resampler = static_cast<const std::uint8_t *>(result.srcImpl) + index * 0x78;
        if (read<const void *>(channel.resampler, 0) != base + 0x194608)
            return fail(Error::UnexpectedVtable);
        if (read<std::int32_t>(channel.resampler, 0x48) != 1)
            return fail(Error::InvalidTopology);
        topology.convolverCount = 1;
        channel.convolver = read<const void *>(channel.resampler, 8);
        channel.interpolator = read<const void *>(channel.resampler, 0x50);
        if (!channel.convolver || !channel.interpolator) return fail(Error::MissingObject);
        if (read<const void *>(channel.convolver, 0) != base + 0x194588 ||
            read<const void *>(channel.interpolator, 0) != base + 0x194648)
            return fail(Error::UnexpectedVtable);
        const auto *convolver = channel.convolver;
        topology.upFactor = read<std::uint32_t>(convolver, 0x28);
        topology.downFactor = read<std::uint32_t>(convolver, 0x2C);
        topology.consumesLatency = read<std::uint8_t>(convolver, 0x30) == 1;
        topology.blockLen2 = read<std::uint32_t>(convolver, 0x34);
        topology.previousInputLen = read<std::uint32_t>(convolver, 0x3C);
        topology.inputLen = read<std::uint32_t>(convolver, 0x40);
        topology.latency = read<std::uint32_t>(convolver, 0x44);
        topology.upShift = read<std::uint32_t>(convolver, 0x50);
        topology.downShift = read<std::uint32_t>(convolver, 0x54);
        topology.inputDelay = read<std::uint32_t>(convolver, 0x58);
        channel.filter = read<const void *>(convolver, 8);
        if (!channel.filter) return fail(Error::MissingObject);
        channel.kernelLength = read<std::int32_t>(channel.filter, 0x48);
        channel.blockLengthBits = read<std::int32_t>(channel.filter, 0x4C);
        if (channel.kernelLength <= 0 || channel.blockLengthBits < 0 || channel.blockLengthBits > 29 ||
            (std::uint64_t{2} << channel.blockLengthBits) != topology.blockLen2 ||
            std::uint32_t((channel.kernelLength - 1) / 2) != topology.previousInputLen)
            return fail(Error::InvalidTopology);
        const auto sourceRate = read<double>(channel.interpolator, 0x1008);
        const auto destinationRate = read<double>(channel.interpolator, 0x1010);
        if (sourceRate != 88200 || destinationRate != context.actualRate)
            return fail(Error::InvalidTopology);
        topology.interpolatorSourceRate = 88200;
        topology.interpolatorDestinationRate = 192000;
        channel.inDataLeft = read<std::int32_t>(convolver, 0x80);
        channel.latencyLeft = read<std::int32_t>(convolver, 0x84);
        channel.latencyFraction = read<double>(convolver, 0x48);
        channel.bufferLeft = read<std::int32_t>(channel.interpolator, 0x1020);
        channel.writePosition = read<std::int32_t>(channel.interpolator, 0x1024);
        channel.readPosition = read<std::int32_t>(channel.interpolator, 0x1028);
        if (channel.inDataLeft < 0 || std::uint32_t(channel.inDataLeft) > topology.inputLen ||
            channel.latencyLeft < 0 || std::uint32_t(channel.latencyLeft) > topology.latency ||
            !std::isfinite(channel.latencyFraction) || channel.latencyFraction < 0 || channel.latencyFraction >= 1 ||
            channel.bufferLeft < 0 || channel.bufferLeft > 245 ||
            channel.writePosition < 0 || channel.writePosition >= 256 ||
            channel.readPosition < 0 || channel.readPosition >= 256)
            return fail(Error::InvalidDynamicBounds);
    }
    // Reuse the actual counter model's topology contract rather than keeping a
    // second, subtly different copy of its arithmetic admissibility rules.
    drain::Tracker validator;
    if (!validator.configure(config)) return fail(Error::InvalidTopology);
    const auto *ring = base + 0x270810;
    result.ring.capacity = read<std::uint64_t>(ring, 0);
    result.ring.mask = read<std::uint64_t>(ring, 8);
    const auto count = read<std::int64_t>(ring, 0x10);
    result.ring.storage = read<const void *>(ring, 0x18);
    result.ring.readIndex = read<std::uint64_t>(ring, 0x30);
    result.ring.writeIndex = read<std::uint64_t>(ring, 0x38);
    if (result.ring.capacity != 32768 || result.ring.mask != 32767 || !result.ring.storage ||
        count < 0 || count > 32768 || (count & 1) ||
        result.ring.readIndex > result.ring.mask || result.ring.writeIndex > result.ring.mask)
        return fail(Error::InvalidRing);
    result.ring.queued = std::uint64_t(count);
    if (((result.ring.readIndex + result.ring.queued) & result.ring.mask) != result.ring.writeIndex)
        return fail(Error::InvalidRing);
    result.error = Error::None;
    return true;
}

inline bool sameTopology(const Snapshot &a, const Snapshot &b) noexcept {
    if (a.error != Error::None || b.error != Error::None ||
        a.context.epoch != b.context.epoch || a.context.generation != b.context.generation ||
        a.context.rateRevision != b.context.rateRevision ||
        a.context.actualRate != b.context.actualRate || !a.context.rateValidated || !b.context.rateValidated ||
        a.module != b.module || a.owner != b.owner || a.stream != b.stream ||
        a.outputSrc != b.outputSrc || a.srcImpl != b.srcImpl ||
        a.ring.storage != b.ring.storage || a.ring.capacity != b.ring.capacity || a.ring.mask != b.ring.mask ||
        a.config.epoch != b.config.epoch || a.config.channels != b.config.channels ||
        a.config.sourceRate != b.config.sourceRate || a.config.destinationRate != b.config.destinationRate ||
        a.config.ringCapacitySamples != b.config.ringCapacitySamples ||
        a.config.maxSrcInputFrames != b.config.maxSrcInputFrames || a.config.maxCallbackFrames != b.config.maxCallbackFrames)
        return false;
    for (std::size_t index = 0; index < a.channels.size(); ++index) {
        const auto &x = a.channels[index];
        const auto &y = b.channels[index];
        const auto &s = a.config.channel[index];
        const auto &t = b.config.channel[index];
        if (x.resampler != y.resampler || x.convolver != y.convolver || x.filter != y.filter ||
            x.interpolator != y.interpolator || x.kernelLength != y.kernelLength ||
            x.blockLengthBits != y.blockLengthBits || x.latencyFraction != y.latencyFraction ||
            s.convolverCount != t.convolverCount || s.upFactor != t.upFactor || s.downFactor != t.downFactor ||
            s.blockLen2 != t.blockLen2 || s.previousInputLen != t.previousInputLen || s.inputLen != t.inputLen ||
            s.latency != t.latency || s.inputDelay != t.inputDelay || s.upShift != t.upShift ||
            s.downShift != t.downShift || s.interpolatorSourceRate != t.interpolatorSourceRate ||
            s.interpolatorDestinationRate != t.interpolatorDestinationRate || s.consumesLatency != t.consumesLatency)
            return false;
    }
    return true;
}

// Independent ring-read evidence. A callback can consume at most 4096 stereo
// samples, strictly below one ring revolution; the modulo delta is unambiguous.
// Tracker still checks min(queued + actual SRC return, requested) and q_after.
inline bool consumedSamples(const Snapshot &before, const Snapshot &after,
                            std::uint64_t callbackFrames, std::uint64_t &samples) noexcept {
    samples = 0;
    if (!sameTopology(before, after) || callbackFrames == 0 ||
        callbackFrames > hook::portaudio::kMaxFrames) return false;
    const auto delta = (after.ring.readIndex + before.ring.capacity - before.ring.readIndex) & before.ring.mask;
    if (delta > callbackFrames * 2 || (delta & 1)) return false;
    samples = delta;
    return true;
}

} // namespace gpvst3::input::drainprobe
