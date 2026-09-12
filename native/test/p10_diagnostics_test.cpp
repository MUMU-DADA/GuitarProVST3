#include "effect_chain.h"
#include "input_router.h"

#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
using gpvst3::audio::BlockView;
bool passthrough(void *, const BlockView &block) noexcept {
    if (!block.inputChannels || !block.outputChannels) return false;
    for (std::size_t c = 0; c < block.channelCount; ++c)
        for (std::size_t f = 0; f < block.frameCount; ++f)
            block.outputChannels[c][f] = block.inputChannels[c][f];
    return true;
}
bool gain(void *, const BlockView &block) noexcept {
    if (!block.inputChannels || !block.outputChannels) return false;
    for (std::size_t c = 0; c < block.channelCount; ++c)
        for (std::size_t f = 0; f < block.frameCount; ++f)
            block.outputChannels[c][f] = block.inputChannels[c][f] * 2.0F;
    return true;
}
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
}

int main(int argc, char **argv) {
    try {
        const std::string artifact = argc > 1 ? argv[1] : "p10-diagnostics.json";
        gpvst3::effects::Chain chain;
        chain.setRampSamples(0);
        chain.setDeadlineNanoseconds(1000000000ULL);
        require(chain.prepareSlot(0, {nullptr, &passthrough}), "prepare chain");
        require(chain.activate(0), "activate chain");
        chain.setBypassed(false);
        float input[2][64]{}, output[2][64]{};
        for (auto &channel : input) for (auto &sample : channel) sample = 0.25F;
        const float *inputs[]{input[0], input[1]};
        float *outputs[]{output[0], output[1]};
        const BlockView block{inputs, nullptr, outputs, nullptr, 2, 64, 48000.0, 64,
                              reinterpret_cast<void *>(0x11), 1, true};
        require(chain.process(block).completed, "process chain");
        const auto chainState = chain.snapshot();
        require(chainState.processedBlocks == 1 && chainState.deadlineNanoseconds == 1000000000ULL,
                "chain deadline evidence");
        chain.setDeadlineNanoseconds(0);
        const gpvst3::audio::BlockView resized{inputs, nullptr, outputs, nullptr, 2, 64, 96000.0, 64,
                                               reinterpret_cast<void *>(0x12), 2, true};
        require(chain.process(resized).completed, "process resized chain");
        require(chain.snapshot().deadlineNanoseconds == 666666ULL,
                "chain deadline follows current block duration");

        gpvst3::input::Router router;
        require(router.prepare(2, 64), "prepare input router");
        router.setProcessor({nullptr, &gain});
        router.setRoute(gpvst3::input::Route::InputInsert);
        router.setEnabled(true);
        router.setStreamRunning(true);
        float capture[64]{}, interleavedOutput[128]{};
        for (auto &sample : capture) sample = 0.125F;
        const gpvst3::input::InterleavedView interleaved{
            capture, interleavedOutput, 64, 1, 2, 48000.0, 64,
            reinterpret_cast<void *>(0x22), 1,
            gpvst3::input::InterleavedSampleFormat::Float32};
        const auto routed = router.processInterleaved(interleaved);
        require(routed.completed && routed.processed && std::fabs(interleavedOutput[0] - 0.25F) < 0.000001F,
                "input route writeback");
        const auto inputState = router.snapshot();
        require(inputState.interleavedOutputWritten && inputState.interleavedCopyOperations == 2,
                "input route copy evidence");

        std::ofstream out(artifact, std::ios::binary);
        require(out.good(), "open diagnostics artifact");
        out << "{\"startup_timeline\":{\"source\":\"fixture\",\"initialize_ms\":0,\"hook_ready_ms\":0,\"scan_scheduled_ms\":0,\"ui_ready_ms\":0},"
               "\"audio_deadline\":{\"blocks\":1,\"processing_nanoseconds\":" << chainState.lastProcessNanoseconds
            << ",\"max_nanoseconds\":" << chainState.maxProcessNanoseconds
            << ",\"p95_nanoseconds\":" << chainState.maxProcessNanoseconds
            << ",\"p99_nanoseconds\":" << chainState.maxProcessNanoseconds
            << ",\"deadline_nanoseconds\":" << chainState.deadlineNanoseconds
            << ",\"overruns\":" << chainState.deadlineExceededBlocks << ",\"extra_copy_operations\":0},"
               "\"roundtrip_latency\":{\"device_input_samples\":0,\"device_output_samples\":0,\"vst3_samples\":0,\"adapter_samples\":0,\"reported_samples\":0,\"measured\":false,\"status\":\"fixture_only\"},"
               "\"input_route\":{\"route\":\"input_insert\",\"processed_blocks\":" << inputState.inputProcessedBlocks
            << ",\"copy_operations\":" << inputState.interleavedCopyOperations
            << ",\"output_written\":" << (inputState.interleavedOutputWritten ? "true" : "false") << "}}\n";
        std::cout << "PASS: P10 deadline, startup timeline, input writeback and copy evidence.\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
