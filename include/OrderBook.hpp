#pragma once

#include "BookSide.hpp"
#include "EventLog.hpp"
#include "MemoryPool.hpp"
#include "Order.hpp"
#include "OrderIdMap.hpp"
#include "Types.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace eigenbook {

class alignas(64) OrderBook final {
    friend struct SnapshotAccess;

public:
    explicit OrderBook(const BookConfig& config,
                       const InstrumentId instrument_id = kInvalidInstrumentId)
        : config_(normalize_config(config)),
          instrument_id_(instrument_id),
          orders_(config_.max_orders),
          order_ids_(effective_order_id_capacity(config_)),
          bids_(Side::Buy, config_),
          asks_(Side::Sell, config_),
          event_log_(config_.event_log_capacity, instrument_id_)
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
                                                 const Timestamp timestamp = 0,
                                                 const TimeInForce time_in_force = TimeInForce::Gtc) noexcept
    {
        AddOrderResult result{};
        result.accepted_quantity = quantity;
        event_log_.begin_operation(0);

        const Status validation_status = validate_new_order(id, price, quantity);
        if (validation_status != Status::Accepted) {
            result.status = validation_status;
            if (!start_event_operation(1)) {
                return event_log_full_result(result);
            }
            emit_order_rejected(id, side, price, quantity, timestamp, time_in_force, validation_status);
            return finish_result(result);
        }

        if (!valid_time_in_force(time_in_force)) {
            result.status = Status::Rejected;
            if (!start_event_operation(1)) {
                return event_log_full_result(result);
            }
            emit_order_rejected(id, side, price, quantity, timestamp, time_in_force, result.status);
            return finish_result(result);
        }

        const Status id_slot_status = order_ids_.can_insert(id);
        if (id_slot_status == Status::DuplicateOrderId || id_slot_status == Status::InvalidOrderId) {
            result.status = id_slot_status;
            if (!start_event_operation(1)) {
                return event_log_full_result(result);
            }
            emit_order_rejected(id, side, price, quantity, timestamp, time_in_force, id_slot_status);
            return finish_result(result);
        }

        BookSide& contra_side = side == Side::Buy ? asks_ : bids_;
        const Quantity executable_quantity = contra_side.executable_quantity(quantity, true, price);

        if (time_in_force == TimeInForce::Fok && executable_quantity < quantity) {
            result.status = Status::FokRejected;
            if (!start_event_operation(1)) {
                return event_log_full_result(result);
            }
            emit_order_rejected(id, side, price, quantity, timestamp, time_in_force, result.status);
            return finish_result(result);
        }

        // Bounded preflight avoids partially executing an order that cannot
        // persist its residual in the fixed order pool or fixed id map.
        if (time_in_force == TimeInForce::Gtc &&
            (id_slot_status != Status::Accepted || orders_.available() == 0U) &&
            executable_quantity < quantity) {
            result.status = residual_reject_status(id_slot_status);
            if (!start_event_operation(1)) {
                return event_log_full_result(result);
            }
            emit_order_rejected(id, side, price, quantity, timestamp, time_in_force, result.status);
            return finish_result(result);
        }

        const std::uint32_t required_event_count =
            required_limit_event_count(contra_side, quantity, executable_quantity, true, price);
        if (!start_event_operation(required_event_count)) {
            result.accepted_quantity = 0;
            return event_log_full_result(result);
        }

        event_log_.append_order(
            BookEvent::Kind::OrderAccepted, Status::Accepted, id, side, price, quantity, timestamp, time_in_force);

        const MatchResult match_result =
            contra_side.match(quantity, true, price, order_ids_, orders_, event_log_, id, side, timestamp);
        result.executed_quantity = match_result.executed_quantity;
        result.fills = match_result.fills;
        result.has_last_price = match_result.has_last_price;
        result.last_price = match_result.last_price;

        if (match_result.status == Status::InternalError) {
            result.status = Status::InternalError;
            return finish_result(result);
        }

        if (match_result.remaining_quantity == 0) {
            result.resting_quantity = 0;
            result.status = Status::Filled;
            return finish_result(result);
        }

        if (time_in_force == TimeInForce::Ioc) {
            result.resting_quantity = 0;
            result.status = result.executed_quantity == 0 ? Status::NoLiquidity : Status::PartiallyFilled;
            event_log_.append_order(BookEvent::Kind::OrderCancelled,
                                    Status::Cancelled,
                                    id,
                                    side,
                                    price,
                                    match_result.remaining_quantity,
                                    timestamp,
                                    time_in_force);
            return finish_result(result);
        }

        if (id_slot_status != Status::Accepted) {
            result.status = id_slot_status;
            return finish_result(result);
        }

        Order* order = orders_.allocate();
        if (order == nullptr) {
            result.status = Status::PoolExhausted;
            return finish_result(result);
        }

        order->reset(id, side, price, match_result.remaining_quantity, timestamp, next_sequence());

        BookSide& same_side = side == Side::Buy ? bids_ : asks_;
        Status status = same_side.add_order(*order);
        if (status != Status::Accepted) {
            order->clear();
            orders_.release(order);
            result.status = status;
            return finish_result(result);
        }

        status = order_ids_.insert(id, order);
        if (status != Status::Accepted) {
            static_cast<void>(same_side.remove_order(*order));
            order->clear();
            orders_.release(order);
            result.status = status;
            return finish_result(result);
        }

        result.resting_quantity = order->quantity;
        result.status = result.executed_quantity == 0 ? Status::Accepted : Status::PartiallyFilled;
        event_log_.append_order(BookEvent::Kind::OrderResting,
                                Status::Accepted,
                                id,
                                side,
                                price,
                                result.resting_quantity,
                                timestamp,
                                time_in_force);
        return finish_result(result);
    }

    [[nodiscard]] CancelResult cancel_order(const OrderId id, const Timestamp timestamp = 0) noexcept
    {
        CancelResult result{};
        event_log_.begin_operation(0);
        Order* order = order_ids_.find(id);
        if (order == nullptr) {
            result.status = Status::UnknownOrderId;
            if (!start_event_operation(1)) {
                return event_log_full_result(result);
            }
            emit_order_rejected(id, Side::Buy, 0, 0, timestamp, TimeInForce::Gtc, result.status);
            return finish_result(result);
        }

        if (!start_event_operation(1)) {
            return event_log_full_result(result);
        }

        result.canceled_quantity = order->quantity;
        const Side side_value = order->side;
        const Price price_value = order->price;
        BookSide& side = order->side == Side::Buy ? bids_ : asks_;
        const Status remove_status = side.remove_order(*order);
        if (remove_status != Status::Accepted) {
            result.status = remove_status;
            emit_order_rejected(id, side_value, price_value, result.canceled_quantity, timestamp, TimeInForce::Gtc, result.status);
            return finish_result(result);
        }

        static_cast<void>(order_ids_.erase(id));
        order->state = OrderState::Cancelled;
        order->clear();
        orders_.release(order);
        result.status = Status::Cancelled;
        event_log_.append_order(BookEvent::Kind::OrderCancelled,
                                Status::Cancelled,
                                id,
                                side_value,
                                price_value,
                                result.canceled_quantity,
                                timestamp);
        return finish_result(result);
    }

    [[nodiscard]] ModifyResult modify_order(const OrderId id,
                                            const Quantity new_quantity,
                                            const Timestamp timestamp = 0) noexcept
    {
        ModifyResult result{};
        event_log_.begin_operation(0);
        if (new_quantity == 0) {
            result.status = Status::InvalidQuantity;
            if (!start_event_operation(1)) {
                return event_log_full_result(result);
            }
            emit_order_rejected(id, Side::Buy, 0, new_quantity, timestamp, TimeInForce::Gtc, result.status);
            return finish_result(result);
        }

        Order* order = order_ids_.find(id);
        if (order == nullptr) {
            result.status = Status::UnknownOrderId;
            if (!start_event_operation(1)) {
                return event_log_full_result(result);
            }
            emit_order_rejected(id, Side::Buy, 0, new_quantity, timestamp, TimeInForce::Gtc, result.status);
            return finish_result(result);
        }

        if (!start_event_operation(1)) {
            return event_log_full_result(result);
        }

        result.old_quantity = order->quantity;
        result.new_quantity = new_quantity;
        const Side side_value = order->side;
        const Price price_value = order->price;

        if (new_quantity == order->quantity) {
            result.status = Status::Accepted;
            event_log_.append_order(BookEvent::Kind::OrderModified,
                                    Status::Accepted,
                                    id,
                                    side_value,
                                    price_value,
                                    new_quantity,
                                    timestamp,
                                    TimeInForce::Gtc,
                                    result.old_quantity,
                                    result.new_quantity);
            return finish_result(result);
        }

        if (new_quantity > order->quantity) {
            result.status = Status::QuantityIncreaseRejected;
            event_log_.append_order(BookEvent::Kind::OrderRejected,
                                    result.status,
                                    id,
                                    side_value,
                                    price_value,
                                    new_quantity,
                                    timestamp,
                                    TimeInForce::Gtc,
                                    result.old_quantity,
                                    result.new_quantity);
            return finish_result(result);
        }

        BookSide& side = order->side == Side::Buy ? bids_ : asks_;
        result.status = side.reduce_order(*order, new_quantity);
        event_log_.append_order(result.status == Status::Accepted ? BookEvent::Kind::OrderModified
                                                                  : BookEvent::Kind::OrderRejected,
                                result.status,
                                id,
                                side_value,
                                price_value,
                                new_quantity,
                                timestamp,
                                TimeInForce::Gtc,
                                result.old_quantity,
                                result.new_quantity);
        return finish_result(result);
    }

    /// Replace a resting order.
    ///
    /// Same-price reductions keep FIFO priority. Price changes or quantity
    /// increases cancel and re-enter the order, so the replacement loses time
    /// priority before matching or resting. IOC/FOK apply only to that
    /// lose-priority replacement path. `Status::EventLogFull` emits no events
    /// and leaves book state unchanged.
    [[nodiscard]] ReplaceResult replace_order(const OrderId id,
                                              const Price new_price,
                                              const Quantity new_quantity,
                                              const TimeInForce time_in_force = TimeInForce::Gtc) noexcept
    {
        return replace_order(id, new_price, new_quantity, 0, time_in_force);
    }

    /// Replace a resting order with an explicit event timestamp.
    [[nodiscard]] ReplaceResult replace_order(const OrderId id,
                                              const Price new_price,
                                              const Quantity new_quantity,
                                              const Timestamp timestamp,
                                              const TimeInForce time_in_force) noexcept
    {
        ReplaceResult result{};
        result.new_price = new_price;
        result.new_quantity = new_quantity;
        event_log_.begin_operation(0);

        Order* order = order_ids_.find(id);
        if (order == nullptr) {
            result.status = Status::UnknownOrderId;
            if (!start_event_operation(1)) {
                return event_log_full_result(result);
            }
            emit_order_rejected(id, Side::Buy, new_price, new_quantity, timestamp, time_in_force, result.status);
            return finish_result(result);
        }

        result.old_price = order->price;
        result.old_quantity = order->quantity;
        const Side side_value = order->side;
        const Price old_price = order->price;
        const Quantity old_quantity = order->quantity;

        const Status validation_status = validate_replacement_order(new_price, new_quantity);
        if (validation_status != Status::Accepted) {
            result.status = validation_status;
            if (!start_event_operation(1)) {
                return event_log_full_result(result);
            }
            event_log_.append_order(BookEvent::Kind::OrderRejected,
                                    result.status,
                                    id,
                                    side_value,
                                    new_price,
                                    new_quantity,
                                    timestamp,
                                    time_in_force,
                                    old_quantity,
                                    new_quantity);
            return finish_result(result);
        }

        if (!valid_time_in_force(time_in_force)) {
            result.status = Status::Rejected;
            if (!start_event_operation(1)) {
                return event_log_full_result(result);
            }
            event_log_.append_order(BookEvent::Kind::OrderRejected,
                                    result.status,
                                    id,
                                    side_value,
                                    new_price,
                                    new_quantity,
                                    timestamp,
                                    time_in_force,
                                    old_quantity,
                                    new_quantity);
            return finish_result(result);
        }

        if (new_price == old_price && new_quantity <= old_quantity) {
            if (!start_event_operation(1)) {
                return event_log_full_result(result);
            }

            if (new_quantity == old_quantity) {
                result.status = Status::Accepted;
                result.resting_quantity = old_quantity;
                event_log_.append_order(BookEvent::Kind::OrderModified,
                                        Status::Accepted,
                                        id,
                                        side_value,
                                        old_price,
                                        new_quantity,
                                        timestamp,
                                        TimeInForce::Gtc,
                                        old_quantity,
                                        new_quantity);
                return finish_result(result);
            }

            BookSide& side = side_value == Side::Buy ? bids_ : asks_;
            result.status = side.reduce_order(*order, new_quantity);
            if (result.status == Status::Accepted) {
                result.resting_quantity = new_quantity;
            }
            event_log_.append_order(result.status == Status::Accepted ? BookEvent::Kind::OrderModified
                                                                      : BookEvent::Kind::OrderRejected,
                                    result.status,
                                    id,
                                    side_value,
                                    old_price,
                                    new_quantity,
                                    timestamp,
                                    TimeInForce::Gtc,
                                    old_quantity,
                                    new_quantity);
            return finish_result(result);
        }

        BookSide& same_side = side_value == Side::Buy ? bids_ : asks_;
        BookSide& contra_side = side_value == Side::Buy ? asks_ : bids_;
        const Quantity executable = contra_side.executable_quantity(new_quantity, true, new_price);
        if (time_in_force == TimeInForce::Fok && executable < new_quantity) {
            result.status = Status::FokRejected;
            if (!start_event_operation(1)) {
                return event_log_full_result(result);
            }
            event_log_.append_order(BookEvent::Kind::OrderRejected,
                                    result.status,
                                    id,
                                    side_value,
                                    new_price,
                                    new_quantity,
                                    timestamp,
                                    time_in_force,
                                    old_quantity,
                                    new_quantity);
            return finish_result(result);
        }

        if (time_in_force == TimeInForce::Gtc && executable < new_quantity) {
            const Quantity residual_quantity = new_quantity - executable;
            const Status storage_status =
                same_side.can_accept_replacement_residual(*order, new_price, residual_quantity);
            if (storage_status != Status::Accepted) {
                result.status = storage_status;
                if (!start_event_operation(1)) {
                    return event_log_full_result(result);
                }
                event_log_.append_order(BookEvent::Kind::OrderRejected,
                                        result.status,
                                        id,
                                        side_value,
                                        new_price,
                                        new_quantity,
                                        timestamp,
                                        time_in_force,
                                        old_quantity,
                                        new_quantity);
                return finish_result(result);
            }
        }

        const std::uint32_t required_event_count =
            required_replace_event_count(contra_side, new_quantity, executable, true, new_price);
        if (!start_event_operation(required_event_count)) {
            return event_log_full_result(result);
        }

        const Status remove_status = same_side.remove_order(*order);
        if (remove_status != Status::Accepted) {
            result.status = remove_status;
            event_log_.append_order(BookEvent::Kind::OrderRejected,
                                    result.status,
                                    id,
                                    side_value,
                                    old_price,
                                    old_quantity,
                                    timestamp,
                                    TimeInForce::Gtc,
                                    old_quantity,
                                    new_quantity);
            return finish_result(result);
        }

        static_cast<void>(order_ids_.erase(id));
        event_log_.append_order(
            BookEvent::Kind::OrderCancelled, Status::Cancelled, id, side_value, old_price, old_quantity, timestamp);
        event_log_.append_order(BookEvent::Kind::OrderAccepted,
                                Status::Accepted,
                                id,
                                side_value,
                                new_price,
                                new_quantity,
                                timestamp,
                                time_in_force,
                                old_quantity,
                                new_quantity);

        const MatchResult match_result =
            contra_side.match(new_quantity, true, new_price, order_ids_, orders_, event_log_, id, side_value, timestamp);
        result.executed_quantity = match_result.executed_quantity;
        result.fills = match_result.fills;
        result.has_last_price = match_result.has_last_price;
        result.last_price = match_result.last_price;

        if (match_result.status == Status::InternalError) {
            order->clear();
            orders_.release(order);
            result.status = Status::InternalError;
            return finish_result(result);
        }

        if (match_result.remaining_quantity == 0) {
            order->clear();
            orders_.release(order);
            result.resting_quantity = 0;
            result.status = Status::Filled;
            return finish_result(result);
        }

        if (time_in_force == TimeInForce::Ioc) {
            order->clear();
            orders_.release(order);
            result.resting_quantity = 0;
            result.status = result.executed_quantity == 0 ? Status::NoLiquidity : Status::PartiallyFilled;
            event_log_.append_order(BookEvent::Kind::OrderCancelled,
                                    Status::Cancelled,
                                    id,
                                    side_value,
                                    new_price,
                                    match_result.remaining_quantity,
                                    timestamp,
                                    time_in_force,
                                    old_quantity,
                                    new_quantity);
            return finish_result(result);
        }

        order->reset(id, side_value, new_price, match_result.remaining_quantity, timestamp, next_sequence());
        Status status = same_side.add_order(*order);
        if (status != Status::Accepted) {
            order->clear();
            orders_.release(order);
            result.status = status;
            return finish_result(result);
        }

        status = order_ids_.insert(id, order);
        if (status != Status::Accepted) {
            static_cast<void>(same_side.remove_order(*order));
            order->clear();
            orders_.release(order);
            result.status = status;
            return finish_result(result);
        }

        result.resting_quantity = order->quantity;
        result.status = result.executed_quantity == 0 ? Status::Accepted : Status::PartiallyFilled;
        event_log_.append_order(BookEvent::Kind::OrderResting,
                                Status::Accepted,
                                id,
                                side_value,
                                new_price,
                                result.resting_quantity,
                                timestamp,
                                time_in_force,
                                old_quantity,
                                new_quantity);
        return finish_result(result);
    }

    [[nodiscard]] MatchResult match_market_order(const Side aggressor_side,
                                                 const Quantity quantity,
                                                 const OrderId aggressor_id = kInvalidOrderId,
                                                 const Timestamp timestamp = 0) noexcept
    {
        event_log_.begin_operation(0);
        if (quantity == 0) {
            MatchResult result{};
            result.status = Status::InvalidQuantity;
            if (!start_event_operation(1)) {
                return event_log_full_result(result);
            }
            emit_order_rejected(aggressor_id, aggressor_side, 0, quantity, timestamp, TimeInForce::Gtc, result.status);
            return finish_result(result);
        }

        BookSide& contra_side = aggressor_side == Side::Buy ? asks_ : bids_;
        const std::uint32_t required_event_count = contra_side.executable_fill_count(quantity, false, 0);
        if (!start_event_operation(required_event_count)) {
            MatchResult result{};
            result.status = Status::EventLogFull;
            result.requested_quantity = quantity;
            result.remaining_quantity = quantity;
            return finish_result(result);
        }

        MatchResult result =
            contra_side.match(quantity, false, 0, order_ids_, orders_, event_log_, aggressor_id, aggressor_side, timestamp);
        return finish_result(result);
    }

    [[nodiscard]] BestQuote best_bid() const noexcept
    {
        return bids_.best_quote();
    }

    [[nodiscard]] BestQuote best_ask() const noexcept
    {
        return asks_.best_quote();
    }

    [[nodiscard]] TopOfBook top_of_book() const noexcept
    {
        return TopOfBook{Status::Accepted, best_bid(), best_ask()};
    }

    [[nodiscard]] Quantity depth_at_price(const Side side, const Price price) const noexcept
    {
        return side == Side::Buy ? bids_.depth_at_price(price) : asks_.depth_at_price(price);
    }

    [[nodiscard]] std::uint32_t depth(const Side side,
                                      const std::uint32_t max_levels,
                                      DepthLevel* const out_buffer) const noexcept
    {
        return side == Side::Buy ? bids_.depth(max_levels, out_buffer) : asks_.depth(max_levels, out_buffer);
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

    [[nodiscard]] InstrumentId instrument_id() const noexcept
    {
        return instrument_id_;
    }

    [[nodiscard]] std::span<const BookEvent> last_events() const noexcept
    {
        return event_log_.last_events();
    }

    [[nodiscard]] std::uint32_t event_log_capacity() const noexcept
    {
        return event_log_.capacity();
    }

    [[nodiscard]] OrderBookStats stats() const noexcept
    {
        const OrderIdMapStats id_stats = order_ids_.stats();
        return OrderBookStats{orders_.size(),
                              orders_.capacity(),
                              utilization(orders_.size(), orders_.capacity()),
                              utilization(id_stats.size, id_stats.capacity),
                              id_stats,
                              bids_.stats(),
                              asks_.stats()};
    }

private:
    BookConfig config_;
    InstrumentId instrument_id_{kInvalidInstrumentId};
    MemoryPool<Order> orders_;
    OrderIdMap order_ids_;
    BookSide bids_;
    BookSide asks_;
    EventLog event_log_;
    SequenceNumber next_sequence_{0};

    [[nodiscard]] static BookConfig normalize_config(const BookConfig& config) noexcept
    {
        BookConfig normalized = config;
        if (normalized.order_id_map_capacity == 0 && normalized.max_orders != 0) {
            normalized.order_id_map_capacity = saturated_double(normalized.max_orders);
        }
        if (normalized.event_log_capacity == 0) {
            normalized.event_log_capacity = minimum_event_log_capacity(normalized.max_orders);
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

    [[nodiscard]] static std::uint32_t minimum_event_log_capacity(const std::uint32_t max_orders) noexcept
    {
        constexpr std::uint32_t kMinimumCapacity = 8;
        constexpr std::uint32_t kOperationSlack = 2;
        if (max_orders > std::numeric_limits<std::uint32_t>::max() - kOperationSlack) {
            return std::numeric_limits<std::uint32_t>::max();
        }

        return std::max(kMinimumCapacity, max_orders + kOperationSlack);
    }

    [[nodiscard]] static double utilization(const std::uint32_t used,
                                            const std::uint32_t capacity) noexcept
    {
        return capacity == 0 ? 0.0 : static_cast<double>(used) / static_cast<double>(capacity);
    }

    [[nodiscard]] static bool valid_time_in_force(const TimeInForce time_in_force) noexcept
    {
        return time_in_force == TimeInForce::Gtc || time_in_force == TimeInForce::Ioc ||
               time_in_force == TimeInForce::Fok;
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

    [[nodiscard]] Status validate_replacement_order(const Price price, const Quantity quantity) const noexcept
    {
        if (!config_.valid()) {
            return Status::InvalidConfiguration;
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

    [[nodiscard]] std::uint32_t max_events_for_add() const noexcept
    {
        if (orders_.size() > std::numeric_limits<std::uint32_t>::max() - 2U) {
            return std::numeric_limits<std::uint32_t>::max();
        }
        return orders_.size() + 2U;
    }

    [[nodiscard]] std::uint32_t max_events_for_replace() const noexcept
    {
        if (orders_.size() > std::numeric_limits<std::uint32_t>::max() - 2U) {
            return std::numeric_limits<std::uint32_t>::max();
        }
        return orders_.size() + 2U;
    }

    [[nodiscard]] std::uint32_t max_events_for_market() const noexcept
    {
        return std::max(1U, orders_.size());
    }

    [[nodiscard]] bool start_event_operation(const std::uint32_t required_event_count) noexcept
    {
        if (!event_log_.can_record(required_event_count)) {
            event_log_.begin_operation(0);
            return false;
        }

        event_log_.begin_operation(required_event_count);
        return true;
    }

    template <typename Result>
    [[nodiscard]] Result event_log_full_result(Result result) noexcept
    {
        result.status = Status::EventLogFull;
        return finish_result(result);
    }

    [[nodiscard]] static std::uint32_t saturated_add(const std::uint32_t lhs,
                                                     const std::uint32_t rhs) noexcept
    {
        return rhs > std::numeric_limits<std::uint32_t>::max() - lhs
                   ? std::numeric_limits<std::uint32_t>::max()
                   : lhs + rhs;
    }

    [[nodiscard]] static std::uint32_t residual_event_count(const Quantity requested_quantity,
                                                            const Quantity executable_quantity) noexcept
    {
        return executable_quantity < requested_quantity ? 1U : 0U;
    }

    [[nodiscard]] static std::uint32_t required_limit_event_count(const BookSide& contra_side,
                                                                  const Quantity requested_quantity,
                                                                  const Quantity executable_quantity,
                                                                  const bool has_limit_price,
                                                                  const Price limit_price) noexcept
    {
        std::uint32_t required = 1U;
        required = saturated_add(
            required, contra_side.executable_fill_count(requested_quantity, has_limit_price, limit_price));
        return saturated_add(required, residual_event_count(requested_quantity, executable_quantity));
    }

    [[nodiscard]] static std::uint32_t required_replace_event_count(const BookSide& contra_side,
                                                                    const Quantity requested_quantity,
                                                                    const Quantity executable_quantity,
                                                                    const bool has_limit_price,
                                                                    const Price limit_price) noexcept
    {
        std::uint32_t required = 2U;
        required = saturated_add(
            required, contra_side.executable_fill_count(requested_quantity, has_limit_price, limit_price));
        return saturated_add(required, residual_event_count(requested_quantity, executable_quantity));
    }

    void emit_order_rejected(const OrderId id,
                             const Side side,
                             const Price price,
                             const Quantity quantity,
                             const Timestamp timestamp,
                             const TimeInForce time_in_force,
                             const Status reason) noexcept
    {
        event_log_.append_order(
            BookEvent::Kind::OrderRejected, reason, id, side, price, quantity, timestamp, time_in_force);
    }

    template <typename Result>
    [[nodiscard]] Result finish_result(Result result) noexcept
    {
        result.events_emitted = event_log_.last_count();
        result.events = event_log_.last_events();
        return result;
    }

    [[nodiscard]] SequenceNumber next_sequence() noexcept
    {
        if (next_sequence_ != std::numeric_limits<SequenceNumber>::max()) {
            ++next_sequence_;
        }
        return next_sequence_;
    }

    [[nodiscard]] SequenceNumber snapshot_next_sequence() const noexcept
    {
        return next_sequence_;
    }

    [[nodiscard]] SequenceNumber snapshot_event_next_sequence() const noexcept
    {
        return event_log_.next_sequence_value();
    }

    [[nodiscard]] std::uint32_t snapshot_level_count() const noexcept
    {
        return bids_.occupied_level_count() + asks_.occupied_level_count();
    }

    template <typename Fn>
    void snapshot_for_each_order(Fn&& fn) const noexcept
    {
        bids_.for_each_order(fn);
        asks_.for_each_order(fn);
    }

    template <typename Fn>
    void snapshot_for_each_level(Fn&& fn) const noexcept
    {
        bids_.for_each_level([&fn](const PriceLevel& level) noexcept {
            fn(BookSnapshotLevelAggregate{Side::Buy, level.price(), level.total_quantity(), level.order_count()});
        });
        asks_.for_each_level([&fn](const PriceLevel& level) noexcept {
            fn(BookSnapshotLevelAggregate{Side::Sell, level.price(), level.total_quantity(), level.order_count()});
        });
    }

    void clear_for_snapshot_restore() noexcept
    {
        bids_.clear();
        asks_.clear();
        order_ids_.clear();
        orders_.clear();
        next_sequence_ = 0;
        event_log_.reset();
    }

    [[nodiscard]] Status restore_snapshot_order(const BookSnapshotOrder& record) noexcept
    {
        Order* order = orders_.allocate();
        if (order == nullptr) {
            return Status::PoolExhausted;
        }

        order->reset(record.id, record.side, record.price, record.quantity, record.timestamp, record.sequence);
        BookSide& side = record.side == Side::Buy ? bids_ : asks_;
        Status status = side.add_order(*order);
        if (status != Status::Accepted) {
            order->clear();
            orders_.release(order);
            return status;
        }

        status = order_ids_.insert(record.id, order);
        if (status != Status::Accepted) {
            static_cast<void>(side.remove_order(*order));
            order->clear();
            orders_.release(order);
            return status;
        }

        return Status::Accepted;
    }

    void finish_snapshot_restore(const SequenceNumber next_sequence,
                                 const SequenceNumber event_next_sequence) noexcept
    {
        next_sequence_ = next_sequence;
        event_log_.reset(event_next_sequence);
    }
};

static_assert(alignof(OrderBook) >= 64);

} // namespace eigenbook
