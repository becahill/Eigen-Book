#pragma once

#include <cstdint>

namespace eigenbook::test::allocation_tracker {

/// Reset the number of C++ allocation calls observed by the tracker.
void reset() noexcept;

/// Enable process-wide counting for the replaceable global new/new[] forms.
void enable() noexcept;

/// Disable allocation counting.
void disable() noexcept;

/// Return the number of allocation calls observed since the last reset.
[[nodiscard]] std::uint64_t count() noexcept;

/// Scope one measured region. Construction resets and enables the tracker;
/// finish() disables it before returning the count.
class ScopedCounter final {
public:
    ScopedCounter() noexcept;
    ~ScopedCounter() noexcept;

    ScopedCounter(const ScopedCounter&) = delete;
    ScopedCounter& operator=(const ScopedCounter&) = delete;
    ScopedCounter(ScopedCounter&&) = delete;
    ScopedCounter& operator=(ScopedCounter&&) = delete;

    [[nodiscard]] std::uint64_t finish() noexcept;

private:
    bool active_{true};
};

} // namespace eigenbook::test::allocation_tracker
