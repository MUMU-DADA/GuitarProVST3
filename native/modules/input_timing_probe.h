#pragma once

#ifndef GPVST3_P13_PROBE_BUILD
#error The callback timing recorder is excluded from release builds.
#endif

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace gpvst3::input::timingprobe {

constexpr std::uint64_t kWindowNanoseconds = 120000000000ULL;
constexpr std::uint64_t kCallbackLimit = 1000000;
constexpr std::size_t kHistogramBuckets = 4096;
constexpr std::uint64_t kBucketNanoseconds = 1000;
constexpr std::size_t kPairedCallbackCapacity = 256;

struct Identity {
    std::uint64_t generation = 0, revision = 0;
    double rate = 0;
    bool validated = false;
};

struct HistogramSnapshot {
    std::array<std::uint64_t, kHistogramBuckets> buckets{};
    std::uint64_t count = 0, overflow = 0, maximum = 0;

    // Control thread only. Zero denotes an unavailable percentile (empty or
    // in the overflow bucket); finite answers are exclusive upper bounds.
    std::uint64_t percentileUpper(unsigned percent) const noexcept {
        if (!count || !percent || percent > 100) return 0;
        const auto rank = (count * percent + 99) / 100;
        std::uint64_t cumulative = 0;
        for (std::size_t i = 0; i < buckets.size(); ++i) {
            cumulative += buckets[i];
            if (cumulative >= rank) return (i + 1) * kBucketNanoseconds;
        }
        return 0;
    }
};

// Immutable once release-published. Retain the same callback's stages,
// rather than subtracting independently aggregated percentiles.
struct PairedCallback {
    std::uint64_t sequence = 0, started = 0, ended = 0, frames = 0, status = 0;
    Identity identity;
    std::uint64_t nativeStarted = 0, nativeEnded = 0;
    std::uint64_t inputStarted = 0, inputEnded = 0, inputCalls = 0;
    std::uint64_t callbackCycles = 0, nativeCycles = 0, inputCycles = 0;
    std::uint64_t monitorToken = 0;
    std::uint32_t thread = 0, firstCpu = 0, lastCpu = 0;
    bool nativeEnabled = false, dry = false, cyclesValid = false;
};

struct Snapshot {
    HistogramSnapshot callback, inputProcess;
    std::uint64_t windowStart = 0, windowEnd = 0, firstStarted = 0, lastEnded = 0;
    std::uint64_t admitted = 0, completed = 0, overlappingCallbacks = 0;
    std::uint64_t statusCallbacks = 0, statusFlags = 0, resultErrors = 0;
    std::uint64_t invalidClocks = 0, unvalidatedBudgets = 0, validatedBudgets = 0;
    std::uint64_t callbackBudgetExceeded = 0, inputBudgetExceeded = 0;
    std::uint64_t inputCalls = 0, inputFailures = 0, identityChanges = 0;
    std::uint64_t minimumFrames = 0, maximumFrames = 0;
    double minimumRate = 0, maximumRate = 0;
    std::array<PairedCallback, kPairedCallbackCapacity> paired{};
    std::size_t pairedCount = 0;
    std::array<PairedCallback, kPairedCallbackCapacity> consecutive{};
    std::size_t consecutiveCount = 0;
    bool enabled = false, stopped = false, timeBoundReached = false, callbackBoundReached = false, coherent = false;
};

class Recorder final {
public:
    struct Ticket {
        Ticket() = default;
        Ticket(const Ticket &) = delete;
        Ticket &operator=(const Ticket &) = delete;
        std::uint64_t started = 0, frames = 0, status = 0;
        std::uint64_t inputNanoseconds = 0, inputCalls = 0, inputFailures = 0;
        Identity identity;
        PairedCallback pair;
        bool active = false, invalidClock = false;
        const Recorder *owner = nullptr;
    };

    // Configure once on the control thread, before any callback can enter.
    bool configure(bool enabled, std::uint64_t start) noexcept {
        if (configured_.test_and_set() || start > (std::numeric_limits<std::uint64_t>::max)() - kWindowNanoseconds)
            return false;
        start_ = start;
        end_ = start + kWindowNanoseconds;
        enabled_.store(enabled, std::memory_order_release);
        return true;
    }
    bool enabled() const noexcept { return enabled_.load(std::memory_order_acquire); }
    // Control-thread retry: claiming the writer after sealing closes the gap
    // between begin() checking stopped_ and publishing its odd version.
    bool stop() noexcept {
        stopped_.store(true);
        if (writer_.test_and_set(std::memory_order_acquire)) return false;
        writer_.clear(std::memory_order_release);
        return true;
    }

    // One nonblocking claim. An overlapping/reentrant callback is counted and
    // skipped, never waited on. Atomics make concurrent snapshots race-free.
    bool begin(Ticket &ticket, std::uint64_t started, std::uint64_t frames,
               std::uint64_t status, Identity identity) noexcept {
        if (ticket.active || !enabled() || stopped_.load() || started < start_ || started >= end_ ||
            admitted_.load(std::memory_order_relaxed) >= kCallbackLimit) return false;
        if (writer_.test_and_set(std::memory_order_acquire)) {
            if (!stopped_.load()) overlaps_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (stopped_.load() || admitted_.load(std::memory_order_relaxed) >= kCallbackLimit) {
            writer_.clear(std::memory_order_release);
            return false;
        }
        version_.fetch_add(1); // Odd until every field of the callback publishes.
        ticket.inputNanoseconds = ticket.inputCalls = ticket.inputFailures = 0;
        ticket.invalidClock = false;
        ticket.started = started;
        ticket.frames = frames;
        ticket.status = status;
        ticket.identity = identity;
        ticket.pair = {};
        ticket.active = true;
        ticket.owner = this;
        ticket.pair.sequence = admitted_.fetch_add(1, std::memory_order_relaxed);
        if (ticket.pair.sequence == 0) firstStarted_.store(started);
        return true;
    }

    static void nativeProcessed(Ticket &ticket, std::uint64_t start, std::uint64_t end) noexcept {
        if (!ticket.active) return;
        if (end < start || start < ticket.started || ticket.pair.nativeStarted) ticket.invalidClock = true;
        ticket.pair.nativeStarted = start;
        ticket.pair.nativeEnded = end;
    }

    static void inputProcessed(Ticket &ticket, std::uint64_t start, std::uint64_t end, bool success) noexcept {
        if (!ticket.active) return;
        ++ticket.inputCalls;
        if (!success) ++ticket.inputFailures;
        if (end < start || start < ticket.started ||
            end - start > (std::numeric_limits<std::uint64_t>::max)() - ticket.inputNanoseconds) {
            ticket.invalidClock = true;
            return;
        }
        ticket.inputNanoseconds += end - start;
        if (ticket.inputCalls == 1) ticket.pair.inputStarted = start;
        ticket.pair.inputEnded = end;
    }

    void finish(Ticket &ticket, std::uint64_t ended, int result, Identity identity) noexcept {
        if (!ticket.active || ticket.owner != this) return;
        ticket.active = false;
        if (ticket.status) statusCallbacks_.fetch_add(1);
        statusFlags_.store(statusFlags_.load() | ticket.status);
        if (result != 0) resultErrors_.fetch_add(1);
        inputCalls_.fetch_add(ticket.inputCalls);
        inputFailures_.fetch_add(ticket.inputFailures);
        const bool clockValid = !ticket.invalidClock && ended >= ticket.started &&
            ticket.inputNanoseconds <= ended - ticket.started &&
            ticket.pair.nativeEnded <= ended && ticket.pair.inputEnded <= ended;
        if (!clockValid) invalidClocks_.fetch_add(1);
        else {
            const auto elapsed = ended - ticket.started;
            callback_.add(elapsed);
            if (ticket.inputCalls) input_.add(ticket.inputNanoseconds);
            const bool valid = identity.validated && ticket.identity.validated &&
                identity.generation && identity.revision && identity.generation == ticket.identity.generation &&
                identity.revision == ticket.identity.revision && identity.rate == ticket.identity.rate &&
                std::isfinite(identity.rate) && identity.rate >= 8000 && identity.rate <= 768000 &&
                ticket.frames > 0 && ticket.frames <= 8192;
            if (!valid) unvalidatedBudgets_.fetch_add(1);
            else {
                if (validatedBudgets_.fetch_add(1) == 0) {
                    minimumRate_.store(identity.rate);
                    minimumFrames_.store(ticket.frames);
                } else if (lastIdentity_.generation != identity.generation ||
                           lastIdentity_.revision != identity.revision || lastIdentity_.rate != identity.rate)
                    identityChanges_.fetch_add(1);
                lastIdentity_ = identity;
                auto &pair = ticket.pair;
                pair.started = ticket.started; pair.ended = ended;
                pair.frames = ticket.frames; pair.status = ticket.status;
                pair.identity = identity; pair.inputCalls = ticket.inputCalls;
                const auto consecutiveIndex = consecutiveCount_.load(std::memory_order_relaxed);
                if (ticket.inputCalls && consecutiveIndex < kPairedCallbackCapacity) {
                    consecutive_[consecutiveIndex] = pair;
                    consecutiveCount_.store(consecutiveIndex + 1, std::memory_order_release);
                }
                if (identity.rate < minimumRate_.load()) minimumRate_.store(identity.rate);
                if (identity.rate > maximumRate_.load()) maximumRate_.store(identity.rate);
                if (ticket.frames < minimumFrames_.load()) minimumFrames_.store(ticket.frames);
                if (ticket.frames > maximumFrames_.load()) maximumFrames_.store(ticket.frames);
                const double budgetProduct = static_cast<double>(ticket.frames) * 1000000000.0;
                if (static_cast<double>(elapsed) * identity.rate > budgetProduct) {
                    callbackBudgetExceeded_.fetch_add(1);
                    const auto index = pairedCount_.load(std::memory_order_relaxed);
                    if (index < kPairedCallbackCapacity) {
                        paired_[index] = pair;
                        pairedCount_.store(index + 1, std::memory_order_release);
                    }
                }
                if (ticket.inputCalls && static_cast<double>(ticket.inputNanoseconds) * identity.rate > budgetProduct)
                    inputBudgetExceeded_.fetch_add(1);
            }
        }
        lastEnded_.store(ended);
        completed_.fetch_add(1);
        version_.fetch_add(1);
        writer_.clear(std::memory_order_release);
    }

    Snapshot snapshot(std::uint64_t now) const noexcept {
        Snapshot value;
        // A live snapshot may straddle a callback; bounded retries occur only
        // on the reader. The callback never waits for readers or copies data.
        for (unsigned attempt = 0; attempt < 4; ++attempt) {
            const auto version = version_.load();
            value.enabled = enabled();
            value.stopped = stopped_.load();
            value.windowStart = start_; value.windowEnd = end_;
            value.firstStarted = firstStarted_.load(); value.lastEnded = lastEnded_.load();
            value.admitted = admitted_.load(); value.completed = completed_.load();
            value.overlappingCallbacks = overlaps_.load();
            value.statusCallbacks = statusCallbacks_.load(); value.statusFlags = statusFlags_.load();
            value.resultErrors = resultErrors_.load(); value.invalidClocks = invalidClocks_.load();
            value.unvalidatedBudgets = unvalidatedBudgets_.load(); value.validatedBudgets = validatedBudgets_.load();
            value.callbackBudgetExceeded = callbackBudgetExceeded_.load(); value.inputBudgetExceeded = inputBudgetExceeded_.load();
            value.inputCalls = inputCalls_.load(); value.inputFailures = inputFailures_.load();
            value.identityChanges = identityChanges_.load();
            value.minimumRate = minimumRate_.load(); value.maximumRate = maximumRate_.load();
            value.minimumFrames = minimumFrames_.load(); value.maximumFrames = maximumFrames_.load();
            value.callback = callback_.snapshot(); value.inputProcess = input_.snapshot();
            value.pairedCount = pairedCount_.load(std::memory_order_acquire);
            for (std::size_t i = 0; i < value.pairedCount; ++i) value.paired[i] = paired_[i];
            value.consecutiveCount = consecutiveCount_.load(std::memory_order_acquire);
            for (std::size_t i = 0; i < value.consecutiveCount; ++i) value.consecutive[i] = consecutive_[i];
            value.timeBoundReached = value.enabled && now >= end_;
            value.callbackBoundReached = value.admitted == kCallbackLimit;
            value.coherent = (version & 1) == 0 && version == version_.load();
            if (value.coherent) break;
        }
        return value;
    }

private:
    struct Histogram {
        std::array<std::atomic<std::uint64_t>, kHistogramBuckets> buckets{};
        std::atomic<std::uint64_t> count{0}, overflow{0}, maximum{0};
        void add(std::uint64_t duration) noexcept {
            const auto index = duration / kBucketNanoseconds;
            if (index < kHistogramBuckets) buckets[static_cast<std::size_t>(index)].fetch_add(1);
            else overflow.fetch_add(1);
            if (duration > maximum.load()) maximum.store(duration);
            count.fetch_add(1);
        }
        HistogramSnapshot snapshot() const noexcept {
            HistogramSnapshot result;
            for (std::size_t i = 0; i < kHistogramBuckets; ++i) result.buckets[i] = buckets[i].load();
            result.count = count.load(); result.overflow = overflow.load(); result.maximum = maximum.load();
            return result;
        }
    };
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free && std::atomic<double>::is_always_lock_free,
                  "Timing diagnostics require lock-free atomic storage");
    std::atomic_flag configured_ = ATOMIC_FLAG_INIT, writer_ = ATOMIC_FLAG_INIT;
    std::atomic<bool> enabled_{false}, stopped_{false};
    std::uint64_t start_ = 0, end_ = 0;
    Identity lastIdentity_;
    Histogram callback_, input_;
    std::array<PairedCallback, kPairedCallbackCapacity> paired_{};
    std::atomic<std::size_t> pairedCount_{0};
    std::array<PairedCallback, kPairedCallbackCapacity> consecutive_{};
    std::atomic<std::size_t> consecutiveCount_{0};
    std::atomic<std::uint64_t> version_{0}, admitted_{0}, completed_{0}, overlaps_{0};
    std::atomic<std::uint64_t> firstStarted_{0}, lastEnded_{0}, statusCallbacks_{0}, statusFlags_{0}, resultErrors_{0};
    std::atomic<std::uint64_t> invalidClocks_{0}, unvalidatedBudgets_{0}, validatedBudgets_{0};
    std::atomic<std::uint64_t> callbackBudgetExceeded_{0}, inputBudgetExceeded_{0}, inputCalls_{0}, inputFailures_{0};
    std::atomic<std::uint64_t> identityChanges_{0}, minimumFrames_{0}, maximumFrames_{0};
    std::atomic<double> minimumRate_{0}, maximumRate_{0};
};

} // namespace gpvst3::input::timingprobe
