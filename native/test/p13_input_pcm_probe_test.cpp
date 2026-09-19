#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <thread>

#include <Windows.h>

#include "input_pcm_probe.h"

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
using namespace gpvst3::input::pcmprobe;

bool check(bool condition, const char *message) {
    if (!condition) std::cerr << "FAIL: " << message << '\n';
    return condition;
}

Metadata metadata(std::uint64_t sequence = 1, std::size_t frames = 64) {
    Metadata m;
    m.sequence = sequence;
    m.parentSequence = sequence;
    m.timestampNanoseconds = sequence * 1000;
    m.streamGeneration = 9;
    m.frames = frames;
    m.sampleRate = 192000.0;
    m.inputChannels = 2;
    m.outputChannels = 2;
    m.ownerAddress = 0x1234;
    m.thread = 3;
    m.configurationValid = true;
    return m;
}

bool lifecycleAndAlias() {
    Recorder recorder;
    Snapshot view;
    view.record.metadata.sequence = 888;
    if (!check(!recorder.enabled() && !recorder.begin(metadata(), nullptr, true) &&
               !recorder.snapshot(0, view) && view.record.metadata.sequence == 888,
               "default disabled and failed reads preserve destination")) return false;
    if (!check(recorder.configure({true}) && !recorder.configure({true}),
               "one-time preallocation")) return false;
    float audio[128];
    for (std::size_t i = 0; i < 128; ++i) audio[i] = static_cast<float>(i) / 128.0F;
    float original[128];
    std::memcpy(original, audio, sizeof(audio));
    auto m = metadata();
    m.pointersAlias = true;
    forbidAllocations = true;
    auto ticket = recorder.begin(m, audio, true);
    forbidAllocations = false;
    if (!check(ticket && ticket.index() == 0 && recorder.claimed() == 1 &&
               recorder.published() == 0 && !recorder.snapshot(0, view) &&
               std::memcmp(original, audio, sizeof(audio)) == 0,
               "begin copies capture without writes or premature publication")) return false;
    auto moved = std::move(ticket);
    if (!check(!ticket && moved && !recorder.complete(ticket, {}, nullptr, false),
               "ticket ownership transfers once")) return false;
    for (auto &sample : audio) sample *= -2.0F;
    Completion c;
    c.callbackResult = 5;
    c.originalNanoseconds = 23;
    forbidAllocations = true;
    const bool complete = recorder.complete(moved, c, audio, true);
    forbidAllocations = false;
    if (!check(complete && !moved && !recorder.complete(moved, {}, audio, true) &&
               recorder.snapshot(0, view) && view.record.capture.status == SampleStatus::Captured &&
               view.record.postOriginal.status == SampleStatus::Captured &&
               view.record.savedFrames == 64 && !view.record.truncated &&
               view.captureSamples == 128 && view.postOriginalSamples == 128 &&
               view.capture != audio && view.postOriginal != audio &&
               view.capture[127] == original[127] && view.postOriginal[127] == -2.0F * original[127] &&
               audio[127] == -2.0F * original[127] &&
               view.record.completion.callbackResult == 5 && view.record.changes == NoChange &&
               !recorder.snapshot(kRecordCapacity, view), "full alias-safe input/output with immutable ownership"))
        return false;
    {
        auto abandoned = recorder.begin(metadata(2), audio, true);
        if (!check(abandoned && !recorder.snapshot(1, view), "abandoned ticket starts invisible")) return false;
    }
    auto later = recorder.begin(metadata(3), audio, true);
    if (!check(later && recorder.complete(later, {}, audio, true) &&
               recorder.abandoned() == 1 && !recorder.snapshot(1, view) &&
               recorder.snapshot(2, view) && (view.record.changes & SequenceDiscontinuity) != 0,
               "abandon releases writer but never publishes fake PCM")) return false;
    return true;
}

bool guardAndValidation() {
    auto *guard = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
    if (!check(guard != nullptr, "inaccessible guard page")) return false;
    Recorder recorder;
    recorder.configure({true});
    Snapshot view;
    bool ok = true;
    for (int variant = 0; variant != 9; ++variant) {
        auto m = metadata(static_cast<std::uint64_t>(variant + 1));
        bool valid = true;
        const void *source = guard;
        SampleStatus expected = SampleStatus::InvalidFormat;
        switch (variant) {
        case 0: valid = false; expected = SampleStatus::Unvalidated; break;
        case 1: m.configurationValid = false; expected = SampleStatus::Unvalidated; break;
        case 2: source = nullptr; expected = SampleStatus::Missing; break;
        case 3: m.frames = 0; break;
        case 4: m.frames = kMaxCallbackFrames + 1; break;
        case 5: m.inputChannels = 3; m.outputChannels = 3; break;
        case 6: m.inputChannels = 0; m.outputChannels = 0; break;
        case 7: m.sampleRate = 0.0; break;
        case 8: m.sampleRate = std::numeric_limits<double>::quiet_NaN(); break;
        }
        forbidAllocations = true;
        auto ticket = recorder.begin(m, source, valid);
        const auto index = ticket.index();
        const bool completed = recorder.complete(ticket, {}, source, valid);
        forbidAllocations = false;
        ok &= check(completed && recorder.snapshot(index, view) &&
                    view.record.capture.status == expected && view.record.postOriginal.status == expected &&
                    view.capture == nullptr && view.postOriginal == nullptr &&
                    view.captureSamples == 0 && view.postOriginalSamples == 0,
                    "invalid or unvalidated buffers never dereferenced or exposed as PCM");
    }
    VirtualFree(guard, 0, MEM_RELEASE);
    float samples[128]{};
    samples[0] = std::numeric_limits<float>::quiet_NaN();
    samples[127] = std::numeric_limits<float>::infinity();
    auto ticket = recorder.begin(metadata(10), samples, true);
    const auto index = ticket.index();
    recorder.complete(ticket, {}, samples, true);
    ok &= check(recorder.snapshot(index, view) && view.record.capture.status == SampleStatus::NonFinite &&
                view.record.postOriginal.status == SampleStatus::NonFinite &&
                view.record.capture.nonFiniteCount == 2 && view.record.capture.firstNonFinite == 0 &&
                view.record.postOriginal.nonFiniteCount == 2 && std::isnan(view.capture[0]) &&
                std::isinf(view.postOriginal[127]) && std::isnan(samples[0]),
                "non-finite PCM retained and explicitly invalid rather than sanitized into evidence");
    return ok;
}

bool capacitiesAndTail() {
    float samples[4096]{};
    Snapshot view;
    Recorder frames;
    if (!check(frames.configure({true, 67, 10}), "small bounded capacity")) return false;
    auto first = frames.begin(metadata(1, 64), samples, true);
    frames.complete(first, {}, samples, true);
    auto second = frames.begin(metadata(2, 64), samples, true);
    frames.complete(second, {}, samples, true);
    if (!check(frames.snapshot(1, view) && view.record.metadata.frames == 64 &&
               view.record.savedFrames == 3 && view.record.frameOffset == 64 &&
               view.record.truncated && view.captureSamples == 6 && view.postOriginalSamples == 6 &&
               frames.framesReserved() == 67 && !frames.begin(metadata(3), samples, true) &&
               frames.claimed() == 2 && frames.capacityDrops() == 1,
               "tail truncation is explicit and capacity never wraps or overwrites")) return false;
    const float *stable = view.capture;
    for (int i = 0; i < 100; ++i) {
        if (frames.begin(metadata(3), samples, true)) return false;
    }
    if (!check(frames.snapshot(1, view) && stable == view.capture &&
               frames.capacityDrops() == 101, "full recorder never reuses storage")) return false;

    auto *pages = static_cast<unsigned char *>(VirtualAlloc(nullptr, 8192,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!check(pages != nullptr, "tail guard allocation")) return false;
    DWORD previousProtection = 0;
    if (!VirtualProtect(pages + 4096, 4096, PAGE_NOACCESS, &previousProtection)) {
        VirtualFree(pages, 0, MEM_RELEASE);
        return check(false, "tail guard protection");
    }
    auto *tail = reinterpret_cast<float *>(pages + 4096) - 6;
    for (std::size_t i = 0; i < 6; ++i) tail[i] = static_cast<float>(i + 1);
    Recorder boundedTail;
    boundedTail.configure({true, 3, 1});
    auto last = boundedTail.begin(metadata(1, 2048), tail, true);
    boundedTail.complete(last, {}, tail, true);
    const bool tailValid = boundedTail.snapshot(0, view) && view.record.truncated &&
        view.record.savedFrames == 3 && view.capture[5] == 6.0F && view.postOriginal[5] == 6.0F;
    VirtualFree(pages, 0, MEM_RELEASE);
    if (!check(tailValid, "truncation never reads beyond the saved-frame prefix")) return false;

    Recorder records;
    records.configure({true, kFrameCapacity, 3});
    for (std::size_t i = 0; i < 3; ++i) {
        auto ticket = records.begin(metadata(i + 1, 1), samples, true);
        if (!records.complete(ticket, {}, samples, true)) return false;
    }
    if (!check(!records.begin(metadata(4, 1), samples, true) && records.claimed() == 3 &&
               records.framesReserved() == 3 && records.capacityDrops() == 1,
               "record capacity is independent of frame capacity")) return false;

    Recorder allRecords;
    allRecords.configure({true});
    for (std::size_t i = 0; i < kRecordCapacity; ++i) {
        auto ticket = allRecords.begin(metadata(i + 1, 1), samples, true);
        if (!allRecords.complete(ticket, {}, samples, true)) return false;
    }
    if (!check(allRecords.published() == kRecordCapacity &&
               !allRecords.begin(metadata(kRecordCapacity + 1, 1), samples, true) &&
               allRecords.snapshot(kRecordCapacity - 1, view),
               "all 8192 records publish once with no record overflow")) return false;

    Recorder maximum;
    maximum.configure({true});
    for (std::size_t i = 0; i < kFrameCapacity / kMaxCallbackFrames; ++i) {
        forbidAllocations = true;
        auto ticket = maximum.begin(metadata(i + 1, kMaxCallbackFrames), samples, true);
        const bool done = maximum.complete(ticket, {}, samples, true);
        forbidAllocations = false;
        if (!done) return false;
    }
    if (!check(maximum.framesReserved() == kFrameCapacity && maximum.published() == 128 &&
               !maximum.begin(metadata(129), samples, true) && maximum.snapshot(127, view) &&
               !view.record.truncated && view.captureSamples == 4096,
               "full 262144-frame capacity retains complete 2048-frame callbacks")) return false;
    for (const auto c : {Config{true, 0, 1}, Config{true, kFrameCapacity + 1, 1},
                         Config{true, 1, 0}, Config{true, 1, kRecordCapacity + 1}}) {
        Recorder invalid;
        if (!check(!invalid.configure(c) && !invalid.enabled() && !invalid.configure({true}),
                   "invalid limits consume configuration and remain disabled")) return false;
    }
    return true;
}

bool identityChanges() {
    Recorder recorder;
    recorder.configure({true});
    float samples[128]{};
    auto first = recorder.begin(metadata(), samples, true);
    recorder.complete(first, {}, samples, true);
    auto changed = metadata(5);
    changed.ownerAddress = 0;
    changed.inputChannels = 1;
    changed.outputChannels = 1;
    changed.streamGeneration = 10;
    changed.sampleRate = 48000.0;
    changed.inputDevice = 5;
    changed.thread = 88;
    changed.driverChannelsValidated = true;
    changed.driverInputSelectors = {{1, -1}};
    changed.driverOutputSelectors = {{0, 1}};
    changed.rateRevision = 2;
    changed.actualRateValidated = true;
    changed.actualSampleRate = 192000;
    auto second = recorder.begin(changed, samples, true);
    recorder.complete(second, {}, samples, true);
    Snapshot view;
    const auto expected = SequenceDiscontinuity | OwnerChanged | ChannelsChanged | GenerationChanged |
                          SampleRateChanged | DeviceChanged | ThreadChanged | MissingOwner |
                          DriverChannelsChanged | ActualRateChanged;
    return check(recorder.snapshot(1, view) && view.record.changes == expected &&
                 view.captureSamples == 64 && view.postOriginalSamples == 64 &&
                 view.record.metadata.sampleRate == 48000.0,
                 "sequence owner channels generation rate device and thread changes are explicit");
}

bool concurrentPublication() {
    Recorder recorder;
    recorder.configure({true});
    float input[128]{};
    auto held = recorder.begin(metadata(), input, true);
    std::atomic<bool> valid{true};
    std::thread contender([&] {
        forbidAllocations = true;
        for (std::size_t i = 0; i < 10000; ++i)
            if (recorder.begin(metadata(2), reinterpret_cast<void *>(1), true))
                valid.store(false, std::memory_order_relaxed);
        forbidAllocations = false;
    });
    contender.join();
    Snapshot before;
    if (!check(valid.load() && recorder.busyDrops() == 10000 && recorder.claimed() == 1 &&
               !recorder.snapshot(0, before), "concurrent/reentrant begin skips immediately without buffer read"))
        return false;
    recorder.complete(held, {}, input, true);
    std::atomic<bool> done{false};
    std::thread writer([&] {
        float capture[128];
        float output[128];
        forbidAllocations = true;
        for (std::uint64_t seq = 2; seq <= 1000; ++seq) {
            for (std::size_t i = 0; i < 128; ++i) {
                capture[i] = static_cast<float>(seq * 128 + i);
                output[i] = -capture[i];
            }
            auto ticket = recorder.begin(metadata(seq), capture, true);
            if (!ticket || !recorder.complete(ticket, {}, output, true)) valid.store(false);
        }
        forbidAllocations = false;
        done.store(true, std::memory_order_release);
    });
    std::size_t read = 1;
    while (!done.load(std::memory_order_acquire) || read < recorder.claimed()) {
        Snapshot view;
        if (!recorder.snapshot(read, view)) {
            std::this_thread::yield();
            continue;
        }
        if (view.record.metadata.sequence != read + 1 || view.record.changes != NoChange ||
            view.captureSamples != 128 || view.postOriginalSamples != 128) valid.store(false);
        for (std::size_t i = 0; i < 128; ++i)
            if (view.capture[i] != static_cast<float>((read + 1) * 128 + i) ||
                view.postOriginal[i] != -view.capture[i]) valid.store(false);
        ++read;
    }
    writer.join();
    return check(valid.load() && read == 1000 && recorder.published() == 1000,
                 "concurrent reader sees complete immutable PCM after release/acquire publication");
}
} // namespace

int main() {
    if (!lifecycleAndAlias() || !guardAndValidation() || !capacitiesAndTail() ||
        !identityChanges() || !concurrentPublication() ||
        !check(callbackAllocations.load() == 0, "no allocation in any callback operation")) return 1;
    std::cout << "PASS: complete PCM publication, alias/guard/capacity/identity/concurrency/no-allocation checks.\n";
    return 0;
}
