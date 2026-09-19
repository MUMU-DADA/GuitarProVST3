#include <atomic>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <new>
#include <thread>
#include <vector>

#include "input_timing_probe.h"

namespace {
thread_local bool forbidAllocations = false;
std::atomic<unsigned> allocations{0};
}
void *operator new(std::size_t size) {
    if (forbidAllocations) allocations.fetch_add(1);
    if (auto *value = std::malloc(size ? size : 1)) return value;
    throw std::bad_alloc();
}
void *operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void *value) noexcept { std::free(value); }
void operator delete[](void *value) noexcept { std::free(value); }
void operator delete(void *value, std::size_t) noexcept { std::free(value); }
void operator delete[](void *value, std::size_t) noexcept { std::free(value); }

namespace {
using namespace gpvst3::input::timingprobe;
const Identity identity{2, 3, 192000, true};
bool check(bool value, const char *message) {
    if (!value) std::cerr << "FAIL: " << message << '\n';
    return value;
}
void record(Recorder &recorder, std::uint64_t start, std::uint64_t duration,
            std::uint64_t frames = 64, Identity actual = identity) {
    Recorder::Ticket ticket;
    if (!recorder.begin(ticket, start, frames, 0, actual)) std::abort();
    recorder.finish(ticket, start + duration, 0, actual);
}
bool boundariesAndPublication() {
    auto recorder = std::make_unique<Recorder>();
    Recorder::Ticket ticket, overlap;
    if (!check(!recorder->begin(ticket, 100, 64, 0, identity), "default disabled")) return false;
    if (!check(recorder->configure(true, 100) && !recorder->configure(false, 0), "one-time configuration")) return false;
    if (!check(!recorder->begin(ticket, 99, 64, 0, identity), "configured start is inclusive")) return false;
    if (!check(recorder->begin(ticket, 100, 64, 5, identity), "starts at boundary")) return false;
    if (!check(!recorder->begin(ticket, 101, 64, 0, identity) &&
               !recorder->begin(overlap, 101, 64, 0, identity), "active ticket and overlap never wait")) return false;
    auto active = recorder->snapshot(105);
    if (!check(!active.coherent && active.admitted == 1 && active.completed == 0 &&
               active.overlappingCallbacks == 1, "in-flight snapshot is explicit")) return false;
    auto wrongRecorder = std::make_unique<Recorder>();
    wrongRecorder->finish(ticket, 110, 0, identity);
    if (!check(ticket.active, "wrong recorder cannot publish ticket")) return false;
    Recorder::inputProcessed(ticket, 110, 1110, true);
    recorder->finish(ticket, 2100, 1, identity);
    recorder->finish(ticket, 2200, 0, identity);
    auto value = recorder->snapshot(2200);
    if (!check(value.coherent && value.completed == 1 && value.callback.count == 1 &&
               value.inputProcess.count == 1 && value.callback.maximum == 2000 &&
               value.inputProcess.maximum == 1000 && value.inputCalls == 1 &&
               value.statusCallbacks == 1 && value.statusFlags == 5 && value.resultErrors == 1,
               "completion records once with separately timed input")) return false;
    if (!check(value.callback.percentileUpper(95) == 3000 && value.inputProcess.percentileUpper(50) == 2000,
               "histogram percentiles are exclusive upper bounds")) return false;
    if (!check(recorder->begin(ticket, 2300, 64, 0, identity), "callback admitted before stopping")) return false;
    if (!check(!recorder->stop(), "stop detects in-flight writer before snapshot")) return false;
    if (!check(!recorder->begin(overlap, 2400, 64, 0, identity), "stopped recorder rejects new callbacks")) return false;
    recorder->finish(ticket, 2500, 0, identity);
    if (!check(recorder->stop(), "stopped writer can be quiesced")) return false;
    value = recorder->snapshot(2600);
    if (!check(value.stopped && value.coherent && value.admitted == 2 && value.completed == 2,
               "stopping preserves completion of already admitted callback")) return false;
    if (!check(!recorder->begin(ticket, 100 + kWindowNanoseconds, 64, 0, identity) &&
               recorder->snapshot(100 + kWindowNanoseconds).timeBoundReached,
               "120-second window excludes callbacks at and after end")) return false;
    return true;
}
bool distributionOverflowAndBudgets() {
    auto recorder = std::make_unique<Recorder>();
    recorder->configure(true, 1);
    for (unsigned index = 0; index < 100; ++index) {
        const auto duration = index < 50 ? 999 : index < 95 ? 1234 : index < 99 ? 333334 : 5000000;
        record(*recorder, 100 + index * 10000000ULL, duration);
    }
    auto value = recorder->snapshot(1000000000);
    if (!check(value.callback.count == 100 && value.callback.maximum == 5000000 && value.callback.overflow == 1 &&
               value.callback.percentileUpper(50) == 1000 && value.callback.percentileUpper(95) == 2000 &&
               value.callback.percentileUpper(100) == 0 && value.callbackBudgetExceeded == 5 &&
               value.validatedBudgets == 100 && value.minimumRate == 192000 && value.minimumFrames == 64,
               "known distribution, overflow, quantiles and actual-rate budget")) return false;
    if (!check(value.inputProcess.count == 0 && value.inputProcess.percentileUpper(50) == 0,
               "native-only callbacks do not manufacture input timings")) return false;
    Recorder::Ticket ticket;
    recorder->begin(ticket, 2000000000, 96, 0, {3, 4, 96000, true});
    Recorder::inputProcessed(ticket, 2000000001, 2001000002, false);
    recorder->finish(ticket, 2001000003, 0, {3, 4, 96000, true});
    value = recorder->snapshot(2001000003);
    if (!check(value.identityChanges == 1 && value.minimumRate == 96000 && value.maximumRate == 192000 &&
               value.minimumFrames == 64 && value.maximumFrames == 96 && value.inputFailures == 1 &&
               value.inputBudgetExceeded == 1 && value.callbackBudgetExceeded == 6,
               "stream/rate change recomputes budget for the same callback")) return false;
    record(*recorder, 3000000000, 21333334, 4096);
    record(*recorder, 3100000000, 42666666, 8192);
    record(*recorder, 3200000000, 42666667, 8192);
    record(*recorder, 3300000000, 50000000, 8193);
    value = recorder->snapshot(3400000000);
    if (!check(value.validatedBudgets == 104 && value.unvalidatedBudgets == 1 &&
               value.maximumFrames == 8192 && value.callbackBudgetExceeded == 8,
               "full driver budgets cover 4096/8192 frames with exact threshold and reject oversize")) return false;
    return true;
}
bool invalidRateAndClock() {
    auto recorder = std::make_unique<Recorder>();
    recorder->configure(true, 1);
    record(*recorder, 1000, 900000, 64, {0, 0, 44100, false});
    record(*recorder, 1000000, 900000, 0);
    record(*recorder, 2000000, 900000, 64, {2, 3, 0, true});
    Recorder::Ticket ticket;
    recorder->begin(ticket, 3000000, 64, 0, identity);
    recorder->finish(ticket, 3900000, 0, {2, 4, 192000, true});
    recorder->begin(ticket, 4000000, 64, 0, identity);
    recorder->finish(ticket, 3999999, 0, identity);
    recorder->begin(ticket, 5000000, 64, 0, identity);
    Recorder::inputProcessed(ticket, 4999999, 5000001, true);
    recorder->finish(ticket, 5900000, 0, identity);
    recorder->begin(ticket, 6000000, 64, 0, identity);
    Recorder::inputProcessed(ticket, 6000000, 6009000, true);
    recorder->finish(ticket, 6001000, 0, identity);
    const auto value = recorder->snapshot(7000000);
    return check(value.completed == 7 && value.invalidClocks == 3 && value.callback.count == 4 &&
                 value.unvalidatedBudgets == 4 && value.validatedBudgets == 0 && value.callbackBudgetExceeded == 0,
                 "invalid clocks/rates and identity changes never become deadline evidence");
}
bool callbackCapAndNoAllocation() {
    auto recorder = std::make_unique<Recorder>();
    recorder->configure(true, 1);
    forbidAllocations = true;
    for (std::uint64_t index = 0; index < kCallbackLimit; ++index) record(*recorder, index * 1000 + 1, 100);
    Recorder::Ticket ticket;
    const bool acceptedBeyondLimit = recorder->begin(ticket, 1000000001, 64, 0, identity);
    forbidAllocations = false;
    const auto value = recorder->snapshot(1000000001);
    return check(!acceptedBeyondLimit && value.completed == kCallbackLimit && value.callbackBoundReached &&
                 !value.timeBoundReached && allocations.load() == 0,
                 "million-callback bound and allocation-free recording");
}
bool concurrentSnapshotAndCallbacks() {
    auto recorder = std::make_unique<Recorder>();
    recorder->configure(true, 1);
    std::atomic<bool> start{false}, done{false}, valid{true};
    std::atomic<std::uint64_t> accepted{0}, attempted{0};
    std::thread reader([&] {
        while (!done.load()) {
            const auto value = recorder->snapshot(1000000);
            if (!value.coherent) continue;
            std::uint64_t sum = value.callback.overflow;
            for (auto count : value.callback.buckets) sum += count;
            if (sum != value.callback.count || value.callback.count != value.completed ||
                value.completed != value.admitted || value.callback.maximum > 999) valid.store(false);
        }
    });
    std::vector<std::thread> writers;
    for (unsigned thread = 0; thread < 4; ++thread) writers.emplace_back([&] {
        while (!start.load()) std::this_thread::yield();
        forbidAllocations = true;
        for (unsigned index = 0; index < 20000; ++index) {
            Recorder::Ticket ticket;
            const auto sequence = attempted.fetch_add(1) + 1;
            if (recorder->begin(ticket, sequence * 1000, 64, 0, identity)) {
                recorder->finish(ticket, sequence * 1000 + 999, 0, identity);
                accepted.fetch_add(1);
            }
        }
        forbidAllocations = false;
    });
    start.store(true);
    for (auto &writer : writers) writer.join();
    done.store(true);
    reader.join();
    const auto value = recorder->snapshot(100000000);
    return check(valid.load() && value.coherent && value.completed == accepted.load() &&
                 value.completed + value.overlappingCallbacks == attempted.load() && allocations.load() == 0,
                 "concurrent snapshots are marked coherent only for complete aggregates; overlap is counted");
}
}

int main() {
    if (!(boundariesAndPublication() && distributionOverflowAndBudgets() && invalidRateAndClock() &&
          callbackCapAndNoAllocation() && concurrentSnapshotAndCallbacks())) return 1;
    std::cout << "PASS: bounded timing, quantiles, actual-rate budgets, concurrency and allocation guards\n";
    return 0;
}
