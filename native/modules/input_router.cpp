#include "input_router.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace gpvst3::input {
namespace {

const float *captureChannel(const CaptureView &capture, std::size_t channel) noexcept {
    return capture.channels && channel < capture.channelCount ? capture.channels[channel] : nullptr;
}

const float *generatedChannel(const GeneratedView &generated, std::size_t channel) noexcept {
    return generated.channels && channel < generated.channelCount ? generated.channels[channel] : nullptr;
}

float clampFinite(float value) noexcept {
    return std::isfinite(value) ? value : 0.0F;
}

} // namespace

const char *routeName(Route route) noexcept {
    switch (route) {
    case Route::InputInsert: return "input_insert";
    case Route::BusMix: return "bus_mix";
    case Route::Disabled: break;
    }
    return "disabled";
}

bool Router::prepare(std::size_t channelCount, std::size_t frameCapacity) noexcept {
    if (channelCount == 0 || frameCapacity == 0) return false;
    if (!mixScratch_.prepare(channelCount, frameCapacity)) return false;
    channelCapacity_ = channelCount;
    frameCapacity_ = frameCapacity;
    return true;
}

void Router::setProcessor(Processor processor) noexcept { processor_ = processor; }

void Router::setRoute(Route route) noexcept { route_.store(route, std::memory_order_release); }

void Router::setEnabled(bool enabled) noexcept { enabled_.store(enabled, std::memory_order_release); }

void Router::setStreamRunning(bool running) noexcept {
    streamRunning_.store(running, std::memory_order_release);
}

float Router::readAtomicFloat(const std::atomic<std::uint32_t> &value) noexcept {
    const auto bits = value.load(std::memory_order_relaxed);
    float result = 0.0F;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}

void Router::writeAtomicFloat(std::atomic<std::uint32_t> &target, float value) noexcept {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    target.store(bits, std::memory_order_relaxed);
}

void Router::updatePeak(std::atomic<std::uint32_t> &target, float value) noexcept {
    value = std::fabs(clampFinite(value));
    std::uint32_t desired = 0;
    std::memcpy(&desired, &value, sizeof(desired));
    auto current = target.load(std::memory_order_relaxed);
    while (current < desired &&
           !target.compare_exchange_weak(current, desired, std::memory_order_relaxed,
                                         std::memory_order_relaxed)) {
    }
}

void Router::observeLevel(const CaptureView &capture) noexcept {
    if (capture.frameCount == 0 || capture.channelCount == 0 ||
        capture.frameCount > frameCapacity_ || capture.channelCount > channelCapacity_)
        return;
    bool observed = false;
    float peak = 0.0F;
    double sumSquares = 0.0;
    std::size_t sampleCount = 0;
    for (std::size_t channel = 0; channel < capture.channelCount; ++channel) {
        const auto *samples = captureChannel(capture, channel);
        if (!samples) continue;
        observed = true;
        for (std::size_t frame = 0; frame < capture.frameCount; ++frame) {
            const auto value = clampFinite(samples[frame]);
            peak = (std::max)(peak, std::fabs(value));
            sumSquares += static_cast<double>(value) * static_cast<double>(value);
            ++sampleCount;
        }
    }
    if (!observed || sampleCount == 0) return;
    captureObserved_.store(true, std::memory_order_release);
    levelObserved_.store(true, std::memory_order_release);
    frameCount_.store(capture.frameCount, std::memory_order_relaxed);
    channelCount_.store(capture.channelCount, std::memory_order_relaxed);
    writeAtomicFloat(lastPeakBits_, peak);
    updatePeak(maxPeakBits_, peak);
    writeAtomicFloat(lastRmsBits_, static_cast<float>(std::sqrt(sumSquares / sampleCount)));
    writeAtomicFloat(sampleRateBits_, static_cast<float>(capture.sampleRate));
}

bool Router::validOutput(const OutputView &output, std::size_t channels) const noexcept {
    if (!output.channels || output.channelCount < channels) return false;
    for (std::size_t channel = 0; channel < channels; ++channel)
        if (!output.channels[channel]) return false;
    return true;
}

bool Router::passthrough(const CaptureView &capture, const GeneratedView &generated,
                         const OutputView &output) noexcept {
    // When a caller supplies both streams, disabled routing preserves the GP
    // generated stream. A capture-only callback still passes the capture data.
    const bool useGenerated = generated.channelCount != 0;
    const auto source = useGenerated ? generated.channelCount : capture.channelCount;
    if (source == 0 || capture.frameCount == 0 || capture.frameCount > frameCapacity_ ||
        source > channelCapacity_ || !validOutput(output, source))
        return false;
    for (std::size_t channel = 0; channel < source; ++channel) {
        const auto *samples = useGenerated ? generatedChannel(generated, channel)
                                           : captureChannel(capture, channel);
        if (samples)
            std::memcpy(output.channels[channel], samples,
                        capture.frameCount * sizeof(float));
        else
            std::fill(output.channels[channel], output.channels[channel] + capture.frameCount, 0.0F);
    }
    return true;
}

bool Router::copyCapture(const CaptureView &capture) noexcept {
    if (capture.frameCount == 0 || capture.frameCount > frameCapacity_ ||
        capture.channelCount > channelCapacity_)
        return false;
    for (std::size_t channel = 0; channel < channelCapacity_; ++channel) {
        auto *target = mixScratch_.inputChannels()[channel];
        const auto *source = captureChannel(capture, channel);
        if (source)
            std::memcpy(target, source, capture.frameCount * sizeof(float));
        else
            std::fill(target, target + capture.frameCount, 0.0F);
    }
    return true;
}

Router::Result Router::process(const CaptureView &capture, const GeneratedView &generated,
                               const OutputView &output) noexcept {
    Result result;
    observeLevel(capture);
    if (capture.frameCount == 0 || capture.frameCount > frameCapacity_ ||
        capture.channelCount > channelCapacity_ || capture.channelCount == 0) {
        droppedBlocks_.fetch_add(1, std::memory_order_relaxed);
        return result;
    }
    captureBlocks_.fetch_add(1, std::memory_order_relaxed);
    const auto route = route_.load(std::memory_order_acquire);
    const bool active = enabled_.load(std::memory_order_acquire) &&
                        streamRunning_.load(std::memory_order_acquire) &&
                        route != Route::Disabled;
    if (!active) {
        result.bypassed = true;
        result.completed = passthrough(capture, generated, output);
        if (!result.completed) droppedBlocks_.fetch_add(1, std::memory_order_relaxed);
        else bypassBlocks_.fetch_add(1, std::memory_order_relaxed);
        return result;
    }
    if (route == Route::InputInsert) {
        if (!validOutput(output, capture.channelCount)) {
            droppedBlocks_.fetch_add(1, std::memory_order_relaxed);
            return result;
        }
        const audio::BlockView block{capture.channels, nullptr, output.channels, nullptr,
                                     capture.channelCount, capture.frameCount,
                                     capture.sampleRate, capture.blockSize};
        const bool processed = processor_.process && processor_.process(processor_.context, block);
        if (processed) {
            inputProcessedBlocks_.fetch_add(1, std::memory_order_relaxed);
            result.completed = true;
            result.processed = true;
            return result;
        }
        errorBlocks_.fetch_add(1, std::memory_order_relaxed);
        result.error = true;
        result.completed = audio::bypass(block);
        result.bypassed = result.completed;
        if (result.completed) bypassBlocks_.fetch_add(1, std::memory_order_relaxed);
        return result;
    }

    if (!validOutput(output, channelCapacity_) || !copyCapture(capture)) {
        droppedBlocks_.fetch_add(1, std::memory_order_relaxed);
        return result;
    }
    for (std::size_t channel = 0; channel < channelCapacity_; ++channel) {
        auto *mixed = mixScratch_.inputChannels()[channel];
        const auto *generatedSamples = generatedChannel(generated, channel);
        for (std::size_t frame = 0; frame < capture.frameCount; ++frame)
            if (generatedSamples) mixed[frame] += generatedSamples[frame];
    }
    const audio::BlockView block{mixScratch_.inputChannels(), nullptr, output.channels, nullptr,
                                 channelCapacity_, capture.frameCount, capture.sampleRate,
                                 capture.blockSize};
    const bool processed = processor_.process && processor_.process(processor_.context, block);
    if (processed) {
        busMixedBlocks_.fetch_add(1, std::memory_order_relaxed);
        result.completed = true;
        result.processed = true;
        return result;
    }
    errorBlocks_.fetch_add(1, std::memory_order_relaxed);
    result.error = true;
    result.completed = audio::bypass(block);
    result.bypassed = result.completed;
    if (result.completed) bypassBlocks_.fetch_add(1, std::memory_order_relaxed);
    return result;
}

Router::Snapshot Router::snapshot() const noexcept {
    Snapshot result;
    result.enabled = enabled_.load(std::memory_order_acquire);
    result.streamRunning = streamRunning_.load(std::memory_order_acquire);
    result.route = route_.load(std::memory_order_acquire);
    result.captureObserved = captureObserved_.load(std::memory_order_acquire);
    result.levelObserved = levelObserved_.load(std::memory_order_acquire);
    result.captureBlocks = captureBlocks_.load(std::memory_order_relaxed);
    result.inputProcessedBlocks = inputProcessedBlocks_.load(std::memory_order_relaxed);
    result.busMixedBlocks = busMixedBlocks_.load(std::memory_order_relaxed);
    result.bypassBlocks = bypassBlocks_.load(std::memory_order_relaxed);
    result.errorBlocks = errorBlocks_.load(std::memory_order_relaxed);
    result.droppedBlocks = droppedBlocks_.load(std::memory_order_relaxed);
    result.frameCount = frameCount_.load(std::memory_order_relaxed);
    result.channelCount = channelCount_.load(std::memory_order_relaxed);
    result.sampleRate = readAtomicFloat(sampleRateBits_);
    result.lastPeak = readAtomicFloat(lastPeakBits_);
    result.maxPeak = readAtomicFloat(maxPeakBits_);
    result.lastRms = readAtomicFloat(lastRmsBits_);
    return result;
}

}
