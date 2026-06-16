#pragma once

#include "MemoryPool.hpp"
#include "OrderIdMap.hpp"
#include "PriceLevel.hpp"
#include "Types.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>

namespace eigenbook {

class alignas(64) BookSide final {
public:
    BookSide(const Side side, const Price min_price, const Price max_price, const Price tick_size)
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

    BookSide(const BookSide&) = delete;
    BookSide& operator=(const BookSide&) = delete;
    BookSide(BookSide&&) = delete;
    BookSide& operator=(BookSide&&) = delete;

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
                                    MemoryPool<Order>& order_pool) noexcept
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

                if (resting_order->quantity == 0) {
                    const OrderId resting_id = resting_order->id;
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
};

static_assert(alignof(BookSide) >= 64);

} // namespace eigenbook
