#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Steinberg::Vst {
class IAudioProcessor;
}

namespace gpvst3::audio {

// A non-owning view of one GP processing block. The private GP IAudioBuffer
// type is intentionally not declared here: the adapter only receives the
// channel pointers obtained by the verified observation layer.
struct BlockView {
    const float *const *inputChannels = nullptr;
    const float *const *generatedChannels = nullptr;
    float **outputChannels = nullptr;
    // Kept for the P0 shape and for callers that already use one channel array.
    float **channels = nullptr;
    std::size_t channelCount = 0;
    std::size_t frameCount = 0;
    double sampleRate = 0.0;
    std::size_t blockSize = 0;
    // Optional evidence supplied by a host adapter. The pointers remain
    // borrowed for the duration of the call; no buffer ownership is retained.
    const void *owner = nullptr;
    std::uint64_t sequence = 0;
    bool outputWritable = true;
};

// Storage is prepared on a control/worker thread and then reused by process()
// on the audio thread. process() never changes the vector sizes.
class PlanarBuffer final {
public:
    PlanarBuffer() = default;

    bool prepare(std::size_t channelCount, std::size_t frameCapacity) noexcept;
    void clear() noexcept;

    std::size_t channelCount() const noexcept { return channelCount_; }
    std::size_t frameCapacity() const noexcept { return frameCapacity_; }
    float **inputChannels() noexcept { return inputPointers_.data(); }
    float **outputChannels() noexcept { return outputPointers_.data(); }
    const float *const *inputChannels() const noexcept { return inputPointers_.data(); }
    const float *const *outputChannels() const noexcept { return outputPointers_.data(); }

private:
    std::size_t channelCount_ = 0;
    std::size_t frameCapacity_ = 0;
    std::vector<float> inputStorage_;
    std::vector<float> outputStorage_;
    std::vector<float *> inputPointers_;
    std::vector<float *> outputPointers_;
};

struct ConversionResult {
    bool valid = false;
    std::size_t channelsCopied = 0;
    std::size_t framesCopied = 0;
};

ConversionResult copyToPlanar(const BlockView &source, PlanarBuffer &target) noexcept;
ConversionResult copyFromPlanar(const PlanarBuffer &source, const BlockView &target) noexcept;

// Copy one block directly through when the chain is bypassed or has entered
// its error fallback. This path uses only caller-owned channel pointers.
bool bypass(const BlockView &block) noexcept;

struct ProcessResult {
    bool processed = false;
    bool bypassed = false;
    bool outputWritten = false;
    bool ownerPointerObserved = false;
    std::size_t frames = 0;
    std::size_t channels = 0;
    const char *error = "none";
};

// Adapt one already prepared VST3 processor to the GP block. The caller owns
// the processor and controls its lifecycle; this function only performs the
// block copy and IAudioProcessor::process call.
ProcessResult process(Steinberg::Vst::IAudioProcessor &processor,
                      const BlockView &block, PlanarBuffer &scratch,
                      bool bypassed = false) noexcept;

}
