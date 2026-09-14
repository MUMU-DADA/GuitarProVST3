#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace gpvst3::audio {

struct SignalLevel {
    bool valid = false;
    float peak = 0.0F;
    float rms = 0.0F;
    float acRms = 0.0F;
};

// Remove each channel's own DC component. Opposite DC offsets in a stereo
// buffer must not be mistaken for an alternating (audible) signal.
template <typename Sample>
SignalLevel measureLevel(std::size_t channels, std::size_t frames, Sample sample) noexcept {
    SignalLevel result;
    if (!channels || !frames) return result;
    double energy = 0.0, acEnergy = 0.0;
    for (std::size_t channel = 0; channel < channels; ++channel) {
        double sum = 0.0, squares = 0.0;
        for (std::size_t frame = 0; frame < frames; ++frame) {
            const auto value = sample(channel, frame);
            if (!std::isfinite(value)) return {};
            result.peak = (std::max)(result.peak, std::fabs(value));
            sum += value;
            squares += static_cast<double>(value) * value;
        }
        energy += squares;
        acEnergy += (std::max)(0.0, squares - sum * sum / frames);
    }
    const auto count = static_cast<double>(channels) * frames;
    result.rms = static_cast<float>(std::sqrt(energy / count));
    result.acRms = static_cast<float>(std::sqrt(acEnergy / count));
    result.valid = true;
    return result;
}

inline SignalLevel measurePlanar(const float *const *channels, std::size_t count,
                                 std::size_t frames) noexcept {
    if (!channels) return {};
    for (std::size_t channel = 0; channel < count; ++channel)
        if (!channels[channel]) return {};
    return measureLevel(count, frames, [channels](std::size_t c, std::size_t f) {
        return channels[c][f];
    });
}

inline SignalLevel measureInterleaved(const float *samples, std::size_t channels,
                                      std::size_t frames) noexcept {
    if (!samples) return {};
    return measureLevel(channels, frames, [samples, channels](std::size_t c, std::size_t f) {
        return samples[f * channels + c];
    });
}

struct LevelSnapshot {
    std::uint64_t instance = 0;
    std::uint64_t sequence = 0;
    std::uint64_t nanoseconds = 0;
    SignalLevel input;
    SignalLevel output;
};

// Single producer (the existing processor guard / device callback), bounded
// reader. Only scalar atomics cross threads; no audio pointer, allocation or
// lock is retained. Sampling at up to 10 Hz also runs after silence or failure.
class LevelProbe final {
public:
    bool due(std::size_t frames, double sampleRate) noexcept {
        if (!frames || !std::isfinite(sampleRate) || sampleRate <= 0.0) return false;
        if (sampleRate != rate_) { remaining_ = 0; rate_ = sampleRate; }
        if (remaining_ > frames) { remaining_ -= frames; return false; }
        remaining_ = static_cast<std::size_t>(sampleRate / 10.0);
        return true;
    }

    void publish(const SignalLevel &input, const SignalLevel &output) noexcept {
        revision_.fetch_add(1);
        inputValid_.store(input.valid);
        inputPeak_.store(input.peak); inputRms_.store(input.rms); inputAcRms_.store(input.acRms);
        outputValid_.store(output.valid);
        outputPeak_.store(output.peak); outputRms_.store(output.rms); outputAcRms_.store(output.acRms);
        nanoseconds_.store(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count()));
        revision_.fetch_add(1);
    }

    void reset() noexcept {
        remaining_ = 0;
        rate_ = 0.0;
        publish({}, {});
    }

    LevelSnapshot snapshot() const noexcept {
        for (int attempt = 0; attempt < 3; ++attempt) {
            const auto before = revision_.load();
            if (before & 1) continue;
            LevelSnapshot value{instance_, before / 2, nanoseconds_.load(),
                {inputValid_.load(), inputPeak_.load(), inputRms_.load(), inputAcRms_.load()},
                {outputValid_.load(), outputPeak_.load(), outputRms_.load(), outputAcRms_.load()}};
            if (before == revision_.load()) return value;
        }
        return {}; // A torn measurement can never satisfy an acceptance gate.
    }

private:
    inline static std::atomic<std::uint64_t> nextInstance_{0};
    const std::uint64_t instance_ = nextInstance_.fetch_add(1) + 1;
    std::size_t remaining_ = 0;
    double rate_ = 0.0;
    std::atomic<std::uint64_t> revision_{0}, nanoseconds_{0};
    std::atomic<bool> inputValid_{false}, outputValid_{false};
    std::atomic<float> inputPeak_{0}, inputRms_{0}, inputAcRms_{0};
    std::atomic<float> outputPeak_{0}, outputRms_{0}, outputAcRms_{0};
};

} // namespace gpvst3::audio
