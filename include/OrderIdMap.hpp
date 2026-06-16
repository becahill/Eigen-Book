#pragma once

#include "Order.hpp"
#include "Types.hpp"

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

    [[nodiscard]] Status can_insert(const OrderId id) const noexcept
    {
        if (id == kInvalidOrderId) {
            return Status::InvalidOrderId;
        }

        bool found_deleted = false;
        for (std::uint32_t probe = 0; probe < capacity_; ++probe) {
            const Entry& entry = entries_[(hash(id) + probe) & mask_];
            if (entry.state == SlotState::Empty) {
                return Status::Accepted;
            }
            if (entry.state == SlotState::Deleted) {
                found_deleted = true;
                continue;
            }
            if (entry.id == id) {
                return Status::DuplicateOrderId;
            }
        }

        return found_deleted ? Status::Accepted : Status::OrderIdMapFull;
    }

    [[nodiscard]] Status insert(const OrderId id, Order* order) noexcept
    {
        if (id == kInvalidOrderId || order == nullptr) {
            return Status::InvalidOrderId;
        }

        std::uint32_t first_deleted = kInvalidIndex;
        for (std::uint32_t probe = 0; probe < capacity_; ++probe) {
            const std::uint32_t index = (hash(id) + probe) & mask_;
            Entry& entry = entries_[index];

            if (entry.state == SlotState::Occupied && entry.id == id) {
                return Status::DuplicateOrderId;
            }

            if (entry.state == SlotState::Deleted && first_deleted == kInvalidIndex) {
                first_deleted = index;
                continue;
            }

            if (entry.state == SlotState::Empty) {
                const std::uint32_t target = first_deleted == kInvalidIndex ? index : first_deleted;
                place(target, id, order);
                return Status::Accepted;
            }
        }

        if (first_deleted != kInvalidIndex) {
            place(first_deleted, id, order);
            return Status::Accepted;
        }

        return Status::OrderIdMapFull;
    }

    [[nodiscard]] Order* find(const OrderId id) noexcept
    {
        return const_cast<Order*>(static_cast<const OrderIdMap&>(*this).find(id));
    }

    [[nodiscard]] const Order* find(const OrderId id) const noexcept
    {
        if (id == kInvalidOrderId) {
            return nullptr;
        }

        for (std::uint32_t probe = 0; probe < capacity_; ++probe) {
            const Entry& entry = entries_[(hash(id) + probe) & mask_];
            if (entry.state == SlotState::Empty) {
                return nullptr;
            }
            if (entry.state == SlotState::Occupied && entry.id == id) {
                return entry.order;
            }
        }

        return nullptr;
    }

    [[nodiscard]] bool erase(const OrderId id) noexcept
    {
        if (id == kInvalidOrderId) {
            return false;
        }

        for (std::uint32_t probe = 0; probe < capacity_; ++probe) {
            Entry& entry = entries_[(hash(id) + probe) & mask_];
            if (entry.state == SlotState::Empty) {
                return false;
            }
            if (entry.state == SlotState::Occupied && entry.id == id) {
                entry.id = kInvalidOrderId;
                entry.order = nullptr;
                entry.state = SlotState::Deleted;
                --size_;
                ++tombstones_;
                return true;
            }
        }

        return false;
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
        SlotState state{SlotState::Empty};
    };

    std::uint32_t capacity_{0};
    std::uint32_t mask_{0};
    std::unique_ptr<Entry[]> entries_;
    std::uint32_t size_{0};
    std::uint32_t tombstones_{0};

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

    void place(const std::uint32_t index, const OrderId id, Order* order) noexcept
    {
        if (entries_[index].state == SlotState::Deleted) {
            --tombstones_;
        }

        entries_[index].id = id;
        entries_[index].order = order;
        entries_[index].state = SlotState::Occupied;
        ++size_;
    }
};

static_assert(alignof(OrderIdMap) >= 64);

} // namespace eigenbook
