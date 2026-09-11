#include "effect_chain.h"
#include "audio_adapter.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
using gpvst3::audio::BlockView;
using gpvst3::effects::Chain;

struct Processor {
    float gain = 1.25F;
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
        const std::string artifact = argc > 1 ? argv[1] : "p9-switch.json";
        Processor processors[2];
        Chain chain;
        chain.setRampSamples(64);
        require(chain.prepareSlot(0, {&processors[0], &process}), "prepare first slot");
        require(chain.activate(0), "activate first slot");
        chain.setBypassed(false);

        float input[2][128]{}, output[2][128]{};
        const float *inputs[]{input[0], input[1]};
        float *outputs[]{output[0], output[1]};
        BlockView block{inputs, nullptr, outputs, nullptr, 2, 128, 44100.0, 128,
                        reinterpret_cast<void *>(1), 1, true};
        auto reset = [&] {
            for (auto &channel : input) std::fill(std::begin(channel), std::end(channel), 0.4F);
            for (auto &channel : output) std::fill(std::begin(channel), std::end(channel), 0.0F);
        };
        reset();
        require(chain.process(block).completed, "first block completes");
        require(output[0][127] > 0.4F, "ramp reaches processed output");

        for (std::uint64_t sequence = 2; sequence <= 201; ++sequence) {
            const auto target = chain.snapshot().activeSlot == 0 ? 1U : 0U;
            processors[target].gain = target == 0 ? 1.25F : 0.75F;
            require(chain.prepareSlot(target, {&processors[target], &process}), "prepare rapid switch");
            require(chain.activate(target), "activate rapid switch");
            chain.setBypassed(false);
            block.sequence = sequence;
            reset();
            require(chain.process(block).completed, "rapid switch block completes");
        }
        auto snapshot = chain.snapshot();
        require(snapshot.switchCount >= 201, "all handoffs observed");
        require(snapshot.switchRequests >= 201 && snapshot.switchPrepared >= 201,
                "switch request/preparation counters observed");
        require(snapshot.sequenceGaps == 0, "contiguous audio sequence");
        require(snapshot.rampSamples == 64 && snapshot.rampRemaining == 0, "ramp drains per block");
        require(snapshot.maxSwitchNanoseconds < 100000000ULL, "handoff remains bounded");

        const auto failingSlot = chain.snapshot().activeSlot == 0 ? 1U : 0U;
        processors[failingSlot].fail = true;
        require(chain.prepareSlot(failingSlot, {&processors[failingSlot], &process}), "prepare failing slot");
        require(chain.activate(failingSlot), "activate failing slot");
        chain.setBypassed(false);
        block.sequence = 202;
        reset();
        const auto failed = chain.process(block);
        require(failed.error && chain.faulted(), "processor failure enters bypass fallback");
        require(chain.snapshot().fallbackBlocks > 0, "fallback block recorded");

        std::ofstream outputFile(artifact, std::ios::binary);
        require(outputFile.good(), "open switch artifact");
        snapshot = chain.snapshot();
        outputFile << "{\"switch_count\":" << snapshot.switchCount
                   << ",\"switch_requests\":" << snapshot.switchRequests
                   << ",\"switch_prepared\":" << snapshot.switchPrepared
                   << ",\"sequence_gaps\":" << snapshot.sequenceGaps
                   << ",\"reader_drain_ns\":" << snapshot.lastReaderDrainNanoseconds
                   << ",\"max_switch_ns\":" << snapshot.maxSwitchNanoseconds
                   << ",\"ramp_samples\":" << snapshot.rampSamples
                   << ",\"fallback_blocks\":" << snapshot.fallbackBlocks << "}\n";
        std::cout << "PASS: P9 switch handoff, bounded reader drain metrics, gain ramp, sequence continuity and failure fallback.\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
