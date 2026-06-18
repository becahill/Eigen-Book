#pragma once

#include "EventLog.hpp"
#include "MemoryPool.hpp"
#include "OrderIdMap.hpp"
#include "PriceLevel.hpp"
#include "Types.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <variant>

namespace eigenbook {

class alignas(64) DenseBookSide final {
public:
    DenseBookSide(const Side side, const Price min_price, const Price max_price, const Price tick_size)
        : side_(side),
          min_price_(min_price),
          max_price_(max_price),
          tick_size_(tick_size),
          level_count_(compute_level_count(min_price, max_price, tick_size)),
          levels_(level_count_ == 0 ? nullptr : std::make_unique<PriceLevel[]>(level_count_)),
          occupancy_word_count_(level_count_ == 0 ? 0 : ((level_count_ + 63U) / 64U)),
          occupancy_(occupancy_word_count_ == 0 ? nullptr : std::make_unique<std::uint64_t[]>(occupancy_word_count_))
    {
        for (std::uint32_t i = 0; i < level_count_; ++i) {
            levels_[i].reset(price_at_index(i));
        }

        for (std::uint32_t i = 0; i < occupancy_word_count_; ++i) {
            occupancy_[i] = 0;
        }
    }

    DenseBookSide(const DenseBookSide&) = delete;
    DenseBookSide& operator=(const DenseBookSide&) = delete;
    DenseBookSide(DenseBookSide&&) = delete;
    DenseBookSide& operator=(DenseBookSide&&) = delete;

    void clear() noexcept
    {
        for (std::uint32_t i = 0; i < level_count_; ++i) {
            levels_[i].reset(price_at_index(i));
        }

        for (std::uint32_t i = 0; i < occupancy_word_count_; ++i) {
            occupancy_[i] = 0;
        }

        best_index_ = kInvalidIndex;
    }

    [[nodiscard]] Side side() const noexcept
    {
        return side_;
    }

    [[nodiscard]] bool price_in_range(const Price price) const noexcept
    {
        if (price < min_price_ || price > max_price_ || tick_size_ <= 0) {
            return false;
        }

        const auto tick = static_cast<std::uint64_t>(tick_size_);
        return price_distance(min_price_, price) % tick == 0U;
    }

    [[nodiscard]] Status add_order(Order& order) noexcept
    {
        if (order.side != side_) {
            return Status::InternalError;
        }
        if (order.quantity == 0) {
            return Status::InvalidQuantity;
        }
        if (!price_in_range(order.price)) {
            return Status::InvalidPrice;
        }

        const std::uint32_t index = index_for_price(order.price);
        PriceLevel& level = levels_[index];
        const bool was_empty = level.empty();
        if (!level.append(order)) {
            return Status::InternalError;
        }

        if (was_empty) {
            set_occupied(index);
        }

        return Status::Accepted;
    }

    [[nodiscard]] Status remove_order(Order& order) noexcept
    {
        if (order.side != side_ || !price_in_range(order.price)) {
            return Status::InternalError;
        }

        const std::uint32_t index = index_for_price(order.price);
        PriceLevel& level = levels_[index];
        if (order.level != &level || !level.remove(order)) {
            return Status::InternalError;
        }

        if (level.empty()) {
            clear_occupied(index);
        }

        return Status::Accepted;
    }

    [[nodiscard]] Status reduce_order(Order& order, const Quantity new_quantity) noexcept
    {
        if (order.side != side_ || !price_in_range(order.price)) {
            return Status::InternalError;
        }

        PriceLevel& level = levels_[index_for_price(order.price)];
        return level.set_quantity_keep_priority(order, new_quantity) ? Status::Accepted : Status::InternalError;
    }

    // Bounded by fills and occupied price words crossed; used to avoid partially executing
    // an order that cannot persist its residual because fixed storage is exhausted.
    [[nodiscard]] Quantity executable_quantity(const Quantity requested_quantity,
                                               const bool has_limit_price,
                                               const Price limit_price) const noexcept
    {
        Quantity executable = 0;
        std::uint32_t index = best_index_;

        while (executable < requested_quantity && index != kInvalidIndex) {
            const PriceLevel& level = levels_[index];
            if (has_limit_price && !crosses(level.price(), limit_price)) {
                break;
            }

            const Quantity remaining = requested_quantity - executable;
            const Quantity level_quantity = level.total_quantity();
            if (level_quantity >= remaining) {
                return requested_quantity;
            }

            executable += level_quantity;
            index = find_next_index_after(index);
        }

        return executable;
    }

    [[nodiscard]] MatchResult match(const Quantity requested_quantity,
                                    const bool has_limit_price,
                                    const Price limit_price,
                                    OrderIdMap& order_ids,
                                    MemoryPool<Order>& order_pool,
                                    EventLog& event_log,
                                    const OrderId aggressor_id,
                                    const Side aggressor_side,
                                    const Timestamp timestamp) noexcept
    {
        MatchResult result{};
        result.requested_quantity = requested_quantity;
        result.remaining_quantity = requested_quantity;

        while (result.remaining_quantity > 0 && best_index_ != kInvalidIndex) {
            PriceLevel& level = levels_[best_index_];
            if (has_limit_price && !crosses(level.price(), limit_price)) {
                break;
            }

            while (result.remaining_quantity > 0 && !level.empty()) {
                Order* resting_order = level.front();
                const OrderId resting_id = resting_order->id;
                const Quantity executed_quantity = level.fill_front(result.remaining_quantity);
                if (executed_quantity == 0) {
                    result.status = Status::InternalError;
                    return result;
                }

                result.remaining_quantity -= executed_quantity;
                result.executed_quantity += executed_quantity;
                ++result.fills;
                result.has_last_price = true;
                result.last_price = level.price();
                event_log.append_trade(
                    aggressor_id, resting_id, aggressor_side, level.price(), executed_quantity, timestamp);

                if (resting_order->quantity == 0) {
                    if (!level.remove(*resting_order)) {
                        result.status = Status::InternalError;
                        return result;
                    }
                    static_cast<void>(order_ids.erase(resting_id));
                    resting_order->clear();
                    order_pool.release(resting_order);
                }
            }

            if (level.empty()) {
                clear_occupied(best_index_);
            }
        }

        if (result.executed_quantity == 0) {
            result.status = Status::NoLiquidity;
        } else if (result.remaining_quantity == 0) {
            result.status = Status::Filled;
        } else {
            result.status = Status::PartiallyFilled;
        }

        return result;
    }

    [[nodiscard]] BestQuote best_quote() const noexcept
    {
        if (best_index_ == kInvalidIndex) {
            return {};
        }

        const PriceLevel& level = levels_[best_index_];
        return BestQuote{true, level.price(), level.total_quantity(), level.order_count()};
    }

    [[nodiscard]] std::uint32_t depth(const std::uint32_t max_levels,
                                      DepthLevel* const out_buffer) const noexcept
    {
        if (max_levels == 0 || out_buffer == nullptr) {
            return 0;
        }

        std::uint32_t written = 0;
        std::uint32_t index = best_index_;
        while (written < max_levels && index != kInvalidIndex) {
            const PriceLevel& level = levels_[index];
            out_buffer[written] = DepthLevel{level.price(), level.total_quantity(), level.order_count()};
            ++written;
            index = find_next_index_after(index);
        }

        return written;
    }

    [[nodiscard]] Quantity depth_at_price(const Price price) const noexcept
    {
        if (!price_in_range(price)) {
            return 0;
        }

        return levels_[index_for_price(price)].total_quantity();
    }

    [[nodiscard]] std::uint32_t order_count_at_price(const Price price) const noexcept
    {
        if (!price_in_range(price)) {
            return 0;
        }

        return levels_[index_for_price(price)].order_count();
    }

    [[nodiscard]] std::uint32_t level_count() const noexcept
    {
        return level_count_;
    }

    [[nodiscard]] std::uint32_t occupied_level_count() const noexcept
    {
        std::uint32_t count = 0;
        for_each_level([&count](const PriceLevel&) noexcept {
            ++count;
        });
        return count;
    }

    template <typename Fn>
    void for_each_level(Fn&& fn) const noexcept
    {
        std::uint32_t index = best_index_;
        while (index != kInvalidIndex) {
            const PriceLevel& level = levels_[index];
            fn(level);
            index = find_next_index_after(index);
        }
    }

    template <typename Fn>
    void for_each_order(Fn&& fn) const noexcept
    {
        for_each_level([&fn](const PriceLevel& level) noexcept {
            const Order* order = level.front();
            while (order != nullptr) {
                fn(*order);
                order = order->next;
            }
        });
    }

    [[nodiscard]] BookSideStats stats() const noexcept
    {
        const std::uint32_t occupied = occupied_level_count();
        return BookSideStats{PriceLevelMode::Dense,
                             level_count_,
                             level_count_,
                             occupied,
                             0,
                             utilization(occupied, level_count_)};
    }

private:
    Side side_{Side::Buy};
    Price min_price_{0};
    Price max_price_{0};
    Price tick_size_{1};
    std::uint32_t level_count_{0};
    std::unique_ptr<PriceLevel[]> levels_;
    std::uint32_t occupancy_word_count_{0};
    std::unique_ptr<std::uint64_t[]> occupancy_;
    std::uint32_t best_index_{kInvalidIndex};

    [[nodiscard]] std::uint32_t index_for_price(const Price price) const noexcept
    {
        const auto tick = static_cast<std::uint64_t>(tick_size_);
        return static_cast<std::uint32_t>(price_distance(min_price_, price) / tick);
    }

    [[nodiscard]] static std::uint32_t compute_level_count(const Price min_price,
                                                           const Price max_price,
                                                           const Price tick_size) noexcept
    {
        if (min_price > max_price || tick_size <= 0) {
            return 0;
        }

        const std::uint64_t range = price_distance(min_price, max_price);
        if (range > static_cast<std::uint64_t>(std::numeric_limits<Price>::max())) {
            return 0;
        }

        const auto tick = static_cast<std::uint64_t>(tick_size);
        if (range % tick != 0U) {
            return 0;
        }

        const std::uint64_t intervals = range / tick;
        if (intervals >= static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())) {
            return 0;
        }

        return static_cast<std::uint32_t>(intervals + 1U);
    }

    [[nodiscard]] Price price_at_index(const std::uint32_t index) const noexcept
    {
        return min_price_ + static_cast<Price>(static_cast<std::uint64_t>(index) *
                                               static_cast<std::uint64_t>(tick_size_));
    }

    [[nodiscard]] bool crosses(const Price resting_price, const Price limit_price) const noexcept
    {
        return side_ == Side::Sell ? resting_price <= limit_price : resting_price >= limit_price;
    }

    void set_occupied(const std::uint32_t index) noexcept
    {
        occupancy_[index / 64U] |= (1ULL << (index % 64U));
        if (best_index_ == kInvalidIndex || better(index, best_index_)) {
            best_index_ = index;
        }
    }

    void clear_occupied(const std::uint32_t index) noexcept
    {
        occupancy_[index / 64U] &= ~(1ULL << (index % 64U));
        if (index == best_index_) {
            best_index_ = find_best_index();
        }
    }

    [[nodiscard]] bool better(const std::uint32_t lhs, const std::uint32_t rhs) const noexcept
    {
        return side_ == Side::Buy ? lhs > rhs : lhs < rhs;
    }

    [[nodiscard]] std::uint32_t find_best_index() const noexcept
    {
        if (occupancy_word_count_ == 0) {
            return kInvalidIndex;
        }

        if (side_ == Side::Buy) {
            for (std::uint32_t word = occupancy_word_count_; word > 0; --word) {
                const std::uint32_t word_index = word - 1U;
                const std::uint64_t bits = occupancy_[word_index];
                if (bits != 0) {
                    return word_index * 64U + highest_set_bit(bits);
                }
            }
        } else {
            for (std::uint32_t word_index = 0; word_index < occupancy_word_count_; ++word_index) {
                const std::uint64_t bits = occupancy_[word_index];
                if (bits != 0) {
                    return word_index * 64U + lowest_set_bit(bits);
                }
            }
        }

        return kInvalidIndex;
    }

    [[nodiscard]] std::uint32_t find_next_index_after(const std::uint32_t index) const noexcept
    {
        if (index >= level_count_) {
            return kInvalidIndex;
        }

        if (side_ == Side::Buy) {
            if (index == 0) {
                return kInvalidIndex;
            }

            const std::uint32_t next_index = index - 1U;
            std::uint32_t word_index = next_index / 64U;
            const std::uint32_t bit_index = next_index % 64U;
            std::uint64_t bits = occupancy_[word_index] & low_bits_through(bit_index);
            if (bits != 0) {
                return word_index * 64U + highest_set_bit(bits);
            }

            while (word_index > 0) {
                --word_index;
                bits = occupancy_[word_index];
                if (bits != 0) {
                    return word_index * 64U + highest_set_bit(bits);
                }
            }

            return kInvalidIndex;
        }

        const std::uint32_t next_index = index + 1U;
        if (next_index >= level_count_) {
            return kInvalidIndex;
        }

        std::uint32_t word_index = next_index / 64U;
        const std::uint32_t bit_index = next_index % 64U;
        std::uint64_t bits = occupancy_[word_index] & (~0ULL << bit_index);
        if (bits != 0) {
            return word_index * 64U + lowest_set_bit(bits);
        }

        for (++word_index; word_index < occupancy_word_count_; ++word_index) {
            bits = occupancy_[word_index];
            if (bits != 0) {
                return word_index * 64U + lowest_set_bit(bits);
            }
        }

        return kInvalidIndex;
    }

    [[nodiscard]] static constexpr std::uint64_t low_bits_through(const std::uint32_t bit_index) noexcept
    {
        return bit_index == 63U ? ~0ULL : ((1ULL << (bit_index + 1U)) - 1ULL);
    }

    static std::uint32_t lowest_set_bit(const std::uint64_t value) noexcept
    {
#if defined(__GNUC__) || defined(__clang__)
        return static_cast<std::uint32_t>(__builtin_ctzll(value));
#else
        std::uint32_t bit = 0;
        std::uint64_t copy = value;
        while ((copy & 1ULL) == 0) {
            copy >>= 1U;
            ++bit;
        }
        return bit;
#endif
    }

    static std::uint32_t highest_set_bit(const std::uint64_t value) noexcept
    {
#if defined(__GNUC__) || defined(__clang__)
        return 63U - static_cast<std::uint32_t>(__builtin_clzll(value));
#else
        std::uint32_t bit = 63;
        std::uint64_t mask = 1ULL << 63U;
        while ((value & mask) == 0) {
            mask >>= 1U;
            --bit;
        }
        return bit;
#endif
    }

    [[nodiscard]] static double utilization(const std::uint32_t used,
                                            const std::uint32_t capacity) noexcept
    {
        return capacity == 0 ? 0.0 : static_cast<double>(used) / static_cast<double>(capacity);
    }
};

static_assert(alignof(DenseBookSide) >= 64);

class alignas(64) SparseBookSide final {
public:
    SparseBookSide(const Side side, const BookConfig& config)
        : side_(side),
          min_price_(config.min_price),
          max_price_(config.max_price),
          tick_size_(config.tick_size),
          configured_level_count_(compute_level_count(config.min_price, config.max_price, config.tick_size)),
          level_capacity_(config.max_orders),
          levels_(level_capacity_ == 0 ? nullptr : std::make_unique<PriceLevel[]>(level_capacity_)),
          metadata_(level_capacity_ == 0 ? nullptr : std::make_unique<LevelMeta[]>(level_capacity_)),
          free_slots_(level_capacity_ == 0 ? nullptr : std::make_unique<std::uint32_t[]>(level_capacity_)),
          sorted_slots_(level_capacity_ == 0 ? nullptr : std::make_unique<std::uint32_t[]>(level_capacity_)),
          level_map_capacity_(level_capacity_ == 0 ? 0 : level_map_capacity_for(level_capacity_)),
          level_map_mask_(level_map_capacity_ == 0 ? 0 : level_map_capacity_ - 1U),
          level_map_(level_map_capacity_ == 0 ? nullptr : std::make_unique<MapEntry[]>(level_map_capacity_))
    {
        reset_free_slots();
    }

    SparseBookSide(const SparseBookSide&) = delete;
    SparseBookSide& operator=(const SparseBookSide&) = delete;
    SparseBookSide(SparseBookSide&&) = delete;
    SparseBookSide& operator=(SparseBookSide&&) = delete;

    void clear() noexcept
    {
        for (std::uint32_t i = 0; i < occupied_level_count_; ++i) {
            const std::uint32_t slot = sorted_slots_[i];
            levels_[slot].reset(0);
            metadata_[slot] = LevelMeta{};
        }

        for (std::uint32_t i = 0; i < level_map_capacity_; ++i) {
            level_map_[i] = MapEntry{};
        }

        occupied_level_count_ = 0;
        level_map_size_ = 0;
        level_map_tombstones_ = 0;
        best_slot_ = kInvalidIndex;
        reset_free_slots();
    }

    [[nodiscard]] Side side() const noexcept
    {
        return side_;
    }

    [[nodiscard]] bool price_in_range(const Price price) const noexcept
    {
        if (price < min_price_ || price > max_price_ || tick_size_ <= 0) {
            return false;
        }

        const auto tick = static_cast<std::uint64_t>(tick_size_);
        return price_distance(min_price_, price) % tick == 0U;
    }

    [[nodiscard]] Status add_order(Order& order) noexcept
    {
        if (order.side != side_) {
            return Status::InternalError;
        }
        if (order.quantity == 0) {
            return Status::InvalidQuantity;
        }
        if (!price_in_range(order.price)) {
            return Status::InvalidPrice;
        }

        std::uint32_t slot = find_slot(order.price);
        if (slot == kInvalidIndex) {
            slot = create_level(order.price);
            if (slot == kInvalidIndex) {
                return Status::PoolExhausted;
            }
        }

        PriceLevel& level = levels_[slot];
        if (!level.append(order)) {
            if (level.empty()) {
                remove_occupied_level(slot);
            }
            return Status::InternalError;
        }

        return Status::Accepted;
    }

    [[nodiscard]] Status remove_order(Order& order) noexcept
    {
        if (order.side != side_ || !price_in_range(order.price)) {
            return Status::InternalError;
        }

        const std::uint32_t slot = find_slot(order.price);
        if (slot == kInvalidIndex) {
            return Status::InternalError;
        }

        PriceLevel& level = levels_[slot];
        if (order.level != &level || !level.remove(order)) {
            return Status::InternalError;
        }

        if (level.empty()) {
            remove_occupied_level(slot);
        }

        return Status::Accepted;
    }

    [[nodiscard]] Status reduce_order(Order& order, const Quantity new_quantity) noexcept
    {
        if (order.side != side_ || !price_in_range(order.price)) {
            return Status::InternalError;
        }

        const std::uint32_t slot = find_slot(order.price);
        if (slot == kInvalidIndex) {
            return Status::InternalError;
        }

        return levels_[slot].set_quantity_keep_priority(order, new_quantity) ? Status::Accepted
                                                                             : Status::InternalError;
    }

    // Bounded by fills and occupied price levels crossed. Sparse best/next price
    // traversal is O(1); level creation/removal shifts at most max_orders slots.
    [[nodiscard]] Quantity executable_quantity(const Quantity requested_quantity,
                                               const bool has_limit_price,
                                               const Price limit_price) const noexcept
    {
        Quantity executable = 0;
        std::uint32_t slot = best_slot_;

        while (executable < requested_quantity && slot != kInvalidIndex) {
            const PriceLevel& level = levels_[slot];
            if (has_limit_price && !crosses(level.price(), limit_price)) {
                break;
            }

            const Quantity remaining = requested_quantity - executable;
            const Quantity level_quantity = level.total_quantity();
            if (level_quantity >= remaining) {
                return requested_quantity;
            }

            executable += level_quantity;
            slot = find_next_slot_after(slot);
        }

        return executable;
    }

    [[nodiscard]] MatchResult match(const Quantity requested_quantity,
                                    const bool has_limit_price,
                                    const Price limit_price,
                                    OrderIdMap& order_ids,
                                    MemoryPool<Order>& order_pool,
                                    EventLog& event_log,
                                    const OrderId aggressor_id,
                                    const Side aggressor_side,
                                    const Timestamp timestamp) noexcept
    {
        MatchResult result{};
        result.requested_quantity = requested_quantity;
        result.remaining_quantity = requested_quantity;

        while (result.remaining_quantity > 0 && best_slot_ != kInvalidIndex) {
            const std::uint32_t slot = best_slot_;
            PriceLevel& level = levels_[slot];
            if (has_limit_price && !crosses(level.price(), limit_price)) {
                break;
            }

            while (result.remaining_quantity > 0 && !level.empty()) {
                Order* resting_order = level.front();
                const OrderId resting_id = resting_order->id;
                const Quantity executed_quantity = level.fill_front(result.remaining_quantity);
                if (executed_quantity == 0) {
                    result.status = Status::InternalError;
                    return result;
                }

                result.remaining_quantity -= executed_quantity;
                result.executed_quantity += executed_quantity;
                ++result.fills;
                result.has_last_price = true;
                result.last_price = level.price();
                event_log.append_trade(
                    aggressor_id, resting_id, aggressor_side, level.price(), executed_quantity, timestamp);

                if (resting_order->quantity == 0) {
                    if (!level.remove(*resting_order)) {
                        result.status = Status::InternalError;
                        return result;
                    }
                    static_cast<void>(order_ids.erase(resting_id));
                    resting_order->clear();
                    order_pool.release(resting_order);
                }
            }

            if (level.empty()) {
                remove_occupied_level(slot);
            }
        }

        if (result.executed_quantity == 0) {
            result.status = Status::NoLiquidity;
        } else if (result.remaining_quantity == 0) {
            result.status = Status::Filled;
        } else {
            result.status = Status::PartiallyFilled;
        }

        return result;
    }

    [[nodiscard]] BestQuote best_quote() const noexcept
    {
        if (best_slot_ == kInvalidIndex) {
            return {};
        }

        const PriceLevel& level = levels_[best_slot_];
        return BestQuote{true, level.price(), level.total_quantity(), level.order_count()};
    }

    [[nodiscard]] std::uint32_t depth(const std::uint32_t max_levels,
                                      DepthLevel* const out_buffer) const noexcept
    {
        if (max_levels == 0 || out_buffer == nullptr) {
            return 0;
        }

        std::uint32_t written = 0;
        std::uint32_t slot = best_slot_;
        while (written < max_levels && slot != kInvalidIndex) {
            const PriceLevel& level = levels_[slot];
            out_buffer[written] = DepthLevel{level.price(), level.total_quantity(), level.order_count()};
            ++written;
            slot = find_next_slot_after(slot);
        }

        return written;
    }

    [[nodiscard]] Quantity depth_at_price(const Price price) const noexcept
    {
        if (!price_in_range(price)) {
            return 0;
        }

        const std::uint32_t slot = find_slot(price);
        return slot == kInvalidIndex ? 0 : levels_[slot].total_quantity();
    }

    [[nodiscard]] std::uint32_t order_count_at_price(const Price price) const noexcept
    {
        if (!price_in_range(price)) {
            return 0;
        }

        const std::uint32_t slot = find_slot(price);
        return slot == kInvalidIndex ? 0 : levels_[slot].order_count();
    }

    [[nodiscard]] std::uint32_t level_count() const noexcept
    {
        return configured_level_count_;
    }

    [[nodiscard]] std::uint32_t occupied_level_count() const noexcept
    {
        return occupied_level_count_;
    }

    template <typename Fn>
    void for_each_level(Fn&& fn) const noexcept
    {
        std::uint32_t slot = best_slot_;
        while (slot != kInvalidIndex) {
            const PriceLevel& level = levels_[slot];
            fn(level);
            slot = find_next_slot_after(slot);
        }
    }

    template <typename Fn>
    void for_each_order(Fn&& fn) const noexcept
    {
        for_each_level([&fn](const PriceLevel& level) noexcept {
            const Order* order = level.front();
            while (order != nullptr) {
                fn(*order);
                order = order->next;
            }
        });
    }

    [[nodiscard]] BookSideStats stats() const noexcept
    {
        return BookSideStats{PriceLevelMode::Sparse,
                             configured_level_count_,
                             level_capacity_,
                             occupied_level_count_,
                             level_map_capacity_,
                             utilization(occupied_level_count_, level_capacity_)};
    }

private:
    enum class SlotState : std::uint8_t {
        Empty,
        Occupied,
        Deleted,
    };

    struct LevelMeta final {
        Price price{0};
        std::uint32_t sorted_position{kInvalidIndex};
        bool occupied{false};
    };

    struct MapEntry final {
        Price price{0};
        std::uint32_t slot{kInvalidIndex};
        std::uint32_t probe_distance{0};
        SlotState state{SlotState::Empty};
    };

    Side side_{Side::Buy};
    Price min_price_{0};
    Price max_price_{0};
    Price tick_size_{1};
    std::uint32_t configured_level_count_{0};
    std::uint32_t level_capacity_{0};
    std::unique_ptr<PriceLevel[]> levels_;
    std::unique_ptr<LevelMeta[]> metadata_;
    std::unique_ptr<std::uint32_t[]> free_slots_;
    std::unique_ptr<std::uint32_t[]> sorted_slots_;
    std::uint32_t free_count_{0};
    std::uint32_t occupied_level_count_{0};
    std::uint32_t best_slot_{kInvalidIndex};
    std::uint32_t level_map_capacity_{0};
    std::uint32_t level_map_mask_{0};
    std::unique_ptr<MapEntry[]> level_map_;
    std::uint32_t level_map_size_{0};
    std::uint32_t level_map_tombstones_{0};

    [[nodiscard]] static std::uint32_t compute_level_count(const Price min_price,
                                                           const Price max_price,
                                                           const Price tick_size) noexcept
    {
        if (min_price > max_price || tick_size <= 0) {
            return 0;
        }

        const std::uint64_t range = price_distance(min_price, max_price);
        if (range > static_cast<std::uint64_t>(std::numeric_limits<Price>::max())) {
            return 0;
        }

        const auto tick = static_cast<std::uint64_t>(tick_size);
        if (range % tick != 0U) {
            return 0;
        }

        const std::uint64_t intervals = range / tick;
        if (intervals >= static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())) {
            return 0;
        }

        return static_cast<std::uint32_t>(intervals + 1U);
    }

    [[nodiscard]] bool crosses(const Price resting_price, const Price limit_price) const noexcept
    {
        return side_ == Side::Sell ? resting_price <= limit_price : resting_price >= limit_price;
    }

    void reset_free_slots() noexcept
    {
        free_count_ = level_capacity_;
        for (std::uint32_t i = 0; i < level_capacity_; ++i) {
            free_slots_[i] = level_capacity_ - 1U - i;
            levels_[i].reset(0);
            metadata_[i] = LevelMeta{};
        }
    }

    [[nodiscard]] std::uint32_t allocate_level_slot() noexcept
    {
        if (free_count_ == 0) {
            return kInvalidIndex;
        }

        --free_count_;
        return free_slots_[free_count_];
    }

    void release_level_slot(const std::uint32_t slot) noexcept
    {
        if (free_count_ < level_capacity_) {
            free_slots_[free_count_] = slot;
            ++free_count_;
        }
    }

    [[nodiscard]] std::uint32_t create_level(const Price price) noexcept
    {
        const std::uint32_t slot = allocate_level_slot();
        if (slot == kInvalidIndex) {
            return kInvalidIndex;
        }

        levels_[slot].reset(price);
        metadata_[slot].price = price;
        metadata_[slot].occupied = true;
        if (!map_insert(price, slot)) {
            metadata_[slot] = LevelMeta{};
            levels_[slot].reset(0);
            release_level_slot(slot);
            return kInvalidIndex;
        }

        insert_sorted_slot(slot);
        return slot;
    }

    void remove_occupied_level(const std::uint32_t slot) noexcept
    {
        if (slot == kInvalidIndex || !metadata_[slot].occupied) {
            return;
        }

        static_cast<void>(map_erase(metadata_[slot].price));
        remove_sorted_slot(slot);
        levels_[slot].reset(0);
        metadata_[slot] = LevelMeta{};
        release_level_slot(slot);
        refresh_best_slot();
    }

    void insert_sorted_slot(const std::uint32_t slot) noexcept
    {
        const Price price = metadata_[slot].price;
        std::uint32_t position = lower_bound_price(price);
        for (std::uint32_t i = occupied_level_count_; i > position; --i) {
            sorted_slots_[i] = sorted_slots_[i - 1U];
            metadata_[sorted_slots_[i]].sorted_position = i;
        }

        sorted_slots_[position] = slot;
        metadata_[slot].sorted_position = position;
        ++occupied_level_count_;
        refresh_best_slot();
    }

    void remove_sorted_slot(const std::uint32_t slot) noexcept
    {
        const std::uint32_t position = metadata_[slot].sorted_position;
        if (position == kInvalidIndex || position >= occupied_level_count_) {
            return;
        }

        for (std::uint32_t i = position + 1U; i < occupied_level_count_; ++i) {
            sorted_slots_[i - 1U] = sorted_slots_[i];
            metadata_[sorted_slots_[i - 1U]].sorted_position = i - 1U;
        }

        --occupied_level_count_;
        metadata_[slot].sorted_position = kInvalidIndex;
    }

    [[nodiscard]] std::uint32_t lower_bound_price(const Price price) const noexcept
    {
        std::uint32_t first = 0;
        std::uint32_t count = occupied_level_count_;
        while (count > 0) {
            const std::uint32_t step = count / 2U;
            const std::uint32_t position = first + step;
            if (metadata_[sorted_slots_[position]].price < price) {
                first = position + 1U;
                count -= step + 1U;
            } else {
                count = step;
            }
        }
        return first;
    }

    void refresh_best_slot() noexcept
    {
        if (occupied_level_count_ == 0) {
            best_slot_ = kInvalidIndex;
            return;
        }

        best_slot_ = side_ == Side::Buy ? sorted_slots_[occupied_level_count_ - 1U] : sorted_slots_[0];
    }

    [[nodiscard]] std::uint32_t find_next_slot_after(const std::uint32_t slot) const noexcept
    {
        if (slot == kInvalidIndex || !metadata_[slot].occupied) {
            return kInvalidIndex;
        }

        const std::uint32_t position = metadata_[slot].sorted_position;
        if (side_ == Side::Buy) {
            return position == 0 ? kInvalidIndex : sorted_slots_[position - 1U];
        }

        const std::uint32_t next_position = position + 1U;
        return next_position >= occupied_level_count_ ? kInvalidIndex : sorted_slots_[next_position];
    }

    [[nodiscard]] std::uint32_t find_slot(const Price price) const noexcept
    {
        if (level_map_capacity_ == 0) {
            return kInvalidIndex;
        }

        std::uint32_t index = hash_price(price);
        for (std::uint32_t probe = 0; probe < level_map_capacity_; ++probe) {
            const MapEntry& entry = level_map_[index];
            if (entry.state == SlotState::Empty) {
                return kInvalidIndex;
            }
            if (entry.state == SlotState::Occupied && entry.price == price) {
                return entry.slot;
            }
            index = (index + 1U) & level_map_mask_;
        }

        return kInvalidIndex;
    }

    [[nodiscard]] bool map_insert(const Price price, const std::uint32_t slot) noexcept
    {
        if (level_map_capacity_ == 0 || level_map_size_ == level_map_capacity_ ||
            find_slot(price) != kInvalidIndex) {
            return false;
        }

        MapEntry incoming{};
        incoming.price = price;
        incoming.slot = slot;
        incoming.state = SlotState::Occupied;
        incoming.probe_distance = 0;

        std::uint32_t index = hash_price(price);
        for (std::uint32_t probe = 0; probe < level_map_capacity_; ++probe) {
            MapEntry& entry = level_map_[index];
            if (entry.state == SlotState::Empty || entry.state == SlotState::Deleted) {
                if (entry.state == SlotState::Deleted) {
                    --level_map_tombstones_;
                }
                entry = incoming;
                ++level_map_size_;
                return true;
            }

            if (entry.probe_distance < incoming.probe_distance) {
                std::swap(entry, incoming);
            }

            index = (index + 1U) & level_map_mask_;
            if (incoming.probe_distance != std::numeric_limits<std::uint32_t>::max()) {
                ++incoming.probe_distance;
            }
        }

        return false;
    }

    [[nodiscard]] bool map_erase(const Price price) noexcept
    {
        if (level_map_capacity_ == 0) {
            return false;
        }

        std::uint32_t index = hash_price(price);
        for (std::uint32_t probe = 0; probe < level_map_capacity_; ++probe) {
            MapEntry& entry = level_map_[index];
            if (entry.state == SlotState::Empty) {
                return false;
            }
            if (entry.state == SlotState::Occupied && entry.price == price) {
                entry = MapEntry{};
                entry.state = SlotState::Deleted;
                --level_map_size_;
                ++level_map_tombstones_;
                return true;
            }
            index = (index + 1U) & level_map_mask_;
        }

        return false;
    }

    [[nodiscard]] std::uint32_t hash_price(const Price price) const noexcept
    {
        return static_cast<std::uint32_t>(splitmix64(static_cast<std::uint64_t>(price))) & level_map_mask_;
    }

    [[nodiscard]] static constexpr std::uint64_t splitmix64(std::uint64_t value) noexcept
    {
        value += 0x9e3779b97f4a7c15ULL;
        value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
        value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
        return value ^ (value >> 31U);
    }

    [[nodiscard]] static constexpr std::uint32_t level_map_capacity_for(const std::uint32_t level_capacity) noexcept
    {
        if (level_capacity == 0) {
            return 0;
        }

        const std::uint32_t required =
            level_capacity > (OrderIdMap::kMaxCapacity / 2U) ? OrderIdMap::kMaxCapacity : level_capacity * 2U;
        return next_power_of_two(std::max(2U, required));
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

    [[nodiscard]] static double utilization(const std::uint32_t used,
                                            const std::uint32_t capacity) noexcept
    {
        return capacity == 0 ? 0.0 : static_cast<double>(used) / static_cast<double>(capacity);
    }
};

static_assert(alignof(SparseBookSide) >= 64);

class alignas(64) BookSide final {
public:
    BookSide(const Side side, const Price min_price, const Price max_price, const Price tick_size)
        : mode_(PriceLevelMode::Dense)
    {
        impl_.emplace<DenseBookSide>(side, min_price, max_price, tick_size);
    }

    BookSide(const Side side, const BookConfig& config)
        : mode_(config.price_level_mode == PriceLevelMode::Sparse ? PriceLevelMode::Sparse : PriceLevelMode::Dense)
    {
        if (mode_ == PriceLevelMode::Sparse) {
            impl_.emplace<SparseBookSide>(side, config);
        } else {
            impl_.emplace<DenseBookSide>(side, config.min_price, config.max_price, config.tick_size);
        }
    }

    BookSide(const BookSide&) = delete;
    BookSide& operator=(const BookSide&) = delete;
    BookSide(BookSide&&) = delete;
    BookSide& operator=(BookSide&&) = delete;

    void clear() noexcept
    {
        if (sparse()) {
            sparse_side().clear();
        } else {
            dense_side().clear();
        }
    }

    [[nodiscard]] Side side() const noexcept
    {
        return sparse() ? sparse_side().side() : dense_side().side();
    }

    [[nodiscard]] bool price_in_range(const Price price) const noexcept
    {
        return sparse() ? sparse_side().price_in_range(price) : dense_side().price_in_range(price);
    }

    [[nodiscard]] Status add_order(Order& order) noexcept
    {
        return sparse() ? sparse_side().add_order(order) : dense_side().add_order(order);
    }

    [[nodiscard]] Status remove_order(Order& order) noexcept
    {
        return sparse() ? sparse_side().remove_order(order) : dense_side().remove_order(order);
    }

    [[nodiscard]] Status reduce_order(Order& order, const Quantity new_quantity) noexcept
    {
        return sparse() ? sparse_side().reduce_order(order, new_quantity)
                        : dense_side().reduce_order(order, new_quantity);
    }

    [[nodiscard]] Quantity executable_quantity(const Quantity requested_quantity,
                                               const bool has_limit_price,
                                               const Price limit_price) const noexcept
    {
        return sparse() ? sparse_side().executable_quantity(requested_quantity, has_limit_price, limit_price)
                        : dense_side().executable_quantity(requested_quantity, has_limit_price, limit_price);
    }

    [[nodiscard]] MatchResult match(const Quantity requested_quantity,
                                    const bool has_limit_price,
                                    const Price limit_price,
                                    OrderIdMap& order_ids,
                                    MemoryPool<Order>& order_pool,
                                    EventLog& event_log,
                                    const OrderId aggressor_id,
                                    const Side aggressor_side,
                                    const Timestamp timestamp) noexcept
    {
        return sparse() ? sparse_side().match(requested_quantity,
                                             has_limit_price,
                                             limit_price,
                                             order_ids,
                                             order_pool,
                                             event_log,
                                             aggressor_id,
                                             aggressor_side,
                                             timestamp)
                        : dense_side().match(requested_quantity,
                                             has_limit_price,
                                             limit_price,
                                             order_ids,
                                             order_pool,
                                             event_log,
                                             aggressor_id,
                                             aggressor_side,
                                             timestamp);
    }

    [[nodiscard]] BestQuote best_quote() const noexcept
    {
        return sparse() ? sparse_side().best_quote() : dense_side().best_quote();
    }

    [[nodiscard]] std::uint32_t depth(const std::uint32_t max_levels,
                                      DepthLevel* const out_buffer) const noexcept
    {
        return sparse() ? sparse_side().depth(max_levels, out_buffer)
                        : dense_side().depth(max_levels, out_buffer);
    }

    [[nodiscard]] Quantity depth_at_price(const Price price) const noexcept
    {
        return sparse() ? sparse_side().depth_at_price(price) : dense_side().depth_at_price(price);
    }

    [[nodiscard]] std::uint32_t order_count_at_price(const Price price) const noexcept
    {
        return sparse() ? sparse_side().order_count_at_price(price) : dense_side().order_count_at_price(price);
    }

    [[nodiscard]] std::uint32_t level_count() const noexcept
    {
        return sparse() ? sparse_side().level_count() : dense_side().level_count();
    }

    [[nodiscard]] std::uint32_t occupied_level_count() const noexcept
    {
        return sparse() ? sparse_side().occupied_level_count() : dense_side().occupied_level_count();
    }

    template <typename Fn>
    void for_each_level(Fn&& fn) const noexcept
    {
        if (sparse()) {
            sparse_side().for_each_level(fn);
        } else {
            dense_side().for_each_level(fn);
        }
    }

    template <typename Fn>
    void for_each_order(Fn&& fn) const noexcept
    {
        if (sparse()) {
            sparse_side().for_each_order(fn);
        } else {
            dense_side().for_each_order(fn);
        }
    }

    [[nodiscard]] BookSideStats stats() const noexcept
    {
        return sparse() ? sparse_side().stats() : dense_side().stats();
    }

private:
    PriceLevelMode mode_{PriceLevelMode::Dense};
    std::variant<std::monostate, DenseBookSide, SparseBookSide> impl_{};

    [[nodiscard]] bool sparse() const noexcept
    {
        return mode_ == PriceLevelMode::Sparse;
    }

    [[nodiscard]] DenseBookSide& dense_side() noexcept
    {
        return *std::get_if<DenseBookSide>(&impl_);
    }

    [[nodiscard]] const DenseBookSide& dense_side() const noexcept
    {
        return *std::get_if<DenseBookSide>(&impl_);
    }

    [[nodiscard]] SparseBookSide& sparse_side() noexcept
    {
        return *std::get_if<SparseBookSide>(&impl_);
    }

    [[nodiscard]] const SparseBookSide& sparse_side() const noexcept
    {
        return *std::get_if<SparseBookSide>(&impl_);
    }
};

static_assert(alignof(BookSide) >= 64);

} // namespace eigenbook
