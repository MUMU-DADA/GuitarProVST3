#define GPVST3_P13_PROBE_BUILD
#include "input_drain_probe.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <new>
#include <vector>

namespace {
thread_local bool forbidAllocations = false;
std::atomic<std::size_t> unexpectedAllocations{0};
}

void *operator new(std::size_t size) {
    if (forbidAllocations) ++unexpectedAllocations;
    if (auto *memory = std::malloc(size ? size : 1)) return memory;
    throw std::bad_alloc();
}
void *operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void *memory) noexcept { std::free(memory); }
void operator delete[](void *memory) noexcept { std::free(memory); }
void operator delete(void *memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void *memory, std::size_t) noexcept { std::free(memory); }

namespace {
using namespace gpvst3::input;
using namespace drainprobe;

template <typename T> void put(void *memory, std::size_t offset, T value) {
    std::memcpy(static_cast<std::uint8_t *>(memory) + offset, &value, sizeof(value));
}

struct Fixture {
    std::vector<std::uint8_t> module = std::vector<std::uint8_t>(0x270880);
    std::array<std::uint8_t, 40> owner{};
    std::array<std::uint8_t, 8> src{};
    std::array<std::uint8_t, 0xF0> impl{};
    std::array<std::array<std::uint8_t, 0x88>, 2> conv{};
    std::array<std::array<std::uint8_t, 0x50>, 2> filter{};
    std::array<std::array<std::uint8_t, 0x88>, 2> secondConv{};
    std::array<std::array<std::uint8_t, 0x50>, 2> secondFilter{};
    std::array<std::array<std::uint8_t, 0x1030>, 2> interp{};
    Context context{3, 7, 2, 192000, true};

    Fixture() {
        put(owner.data(), 8, static_cast<void *>(module.data()));
        put(owner.data(), 0x20, static_cast<void *>(src.data()));
        put(src.data(), 0, static_cast<void *>(impl.data()));
        put(module.data(), 0x2507F0, std::int32_t{2});
        for (std::size_t i = 0; i < 2; ++i) {
            auto *r = impl.data() + i * 0x78;
            put(r, 0, module.data() + 0x194608);
            put(r, 8, conv[i].data());
            put(r, 0x48, std::int32_t{1});
            put(r, 0x50, interp[i].data());
            auto *c = conv[i].data();
            put(c, 0, module.data() + 0x194588);
            put(c, 8, filter[i].data());
            put(c, 0x28, std::int32_t{2});
            put(c, 0x2C, std::int32_t{1});
            put(c, 0x30, std::uint8_t{1});
            put(c, 0x34, std::int32_t{16});
            put(c, 0x3C, std::int32_t{2});
            put(c, 0x40, std::int32_t{12});
            put(c, 0x44, std::int32_t{14});
            put(c, 0x50, std::int32_t{1});
            put(c, 0x80, std::int32_t{12});
            put(filter[i].data(), 0x48, std::int32_t{5});
            put(filter[i].data(), 0x4C, std::int32_t{3});
            put(interp[i].data(), 0, module.data() + 0x194648);
            put(interp[i].data(), 0x1008, double{88200});
            put(interp[i].data(), 0x1010, double{192000});
        }
        auto *ring = module.data() + 0x270810;
        put(ring, 0, std::uint64_t{32768});
        put(ring, 8, std::uint64_t{32767});
        put(ring, 0x18, module.data());
    }

    void ring(std::uint64_t queued, std::uint64_t read, std::uint64_t write) {
        auto *memory = module.data() + 0x270810;
        put(memory, 0x10, queued);
        put(memory, 0x30, read);
        put(memory, 0x38, write);
    }
    bool snapshot(Snapshot &result) {
        return readSnapshot(module.data(), owner.data(), context, result);
    }
    void rate(double value) {
        context.actualRate = value;
        if (value == 44100) {
            put(owner.data(), 0x20, static_cast<void *>(nullptr));
            return;
        }
        for (std::size_t i = 0; i < 2; ++i) {
            auto *r = impl.data() + i * 0x78;
            if (value == 88200 || value == 176400)
                put(r, 0x50, static_cast<void *>(nullptr));
            else put(interp[i].data(), 0x1010, value);
            if (value == 176400) {
                secondConv[i] = conv[i];
                secondFilter[i] = filter[i];
                put(secondConv[i].data(), 8, secondFilter[i].data());
                put(r, 16, secondConv[i].data());
                put(r, 0x48, std::uint32_t{2});
            }
        }
    }
};

bool check(bool condition, const char *message) {
    if (!condition) std::cerr << "FAIL: " << message << '\n';
    return condition;
}

bool snapshotAndRing() {
    Fixture f;
    Snapshot a, b;
    if (!check(f.snapshot(a) && a.config.channel[0].inputLen == 12 &&
               a.config.maxCallbackFrames == 2048 && a.outputSrc == f.src.data(),
               "snapshot reads actual SRC identity and bounded config")) return false;
    f.ring(10, 32760, 2);
    if (!check(f.snapshot(a), "wrapped queue snapshot accepted")) return false;
    f.ring(14, 6, 20);
    if (!check(f.snapshot(b), "post snapshot accepted")) return false;
    std::uint64_t consumed = 999;
    if (!check(consumedSamples(a, b, 64, consumed) && consumed == 14,
               "read delta crosses ring wrap without confusing queued or appended")) return false;
    b.context.rateRevision++;
    if (!check(!consumedSamples(a, b, 64, consumed) && consumed == 0,
               "rate revision change invalidates pre/post pair")) return false;
    if (!f.snapshot(b)) return false;
    b.ring.storage = f.src.data();
    if (!check(!sameTopology(a, b), "ring storage replacement invalidates pair")) return false;
    f.ring(0, 200, 200);
    if (!f.snapshot(b)) return false;
    if (!check(!consumedSamples(a, b, 64, consumed), "more than requested cannot masquerade as consumption")) return false;
    f.ring(32768, 8, 8);
    return check(f.snapshot(b), "full ring is distinguished from empty by count");
}

bool invalidSnapshots() {
    using Mutation = void (*)(Fixture &);
    const Mutation invalid[] = {
        [](Fixture &f) { f.context.rateValidated = false; },
        [](Fixture &f) { f.context.generation = 0; },
        [](Fixture &f) { f.context.actualRate = 96000; },
        [](Fixture &f) { put(f.owner.data(), 0x20, static_cast<void *>(nullptr)); },
        [](Fixture &f) { put(f.impl.data(), 0, f.module.data()); },
        [](Fixture &f) { put(f.impl.data(), 0x48, std::int32_t{2}); },
        [](Fixture &f) { put(f.filter[1].data(), 0x48, std::int32_t{7}); },
        [](Fixture &f) { put(f.filter[1].data(), 0x4C, std::int32_t{30}); },
        [](Fixture &f) { put(f.conv[1].data(), 0x3C, std::int32_t{-1}); },
        [](Fixture &f) { put(f.conv[1].data(), 0x80, std::int32_t{13}); },
        [](Fixture &f) { put(f.conv[1].data(), 0x84, std::int32_t{15}); },
        [](Fixture &f) { put(f.conv[1].data(), 0x48, std::numeric_limits<double>::quiet_NaN()); },
        [](Fixture &f) { put(f.interp[0].data(), 0x1008, double{44100}); },
        [](Fixture &f) { put(f.interp[0].data(), 0x1020, std::int32_t{246}); },
        [](Fixture &f) { put(f.interp[0].data(), 0x1024, std::int32_t{-1}); },
        [](Fixture &f) { put(f.interp[0].data(), 0x1028, std::int32_t{256}); },
        [](Fixture &f) { f.ring(2, 0, 4); },
        [](Fixture &f) { f.ring(1, 0, 1); },
        [](Fixture &f) { f.ring(32770, 0, 2); },
        [](Fixture &f) { put(f.module.data(), 0x2507F0, std::int32_t{1}); },
    };
    for (const auto mutate : invalid) {
        Fixture f;
        mutate(f);
        Snapshot s;
        if (!check(!f.snapshot(s) && s.error != Error::None, "malformed topology/dynamics/context/ring rejected")) return false;
    }
    return true;
}

bool trackerIntegrationNoAllocation() {
    Fixture f;
    Snapshot initial;
    if (!f.snapshot(initial)) return false;
    drain::Tracker tracker;
    if (!tracker.configure(initial.config)) return false;
    bool valid = true;
    forbidAllocations = true;
    std::uint64_t queue = 0, read = 0, write = 0;
    for (std::uint64_t sequence = 1; sequence <= 40 && valid; ++sequence) {
        Snapshot before, after;
        valid = f.snapshot(before) && tracker.beginCallback(before.config, sequence, 64, queue, true);
        if (queue < 128) {
            valid = valid && tracker.srcCompleted(16, 70);
            queue += 140;
            write = (write + 140) & 32767;
        }
        const auto amount = queue < 128 ? queue : 128;
        queue -= amount;
        read = (read + amount) & 32767;
        f.ring(queue, read, write);
        std::uint64_t consumed = 0;
        valid = valid && f.snapshot(after) && consumedSamples(before, after, 64, consumed) &&
            consumed == amount && tracker.endCallback(after.config, queue, consumed);
    }
    forbidAllocations = false;
    return check(valid && tracker.snapshot().state == drain::State::Ready && unexpectedAllocations.load() == 0,
                 "observed snapshots feed actual Tracker through all phases without allocation");
}

bool rateTopologyMatrix() {
    for (const auto rate : {44100., 48000., 88200., 96000., 176400., 192000.}) {
        Fixture f;
        f.rate(rate);
        Snapshot before, after;
        if (!check(f.snapshot(before) && before.config.destinationRate == rate &&
                   before.config.usesOutputRing == (rate != 44100),
                   "actual device rate selects verified direct/one-stage/two-stage topology")) return false;
        if (!check(f.snapshot(after) && sameTopology(before, after),
                   "all supported topology snapshots compare consistently")) return false;
        if (rate == 176400) {
            put(f.secondConv[1].data(), 0x44, std::uint32_t{15});
            if (!check(f.snapshot(after) && !sameTopology(before, after),
                       "second convolver change invalidates continuity")) return false;
            put(f.secondConv[1].data(), 0x80, std::int32_t{13});
            if (!check(!f.snapshot(after), "second convolver dynamic bounds checked")) return false;
        }
        if (rate == 44100) {
            std::uint64_t consumed = 99;
            if (!check(consumedSamples(before, after, 2048, consumed) && consumed == 0,
                       "equal-rate route never inspects inactive shared ring")) return false;
        }
    }
    return true;
}
}

int main() {
    if (!snapshotAndRing() || !invalidSnapshots() || !trackerIntegrationNoAllocation() ||
        !rateTopologyMatrix()) return 1;
    std::cout << "PASS: drain snapshot topology/dynamic gates, ring wrap evidence, identity changes, Tracker integration, no allocation.\n";
    return 0;
}
