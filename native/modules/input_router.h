#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "audio_adapter.h"

namespace gpvst3::input {

// The capture tap is intentionally a pointer view. The owner remains
// AudioLayer/PortAudio; the tap never keeps a pointer after process() returns.
struct CaptureView {
    const float *const *channels = nullptr;
    std::size_t channelCount = 0;
    std::size_t frameCount = 0;
    double sampleRate = 0.0;
    std::size_t blockSize = 0;
};

struct GeneratedView {
    const float *const *channels = nullptr;
    std::size_t channelCount = 0;
};

struct OutputView {
    float **channels = nullptr;
    std::size_t channelCount = 0;
};

// PortAudio delivers the stream callback buffers as an interleaved view. The
// pointer is borrowed and is valid only for the duration of processInterleaved.
// The adapter currently accepts the format observed in the locked host:
// interleaved float32. No pointer is retained after the call returns.
enum class InterleavedSampleFormat : std::uint8_t {
    Float32,
};

struct InterleavedView {
    const void *input = nullptr;
    void *output = nullptr;
    std::size_t frameCount = 0;
    // Input and output channel counts come from the current PortAudio stream
    // configuration. They may differ (a mono guitar input feeding stereo
    // monitoring), so the adapter maps channels explicitly.
    std::size_t inputChannelCount = 0;
    std::size_t outputChannelCount = 0;
    double sampleRate = 0.0;
    std::size_t blockSize = 0;
    const void *owner = nullptr;
    std::uint64_t sequence = 0;
    InterleavedSampleFormat format = InterleavedSampleFormat::Float32;
};

enum class Route : std::uint8_t {
    Disabled,
    InputInsert,
    BusMix,
};

const char *routeName(Route route) noexcept;

class Router final {
public:
    using ProcessFn = bool (*)(void *, const audio::BlockView &) noexcept;

    struct Processor {
        void *context = nullptr;
        ProcessFn process = nullptr;
    };

    struct Result {
        bool completed = false;
        bool bypassed = false;
        bool processed = false;
        bool error = false;
    };

    struct Snapshot {
        bool enabled = false;
        bool bypassed = false;
        bool streamRunning = false;
        Route route = Route::Disabled;
        bool captureObserved = false;
        bool levelObserved = false;
        std::size_t captureBlocks = 0;
        std::size_t inputProcessedBlocks = 0;
        std::size_t busMixedBlocks = 0;
        std::size_t bypassBlocks = 0;
        std::size_t errorBlocks = 0;
        std::size_t droppedBlocks = 0;
        std::size_t frameCount = 0;
        std::size_t channelCount = 0;
        double sampleRate = 0.0;
        float lastPeak = 0.0F;
        float maxPeak = 0.0F;
        float lastRms = 0.0F;
        bool interleavedFormatObserved = false;
        bool interleavedInputObserved = false;
        bool interleavedOutputWritten = false;
        std::size_t interleavedBlocks = 0;
        std::size_t interleavedFormatErrors = 0;
        std::size_t interleavedMissingBlocks = 0;
        std::size_t interleavedInputChannelCount = 0;
        std::size_t interleavedOutputChannelCount = 0;
        std::uintptr_t firstCaptureAddress = 0;
        std::uintptr_t lastCaptureAddress = 0;
        std::uintptr_t firstCaptureOwner = 0;
        std::uintptr_t lastCaptureOwner = 0;
        std::uintptr_t firstOutputAddress = 0;
        std::uintptr_t lastOutputAddress = 0;
    };

    Router() = default;
    Router(const Router &) = delete;
    Router &operator=(const Router &) = delete;

    // Called on the control/worker thread. The audio callback only uses the
    // resulting fixed-capacity scratch storage.
    bool prepare(std::size_t channelCount, std::size_t frameCapacity) noexcept;
    std::size_t channelCapacity() const noexcept { return channelCapacity_; }
    std::size_t frameCapacity() const noexcept { return frameCapacity_; }
    void setProcessor(Processor processor) noexcept;
    void setRoute(Route route) noexcept;
    void setEnabled(bool enabled) noexcept;
    void setBypassed(bool bypassed) noexcept;
    void setStreamRunning(bool running) noexcept;

    // Called from the verified AudioLayer/PortAudio capture tap. No allocation,
    // Qt calls, disk I/O, or blocking locks occur here.
    Result process(const CaptureView &capture, const GeneratedView &generated,
                   const OutputView &output) noexcept;
    // Convert one borrowed PortAudio interleaved float32 block to planar
    // storage, route it, and interleave the result back into the borrowed
    // output pointer before returning.
    Result processInterleaved(const InterleavedView &view) noexcept;
    void observeLevel(const CaptureView &capture) noexcept;

    Snapshot snapshot() const noexcept;

private:
    static float readAtomicFloat(const std::atomic<std::uint32_t> &value) noexcept;
    static void writeAtomicFloat(std::atomic<std::uint32_t> &target, float value) noexcept;
    static void updatePeak(std::atomic<std::uint32_t> &target, float value) noexcept;
    bool validOutput(const OutputView &output, std::size_t channels) const noexcept;
    bool passthrough(const CaptureView &capture, const GeneratedView &generated,
                     const OutputView &output) noexcept;
    bool copyCapture(const CaptureView &capture) noexcept;
    bool deinterleave(const void *source, std::size_t frames,
                      std::size_t channels, audio::PlanarBuffer &target) noexcept;
    bool interleave(const audio::PlanarBuffer &source, void *target,
                    std::size_t frames, std::size_t channels) noexcept;

    std::size_t channelCapacity_ = 0;
    std::size_t frameCapacity_ = 0;
    audio::PlanarBuffer mixScratch_;
    audio::PlanarBuffer interleavedCaptureScratch_;
    audio::PlanarBuffer interleavedGeneratedScratch_;
    audio::PlanarBuffer interleavedOutputScratch_;
    Processor processor_{};
    std::atomic<Route> route_{Route::Disabled};
    std::atomic<bool> enabled_{false};
    std::atomic<bool> bypassed_{false};
    std::atomic<bool> streamRunning_{false};
    std::atomic<bool> captureObserved_{false};
    std::atomic<bool> levelObserved_{false};
    std::atomic<std::size_t> captureBlocks_{0};
    std::atomic<std::size_t> inputProcessedBlocks_{0};
    std::atomic<std::size_t> busMixedBlocks_{0};
    std::atomic<std::size_t> bypassBlocks_{0};
    std::atomic<std::size_t> errorBlocks_{0};
    std::atomic<std::size_t> droppedBlocks_{0};
    std::atomic<std::size_t> frameCount_{0};
    std::atomic<std::size_t> channelCount_{0};
    std::atomic<std::uint32_t> sampleRateBits_{0};
    std::atomic<std::uint32_t> lastPeakBits_{0};
    std::atomic<std::uint32_t> maxPeakBits_{0};
    std::atomic<std::uint32_t> lastRmsBits_{0};
    std::atomic<bool> interleavedFormatObserved_{false};
    std::atomic<bool> interleavedInputObserved_{false};
    std::atomic<bool> interleavedOutputWritten_{false};
    std::atomic<std::size_t> interleavedBlocks_{0};
    std::atomic<std::size_t> interleavedFormatErrors_{0};
    std::atomic<std::size_t> interleavedMissingBlocks_{0};
    std::atomic<std::size_t> interleavedInputChannelCount_{0};
    std::atomic<std::size_t> interleavedOutputChannelCount_{0};
    std::atomic<std::uintptr_t> firstCaptureAddress_{0};
    std::atomic<std::uintptr_t> lastCaptureAddress_{0};
    std::atomic<std::uintptr_t> firstCaptureOwner_{0};
    std::atomic<std::uintptr_t> lastCaptureOwner_{0};
    std::atomic<std::uintptr_t> firstOutputAddress_{0};
    std::atomic<std::uintptr_t> lastOutputAddress_{0};
};

}
