#include "effect_chain.h"

#include <chrono>
#include <algorithm>
#include <cstring>
#include <thread>

#include "audio_adapter.h"

namespace gpvst3::effects {
namespace {

using Clock = std::chrono::steady_clock;

void updateMax(std::atomic<std::uint64_t> &target, std::uint64_t value) noexcept {
    auto current = target.load(std::memory_order_relaxed);
    while (current < value &&
           !target.compare_exchange_weak(current, value, std::memory_order_relaxed,
                                          std::memory_order_relaxed)) {
    }
}

} // namespace

bool Chain::acquire(Slot &slot, int index) noexcept {
    if (!slot.accepting.load(std::memory_order_acquire) ||
        !slot.configured.load(std::memory_order_acquire))
        return false;
    slot.readers.fetch_add(1, std::memory_order_acquire);
    if (activeSlot_.load(std::memory_order_acquire) == index &&
        slot.accepting.load(std::memory_order_acquire) &&
        slot.configured.load(std::memory_order_acquire))
        return true;
    slot.readers.fetch_sub(1, std::memory_order_release);
    return false;
}

void Chain::waitForReaders(Slot &slot) noexcept {
    const auto started = Clock::now();
    while (slot.readers.load(std::memory_order_acquire) != 0)
        std::this_thread::yield();
    const auto elapsed = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started).count());
    lastReaderDrainNanoseconds_.store(elapsed, std::memory_order_relaxed);
    updateMax(maxReaderDrainNanoseconds_, elapsed);
}

bool Chain::prepareSlot(std::size_t index, SlotConfig config) noexcept {
    if (index >= kSlotCount || !config.process) return false;
    switchRequests_.fetch_add(1, std::memory_order_relaxed);
    const auto current = activeSlot_.load(std::memory_order_acquire);
    if (current == static_cast<int>(index)) return false;
    auto &slot = slots_[index];
    slot.accepting.store(false, std::memory_order_release);
    waitForReaders(slot);
    slot.config = config;
    slot.configured.store(true, std::memory_order_release);
    switchPrepared_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool Chain::activate(std::size_t index) noexcept {
    if (index >= kSlotCount || !slots_[index].configured.load(std::memory_order_acquire))
        return false;
    const auto old = activeSlot_.load(std::memory_order_acquire);
    if (old == static_cast<int>(index)) {
        slots_[index].accepting.store(true, std::memory_order_release);
        bypassed_.store(requestedBypass_.load(std::memory_order_acquire) || faulted(),
                        std::memory_order_release);
        return true;
    }
    if (old >= 0) slots_[old].accepting.store(false, std::memory_order_release);
    if (old >= 0) retiredSlots_.fetch_add(1, std::memory_order_relaxed);
    slots_[index].accepting.store(true, std::memory_order_release);
    activeSlot_.store(static_cast<int>(index), std::memory_order_release);
    switchCount_.fetch_add(1, std::memory_order_relaxed);
    const auto started = Clock::now();
    rampRemaining_.store(rampSamples_.load(std::memory_order_acquire), std::memory_order_release);
    bypassed_.store(requestedBypass_.load(std::memory_order_acquire) || faulted(),
                    std::memory_order_release);
    const auto elapsed = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started).count());
    lastSwitchNanoseconds_.store(elapsed, std::memory_order_relaxed);
    updateMax(maxSwitchNanoseconds_, elapsed);
    return true;
}

void Chain::deactivate() noexcept {
    const auto old = activeSlot_.exchange(-1, std::memory_order_acq_rel);
    if (old >= 0) {
        slots_[old].accepting.store(false, std::memory_order_release);
        waitForReaders(slots_[old]);
    }
    for (auto &slot : slots_) {
        slot.accepting.store(false, std::memory_order_release);
        waitForReaders(slot);
    }
}

void Chain::setBypassed(bool value) noexcept {
    requestedBypass_.store(value, std::memory_order_release);
    bypassed_.store(value || faulted(),
                    std::memory_order_release);
}

void Chain::setRampSamples(std::size_t samples) noexcept {
    rampSamples_.store((std::min)(samples, std::size_t{4096}), std::memory_order_release);
}

void Chain::clearFault() noexcept {
    faulted_.store(false, std::memory_order_release);
    bypassed_.store(requestedBypass_.load(std::memory_order_acquire), std::memory_order_release);
}

Chain::ProcessResult Chain::process(const audio::BlockView &block) noexcept {
    ProcessResult result;
    processBlocks_.fetch_add(1, std::memory_order_relaxed);
    const auto previousSequence = lastSequence_.exchange(block.sequence, std::memory_order_relaxed);
    if (previousSequence != 0 && block.sequence != 0 && block.sequence != previousSequence + 1)
        sequenceGaps_.fetch_add(1, std::memory_order_relaxed);
    if (bypassed_.load(std::memory_order_acquire)) {
        bypassBlocks_.fetch_add(1, std::memory_order_relaxed);
        result.completed = true;
        result.bypassed = true;
        return result;
    }

    // A bounded retry keeps the audio side wait-free if a worker changes the
    // active slot at the same moment as this block.
    for (int attempt = 0; attempt != 2; ++attempt) {
        const auto index = activeSlot_.load(std::memory_order_acquire);
        if (index < 0 || index >= static_cast<int>(kSlotCount)) break;
        auto &slot = slots_[index];
        if (!acquire(slot, index)) continue;
        const auto started = Clock::now();
        const bool ok = slot.config.process(slot.config.context, block);
        const auto elapsed = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started).count());
        slot.readers.fetch_sub(1, std::memory_order_release);
        result.elapsedNanoseconds = elapsed;
        lastProcessNanoseconds_.store(elapsed, std::memory_order_relaxed);
        totalProcessNanoseconds_.fetch_add(elapsed, std::memory_order_relaxed);
        updateMax(maxProcessNanoseconds_, elapsed);
        if (ok) {
            applyRamp(block);
            processedBlocks_.fetch_add(1, std::memory_order_relaxed);
            result.completed = true;
            return result;
        }
        errorBlocks_.fetch_add(1, std::memory_order_relaxed);
        fallbackBlocks_.fetch_add(1, std::memory_order_relaxed);
        faulted_.store(true, std::memory_order_release);
        bypassed_.store(true, std::memory_order_release);
        result.error = true;
        return result;
    }

    bypassBlocks_.fetch_add(1, std::memory_order_relaxed);
    result.completed = true;
    result.bypassed = true;
    return result;
}

void Chain::applyRamp(const audio::BlockView &block) noexcept {
    auto remaining = rampRemaining_.load(std::memory_order_acquire);
    const auto total = rampSamples_.load(std::memory_order_acquire);
    if (remaining == 0 || total == 0 || block.frameCount == 0) return;
    const auto *const *inputs = block.generatedChannels ? block.generatedChannels : block.inputChannels;
    float *const *outputs = block.outputChannels ? block.outputChannels : block.channels;
    if (!inputs || !outputs) return;
    const auto frames = block.frameCount;
    for (std::size_t frame = 0; frame < frames && remaining > 0; ++frame) {
        const auto progressed = total - remaining;
        const float gain = static_cast<float>(progressed + 1) / static_cast<float>(total);
        const float inverse = 1.0F - gain;
        for (std::size_t channel = 0; channel < block.channelCount; ++channel) {
            if (!inputs[channel] || !outputs[channel]) continue;
            outputs[channel][frame] = inputs[channel][frame] * inverse + outputs[channel][frame] * gain;
        }
        --remaining;
    }
    rampRemaining_.store(remaining, std::memory_order_release);
}

Chain::Snapshot Chain::snapshot() const noexcept {
    Snapshot result;
    result.bypassed = bypassed();
    result.faulted = faulted();
    result.activeSlot = activeSlot_.load(std::memory_order_acquire);
    for (const auto &slot : slots_)
        if (slot.configured.load(std::memory_order_acquire)) ++result.preparedSlots;
    result.processBlocks = processBlocks_.load(std::memory_order_relaxed);
    result.processedBlocks = processedBlocks_.load(std::memory_order_relaxed);
    result.bypassBlocks = bypassBlocks_.load(std::memory_order_relaxed);
    result.errorBlocks = errorBlocks_.load(std::memory_order_relaxed);
    result.fallbackBlocks = fallbackBlocks_.load(std::memory_order_relaxed);
    result.lastProcessNanoseconds = lastProcessNanoseconds_.load(std::memory_order_relaxed);
    result.maxProcessNanoseconds = maxProcessNanoseconds_.load(std::memory_order_relaxed);
    result.totalProcessNanoseconds = totalProcessNanoseconds_.load(std::memory_order_relaxed);
    result.switchCount = switchCount_.load(std::memory_order_relaxed);
    result.switchRequests = switchRequests_.load(std::memory_order_relaxed);
    result.switchPrepared = switchPrepared_.load(std::memory_order_relaxed);
    result.retiredSlots = retiredSlots_.load(std::memory_order_relaxed);
    result.lastSwitchNanoseconds = lastSwitchNanoseconds_.load(std::memory_order_relaxed);
    result.maxSwitchNanoseconds = maxSwitchNanoseconds_.load(std::memory_order_relaxed);
    result.lastReaderDrainNanoseconds = lastReaderDrainNanoseconds_.load(std::memory_order_relaxed);
    result.maxReaderDrainNanoseconds = maxReaderDrainNanoseconds_.load(std::memory_order_relaxed);
    result.readerDrainTimeouts = readerDrainTimeouts_.load(std::memory_order_relaxed);
    result.sequenceGaps = sequenceGaps_.load(std::memory_order_relaxed);
    result.lastSequence = lastSequence_.load(std::memory_order_relaxed);
    result.rampSamples = rampSamples_.load(std::memory_order_relaxed);
    result.rampRemaining = rampRemaining_.load(std::memory_order_relaxed);
    return result;
}

}
