#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <limits>

namespace gpvst3::input {

// One control writer; any callback reader. Contexts live in two fixed owner
// slots. Publication never frees memory. The writer may mutate only a retired
// slot with readers()==0. Monotonic tokens prevent index reuse from causing ABA.
class MonitorExchange final {
public:
    class Lease final {
    public:
        Lease() = default;
        Lease(const Lease &) = delete;
        Lease &operator=(const Lease &) = delete;
        ~Lease() { if (owner_) owner_->readers_[index_].fetch_sub(1); }
        bool suppressNative() const noexcept { return token_ != 0 && (token_ & 4) != 0; }
        bool watchStream() const noexcept { return (token_ & 16) != 0; }
        bool legacy() const noexcept { return (token_ & 8) != 0; }
        bool slotExpected() const noexcept { return (token_ & 2) != 0; }
        int index() const noexcept { return owner_ ? index_ : -1; }
        std::uint64_t token() const noexcept { return token_; }
    private:
        friend class MonitorExchange;
        MonitorExchange *owner_ = nullptr;
        std::uint64_t token_ = 0;
        int index_ = -1;
    };

    // Bounded admission: when publication races both attempts, keep the last
    // mode snapshot but fail muted rather than touching unowned slot storage.
    void acquire(Lease &lease) noexcept {
        if (lease.owner_) return;
        for (int attempt = 0; attempt < 2; ++attempt) {
            const auto token = published_.load();
            lease.token_ = token;
            if ((token & 2) == 0) return;
            const auto index = int(token & 1);
            readers_[index].fetch_add(1);
            if (published_.load() == token) {
                lease.owner_ = this;
                lease.index_ = index;
                return;
            }
            readers_[index].fetch_sub(1);
        }
    }

    int activeIndex() const noexcept {
        const auto token = published_.load();
        return (token & 2) ? int(token & 1) : -1;
    }
    bool suppressed() const noexcept { return (published_.load() & 4) != 0; }
    bool watching() const noexcept { return (published_.load() & 16) != 0; }
    bool current(std::uint64_t token) const noexcept { return published_.load() == token; }
    bool legacyFallback() const noexcept { return (published_.load() & 8) != 0; }
    static std::uint64_t versionOf(std::uint64_t token) noexcept { return token & ~std::uint64_t{31}; }
    std::uint64_t version() const noexcept { return versionOf(published_.load()); }
    bool writable(int index) const noexcept {
        return index >= 0 && index < 2 && activeIndex() != index && readers_[index].load() == 0;
    }
    // Caller has fully prepared the slot. A false result leaves publication
    // unchanged; it never makes an unprepared slot visible to the callback.
    bool publish(int index) noexcept {
        if (!writable(index) || revision_ == (std::numeric_limits<std::uint64_t>::max)() / 32)
            return false;
        published_.store((++revision_ * 32) | 22 | std::uint64_t(index) | (published_.load() & 8));
        return true;
    }
    // Mutation is allowed only after retiring and observing readers drain.
    void mute() noexcept { publishMode(20 | (published_.load() & 8)); }
    // A rejected stream must remain observable without changing native audio.
    // A later supported generation can then prepare a fresh input slot.
    void observe() noexcept { publishMode(16 | (published_.load() & 8)); }
    void off() noexcept { publishMode(0); }
    void legacy() noexcept { publishMode(8); }

private:
    void publishMode(std::uint64_t mode) noexcept {
        if (revision_ != (std::numeric_limits<std::uint64_t>::max)() / 32) ++revision_;
        published_.store(revision_ * 32 | mode);
    }
    std::atomic<std::uint64_t> published_{8}; // Existing users retain legacy routing until a request.
    std::array<std::atomic<std::uint64_t>, 2> readers_{};
    std::uint64_t revision_ = 0;
};

} // namespace gpvst3::input
