#pragma once

#include "Types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace eigenbook::detail {

struct SnapshotValidationRecord final {
    std::uint64_t key{0};
    std::uint64_t value{0};
    std::uint64_t auxiliary{0};
};

static_assert(sizeof(SnapshotValidationRecord) == 24U);

/// Preallocated scratch storage used only by snapshot validation.
///
/// Three records are reserved per configured order slot: two for stable radix
/// sorting and one for order-derived price-level aggregates. Construction may
/// allocate; validation and restore do not.
class SnapshotValidationWorkspace final {
public:
    explicit SnapshotValidationWorkspace(const std::uint32_t capacity)
        : capacity_(capacity),
          primary_(capacity == 0 ? nullptr : std::make_unique<SnapshotValidationRecord[]>(capacity)),
          scratch_(capacity == 0 ? nullptr : std::make_unique<SnapshotValidationRecord[]>(capacity)),
          aggregates_(capacity == 0 ? nullptr : std::make_unique<SnapshotValidationRecord[]>(capacity))
    {
    }

    SnapshotValidationWorkspace(const SnapshotValidationWorkspace&) = delete;
    SnapshotValidationWorkspace& operator=(const SnapshotValidationWorkspace&) = delete;
    SnapshotValidationWorkspace(SnapshotValidationWorkspace&&) = delete;
    SnapshotValidationWorkspace& operator=(SnapshotValidationWorkspace&&) = delete;

    [[nodiscard]] std::uint32_t capacity() const noexcept
    {
        return capacity_;
    }

    [[nodiscard]] SnapshotValidationRecord* primary() noexcept
    {
        return primary_.get();
    }

    [[nodiscard]] SnapshotValidationRecord* aggregates() noexcept
    {
        return aggregates_.get();
    }

    [[nodiscard]] std::size_t memory_bytes() const noexcept
    {
        return static_cast<std::size_t>(capacity_) * sizeof(SnapshotValidationRecord) * 3U;
    }

    /// Stable least-significant-digit radix sort by `key`.
    ///
    /// Eight fixed byte passes leave the sorted records in `primary()`.
    void sort_primary(const std::uint32_t count) noexcept
    {
        if (count < 2U) {
            return;
        }

        SnapshotValidationRecord* source = primary_.get();
        SnapshotValidationRecord* destination = scratch_.get();
        for (std::uint32_t pass = 0; pass < 8U; ++pass) {
            digit_offsets_.fill(0);
            const std::uint32_t shift = pass * 8U;

            for (std::uint32_t index = 0; index < count; ++index) {
                ++digit_offsets_[digit(source[index].key, shift)];
            }

            std::uint32_t next_offset = 0;
            for (std::uint32_t& offset : digit_offsets_) {
                const std::uint32_t occurrences = offset;
                offset = next_offset;
                next_offset += occurrences;
            }

            for (std::uint32_t index = 0; index < count; ++index) {
                const std::uint32_t bucket = digit(source[index].key, shift);
                destination[digit_offsets_[bucket]++] = source[index];
            }

            std::swap(source, destination);
        }
    }

private:
    std::uint32_t capacity_{0};
    std::unique_ptr<SnapshotValidationRecord[]> primary_;
    std::unique_ptr<SnapshotValidationRecord[]> scratch_;
    std::unique_ptr<SnapshotValidationRecord[]> aggregates_;
    std::array<std::uint32_t, 256> digit_offsets_{};

    [[nodiscard]] static constexpr std::uint32_t digit(const std::uint64_t key,
                                                       const std::uint32_t shift) noexcept
    {
        return static_cast<std::uint32_t>((key >> shift) & 0xffU);
    }
};

} // namespace eigenbook::detail
