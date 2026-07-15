#pragma once

#include "Order.hpp"
#include "Types.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>

namespace eigenbook {

class alignas(64) OrderIdMap final {
public:
    static constexpr std::uint32_t kMaxCapacity = (std::numeric_limits<std::uint32_t>::max() / 2U) + 1U;

    /// Stable only until the next successful map mutation. This token lets a
    /// caller inspect an order before erasing it without repeating the id
    /// lookup. `erase()` validates the token before changing the table.
    class EraseToken final {
    public:
        [[nodiscard]] Order* order() const noexcept
        {
            return order_;
        }

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return order_ != nullptr;
        }

    private:
        friend class OrderIdMap;

        OrderIdMap* owner_{nullptr};
        Order* order_{nullptr};
        OrderId id_{kInvalidOrderId};
        std::uint32_t index_{kInvalidIndex};
        std::uint32_t lookup_probe_count_{0};
        std::uint64_t mutation_revision_{0};
        std::uint64_t stats_revision_{0};
    };

    explicit OrderIdMap(const std::uint32_t requested_capacity)
        : capacity_(capacity_for(requested_capacity)),
          mask_(capacity_ - 1U),
          entries_(std::make_unique<Entry[]>(capacity_))
    {
    }

    OrderIdMap(const OrderIdMap&) = delete;
    OrderIdMap& operator=(const OrderIdMap&) = delete;
    OrderIdMap(OrderIdMap&&) = delete;
    OrderIdMap& operator=(OrderIdMap&&) = delete;

    void clear() noexcept
    {
        for (std::uint32_t i = 0; i < capacity_; ++i) {
            entries_[i] = Entry{};
        }
        size_ = 0;
        last_probe_count_ = 0;
        probe_histogram_.fill(0);
        ++mutation_revision_;
        ++stats_revision_;
    }

    [[nodiscard]] Status can_insert(const OrderId id) const noexcept
    {
        if (id == kInvalidOrderId) {
            return Status::InvalidOrderId;
        }

        std::uint32_t probes = 0;
        if (find_index(id, probes) != kInvalidIndex) {
            return Status::DuplicateOrderId;
        }

        return size_ < capacity_ ? Status::Accepted : Status::OrderIdMapFull;
    }

    [[nodiscard]] Status insert(const OrderId id, Order* const order) noexcept
    {
        if (id == kInvalidOrderId || order == nullptr) {
            record_probe_count(0);
            return Status::InvalidOrderId;
        }

        std::uint32_t duplicate_probes = 0;
        if (find_index(id, duplicate_probes) != kInvalidIndex) {
            record_probe_count(duplicate_probes);
            return Status::DuplicateOrderId;
        }

        if (size_ == capacity_) {
            record_probe_count(duplicate_probes);
            return Status::OrderIdMapFull;
        }

        Entry incoming{};
        incoming.id = id;
        incoming.order = order;
        incoming.state = SlotState::Occupied;
        incoming.probe_distance = 0;

        std::uint32_t slot = hash(id);
        std::uint32_t probes = 0;
        for (std::uint32_t step = 0; step < capacity_; ++step) {
            ++probes;
            Entry& entry = entries_[slot];

            if (entry.state == SlotState::Empty) {
                entry = incoming;
                ++size_;
                ++mutation_revision_;
                record_probe_count(probes);
                return Status::Accepted;
            }

            if (entry.probe_distance < incoming.probe_distance) {
                std::swap(entry, incoming);
            }

            slot = (slot + 1U) & mask_;
            if (incoming.probe_distance != std::numeric_limits<std::uint32_t>::max()) {
                ++incoming.probe_distance;
            }
        }

        record_probe_count(probes);
        return Status::OrderIdMapFull;
    }

    [[nodiscard]] Order* find(const OrderId id) noexcept
    {
        return const_cast<Order*>(static_cast<const OrderIdMap&>(*this).find(id));
    }

    [[nodiscard]] const Order* find(const OrderId id) const noexcept
    {
        if (id == kInvalidOrderId) {
            record_probe_count(0);
            return nullptr;
        }

        std::uint32_t probes = 0;
        const std::uint32_t index = find_index(id, probes);
        record_probe_count(probes);
        return index == kInvalidIndex ? nullptr : entries_[index].order;
    }

    /// Find an order and retain its table slot for a subsequent erase.
    ///
    /// The lookup is recorded immediately. If `erase(token)` follows without
    /// another map-statistics operation, that sample is replaced by the total
    /// lookup-plus-compaction work for the erase.
    [[nodiscard]] EraseToken find_for_erase(const OrderId id) noexcept
    {
        EraseToken token{};
        if (id == kInvalidOrderId) {
            record_probe_count(0);
            return token;
        }

        std::uint32_t probes = 0;
        const std::uint32_t index = find_index(id, probes);
        record_probe_count(probes);
        if (index == kInvalidIndex) {
            return token;
        }

        token.owner_ = this;
        token.order_ = entries_[index].order;
        token.id_ = id;
        token.index_ = index;
        token.lookup_probe_count_ = probes;
        token.mutation_revision_ = mutation_revision_;
        token.stats_revision_ = stats_revision_;
        return token;
    }

    [[nodiscard]] bool erase(const OrderId id) noexcept
    {
        if (id == kInvalidOrderId) {
            record_probe_count(0);
            return false;
        }

        std::uint32_t probes = 0;
        const std::uint32_t index = find_index(id, probes);
        if (index == kInvalidIndex) {
            record_probe_count(probes);
            return false;
        }

        erase_index(index, probes, false);
        return true;
    }

    /// Erase a previously located entry without hashing and probing for its id
    /// a second time.
    [[nodiscard]] bool erase(const EraseToken& token) noexcept
    {
        if (token.owner_ != this || token.order_ == nullptr ||
            token.mutation_revision_ != mutation_revision_ ||
            token.index_ >= capacity_) {
            return false;
        }

        const Entry& entry = entries_[token.index_];
        if (entry.state != SlotState::Occupied || entry.id != token.id_ ||
            entry.order != token.order_) {
            return false;
        }

        const bool replace_lookup_sample = token.stats_revision_ == stats_revision_;
        erase_index(token.index_, token.lookup_probe_count_, replace_lookup_sample);
        return true;
    }

    [[nodiscard]] std::uint32_t capacity() const noexcept
    {
        return capacity_;
    }

    [[nodiscard]] std::uint32_t size() const noexcept
    {
        return size_;
    }

    [[nodiscard]] std::uint32_t tombstones() const noexcept
    {
        return 0;
    }

    [[nodiscard]] bool full() const noexcept
    {
        return size_ == capacity_;
    }

    [[nodiscard]] OrderIdMapStats stats() const noexcept
    {
        return OrderIdMapStats{size_, capacity_, 0, last_probe_count_, probe_histogram_};
    }

    [[nodiscard]] static constexpr std::uint32_t capacity_for(const std::uint32_t requested_capacity) noexcept
    {
        const std::uint32_t value = requested_capacity < 2U ? 2U : requested_capacity;
        return value > kMaxCapacity ? kMaxCapacity : next_power_of_two(value);
    }

private:
    enum class SlotState : std::uint8_t {
        Empty,
        Occupied,
    };

    struct Entry final {
        OrderId id{kInvalidOrderId};
        Order* order{nullptr};
        std::uint32_t probe_distance{0};
        SlotState state{SlotState::Empty};
    };

    std::uint32_t capacity_{0};
    std::uint32_t mask_{0};
    std::unique_ptr<Entry[]> entries_;
    std::uint32_t size_{0};
    mutable std::uint32_t last_probe_count_{0};
    mutable std::array<std::uint64_t, kProbeHistogramBucketCount> probe_histogram_{};
    std::uint64_t mutation_revision_{0};
    mutable std::uint64_t stats_revision_{0};

    void erase_index(const std::uint32_t index,
                     const std::uint32_t lookup_probes,
                     const bool replace_lookup_sample) noexcept
    {
        std::uint32_t hole = index;
        std::uint32_t next = (hole + 1U) & mask_;
        std::uint32_t compaction_moves = 0;
        while (entries_[next].state == SlotState::Occupied &&
               entries_[next].probe_distance > 0U) {
            entries_[hole] = entries_[next];
            --entries_[hole].probe_distance;
            hole = next;
            next = (next + 1U) & mask_;
            ++compaction_moves;
        }

        entries_[hole] = Entry{};
        --size_;
        ++mutation_revision_;

        const std::uint32_t work = saturated_add(lookup_probes, compaction_moves);
        if (replace_lookup_sample) {
            replace_last_probe_count(lookup_probes, work);
        } else {
            record_probe_count(work);
        }
    }

    static constexpr std::uint64_t splitmix64(std::uint64_t value) noexcept
    {
        value += 0x9e3779b97f4a7c15ULL;
        value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
        value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
        return value ^ (value >> 31U);
    }

    [[nodiscard]] std::uint32_t hash(const OrderId id) const noexcept
    {
        return static_cast<std::uint32_t>(splitmix64(id)) & mask_;
    }

    static constexpr std::uint32_t next_power_of_two(std::uint32_t value) noexcept
    {
        --value;
        value |= value >> 1U;
        value |= value >> 2U;
        value |= value >> 4U;
        value |= value >> 8U;
        value |= value >> 16U;
        return value + 1U;
    }

    [[nodiscard]] std::uint32_t find_index(const OrderId id, std::uint32_t& probes) const noexcept
    {
        probes = 0;
        if (capacity_ == 0 || id == kInvalidOrderId) {
            return kInvalidIndex;
        }

        std::uint32_t slot = hash(id);
        std::uint32_t search_distance = 0;
        for (std::uint32_t step = 0; step < capacity_; ++step) {
            ++probes;
            const Entry& entry = entries_[slot];
            if (entry.state == SlotState::Empty) {
                return kInvalidIndex;
            }
            if (entry.probe_distance < search_distance) {
                return kInvalidIndex;
            }
            if (entry.id == id) {
                return slot;
            }
            slot = (slot + 1U) & mask_;
            ++search_distance;
        }

        return kInvalidIndex;
    }

    void record_probe_count(const std::uint32_t probes) const noexcept
    {
        last_probe_count_ = probes;
        ++probe_histogram_[histogram_bucket(probes)];
        ++stats_revision_;
    }

    void replace_last_probe_count(const std::uint32_t previous,
                                  const std::uint32_t work) const noexcept
    {
        const std::size_t previous_bucket = histogram_bucket(previous);
        if (probe_histogram_[previous_bucket] != 0U) {
            --probe_histogram_[previous_bucket];
        }
        last_probe_count_ = work;
        ++probe_histogram_[histogram_bucket(work)];
        ++stats_revision_;
    }

    [[nodiscard]] static constexpr std::uint32_t saturated_add(
        const std::uint32_t lhs,
        const std::uint32_t rhs) noexcept
    {
        return rhs > std::numeric_limits<std::uint32_t>::max() - lhs
                   ? std::numeric_limits<std::uint32_t>::max()
                   : lhs + rhs;
    }

    [[nodiscard]] static constexpr std::size_t histogram_bucket(
        const std::uint32_t probes) noexcept
    {
        return std::min<std::size_t>(static_cast<std::size_t>(probes),
                                     kProbeHistogramBucketCount - 1U);
    }
};

static_assert(alignof(OrderIdMap) >= 64);

} // namespace eigenbook
