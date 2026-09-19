// Executes the hash-gated host SRC in an isolated process. No audio stream,
// driver, GP application, hook or user configuration is opened or modified.
#include <Windows.h>
#include "input_drain_probe.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <vector>

namespace {
using Construct = void *(*)(void *, int, int);
using Destroy = void (*)(void *);
using Process = std::int64_t (*)(void *, const float *, const void *, float *, const void *, std::int64_t);
template <typename T> void put(void *p, std::size_t offset, T value) {
    std::memcpy(static_cast<std::uint8_t *>(p) + offset, &value, sizeof(value));
}
bool check(bool valid, const char *message) {
    if (!valid) std::cerr << "FAIL: " << message << '\n';
    return valid;
}
}

int wmain(int argc, wchar_t **argv) {
    if (argc != 2) return 2;
    const auto module = LoadLibraryExW(argv[1], nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module) { std::cerr << "LoadLibrary failed: " << GetLastError() << '\n'; return 3; }
    auto *base = reinterpret_cast<std::uint8_t *>(module);
    const auto construct = reinterpret_cast<Construct>(base + 0x516B0);
    const auto destroy = reinterpret_cast<Destroy>(base + 0x517F0);
    const auto process = reinterpret_cast<Process>(base + 0x51890);
    auto *format = base + 0x2507E8;
    // This function-local native format is normally initialized by the first
    // GP callback. Initialize it using its real constructor in this test-only
    // process; no stream or application state exists here.
    using Format = void *(*)(void *, int, std::uint8_t, int, int);
    reinterpret_cast<Format>(base + 0x43A40)(format, 3, 32, 2, 0);
    for (const auto rate : {48000, 88200, 96000, 176400, 192000}) {
        void *dirty = nullptr, *clean = nullptr;
        construct(&dirty, 44100, rate);
        construct(&clean, 44100, rate);
        std::array<std::uint8_t, 40> owner{};
        put(owner.data(), 8, static_cast<void *>(base));
        put(owner.data(), 0x20, static_cast<void *>(&dirty));
        gpvst3::input::drainprobe::Snapshot observed;
        const bool read = gpvst3::input::drainprobe::readSnapshot(base, owner.data(),
            {1, 1, 1, double(rate), true}, observed);
        if (!read && observed.error != gpvst3::input::drainprobe::Error::InvalidRing) {
            const auto &t = observed.config.channel[0];
            std::cerr << "rate=" << rate << " error=" << int(observed.error)
                      << " count=" << t.convolverCount << " input=" << t.inputLen
                      << " previous=" << t.previousInputLen << " block=" << t.blockLen2
                      << " latency=" << t.latency << " src=" << t.interpolatorSourceRate
                      << " dst=" << t.interpolatorDestinationRate << '\n';
        }
        // The isolated process has no native stream/ring; SRC objects above
        // are real. InvalidRing is expected only after complete topology checks.
        if (!check(read || observed.error == gpvst3::input::drainprobe::Error::InvalidRing,
                   "real host SRC topology passes all static/dynamic checks")) return 4;
        gpvst3::input::drain::Tracker tracker;
        if (!check(tracker.configure(observed.config), "actual topology config accepted")) return 5;
        const auto bounds = tracker.snapshot();
        std::vector<float> a(2048 * 2), b(2048 * 2), outA(16384 * 2), outB(16384 * 2);
        // Same phase/count history, different old input. Only the dirty SRC
        // contains native-listener-like history when suppression begins.
        std::fill(a.begin(), a.end(), 0.75F);
        for (int i = 0; i < 40; ++i) {
            process(&dirty, a.data(), format, outA.data(), format, 113);
            process(&clean, b.data(), format, outB.data(), format, 113);
        }
        unsigned phase = 0;
        std::uint64_t phaseFrames = 0, compared = 0;
        double maxDifference = 0;
        constexpr std::array<std::size_t, 5> frames{{1, 17, 64, 511, 2048}};
        for (unsigned block = 0; block < 400; ++block) {
            const auto count = frames[block % frames.size()];
            for (std::size_t i = 0; i < count * 2; ++i)
                a[i] = b[i] = float(std::sin(double(block * 4096 + i) * 0.007) * 0.1);
            const auto na = process(&dirty, a.data(), format, outA.data(), format, count);
            const auto nb = process(&clean, b.data(), format, outB.data(), format, count);
            if (!check(na == nb && na >= 0 && na <= 16384, "old history cannot change output frame count")) return 6;
            if (phase == 2) {
                for (std::int64_t i = 0; i < na * 2; ++i) {
                    const auto difference = std::abs(double(outA[std::size_t(i)]) - outB[std::size_t(i)]);
                    if (!check(std::isfinite(difference), "native outputs remain finite")) return 7;
                    maxDifference = (std::max)(maxDifference, difference);
                    ++compared;
                }
            } else {
                phaseFrames += count;
                const auto target = phase == 0 ? bounds.convolverTarget : bounds.interpolatorTarget;
                if (phaseFrames >= target) {
                    phaseFrames = 0;
                    phase = phase == 0 && bounds.interpolatorTarget ? 1 : 2;
                }
            }
        }
        if (!check(compared > 10000 && maxDifference <= 1e-6,
                   "all post-bound output excludes old input history")) return 8;
        std::cout << "rate=" << rate << " stages=" << observed.config.channel[0].convolverCount
                  << " A=" << bounds.convolverTarget << " B=" << bounds.interpolatorTarget
                  << " compared=" << compared << " max_difference=" << maxDifference << '\n';
        destroy(&dirty);
        destroy(&clean);
    }
    FreeLibrary(module);
    std::cout << "PASS: actual AMAudio SRC topology and old-history exclusion; no driver/deadline acceptance implied.\n";
    return 0;
}
