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
        tombstones_ = 0;
        last_probe_count_ = 0;
        probe_histogram_.fill(0);
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

            if (entry.state == SlotState::Empty || entry.state == SlotState::Deleted) {
                if (entry.state == SlotState::Deleted) {
                    --tombstones_;
                }
                entry = incoming;
                ++size_;
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

    [[nodiscard]] bool erase(const OrderId id) noexcept
    {
        if (id == kInvalidOrderId) {
            record_probe_count(0);
            return false;
        }

        std::uint32_t probes = 0;
        const std::uint32_t index = find_index(id, probes);
        record_probe_count(probes);
        if (index == kInvalidIndex) {
            return false;
        }

        Entry& entry = entries_[index];
        entry.id = kInvalidOrderId;
        entry.order = nullptr;
        entry.state = SlotState::Deleted;
        entry.probe_distance = 0;
        --size_;
        ++tombstones_;
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
        return tombstones_;
    }

    [[nodiscard]] bool full() const noexcept
    {
        return size_ == capacity_;
    }

    [[nodiscard]] OrderIdMapStats stats() const noexcept
    {
        return OrderIdMapStats{size_, capacity_, tombstones_, last_probe_count_, probe_histogram_};
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
        Deleted,
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
    std::uint32_t tombstones_{0};
    mutable std::uint32_t last_probe_count_{0};
    mutable std::array<std::uint64_t, kProbeHistogramBucketCount> probe_histogram_{};

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
        for (std::uint32_t step = 0; step < capacity_; ++step) {
            ++probes;
            const Entry& entry = entries_[slot];
            if (entry.state == SlotState::Empty) {
                return kInvalidIndex;
            }
            if (entry.state == SlotState::Occupied && entry.id == id) {
                return slot;
            }
            slot = (slot + 1U) & mask_;
        }

        return kInvalidIndex;
    }

    void record_probe_count(const std::uint32_t probes) const noexcept
    {
        last_probe_count_ = probes;
        const std::size_t bucket =
            std::min<std::size_t>(static_cast<std::size_t>(probes), kProbeHistogramBucketCount - 1U);
        ++probe_histogram_[bucket];
    }
};

static_assert(alignof(OrderIdMap) >= 64);

} // namespace eigenbook
