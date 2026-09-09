#include <cmath>
#include <cstddef>
#include <iostream>

#include "input_router.h"

namespace {

bool gainTwo(void *, const gpvst3::audio::BlockView &block) noexcept {
    if (!block.outputChannels || !block.inputChannels) return false;
    for (std::size_t channel = 0; channel < block.channelCount; ++channel) {
        if (!block.outputChannels[channel] || !block.inputChannels[channel]) return false;
        for (std::size_t frame = 0; frame < block.frameCount; ++frame)
            block.outputChannels[channel][frame] = block.inputChannels[channel][frame] * 2.0F;
    }
    return true;
}

bool close(float actual, float expected) { return std::fabs(actual - expected) < 0.0001F; }

bool check(bool condition, const char *message) {
    if (!condition) std::cerr << "FAIL: " << message << '\n';
    return condition;
}

} // namespace

int main() {
    using namespace gpvst3;
    input::Router router;
    if (!check(router.prepare(2, 8), "prepare")) return 1;
    router.setProcessor({nullptr, &gainTwo});
    router.setEnabled(true);
    router.setStreamRunning(true);

    constexpr std::size_t frames = 4;
    float captureLeft[frames]{0.25F, -0.25F, 0.5F, -0.5F};
    float captureRight[frames]{0.1F, -0.1F, 0.2F, -0.2F};
    const float *captureChannels[2]{captureLeft, captureRight};
    float generatedLeft[frames]{0.5F, 0.5F, 0.5F, 0.5F};
    float generatedRight[frames]{0.25F, 0.25F, 0.25F, 0.25F};
    const float *generatedChannels[2]{generatedLeft, generatedRight};
    float outputLeft[frames]{};
    float outputRight[frames]{};
    float *outputChannels[2]{outputLeft, outputRight};
    const input::CaptureView capture{captureChannels, 2, frames, 48000.0, frames};
    const input::GeneratedView generated{generatedChannels, 2};
    const input::OutputView output{outputChannels, 2};

    router.setRoute(input::Route::InputInsert);
    auto result = router.process(capture, {}, output);
    if (!check(result.completed && result.processed && close(outputLeft[0], 0.5F),
               "input insert"))
        return 1;

    router.setRoute(input::Route::BusMix);
    result = router.process(capture, generated, output);
    if (!check(result.completed && result.processed && close(outputLeft[0], 1.5F) &&
                   close(outputRight[0], 0.7F),
               "bus mix"))
        return 1;

    router.setRoute(input::Route::Disabled);
    result = router.process(capture, generated, output);
    if (!check(result.completed && result.bypassed && close(outputLeft[0], 0.5F),
               "disabled passthrough"))
        return 1;

    const auto snapshot = router.snapshot();
    if (!check(snapshot.captureObserved && snapshot.levelObserved && snapshot.captureBlocks == 3,
               "level/capture counters"))
        return 1;
    if (!check(snapshot.inputProcessedBlocks == 1 && snapshot.busMixedBlocks == 1 &&
                   snapshot.bypassBlocks == 1 && close(snapshot.maxPeak, 0.5F) &&
                   snapshot.sampleRate == 48000.0,
               "route counters and meter"))
        return 1;
    std::cout << "PASS: P4 capture monitor, input insert, bus mix and bypass routes.\n";
    return 0;
}
