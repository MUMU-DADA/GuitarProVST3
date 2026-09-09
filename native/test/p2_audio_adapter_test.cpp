#include "audio_adapter.h"

#include <cmath>
#include <cstdint>
#include <iostream>

#include "pluginterfaces/vst/ivstaudioprocessor.h"

namespace {

using namespace Steinberg;
using namespace Steinberg::Vst;

class GainProcessor final : public IAudioProcessor {
public:
    tresult PLUGIN_API queryInterface(const TUID, void **object) override {
        if (object) *object = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }
    tresult PLUGIN_API setBusArrangements(SpeakerArrangement *, int32, SpeakerArrangement *, int32) override {
        return kResultTrue;
    }
    tresult PLUGIN_API getBusArrangement(BusDirection, int32, SpeakerArrangement &arrangement) override {
        arrangement = SpeakerArr::kStereo;
        return kResultTrue;
    }
    tresult PLUGIN_API canProcessSampleSize(int32) override { return kResultTrue; }
    uint32 PLUGIN_API getLatencySamples() override { return 0; }
    tresult PLUGIN_API setupProcessing(ProcessSetup &) override { return kResultTrue; }
    tresult PLUGIN_API setProcessing(TBool) override { return kResultTrue; }
    tresult PLUGIN_API process(ProcessData &data) override {
        if (!data.inputs || !data.outputs || data.numInputs != 1 || data.numOutputs != 1)
            return kInvalidArgument;
        const auto &input = data.inputs[0];
        auto &output = data.outputs[0];
        for (int32 channel = 0; channel < output.numChannels; ++channel) {
            for (int32 frame = 0; frame < data.numSamples; ++frame)
                output.channelBuffers32[channel][frame] = input.channelBuffers32[channel][frame] * 2.0F;
        }
        return kResultOk;
    }
    uint32 PLUGIN_API getTailSamples() override { return kNoTail; }
};

bool check(bool condition, const char *message) {
    if (!condition) std::cerr << "FAIL: " << message << '\n';
    return condition;
}

bool close(float actual, float expected) { return std::fabs(actual - expected) < 0.0001F; }

} // namespace

int main() {
    using namespace gpvst3::audio;
    PlanarBuffer scratch;
    if (!check(scratch.prepare(2, 8), "prepare stereo scratch")) return 1;
    if (!check(!scratch.prepare(static_cast<std::size_t>(-1), 2), "reject capacity overflow")) return 1;

    GainProcessor processor;
    float inputLeft[4]{0.25F, -0.25F, 0.5F, -0.5F};
    float outputLeft[4]{};
    const float *inputs[1]{inputLeft};
    float *outputs[1]{outputLeft};
    const BlockView mono{inputs, nullptr, outputs, nullptr, 1, 4, 48000.0, 4,
                         reinterpret_cast<void *>(0x1234), 7, true};
    const auto processed = process(processor, mono, scratch);
    if (!check(processed.processed && processed.outputWritten && processed.ownerPointerObserved &&
                   processed.channels == 1 && close(outputLeft[0], 0.5F) && close(outputLeft[3], -1.0F),
               "mono process and owner evidence"))
        return 1;

    outputLeft[0] = 9.0F;
    const BlockView readOnly{inputs, nullptr, outputs, nullptr, 1, 4, 48000.0, 4,
                             reinterpret_cast<void *>(0x1234), 8, false};
    const auto refused = process(processor, readOnly, scratch);
    if (!check(!refused.processed && !refused.outputWritten &&
                   std::string(refused.error) == "output_not_writable" && outputLeft[0] == 9.0F,
               "reject read-only output"))
        return 1;

    std::cout << "PASS: P2 planar adapter bounds, mono processing and writeability contract.\n";
    return 0;
}
