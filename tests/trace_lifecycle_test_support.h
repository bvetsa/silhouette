#pragma once

#include "active_trace_manager_test_peer.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <mutex>

namespace silhouette::testing {

// Fake time controls eligibility; WakeLifecycle explicitly interrupts the
// worker's physical wait. No fake scheduler or wall-clock sleeps are needed.
class FakeTraceClock final {
public:
    using Clock = ActiveTraceManager::Clock;

    static Clock::time_point Now(void* context) noexcept
    {
        auto& clock = *static_cast<FakeTraceClock*>(context);
        clock.samples_.fetch_add(1);
        return Clock::time_point{Clock::duration{clock.ticks_.load()}};
    }

    void Advance(const Clock::duration amount) noexcept
    {
        ticks_.fetch_add(amount.count());
    }

    std::size_t samples() const noexcept { return samples_.load(); }

private:
    std::atomic<Clock::duration::rep> ticks_{0};
    std::atomic<std::size_t> samples_{0};
};

class LifecycleSweepSync final {
public:
    static void Signal(void* context) noexcept
    {
        auto& sync = *static_cast<LifecycleSweepSync*>(context);
        {
            std::scoped_lock lock{sync.mutex_};
            ++sync.completed_;
        }
        sync.cv_.notify_all();
    }

    void WaitFor(const std::size_t count)
    {
        std::unique_lock lock{mutex_};
        cv_.wait(lock, [&] { return completed_ >= count; });
    }

    std::size_t completed() const
    {
        std::scoped_lock lock{mutex_};
        return completed_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::size_t completed_{0};
};

// Capture the next acknowledgment and advance time under the manager mutex,
// preventing an unrelated sweep from satisfying the test's intended wake.
inline std::size_t AdvanceAndWakeLifecycle(
    ActiveTraceManager& manager, FakeTraceClock& clock,
    LifecycleSweepSync& sweeps, const FakeTraceClock::Clock::duration amount)
{
    struct Advance final {
        FakeTraceClock& clock;
        LifecycleSweepSync& sweeps;
        FakeTraceClock::Clock::duration amount;
        std::size_t acknowledgment{0};
    };
    Advance advance{clock, sweeps, amount};
    ActiveTraceManagerTestPeer::WakeLifecycle(
        manager, [](void* context) noexcept {
            auto& advance = *static_cast<Advance*>(context);
            advance.clock.Advance(advance.amount);
            advance.acknowledgment = advance.sweeps.completed() + 1;
        }, &advance);
    return advance.acknowledgment;
}

} // namespace silhouette::testing
