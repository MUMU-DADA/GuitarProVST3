#include "input_monitor_exchange.h"
#include <array>
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>

using gpvst3::input::MonitorExchange;
void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

int main() {
    try {
        MonitorExchange exchange;
        require(exchange.publish(0), "initial publish");
        {
            MonitorExchange::Lease old;
            exchange.acquire(old);
            require(old.index() == 0 && old.suppressNative(), "admit prepared slot");
            exchange.mute();
            require(!exchange.writable(0) && !exchange.publish(0), "retired live reader protects storage");
            require(exchange.publish(1), "independent prepared slot");
            require(old.index() == 0, "one callback retains its configuration");
            exchange.off();
            MonitorExchange::Lease off;
            exchange.acquire(off);
            require(!off.suppressNative() && !off.watchStream() && off.index() < 0,
                    "off restores original callback and stops stream observation");
        }
        require(exchange.writable(0) && exchange.writable(1), "readers drained");
        exchange.legacy();
        exchange.observe();
        {
            MonitorExchange::Lease watching;
            exchange.acquire(watching);
            require(watching.watchStream() && !watching.suppressNative() && watching.legacy() &&
                    watching.index() < 0, "unsupported stream observation preserves original legacy route");
        }
        exchange.mute();
        {
            MonitorExchange::Lease muted;
            exchange.acquire(muted);
            require(muted.suppressNative() && muted.watchStream() && muted.index() < 0,
                    "fault suppresses native without processor and still observes stream changes");
        }
        struct Payload { std::array<std::uint64_t, 64> values{}; } payloads[2];
        std::atomic<bool> stop{false}, failed{false};
        std::atomic<std::uint64_t> reads{0};
        require(exchange.publish(0), "publish stress initial data");
        std::thread reader([&] {
            while (!stop.load()) {
                MonitorExchange::Lease lease;
                exchange.acquire(lease);
                if (lease.index() < 0) continue;
                const auto &data = payloads[lease.index()].values;
                const auto expected = data[0];
                for (const auto value : data) if (value != expected) failed.store(true);
                reads.fetch_add(1);
            }
        });
        while (reads.load() == 0) std::this_thread::yield();
        for (std::uint64_t n = 1; n <= 50000; ++n) {
            const int target = exchange.activeIndex() == 0 ? 1 : 0;
            while (!exchange.writable(target)) std::this_thread::yield();
            payloads[target].values.fill(n);
            // A delayed reader can transiently increment the retired slot
            // before rejecting its old token. Keep the prepared data intact
            // and retry publication; this is a safe control-thread failure.
            while (!exchange.publish(target)) std::this_thread::yield();
            if (n % 17 == 0) exchange.mute();
            if (n % 31 == 0) exchange.off();
        }
        stop.store(true);
        reader.join();
        std::cout << "reads=" << reads.load() << ", inconsistent=" << failed.load() << '\n';
        require(!failed.load() && reads.load() > 0, "publication stress preserves slot lifetime and content");
        std::cout << "PASS: input exchange retirement, failure mute, off and 50000 concurrent publications\n";
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
