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
    };

    Router() = default;
    Router(const Router &) = delete;
    Router &operator=(const Router &) = delete;

    // Called on the control/worker thread. The audio callback only uses the
    // resulting fixed-capacity scratch storage.
    bool prepare(std::size_t channelCount, std::size_t frameCapacity) noexcept;
    void setProcessor(Processor processor) noexcept;
    void setRoute(Route route) noexcept;
    void setEnabled(bool enabled) noexcept;
    void setStreamRunning(bool running) noexcept;

    // Called from the verified AudioLayer/PortAudio capture tap. No allocation,
    // Qt calls, disk I/O, or blocking locks occur here.
    Result process(const CaptureView &capture, const GeneratedView &generated,
                   const OutputView &output) noexcept;
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

    std::size_t channelCapacity_ = 0;
    std::size_t frameCapacity_ = 0;
    audio::PlanarBuffer mixScratch_;
    Processor processor_{};
    std::atomic<Route> route_{Route::Disabled};
    std::atomic<bool> enabled_{false};
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
};

}
