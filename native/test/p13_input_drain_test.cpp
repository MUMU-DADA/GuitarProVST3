#include <atomic>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>

#include "input_drain.h"

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
using namespace gpvst3::input::drain;
constexpr auto max64 = std::numeric_limits<std::uint64_t>::max();

bool check(bool value, const char *message) {
    if (!value) std::cerr << "FAIL: " << message << '\n';
    return value;
}

Config config() {
    Config c;
    c.epoch = 19;
    c.channels = 2;
    c.sourceRate = 44100;
    c.destinationRate = 192000;
    c.ringCapacitySamples = 32768;
    c.maxSrcInputFrames = 32768;
    c.maxCallbackFrames = 32768;
    for (auto &t : c.channel) {
        t.convolverCount = 1;
        t.upFactor = 2;
        t.downFactor = 1;
        t.blockLen2 = 16;
        t.previousInputLen = 2;
        t.inputLen = 12;
        t.latency = 14;
        t.upShift = 1;
        t.interpolatorSourceRate = 88200;
        t.interpolatorDestinationRate = 192000;
        t.consumesLatency = true;
    }
    return c;
}

// Explicit evidence: each case states the expected post-callback queue and
// actual consumption. The fixture never manufactures those two observations.
struct Fixture {
    Config c = config();
    Tracker tracker;
    std::uint64_t sequence = 1;
    std::uint64_t queue = 0;
    bool start() { return tracker.configure(c); }
    bool callback(std::uint64_t frames, bool src, std::uint64_t input,
                  std::uint64_t output, std::uint64_t after, std::uint64_t consumed) {
        if (!tracker.beginCallback(c, sequence++, frames, queue, true)) return false;
        if (src && !tracker.srcCompleted(input, output)) return false;
        if (!tracker.endCallback(c, after, consumed)) return false;
        queue = after;
        return true;
    }
};

bool defaultsAndValidation() {
    Tracker unconfigured;
    const auto c = config();
    if (!check(unconfigured.snapshot().state == State::Invalid &&
               !unconfigured.beginCallback(c, 1, 64, 0, true) &&
               !unconfigured.srcCompleted(1, 1) && !unconfigured.endCallback(c, 0, 0),
               "unconfigured tracker cannot produce progress")) return false;
    if (!check(unconfigured.configure(c) && !unconfigured.configure(c) &&
               unconfigured.snapshot().error == Error::Reconfigured &&
               !unconfigured.beginCallback(c, 1, 1, 0, true),
               "configuration is one-shot and failure is sticky")) return false;

    using Mutation = void (*)(Config &);
    const Mutation invalid[] = {
        [](Config &x) { x.epoch = 0; },
        [](Config &x) { x.channels = 0; },
        [](Config &x) { x.channels = 3; },
        [](Config &x) { x.sourceRate = 48000; },
        [](Config &x) { x.destinationRate = 44100; },
        [](Config &x) { x.ringCapacitySamples = 0; },
        [](Config &x) { x.ringCapacitySamples = 65536; },
        [](Config &x) { x.maxSrcInputFrames = 0; },
        [](Config &x) { x.maxSrcInputFrames = 32769; },
        [](Config &x) { x.maxCallbackFrames = 0; },
        [](Config &x) { x.maxCallbackFrames = 32769; },
        [](Config &x) { x.channel[1].convolverCount = 2; },
        [](Config &x) { x.channel[1].upFactor = 1; },
        [](Config &x) { x.channel[1].downFactor = 2; },
        [](Config &x) { x.channel[1].consumesLatency = false; },
        [](Config &x) { x.channel[1].inputDelay = 1; },
        [](Config &x) { x.channel[1].upShift = 0; },
        [](Config &x) { x.channel[1].downShift = 1; },
        [](Config &x) { x.channel[1].interpolatorSourceRate = 44100; },
        [](Config &x) { x.channel[1].interpolatorDestinationRate = 96000; },
        [](Config &x) { x.channel[1].blockLen2 = 0; },
        [](Config &x) { x.channel[1].blockLen2 = 15; },
        [](Config &x) { x.channel[1].inputLen = 0; },
        [](Config &x) { x.channel[1].inputLen = 13; },
        [](Config &x) { x.channel[1].previousInputLen = 7; },
        [](Config &x) { x.channel[1].previousInputLen = 1; },
        [](Config &x) { x.channel[1].latency = 11; },
        [](Config &x) { x.channel[1].latency = 0xFFFFFFFFU; },
        [](Config &x) { x.channel[1].inputLen = 0xFFFFFFFEU; },
    };
    for (const auto mutate : invalid) {
        auto bad = c;
        mutate(bad);
        Tracker t;
        if (!check(!t.configure(bad) && t.snapshot().state == State::Invalid &&
                   t.snapshot().error == Error::InvalidConfig && !t.configure(c),
                   "invalid topology and unsafe capacities fail closed")) return false;
    }
    return true;
}

bool thresholdsAndNextCallback() {
    Fixture f;
    if (!check(f.start() && f.tracker.snapshot().convolverTarget == 18 &&
               f.tracker.snapshot().interpolatorTarget == 135, "derived thresholds")) return false;
    if (!check(f.callback(64, true, 17, 64, 0, 128) &&
               f.tracker.snapshot().phase == Phase::Convolver &&
               f.tracker.snapshot().convolverFrames == 17, "one below first threshold")) return false;
    if (!check(f.callback(64, true, 32768, 64, 0, 128) &&
               f.tracker.snapshot().phase == Phase::Interpolator &&
               f.tracker.snapshot().convolverFrames == 18 &&
               f.tracker.snapshot().interpolatorFrames == 0,
               "first crossing call surplus cannot advance interpolator")) return false;
    if (!check(f.callback(64, true, 134, 64, 0, 128) &&
               f.tracker.snapshot().phase == Phase::Interpolator,
               "one below interpolator threshold")) return false;
    if (!check(f.callback(64, true, 32768, 64, 0, 128) &&
               f.tracker.snapshot().interpolatorFrames == 135 &&
               f.tracker.snapshot().phase == Phase::NextCallback &&
               f.tracker.snapshot().state == State::Preparing,
               "empty frontier still cannot mark crossing callback ready")) return false;
    if (!check(f.tracker.beginCallback(f.c, f.sequence, 0, 0, true) &&
               f.tracker.snapshot().state == State::Ready &&
               f.tracker.endCallback(f.c, 0, 0), "only next verified entry becomes ready")) return false;
    return true;
}

bool realFrontierConsumption() {
    Fixture f;
    if (!check(f.start() && f.callback(64, true, 18, 64, 0, 128) &&
               f.callback(64, true, 135, 100, 72, 128) &&
               f.tracker.snapshot().phase == Phase::Ring &&
               f.tracker.snapshot().oldRingSamples == 72,
               "frontier is taken after whole callback without double subtraction")) return false;
    if (!check(f.callback(0, false, 0, 0, 72, 0) &&
               f.tracker.snapshot().oldRingSamples == 72, "zero device frames do not drain")) return false;
    if (!check(f.callback(16, false, 0, 0, 40, 32) &&
               f.tracker.snapshot().oldRingSamples == 40 &&
               f.callback(16, false, 0, 0, 8, 32) &&
               f.tracker.snapshot().oldRingSamples == 8,
               "skipped SRC calls still consume real ring samples")) return false;
    if (!check(f.callback(16, true, 4, 32, 40, 32) &&
               f.tracker.snapshot().oldRingSamples == 0 &&
               f.tracker.snapshot().phase == Phase::NextCallback &&
               f.tracker.snapshot().state == State::Preparing,
               "clean new appends do not extend old frontier or activate current block")) return false;
    if (!check(f.callback(16, false, 0, 0, 8, 32) &&
               f.tracker.snapshot().state == State::Ready,
               "next block can be ready while queue contains only clean samples")) return false;
    return true;
}

bool unequalChannelsAndNoProgress() {
    Fixture f;
    auto &t = f.c.channel[1];
    t.blockLen2 = 32;
    t.previousInputLen = 4;
    t.inputLen = 24;
    t.latency = 29; // Odd latency exercises ceil rather than truncation.
    if (!check(f.start() && f.tracker.snapshot().convolverTarget == 36 &&
               f.tracker.snapshot().interpolatorTarget == 143,
               "both unequal channel bounds use maximum and integer ceiling")) return false;
    for (int i = 0; i < 2000; ++i) {
        if (!f.callback(0, false, 0, 0, 0, 0)) return false;
        if (!f.callback(64, true, 0, 0, 0, 0)) return false;
    }
    if (!check(f.tracker.snapshot().convolverFrames == 0 &&
               f.tracker.snapshot().state == State::Preparing, "no clock or callback-count progress")) return false;
    if (!check(f.callback(64, true, 18, 64, 0, 128) &&
               f.tracker.snapshot().phase == Phase::Convolver &&
               f.callback(64, true, 18, 64, 0, 128) &&
               f.tracker.snapshot().phase == Phase::Interpolator &&
               f.callback(64, true, 135, 64, 0, 128) &&
               f.tracker.snapshot().phase == Phase::Interpolator &&
               f.callback(64, true, 8, 64, 0, 128) &&
               f.tracker.snapshot().phase == Phase::NextCallback,
               "shorter channel cannot prematurely complete either phase")) return false;
    return true;
}

bool invalidEvidence() {
    const auto c = config();
    auto expectInvalid = [](Tracker &t, Error error) {
        return check(t.snapshot().state == State::Invalid && t.snapshot().error == error &&
                     !t.srcCompleted(32768, 0), "error is terminal and cannot advance to ready");
    };
    {
        Tracker t; t.configure(c);
        if (t.srcCompleted(1, 1) || !expectInvalid(t, Error::CallbackOrder)) return false;
    }
    {
        Tracker t; t.configure(c);
        if (t.endCallback(c, 0, 0) || !expectInvalid(t, Error::CallbackOrder)) return false;
    }
    {
        Tracker t; t.configure(c); t.beginCallback(c, 1, 64, 0, true);
        if (t.beginCallback(c, 2, 64, 0, true) || !expectInvalid(t, Error::CallbackOrder)) return false;
    }
    {
        Tracker t; t.configure(c);
        if (t.beginCallback(c, 0, 64, 0, true) || !expectInvalid(t, Error::CallbackOrder)) return false;
    }
    for (const auto sequence : {std::uint64_t{1}, std::uint64_t{3}}) {
        Tracker t; t.configure(c); t.beginCallback(c, 1, 0, 0, true); t.endCallback(c, 0, 0);
        if (t.beginCallback(c, sequence, 0, 0, true) || !expectInvalid(t, Error::CallbackOrder)) return false;
    }
    {
        Tracker t; t.configure(c); t.beginCallback(c, max64, 0, 0, true); t.endCallback(c, 0, 0);
        if (t.beginCallback(c, 1, 0, 0, true) || !expectInvalid(t, Error::CallbackOrder)) return false;
    }
    {
        Tracker t; t.configure(c);
        if (t.beginCallback(c, 1, 64, 0, false) || !expectInvalid(t, Error::SuppressionLost)) return false;
    }
    for (const auto queue : {std::uint64_t{1}, std::uint64_t{32770}, max64}) {
        Tracker t; t.configure(c);
        if (t.beginCallback(c, 1, 64, queue, true) || !expectInvalid(t, Error::InvalidCount)) return false;
    }
    {
        Tracker t; t.configure(c);
        if (t.beginCallback(c, 1, max64, 0, true) || !expectInvalid(t, Error::InvalidCount)) return false;
    }
    {
        Tracker t; t.configure(c); t.beginCallback(c, 1, 64, 0, true); t.srcCompleted(1, 64);
        if (t.srcCompleted(1, 1) || !expectInvalid(t, Error::DuplicateSrc)) return false;
    }
    for (const auto input : {std::uint64_t{32769}, max64}) {
        Tracker t; t.configure(c); t.beginCallback(c, 1, 64, 0, true);
        if (t.srcCompleted(input, 0) || !expectInvalid(t, Error::InvalidSrc)) return false;
    }
    for (const auto output : {std::uint64_t{16385}, max64}) {
        Tracker t; t.configure(c); t.beginCallback(c, 1, 64, 0, true);
        if (t.srcCompleted(1, output) || !expectInvalid(t, Error::InvalidSrc)) return false;
    }
    {
        Tracker t; t.configure(c); t.beginCallback(c, 1, 64, 0, true);
        if (t.srcCompleted(0, 1) || !expectInvalid(t, Error::InvalidSrc)) return false;
    }
    {
        Tracker t; t.configure(c); t.beginCallback(c, 1, 64, 128, true);
        if (t.srcCompleted(1, 1) || !expectInvalid(t, Error::InvalidSrc)) return false;
    }
    {
        Tracker t; t.configure(c); t.beginCallback(c, 1, 64, 0, true);
        if (t.endCallback(c, 0, 0) || !expectInvalid(t, Error::MissingSrc)) return false;
    }
    {
        Tracker t; t.configure(c); t.beginCallback(c, 1, 64, 2, true);
        if (t.srcCompleted(1, 16384) || !expectInvalid(t, Error::InvalidRingEvidence)) return false;
    }
    for (const auto consumed : {std::uint64_t{0}, std::uint64_t{127}, std::uint64_t{130}, max64}) {
        Tracker t; t.configure(c); t.beginCallback(c, 1, 64, 0, true); t.srcCompleted(1, 64);
        if (t.endCallback(c, 0, consumed) || !expectInvalid(t, Error::InvalidRingEvidence)) return false;
    }
    for (const auto after : {std::uint64_t{1}, std::uint64_t{2}, max64}) {
        Tracker t; t.configure(c); t.beginCallback(c, 1, 64, 0, true); t.srcCompleted(1, 64);
        if (t.endCallback(c, after, 128) || !expectInvalid(t, Error::InvalidRingEvidence)) return false;
    }
    {
        Tracker t; t.configure(c); t.beginCallback(c, 1, 0, 0, true); t.endCallback(c, 0, 0);
        if (t.beginCallback(c, 2, 0, 2, true) || !expectInvalid(t, Error::RingDiscontinuity)) return false;
    }
    {
        Fixture f; f.start(); f.callback(64, true, 18, 64, 0, 128);
        if (f.callback(64, true, 135, 0, 0, 0) || !expectInvalid(f.tracker, Error::InvalidSrc))
            return false;
    }
    {
        // Real SRC warmup can return zero in some calls. B must see eventual
        // output but cannot advance at all on a zero-length input call.
        Fixture f; f.start(); f.callback(64, true, 18, 64, 0, 128);
        if (!f.callback(64, true, 0, 0, 0, 0) ||
            !check(f.tracker.snapshot().interpolatorFrames == 0,
                   "zero input cannot advance second phase") ||
            !f.callback(64, true, 1, 0, 0, 0) ||
            !f.callback(64, true, 134, 64, 0, 128) ||
            !check(f.tracker.snapshot().phase == Phase::NextCallback,
                   "some zero-output calls are allowed before real output progress")) return false;
    }
    return true;
}

bool configChangesCannotActivate() {
    for (int point = 0; point < 4; ++point) {
        for (int change = 0; change < 3; ++change) {
            Fixture f;
            if (!f.start()) return false;
            if (point >= 1 && !f.callback(64, true, 18, 64, 0, 128)) return false;
            if (point >= 2 && !f.callback(64, true, 135, 64, 0, 128)) return false;
            if (point >= 3 && !f.callback(0, false, 0, 0, 0, 0)) return false;
            auto changed = f.c;
            if (change == 0) ++changed.epoch;
            if (change == 1) ++changed.channel[1].latency;
            if (change == 2) changed.channels = 1;
            if (!check(!f.tracker.beginCallback(changed, f.sequence, 0, 0, true) &&
                       f.tracker.snapshot().state == State::Invalid &&
                       f.tracker.snapshot().error == Error::ConfigChanged,
                       "generation or parameters invalidate all phases including pending/ready")) return false;
        }
    }
    Fixture f;
    if (!f.start() || !f.tracker.beginCallback(f.c, 1, 64, 0, true) ||
        !f.tracker.srcCompleted(18, 64)) return false;
    auto changed = f.c;
    ++changed.epoch;
    return check(!f.tracker.endCallback(changed, 0, 128) &&
                 f.tracker.snapshot().state == State::Invalid, "mid-callback epoch change invalidates");
}

bool largeBoundsAndNoAllocations() {
    Fixture f;
    // Largest practical arithmetic stress accepted by uint32 topology, without
    // allocating the fictional host buffers. Bounds must stay uint64, not wrap.
    for (auto &c : f.c.channel) {
        c.blockLen2 = 0x80000000U;
        c.previousInputLen = 0;
        c.inputLen = 0x80000000U;
        c.latency = 0xFFFFFFFFU;
    }
    forbidAllocations = true;
    bool ok = f.start() && f.tracker.snapshot().convolverTarget == 3221225472ULL &&
        f.tracker.snapshot().interpolatorTarget == 2147483776ULL;
    for (int i = 0; i < 1000 && ok; ++i) ok = f.callback(64, true, 32768, 64, 0, 128);
    ok = ok && f.tracker.snapshot().state == State::Preparing &&
        f.tracker.snapshot().convolverFrames == 32768000ULL;
    forbidAllocations = false;
    if (!check(ok && allocations.load(std::memory_order_relaxed) == 0,
               "large finite targets and callback work allocate nothing")) return false;

    Fixture mono;
    mono.c.channels = 1;
    if (!check(mono.start() && mono.callback(32768, true, 18, 32768, 0, 32768) &&
               mono.callback(32768, true, 135, 32768, 0, 32768) &&
               mono.tracker.snapshot().state == State::Preparing &&
               mono.callback(0, false, 0, 0, 0, 0) && mono.tracker.snapshot().state == State::Ready,
               "exact full-capacity mono bounds are safe")) return false;
    return true;
}
}

int main() {
    if (!defaultsAndValidation() || !thresholdsAndNextCallback() || !realFrontierConsumption() ||
        !unequalChannelsAndNoProgress() || !invalidEvidence() || !configChangesCannotActivate() ||
        !largeBoundsAndNoAllocations()) return 1;
    std::cout << "PASS: P13 experimental drain FSM; no host or audio acceptance implied.\n";
    return 0;
}
