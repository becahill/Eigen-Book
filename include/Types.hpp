#pragma once

#include <cstdint>
#include <limits>

namespace eigenbook {

using OrderId = std::uint64_t;
using Price = std::int64_t;
using Quantity = std::uint64_t;
using Timestamp = std::uint64_t;
using SequenceNumber = std::uint64_t;

inline constexpr OrderId kInvalidOrderId = 0;
inline constexpr std::uint32_t kInvalidIndex = std::numeric_limits<std::uint32_t>::max();

enum class Side : std::uint8_t {
    Buy,
    Sell,
};

enum class OrderState : std::uint8_t {
    Inactive,
    Resting,
    PartiallyFilled,
    Filled,
    Cancelled,
};

enum class Status : std::uint8_t {
    Accepted,
    Rejected,
    Cancelled,
    Filled,
    PartiallyFilled,
    NoLiquidity,
    InvalidOrderId,
    UnknownOrderId,
    InvalidQuantity,
    InvalidPrice,
    DuplicateOrderId,
    PoolExhausted,
    OrderIdMapFull,
    QuantityIncreaseRejected,
    InvalidConfiguration,
    InternalError,

    // Compatibility aliases for older call sites.
    Ok = Accepted,
    OrderNotFound = UnknownOrderId,
};

[[nodiscard]] constexpr std::uint64_t price_distance(const Price lower, const Price upper) noexcept
{
    return static_cast<std::uint64_t>(upper) - static_cast<std::uint64_t>(lower);
}

struct BookConfig final {
    Price min_price{0};
    Price max_price{0};
    std::uint32_t max_orders{0};
    std::uint32_t order_id_map_capacity{0};
    Price tick_size{1};

    [[nodiscard]] bool valid() const noexcept
    {
        if (min_price > max_price || max_orders == 0 || tick_size <= 0) {
            return false;
        }

        const std::uint64_t range = price_distance(min_price, max_price);
        if (range > static_cast<std::uint64_t>(std::numeric_limits<Price>::max())) {
            return false;
        }

        const auto tick = static_cast<std::uint64_t>(tick_size);
        if (range % tick != 0U) {
            return false;
        }

        const std::uint64_t intervals = range / tick;
        return intervals < static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max());
    }

    [[nodiscard]] std::uint32_t price_level_count() const noexcept
    {
        if (!valid()) {
            return 0;
        }

        const std::uint64_t range = price_distance(min_price, max_price);
        const auto tick = static_cast<std::uint64_t>(tick_size);
        return static_cast<std::uint32_t>((range / tick) + 1U);
    }
};

struct BestQuote final {
    bool valid{false};
    Price price{0};
    Quantity quantity{0};
    std::uint32_t order_count{0};
};

struct AddOrderResult final {
    Status status{Status::InternalError};
    Quantity accepted_quantity{0};
    Quantity executed_quantity{0};
    Quantity resting_quantity{0};
    std::uint32_t fills{0};
    bool has_last_price{false};
    Price last_price{0};
};

struct CancelResult final {
    Status status{Status::InternalError};
    Quantity canceled_quantity{0};
};

struct ModifyResult final {
    Status status{Status::InternalError};
    Quantity old_quantity{0};
    Quantity new_quantity{0};
};

struct MatchResult final {
    Status status{Status::InternalError};
    Quantity requested_quantity{0};
    Quantity executed_quantity{0};
    Quantity remaining_quantity{0};
    std::uint32_t fills{0};
    bool has_last_price{false};
    Price last_price{0};
};

[[nodiscard]] constexpr bool is_buy(const Side side) noexcept
{
    return side == Side::Buy;
}

[[nodiscard]] constexpr bool is_sell(const Side side) noexcept
{
    return side == Side::Sell;
}

} // namespace eigenbook
