#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <thread>
#include <vector>

#include <Windows.h>

#include "input_probe.h"

namespace {
thread_local bool forbidAllocations = false;
std::atomic<std::size_t> callbackAllocations{0};
}

void *operator new(std::size_t size) {
    if (forbidAllocations) callbackAllocations.fetch_add(1, std::memory_order_relaxed);
    if (void *result = std::malloc(size ? size : 1)) return result;
    throw std::bad_alloc();
}
void *operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void *value) noexcept { std::free(value); }
void operator delete[](void *value) noexcept { std::free(value); }
void operator delete(void *value, std::size_t) noexcept { std::free(value); }
void operator delete[](void *value, std::size_t) noexcept { std::free(value); }

namespace {
using namespace gpvst3::input::probe;

bool check(bool value, const char *message) {
    if (!value) std::cerr << "FAIL: " << message << '\n';
    return value;
}

Metadata metadata(std::uint64_t sequence = 1) {
    Metadata result;
    result.sequence = sequence;
    result.timestampNanoseconds = sequence * 100;
    result.streamGeneration = 3;
    result.status = 7;
    result.frames = 128;
    result.sampleRate = 48000.0;
    result.inputChannels = 2;
    result.outputChannels = 2;
    result.inputDevice = 4;
    result.outputDevice = 5;
    result.hostApiType = 3;
    result.driverFrames = 64;
    result.configurationValid = true;
    return result;
}

Completion completion(const Metadata &value) {
    Completion result;
    result.originalEndNanoseconds = value.timestampNanoseconds + 10;
    result.callbackEndNanoseconds = value.timestampNanoseconds + 15;
    result.originalNanoseconds = 10;
    result.callbackNanoseconds = 15;
    result.callbackResult = 2;
    return result;
}

bool defaultAndPublication() {
    auto recorder = std::make_unique<Recorder>();
    Record record;
    record.metadata.sequence = 77;
    if (!check(!recorder->enabled() && !recorder->claim(metadata()) &&
               recorder->claimed() == 0 && recorder->published() == 0 &&
               !recorder->snapshot(0, record) && record.metadata.sequence == 77,
               "disabled by default and failed reads leave destination untouched")) return false;
    if (!check(recorder->configure({true, true}) && !recorder->configure({false, false}) &&
               recorder->enabled(), "configuration is one-shot")) return false;
    auto first = recorder->claim(metadata());
    auto second = recorder->claim(metadata(2));
    float source[32]{};
    for (std::size_t i = 0; i < 32; ++i) source[i] = static_cast<float>(i);
    if (!check(first && second && first.index() == 0 && second.index() == 1,
               "claims own unique slots")) return false;
    if (!check(recorder->captureInput(first, source, true) &&
               !recorder->snapshot(first.index(), record), "partial record is invisible")) return false;
    if (!check(recorder->captureInput(second, source, true) &&
               recorder->complete(second, completion(metadata(2)), source, true) &&
               !recorder->snapshot(first.index(), record) && recorder->snapshot(1, record) &&
               recorder->published() == 1, "publication need not follow claim order")) return false;
    auto moved = std::move(first);
    if (!check(!first && moved && !recorder->complete(first, {}, nullptr, false),
               "ticket transfer revokes the original writer")) return false;
    if (!check(recorder->complete(moved, completion(metadata()), source, true) && !moved &&
               !recorder->captureInput(moved, source, true) &&
               !recorder->complete(moved, {}, nullptr, false),
               "published record cannot be modified or published twice")) return false;
    if (!check(recorder->snapshot(0, record) && record.capture.frames == 16 &&
               record.capture.channels == 2 && record.capture.values[31] == 31.0F &&
               record.postOriginal.values[31] == 31.0F &&
               record.metadata.driverFrames == 64 && record.metadata.frames == 128 &&
               record.completion.callbackResult == 2 && !recorder->snapshot(kCapacity, record),
               "bounded samples and independent driver/callback metadata")) return false;
    return true;
}

bool sampleGuards() {
    // An inaccessible page makes accidental reads (including reading output
    // before it has been established valid) fail the test immediately.
    void *guard = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
    if (!check(guard != nullptr, "guard page allocation")) return false;
    auto recorder = std::make_unique<Recorder>();
    recorder->configure({true, true});
    auto unvalidated = recorder->claim(metadata());
    recorder->captureInput(unvalidated, guard, false);
    recorder->complete(unvalidated, {}, guard, false);
    Record record;
    bool ok = check(recorder->snapshot(0, record) &&
        record.capture.status == SampleStatus::Unvalidated &&
        record.postOriginal.status == SampleStatus::Unvalidated,
        "unvalidated input/output are never read");
    auto invalidConfig = metadata(2);
    invalidConfig.configurationValid = false;
    auto configuration = recorder->claim(invalidConfig);
    recorder->captureInput(configuration, guard, true);
    recorder->complete(configuration, {}, guard, true);
    ok &= check(recorder->snapshot(1, record) &&
        record.capture.status == SampleStatus::Unvalidated &&
        record.postOriginal.status == SampleStatus::Unvalidated,
        "sample validity cannot override invalid host configuration");
    auto missing = recorder->claim(metadata(3));
    recorder->captureInput(missing, nullptr, true);
    recorder->complete(missing, {}, nullptr, true);
    ok &= check(recorder->snapshot(2, record) && record.capture.status == SampleStatus::Missing &&
        record.postOriginal.status == SampleStatus::Missing, "missing pointers are explicit");
    auto invalidFormat = metadata(4);
    invalidFormat.inputChannels = 3;
    invalidFormat.outputChannels = 0;
    auto badFormat = recorder->claim(invalidFormat);
    recorder->captureInput(badFormat, guard, true);
    recorder->complete(badFormat, {}, guard, true);
    ok &= check(recorder->snapshot(3, record) && record.capture.status == SampleStatus::InvalidFormat &&
        record.postOriginal.status == SampleStatus::InvalidFormat, "unsupported channels are not read");
    auto emptyFormat = metadata(5);
    emptyFormat.frames = 0;
    auto empty = recorder->claim(emptyFormat);
    recorder->captureInput(empty, guard, true);
    recorder->complete(empty, {}, guard, true);
    ok &= check(recorder->snapshot(4, record) && record.capture.status == SampleStatus::InvalidFormat &&
        record.postOriginal.status == SampleStatus::InvalidFormat, "empty buffers are not read");
    float source[32]{};
    source[0] = std::numeric_limits<float>::quiet_NaN();
    source[8] = std::numeric_limits<float>::infinity();
    source[31] = -std::numeric_limits<float>::infinity();
    auto nonfinite = recorder->claim(metadata(6));
    recorder->captureInput(nonfinite, source, true);
    recorder->complete(nonfinite, {}, source, true);
    ok &= check(recorder->snapshot(5, record) && record.capture.status == SampleStatus::Captured &&
        record.capture.nonFiniteMask == 0x80000101U && record.capture.values[0] == 0.0F &&
        record.capture.values[31] == 0.0F && record.postOriginal.nonFiniteMask == 0x80000101U,
        "non-finite samples are marked and represented safely");
    auto aliasMeta = metadata(7);
    aliasMeta.frames = 2;
    aliasMeta.inputChannels = 1;
    aliasMeta.outputChannels = 1;
    aliasMeta.pointersAlias = true;
    float aliased[2]{0.25F, -0.5F};
    auto alias = recorder->claim(aliasMeta);
    recorder->captureInput(alias, aliased, true);
    aliased[0] = 0.75F;
    recorder->complete(alias, {}, aliased, true);
    ok &= check(recorder->snapshot(6, record) && record.capture.frames == 2 &&
        record.capture.channels == 1 && record.capture.values[0] == 0.25F &&
        record.postOriginal.values[0] == 0.75F && record.capture.values[2] == 0.0F &&
        record.metadata.pointersAlias, "capture copies survive original callback alias writes");
    auto disabledSamples = std::make_unique<Recorder>();
    disabledSamples->configure({true, false});
    auto disabled = disabledSamples->claim(metadata());
    disabledSamples->captureInput(disabled, guard, true);
    disabledSamples->complete(disabled, {}, guard, true);
    ok &= check(disabledSamples->snapshot(0, record) && record.capture.status == SampleStatus::Disabled &&
        record.postOriginal.status == SampleStatus::Disabled, "metadata-only recording reads no samples");
    VirtualFree(guard, 0, MEM_RELEASE);
    return ok;
}

bool concurrentPublicationAndCapacity() {
    auto recorder = std::make_unique<Recorder>();
    recorder->configure({true, true});
    auto withheld = recorder->claim(metadata(9999));
    std::atomic<bool> start{false};
    std::atomic<bool> done{false};
    std::atomic<bool> valid{true};
    std::atomic<std::size_t> reads{0};
    std::vector<std::thread> readers;
    std::vector<std::thread> writers;
    const auto validRecord = [](const Record &record) {
        const auto sequence = record.metadata.sequence;
        if (sequence < 1 || sequence > 800 || record.metadata.timestampNanoseconds != sequence * 100 ||
            record.completion.originalEndNanoseconds != sequence * 100 + 10 ||
            record.completion.callbackEndNanoseconds != sequence * 100 + 15 ||
            record.capture.frames != 16 || record.postOriginal.frames != 16) return false;
        for (std::size_t i = 0; i < 32; ++i)
            if (record.capture.values[i] != static_cast<float>(sequence) ||
                record.postOriginal.values[i] != -static_cast<float>(sequence)) return false;
        return true;
    };
    for (int reader = 0; reader < 4; ++reader) readers.emplace_back([&] {
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        Record record;
        do {
            forbidAllocations = true;
            for (std::size_t index = 0; index < recorder->claimed(); ++index) {
                if (!recorder->snapshot(index, record)) continue;
                if (!validRecord(record)) valid.store(false, std::memory_order_relaxed);
                reads.fetch_add(1, std::memory_order_relaxed);
            }
            forbidAllocations = false;
        } while (!done.load(std::memory_order_acquire));
    });
    for (std::uint64_t writer = 0; writer < 8; ++writer) writers.emplace_back([&, writer] {
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        float capture[32];
        float output[32];
        for (std::uint64_t i = 0; i < 100; ++i) {
            auto value = metadata(writer * 100 + i + 1);
            value.thread = static_cast<std::uint32_t>(writer);
            for (std::size_t sample = 0; sample < 32; ++sample) {
                capture[sample] = static_cast<float>(value.sequence);
                output[sample] = -capture[sample];
            }
            forbidAllocations = true;
            auto ticket = recorder->claim(value);
            if (ticket) {
                recorder->captureInput(ticket, capture, true);
                // Readers can run between the sample write and publication.
                std::this_thread::yield();
                recorder->complete(ticket, completion(value), output, true);
            }
            forbidAllocations = false;
        }
    });
    start.store(true, std::memory_order_release);
    for (auto &thread : writers) thread.join();
    done.store(true, std::memory_order_release);
    for (auto &thread : readers) thread.join();
    Record record;
    bool ok = check(valid.load() && reads.load() > 0 && recorder->claimed() == kCapacity &&
        recorder->published() == kCapacity - 1 && !recorder->snapshot(0, record),
        "concurrent readers never observe partial or overwritten records");
    std::array<bool, 801> identities{};
    for (std::size_t index = 1; index < kCapacity; ++index) {
        if (!recorder->snapshot(index, record) || !validRecord(record) || identities[record.metadata.sequence]) {
            ok = check(false, "unique concurrent claims and capacity");
            break;
        }
        identities[record.metadata.sequence] = true;
    }
    for (int attempt = 0; attempt < 10000; ++attempt)
        if (recorder->claim(metadata())) ok = check(false, "full recorder never reuses a slot");
    recorder->complete(withheld, {}, nullptr, false);
    ok &= check(recorder->published() == kCapacity && recorder->snapshot(0, record) &&
        record.metadata.sequence == 9999 && callbackAllocations.load() == 0,
        "late publish and callback operations perform no allocation");
    return ok;
}
} // namespace

int main() {
    if (!defaultAndPublication() || !sampleGuards() || !concurrentPublicationAndCapacity()) return 1;
    std::cout << "PASS: P13-0 probe defaults, bounded samples, output guards, aliasing, non-finite values, "
                 "one-shot publication, concurrent readers/writers, fixed capacity, no callback allocation.\n";
    return 0;
}
