#include "effect_chain.h"
#include "audio_adapter.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
using gpvst3::audio::BlockView;
using gpvst3::effects::Chain;

struct Processor {
    float gain = 1.0F;
    bool fail = false;
};

bool process(void *context, const BlockView &block) noexcept {
    auto *processor = static_cast<Processor *>(context);
    if (!processor || processor->fail || !block.inputChannels || !block.outputChannels) return false;
    for (std::size_t channel = 0; channel < block.channelCount; ++channel)
        for (std::size_t frame = 0; frame < block.frameCount; ++frame)
            block.outputChannels[channel][frame] = block.inputChannels[channel][frame] * processor->gain;
    return true;
}

void require(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}
}

int main(int argc, char **argv) {
    try {
        const std::string artifact = argc > 1 ? argv[1] : "p10-activation.json";
        Processor processors[2]{{1.25F, false}, {0.75F, false}};
        Chain chain;
        chain.setRampSamples(0);
        require(chain.prepareSlot(0, {&processors[0], &process}), "prepare cold slot");
        require(chain.activate(0), "activate cold slot");
        chain.setBypassed(false);

        float input[2][64]{}, output[2][64]{};
        std::fill(&input[0][0], &input[0][0] + 128, 0.4F);
        const float *inputs[]{input[0], input[1]};
        float *outputs[]{output[0], output[1]};
        BlockView block{inputs, nullptr, outputs, nullptr, 2, 64, 44100.0, 64,
                        reinterpret_cast<void *>(1), 1, true};
        require(chain.process(block).completed && output[0][0] == 0.5F,
                "first cold block is processed");
        auto cold = chain.snapshot();
        require(cold.firstProcessedSequence == 1 && cold.callbacksToFirstProcess == 1,
                "cold activation records first callback evidence");

        std::fill(&output[0][0], &output[0][0] + 128, 0.0F);
        chain.setBypassed(true);
        block.sequence = 2;
        const auto bypass = chain.process(block);
        require(bypass.completed && bypass.bypassed && output[0][0] == 0.0F,
                "bypass is visible on the next callback");

        // Reuse the second preallocated slot and switch back repeatedly. The
        // chain only flips atomics at the handoff; no callback allocates or
        // waits for plugin construction.
        require(chain.prepareSlot(1, {&processors[1], &process}), "prepare warm slot");
        require(chain.activate(1), "activate warm slot");
        chain.setBypassed(false);
        block.sequence = 3;
        std::fill(&output[0][0], &output[0][0] + 128, 0.0F);
        require(chain.process(block).completed && output[0][0] == 0.3F,
                "warm activation first block is processed");
        const auto warm = chain.snapshot();
        require(warm.firstProcessedSequence == 3 && warm.callbacksToFirstProcess == 1,
                "warm activation records a bounded first callback");

        for (std::uint64_t sequence = 4; sequence < 24; ++sequence) {
            const auto target = chain.snapshot().activeSlot == 0 ? 1U : 0U;
            require(chain.prepareSlot(target, {&processors[target], &process}), "rapid prepare");
            require(chain.activate(target), "rapid activate");
            chain.setBypassed(false);
            block.sequence = sequence;
            std::fill(&output[0][0], &output[0][0] + 128, 0.0F);
            require(chain.process(block).completed, "rapid block completes");
        }
        const auto result = chain.snapshot();
        require(result.sequenceGaps == 0, "activation sequence remains contiguous");
        require(result.switchCount >= 22, "rapid handoffs are observed");

        std::ofstream file(artifact, std::ios::binary);
        require(file.good(), "open activation artifact");
        file << "{\"bypass_next_callback\":true"
             << ",\"cold_first_sequence\":" << cold.firstProcessedSequence
             << ",\"cold_callbacks_to_first\":" << cold.callbacksToFirstProcess
             << ",\"warm_first_sequence\":" << warm.firstProcessedSequence
             << ",\"warm_callbacks_to_first\":" << warm.callbacksToFirstProcess
             << ",\"switch_count\":" << result.switchCount
             << ",\"sequence_gaps\":" << result.sequenceGaps << "}\n";
        std::cout << "PASS: P10 activation bypass, warm handoff and first callback evidence.\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
