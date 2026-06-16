#pragma once

#include "BookSide.hpp"
#include "MemoryPool.hpp"
#include "Order.hpp"
#include "OrderIdMap.hpp"
#include "Types.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace eigenbook {

class alignas(64) OrderBook final {
public:
    explicit OrderBook(const BookConfig& config)
        : config_(normalize_config(config)),
          orders_(config_.max_orders),
          order_ids_(effective_order_id_capacity(config_)),
          bids_(Side::Buy, config_.min_price, config_.max_price, config_.tick_size),
          asks_(Side::Sell, config_.min_price, config_.max_price, config_.tick_size)
    {
    }

    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;
    OrderBook(OrderBook&&) = delete;
    OrderBook& operator=(OrderBook&&) = delete;

    [[nodiscard]] AddOrderResult add_limit_order(const OrderId id,
                                                 const Side side,
                                                 const Price price,
                                                 const Quantity quantity,
                                                 const Timestamp timestamp = 0) noexcept
    {
        AddOrderResult result{};
        result.accepted_quantity = quantity;

        const Status validation_status = validate_new_order(id, price, quantity);
        if (validation_status != Status::Accepted) {
            result.status = validation_status;
            return result;
        }

        const Status id_slot_status = order_ids_.can_insert(id);
        if (id_slot_status == Status::DuplicateOrderId || id_slot_status == Status::InvalidOrderId) {
            result.status = id_slot_status;
            return result;
        }

        BookSide& contra_side = side == Side::Buy ? asks_ : bids_;

        // Bounded preflight avoids partially executing an order that cannot
        // persist its residual in the fixed order pool or fixed id map.
        if ((id_slot_status != Status::Accepted || orders_.available() == 0U) &&
            contra_side.executable_quantity(quantity, true, price) < quantity) {
            result.status = residual_reject_status(id_slot_status);
            return result;
        }

        const MatchResult match_result = contra_side.match(quantity, true, price, order_ids_, orders_);
        result.executed_quantity = match_result.executed_quantity;
        result.fills = match_result.fills;
        result.has_last_price = match_result.has_last_price;
        result.last_price = match_result.last_price;

        if (match_result.status == Status::InternalError) {
            result.status = Status::InternalError;
            return result;
        }

        if (match_result.remaining_quantity == 0) {
            result.resting_quantity = 0;
            result.status = Status::Filled;
            return result;
        }

        if (id_slot_status != Status::Accepted) {
            result.status = id_slot_status;
            return result;
        }

        Order* order = orders_.allocate();
        if (order == nullptr) {
            result.status = Status::PoolExhausted;
            return result;
        }

        order->reset(id, side, price, match_result.remaining_quantity, timestamp, next_sequence());

        BookSide& same_side = side == Side::Buy ? bids_ : asks_;
        Status status = same_side.add_order(*order);
        if (status != Status::Accepted) {
            order->clear();
            orders_.release(order);
            result.status = status;
            return result;
        }

        status = order_ids_.insert(id, order);
        if (status != Status::Accepted) {
            static_cast<void>(same_side.remove_order(*order));
            order->clear();
            orders_.release(order);
            result.status = status;
            return result;
        }

        result.resting_quantity = order->quantity;
        result.status = result.executed_quantity == 0 ? Status::Accepted : Status::PartiallyFilled;
        return result;
    }

    [[nodiscard]] CancelResult cancel_order(const OrderId id) noexcept
    {
        CancelResult result{};
        Order* order = order_ids_.find(id);
        if (order == nullptr) {
            result.status = Status::UnknownOrderId;
            return result;
        }

        result.canceled_quantity = order->quantity;
        BookSide& side = order->side == Side::Buy ? bids_ : asks_;
        const Status remove_status = side.remove_order(*order);
        if (remove_status != Status::Accepted) {
            result.status = remove_status;
            return result;
        }

        static_cast<void>(order_ids_.erase(id));
        order->state = OrderState::Cancelled;
        order->clear();
        orders_.release(order);
        result.status = Status::Cancelled;
        return result;
    }

    [[nodiscard]] ModifyResult modify_order(const OrderId id, const Quantity new_quantity) noexcept
    {
        ModifyResult result{};
        if (new_quantity == 0) {
            result.status = Status::InvalidQuantity;
            return result;
        }

        Order* order = order_ids_.find(id);
        if (order == nullptr) {
            result.status = Status::UnknownOrderId;
            return result;
        }

        result.old_quantity = order->quantity;
        result.new_quantity = new_quantity;

        if (new_quantity == order->quantity) {
            result.status = Status::Accepted;
            return result;
        }

        if (new_quantity > order->quantity) {
            result.status = Status::QuantityIncreaseRejected;
            return result;
        }

        BookSide& side = order->side == Side::Buy ? bids_ : asks_;
        result.status = side.reduce_order(*order, new_quantity);
        return result;
    }

    [[nodiscard]] MatchResult match_market_order(const Side aggressor_side, const Quantity quantity) noexcept
    {
        if (quantity == 0) {
            MatchResult result{};
            result.status = Status::InvalidQuantity;
            return result;
        }

        BookSide& contra_side = aggressor_side == Side::Buy ? asks_ : bids_;
        return contra_side.match(quantity, false, 0, order_ids_, orders_);
    }

    [[nodiscard]] BestQuote best_bid() const noexcept
    {
        return bids_.best_quote();
    }

    [[nodiscard]] BestQuote best_ask() const noexcept
    {
        return asks_.best_quote();
    }

    [[nodiscard]] Quantity depth_at_price(const Side side, const Price price) const noexcept
    {
        return side == Side::Buy ? bids_.depth_at_price(price) : asks_.depth_at_price(price);
    }

    [[nodiscard]] std::uint32_t order_count_at_price(const Side side, const Price price) const noexcept
    {
        return side == Side::Buy ? bids_.order_count_at_price(price) : asks_.order_count_at_price(price);
    }

    [[nodiscard]] const Order* find_order(const OrderId id) const noexcept
    {
        return order_ids_.find(id);
    }

    [[nodiscard]] std::uint32_t live_order_count() const noexcept
    {
        return orders_.size();
    }

    [[nodiscard]] std::uint32_t order_capacity() const noexcept
    {
        return orders_.capacity();
    }

    [[nodiscard]] const BookConfig& config() const noexcept
    {
        return config_;
    }

private:
    BookConfig config_;
    MemoryPool<Order> orders_;
    OrderIdMap order_ids_;
    BookSide bids_;
    BookSide asks_;
    SequenceNumber next_sequence_{0};

    [[nodiscard]] static BookConfig normalize_config(const BookConfig& config) noexcept
    {
        BookConfig normalized = config;
        if (normalized.order_id_map_capacity == 0 && normalized.max_orders != 0) {
            normalized.order_id_map_capacity = saturated_double(normalized.max_orders);
        }
        return normalized;
    }

    [[nodiscard]] static std::uint32_t effective_order_id_capacity(const BookConfig& config) noexcept
    {
        return config.order_id_map_capacity == 0 ? saturated_double(config.max_orders) : config.order_id_map_capacity;
    }

    [[nodiscard]] static std::uint32_t saturated_double(const std::uint32_t value) noexcept
    {
        if (value > std::numeric_limits<std::uint32_t>::max() / 2U) {
            return std::numeric_limits<std::uint32_t>::max();
        }
        return std::max(2U, value * 2U);
    }

    [[nodiscard]] Status validate_new_order(const OrderId id, const Price price, const Quantity quantity) const noexcept
    {
        if (!config_.valid()) {
            return Status::InvalidConfiguration;
        }
        if (id == kInvalidOrderId) {
            return Status::InvalidOrderId;
        }
        if (quantity == 0) {
            return Status::InvalidQuantity;
        }
        if (price < config_.min_price || price > config_.max_price ||
            price_distance(config_.min_price, price) % static_cast<std::uint64_t>(config_.tick_size) != 0U) {
            return Status::InvalidPrice;
        }
        return Status::Accepted;
    }

    [[nodiscard]] Status residual_reject_status(const Status id_slot_status) const noexcept
    {
        if (orders_.available() == 0U) {
            return Status::PoolExhausted;
        }
        return id_slot_status;
    }

    [[nodiscard]] SequenceNumber next_sequence() noexcept
    {
        if (next_sequence_ != std::numeric_limits<SequenceNumber>::max()) {
            ++next_sequence_;
        }
        return next_sequence_;
    }
};

static_assert(alignof(OrderBook) >= 64);

} // namespace eigenbook
