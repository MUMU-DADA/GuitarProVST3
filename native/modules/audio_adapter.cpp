#include "audio_adapter.h"

#include <algorithm>
#include <cstring>

#include "pluginterfaces/vst/ivstaudioprocessor.h"

namespace gpvst3::audio {
namespace {

const float *sourceChannel(const BlockView &block, std::size_t channel) noexcept {
    const auto channels = block.generatedChannels ? block.generatedChannels : block.inputChannels;
    if (channels) return channels[channel];
    return block.channels ? block.channels[channel] : nullptr;
}

float *targetChannel(const BlockView &block, std::size_t channel) noexcept {
    const auto channels = block.outputChannels ? block.outputChannels : block.channels;
    return channels ? channels[channel] : nullptr;
}

} // namespace

bool PlanarBuffer::prepare(std::size_t channelCount, std::size_t frameCapacity) noexcept {
    try {
        channelCount_ = channelCount;
        frameCapacity_ = frameCapacity;
        inputStorage_.assign(channelCount * frameCapacity, 0.0F);
        outputStorage_.assign(channelCount * frameCapacity, 0.0F);
        inputPointers_.resize(channelCount);
        outputPointers_.resize(channelCount);
        for (std::size_t channel = 0; channel < channelCount; ++channel) {
            inputPointers_[channel] = inputStorage_.data() + channel * frameCapacity;
            outputPointers_[channel] = outputStorage_.data() + channel * frameCapacity;
        }
        return true;
    } catch (...) {
        channelCount_ = 0;
        frameCapacity_ = 0;
        inputStorage_.clear();
        outputStorage_.clear();
        inputPointers_.clear();
        outputPointers_.clear();
        return false;
    }
}

void PlanarBuffer::clear() noexcept {
    std::fill(inputStorage_.begin(), inputStorage_.end(), 0.0F);
    std::fill(outputStorage_.begin(), outputStorage_.end(), 0.0F);
}

ConversionResult copyToPlanar(const BlockView &source, PlanarBuffer &target) noexcept {
    ConversionResult result;
    if (source.frameCount > target.frameCapacity() || source.channelCount > target.channelCount())
        return result;
    if (source.frameCount == 0 || source.channelCount == 0) {
        result.valid = true;
        return result;
    }
    for (std::size_t channel = 0; channel < target.channelCount(); ++channel) {
        auto *destination = target.inputChannels()[channel];
        const auto *input = channel < source.channelCount ? sourceChannel(source, channel) : nullptr;
        if (input)
            std::memcpy(destination, input, source.frameCount * sizeof(float));
        else
            std::fill(destination, destination + source.frameCount, 0.0F);
    }
    result.valid = true;
    result.channelsCopied = std::min(source.channelCount, target.channelCount());
    result.framesCopied = source.frameCount;
    return result;
}

ConversionResult copyFromPlanar(const PlanarBuffer &source, const BlockView &target) noexcept {
    ConversionResult result;
    if (target.frameCount > source.frameCapacity() || target.channelCount > source.channelCount())
        return result;
    if (target.frameCount == 0 || target.channelCount == 0) {
        result.valid = true;
        return result;
    }
    bool outputsAvailable = true;
    for (std::size_t channel = 0; channel < target.channelCount; ++channel) {
        auto *destination = targetChannel(target, channel);
        if (destination)
            std::memcpy(destination, source.outputChannels()[channel], target.frameCount * sizeof(float));
        else
            outputsAvailable = false;
    }
    result.valid = outputsAvailable;
    result.channelsCopied = target.channelCount;
    result.framesCopied = target.frameCount;
    return result;
}

ProcessResult process(Steinberg::Vst::IAudioProcessor &processor,
                      const BlockView &block, PlanarBuffer &scratch, bool bypassed) noexcept {
    ProcessResult result;
    result.frames = block.frameCount;
    if (bypassed) {
        result.bypassed = true;
        const auto copied = copyToPlanar(block, scratch);
        if (!copied.valid) {
            result.error = "buffer_capacity";
            return result;
        }
        for (std::size_t channel = 0; channel < scratch.channelCount(); ++channel)
            std::memcpy(scratch.outputChannels()[channel], scratch.inputChannels()[channel],
                        block.frameCount * sizeof(float));
        const auto written = copyFromPlanar(scratch, block);
        result.processed = written.valid;
        result.error = written.valid ? "none" : "output_buffer";
        return result;
    }

    const auto copied = copyToPlanar(block, scratch);
    if (!copied.valid) {
        result.error = "buffer_capacity";
        return result;
    }
    for (std::size_t channel = 0; channel < scratch.channelCount(); ++channel)
        std::fill(scratch.outputChannels()[channel],
                  scratch.outputChannels()[channel] + block.frameCount, 0.0F);
    Steinberg::Vst::AudioBusBuffers inputBus;
    inputBus.numChannels = static_cast<Steinberg::int32>(block.channelCount);
    inputBus.channelBuffers32 = scratch.inputChannels();
    Steinberg::Vst::AudioBusBuffers outputBus;
    outputBus.numChannels = static_cast<Steinberg::int32>(block.channelCount);
    outputBus.channelBuffers32 = scratch.outputChannels();
    Steinberg::Vst::ProcessData data;
    data.processMode = Steinberg::Vst::kRealtime;
    data.symbolicSampleSize = Steinberg::Vst::kSample32;
    data.numSamples = static_cast<Steinberg::int32>(block.frameCount);
    data.numInputs = block.channelCount == 0 ? 0 : 1;
    data.numOutputs = block.channelCount == 0 ? 0 : 1;
    data.inputs = data.numInputs ? &inputBus : nullptr;
    data.outputs = data.numOutputs ? &outputBus : nullptr;
    const auto processResult = processor.process(data);
    if (processResult != Steinberg::kResultOk && processResult != Steinberg::kResultTrue) {
        result.error = "processor_process_failed";
        return result;
    }
    const auto written = copyFromPlanar(scratch, block);
    result.processed = written.valid;
    result.error = written.valid ? "none" : "output_buffer";
    return result;
}

} // namespace gpvst3::audio
