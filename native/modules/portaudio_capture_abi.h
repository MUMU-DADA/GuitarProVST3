#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace gpvst3::hook::portaudio {

// AMAudio.dll 8.1.1.17 only. See docs/archive/phase-records/P4_CAPTURE_ABI.md for the call sites.
// Access these fields only inside that module's gated streamCallback: GP
// stops the old stream before changing its configuration or freeing Impl.
constexpr std::uintptr_t kInputParametersRva = 0x270880;
constexpr std::uintptr_t kOutputParametersRva = 0x2708A0;
constexpr std::size_t kMaxFrames = 2048; // The native callback clamps to 0x800.

struct StreamParameters {
    std::int32_t device;
    std::int32_t channelCount;
    std::uint32_t sampleFormat;
    std::uint32_t padding;
    double suggestedLatency;
    void *hostApiSpecificStreamInfo;
};
static_assert(sizeof(StreamParameters) == 32);

struct Configuration {
    std::size_t inputChannels = 0;
    std::size_t outputChannels = 0;
    double sampleRate = 0.0;
    int inputDevice = -1;
    int outputDevice = -1;
};

inline bool validParameters(const StreamParameters &input, const StreamParameters &output,
                            double sampleRate) noexcept {
    return input.channelCount >= 1 && input.channelCount <= 2 &&
        output.channelCount >= 1 && output.channelCount <= 2 &&
        input.sampleFormat == 1 && output.sampleFormat == 1 && // paFloat32, interleaved
        std::isfinite(sampleRate) && sampleRate >= 8000.0 && sampleRate <= 192000.0;
}

template <typename T> inline T read(const void *base, std::size_t offset) noexcept {
    T result{};
    std::memcpy(&result, static_cast<const std::uint8_t *>(base) + offset, sizeof(T));
    return result;
}

inline bool configuration(const void *module, const void *impl,
                          Configuration &result) noexcept {
    if (!module || !impl) return false;
    const auto *parent = read<const void *>(impl, 0);
    const auto *stream = read<const void *>(impl, 8);
    if (!parent || !stream || read<const void *>(parent, 0x178) != impl ||
        read<std::uint32_t>(stream, 0) != 0x18273645) return false;
    const auto sampleRate = read<double>(stream, 0x48);
    const auto input = read<StreamParameters>(module, kInputParametersRva);
    const auto output = read<StreamParameters>(module, kOutputParametersRva);
    if (!validParameters(input, output, sampleRate)) return false;
    result = {static_cast<std::size_t>(input.channelCount),
              static_cast<std::size_t>(output.channelCount), sampleRate,
              input.device, output.device};
    return true;
}

} // namespace gpvst3::hook::portaudio
