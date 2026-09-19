#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <iterator>
#include <limits>
#include <new>

#include "input_router.h"

namespace {
thread_local bool forbidAllocations = false;
std::atomic<std::size_t> allocations{0};
}

void *operator new(std::size_t size) {
    if (forbidAllocations) allocations.fetch_add(1, std::memory_order_relaxed);
    if (void *result = std::malloc(size ? size : 1)) return result;
    throw std::bad_alloc();
}
void *operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void *value) noexcept { std::free(value); }
void operator delete[](void *value) noexcept { std::free(value); }
void operator delete(void *value, std::size_t) noexcept { std::free(value); }
void operator delete[](void *value, std::size_t) noexcept { std::free(value); }

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

bool alwaysFalse(void *, const gpvst3::audio::BlockView &) noexcept { return false; }

bool writeNan(void *, const gpvst3::audio::BlockView &block) noexcept {
    if (!block.outputChannels) return false;
    for (std::size_t channel = 0; channel < block.channelCount; ++channel) {
        if (!block.outputChannels[channel]) return false;
        for (std::size_t frame = 0; frame < block.frameCount; ++frame)
            block.outputChannels[channel][frame] = std::numeric_limits<float>::quiet_NaN();
    }
    return true;
}

bool writeMax(void *, const gpvst3::audio::BlockView &block) noexcept {
    if (!block.outputChannels) return false;
    for (std::size_t channel = 0; channel < block.channelCount; ++channel) {
        if (!block.outputChannels[channel]) return false;
        for (std::size_t frame = 0; frame < block.frameCount; ++frame)
            block.outputChannels[channel][frame] = std::numeric_limits<float>::max();
    }
    return true;
}

bool nonFinite(void *, const gpvst3::audio::BlockView &block) noexcept {
    if (!block.outputChannels || !block.inputChannels) return false;
    for (std::size_t channel = 0; channel < block.channelCount; ++channel)
        for (std::size_t frame = 0; frame < block.frameCount; ++frame)
            block.outputChannels[channel][frame] = frame == 1
                ? std::numeric_limits<float>::quiet_NaN() : 0.25F;
    return true;
}

struct Observations {
    std::size_t calls = 0;
    std::size_t frames = 0;
    bool receivedGenerated = false;
    gpvst3::input::Router *changeMode = nullptr;
};

bool observedGain(void *context, const gpvst3::audio::BlockView &block) noexcept {
    auto &observed = *static_cast<Observations *>(context);
    ++observed.calls;
    observed.frames = block.frameCount;
    observed.receivedGenerated = observed.receivedGenerated || block.generatedChannels != nullptr;
    if (observed.changeMode) {
        observed.changeMode->setRoute(gpvst3::input::Route::InputInsert);
        observed.changeMode->setBypassed(true);
    }
    return gainTwo(nullptr, block);
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

    // Overlay must preserve the already rendered host output and commit the
    // addition only after the independent processor has produced a finite
    // complete block.
    outputLeft[0] = 0.5F;
    outputRight[0] = 0.25F;
    router.setRoute(input::Route::Overlay);
    result = router.process(capture, {}, output);
    if (!check(result.completed && result.processed && close(outputLeft[0], 1.0F) &&
                   close(outputRight[0], 0.45F), "overlay adds to existing output"))
        return 1;

    const auto snapshot = router.snapshot();
    if (!check(snapshot.captureObserved && snapshot.levelObserved && snapshot.captureBlocks == 4,
               "level/capture counters"))
        return 1;
    if (!check(snapshot.inputProcessedBlocks == 2 && snapshot.busMixedBlocks == 1 &&
                   snapshot.bypassBlocks == 1 && close(snapshot.maxPeak, 0.5F) &&
                   snapshot.sampleRate == 48000.0,
               "route counters and meter"))
        return 1;

    // Overlay failure is transactional: a failed processor, non-finite
    // processor output, or a non-finite sum must leave every host sample
    // untouched.  The successful path also deliberately permits values above
    // 1.0; overlay does not add a limiter or reuse the legacy clamp.
    float failedLeft[frames]{0.2F, -0.2F, 0.3F, -0.3F};
    float failedRight[frames]{0.4F, -0.4F, 0.5F, -0.5F};
    float failedLeftBefore[frames]{};
    float failedRightBefore[frames]{};
    std::copy(std::begin(failedLeft), std::end(failedLeft), std::begin(failedLeftBefore));
    std::copy(std::begin(failedRight), std::end(failedRight), std::begin(failedRightBefore));
    float *failedOutput[2]{failedLeft, failedRight};
    const input::OutputView failedView{failedOutput, 2};
    router.setProcessor({nullptr, &alwaysFalse});
    result = router.process(capture, {}, failedView);
    if (!check(!result.completed && result.error &&
                   std::equal(std::begin(failedLeft), std::end(failedLeft),
                              std::begin(failedLeftBefore)) &&
                   std::equal(std::begin(failedRight), std::end(failedRight),
                              std::begin(failedRightBefore)),
               "overlay processor failure is transactional"))
        return 1;
    router.setProcessor({nullptr, &writeNan});
    result = router.process(capture, {}, failedView);
    if (!check(!result.completed && result.error &&
                   std::equal(std::begin(failedLeft), std::end(failedLeft),
                              std::begin(failedLeftBefore)) &&
                   std::equal(std::begin(failedRight), std::end(failedRight),
                              std::begin(failedRightBefore)),
               "overlay non-finite processor output is transactional"))
        return 1;
    router.setProcessor({nullptr, &writeMax});
    failedRight[frames - 1] = std::numeric_limits<float>::max();
    failedRightBefore[frames - 1] = std::numeric_limits<float>::max();
    result = router.process(capture, {}, failedView);
    if (!check(!result.completed && result.error &&
                   std::equal(std::begin(failedLeft), std::end(failedLeft),
                              std::begin(failedLeftBefore)) &&
                   std::equal(std::begin(failedRight), std::end(failedRight),
                              std::begin(failedRightBefore)),
               "overlay add overflow is transactional"))
        return 1;

    // Restore the normal processor.  Capture/output aliasing is safe because
    // capture is copied to the prepared scratch before the commit pass.
    router.setProcessor({nullptr, &gainTwo});
    float aliasLeft[frames]{0.25F, -0.25F, 0.5F, -0.5F};
    float aliasRight[frames]{0.1F, -0.1F, 0.2F, -0.2F};
    const float *aliasCaptureChannels[2]{aliasLeft, aliasRight};
    float *aliasOutputChannels[2]{aliasLeft, aliasRight};
    const input::CaptureView aliasCapture{aliasCaptureChannels, 2, frames, 48000.0, frames};
    const input::OutputView aliasOutput{aliasOutputChannels, 2};
    result = router.process(aliasCapture, {}, aliasOutput);
    if (!check(result.completed && close(aliasLeft[0], 0.75F) &&
                   close(aliasRight[0], 0.3F), "overlay planar input/output alias"))
        return 1;

    // A non-finite plugin result must not partially publish an overlay block.
    outputLeft[0] = 0.75F;
    outputLeft[1] = -0.25F;
    outputRight[0] = 0.5F;
    outputRight[1] = -0.5F;
    router.setProcessor({nullptr, &nonFinite});
    result = router.process(capture, {}, output);
    if (!check(!result.completed && result.error && close(outputLeft[0], 0.75F) &&
                   close(outputLeft[1], -0.25F) && close(outputRight[0], 0.5F) &&
                   close(outputRight[1], -0.5F), "overlay nonfinite rollback"))
        return 1;

    // Interleaved overlay uses an exact two-pass commit and therefore neither
    // clamps a valid sum nor writes a prefix before a later invalid sample.
    router.setProcessor({nullptr, &gainTwo});
    float overlayInput[frames]{0.25F, 0.5F, -0.25F, -0.5F};
    float overlayOutput[frames * 2]{1.25F, -1.25F, 1.0F, -1.0F,
                                    0.75F, -0.75F, 0.5F, -0.5F};
    const input::InterleavedView overlayView{
        overlayInput, overlayOutput, frames, 1, 2, 48000.0, frames,
        reinterpret_cast<void *>(0x43), 21, input::InterleavedSampleFormat::Float32};
    result = router.processInterleaved(overlayView);
    if (!check(result.completed && close(overlayOutput[0], 1.75F) &&
                   close(overlayOutput[1], -0.75F), "interleaved overlay has no clamp"))
        return 1;

    router.setProcessor({nullptr, &nonFinite});
    const float rollback[frames * 2]{2.0F, -2.0F, 1.5F, -1.5F,
                                     1.0F, -1.0F, 0.5F, -0.5F};
    std::copy(std::begin(rollback), std::end(rollback), std::begin(overlayOutput));
    result = router.processInterleaved(overlayView);
    if (!check(!result.completed &&
                   std::equal(std::begin(rollback), std::end(rollback),
                              std::begin(overlayOutput)),
               "interleaved overlay nonfinite rollback"))
        return 1;

    router.setProcessor({nullptr, &gainTwo});

    // PortAudio contract: capture/output are borrowed interleaved float32
    // buffers. Verify the mono-capture to stereo-output mapping and writeback.
    router.setRoute(input::Route::InputInsert);
    const auto beforeLegacyInterleaved = router.snapshot();
    float captureInterleaved[frames]{0.25F, -0.25F, 0.5F, -0.5F};
    float outputInterleaved[frames * 2]{};
    const input::InterleavedView interleaved{
        captureInterleaved, outputInterleaved, frames, 1, 2, 48000.0, frames,
        reinterpret_cast<void *>(0x42), 17, input::InterleavedSampleFormat::Float32};
    result = router.processInterleaved(interleaved);
    if (!check(result.completed && result.processed && close(outputInterleaved[0], 0.5F) &&
                   close(outputInterleaved[1], 0.5F) && close(outputInterleaved[6], -1.0F) &&
                   close(outputInterleaved[7], -1.0F),
               "interleaved mono capture to stereo output"))
        return 1;
    const auto interleavedSnapshot = router.snapshot();
    if (!check(interleavedSnapshot.interleavedFormatObserved &&
                   interleavedSnapshot.interleavedInputObserved &&
                   interleavedSnapshot.interleavedOutputWritten &&
                   interleavedSnapshot.interleavedBlocks == beforeLegacyInterleaved.interleavedBlocks + 1 &&
                   interleavedSnapshot.interleavedInputChannelCount == 1 &&
                   interleavedSnapshot.interleavedOutputChannelCount == 2 &&
                   interleavedSnapshot.firstCaptureAddress != 0 &&
                   interleavedSnapshot.firstCaptureOwner == 0x43,
               "interleaved ownership and format evidence"))
        return 1;
    float stereoCaptureInterleaved[frames * 2]{
        0.25F, 0.5F, -0.25F, -0.5F, 0.5F, 0.25F, -0.5F, -0.25F};
    float monoOutputInterleaved[frames]{};
    const input::InterleavedView stereoToMono{
        stereoCaptureInterleaved, monoOutputInterleaved, frames, 2, 1, 48000.0, frames,
        reinterpret_cast<void *>(0x42), 18, input::InterleavedSampleFormat::Float32};
    result = router.processInterleaved(stereoToMono);
    if (!check(result.completed && result.processed && close(monoOutputInterleaved[0], 0.75F) &&
                   close(monoOutputInterleaved[3], -0.75F),
               "interleaved stereo capture to mono output"))
        return 1;
    const input::InterleavedView missing{
        nullptr, outputInterleaved, frames, 1, 2, 48000.0, frames,
        reinterpret_cast<void *>(0x42), 19, input::InterleavedSampleFormat::Float32};
    result = router.processInterleaved(missing);
    if (!check(!result.completed && router.snapshot().interleavedMissingBlocks == 1,
               "missing capture fails closed"))
        return 1;
    const input::InterleavedView invalidFormat{
        captureInterleaved, outputInterleaved, frames, 1, 2, 48000.0, frames,
        reinterpret_cast<void *>(0x42), 20,
        static_cast<input::InterleavedSampleFormat>(0x7F)};
    result = router.processInterleaved(invalidFormat);
    if (!check(!result.completed && router.snapshot().interleavedFormatErrors == 1,
               "unsupported interleaved format fails closed"))
        return 1;

    // Interleaved overlay commits the un-clamped sum and supports a borrowed
    // input/output alias.  Use non-block-sized calls to cover capacity-internal
    // variable frames without rebuilding the prepared storage.
    router.setProcessor({nullptr, &gainTwo});
    router.setRoute(input::Route::Overlay);
    float overlayCapture[5 * 2]{0.25F, 0.5F, -0.25F, -0.5F,
                                 0.5F, 0.25F, -0.5F, -0.25F, 0.125F, 0.25F};
    float variedOutput[5 * 2]{0.75F, 0.75F, 0.75F, 0.75F,
                                0.75F, 0.75F, 0.75F, 0.75F, 0.75F, 0.75F};
    const input::InterleavedView variedView{
        overlayCapture, variedOutput, 5, 2, 2, 48000.0, 5,
        reinterpret_cast<void *>(0x43), 21, input::InterleavedSampleFormat::Float32};
    result = router.processInterleaved(variedView);
    if (!check(result.completed && close(variedOutput[0], 1.25F) &&
                   close(variedOutput[1], 1.75F) && close(variedOutput[8], 1.0F) &&
                   close(variedOutput[9], 1.25F), "interleaved overlay is not clamped"))
        return 1;
    float aliasedInterleaved[frames * 2]{0.25F, 0.5F, -0.25F, -0.5F,
                                         0.5F, 0.25F, -0.5F, -0.25F};
    const input::InterleavedView aliasedView{
        aliasedInterleaved, aliasedInterleaved, frames, 2, 2, 48000.0, frames,
        reinterpret_cast<void *>(0x43), 22, input::InterleavedSampleFormat::Float32};
    result = router.processInterleaved(aliasedView);
    if (!check(result.completed && close(aliasedInterleaved[0], 0.75F) &&
                   close(aliasedInterleaved[1], 1.5F),
               "interleaved overlay input/output alias"))
        return 1;
    float monoOverlayCapture[frames * 2]{0.25F, 0.5F, -0.25F, -0.5F,
                                         0.5F, 0.25F, -0.5F, -0.25F};
    float monoOverlayOutput[frames]{0.5F, 0.5F, 0.5F, 0.5F};
    const input::InterleavedView monoOverlayView{
        monoOverlayCapture, monoOverlayOutput, frames, 2, 1, 48000.0, frames,
        reinterpret_cast<void *>(0x43), 23, input::InterleavedSampleFormat::Float32};
    result = router.processInterleaved(monoOverlayView);
    if (!check(result.completed && close(monoOverlayOutput[0], 1.25F),
               "interleaved overlay stereo-to-mono mapping"))
        return 1;

    for (const auto processor : {&alwaysFalse, &nonFinite, &writeMax}) {
        std::copy(std::begin(rollback), std::end(rollback), std::begin(overlayOutput));
        if (processor == &writeMax) overlayOutput[frames * 2 - 1] = std::numeric_limits<float>::max();
        float before[frames * 2];
        std::memcpy(before, overlayOutput, sizeof(before));
        router.setProcessor({nullptr, processor});
        result = router.processInterleaved(overlayView);
        if (!check(!result.completed && result.error && !result.bypassed &&
                       std::memcmp(before, overlayOutput, sizeof(before)) == 0,
                   "interleaved processor error/late NaN/add overflow preserve every host byte"))
            return 1;
    }

    // Inactive overlay must never emit the captured dry input.
    router.setProcessor({nullptr, &gainTwo});
    for (int mode = 0; mode != 3; ++mode) {
        router.setEnabled(mode != 0);
        router.setBypassed(mode == 1);
        router.setStreamRunning(mode != 2);
        float before[frames];
        std::memcpy(before, failedLeft, sizeof(before));
        result = router.process(capture, generated, failedView);
        if (!check(result.completed && result.bypassed &&
                       std::memcmp(before, failedLeft, sizeof(before)) == 0,
                   "inactive planar overlay preserves host output")) return 1;
        float interleavedBefore[frames * 2];
        std::memcpy(interleavedBefore, overlayOutput, sizeof(interleavedBefore));
        result = router.processInterleaved(overlayView);
        if (!check(result.completed && result.bypassed &&
                       std::memcmp(interleavedBefore, overlayOutput, sizeof(interleavedBefore)) == 0,
                   "inactive interleaved overlay preserves host output")) return 1;
    }
    router.setEnabled(true);
    router.setBypassed(false);
    router.setStreamRunning(true);

    float overlapStorage[frames + 1]{0.2F, 0.3F, 0.4F, 0.5F, 0.6F};
    float overlapBefore[frames + 1];
    std::memcpy(overlapBefore, overlapStorage, sizeof(overlapBefore));
    float *overlapChannels[2]{overlapStorage, overlapStorage + 1};
    result = router.process(capture, {}, {overlapChannels, 2});
    if (!check(!result.completed && result.error &&
                   std::memcmp(overlapStorage, overlapBefore, sizeof(overlapBefore)) == 0,
               "overlapping planar output channels fail without partial commit")) return 1;

    // Missing capture channels and already invalid host output must also be
    // rejected without substituting silence or changing the remaining output.
    const float *missingChannel[2]{captureLeft, nullptr};
    const input::CaptureView missingPlanar{missingChannel, 2, frames, 48000.0, frames};
    float planarBefore[frames];
    std::memcpy(planarBefore, failedLeft, sizeof(planarBefore));
    result = router.process(missingPlanar, {}, failedView);
    if (!check(!result.completed && result.error &&
                   std::memcmp(planarBefore, failedLeft, sizeof(planarBefore)) == 0,
               "missing planar capture channel preserves output")) return 1;
    std::fill_n(overlayOutput, frames * 2, 0.75F);
    overlayOutput[frames * 2 - 1] = std::numeric_limits<float>::quiet_NaN();
    float invalidHostBefore[frames * 2];
    std::memcpy(invalidHostBefore, overlayOutput, sizeof(invalidHostBefore));
    result = router.processInterleaved(overlayView);
    if (!check(!result.completed && result.error &&
                   std::memcmp(invalidHostBefore, overlayOutput, sizeof(invalidHostBefore)) == 0,
               "late non-finite host sample prevents every output write")) return 1;
    router.setProcessor({});
    result = router.processInterleaved(overlayView);
    if (!check(!result.completed && result.error && !result.bypassed &&
                   std::memcmp(invalidHostBefore, overlayOutput, sizeof(invalidHostBefore)) == 0,
               "empty overlay processor does not emit dry fallback")) return 1;

    // A callback observes its selected route once even if a control setter is
    // called while its processor is executing. The next block sees the change.
    Observations switchObserved;
    switchObserved.changeMode = &router;
    router.setProcessor({&switchObserved, &observedGain});
    std::fill_n(overlayOutput, frames * 2, 0.75F);
    result = router.processInterleaved(overlayView);
    if (!check(result.completed && close(overlayOutput[0], 1.25F) &&
                   router.snapshot().route == input::Route::InputInsert,
               "overlay commits using the mode selected at block entry")) return 1;

    input::Router variable;
    if (!check(variable.prepare(2, 2048), "prepare variable-block overlay")) return 1;
    Observations observed;
    variable.setProcessor({&observed, &observedGain});
    variable.setEnabled(true);
    variable.setStreamRunning(true);
    variable.setRoute(input::Route::Overlay);
    float variableInput[2048 * 2 + 2];
    float variableOutput[2048 * 2 + 2];
    std::fill_n(variableInput, std::size(variableInput), 0.25F);
    for (const std::size_t count : {1U, 64U, 127U, 128U, 256U, 512U, 1024U, 2048U, 65U}) {
        std::fill_n(variableOutput, std::size(variableOutput), 0.75F);
        input::InterleavedView view{variableInput, variableOutput, count, 2, 2, 192000.0, 2048};
        const auto beforeCalls = observed.calls;
        forbidAllocations = true;
        result = variable.processInterleaved(view);
        forbidAllocations = false;
        if (!check(result.completed && result.processed && observed.frames == count &&
                       observed.calls == beforeCalls + 1 && !observed.receivedGenerated &&
                       variable.frameCapacity() == 2048 && allocations.load() == 0 &&
                       variableOutput[count * 2 - 1] == 1.25F && variableOutput[count * 2] == 0.75F,
                   "64/128/256/512/1024 and variable blocks use exact frames without allocation")) return 1;
    }
    // Invalid size/format/input and NaN capture cannot reach the processor or
    // alter host output. The capture buffer is valid but contains sentinel data.
    for (int invalid = 0; invalid != 8; ++invalid) {
        input::InterleavedView view{variableInput, variableOutput, 64, 2, 2, 192000.0, 2048};
        switch (invalid) {
        case 0: view.frameCount = 0; break;
        case 1: view.frameCount = 2049; break;
        case 2: view.input = nullptr; break;
        case 3: view.output = nullptr; break;
        case 4: view.inputChannelCount = 3; break;
        case 5: view.format = static_cast<input::InterleavedSampleFormat>(99); break;
        case 6: view.sampleRate = std::numeric_limits<double>::quiet_NaN(); break;
        case 7: variableInput[127] = std::numeric_limits<float>::quiet_NaN(); break;
        }
        std::fill_n(variableOutput, std::size(variableOutput), 0.75F);
        const auto beforeCalls = observed.calls;
        forbidAllocations = true;
        result = variable.processInterleaved(view);
        forbidAllocations = false;
        if (!check(!result.completed && observed.calls == beforeCalls &&
                       std::all_of(std::begin(variableOutput), std::end(variableOutput),
                                   [](float sample) { return sample == 0.75F; }) && allocations.load() == 0,
                   "invalid overlay view preserves output and never calls processor")) return 1;
        variableInput[127] = 0.25F;
    }

    // Borrowed views may partially overlap, not just share the same address.
    // The untouched input suffix proves the commit stays within output bounds.
    std::fill_n(variableInput, std::size(variableInput), 0.25F);
    input::InterleavedView partialAlias{variableInput, variableInput + 1, 64, 2, 2, 192000.0, 2048};
    result = variable.processInterleaved(partialAlias);
    if (!check(result.completed && variableInput[0] == 0.25F &&
                   variableInput[1] == 0.75F && variableInput[128] == 0.75F &&
                   variableInput[129] == 0.25F, "partially overlapping interleaved pointers")) return 1;
    std::cout << "PASS: P4 legacy routing and P13 transactional overlay, alias, mapping, variable frames and no allocation.\n";
    return 0;
}
