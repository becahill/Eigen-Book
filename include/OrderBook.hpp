#pragma once

#include "BookSide.hpp"
#include "EventLog.hpp"
#include "MarketData.hpp"
#include "MemoryPool.hpp"
#include "Order.hpp"
#include "OrderIdMap.hpp"
#include "SnapshotValidationWorkspace.hpp"
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
          event_log_(config_.event_log_capacity, instrument_id_),
          market_data_(config_.market_data_capacity, instrument_id_),
          snapshot_validation_(config_.max_orders)
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
                                                 const TimeInForce time_in_force = TimeInForce::Gtc,
                                                 const ParticipantId participant_id = kAnonymousParticipantId,
                                                 const bool post_only = false) noexcept
    {
        AddOrderResult result{};
        begin_operation();
        if (side != Side::Buy && side != Side::Sell) {
            result.status = Status::InvalidCommand;
            return finish_result(result);
        }

        result.accepted_quantity = quantity;

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

        if (post_only && time_in_force != TimeInForce::Gtc) {
            result.status = Status::InvalidPostOnlyTimeInForce;
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

        BookSide& same_side = side == Side::Buy ? bids_ : asks_;
        BookSide& contra_side = side == Side::Buy ? asks_ : bids_;
        const Quantity crossing_quantity = contra_side.executable_quantity(quantity, true, price);
        if (post_only && crossing_quantity != 0) {
            result.status = Status::PostOnlyWouldCross;
            if (!start_event_operation(1)) {
                return event_log_full_result(result);
            }
            emit_order_rejected(id, side, price, quantity, timestamp, time_in_force, result.status);
            return finish_result(result);
        }

        MatchPreflight preflight{};
        if (config_.self_trade_policy == SelfTradePolicy::Disabled) {
            preflight.executable_quantity = crossing_quantity;
            preflight.fill_count =
                contra_side.executable_fill_count(quantity, true, price);
        } else {
            preflight =
                contra_side.preflight(quantity, true, price, participant_id, config_.self_trade_policy);
        }
        const Quantity executable_quantity = preflight.executable_quantity;

        if (time_in_force == TimeInForce::Fok &&
            (executable_quantity < quantity || preflight.aggressor_cancelled)) {
            result.status = Status::FokRejected;
            if (!start_event_operation(1)) {
                return event_log_full_result(result);
            }
            emit_order_rejected(id, side, price, quantity, timestamp, time_in_force, result.status);
            return finish_result(result);
        }

        if (time_in_force == TimeInForce::Gtc && executable_quantity < quantity &&
            !preflight.aggressor_cancelled) {
            const Quantity residual_quantity = quantity - executable_quantity;
            const Status level_status =
                same_side.can_accept_residual(price, residual_quantity);
            if (level_status != Status::Accepted) {
                result.accepted_quantity = 0;
                result.status = level_status;
                if (!start_event_operation(1)) {
                    return event_log_full_result(result);
                }
                emit_preflight_order_rejected(id,
                                              side,
                                              price,
                                              quantity,
                                              timestamp,
                                              time_in_force,
                                              level_status);
                return finish_result(result);
            }
        }

        // Bounded preflight avoids partially executing an order that cannot
        // persist its residual in the fixed order pool or fixed id map.
        if (time_in_force == TimeInForce::Gtc &&
            (id_slot_status != Status::Accepted || orders_.available() == 0U) &&
            executable_quantity < quantity && !preflight.aggressor_cancelled) {
            result.status = residual_reject_status(id_slot_status);
            if (!start_event_operation(1)) {
                return event_log_full_result(result);
            }
            emit_order_rejected(id, side, price, quantity, timestamp, time_in_force, result.status);
            return finish_result(result);
        }

        const std::uint32_t required_event_count =
            required_limit_event_count(preflight, quantity, executable_quantity);
        const std::uint32_t required_market_data_count =
            required_limit_market_data_count(preflight, quantity, executable_quantity);
        const Status capacity_status = start_operation(required_event_count, required_market_data_count);
        if (capacity_status != Status::Accepted) {
            result.accepted_quantity = 0;
            result.status = capacity_status;
            return finish_result(result);
        }

        event_log_.append_order(
            BookEvent::Kind::OrderAccepted, Status::Accepted, id, side, price, quantity, timestamp, time_in_force);

        const MatchResult match_result =
            contra_side.match(quantity,
                              true,
                              price,
                              order_ids_,
                              orders_,
                              event_log_,
                              id,
                              side,
                              timestamp,
                              participant_id,
                              config_.self_trade_policy,
                              active_market_data());
        result.executed_quantity = match_result.executed_quantity;
        result.fills = match_result.fills;
        result.has_last_price = match_result.has_last_price;
        result.last_price = match_result.last_price;
        result.aggressor_cancelled_by_stp = match_result.aggressor_cancelled_by_stp;
        result.resting_orders_cancelled_by_stp =
            match_result.resting_orders_cancelled_by_stp;

        if (match_result.status == Status::InternalError) {
            result.status = Status::InternalError;
            return finish_result(result);
        }

        if (match_result.remaining_quantity == 0) {
            result.resting_quantity = 0;
            result.status = Status::Filled;
            return finish_result(result);
        }

        if (match_result.aggressor_cancelled_by_stp) {
            result.resting_quantity = 0;
            result.status =
                result.executed_quantity == 0 ? Status::SelfTradePrevented : Status::PartiallyFilled;
            event_log_.append_order(BookEvent::Kind::OrderCancelled,
                                    Status::SelfTradePrevented,
                                    id,
                                    side,
                                    price,
                                    match_result.remaining_quantity,
                                    timestamp,
                                    time_in_force);
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

        order->reset(id,
                     side,
                     price,
                     match_result.remaining_quantity,
                     timestamp,
                     next_sequence(),
                     participant_id,
                     post_only);

        Quantity previous_level_quantity = 0;
        BestQuote previous_best{};
        if (market_data_.enabled()) {
            previous_level_quantity = same_side.depth_at_price(price);
            previous_best = same_side.best_quote();
        }
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

        if (market_data_.enabled()) {
            emit_market_data_level_change(&market_data_,
                                          side,
                                          price,
                                          previous_level_quantity,
                                          same_side.depth_at_price(price),
                                          same_side.order_count_at_price(price),
                                          previous_best,
                                          same_side.best_quote(),
                                          timestamp);
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
        begin_operation();
        const OrderIdMap::EraseToken erase_token = order_ids_.find_for_erase(id);
        Order* const order = erase_token.order();
        if (order == nullptr) {
            result.status = Status::UnknownOrderId;
            if (!start_event_operation(1)) {
                return event_log_full_result(result);
            }
            emit_order_rejected(id, Side::Buy, 0, 0, timestamp, TimeInForce::Gtc, result.status);
            return finish_result(result);
        }

        const Status capacity_status = start_operation(1, required_single_level_market_data_count());
        if (capacity_status != Status::Accepted) {
            result.status = capacity_status;
            return finish_result(result);
        }

        result.canceled_quantity = order->quantity;
        const Side side_value = order->side;
        const Price price_value = order->price;
        BookSide& side = order->side == Side::Buy ? bids_ : asks_;
        Quantity previous_level_quantity = 0;
        BestQuote previous_best{};
        if (market_data_.enabled()) {
            previous_level_quantity = side.depth_at_price(price_value);
            previous_best = side.best_quote();
        }
        const Status remove_status = side.remove_order(*order);
        if (remove_status != Status::Accepted) {
            result.status = remove_status;
            emit_order_rejected(id, side_value, price_value, result.canceled_quantity, timestamp, TimeInForce::Gtc, result.status);
            return finish_result(result);
        }

        static_cast<void>(order_ids_.erase(erase_token));
        order->state = OrderState::Cancelled;
        order->clear();
        orders_.release(order);
        if (market_data_.enabled()) {
            emit_market_data_level_change(&market_data_,
                                          side_value,
                                          price_value,
                                          previous_level_quantity,
                                          side.depth_at_price(price_value),
                                          side.order_count_at_price(price_value),
                                          previous_best,
                                          side.best_quote(),
                                          timestamp);
        }
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
        begin_operation();
        if (!valid_lot_quantity(new_quantity)) {
            result.status = new_quantity == 0 ? Status::InvalidQuantity : Status::LotSizeViolation;
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

        const Status capacity_status =
            start_operation(1,
                            new_quantity < order->quantity ? required_single_level_market_data_count()
                                                           : 0U);
        if (capacity_status != Status::Accepted) {
            result.status = capacity_status;
            return finish_result(result);
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
        Quantity previous_level_quantity = 0;
        BestQuote previous_best{};
        if (market_data_.enabled()) {
            previous_level_quantity = side.depth_at_price(price_value);
            previous_best = side.best_quote();
        }
        result.status = side.reduce_order(*order, new_quantity);
        if (result.status == Status::Accepted && market_data_.enabled()) {
            emit_market_data_level_change(&market_data_,
                                          side_value,
                                          price_value,
                                          previous_level_quantity,
                                          side.depth_at_price(price_value),
                                          side.order_count_at_price(price_value),
                                          previous_best,
                                          side.best_quote(),
                                          timestamp);
        }
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
                                              const TimeInForce time_in_force = TimeInForce::Gtc,
                                              const bool post_only = false) noexcept
    {
        return replace_order(id, new_price, new_quantity, 0, time_in_force, post_only);
    }

    /// Replace a resting order with an explicit event timestamp.
    [[nodiscard]] ReplaceResult replace_order(const OrderId id,
                                              const Price new_price,
                                              const Quantity new_quantity,
                                              const Timestamp timestamp,
                                              const TimeInForce time_in_force,
                                              const bool post_only = false) noexcept
    {
        ReplaceResult result{};
        result.new_price = new_price;
        result.new_quantity = new_quantity;
        begin_operation();

        const OrderIdMap::EraseToken erase_token = order_ids_.find_for_erase(id);
        Order* const order = erase_token.order();
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

        if (post_only && time_in_force != TimeInForce::Gtc) {
            result.status = Status::InvalidPostOnlyTimeInForce;
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
            const Status capacity_status =
                start_operation(1,
                                new_quantity == old_quantity ? 0U
                                                             : required_single_level_market_data_count());
            if (capacity_status != Status::Accepted) {
                result.status = capacity_status;
                return finish_result(result);
            }

            if (new_quantity == old_quantity) {
                order->post_only = post_only;
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
            Quantity previous_level_quantity = 0;
            BestQuote previous_best{};
            if (market_data_.enabled()) {
                previous_level_quantity = side.depth_at_price(old_price);
                previous_best = side.best_quote();
            }
            result.status = side.reduce_order(*order, new_quantity);
            if (result.status == Status::Accepted) {
                result.resting_quantity = new_quantity;
                order->post_only = post_only;
                if (market_data_.enabled()) {
                    emit_market_data_level_change(&market_data_,
                                                  side_value,
                                                  old_price,
                                                  previous_level_quantity,
                                                  side.depth_at_price(old_price),
                                                  side.order_count_at_price(old_price),
                                                  previous_best,
                                                  side.best_quote(),
                                                  timestamp);
                }
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
        if (post_only && contra_side.executable_quantity(new_quantity, true, new_price) != 0) {
            result.status = Status::PostOnlyWouldCross;
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

        const MatchPreflight preflight = contra_side.preflight(
            new_quantity, true, new_price, order->participant_id, config_.self_trade_policy);
        const Quantity executable = preflight.executable_quantity;
        if (time_in_force == TimeInForce::Fok &&
            (executable < new_quantity || preflight.aggressor_cancelled)) {
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

        if (time_in_force == TimeInForce::Gtc && executable < new_quantity &&
            !preflight.aggressor_cancelled) {
            const Quantity residual_quantity = new_quantity - executable;
            const Status storage_status =
                same_side.can_accept_replacement_residual(*order, new_price, residual_quantity);
            if (storage_status != Status::Accepted) {
                result.status = storage_status;
                if (!start_event_operation(1)) {
                    return event_log_full_result(result);
                }
                emit_preflight_order_rejected(id,
                                              side_value,
                                              new_price,
                                              new_quantity,
                                              timestamp,
                                              time_in_force,
                                              result.status,
                                              old_quantity,
                                              new_quantity);
                return finish_result(result);
            }
        }

        const std::uint32_t required_event_count =
            required_replace_event_count(preflight, new_quantity, executable);
        const Status capacity_status =
            start_operation(required_event_count,
                            required_replace_market_data_count(preflight, new_quantity, executable));
        if (capacity_status != Status::Accepted) {
            result.status = capacity_status;
            return finish_result(result);
        }

        Quantity previous_old_level_quantity = 0;
        BestQuote previous_same_best{};
        if (market_data_.enabled()) {
            previous_old_level_quantity = same_side.depth_at_price(old_price);
            previous_same_best = same_side.best_quote();
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

        static_cast<void>(order_ids_.erase(erase_token));
        if (market_data_.enabled()) {
            emit_market_data_level_change(&market_data_,
                                          side_value,
                                          old_price,
                                          previous_old_level_quantity,
                                          same_side.depth_at_price(old_price),
                                          same_side.order_count_at_price(old_price),
                                          previous_same_best,
                                          same_side.best_quote(),
                                          timestamp);
        }
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
            contra_side.match(new_quantity,
                              true,
                              new_price,
                              order_ids_,
                              orders_,
                              event_log_,
                              id,
                              side_value,
                              timestamp,
                              order->participant_id,
                              config_.self_trade_policy,
                              active_market_data());
        result.executed_quantity = match_result.executed_quantity;
        result.fills = match_result.fills;
        result.has_last_price = match_result.has_last_price;
        result.last_price = match_result.last_price;
        result.aggressor_cancelled_by_stp = match_result.aggressor_cancelled_by_stp;
        result.resting_orders_cancelled_by_stp =
            match_result.resting_orders_cancelled_by_stp;

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

        if (match_result.aggressor_cancelled_by_stp) {
            order->clear();
            orders_.release(order);
            result.resting_quantity = 0;
            result.status =
                result.executed_quantity == 0 ? Status::SelfTradePrevented : Status::PartiallyFilled;
            event_log_.append_order(BookEvent::Kind::OrderCancelled,
                                    Status::SelfTradePrevented,
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

        const ParticipantId participant_id = order->participant_id;
        order->reset(id,
                     side_value,
                     new_price,
                     match_result.remaining_quantity,
                     timestamp,
                     next_sequence(),
                     participant_id,
                     post_only);
        Quantity previous_new_level_quantity = 0;
        BestQuote previous_new_best{};
        if (market_data_.enabled()) {
            previous_new_level_quantity = same_side.depth_at_price(new_price);
            previous_new_best = same_side.best_quote();
        }
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

        if (market_data_.enabled()) {
            emit_market_data_level_change(&market_data_,
                                          side_value,
                                          new_price,
                                          previous_new_level_quantity,
                                          same_side.depth_at_price(new_price),
                                          same_side.order_count_at_price(new_price),
                                          previous_new_best,
                                          same_side.best_quote(),
                                          timestamp);
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
                                                 const Timestamp timestamp = 0,
                                                 const ParticipantId participant_id =
                                                     kAnonymousParticipantId) noexcept
    {
        MatchResult result{};
        result.requested_quantity = quantity;
        result.remaining_quantity = quantity;
        begin_operation();
        if (aggressor_side != Side::Buy && aggressor_side != Side::Sell) {
            result.status = Status::InvalidCommand;
            return finish_result(result);
        }

        if (!valid_lot_quantity(quantity)) {
            result.status = quantity == 0 ? Status::InvalidQuantity : Status::LotSizeViolation;
            if (!start_event_operation(1)) {
                return event_log_full_result(result);
            }
            emit_order_rejected(aggressor_id, aggressor_side, 0, quantity, timestamp, TimeInForce::Gtc, result.status);
            return finish_result(result);
        }

        BookSide& contra_side = aggressor_side == Side::Buy ? asks_ : bids_;
        MatchPreflight preflight{};
        if (config_.self_trade_policy == SelfTradePolicy::Disabled) {
            preflight.fill_count =
                contra_side.executable_fill_count(quantity, false, 0);
        } else {
            preflight =
                contra_side.preflight(quantity, false, 0, participant_id, config_.self_trade_policy);
        }
        const std::uint32_t required_event_count =
            saturated_add(preflight.fill_count, preflight.resting_cancel_count);
        const Status capacity_status =
            start_operation(required_event_count, required_match_market_data_count(preflight));
        if (capacity_status != Status::Accepted) {
            result.status = capacity_status;
            return finish_result(result);
        }

        result = contra_side.match(quantity,
                                   false,
                                   0,
                                   order_ids_,
                                   orders_,
                                   event_log_,
                                   aggressor_id,
                                   aggressor_side,
                                   timestamp,
                                   participant_id,
                                   config_.self_trade_policy,
                                   active_market_data());
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

    [[nodiscard]] std::span<const MarketDataEvent> last_market_data_events() const noexcept
    {
        return market_data_.last_events();
    }

    [[nodiscard]] SequenceNumber market_data_sequence() const noexcept
    {
        return market_data_.current_sequence();
    }

    /// Current resting-order FIFO sequence checkpoint.
    [[nodiscard]] SequenceNumber fifo_sequence() const noexcept
    {
        return next_sequence_;
    }

    /// Current sequenced command-event checkpoint.
    [[nodiscard]] SequenceNumber event_sequence() const noexcept
    {
        return event_log_.next_sequence_value();
    }

    [[nodiscard]] std::uint32_t market_data_capacity() const noexcept
    {
        return market_data_.capacity();
    }

    /// Canonical FNV-1a checksum of logical book state.
    ///
    /// Dense/sparse storage, capacities, padding, and pointers are excluded.
    [[nodiscard]] std::uint64_t state_checksum() const noexcept
    {
        std::uint64_t hash = kChecksumOffsetBasis;
        checksum_u32(hash, instrument_id_);
        checksum_u64(hash, static_cast<std::uint64_t>(config_.min_price));
        checksum_u64(hash, static_cast<std::uint64_t>(config_.max_price));
        checksum_u64(hash, static_cast<std::uint64_t>(config_.tick_size));
        checksum_u64(hash, config_.lot_size);
        checksum_u8(hash, static_cast<std::uint8_t>(config_.self_trade_policy));
        checksum_u64(hash, next_sequence_);
        checksum_u64(hash, market_data_.current_sequence());

        const auto checksum_side = [&hash](const Side side, const BookSide& book_side) noexcept {
            checksum_u8(hash, static_cast<std::uint8_t>(side));
            book_side.for_each_level([&hash](const PriceLevel& level) noexcept {
                checksum_u8(hash, 0x4cU);
                checksum_u64(hash, static_cast<std::uint64_t>(level.price()));
                checksum_u64(hash, level.total_quantity());
                checksum_u32(hash, level.order_count());
                const Order* order = level.front();
                while (order != nullptr) {
                    checksum_u8(hash, 0x4fU);
                    checksum_u64(hash, order->id);
                    checksum_u64(hash, static_cast<std::uint64_t>(order->price));
                    checksum_u64(hash, order->quantity);
                    checksum_u64(hash, order->timestamp);
                    checksum_u64(hash, order->sequence);
                    checksum_u64(hash, order->participant_id);
                    checksum_u8(hash, order->post_only ? 1U : 0U);
                    checksum_u64(hash, order->initial_quantity);
                    checksum_u8(hash, static_cast<std::uint8_t>(order->state));
                    order = order->next;
                }
            });
        };
        checksum_side(Side::Buy, bids_);
        checksum_side(Side::Sell, asks_);
        return hash;
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
    MarketDataLog market_data_;
    detail::SnapshotValidationWorkspace snapshot_validation_;
    SequenceNumber next_sequence_{0};
    static constexpr std::uint64_t kChecksumOffsetBasis = 14695981039346656037ULL;
    static constexpr std::uint64_t kChecksumPrime = 1099511628211ULL;

    static void checksum_u8(std::uint64_t& hash, const std::uint8_t value) noexcept
    {
        hash ^= value;
        hash *= kChecksumPrime;
    }

    static void checksum_u32(std::uint64_t& hash, const std::uint32_t value) noexcept
    {
        for (std::uint32_t byte = 0; byte < 4U; ++byte) {
            checksum_u8(hash, static_cast<std::uint8_t>((value >> (byte * 8U)) & 0xffU));
        }
    }

    static void checksum_u64(std::uint64_t& hash, const std::uint64_t value) noexcept
    {
        for (std::uint32_t byte = 0; byte < 8U; ++byte) {
            checksum_u8(hash, static_cast<std::uint8_t>((value >> (byte * 8U)) & 0xffU));
        }
    }

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
        if (!valid_lot_quantity(quantity)) {
            return quantity == 0 ? Status::InvalidQuantity : Status::LotSizeViolation;
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
        if (!valid_lot_quantity(quantity)) {
            return quantity == 0 ? Status::InvalidQuantity : Status::LotSizeViolation;
        }
        if (price < config_.min_price || price > config_.max_price ||
            price_distance(config_.min_price, price) % static_cast<std::uint64_t>(config_.tick_size) != 0U) {
            return Status::InvalidPrice;
        }
        return Status::Accepted;
    }

    [[nodiscard]] bool valid_lot_quantity(const Quantity quantity) const noexcept
    {
        return quantity != 0 &&
               (config_.lot_size <= 1U || quantity % config_.lot_size == 0U);
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
        if (market_data_.enabled()) {
            market_data_.begin_operation(0);
        }
        if (!event_log_.can_record(required_event_count)) {
            event_log_.begin_operation(0);
            return false;
        }

        event_log_.begin_operation(required_event_count);
        return true;
    }

    void begin_operation() noexcept
    {
        event_log_.begin_operation(0);
        if (market_data_.enabled()) {
            market_data_.begin_operation(0);
        }
    }

    [[nodiscard]] Status start_operation(const std::uint32_t required_event_count,
                                         const std::uint32_t required_market_data_count) noexcept
    {
        if (!event_log_.can_record(required_event_count)) {
            begin_operation();
            return Status::EventLogFull;
        }
        if (market_data_.enabled() &&
            !market_data_.can_record(required_market_data_count)) {
            begin_operation();
            return Status::MarketDataLogFull;
        }

        event_log_.begin_operation(required_event_count);
        if (market_data_.enabled()) {
            market_data_.begin_operation(required_market_data_count);
        }
        return Status::Accepted;
    }

    [[nodiscard]] MarketDataLog* active_market_data() noexcept
    {
        return market_data_.enabled() ? &market_data_ : nullptr;
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

    [[nodiscard]] static std::uint32_t required_limit_event_count(const MatchPreflight& preflight,
                                                                  const Quantity requested_quantity,
                                                                  const Quantity executable_quantity) noexcept
    {
        std::uint32_t required = 1U;
        required = saturated_add(required, preflight.fill_count);
        required = saturated_add(required, preflight.resting_cancel_count);
        return saturated_add(required, residual_event_count(requested_quantity, executable_quantity));
    }

    [[nodiscard]] static std::uint32_t required_replace_event_count(const MatchPreflight& preflight,
                                                                    const Quantity requested_quantity,
                                                                    const Quantity executable_quantity) noexcept
    {
        std::uint32_t required = 2U;
        required = saturated_add(required, preflight.fill_count);
        required = saturated_add(required, preflight.resting_cancel_count);
        return saturated_add(required, residual_event_count(requested_quantity, executable_quantity));
    }

    [[nodiscard]] static std::uint32_t saturated_multiply(const std::uint32_t value,
                                                          const std::uint32_t multiplier) noexcept
    {
        return value > std::numeric_limits<std::uint32_t>::max() / multiplier
                   ? std::numeric_limits<std::uint32_t>::max()
                   : value * multiplier;
    }

    [[nodiscard]] std::uint32_t required_single_level_market_data_count() const noexcept
    {
        return market_data_.enabled() ? 2U : 0U;
    }

    [[nodiscard]] std::uint32_t required_match_market_data_count(
        const MatchPreflight& preflight) const noexcept
    {
        if (!market_data_.enabled()) {
            return 0;
        }
        return saturated_add(saturated_multiply(preflight.fill_count, 3U),
                             saturated_multiply(preflight.resting_cancel_count, 2U));
    }

    [[nodiscard]] std::uint32_t required_limit_market_data_count(
        const MatchPreflight& preflight,
        const Quantity requested_quantity,
        const Quantity executable_quantity) const noexcept
    {
        std::uint32_t required = required_match_market_data_count(preflight);
        if (!preflight.aggressor_cancelled && executable_quantity < requested_quantity) {
            required = saturated_add(required, required_single_level_market_data_count());
        }
        return required;
    }

    [[nodiscard]] std::uint32_t required_replace_market_data_count(
        const MatchPreflight& preflight,
        const Quantity requested_quantity,
        const Quantity executable_quantity) const noexcept
    {
        if (!market_data_.enabled()) {
            return 0;
        }
        std::uint32_t required = required_single_level_market_data_count();
        required = saturated_add(required, required_match_market_data_count(preflight));
        if (!preflight.aggressor_cancelled && executable_quantity < requested_quantity) {
            required = saturated_add(required, required_single_level_market_data_count());
        }
        return required;
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

    void emit_preflight_order_rejected(const OrderId id,
                                       const Side side,
                                       const Price price,
                                       const Quantity quantity,
                                       const Timestamp timestamp,
                                       const TimeInForce time_in_force,
                                       const Status reason,
                                       const Quantity old_quantity = 0,
                                       const Quantity new_quantity = 0) noexcept
    {
        if (reason == Status::PriceLevelQuantityOverflow) {
            event_log_.append_unsequenced_order(BookEvent::Kind::OrderRejected,
                                                reason,
                                                id,
                                                side,
                                                price,
                                                quantity,
                                                timestamp,
                                                time_in_force,
                                                old_quantity,
                                                new_quantity);
            return;
        }

        event_log_.append_order(BookEvent::Kind::OrderRejected,
                                reason,
                                id,
                                side,
                                price,
                                quantity,
                                timestamp,
                                time_in_force,
                                old_quantity,
                                new_quantity);
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

    [[nodiscard]] SequenceNumber snapshot_market_data_sequence() const noexcept
    {
        return market_data_.current_sequence();
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
        market_data_.reset();
    }

    [[nodiscard]] Status restore_snapshot_order(const BookSnapshotOrder& record) noexcept
    {
        Order* order = orders_.allocate();
        if (order == nullptr) {
            return Status::PoolExhausted;
        }

        order->reset(record.id,
                     record.side,
                     record.price,
                     record.quantity,
                     record.timestamp,
                     record.sequence,
                     record.participant_id,
                     record.post_only);
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

        order->initial_quantity = record.initial_quantity;
        order->state = record.state;

        return Status::Accepted;
    }

    void finish_snapshot_restore(const SequenceNumber next_sequence,
                                 const SequenceNumber event_next_sequence,
                                 const SequenceNumber market_data_sequence) noexcept
    {
        next_sequence_ = next_sequence;
        event_log_.reset(event_next_sequence);
        market_data_.reset(market_data_sequence);
    }
};

static_assert(alignof(OrderBook) >= 64);

} // namespace eigenbook
