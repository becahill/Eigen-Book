#include "Command.hpp"
#include "Journal.hpp"
#include "MarketData.hpp"
#include "MatchingEngine.hpp"
#include "OrderBook.hpp"
#include "Snapshot.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using namespace eigenbook;

[[noreturn]] void fail(const char* file, const int line, const char* expression)
{
    std::fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expression);
    std::abort();
}

#define CHECK(expression)                                                                            \
    do {                                                                                             \
        if (!(expression)) {                                                                         \
            fail(__FILE__, __LINE__, #expression);                                                   \
        }                                                                                            \
    } while (false)

[[nodiscard]] constexpr unsigned status_value(const Status status) noexcept
{
    return static_cast<unsigned>(status);
}

[[nodiscard]] constexpr std::byte byte_value(const unsigned value) noexcept
{
    return static_cast<std::byte>(value);
}

void write_u64_le(std::span<std::byte> buffer, const std::size_t offset, const std::uint64_t value)
{
    CHECK(offset <= buffer.size());
    CHECK(sizeof(value) <= buffer.size() - offset);
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        buffer[offset + index] = byte_value(static_cast<unsigned>((value >> (index * 8U)) & 0xffU));
    }
}

void write_u32_le(std::span<std::byte> buffer, const std::size_t offset, const std::uint32_t value)
{
    CHECK(offset <= buffer.size());
    CHECK(sizeof(value) <= buffer.size() - offset);
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        buffer[offset + index] = byte_value(static_cast<unsigned>((value >> (index * 8U)) & 0xffU));
    }
}

void check_status(const Status actual, const Status expected, const char* context)
{
    if (actual != expected) {
        std::fprintf(stderr,
                     "%s: status mismatch: actual=%u expected=%u\n",
                     context,
                     status_value(actual),
                     status_value(expected));
        std::abort();
    }
}

void check_best_quote(const BestQuote& actual, const BestQuote& expected, const char* context)
{
    if (actual.valid != expected.valid || actual.price != expected.price || actual.quantity != expected.quantity ||
        actual.order_count != expected.order_count) {
        std::fprintf(stderr,
                     "%s: quote mismatch: valid %d/%d price %lld/%lld qty %llu/%llu orders %u/%u\n",
                     context,
                     actual.valid ? 1 : 0,
                     expected.valid ? 1 : 0,
                     static_cast<long long>(actual.price),
                     static_cast<long long>(expected.price),
                     static_cast<unsigned long long>(actual.quantity),
                     static_cast<unsigned long long>(expected.quantity),
                     actual.order_count,
                     expected.order_count);
        std::abort();
    }
}

void check_depth_level(const DepthLevel& actual,
                       const Price price,
                       const Quantity aggregate_quantity,
                       const std::uint32_t order_count)
{
    CHECK(actual.price == price);
    CHECK(actual.aggregate_quantity == aggregate_quantity);
    CHECK(actual.order_count == order_count);
}

template <typename Result>
void check_result_events(const Result& actual, const Result& expected, const char* context);

void check_add_result(const AddOrderResult& actual, const AddOrderResult& expected, const char* context)
{
    check_status(actual.status, expected.status, context);
    CHECK(actual.accepted_quantity == expected.accepted_quantity);
    CHECK(actual.executed_quantity == expected.executed_quantity);
    CHECK(actual.resting_quantity == expected.resting_quantity);
    CHECK(actual.fills == expected.fills);
    CHECK(actual.has_last_price == expected.has_last_price);
    CHECK(actual.last_price == expected.last_price);
    CHECK(actual.aggressor_cancelled_by_stp == expected.aggressor_cancelled_by_stp);
    CHECK(actual.resting_orders_cancelled_by_stp ==
          expected.resting_orders_cancelled_by_stp);
    check_result_events(actual, expected, context);
}

void check_cancel_result(const CancelResult& actual, const CancelResult& expected, const char* context)
{
    check_status(actual.status, expected.status, context);
    CHECK(actual.canceled_quantity == expected.canceled_quantity);
    check_result_events(actual, expected, context);
}

void check_modify_result(const ModifyResult& actual, const ModifyResult& expected, const char* context)
{
    check_status(actual.status, expected.status, context);
    CHECK(actual.old_quantity == expected.old_quantity);
    CHECK(actual.new_quantity == expected.new_quantity);
    check_result_events(actual, expected, context);
}

void check_replace_result(const ReplaceResult& actual, const ReplaceResult& expected, const char* context)
{
    check_status(actual.status, expected.status, context);
    CHECK(actual.old_price == expected.old_price);
    CHECK(actual.new_price == expected.new_price);
    CHECK(actual.old_quantity == expected.old_quantity);
    CHECK(actual.new_quantity == expected.new_quantity);
    CHECK(actual.executed_quantity == expected.executed_quantity);
    CHECK(actual.resting_quantity == expected.resting_quantity);
    CHECK(actual.fills == expected.fills);
    CHECK(actual.has_last_price == expected.has_last_price);
    CHECK(actual.last_price == expected.last_price);
    CHECK(actual.aggressor_cancelled_by_stp == expected.aggressor_cancelled_by_stp);
    CHECK(actual.resting_orders_cancelled_by_stp ==
          expected.resting_orders_cancelled_by_stp);
    check_result_events(actual, expected, context);
}

void check_match_result(const MatchResult& actual, const MatchResult& expected, const char* context)
{
    check_status(actual.status, expected.status, context);
    CHECK(actual.requested_quantity == expected.requested_quantity);
    CHECK(actual.executed_quantity == expected.executed_quantity);
    CHECK(actual.remaining_quantity == expected.remaining_quantity);
    CHECK(actual.fills == expected.fills);
    CHECK(actual.has_last_price == expected.has_last_price);
    CHECK(actual.last_price == expected.last_price);
    CHECK(actual.aggressor_cancelled_by_stp == expected.aggressor_cancelled_by_stp);
    CHECK(actual.resting_orders_cancelled_by_stp ==
          expected.resting_orders_cancelled_by_stp);
    check_result_events(actual, expected, context);
}

void check_trade_event(const TradeEvent& actual, const TradeEvent& expected)
{
    CHECK(actual.instrument_id == expected.instrument_id);
    CHECK(actual.aggressor_id == expected.aggressor_id);
    CHECK(actual.resting_id == expected.resting_id);
    CHECK(actual.aggressor_side == expected.aggressor_side);
    CHECK(actual.price == expected.price);
    CHECK(actual.quantity == expected.quantity);
    CHECK(actual.timestamp == expected.timestamp);
    CHECK(actual.sequence == expected.sequence);
}

void check_book_event(const BookEvent& actual, const BookEvent& expected)
{
    CHECK(actual.kind == expected.kind);
    CHECK(actual.instrument_id == expected.instrument_id);
    CHECK(actual.status == expected.status);
    CHECK(actual.order_id == expected.order_id);
    CHECK(actual.side == expected.side);
    CHECK(actual.price == expected.price);
    CHECK(actual.quantity == expected.quantity);
    CHECK(actual.old_quantity == expected.old_quantity);
    CHECK(actual.new_quantity == expected.new_quantity);
    CHECK(actual.timestamp == expected.timestamp);
    CHECK(actual.sequence == expected.sequence);
    CHECK(actual.time_in_force == expected.time_in_force);
    check_trade_event(actual.trade, expected.trade);
}

void check_event_stream(std::span<const BookEvent> actual,
                        std::span<const BookEvent> expected,
                        const char* context)
{
    if (actual.size() != expected.size()) {
        std::fprintf(stderr,
                     "%s: event count mismatch: actual=%zu expected=%zu\n",
                     context,
                     actual.size(),
                     expected.size());
        std::abort();
    }

    for (std::size_t i = 0; i < actual.size(); ++i) {
        check_book_event(actual[i], expected[i]);
    }
}

template <typename Result>
void check_result_events(const Result& actual, const Result& expected, const char* context)
{
    CHECK(actual.events_emitted == expected.events_emitted);
    CHECK(actual.events_emitted == actual.events.size());
    CHECK(expected.events_emitted == expected.events.size());
    check_event_stream(actual.events, expected.events, context);
}

void check_order_event_fields(const BookEvent& event,
                              const BookEvent::Kind kind,
                              const Status status,
                              const OrderId order_id,
                              const Side side,
                              const Price price,
                              const Quantity quantity,
                              const Timestamp timestamp,
                              const InstrumentId instrument_id = kInvalidInstrumentId)
{
    CHECK(event.kind == kind);
    CHECK(event.instrument_id == instrument_id);
    CHECK(event.status == status);
    CHECK(event.order_id == order_id);
    CHECK(event.side == side);
    CHECK(event.price == price);
    CHECK(event.quantity == quantity);
    CHECK(event.timestamp == timestamp);
}

void check_trade_event_fields(const BookEvent& event,
                              const OrderId aggressor_id,
                              const OrderId resting_id,
                              const Side aggressor_side,
                              const Price price,
                              const Quantity quantity,
                              const Timestamp timestamp,
                              const InstrumentId instrument_id = kInvalidInstrumentId)
{
    CHECK(event.kind == BookEvent::Kind::Trade);
    CHECK(event.instrument_id == instrument_id);
    CHECK(event.status == Status::Filled);
    CHECK(event.order_id == aggressor_id);
    CHECK(event.side == aggressor_side);
    CHECK(event.price == price);
    CHECK(event.quantity == quantity);
    CHECK(event.timestamp == timestamp);
    CHECK(event.sequence == event.trade.sequence);
    CHECK(event.trade.instrument_id == instrument_id);
    CHECK(event.trade.aggressor_id == aggressor_id);
    CHECK(event.trade.resting_id == resting_id);
    CHECK(event.trade.aggressor_side == aggressor_side);
    CHECK(event.trade.price == price);
    CHECK(event.trade.quantity == quantity);
    CHECK(event.trade.timestamp == timestamp);
}

[[nodiscard]] bool crosses(const Side resting_side, const Price resting_price, const Price limit_price) noexcept
{
    return resting_side == Side::Sell ? resting_price <= limit_price : resting_price >= limit_price;
}

struct ReferenceOrder final {
    OrderId id{kInvalidOrderId};
    Price price{0};
    Quantity quantity{0};
    Timestamp timestamp{0};
    SequenceNumber sequence{0};
    Side side{Side::Buy};
    bool active{false};
};

class ReferenceOrderBook final {
public:
    explicit ReferenceOrderBook(const BookConfig& config,
                                const InstrumentId instrument_id = kInvalidInstrumentId)
        : config_(normalize_config(config)),
          event_log_(config_.event_log_capacity, instrument_id),
          id_capacity_(OrderIdMap::capacity_for(config_.order_id_map_capacity))
    {
    }

    [[nodiscard]] AddOrderResult add_limit_order(const OrderId id,
                                                 const Side side,
                                                 const Price price,
                                                 const Quantity quantity,
                                                 const Timestamp timestamp = 0,
                                                 const TimeInForce time_in_force = TimeInForce::Gtc)
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

        if (is_active(id)) {
            result.status = Status::DuplicateOrderId;
            if (!start_event_operation(1)) {
                return event_log_full_result(result);
            }
            emit_order_rejected(id, side, price, quantity, timestamp, time_in_force, result.status);
            return finish_result(result);
        }

        const Status id_slot_status = can_insert_id();
        const Quantity executable = executable_quantity(side, quantity, price);
        if (time_in_force == TimeInForce::Fok && executable < quantity) {
            result.status = Status::FokRejected;
            if (!start_event_operation(1)) {
                return event_log_full_result(result);
            }
            emit_order_rejected(id, side, price, quantity, timestamp, time_in_force, result.status);
            return finish_result(result);
        }

        if (time_in_force == TimeInForce::Gtc && executable < quantity) {
            const auto* const target_level = level_at(side, price);
            const Quantity residual_quantity = quantity - executable;
            if (target_level != nullptr &&
                std::numeric_limits<Quantity>::max() - level_depth(*target_level) <
                    residual_quantity) {
                result.accepted_quantity = 0;
                result.status = Status::PriceLevelQuantityOverflow;
                if (!start_event_operation(1)) {
                    return event_log_full_result(result);
                }
                event_log_.append_unsequenced_order(BookEvent::Kind::OrderRejected,
                                                    result.status,
                                                    id,
                                                    side,
                                                    price,
                                                    quantity,
                                                    timestamp,
                                                    time_in_force);
                return finish_result(result);
            }
        }

        if (time_in_force == TimeInForce::Gtc &&
            (id_slot_status != Status::Accepted || live_order_count_ == config_.max_orders) &&
            executable < quantity) {
            result.status = residual_reject_status(id_slot_status);
            if (!start_event_operation(1)) {
                return event_log_full_result(result);
            }
            emit_order_rejected(id, side, price, quantity, timestamp, time_in_force, result.status);
            return finish_result(result);
        }

        const std::uint32_t required_event_count =
            required_limit_event_count(side, quantity, executable, true, price);
        if (!start_event_operation(required_event_count)) {
            result.accepted_quantity = 0;
            return event_log_full_result(result);
        }

        event_log_.append_order(
            BookEvent::Kind::OrderAccepted, Status::Accepted, id, side, price, quantity, timestamp, time_in_force);

        const MatchResult match_result = match_against(side, quantity, true, price, id, timestamp);
        result.executed_quantity = match_result.executed_quantity;
        result.fills = match_result.fills;
        result.has_last_price = match_result.has_last_price;
        result.last_price = match_result.last_price;

        if (match_result.remaining_quantity == 0) {
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

        if (live_order_count_ == config_.max_orders) {
            result.status = Status::PoolExhausted;
            return finish_result(result);
        }

        const auto* const target_level = level_at(side, price);
        if (target_level != nullptr &&
            std::numeric_limits<Quantity>::max() - level_depth(*target_level) <
                match_result.remaining_quantity) {
            result.status = Status::PriceLevelQuantityOverflow;
            return finish_result(result);
        }

        ReferenceOrder order{};
        order.id = id;
        order.side = side;
        order.price = price;
        order.quantity = match_result.remaining_quantity;
        order.timestamp = timestamp;
        order.sequence = next_sequence();
        order.active = true;

        orders_[id] = order;
        level_for(side, price).push_back(id);
        ++live_order_count_;

        result.resting_quantity = order.quantity;
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

    [[nodiscard]] CancelResult cancel_order(const OrderId id, const Timestamp timestamp = 0)
    {
        CancelResult result{};
        event_log_.begin_operation(0);
        auto order_it = orders_.find(id);
        if (order_it == orders_.end() || !order_it->second.active) {
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

        ReferenceOrder& order = order_it->second;
        result.canceled_quantity = order.quantity;
        const Side side = order.side;
        const Price price = order.price;
        remove_from_level(order.side, order.price, id);
        order.active = false;
        order.quantity = 0;
        --live_order_count_;
        result.status = Status::Cancelled;
        event_log_.append_order(
            BookEvent::Kind::OrderCancelled, Status::Cancelled, id, side, price, result.canceled_quantity, timestamp);
        return finish_result(result);
    }

    [[nodiscard]] ModifyResult modify_order(const OrderId id,
                                            const Quantity new_quantity,
                                            const Timestamp timestamp = 0)
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

        auto order_it = orders_.find(id);
        if (order_it == orders_.end() || !order_it->second.active) {
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

        ReferenceOrder& order = order_it->second;
        result.old_quantity = order.quantity;
        result.new_quantity = new_quantity;
        const Side side = order.side;
        const Price price = order.price;

        if (new_quantity == order.quantity) {
            result.status = Status::Accepted;
            event_log_.append_order(BookEvent::Kind::OrderModified,
                                    Status::Accepted,
                                    id,
                                    side,
                                    price,
                                    new_quantity,
                                    timestamp,
                                    TimeInForce::Gtc,
                                    result.old_quantity,
                                    result.new_quantity);
            return finish_result(result);
        }

        if (new_quantity > order.quantity) {
            result.status = Status::QuantityIncreaseRejected;
            event_log_.append_order(BookEvent::Kind::OrderRejected,
                                    result.status,
                                    id,
                                    side,
                                    price,
                                    new_quantity,
                                    timestamp,
                                    TimeInForce::Gtc,
                                    result.old_quantity,
                                    result.new_quantity);
            return finish_result(result);
        }

        order.quantity = new_quantity;
        result.status = Status::Accepted;
        event_log_.append_order(BookEvent::Kind::OrderModified,
                                Status::Accepted,
                                id,
                                side,
                                price,
                                new_quantity,
                                timestamp,
                                TimeInForce::Gtc,
                                result.old_quantity,
                                result.new_quantity);
        return finish_result(result);
    }

    [[nodiscard]] ReplaceResult replace_order(const OrderId id,
                                              const Price new_price,
                                              const Quantity new_quantity,
                                              const TimeInForce time_in_force = TimeInForce::Gtc)
    {
        return replace_order(id, new_price, new_quantity, 0, time_in_force);
    }

    [[nodiscard]] ReplaceResult replace_order(const OrderId id,
                                              const Price new_price,
                                              const Quantity new_quantity,
                                              const Timestamp timestamp,
                                              const TimeInForce time_in_force)
    {
        ReplaceResult result{};
        result.new_price = new_price;
        result.new_quantity = new_quantity;
        event_log_.begin_operation(0);

        auto order_it = orders_.find(id);
        if (order_it == orders_.end() || !order_it->second.active) {
            result.status = Status::UnknownOrderId;
            if (!start_event_operation(1)) {
                return event_log_full_result(result);
            }
            emit_order_rejected(id, Side::Buy, new_price, new_quantity, timestamp, time_in_force, result.status);
            return finish_result(result);
        }

        ReferenceOrder& order = order_it->second;
        result.old_price = order.price;
        result.old_quantity = order.quantity;
        const Side side = order.side;
        const Price old_price = order.price;
        const Quantity old_quantity = order.quantity;

        const Status validation_status = validate_replacement_order(new_price, new_quantity);
        if (validation_status != Status::Accepted) {
            result.status = validation_status;
            if (!start_event_operation(1)) {
                return event_log_full_result(result);
            }
            event_log_.append_order(BookEvent::Kind::OrderRejected,
                                    result.status,
                                    id,
                                    side,
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
                                    side,
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

            order.quantity = new_quantity;
            result.status = Status::Accepted;
            result.resting_quantity = new_quantity;
            event_log_.append_order(BookEvent::Kind::OrderModified,
                                    Status::Accepted,
                                    id,
                                    side,
                                    old_price,
                                    new_quantity,
                                    timestamp,
                                    TimeInForce::Gtc,
                                    old_quantity,
                                    new_quantity);
            return finish_result(result);
        }

        const Quantity executable = executable_quantity(side, new_quantity, new_price);
        if (time_in_force == TimeInForce::Fok && executable < new_quantity) {
            result.status = Status::FokRejected;
            if (!start_event_operation(1)) {
                return event_log_full_result(result);
            }
            event_log_.append_order(BookEvent::Kind::OrderRejected,
                                    result.status,
                                    id,
                                    side,
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
            const Status storage_status = can_accept_replacement_residual(order, new_price, residual_quantity);
            if (storage_status != Status::Accepted) {
                result.status = storage_status;
                if (!start_event_operation(1)) {
                    return event_log_full_result(result);
                }
                if (result.status == Status::PriceLevelQuantityOverflow) {
                    event_log_.append_unsequenced_order(BookEvent::Kind::OrderRejected,
                                                        result.status,
                                                        id,
                                                        side,
                                                        new_price,
                                                        new_quantity,
                                                        timestamp,
                                                        time_in_force,
                                                        old_quantity,
                                                        new_quantity);
                } else {
                    event_log_.append_order(BookEvent::Kind::OrderRejected,
                                            result.status,
                                            id,
                                            side,
                                            new_price,
                                            new_quantity,
                                            timestamp,
                                            time_in_force,
                                            old_quantity,
                                            new_quantity);
                }
                return finish_result(result);
            }
        }

        const std::uint32_t required_event_count =
            required_replace_event_count(side, new_quantity, executable, true, new_price);
        if (!start_event_operation(required_event_count)) {
            return event_log_full_result(result);
        }

        remove_from_level(side, old_price, id);
        order.active = false;
        order.quantity = 0;
        --live_order_count_;
        event_log_.append_order(
            BookEvent::Kind::OrderCancelled, Status::Cancelled, id, side, old_price, old_quantity, timestamp);
        event_log_.append_order(BookEvent::Kind::OrderAccepted,
                                Status::Accepted,
                                id,
                                side,
                                new_price,
                                new_quantity,
                                timestamp,
                                time_in_force,
                                old_quantity,
                                new_quantity);

        const MatchResult match_result = match_against(side, new_quantity, true, new_price, id, timestamp);
        result.executed_quantity = match_result.executed_quantity;
        result.fills = match_result.fills;
        result.has_last_price = match_result.has_last_price;
        result.last_price = match_result.last_price;

        if (match_result.remaining_quantity == 0) {
            result.status = Status::Filled;
            return finish_result(result);
        }

        if (time_in_force == TimeInForce::Ioc) {
            result.status = result.executed_quantity == 0 ? Status::NoLiquidity : Status::PartiallyFilled;
            event_log_.append_order(BookEvent::Kind::OrderCancelled,
                                    Status::Cancelled,
                                    id,
                                    side,
                                    new_price,
                                    match_result.remaining_quantity,
                                    timestamp,
                                    time_in_force,
                                    old_quantity,
                                    new_quantity);
            return finish_result(result);
        }

        order.id = id;
        order.side = side;
        order.price = new_price;
        order.quantity = match_result.remaining_quantity;
        order.timestamp = timestamp;
        order.sequence = next_sequence();
        order.active = true;
        level_for(side, new_price).push_back(id);
        ++live_order_count_;

        result.resting_quantity = order.quantity;
        result.status = result.executed_quantity == 0 ? Status::Accepted : Status::PartiallyFilled;
        event_log_.append_order(BookEvent::Kind::OrderResting,
                                Status::Accepted,
                                id,
                                side,
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
                                                 const Timestamp timestamp = 0)
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

        const std::uint32_t required_event_count = executable_fill_count(aggressor_side, quantity, false, 0);
        if (!start_event_operation(required_event_count)) {
            MatchResult result{};
            result.status = Status::EventLogFull;
            result.requested_quantity = quantity;
            result.remaining_quantity = quantity;
            return finish_result(result);
        }

        MatchResult result = match_against(aggressor_side, quantity, false, 0, aggressor_id, timestamp);
        return finish_result(result);
    }

    [[nodiscard]] BestQuote best_bid() const
    {
        return best_quote(Side::Buy);
    }

    [[nodiscard]] BestQuote best_ask() const
    {
        return best_quote(Side::Sell);
    }

    [[nodiscard]] Quantity depth_at_price(const Side side, const Price price) const
    {
        const auto* level = level_at(side, price);
        if (level == nullptr) {
            return 0;
        }

        Quantity total = 0;
        for (const OrderId id : *level) {
            const auto order_it = orders_.find(id);
            CHECK(order_it != orders_.end());
            CHECK(order_it->second.active);
            total += order_it->second.quantity;
        }
        return total;
    }

    [[nodiscard]] std::uint32_t order_count_at_price(const Side side, const Price price) const
    {
        const auto* level = level_at(side, price);
        return level == nullptr ? 0U : static_cast<std::uint32_t>(level->size());
    }

    [[nodiscard]] std::uint32_t depth(const Side side,
                                      const std::uint32_t max_levels,
                                      DepthLevel* const out_buffer) const
    {
        if (max_levels == 0 || out_buffer == nullptr) {
            return 0;
        }

        std::uint32_t written = 0;
        const auto& levels = levels_for(side);
        if (side == Side::Buy) {
            for (auto level_it = levels.rbegin(); level_it != levels.rend() && written < max_levels; ++level_it) {
                out_buffer[written] = DepthLevel{level_it->first,
                                                 level_depth(level_it->second),
                                                 static_cast<std::uint32_t>(level_it->second.size())};
                ++written;
            }
            return written;
        }

        for (auto level_it = levels.begin(); level_it != levels.end() && written < max_levels; ++level_it) {
            out_buffer[written] = DepthLevel{level_it->first,
                                             level_depth(level_it->second),
                                             static_cast<std::uint32_t>(level_it->second.size())};
            ++written;
        }
        return written;
    }

    [[nodiscard]] bool is_active(const OrderId id) const
    {
        const auto order_it = orders_.find(id);
        return order_it != orders_.end() && order_it->second.active;
    }

    [[nodiscard]] const ReferenceOrder* find_order(const OrderId id) const
    {
        const auto order_it = orders_.find(id);
        if (order_it == orders_.end() || !order_it->second.active) {
            return nullptr;
        }
        return &order_it->second;
    }

    [[nodiscard]] std::uint32_t live_order_count() const noexcept
    {
        return live_order_count_;
    }

    [[nodiscard]] const BookConfig& config() const noexcept
    {
        return config_;
    }

private:
    BookConfig config_;
    EventLog event_log_;
    std::uint32_t id_capacity_{0};
    std::map<OrderId, ReferenceOrder> orders_;
    std::map<Price, std::deque<OrderId>> bids_;
    std::map<Price, std::deque<OrderId>> asks_;
    std::uint32_t live_order_count_{0};
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

    [[nodiscard]] static bool valid_time_in_force(const TimeInForce time_in_force) noexcept
    {
        return time_in_force == TimeInForce::Gtc || time_in_force == TimeInForce::Ioc ||
               time_in_force == TimeInForce::Fok;
    }

    [[nodiscard]] Status validate_new_order(const OrderId id, const Price price, const Quantity quantity) const
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

    [[nodiscard]] Status validate_replacement_order(const Price price, const Quantity quantity) const
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

    [[nodiscard]] Status can_insert_id() const noexcept
    {
        return live_order_count_ >= id_capacity_ ? Status::OrderIdMapFull : Status::Accepted;
    }

    [[nodiscard]] Status residual_reject_status(const Status id_slot_status) const noexcept
    {
        if (live_order_count_ == config_.max_orders) {
            return Status::PoolExhausted;
        }
        return id_slot_status;
    }

    [[nodiscard]] Status can_accept_replacement_residual(const ReferenceOrder& old_order,
                                                         const Price new_price,
                                                         const Quantity residual_quantity) const
    {
        if (residual_quantity == 0) {
            return Status::Accepted;
        }

        const auto& levels = levels_for(old_order.side);
        const auto old_level_it = levels.find(old_order.price);
        CHECK(old_level_it != levels.end());

        const auto target_level_it = levels.find(new_price);
        if (target_level_it != levels.end()) {
            Quantity target_quantity = level_depth(target_level_it->second);
            if (new_price == old_order.price) {
                CHECK(old_order.quantity <= target_quantity);
                target_quantity -= old_order.quantity;
            }
            return std::numeric_limits<Quantity>::max() - target_quantity < residual_quantity
                       ? Status::PriceLevelQuantityOverflow
                       : Status::Accepted;
        }

        if (config_.price_level_mode != PriceLevelMode::Sparse || old_level_it->second.size() == 1U ||
            levels.size() < config_.max_orders) {
            return Status::Accepted;
        }

        return Status::PoolExhausted;
    }

    [[nodiscard]] std::uint32_t max_events_for_add() const noexcept
    {
        if (live_order_count_ > std::numeric_limits<std::uint32_t>::max() - 2U) {
            return std::numeric_limits<std::uint32_t>::max();
        }
        return live_order_count_ + 2U;
    }

    [[nodiscard]] std::uint32_t max_events_for_replace() const noexcept
    {
        if (live_order_count_ > std::numeric_limits<std::uint32_t>::max() - 2U) {
            return std::numeric_limits<std::uint32_t>::max();
        }
        return live_order_count_ + 2U;
    }

    [[nodiscard]] std::uint32_t max_events_for_market() const noexcept
    {
        return std::max(1U, live_order_count_);
    }

    [[nodiscard]] bool start_event_operation(const std::uint32_t required_event_count)
    {
        if (!event_log_.can_record(required_event_count)) {
            event_log_.begin_operation(0);
            return false;
        }

        event_log_.begin_operation(required_event_count);
        return true;
    }

    template <typename Result>
    [[nodiscard]] Result event_log_full_result(Result result)
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

    [[nodiscard]] std::uint32_t required_limit_event_count(const Side aggressor_side,
                                                           const Quantity requested_quantity,
                                                           const Quantity executable_quantity,
                                                           const bool has_limit_price,
                                                           const Price limit_price) const
    {
        std::uint32_t required = 1U;
        required = saturated_add(
            required, executable_fill_count(aggressor_side, requested_quantity, has_limit_price, limit_price));
        return saturated_add(required, residual_event_count(requested_quantity, executable_quantity));
    }

    [[nodiscard]] std::uint32_t required_replace_event_count(const Side aggressor_side,
                                                             const Quantity requested_quantity,
                                                             const Quantity executable_quantity,
                                                             const bool has_limit_price,
                                                             const Price limit_price) const
    {
        std::uint32_t required = 2U;
        required = saturated_add(
            required, executable_fill_count(aggressor_side, requested_quantity, has_limit_price, limit_price));
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

    [[nodiscard]] std::map<Price, std::deque<OrderId>>& levels_for(const Side side) noexcept
    {
        return side == Side::Buy ? bids_ : asks_;
    }

    [[nodiscard]] const std::map<Price, std::deque<OrderId>>& levels_for(const Side side) const noexcept
    {
        return side == Side::Buy ? bids_ : asks_;
    }

    [[nodiscard]] std::deque<OrderId>& level_for(const Side side, const Price price)
    {
        return levels_for(side)[price];
    }

    [[nodiscard]] const std::deque<OrderId>* level_at(const Side side, const Price price) const
    {
        const auto& levels = levels_for(side);
        const auto level_it = levels.find(price);
        return level_it == levels.end() ? nullptr : &level_it->second;
    }

    void remove_from_level(const Side side, const Price price, const OrderId id)
    {
        auto& levels = levels_for(side);
        auto level_it = levels.find(price);
        CHECK(level_it != levels.end());

        auto& queue = level_it->second;
        const auto order_it = std::find(queue.begin(), queue.end(), id);
        CHECK(order_it != queue.end());
        queue.erase(order_it);

        if (queue.empty()) {
            levels.erase(level_it);
        }
    }

    [[nodiscard]] Quantity level_depth(const std::deque<OrderId>& level) const
    {
        Quantity total = 0;
        for (const OrderId id : level) {
            const auto order_it = orders_.find(id);
            CHECK(order_it != orders_.end());
            CHECK(order_it->second.active);
            total += order_it->second.quantity;
        }
        return total;
    }

    [[nodiscard]] Quantity executable_quantity(const Side aggressor_side,
                                               const Quantity requested_quantity,
                                               const Price limit_price) const
    {
        const Side resting_side = aggressor_side == Side::Buy ? Side::Sell : Side::Buy;
        Quantity executable = 0;

        if (resting_side == Side::Sell) {
            for (const auto& [price, level] : asks_) {
                if (!crosses(resting_side, price, limit_price)) {
                    break;
                }

                executable += level_depth(level);
                if (executable >= requested_quantity) {
                    return requested_quantity;
                }
            }
            return executable;
        }

        for (auto level_it = bids_.rbegin(); level_it != bids_.rend(); ++level_it) {
            if (!crosses(resting_side, level_it->first, limit_price)) {
                break;
            }

            executable += level_depth(level_it->second);
            if (executable >= requested_quantity) {
                return requested_quantity;
            }
        }

        return executable;
    }

    [[nodiscard]] std::uint32_t executable_fill_count(const Side aggressor_side,
                                                      const Quantity requested_quantity,
                                                      const bool has_limit_price,
                                                      const Price limit_price) const
    {
        const Side resting_side = aggressor_side == Side::Buy ? Side::Sell : Side::Buy;
        Quantity remaining = requested_quantity;
        std::uint32_t fills = 0;

        const auto count_level = [&](const Price price, const std::deque<OrderId>& level) {
            if (has_limit_price && !crosses(resting_side, price, limit_price)) {
                return false;
            }

            for (const OrderId id : level) {
                const auto order_it = orders_.find(id);
                CHECK(order_it != orders_.end());
                CHECK(order_it->second.active);

                const Quantity executed_quantity = std::min(remaining, order_it->second.quantity);
                if (executed_quantity == 0) {
                    return false;
                }

                remaining -= executed_quantity;
                if (fills != std::numeric_limits<std::uint32_t>::max()) {
                    ++fills;
                }
                if (remaining == 0) {
                    return false;
                }
            }

            return true;
        };

        if (resting_side == Side::Sell) {
            for (const auto& [price, level] : asks_) {
                if (!count_level(price, level)) {
                    break;
                }
            }
            return fills;
        }

        for (auto level_it = bids_.rbegin(); level_it != bids_.rend(); ++level_it) {
            if (!count_level(level_it->first, level_it->second)) {
                break;
            }
        }

        return fills;
    }

    [[nodiscard]] bool best_price(const Side resting_side, Price& price) const
    {
        const auto& levels = levels_for(resting_side);
        if (levels.empty()) {
            return false;
        }

        if (resting_side == Side::Sell) {
            price = levels.begin()->first;
        } else {
            price = levels.rbegin()->first;
        }
        return true;
    }

    [[nodiscard]] MatchResult match_against(const Side aggressor_side,
                                            const Quantity requested_quantity,
                                            const bool has_limit_price,
                                            const Price limit_price,
                                            const OrderId aggressor_id,
                                            const Timestamp timestamp)
    {
        const Side resting_side = aggressor_side == Side::Buy ? Side::Sell : Side::Buy;
        auto& levels = levels_for(resting_side);

        MatchResult result{};
        result.requested_quantity = requested_quantity;
        result.remaining_quantity = requested_quantity;

        Price price = 0;
        while (result.remaining_quantity > 0 && best_price(resting_side, price)) {
            if (has_limit_price && !crosses(resting_side, price, limit_price)) {
                break;
            }

            auto level_it = levels.find(price);
            CHECK(level_it != levels.end());
            auto& level = level_it->second;

            while (result.remaining_quantity > 0 && !level.empty()) {
                const OrderId id = level.front();
                ReferenceOrder& resting_order = orders_.find(id)->second;
                const Quantity executed_quantity = std::min(result.remaining_quantity, resting_order.quantity);
                CHECK(executed_quantity > 0);

                resting_order.quantity -= executed_quantity;
                result.remaining_quantity -= executed_quantity;
                result.executed_quantity += executed_quantity;
                ++result.fills;
                result.has_last_price = true;
                result.last_price = price;
                event_log_.append_trade(aggressor_id, id, aggressor_side, price, executed_quantity, timestamp);

                if (resting_order.quantity == 0) {
                    resting_order.active = false;
                    level.pop_front();
                    --live_order_count_;
                }
            }

            if (level.empty()) {
                levels.erase(level_it);
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

    [[nodiscard]] BestQuote best_quote(const Side side) const
    {
        BestQuote result{};
        Price price = 0;
        if (!best_price(side, price)) {
            return result;
        }

        result.valid = true;
        result.price = price;
        result.quantity = depth_at_price(side, price);
        result.order_count = order_count_at_price(side, price);
        return result;
    }
};

void check_books_equal(const OrderBook& actual, const ReferenceOrderBook& expected)
{
    const BookConfig& config = expected.config();
    check_best_quote(actual.best_bid(), expected.best_bid(), "best_bid");
    check_best_quote(actual.best_ask(), expected.best_ask(), "best_ask");
    CHECK(actual.live_order_count() == expected.live_order_count());
    CHECK(actual.order_capacity() == config.max_orders);

    if (config.price_level_count() <= 4'096U) {
        for (std::uint32_t i = 0; i < config.price_level_count(); ++i) {
            const Price price = config.min_price + static_cast<Price>(i) * config.tick_size;
            CHECK(actual.depth_at_price(Side::Buy, price) == expected.depth_at_price(Side::Buy, price));
            CHECK(actual.depth_at_price(Side::Sell, price) == expected.depth_at_price(Side::Sell, price));
            CHECK(actual.order_count_at_price(Side::Buy, price) == expected.order_count_at_price(Side::Buy, price));
            CHECK(actual.order_count_at_price(Side::Sell, price) == expected.order_count_at_price(Side::Sell, price));
        }
    }

    const std::uint32_t depth_capacity = std::max(1U, config.max_orders);
    std::vector<DepthLevel> actual_depth(depth_capacity);
    std::vector<DepthLevel> expected_depth(depth_capacity);
    for (const Side side : {Side::Buy, Side::Sell}) {
        const std::uint32_t actual_count =
            actual.depth(side, static_cast<std::uint32_t>(actual_depth.size()), actual_depth.data());
        const std::uint32_t expected_count =
            expected.depth(side, static_cast<std::uint32_t>(expected_depth.size()), expected_depth.data());
        CHECK(actual_count == expected_count);
        for (std::uint32_t i = 0; i < actual_count; ++i) {
            CHECK(actual_depth[i].price == expected_depth[i].price);
            CHECK(actual_depth[i].aggregate_quantity == expected_depth[i].aggregate_quantity);
            CHECK(actual_depth[i].order_count == expected_depth[i].order_count);
            CHECK(actual.depth_at_price(side, actual_depth[i].price) ==
                  expected.depth_at_price(side, expected_depth[i].price));
            CHECK(actual.order_count_at_price(side, actual_depth[i].price) ==
                  expected.order_count_at_price(side, expected_depth[i].price));
        }
    }

    const BestQuote bid = actual.best_bid();
    const BestQuote ask = actual.best_ask();
    if (bid.valid && ask.valid) {
        CHECK(bid.price < ask.price);
    }

    for (OrderId id = 1; id <= 192; ++id) {
        const Order* actual_order = actual.find_order(id);
        const auto* expected_order = expected.find_order(id);
        CHECK((actual_order != nullptr) == (expected_order != nullptr));

        if (actual_order != nullptr && expected_order != nullptr) {
            CHECK(actual_order->id == expected_order->id);
            CHECK(actual_order->side == expected_order->side);
            CHECK(actual_order->price == expected_order->price);
            CHECK(actual_order->quantity == expected_order->quantity);
            CHECK(actual_order->active);
        }
    }
}

constexpr std::size_t kSnapshotTestBufferSize = 131'072;

void check_book_snapshots_equal(const OrderBook& actual, const OrderBook& expected, const char* context)
{
    std::array<std::byte, kSnapshotTestBufferSize> actual_buffer{};
    std::array<std::byte, kSnapshotTestBufferSize> expected_buffer{};

    const SnapshotWriteResult actual_snapshot = serialize(actual, actual_buffer);
    const SnapshotWriteResult expected_snapshot = serialize(expected, expected_buffer);
    if (actual_snapshot.status != Status::Accepted || expected_snapshot.status != Status::Accepted) {
        std::fprintf(stderr,
                     "%s: serialize failed: actual=%u expected=%u\n",
                     context,
                     status_value(actual_snapshot.status),
                     status_value(expected_snapshot.status));
        std::abort();
    }

    if (actual_snapshot.bytes_written != expected_snapshot.bytes_written ||
        std::memcmp(actual_buffer.data(), expected_buffer.data(), actual_snapshot.bytes_written) != 0) {
        std::fprintf(stderr,
                     "%s: snapshot bytes differ: actual=%zu expected=%zu\n",
                     context,
                     actual_snapshot.bytes_written,
                     expected_snapshot.bytes_written);
        std::abort();
    }
}

void check_engine_snapshots_equal(const MatchingEngine& actual, const MatchingEngine& expected, const char* context)
{
    std::array<std::byte, kSnapshotTestBufferSize> actual_buffer{};
    std::array<std::byte, kSnapshotTestBufferSize> expected_buffer{};

    const SnapshotWriteResult actual_snapshot = serialize(actual, actual_buffer);
    const SnapshotWriteResult expected_snapshot = serialize(expected, expected_buffer);
    if (actual_snapshot.status != Status::Accepted || expected_snapshot.status != Status::Accepted) {
        std::fprintf(stderr,
                     "%s: engine serialize failed: actual=%u expected=%u\n",
                     context,
                     status_value(actual_snapshot.status),
                     status_value(expected_snapshot.status));
        std::abort();
    }

    if (actual_snapshot.bytes_written != expected_snapshot.bytes_written ||
        std::memcmp(actual_buffer.data(), expected_buffer.data(), actual_snapshot.bytes_written) != 0) {
        std::fprintf(stderr,
                     "%s: engine snapshot bytes differ: actual=%zu expected=%zu\n",
                     context,
                     actual_snapshot.bytes_written,
                     expected_snapshot.bytes_written);
        std::abort();
    }
}

class SplitMix64 final {
public:
    explicit SplitMix64(const std::uint64_t seed) noexcept : state_(seed) {}

    [[nodiscard]] std::uint64_t next() noexcept
    {
        std::uint64_t value = (state_ += 0x9e3779b97f4a7c15ULL);
        value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
        value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
        return value ^ (value >> 31U);
    }

    [[nodiscard]] std::uint32_t uniform(const std::uint32_t upper_bound) noexcept
    {
        return static_cast<std::uint32_t>(next() % upper_bound);
    }

private:
    std::uint64_t state_{0};
};

[[nodiscard]] Side random_side(SplitMix64& rng) noexcept
{
    return rng.uniform(2U) == 0U ? Side::Buy : Side::Sell;
}

[[nodiscard]] OrderId random_order_id(SplitMix64& rng) noexcept
{
    if (rng.uniform(32U) == 0U) {
        return kInvalidOrderId;
    }
    return static_cast<OrderId>(1U + rng.uniform(192U));
}

[[nodiscard]] Price random_price(SplitMix64& rng) noexcept
{
    return static_cast<Price>(88 + static_cast<int>(rng.uniform(45U)));
}

[[nodiscard]] Quantity random_quantity(SplitMix64& rng) noexcept
{
    if (rng.uniform(24U) == 0U) {
        return 0;
    }
    return static_cast<Quantity>(1U + rng.uniform(24U));
}

[[nodiscard]] Price random_sparse_price(SplitMix64& rng) noexcept
{
    constexpr std::array<Price, 20> prices{
        10,
        50'000,
        100'000,
        150'000,
        200'000,
        250'000,
        300'000,
        350'000,
        400'000,
        450'000,
        500'000,
        550'000,
        600'000,
        650'000,
        700'000,
        750'000,
        800'000,
        850'000,
        900'000,
        950'000,
    };
    return prices[rng.uniform(static_cast<std::uint32_t>(prices.size()))];
}

[[nodiscard]] std::uint64_t histogram_total(const OrderIdMapStats& stats) noexcept
{
    std::uint64_t total = 0;
    for (const std::uint64_t bucket : stats.probe_histogram) {
        total += bucket;
    }
    return total;
}

[[nodiscard]] TimeInForce random_time_in_force(SplitMix64& rng) noexcept
{
    const std::uint32_t value = rng.uniform(8U);
    if (value < 4U) {
        return TimeInForce::Gtc;
    }
    if (value < 6U) {
        return TimeInForce::Ioc;
    }
    return TimeInForce::Fok;
}

void run_add(OrderBook& actual,
             ReferenceOrderBook& expected,
             const OrderId id,
             const Side side,
             const Price price,
             const Quantity quantity,
             const Timestamp timestamp,
             const TimeInForce time_in_force = TimeInForce::Gtc)
{
    const AddOrderResult actual_result = actual.add_limit_order(id, side, price, quantity, timestamp, time_in_force);
    const AddOrderResult expected_result = expected.add_limit_order(id, side, price, quantity, timestamp, time_in_force);
    check_add_result(actual_result, expected_result, "add_limit_order");
    check_books_equal(actual, expected);
}

void run_cancel(OrderBook& actual, ReferenceOrderBook& expected, const OrderId id)
{
    const CancelResult actual_result = actual.cancel_order(id);
    const CancelResult expected_result = expected.cancel_order(id);
    check_cancel_result(actual_result, expected_result, "cancel_order");
    check_books_equal(actual, expected);
}

void run_modify(OrderBook& actual, ReferenceOrderBook& expected, const OrderId id, const Quantity quantity)
{
    const ModifyResult actual_result = actual.modify_order(id, quantity);
    const ModifyResult expected_result = expected.modify_order(id, quantity);
    check_modify_result(actual_result, expected_result, "modify_order");
    check_books_equal(actual, expected);
}

void run_replace(OrderBook& actual,
                 ReferenceOrderBook& expected,
                 const OrderId id,
                 const Price price,
                 const Quantity quantity,
                 const Timestamp timestamp = 0,
                 const TimeInForce time_in_force = TimeInForce::Gtc)
{
    const ReplaceResult actual_result = actual.replace_order(id, price, quantity, timestamp, time_in_force);
    const ReplaceResult expected_result = expected.replace_order(id, price, quantity, timestamp, time_in_force);
    check_replace_result(actual_result, expected_result, "replace_order");
    check_books_equal(actual, expected);
}

void run_market(OrderBook& actual, ReferenceOrderBook& expected, const Side side, const Quantity quantity)
{
    const MatchResult actual_result = actual.match_market_order(side, quantity);
    const MatchResult expected_result = expected.match_market_order(side, quantity);
    check_match_result(actual_result, expected_result, "match_market_order");
    check_books_equal(actual, expected);
}

void check_engine_book_equal(const MatchingEngine& actual,
                             const InstrumentId instrument_id,
                             const ReferenceOrderBook& expected)
{
    const OrderBook* book = actual.order_book(instrument_id);
    CHECK(book != nullptr);
    CHECK(book->instrument_id() == instrument_id);
    check_books_equal(*book, expected);

    const TopOfBook top = actual.top_of_book(instrument_id);
    CHECK(top.status == Status::Accepted);
    check_best_quote(top.bid, expected.best_bid(), "engine_top_bid");
    check_best_quote(top.ask, expected.best_ask(), "engine_top_ask");
}

void run_engine_add(MatchingEngine& actual,
                    ReferenceOrderBook& expected,
                    const InstrumentId instrument_id,
                    const OrderId id,
                    const Side side,
                    const Price price,
                    const Quantity quantity,
                    const Timestamp timestamp,
                    const TimeInForce time_in_force = TimeInForce::Gtc)
{
    const AddOrderResult actual_result =
        actual.add_limit_order(instrument_id, id, side, price, quantity, timestamp, time_in_force);
    const AddOrderResult expected_result =
        expected.add_limit_order(id, side, price, quantity, timestamp, time_in_force);
    check_add_result(actual_result, expected_result, "engine_add_limit_order");
    check_engine_book_equal(actual, instrument_id, expected);
}

void run_engine_cancel(MatchingEngine& actual,
                       ReferenceOrderBook& expected,
                       const InstrumentId instrument_id,
                       const OrderId id,
                       const Timestamp timestamp = 0)
{
    const CancelResult actual_result = actual.cancel(instrument_id, id, timestamp);
    const CancelResult expected_result = expected.cancel_order(id, timestamp);
    check_cancel_result(actual_result, expected_result, "engine_cancel");
    check_engine_book_equal(actual, instrument_id, expected);
}

void run_engine_modify(MatchingEngine& actual,
                       ReferenceOrderBook& expected,
                       const InstrumentId instrument_id,
                       const OrderId id,
                       const Quantity quantity,
                       const Timestamp timestamp = 0)
{
    const ModifyResult actual_result = actual.modify(instrument_id, id, quantity, timestamp);
    const ModifyResult expected_result = expected.modify_order(id, quantity, timestamp);
    check_modify_result(actual_result, expected_result, "engine_modify");
    check_engine_book_equal(actual, instrument_id, expected);
}

void run_engine_replace(MatchingEngine& actual,
                        ReferenceOrderBook& expected,
                        const InstrumentId instrument_id,
                        const OrderId id,
                        const Price price,
                        const Quantity quantity,
                        const Timestamp timestamp = 0,
                        const TimeInForce time_in_force = TimeInForce::Gtc)
{
    const ReplaceResult actual_result = actual.replace(instrument_id, id, price, quantity, timestamp, time_in_force);
    const ReplaceResult expected_result = expected.replace_order(id, price, quantity, timestamp, time_in_force);
    check_replace_result(actual_result, expected_result, "engine_replace");
    check_engine_book_equal(actual, instrument_id, expected);
}

void run_engine_market(MatchingEngine& actual,
                       ReferenceOrderBook& expected,
                       const InstrumentId instrument_id,
                       const Side side,
                       const Quantity quantity,
                       const OrderId aggressor_id = kInvalidOrderId,
                       const Timestamp timestamp = 0)
{
    const MatchResult actual_result =
        actual.match_market_order(instrument_id, side, quantity, aggressor_id, timestamp);
    const MatchResult expected_result = expected.match_market_order(side, quantity, aggressor_id, timestamp);
    check_match_result(actual_result, expected_result, "engine_match_market_order");
    check_engine_book_equal(actual, instrument_id, expected);
}

void check_command_equal(const Command& actual, const Command& expected)
{
    CHECK(actual.instrument_id == expected.instrument_id);
    CHECK(actual.op == expected.op);
    CHECK(actual.order_id == expected.order_id);
    CHECK(actual.side == expected.side);
    CHECK(actual.price == expected.price);
    CHECK(actual.quantity == expected.quantity);
    CHECK(actual.time_in_force == expected.time_in_force);
    CHECK(actual.timestamp == expected.timestamp);
}

void check_dispatch_events(const DispatchResult& actual,
                           const std::uint32_t expected_events_emitted,
                           std::span<const BookEvent> expected_events,
                           const char* context)
{
    CHECK(actual.events_emitted == expected_events_emitted);
    CHECK(actual.events_emitted == actual.events.size());
    CHECK(expected_events_emitted == expected_events.size());
    check_event_stream(actual.events, expected_events, context);
}

void check_dispatch_add_result(const DispatchResult& actual,
                               const AddOrderResult& expected,
                               const char* context)
{
    check_status(actual.status, expected.status, context);
    CHECK(actual.accepted_quantity == expected.accepted_quantity);
    CHECK(actual.executed_quantity == expected.executed_quantity);
    CHECK(actual.resting_quantity == expected.resting_quantity);
    CHECK(actual.fills == expected.fills);
    CHECK(actual.has_last_price == expected.has_last_price);
    CHECK(actual.last_price == expected.last_price);
    check_dispatch_events(actual, expected.events_emitted, expected.events, context);
}

void check_dispatch_cancel_result(const DispatchResult& actual,
                                  const CancelResult& expected,
                                  const char* context)
{
    check_status(actual.status, expected.status, context);
    CHECK(actual.canceled_quantity == expected.canceled_quantity);
    check_dispatch_events(actual, expected.events_emitted, expected.events, context);
}

void check_dispatch_modify_result(const DispatchResult& actual,
                                  const ModifyResult& expected,
                                  const char* context)
{
    check_status(actual.status, expected.status, context);
    CHECK(actual.old_quantity == expected.old_quantity);
    CHECK(actual.new_quantity == expected.new_quantity);
    check_dispatch_events(actual, expected.events_emitted, expected.events, context);
}

void check_dispatch_replace_result(const DispatchResult& actual,
                                   const ReplaceResult& expected,
                                   const char* context)
{
    check_status(actual.status, expected.status, context);
    CHECK(actual.old_price == expected.old_price);
    CHECK(actual.new_price == expected.new_price);
    CHECK(actual.old_quantity == expected.old_quantity);
    CHECK(actual.new_quantity == expected.new_quantity);
    CHECK(actual.executed_quantity == expected.executed_quantity);
    CHECK(actual.resting_quantity == expected.resting_quantity);
    CHECK(actual.fills == expected.fills);
    CHECK(actual.has_last_price == expected.has_last_price);
    CHECK(actual.last_price == expected.last_price);
    check_dispatch_events(actual, expected.events_emitted, expected.events, context);
}

void check_dispatch_match_result(const DispatchResult& actual,
                                 const MatchResult& expected,
                                 const char* context)
{
    check_status(actual.status, expected.status, context);
    CHECK(actual.requested_quantity == expected.requested_quantity);
    CHECK(actual.executed_quantity == expected.executed_quantity);
    CHECK(actual.remaining_quantity == expected.remaining_quantity);
    CHECK(actual.fills == expected.fills);
    CHECK(actual.has_last_price == expected.has_last_price);
    CHECK(actual.last_price == expected.last_price);
    check_dispatch_events(actual, expected.events_emitted, expected.events, context);
}

void check_dispatch_results_equal(const DispatchResult& actual,
                                  const DispatchResult& expected,
                                  const char* context)
{
    check_status(actual.status, expected.status, context);
    CHECK(actual.accepted_quantity == expected.accepted_quantity);
    CHECK(actual.requested_quantity == expected.requested_quantity);
    CHECK(actual.executed_quantity == expected.executed_quantity);
    CHECK(actual.remaining_quantity == expected.remaining_quantity);
    CHECK(actual.resting_quantity == expected.resting_quantity);
    CHECK(actual.canceled_quantity == expected.canceled_quantity);
    CHECK(actual.old_quantity == expected.old_quantity);
    CHECK(actual.new_quantity == expected.new_quantity);
    CHECK(actual.old_price == expected.old_price);
    CHECK(actual.new_price == expected.new_price);
    CHECK(actual.fills == expected.fills);
    CHECK(actual.has_last_price == expected.has_last_price);
    CHECK(actual.last_price == expected.last_price);
    check_dispatch_events(actual, expected.events_emitted, expected.events, context);
}

void run_dispatch_command(MatchingEngine& actual,
                          ReferenceOrderBook& expected,
                          const Command& command,
                          const char* context)
{
    const DispatchResult actual_result = actual.dispatch(command);

    switch (command.op) {
    case CommandOp::Add: {
        const AddOrderResult expected_result = expected.add_limit_order(command.order_id,
                                                                        command.side,
                                                                        command.price,
                                                                        command.quantity,
                                                                        command.timestamp,
                                                                        command.time_in_force);
        check_dispatch_add_result(actual_result, expected_result, context);
        break;
    }
    case CommandOp::Cancel: {
        const CancelResult expected_result = expected.cancel_order(command.order_id, command.timestamp);
        check_dispatch_cancel_result(actual_result, expected_result, context);
        break;
    }
    case CommandOp::Modify: {
        const ModifyResult expected_result =
            expected.modify_order(command.order_id, command.quantity, command.timestamp);
        check_dispatch_modify_result(actual_result, expected_result, context);
        break;
    }
    case CommandOp::Replace: {
        const ReplaceResult expected_result = expected.replace_order(command.order_id,
                                                                     command.price,
                                                                     command.quantity,
                                                                     command.timestamp,
                                                                     command.time_in_force);
        check_dispatch_replace_result(actual_result, expected_result, context);
        break;
    }
    case CommandOp::Market: {
        const MatchResult expected_result =
            expected.match_market_order(command.side, command.quantity, command.order_id, command.timestamp);
        check_dispatch_match_result(actual_result, expected_result, context);
        break;
    }
    }

    check_engine_book_equal(actual, command.instrument_id, expected);
}

void test_command_encode_decode_round_trip()
{
    constexpr InstrumentId instrument_id = 101;
    constexpr std::array<CommandOp, 5> ops{
        CommandOp::Add,
        CommandOp::Cancel,
        CommandOp::Modify,
        CommandOp::Replace,
        CommandOp::Market,
    };

    for (std::size_t i = 0; i < ops.size(); ++i) {
        const Command expected{instrument_id,
                               ops[i],
                               static_cast<OrderId>(10 + i),
                               i % 2U == 0U ? Side::Buy : Side::Sell,
                               static_cast<Price>(100 + static_cast<int>(i)),
                               static_cast<Quantity>(5 + i),
                               i % 3U == 0U ? TimeInForce::Gtc
                                            : (i % 3U == 1U ? TimeInForce::Ioc : TimeInForce::Fok),
                               static_cast<Timestamp>(1'000 + i)};
        std::array<std::byte, kCommandWireSize> buffer{};
        CHECK(encode(expected, buffer) == Status::Accepted);

        Command actual{};
        CHECK(decode(buffer, actual) == Status::Accepted);
        check_command_equal(actual, expected);
    }

    const Command invalid_side{instrument_id,
                               CommandOp::Add,
                               1,
                               static_cast<Side>(99U),
                               100,
                               1,
                               TimeInForce::Gtc,
                               1};
    std::array<std::byte, kCommandWireSize> buffer{};
    CHECK(encode(invalid_side, buffer) == Status::InvalidCommand);

    Command decoded{};
    CHECK(decode(std::span<const std::byte>(buffer.data(), kCommandWireSize - 1U), decoded) ==
          Status::BufferTooSmall);

    const Command valid{instrument_id, CommandOp::Add, 1, Side::Buy, 100, 1, TimeInForce::Gtc, 1};
    CHECK(encode(valid, buffer) == Status::Accepted);
    buffer[4] = static_cast<std::byte>(0xffU);
    CHECK(decode(buffer, decoded) == Status::InvalidCommand);

    CHECK(encode(valid, std::span<std::byte>(buffer.data(), kCommandWireSize - 1U)) ==
          Status::BufferTooSmall);
}

void test_command_wire_format_exact_size_and_little_endian()
{
    static_assert(kCommandWireSize == 39U);

    const Command command{0x01020304U,
                          CommandOp::Replace,
                          0x0102030405060708ULL,
                          Side::Sell,
                          0x1112131415161718LL,
                          0x2122232425262728ULL,
                          TimeInForce::Fok,
                          0x3132333435363738ULL};

    constexpr std::array<std::byte, kCommandWireSize> expected{
        byte_value(0x04), byte_value(0x03), byte_value(0x02), byte_value(0x01),
        byte_value(0x03),
        byte_value(0x08), byte_value(0x07), byte_value(0x06), byte_value(0x05),
        byte_value(0x04), byte_value(0x03), byte_value(0x02), byte_value(0x01),
        byte_value(0x01),
        byte_value(0x18), byte_value(0x17), byte_value(0x16), byte_value(0x15),
        byte_value(0x14), byte_value(0x13), byte_value(0x12), byte_value(0x11),
        byte_value(0x28), byte_value(0x27), byte_value(0x26), byte_value(0x25),
        byte_value(0x24), byte_value(0x23), byte_value(0x22), byte_value(0x21),
        byte_value(0x02),
        byte_value(0x38), byte_value(0x37), byte_value(0x36), byte_value(0x35),
        byte_value(0x34), byte_value(0x33), byte_value(0x32), byte_value(0x31),
    };

    std::array<std::byte, kCommandWireSize + 3U> buffer{};
    buffer.fill(byte_value(0xaa));
    CHECK(encode(command, buffer) == Status::Accepted);

    for (std::size_t i = 0; i < expected.size(); ++i) {
        CHECK(buffer[i] == expected[i]);
    }
    CHECK(buffer[kCommandWireSize] == byte_value(0xaa));
    CHECK(buffer[kCommandWireSize + 1U] == byte_value(0xaa));
    CHECK(buffer[kCommandWireSize + 2U] == byte_value(0xaa));

    Command decoded{};
    CHECK(decode(buffer, decoded) == Status::Accepted);
    check_command_equal(decoded, command);
}

void test_dispatch_fixed_sequence_matches_oracle()
{
    constexpr InstrumentId instrument_id = 701;
    const BookConfig config{90, 130, 128, 256, 1};
    const InstrumentConfig configs[] = {
        InstrumentConfig{instrument_id, config},
    };

    MatchingEngine actual(configs);
    ReferenceOrderBook expected(config, instrument_id);
    CHECK(actual.valid());

    std::array<Command, 64> commands{};
    std::uint32_t count = 0;

    for (std::uint32_t i = 0; i < 20U; ++i) {
        commands[count] = Command{instrument_id,
                                  CommandOp::Add,
                                  static_cast<OrderId>(1U + i),
                                  Side::Buy,
                                  static_cast<Price>(98 + static_cast<int>(i % 3U)),
                                  static_cast<Quantity>(5U + (i % 4U)),
                                  TimeInForce::Gtc,
                                  count + 1U};
        ++count;
    }
    for (std::uint32_t i = 0; i < 10U; ++i) {
        commands[count] = Command{instrument_id,
                                  CommandOp::Add,
                                  static_cast<OrderId>(100U + i),
                                  Side::Sell,
                                  static_cast<Price>(105 + static_cast<int>(i % 2U)),
                                  static_cast<Quantity>(4U + (i % 3U)),
                                  TimeInForce::Gtc,
                                  count + 1U};
        ++count;
    }
    for (std::uint32_t i = 0; i < 8U; ++i) {
        commands[count] = Command{instrument_id,
                                  CommandOp::Modify,
                                  static_cast<OrderId>(1U + i),
                                  Side::Buy,
                                  0,
                                  static_cast<Quantity>(2U + (i % 3U)),
                                  TimeInForce::Gtc,
                                  count + 1U};
        ++count;
    }
    for (std::uint32_t i = 0; i < 8U; ++i) {
        commands[count] = Command{instrument_id,
                                  CommandOp::Cancel,
                                  static_cast<OrderId>(10U + i),
                                  Side::Buy,
                                  0,
                                  0,
                                  TimeInForce::Gtc,
                                  count + 1U};
        ++count;
    }
    for (std::uint32_t i = 0; i < 10U; ++i) {
        commands[count] = Command{instrument_id,
                                  CommandOp::Replace,
                                  static_cast<OrderId>(100U + i),
                                  Side::Sell,
                                  static_cast<Price>(100 + static_cast<int>(i % 2U)),
                                  static_cast<Quantity>(3U + (i % 4U)),
                                  i % 3U == 0U ? TimeInForce::Ioc : TimeInForce::Gtc,
                                  count + 1U};
        ++count;
    }
    for (std::uint32_t i = 0; i < 8U; ++i) {
        commands[count] = Command{instrument_id,
                                  CommandOp::Market,
                                  static_cast<OrderId>(900U + i),
                                  i % 2U == 0U ? Side::Sell : Side::Buy,
                                  0,
                                  static_cast<Quantity>(1U + (i % 3U)),
                                  TimeInForce::Gtc,
                                  count + 1U};
        ++count;
    }

    CHECK(count >= 50U);
    for (std::uint32_t i = 0; i < count; ++i) {
        run_dispatch_command(actual, expected, commands[i], "dispatch_fixed_sequence");
    }

    const MatchingEngineStats stats = actual.stats();
    CHECK(stats.dispatch_count == count);
    CHECK(stats.adds == 30U);
    CHECK(stats.modifies == 8U);
    CHECK(stats.cancels == 8U);
    CHECK(stats.replaces == 10U);
    CHECK(stats.market_matches == 8U);
    CHECK(stats.event_log_high_water_mark > 0U);
}

void test_dispatch_rejects_corrupt_and_truncated_buffers()
{
    constexpr InstrumentId instrument_id = 702;
    const BookConfig config{90, 130, 16, 64, 1};
    const InstrumentConfig configs[] = {
        InstrumentConfig{instrument_id, config},
    };

    MatchingEngine engine(configs);
    CHECK(engine.valid());

    const Command valid{instrument_id, CommandOp::Add, 1, Side::Buy, 100, 5, TimeInForce::Gtc, 1};
    std::array<std::byte, kCommandWireSize> buffer{};
    CHECK(encode(valid, buffer) == Status::Accepted);

    DispatchResult result =
        engine.dispatch(std::span<const std::byte>(buffer.data(), kCommandWireSize - 1U));
    CHECK(result.status == Status::BufferTooSmall);
    CHECK(result.events_emitted == 0);
    CHECK(engine.live_order_count(instrument_id) == 0);

    buffer[4] = static_cast<std::byte>(0xffU);
    result = engine.dispatch(buffer);
    CHECK(result.status == Status::InvalidCommand);
    CHECK(result.events_emitted == 0);
    CHECK(engine.live_order_count(instrument_id) == 0);

    Command invalid_op = valid;
    invalid_op.op = static_cast<CommandOp>(99U);
    result = engine.dispatch(invalid_op);
    CHECK(result.status == Status::InvalidCommand);
    CHECK(result.events_emitted == 0);
    CHECK(engine.live_order_count(instrument_id) == 0);

    Command unknown_instrument = valid;
    unknown_instrument.instrument_id = 999;
    result = engine.dispatch(unknown_instrument);
    CHECK(result.status == Status::UnknownInstrument);
    CHECK(result.events_emitted == 0);
    CHECK(engine.live_order_count(instrument_id) == 0);

    const MatchingEngineStats stats = engine.stats();
    CHECK(stats.dispatch_count == 4U);
    CHECK(stats.adds == 1U);
    CHECK(stats.decode_errors == 2U);
    CHECK(stats.rejects == 4U);
    CHECK(stats.rejects_by_status[static_cast<std::size_t>(Status::BufferTooSmall)] == 1U);
    CHECK(stats.rejects_by_status[static_cast<std::size_t>(Status::InvalidCommand)] == 2U);
    CHECK(stats.rejects_by_status[static_cast<std::size_t>(Status::UnknownInstrument)] == 1U);
}

void test_dispatch_failure_results_have_empty_events_and_preserve_books()
{
    constexpr InstrumentId instrument_id = 704;
    const BookConfig config{90, 130, 16, 64, 1};
    const InstrumentConfig configs[] = {
        InstrumentConfig{instrument_id, config},
    };

    MatchingEngine engine(configs);
    CHECK(engine.valid());

    const AddOrderResult resting = engine.add_limit_order(instrument_id, 1, Side::Buy, 100, 5, 1);
    CHECK(resting.status == Status::Accepted);
    CHECK(resting.events_emitted == 2U);
    const std::span<const BookEvent> initial_events = engine.last_events(instrument_id);
    CHECK(initial_events.size() == 2U);
    const BookEvent* const initial_events_data = initial_events.data();

    const Command valid{instrument_id, CommandOp::Add, 2, Side::Buy, 99, 3, TimeInForce::Gtc, 2};
    std::array<std::byte, kCommandWireSize> buffer{};
    CHECK(encode(valid, buffer) == Status::Accepted);

    DispatchResult result =
        engine.dispatch(std::span<const std::byte>(buffer.data(), kCommandWireSize - 1U));
    CHECK(result.status == Status::BufferTooSmall);
    CHECK(result.events_emitted == 0U);
    CHECK(result.events.empty());
    CHECK(engine.live_order_count(instrument_id) == 1U);
    CHECK(engine.last_events(instrument_id).data() == initial_events_data);
    CHECK(engine.last_events(instrument_id).size() == initial_events.size());

    buffer[4] = byte_value(0xff);
    result = engine.dispatch(buffer);
    CHECK(result.status == Status::InvalidCommand);
    CHECK(result.events_emitted == 0U);
    CHECK(result.events.empty());
    CHECK(engine.live_order_count(instrument_id) == 1U);
    CHECK(engine.last_events(instrument_id).data() == initial_events_data);
    CHECK(engine.last_events(instrument_id).size() == initial_events.size());

    Command unknown_instrument = valid;
    unknown_instrument.instrument_id = 999;
    result = engine.dispatch(unknown_instrument);
    CHECK(result.status == Status::UnknownInstrument);
    CHECK(result.events_emitted == 0U);
    CHECK(result.events.empty());
    CHECK(engine.live_order_count(instrument_id) == 1U);
    CHECK(engine.depth_at_price(instrument_id, Side::Buy, 100) == 5U);
    CHECK(engine.find_order(instrument_id, 1) != nullptr);
    CHECK(engine.find_order(instrument_id, 2) == nullptr);
    CHECK(engine.last_events(instrument_id).data() == initial_events_data);
    CHECK(engine.last_events(instrument_id).size() == initial_events.size());
}

void test_event_log_full_policy()
{
    const BookConfig config{90, 130, 4, 16, 1, 2};
    OrderBook actual(config);
    ReferenceOrderBook expected(config);
    CHECK(actual.event_log_capacity() == 2U);

    run_add(actual, expected, 1, Side::Sell, 100, 1, 1);
    run_add(actual, expected, 2, Side::Sell, 101, 1, 2);

    const AddOrderResult actual_result = actual.add_limit_order(3, Side::Buy, 101, 2, 3);
    const AddOrderResult expected_result = expected.add_limit_order(3, Side::Buy, 101, 2, 3);
    check_add_result(actual_result, expected_result, "event_log_full_add");
    CHECK(actual_result.status == Status::EventLogFull);
    CHECK(actual_result.events_emitted == 0);
    CHECK(actual.find_order(1) != nullptr);
    CHECK(actual.find_order(2) != nullptr);
    CHECK(actual.find_order(3) == nullptr);
    CHECK(actual.depth_at_price(Side::Sell, 100) == 1U);
    CHECK(actual.depth_at_price(Side::Sell, 101) == 1U);
    check_books_equal(actual, expected);
}

void test_reference_oracle_models_price_level_quantity_overflow()
{
    const BookConfig config{-100, 100, 4, 4, 2};
    OrderBook actual(config);
    ReferenceOrderBook expected(config);

    run_add(actual,
            expected,
            1,
            Side::Sell,
            -10,
            std::numeric_limits<Quantity>::max(),
            1);

    const AddOrderResult actual_result = actual.add_limit_order(
        2, Side::Sell, -10, std::numeric_limits<Quantity>::max(), 2);
    const AddOrderResult expected_result = expected.add_limit_order(
        2, Side::Sell, -10, std::numeric_limits<Quantity>::max(), 2);
    check_add_result(actual_result, expected_result, "price_level_quantity_overflow");
    CHECK(actual_result.status == Status::PriceLevelQuantityOverflow);
    CHECK(actual_result.accepted_quantity == 0U);
    CHECK(actual_result.events_emitted == 1U);
    CHECK(actual_result.events[0].kind == BookEvent::Kind::OrderRejected);
    CHECK(actual_result.events[0].status == Status::PriceLevelQuantityOverflow);
    CHECK(actual_result.events[0].sequence == 0U);
    CHECK(actual.find_order(1) != nullptr);
    CHECK(actual.find_order(2) == nullptr);
    CHECK(actual.depth_at_price(Side::Sell, -10) ==
          std::numeric_limits<Quantity>::max());
    check_books_equal(actual, expected);
}

void test_price_level_quantity_overflow_rejection_is_atomic()
{
    constexpr std::array<PriceLevelMode, 2> modes{
        PriceLevelMode::Dense,
        PriceLevelMode::Sparse,
    };
    constexpr std::array<std::uint32_t, 2> market_data_capacities{0U, 16U};
    constexpr Quantity max_quantity = std::numeric_limits<Quantity>::max();

    for (const PriceLevelMode mode : modes) {
        for (const std::uint32_t market_data_capacity : market_data_capacities) {
            BookConfig config{};
            config.min_price = 90;
            config.max_price = 110;
            config.max_orders = 4;
            config.order_id_map_capacity = 8;
            config.tick_size = 1;
            config.event_log_capacity = 8;
            config.price_level_mode = mode;
            config.market_data_capacity = market_data_capacity;

            OrderBook book(config, 77);
            const AddOrderResult initial =
                book.add_limit_order(1, Side::Buy, 100, max_quantity, 1);
            CHECK(initial.status == Status::Accepted);
            CHECK(initial.events_emitted == 2U);

            const std::uint64_t checksum_before = book.state_checksum();
            const std::uint32_t live_orders_before = book.live_order_count();
            const SequenceNumber fifo_sequence_before = book.fifo_sequence();
            const SequenceNumber event_sequence_before = book.event_sequence();
            const SequenceNumber market_data_sequence_before =
                book.market_data_sequence();

            const AddOrderResult rejected =
                book.add_limit_order(2, Side::Buy, 100, 1, 2);
            CHECK(rejected.status == Status::PriceLevelQuantityOverflow);
            CHECK(rejected.accepted_quantity == 0U);
            CHECK(rejected.executed_quantity == 0U);
            CHECK(rejected.resting_quantity == 0U);
            CHECK(rejected.events_emitted == 1U);
            CHECK(rejected.events.size() == 1U);
            check_order_event_fields(rejected.events[0],
                                     BookEvent::Kind::OrderRejected,
                                     Status::PriceLevelQuantityOverflow,
                                     2,
                                     Side::Buy,
                                     100,
                                     1,
                                     2,
                                     77);
            CHECK(rejected.events[0].sequence == 0U);
            CHECK(book.last_events().size() == 1U);
            CHECK(book.last_market_data_events().empty());

            CHECK(book.state_checksum() == checksum_before);
            CHECK(book.live_order_count() == live_orders_before);
            CHECK(book.find_order(1) != nullptr);
            CHECK(book.find_order(2) == nullptr);
            CHECK(book.depth_at_price(Side::Buy, 100) == max_quantity);
            CHECK(book.order_count_at_price(Side::Buy, 100) == 1U);
            CHECK(book.fifo_sequence() == fifo_sequence_before);
            CHECK(book.event_sequence() == event_sequence_before);
            CHECK(book.market_data_sequence() == market_data_sequence_before);

            const AddOrderResult following =
                book.add_limit_order(3, Side::Buy, 99, 1, 3);
            CHECK(following.status == Status::Accepted);
            CHECK(following.events[0].sequence == event_sequence_before + 1U);
            CHECK(book.fifo_sequence() == fifo_sequence_before + 1U);
        }
    }
}

[[nodiscard]] Price random_valid_price(SplitMix64& rng) noexcept
{
    return static_cast<Price>(90 + static_cast<int>(rng.uniform(41U)));
}

[[nodiscard]] Quantity random_nonzero_quantity(SplitMix64& rng) noexcept
{
    return static_cast<Quantity>(1U + rng.uniform(16U));
}

[[nodiscard]] OrderId random_valid_command_order_id(SplitMix64& rng) noexcept
{
    return static_cast<OrderId>(1U + rng.uniform(96U));
}

void test_dispatch_seeded_replay_stream_matches_oracle()
{
    constexpr InstrumentId instrument_id = 703;
    const BookConfig config{90, 130, 128, 256, 1};
    const InstrumentConfig configs[] = {
        InstrumentConfig{instrument_id, config},
    };

    MatchingEngine actual(configs);
    ReferenceOrderBook expected(config, instrument_id);
    SplitMix64 rng(0x4449535041544348ULL);

    for (std::uint32_t step = 0; step < 750U; ++step) {
        const std::uint32_t action = rng.uniform(100U);
        Command command{};
        command.instrument_id = instrument_id;
        command.order_id = random_valid_command_order_id(rng);
        command.side = random_side(rng);
        command.price = random_valid_price(rng);
        command.quantity = random_nonzero_quantity(rng);
        command.time_in_force = TimeInForce::Gtc;
        command.timestamp = static_cast<Timestamp>(step + 1U);

        if (action < 45U) {
            command.op = CommandOp::Add;
            command.time_in_force = random_time_in_force(rng);
        } else if (action < 60U) {
            command.op = CommandOp::Cancel;
        } else if (action < 75U) {
            command.op = CommandOp::Modify;
        } else if (action < 90U) {
            command.op = CommandOp::Replace;
            command.time_in_force = random_time_in_force(rng);
        } else {
            command.op = CommandOp::Market;
            command.order_id = static_cast<OrderId>(800'000U + step);
        }

        run_dispatch_command(actual, expected, command, "dispatch_seeded_replay");
    }

    const MatchingEngineStats stats = actual.stats();
    CHECK(stats.dispatch_count == 750U);
    CHECK(stats.adds > 0U);
    CHECK(stats.cancels > 0U);
    CHECK(stats.modifies > 0U);
    CHECK(stats.replaces > 0U);
    CHECK(stats.market_matches > 0U);
    CHECK(stats.event_log_high_water_mark > 0U);
}

void test_fifo_and_price_priority()
{
    const BookConfig config{90, 110, 16, 64, 1};
    OrderBook actual(config);
    ReferenceOrderBook expected(config);

    run_add(actual, expected, 1, Side::Sell, 100, 5, 1);
    run_add(actual, expected, 2, Side::Sell, 100, 7, 2);
    run_add(actual, expected, 3, Side::Sell, 101, 4, 3);
    run_market(actual, expected, Side::Buy, 6);

    const Order* order_one = actual.find_order(1);
    const Order* order_two = actual.find_order(2);
    CHECK(order_one == nullptr);
    CHECK(order_two != nullptr);
    CHECK(order_two->quantity == 6);
    CHECK(actual.best_ask().price == 100);
    CHECK(actual.best_ask().quantity == 6);
    CHECK(actual.best_ask().order_count == 1);
}

void test_reduce_keeps_priority_and_increase_rejects()
{
    const BookConfig config{90, 110, 16, 64, 1};
    OrderBook actual(config);
    ReferenceOrderBook expected(config);

    run_add(actual, expected, 1, Side::Sell, 100, 10, 1);
    run_add(actual, expected, 2, Side::Sell, 100, 10, 2);
    run_modify(actual, expected, 1, 5);
    run_modify(actual, expected, 1, 6);
    run_market(actual, expected, Side::Buy, 6);

    CHECK(actual.find_order(1) == nullptr);
    const Order* order_two = actual.find_order(2);
    CHECK(order_two != nullptr);
    CHECK(order_two->quantity == 9);
}

void test_replace_price_change_loses_priority()
{
    const BookConfig config{90, 110, 16, 64, 1};
    OrderBook actual(config);
    ReferenceOrderBook expected(config);

    run_add(actual, expected, 1, Side::Buy, 100, 5, 1);
    run_add(actual, expected, 2, Side::Buy, 101, 5, 2);
    run_replace(actual, expected, 2, 100, 5, 3);
    run_add(actual, expected, 3, Side::Sell, 100, 6, 4);

    CHECK(actual.find_order(1) == nullptr);
    const Order* replaced = actual.find_order(2);
    CHECK(replaced != nullptr);
    CHECK(replaced->price == 100);
    CHECK(replaced->quantity == 4);
    CHECK(actual.best_bid().valid);
    CHECK(actual.best_bid().price == 100);
    CHECK(actual.best_bid().quantity == 4);
    CHECK(actual.best_bid().order_count == 1);
}

void test_replace_crossing_price_executes_before_resting()
{
    const BookConfig config{90, 110, 16, 64, 1};
    OrderBook actual(config);
    ReferenceOrderBook expected(config);

    run_add(actual, expected, 1, Side::Sell, 100, 5, 1);
    run_add(actual, expected, 2, Side::Buy, 99, 8, 2);

    const ReplaceResult actual_result = actual.replace_order(2, 100, 8, 3, TimeInForce::Gtc);
    const ReplaceResult expected_result = expected.replace_order(2, 100, 8, 3, TimeInForce::Gtc);
    check_replace_result(actual_result, expected_result, "replace_crossing");
    check_books_equal(actual, expected);

    CHECK(actual_result.status == Status::PartiallyFilled);
    CHECK(actual_result.executed_quantity == 5);
    CHECK(actual_result.resting_quantity == 3);
    CHECK(actual_result.fills == 1);
    CHECK(actual_result.events_emitted == 4);
    check_order_event_fields(
        actual_result.events[0], BookEvent::Kind::OrderCancelled, Status::Cancelled, 2, Side::Buy, 99, 8, 3);
    check_order_event_fields(
        actual_result.events[1], BookEvent::Kind::OrderAccepted, Status::Accepted, 2, Side::Buy, 100, 8, 3);
    CHECK(actual_result.events[1].old_quantity == 8);
    CHECK(actual_result.events[1].new_quantity == 8);
    check_trade_event_fields(actual_result.events[2], 2, 1, Side::Buy, 100, 5, 3);
    check_order_event_fields(
        actual_result.events[3], BookEvent::Kind::OrderResting, Status::Accepted, 2, Side::Buy, 100, 3, 3);
    CHECK(actual_result.events[3].old_quantity == 8);
    CHECK(actual_result.events[3].new_quantity == 8);
    CHECK(actual_result.events[1].sequence == actual_result.events[0].sequence + 1U);
    CHECK(actual_result.events[2].sequence == actual_result.events[1].sequence + 1U);
    CHECK(actual_result.events[3].sequence == actual_result.events[2].sequence + 1U);

    const Order* replaced = actual.find_order(2);
    CHECK(replaced != nullptr);
    CHECK(replaced->price == 100);
    CHECK(replaced->quantity == 3);
    CHECK(actual.find_order(1) == nullptr);
}

void test_replace_reuses_existing_order_slot_when_pool_full()
{
    const BookConfig config{90, 110, 2, 16, 1};
    OrderBook actual(config);
    ReferenceOrderBook expected(config);

    run_add(actual, expected, 1, Side::Buy, 99, 5, 1);
    run_add(actual, expected, 2, Side::Buy, 98, 7, 2);

    const ReplaceResult actual_result = actual.replace_order(1, 97, 5, 3, TimeInForce::Gtc);
    const ReplaceResult expected_result = expected.replace_order(1, 97, 5, 3, TimeInForce::Gtc);
    check_replace_result(actual_result, expected_result, "replace_reuse_pool_slot");
    check_books_equal(actual, expected);

    CHECK(actual_result.status == Status::Accepted);
    CHECK(actual_result.executed_quantity == 0);
    CHECK(actual_result.resting_quantity == 5);
    CHECK(actual_result.events_emitted == 3);
    check_order_event_fields(
        actual_result.events[0], BookEvent::Kind::OrderCancelled, Status::Cancelled, 1, Side::Buy, 99, 5, 3);
    check_order_event_fields(
        actual_result.events[1], BookEvent::Kind::OrderAccepted, Status::Accepted, 1, Side::Buy, 97, 5, 3);
    check_order_event_fields(
        actual_result.events[2], BookEvent::Kind::OrderResting, Status::Accepted, 1, Side::Buy, 97, 5, 3);
    CHECK(actual_result.events[1].old_quantity == 5);
    CHECK(actual_result.events[1].new_quantity == 5);
    CHECK(actual_result.events[2].old_quantity == 5);
    CHECK(actual_result.events[2].new_quantity == 5);

    const Order* first = actual.find_order(1);
    const Order* second = actual.find_order(2);
    CHECK(first != nullptr);
    CHECK(first->price == 97);
    CHECK(first->quantity == 5);
    CHECK(second != nullptr);
    CHECK(second->price == 98);
    CHECK(second->quantity == 7);
    CHECK(actual.live_order_count() == 2);
    CHECK(actual.best_bid().price == 98);
    CHECK(actual.depth_at_price(Side::Buy, 99) == 0);
    CHECK(actual.depth_at_price(Side::Buy, 98) == 7);
    CHECK(actual.depth_at_price(Side::Buy, 97) == 5);
}

void test_replace_reuses_existing_order_id_when_id_map_full()
{
    const BookConfig config{90, 110, 4, 2, 1};
    OrderBook actual(config);
    ReferenceOrderBook expected(config);

    run_add(actual, expected, 1, Side::Buy, 99, 5, 1);
    run_add(actual, expected, 2, Side::Buy, 98, 7, 2);

    const OrderBookStats before_stats = actual.stats();
    CHECK(before_stats.order_id_map.size == before_stats.order_id_map.capacity);

    const ReplaceResult actual_result = actual.replace_order(1, 97, 5, 3, TimeInForce::Gtc);
    const ReplaceResult expected_result = expected.replace_order(1, 97, 5, 3, TimeInForce::Gtc);
    check_replace_result(actual_result, expected_result, "replace_reuse_order_id");
    check_books_equal(actual, expected);

    CHECK(actual_result.status == Status::Accepted);
    CHECK(actual_result.resting_quantity == 5);
    CHECK(actual_result.events_emitted == 3);
    check_order_event_fields(
        actual_result.events[0], BookEvent::Kind::OrderCancelled, Status::Cancelled, 1, Side::Buy, 99, 5, 3);
    check_order_event_fields(
        actual_result.events[1], BookEvent::Kind::OrderAccepted, Status::Accepted, 1, Side::Buy, 97, 5, 3);
    check_order_event_fields(
        actual_result.events[2], BookEvent::Kind::OrderResting, Status::Accepted, 1, Side::Buy, 97, 5, 3);

    const Order* first = actual.find_order(1);
    CHECK(first != nullptr);
    CHECK(first->price == 97);
    CHECK(first->quantity == 5);
    CHECK(actual.find_order(2) != nullptr);
    CHECK(actual.stats().order_id_map.size == before_stats.order_id_map.size);
}

void test_sparse_replace_reuses_freed_level_when_level_storage_full()
{
    const BookConfig config{1, 1'000, 3, 16, 1, 0, PriceLevelMode::Sparse};
    OrderBook actual(config);
    ReferenceOrderBook expected(config);

    run_add(actual, expected, 1, Side::Buy, 100, 5, 1);
    run_add(actual, expected, 2, Side::Buy, 200, 7, 2);
    run_add(actual, expected, 3, Side::Buy, 300, 9, 3);

    OrderBookStats stats = actual.stats();
    CHECK(stats.bids.mode == PriceLevelMode::Sparse);
    CHECK(stats.bids.level_storage_capacity == config.max_orders);
    CHECK(stats.bids.occupied_level_count == stats.bids.level_storage_capacity);

    const ReplaceResult actual_result = actual.replace_order(1, 400, 5, 4, TimeInForce::Gtc);
    const ReplaceResult expected_result = expected.replace_order(1, 400, 5, 4, TimeInForce::Gtc);
    check_replace_result(actual_result, expected_result, "sparse_replace_reuses_freed_level");
    check_books_equal(actual, expected);

    CHECK(actual_result.status == Status::Accepted);
    CHECK(actual_result.executed_quantity == 0U);
    CHECK(actual_result.resting_quantity == 5U);
    CHECK(actual_result.events_emitted == 3U);
    CHECK(actual.depth_at_price(Side::Buy, 100) == 0U);
    CHECK(actual.depth_at_price(Side::Buy, 200) == 7U);
    CHECK(actual.depth_at_price(Side::Buy, 300) == 9U);
    CHECK(actual.depth_at_price(Side::Buy, 400) == 5U);
    CHECK(actual.find_order(1) != nullptr);
    CHECK(actual.find_order(1)->price == 400);
    CHECK(actual.live_order_count() == 3U);

    stats = actual.stats();
    CHECK(stats.bids.occupied_level_count == stats.bids.level_storage_capacity);
    CHECK(stats.bids.occupied_level_count == 3U);
}

void test_replace_rejects_when_residual_level_cannot_store_quantity()
{
    const BookConfig config{90, 110, 3, 16, 1};
    OrderBook actual(config);
    ReferenceOrderBook expected(config);
    constexpr Quantity max_quantity = std::numeric_limits<Quantity>::max();

    run_add(actual, expected, 1, Side::Buy, 100, 5, 1);
    run_add(actual, expected, 2, Side::Buy, 101, max_quantity - 5U, 2);

    const SequenceNumber fifo_sequence_before = actual.fifo_sequence();
    const SequenceNumber event_sequence_before = actual.event_sequence();
    const std::uint64_t checksum_before = actual.state_checksum();
    const ReplaceResult actual_result = actual.replace_order(1, 101, 6, 3, TimeInForce::Gtc);
    const ReplaceResult expected_result = expected.replace_order(1, 101, 6, 3, TimeInForce::Gtc);
    check_replace_result(actual_result, expected_result, "replace_residual_quantity_capacity");
    check_books_equal(actual, expected);

    CHECK(actual_result.status == Status::PriceLevelQuantityOverflow);
    CHECK(actual_result.events_emitted == 1);
    check_order_event_fields(
        actual_result.events[0],
        BookEvent::Kind::OrderRejected,
        Status::PriceLevelQuantityOverflow,
        1,
        Side::Buy,
        101,
        6,
        3);
    CHECK(actual_result.events[0].sequence == 0U);
    CHECK(actual_result.events[0].old_quantity == 5);
    CHECK(actual_result.events[0].new_quantity == 6);

    const Order* first = actual.find_order(1);
    const Order* second = actual.find_order(2);
    CHECK(first != nullptr);
    CHECK(first->price == 100);
    CHECK(first->quantity == 5);
    CHECK(second != nullptr);
    CHECK(second->price == 101);
    CHECK(second->quantity == max_quantity - 5U);
    CHECK(actual.live_order_count() == 2);
    CHECK(actual.depth_at_price(Side::Buy, 100) == 5);
    CHECK(actual.depth_at_price(Side::Buy, 101) == max_quantity - 5U);
    CHECK(actual.fifo_sequence() == fifo_sequence_before);
    CHECK(actual.event_sequence() == event_sequence_before);
    CHECK(actual.state_checksum() == checksum_before);
}

void test_replace_event_log_full_preserves_book()
{
    const BookConfig config{90, 110, 4, 16, 1, 3};
    OrderBook actual(config);
    ReferenceOrderBook expected(config);

    run_add(actual, expected, 1, Side::Sell, 100, 5, 1);
    run_add(actual, expected, 2, Side::Buy, 99, 8, 2);

    const ReplaceResult actual_result = actual.replace_order(2, 100, 8, 3, TimeInForce::Gtc);
    const ReplaceResult expected_result = expected.replace_order(2, 100, 8, 3, TimeInForce::Gtc);
    check_replace_result(actual_result, expected_result, "replace_event_log_full");
    check_books_equal(actual, expected);

    CHECK(actual_result.status == Status::EventLogFull);
    CHECK(actual_result.events_emitted == 0);
    CHECK(actual.find_order(1) != nullptr);
    CHECK(actual.find_order(1)->price == 100);
    CHECK(actual.find_order(1)->quantity == 5);
    CHECK(actual.find_order(2) != nullptr);
    CHECK(actual.find_order(2)->price == 99);
    CHECK(actual.find_order(2)->quantity == 8);
    CHECK(actual.live_order_count() == 2);
    CHECK(actual.depth_at_price(Side::Sell, 100) == 5);
    CHECK(actual.depth_at_price(Side::Buy, 99) == 8);
    CHECK(actual.depth_at_price(Side::Buy, 100) == 0);
}

void test_replace_ioc_and_fok_paths()
{
    {
        const BookConfig config{90, 110, 16, 64, 1};
        OrderBook actual(config);
        ReferenceOrderBook expected(config);

        run_add(actual, expected, 1, Side::Sell, 100, 5, 1);
        run_add(actual, expected, 2, Side::Buy, 99, 8, 2);

        const ReplaceResult actual_result = actual.replace_order(2, 100, 8, 3, TimeInForce::Fok);
        const ReplaceResult expected_result = expected.replace_order(2, 100, 8, 3, TimeInForce::Fok);
        check_replace_result(actual_result, expected_result, "replace_fok_reject");
        check_books_equal(actual, expected);

        CHECK(actual_result.status == Status::FokRejected);
        CHECK(actual_result.events_emitted == 1);
        check_order_event_fields(
            actual_result.events[0], BookEvent::Kind::OrderRejected, Status::FokRejected, 2, Side::Buy, 100, 8, 3);
        CHECK(actual.find_order(1) != nullptr);
        CHECK(actual.find_order(1)->quantity == 5);
        CHECK(actual.find_order(2) != nullptr);
        CHECK(actual.find_order(2)->price == 99);
        CHECK(actual.find_order(2)->quantity == 8);

        const ReplaceResult accepted_actual = actual.replace_order(2, 100, 5, 4, TimeInForce::Fok);
        const ReplaceResult accepted_expected = expected.replace_order(2, 100, 5, 4, TimeInForce::Fok);
        check_replace_result(accepted_actual, accepted_expected, "replace_fok_accept");
        check_books_equal(actual, expected);

        CHECK(accepted_actual.status == Status::Filled);
        CHECK(accepted_actual.executed_quantity == 5);
        CHECK(accepted_actual.resting_quantity == 0);
        CHECK(accepted_actual.events_emitted == 3);
        check_order_event_fields(
            accepted_actual.events[0], BookEvent::Kind::OrderCancelled, Status::Cancelled, 2, Side::Buy, 99, 8, 4);
        check_order_event_fields(
            accepted_actual.events[1], BookEvent::Kind::OrderAccepted, Status::Accepted, 2, Side::Buy, 100, 5, 4);
        CHECK(accepted_actual.events[1].time_in_force == TimeInForce::Fok);
        check_trade_event_fields(accepted_actual.events[2], 2, 1, Side::Buy, 100, 5, 4);
        CHECK(actual.find_order(1) == nullptr);
        CHECK(actual.find_order(2) == nullptr);
    }

    {
        const BookConfig config{90, 110, 16, 64, 1};
        OrderBook actual(config);
        ReferenceOrderBook expected(config);

        run_add(actual, expected, 1, Side::Sell, 100, 5, 1);
        run_add(actual, expected, 2, Side::Buy, 99, 8, 2);

        const ReplaceResult actual_result = actual.replace_order(2, 100, 8, 3, TimeInForce::Ioc);
        const ReplaceResult expected_result = expected.replace_order(2, 100, 8, 3, TimeInForce::Ioc);
        check_replace_result(actual_result, expected_result, "replace_ioc_partial");
        check_books_equal(actual, expected);

        CHECK(actual_result.status == Status::PartiallyFilled);
        CHECK(actual_result.executed_quantity == 5);
        CHECK(actual_result.resting_quantity == 0);
        CHECK(actual_result.events_emitted == 4);
        check_order_event_fields(
            actual_result.events[0], BookEvent::Kind::OrderCancelled, Status::Cancelled, 2, Side::Buy, 99, 8, 3);
        check_order_event_fields(
            actual_result.events[1], BookEvent::Kind::OrderAccepted, Status::Accepted, 2, Side::Buy, 100, 8, 3);
        CHECK(actual_result.events[1].time_in_force == TimeInForce::Ioc);
        check_trade_event_fields(actual_result.events[2], 2, 1, Side::Buy, 100, 5, 3);
        check_order_event_fields(
            actual_result.events[3], BookEvent::Kind::OrderCancelled, Status::Cancelled, 2, Side::Buy, 100, 3, 3);
        CHECK(actual_result.events[3].time_in_force == TimeInForce::Ioc);
        CHECK(actual.find_order(1) == nullptr);
        CHECK(actual.find_order(2) == nullptr);
    }

    {
        const BookConfig config{90, 110, 16, 64, 1};
        OrderBook actual(config);
        ReferenceOrderBook expected(config);

        run_add(actual, expected, 2, Side::Buy, 99, 8, 1);

        const ReplaceResult actual_result = actual.replace_order(2, 100, 8, 2, TimeInForce::Ioc);
        const ReplaceResult expected_result = expected.replace_order(2, 100, 8, 2, TimeInForce::Ioc);
        check_replace_result(actual_result, expected_result, "replace_ioc_no_liquidity");
        check_books_equal(actual, expected);

        CHECK(actual_result.status == Status::NoLiquidity);
        CHECK(actual_result.executed_quantity == 0);
        CHECK(actual_result.resting_quantity == 0);
        CHECK(actual_result.events_emitted == 3);
        check_order_event_fields(
            actual_result.events[0], BookEvent::Kind::OrderCancelled, Status::Cancelled, 2, Side::Buy, 99, 8, 2);
        check_order_event_fields(
            actual_result.events[1], BookEvent::Kind::OrderAccepted, Status::Accepted, 2, Side::Buy, 100, 8, 2);
        check_order_event_fields(
            actual_result.events[2], BookEvent::Kind::OrderCancelled, Status::Cancelled, 2, Side::Buy, 100, 8, 2);
        CHECK(actual_result.events[2].time_in_force == TimeInForce::Ioc);
        CHECK(actual.find_order(2) == nullptr);
        CHECK(actual.live_order_count() == 0);
    }
}

void test_modify_reduce_equivalent_to_replace_reduce()
{
    const BookConfig config{90, 110, 16, 64, 1};
    OrderBook modified(config);
    OrderBook replaced(config);

    CHECK(modified.add_limit_order(1, Side::Sell, 100, 10, 1).status == Status::Accepted);
    CHECK(modified.add_limit_order(2, Side::Sell, 100, 10, 2).status == Status::Accepted);
    CHECK(replaced.add_limit_order(1, Side::Sell, 100, 10, 1).status == Status::Accepted);
    CHECK(replaced.add_limit_order(2, Side::Sell, 100, 10, 2).status == Status::Accepted);

    const ModifyResult modify = modified.modify_order(1, 5, 3);
    const ReplaceResult replace = replaced.replace_order(1, 100, 5, 3, TimeInForce::Gtc);

    CHECK(modify.status == replace.status);
    CHECK(modify.old_quantity == replace.old_quantity);
    CHECK(modify.new_quantity == replace.new_quantity);
    check_event_stream(modify.events, replace.events, "modify_replace_reduce_events");

    const MatchResult modified_match = modified.match_market_order(Side::Buy, 6, 99, 4);
    const MatchResult replaced_match = replaced.match_market_order(Side::Buy, 6, 99, 4);
    check_match_result(modified_match, replaced_match, "modify_replace_reduce_match");

    CHECK(modified.find_order(1) == nullptr);
    CHECK(replaced.find_order(1) == nullptr);
    CHECK(modified.find_order(2) != nullptr);
    CHECK(replaced.find_order(2) != nullptr);
    CHECK(modified.find_order(2)->quantity == replaced.find_order(2)->quantity);
}

void test_pool_exhaustion_preserves_resting_liquidity()
{
    const BookConfig config{90, 110, 1, 16, 1};
    OrderBook actual(config);
    ReferenceOrderBook expected(config);

    run_add(actual, expected, 1, Side::Buy, 90, 10, 1);
    run_add(actual, expected, 2, Side::Sell, 90, 20, 2);

    const Order* resting_order = actual.find_order(1);
    CHECK(resting_order != nullptr);
    CHECK(resting_order->quantity == 10);
    CHECK(actual.live_order_count() == 1);
    CHECK(actual.depth_at_price(Side::Buy, 90) == 10);

    run_add(actual, expected, 2, Side::Sell, 90, 10, 3);
    CHECK(actual.live_order_count() == 0);
    CHECK(!actual.best_bid().valid);
    CHECK(!actual.best_ask().valid);
}

void test_order_id_map_full_preflight()
{
    const BookConfig config{90, 110, 4, 2, 1};
    OrderBook actual(config);
    ReferenceOrderBook expected(config);

    run_add(actual, expected, 1, Side::Buy, 90, 1, 1);
    run_add(actual, expected, 2, Side::Buy, 90, 1, 2);
    run_add(actual, expected, 3, Side::Buy, 91, 1, 3);

    CHECK(actual.find_order(1) != nullptr);
    CHECK(actual.find_order(2) != nullptr);
    CHECK(actual.find_order(3) == nullptr);
    CHECK(actual.live_order_count() == 2);
    CHECK(actual.depth_at_price(Side::Buy, 90) == 2);

    run_add(actual, expected, 3, Side::Sell, 90, 2, 4);
    CHECK(actual.live_order_count() == 0);
    CHECK(!actual.best_bid().valid);
    CHECK(!actual.best_ask().valid);
}

void test_order_id_map_capacity_for_saturates()
{
    constexpr std::uint32_t max_power_of_two = OrderIdMap::kMaxCapacity;
    static_assert(OrderIdMap::capacity_for(0U) == 2U);
    static_assert(OrderIdMap::capacity_for(1U) == 2U);
    static_assert(OrderIdMap::capacity_for(2U) == 2U);
    static_assert(OrderIdMap::capacity_for(3U) == 4U);
    static_assert(OrderIdMap::capacity_for(17U) == 32U);
    static_assert(OrderIdMap::capacity_for(max_power_of_two) == max_power_of_two);
    static_assert(OrderIdMap::capacity_for(max_power_of_two + 1U) == max_power_of_two);
    static_assert(OrderIdMap::capacity_for(std::numeric_limits<std::uint32_t>::max()) == max_power_of_two);

    CHECK(OrderIdMap::capacity_for(std::numeric_limits<std::uint32_t>::max()) == max_power_of_two);
}

void test_order_id_map_probe_stats()
{
    OrderIdMap map(8);
    Order first{};
    Order second{};
    first.reset(1, Side::Buy, 100, 10);
    second.reset(2, Side::Sell, 101, 20);

    CHECK(histogram_total(map.stats()) == 0);
    CHECK(map.insert(first.id, &first) == Status::Accepted);
    OrderIdMapStats stats = map.stats();
    CHECK(stats.size == 1);
    CHECK(stats.capacity == OrderIdMap::capacity_for(8));
    CHECK(stats.tombstones == 0);
    CHECK(stats.last_probe_count > 0);
    CHECK(histogram_total(stats) == 1);

    CHECK(map.find(first.id) == &first);
    stats = map.stats();
    CHECK(stats.last_probe_count > 0);
    CHECK(histogram_total(stats) == 2);

    CHECK(map.insert(second.id, &second) == Status::Accepted);
    stats = map.stats();
    CHECK(stats.size == 2);
    CHECK(histogram_total(stats) == 3);

    CHECK(map.find(999) == nullptr);
    stats = map.stats();
    CHECK(stats.last_probe_count > 0);
    CHECK(histogram_total(stats) == 4);

    CHECK(map.erase(first.id));
    stats = map.stats();
    CHECK(stats.size == 1);
    CHECK(stats.tombstones == 0);
    CHECK(histogram_total(stats) == 5);
}

void test_order_id_map_backward_shift_deletion()
{
    // These IDs collide in an eight-slot map under OrderIdMap's splitmix64
    // hash. Erasing from the middle exercises backward shifting, including
    // preservation of every displaced entry's lookup path.
    constexpr std::array<OrderId, 6> ids{9, 19, 20, 24, 28, 39};
    std::array<Order, ids.size()> orders{};
    OrderIdMap map(8);

    for (std::size_t i = 0; i < ids.size(); ++i) {
        orders[i].reset(ids[i], Side::Buy, 100, 1);
        CHECK(map.insert(ids[i], &orders[i]) == Status::Accepted);
    }

    CHECK(map.erase(ids[2]));
    // Three probes locate ids[2], then three displaced entries are shifted.
    CHECK(map.stats().last_probe_count == 6U);
    CHECK(map.find(ids[2]) == nullptr);
    for (std::size_t i = 0; i < ids.size(); ++i) {
        if (i != 2U) {
            CHECK(map.find(ids[i]) == &orders[i]);
        }
    }
    CHECK(map.tombstones() == 0U);
    CHECK(map.stats().tombstones == 0U);

    CHECK(map.insert(ids[2], &orders[2]) == Status::Accepted);
    CHECK(map.find(ids[2]) == &orders[2]);

    // Repeated erasure must return the map to truly empty slots. A missing
    // lookup then terminates at its first probe instead of scanning tombstones.
    for (const OrderId id : ids) {
        CHECK(map.erase(id));
    }
    CHECK(map.size() == 0U);
    CHECK(map.find(999) == nullptr);
    CHECK(map.stats().last_probe_count == 1U);
}

void test_order_id_map_robin_hood_early_miss_and_full_table_bound()
{
    // The first four ids hash to bucket four. The final id hashes to bucket
    // zero and therefore forms a Robin Hood cluster boundary after wraparound.
    constexpr std::array<OrderId, 5> early_miss_ids{9, 19, 20, 24, 6};
    std::array<Order, early_miss_ids.size()> early_miss_orders{};
    OrderIdMap early_miss_map(8);
    for (std::size_t i = 0; i < early_miss_ids.size(); ++i) {
        early_miss_orders[i].reset(early_miss_ids[i], Side::Buy, 100, 1);
        CHECK(early_miss_map.insert(early_miss_ids[i], &early_miss_orders[i]) ==
              Status::Accepted);
    }

    // Order id 28 also hashes to bucket four. The lookup stops at bucket zero
    // when the resident's probe distance drops from three to zero, one slot
    // before the following empty bucket.
    CHECK(early_miss_map.find(28) == nullptr);
    CHECK(early_miss_map.stats().last_probe_count == 5U);

    // A completely full same-bucket cluster has no earlier proof of absence;
    // the negative lookup is nevertheless capped at exactly table capacity.
    constexpr std::array<OrderId, 8> full_ids{9, 19, 20, 24, 28, 39, 55, 59};
    std::array<Order, full_ids.size()> full_orders{};
    OrderIdMap full_map(8);
    for (std::size_t i = 0; i < full_ids.size(); ++i) {
        full_orders[i].reset(full_ids[i], Side::Sell, 101, 1);
        CHECK(full_map.insert(full_ids[i], &full_orders[i]) == Status::Accepted);
    }
    CHECK(full_map.full());
    CHECK(full_map.find(67) == nullptr);
    CHECK(full_map.stats().last_probe_count == full_map.capacity());

    // Reuse the located slot instead of probing for the id again. The one
    // lookup sample is replaced by lookup-plus-seven-shift work, rather than
    // adding a second histogram sample for erase.
    const std::uint64_t samples_before = histogram_total(full_map.stats());
    const OrderIdMap::EraseToken token = full_map.find_for_erase(full_ids[0]);
    CHECK(token.order() == &full_orders[0]);
    CHECK(full_map.erase(token));
    const OrderIdMapStats erase_stats = full_map.stats();
    CHECK(erase_stats.last_probe_count == full_map.capacity());
    CHECK(histogram_total(erase_stats) == samples_before + 1U);
    for (std::size_t i = 1; i < full_ids.size(); ++i) {
        CHECK(full_map.find(full_ids[i]) == &full_orders[i]);
    }
}

void test_cancel_best_level_dense_boundaries()
{
    const BookConfig config{-4'096, 4'095, 8, 16, 1, 32, PriceLevelMode::Dense};
    OrderBook bid_book(config);
    OrderBook ask_book(config);

    CHECK(bid_book.add_limit_order(1, Side::Buy, -4'000, 1, 1).status ==
          Status::Accepted);
    CHECK(bid_book.add_limit_order(2, Side::Buy, 4'000, 1, 2).status ==
          Status::Accepted);
    CHECK(ask_book.add_limit_order(3, Side::Sell, -3'999, 1, 3).status ==
          Status::Accepted);
    CHECK(ask_book.add_limit_order(4, Side::Sell, 3'999, 1, 4).status ==
          Status::Accepted);

    // Removing the sole order at each best level forces dense occupancy-word
    // discovery toward the opposite end of the configured 128-word bitset.
    CHECK(bid_book.cancel_order(2, 5).status == Status::Cancelled);
    CHECK(bid_book.best_bid().valid);
    CHECK(bid_book.best_bid().price == -4'000);
    CHECK(ask_book.cancel_order(3, 6).status == Status::Cancelled);
    CHECK(ask_book.best_ask().valid);
    CHECK(ask_book.best_ask().price == 3'999);
}

void test_cancel_sparse_final_level_maintains_sorted_slots()
{
    BookConfig config{1, 1'000'000, 8, 16, 1, 32, PriceLevelMode::Sparse};
    OrderBook book(config);
    constexpr std::array<Price, 8> prices{
        10, 100'000, 200'000, 300'000, 400'000, 500'000, 600'000, 700'000};

    for (std::size_t i = 0; i < prices.size(); ++i) {
        CHECK(book.add_limit_order(static_cast<OrderId>(i + 1U),
                                   Side::Buy,
                                   prices[i],
                                   1,
                                   static_cast<Timestamp>(i + 1U))
                  .status == Status::Accepted);
    }
    CHECK(book.stats().bids.occupied_level_count == prices.size());

    // Every level has one order. Removing the lowest level shifts all seven
    // following sorted-slot entries while leaving the cached best unchanged.
    CHECK(book.cancel_order(1, 20).status == Status::Cancelled);
    CHECK(book.depth_at_price(Side::Buy, prices[0]) == 0U);
    CHECK(book.stats().bids.occupied_level_count == prices.size() - 1U);
    CHECK(book.best_bid().valid);
    CHECK(book.best_bid().price == prices.back());

    // The released sparse level slot and a tombstoned map position remain
    // reusable without disturbing sorted traversal.
    CHECK(book.add_limit_order(9, Side::Buy, 50'000, 2, 21).status == Status::Accepted);
    std::array<DepthLevel, 8> depth{};
    CHECK(book.depth(Side::Buy, depth.size(), depth.data()) == depth.size());
    CHECK(depth.front().price == prices.back());
    CHECK(depth.back().price == 50'000);

    CHECK(book.cancel_order(8, 22).status == Status::Cancelled);
    CHECK(book.best_bid().valid);
    CHECK(book.best_bid().price == prices[6]);
}

void test_book_and_engine_stats()
{
    const BookConfig config{1, 1'000'000, 32, 128, 1, 0, PriceLevelMode::Sparse};
    OrderBook book(config);

    OrderBookStats stats = book.stats();
    CHECK(stats.live_order_count == 0);
    CHECK(stats.order_capacity == config.max_orders);
    CHECK(stats.order_pool_utilization == 0.0);
    CHECK(stats.order_id_map.size == 0);
    CHECK(stats.bids.mode == PriceLevelMode::Sparse);
    CHECK(stats.bids.configured_level_count == config.price_level_count());
    CHECK(stats.bids.level_storage_capacity == config.max_orders);
    CHECK(stats.bids.occupied_level_count == 0);

    CHECK(book.add_limit_order(1, Side::Buy, 500'000, 10).status == Status::Accepted);
    stats = book.stats();
    CHECK(stats.live_order_count == 1);
    CHECK(stats.order_pool_utilization > 0.0);
    CHECK(stats.order_id_map.size == 1);
    CHECK(stats.order_id_map_utilization > 0.0);
    CHECK(histogram_total(stats.order_id_map) == 1);
    CHECK(stats.bids.occupied_level_count == 1);
    CHECK(stats.bids.level_utilization > 0.0);

    CHECK(book.find_order(1) != nullptr);
    stats = book.stats();
    CHECK(stats.order_id_map.last_probe_count > 0);
    CHECK(histogram_total(stats.order_id_map) == 2);

    constexpr InstrumentId instrument_id = 77;
    const InstrumentConfig configs[] = {
        InstrumentConfig{instrument_id, config},
    };
    MatchingEngine engine(configs);
    CHECK(engine.valid());
    CHECK(engine.add_limit_order(instrument_id, 1, Side::Sell, 600'000, 5).status == Status::Accepted);
    CHECK(engine.find_order(instrument_id, 1) != nullptr);

    const OrderBookStats engine_book_stats = engine.stats(instrument_id);
    CHECK(engine_book_stats.live_order_count == 1);
    CHECK(histogram_total(engine_book_stats.order_id_map) == 2);

    const MatchingEngineStats engine_stats = engine.stats();
    CHECK(engine_stats.instrument_count == 1);
    CHECK(engine_stats.max_instruments == 1);
    CHECK(engine_stats.total_live_order_count == 1);
    CHECK(engine_stats.total_order_capacity == config.max_orders);
    CHECK(engine_stats.order_pool_utilization > 0.0);
    CHECK(engine_stats.order_id_map_utilization > 0.0);
    CHECK(engine_stats.aggregate_order_id_map.size == 1);
    CHECK(histogram_total(engine_stats.aggregate_order_id_map) == 2);
}

void test_invalid_and_unknown_inputs()
{
    const BookConfig invalid_config{100, 90, 16, 64, 1};
    OrderBook invalid_actual(invalid_config);
    const AddOrderResult invalid_result = invalid_actual.add_limit_order(1, Side::Buy, 100, 10);
    CHECK(invalid_result.status == Status::InvalidConfiguration);

    const BookConfig config{90, 110, 16, 64, 2};
    OrderBook actual(config);
    ReferenceOrderBook expected(config);

    run_add(actual, expected, kInvalidOrderId, Side::Buy, 100, 10, 1);
    run_add(actual, expected, 1, Side::Buy, 100, 0, 2);
    run_add(actual, expected, 1, Side::Buy, 101, 10, 3);
    run_cancel(actual, expected, 42);
    run_modify(actual, expected, 42, 0);
    run_modify(actual, expected, 42, 10);
    run_replace(actual, expected, 42, 100, 10);
    run_market(actual, expected, Side::Buy, 0);

    run_add(actual, expected, 2, Side::Buy, 100, 10, 4);
    run_replace(actual, expected, 2, 100, 0);
    run_replace(actual, expected, 2, 101, 10);
}

void test_invalid_side_direct_api_invalidates_event_spans()
{
    const BookConfig config{90,
                            110,
                            16,
                            64,
                            1,
                            32,
                            PriceLevelMode::Dense,
                            0,
                            SelfTradePolicy::Disabled,
                            32};
    OrderBook book(config);
    CHECK(book.add_limit_order(1, Side::Sell, 100, 5, 1).status == Status::Accepted);
    CHECK(!book.last_events().empty());
    CHECK(!book.last_market_data_events().empty());

    const Side invalid_side = static_cast<Side>(0xffU);
    const std::uint64_t checksum_before = book.state_checksum();
    const std::uint32_t live_orders_before = book.live_order_count();
    const Quantity ask_depth_before = book.depth_at_price(Side::Sell, 100);
    const SequenceNumber fifo_sequence_before = book.fifo_sequence();
    const SequenceNumber event_sequence_before = book.event_sequence();
    const SequenceNumber market_data_sequence_before = book.market_data_sequence();

    const AddOrderResult add = book.add_limit_order(2, invalid_side, 100, 5, 2);
    CHECK(add.status == Status::InvalidCommand);
    CHECK(add.accepted_quantity == 0U);
    CHECK(add.events_emitted == 0U);
    CHECK(add.events.empty());
    CHECK(book.find_order(2) == nullptr);
    CHECK(book.state_checksum() == checksum_before);
    CHECK(book.live_order_count() == live_orders_before);
    CHECK(book.depth_at_price(Side::Sell, 100) == ask_depth_before);
    CHECK(book.last_events().empty());
    CHECK(book.last_market_data_events().empty());
    CHECK(book.fifo_sequence() == fifo_sequence_before);
    CHECK(book.event_sequence() == event_sequence_before);
    CHECK(book.market_data_sequence() == market_data_sequence_before);

    CHECK(book.modify_order(1, 4, 3).status == Status::Accepted);
    CHECK(!book.last_events().empty());
    CHECK(!book.last_market_data_events().empty());
    const std::uint64_t checksum_before_market = book.state_checksum();
    const SequenceNumber fifo_sequence_before_market = book.fifo_sequence();
    const SequenceNumber event_sequence_before_market = book.event_sequence();
    const SequenceNumber market_data_sequence_before_market =
        book.market_data_sequence();

    const MatchResult market = book.match_market_order(invalid_side, 5, 3, 4);
    CHECK(market.status == Status::InvalidCommand);
    CHECK(market.requested_quantity == 5U);
    CHECK(market.executed_quantity == 0U);
    CHECK(market.remaining_quantity == 5U);
    CHECK(market.fills == 0U);
    CHECK(market.events_emitted == 0U);
    CHECK(market.events.empty());
    CHECK(book.state_checksum() == checksum_before_market);
    CHECK(book.live_order_count() == live_orders_before);
    CHECK(book.depth_at_price(Side::Sell, 100) == 4U);
    CHECK(book.last_events().empty());
    CHECK(book.last_market_data_events().empty());
    CHECK(book.fifo_sequence() == fifo_sequence_before_market);
    CHECK(book.event_sequence() == event_sequence_before_market);
    CHECK(book.market_data_sequence() == market_data_sequence_before_market);
}

void test_invalid_market_quantity_preserves_requested_and_remaining()
{
    BookConfig config{90, 110, 8, 32, 1};
    config.lot_size = 10;
    OrderBook book(config);

    const MatchResult zero = book.match_market_order(Side::Buy, 0, 10, 1);
    CHECK(zero.status == Status::InvalidQuantity);
    CHECK(zero.requested_quantity == 0U);
    CHECK(zero.executed_quantity == 0U);
    CHECK(zero.remaining_quantity == 0U);
    CHECK(zero.events_emitted == 1U);

    const MatchResult wrong_lot =
        book.match_market_order(Side::Sell, 15, 11, 2);
    CHECK(wrong_lot.status == Status::LotSizeViolation);
    CHECK(wrong_lot.requested_quantity == 15U);
    CHECK(wrong_lot.executed_quantity == 0U);
    CHECK(wrong_lot.remaining_quantity == 15U);
    CHECK(wrong_lot.events_emitted == 1U);

    constexpr InstrumentId instrument_id = 88;
    const InstrumentConfig instruments[] = {
        InstrumentConfig{instrument_id, config},
    };
    MatchingEngine engine(instruments);
    const MatchResult routed =
        engine.match_market_order(instrument_id, Side::Buy, 15, 12, 3);
    CHECK(routed.status == Status::LotSizeViolation);
    CHECK(routed.requested_quantity == 15U);
    CHECK(routed.remaining_quantity == 15U);

    const Command command{
        instrument_id, CommandOp::Market, 13, Side::Buy, 0, 15, TimeInForce::Ioc, 4};
    const DispatchResult dispatched = engine.dispatch(command);
    CHECK(dispatched.status == Status::LotSizeViolation);
    CHECK(dispatched.requested_quantity == 15U);
    CHECK(dispatched.executed_quantity == 0U);
    CHECK(dispatched.remaining_quantity == 15U);
}

void test_event_single_fill()
{
    const BookConfig config{90, 110, 16, 64, 1};
    OrderBook book(config);

    const AddOrderResult resting = book.add_limit_order(1, Side::Sell, 100, 5, 10);
    CHECK(resting.status == Status::Accepted);
    CHECK(resting.events_emitted == 2);

    const AddOrderResult aggressive = book.add_limit_order(2, Side::Buy, 101, 5, 11);
    CHECK(aggressive.status == Status::Filled);
    CHECK(aggressive.executed_quantity == 5);
    CHECK(aggressive.resting_quantity == 0);
    CHECK(aggressive.events_emitted == 2);

    check_order_event_fields(
        aggressive.events[0], BookEvent::Kind::OrderAccepted, Status::Accepted, 2, Side::Buy, 101, 5, 11);
    check_trade_event_fields(aggressive.events[1], 2, 1, Side::Buy, 100, 5, 11);
    CHECK(aggressive.events[1].sequence == aggressive.events[0].sequence + 1U);
}

void test_event_multi_level_sweep()
{
    const BookConfig config{90, 110, 16, 64, 1};
    OrderBook book(config);

    CHECK(book.add_limit_order(1, Side::Sell, 100, 2, 1).status == Status::Accepted);
    CHECK(book.add_limit_order(2, Side::Sell, 101, 3, 2).status == Status::Accepted);
    CHECK(book.add_limit_order(3, Side::Sell, 102, 5, 3).status == Status::Accepted);

    const MatchResult sweep = book.match_market_order(Side::Buy, 7, 900, 50);
    CHECK(sweep.status == Status::Filled);
    CHECK(sweep.executed_quantity == 7);
    CHECK(sweep.remaining_quantity == 0);
    CHECK(sweep.fills == 3);
    CHECK(sweep.events_emitted == 3);

    check_trade_event_fields(sweep.events[0], 900, 1, Side::Buy, 100, 2, 50);
    check_trade_event_fields(sweep.events[1], 900, 2, Side::Buy, 101, 3, 50);
    check_trade_event_fields(sweep.events[2], 900, 3, Side::Buy, 102, 2, 50);
    CHECK(sweep.events[1].sequence == sweep.events[0].sequence + 1U);
    CHECK(sweep.events[2].sequence == sweep.events[1].sequence + 1U);
    CHECK(book.find_order(3) != nullptr);
    CHECK(book.find_order(3)->quantity == 3);
}

void test_event_partial_fill_and_rest()
{
    const BookConfig config{90, 110, 16, 64, 1};
    OrderBook book(config);

    CHECK(book.add_limit_order(1, Side::Sell, 100, 5, 1).status == Status::Accepted);

    const AddOrderResult result = book.add_limit_order(2, Side::Buy, 100, 8, 2);
    CHECK(result.status == Status::PartiallyFilled);
    CHECK(result.executed_quantity == 5);
    CHECK(result.resting_quantity == 3);
    CHECK(result.events_emitted == 3);

    check_order_event_fields(
        result.events[0], BookEvent::Kind::OrderAccepted, Status::Accepted, 2, Side::Buy, 100, 8, 2);
    check_trade_event_fields(result.events[1], 2, 1, Side::Buy, 100, 5, 2);
    check_order_event_fields(
        result.events[2], BookEvent::Kind::OrderResting, Status::Accepted, 2, Side::Buy, 100, 3, 2);
    CHECK(result.events[1].sequence == result.events[0].sequence + 1U);
    CHECK(result.events[2].sequence == result.events[1].sequence + 1U);
    CHECK(book.depth_at_price(Side::Buy, 100) == 3);
}

void test_event_cancel_after_partial()
{
    const BookConfig config{90, 110, 16, 64, 1};
    OrderBook book(config);

    CHECK(book.add_limit_order(1, Side::Sell, 100, 5, 1).status == Status::Accepted);
    CHECK(book.add_limit_order(2, Side::Buy, 100, 8, 2).status == Status::PartiallyFilled);

    const CancelResult cancel = book.cancel_order(2, 3);
    CHECK(cancel.status == Status::Cancelled);
    CHECK(cancel.canceled_quantity == 3);
    CHECK(cancel.events_emitted == 1);
    check_order_event_fields(
        cancel.events[0], BookEvent::Kind::OrderCancelled, Status::Cancelled, 2, Side::Buy, 100, 3, 3);
    CHECK(book.find_order(2) == nullptr);
}

void test_fok_reject_insufficient_liquidity_preserves_book()
{
    const BookConfig config{90, 110, 16, 64, 1};
    OrderBook book(config);

    CHECK(book.add_limit_order(1, Side::Sell, 100, 5, 1).status == Status::Accepted);
    CHECK(book.add_limit_order(2, Side::Sell, 101, 4, 2).status == Status::Accepted);

    const AddOrderResult fok = book.add_limit_order(3, Side::Buy, 100, 10, 3, TimeInForce::Fok);
    CHECK(fok.status == Status::FokRejected);
    CHECK(fok.executed_quantity == 0);
    CHECK(fok.resting_quantity == 0);
    CHECK(fok.events_emitted == 1);
    check_order_event_fields(
        fok.events[0], BookEvent::Kind::OrderRejected, Status::FokRejected, 3, Side::Buy, 100, 10, 3);
    CHECK(fok.events[0].time_in_force == TimeInForce::Fok);

    CHECK(book.live_order_count() == 2);
    CHECK(book.depth_at_price(Side::Sell, 100) == 5);
    CHECK(book.depth_at_price(Side::Sell, 101) == 4);
    CHECK(book.depth_at_price(Side::Buy, 100) == 0);
    CHECK(book.find_order(1) != nullptr);
    CHECK(book.find_order(1)->quantity == 5);
    CHECK(book.find_order(2) != nullptr);
    CHECK(book.find_order(2)->quantity == 4);
    CHECK(book.find_order(3) == nullptr);
    CHECK(book.best_ask().valid);
    CHECK(book.best_ask().price == 100);
    CHECK(book.best_ask().quantity == 5);
}

void test_fok_accept_exact_liquidity()
{
    const BookConfig config{90, 110, 16, 64, 1};
    OrderBook book(config);

    CHECK(book.add_limit_order(1, Side::Sell, 100, 5, 1).status == Status::Accepted);
    CHECK(book.add_limit_order(2, Side::Sell, 101, 3, 2).status == Status::Accepted);

    const AddOrderResult fok = book.add_limit_order(3, Side::Buy, 101, 8, 3, TimeInForce::Fok);
    CHECK(fok.status == Status::Filled);
    CHECK(fok.executed_quantity == 8);
    CHECK(fok.resting_quantity == 0);
    CHECK(fok.fills == 2);
    CHECK(fok.events_emitted == 3);

    check_order_event_fields(
        fok.events[0], BookEvent::Kind::OrderAccepted, Status::Accepted, 3, Side::Buy, 101, 8, 3);
    CHECK(fok.events[0].time_in_force == TimeInForce::Fok);
    check_trade_event_fields(fok.events[1], 3, 1, Side::Buy, 100, 5, 3);
    check_trade_event_fields(fok.events[2], 3, 2, Side::Buy, 101, 3, 3);
    CHECK(fok.events[1].sequence == fok.events[0].sequence + 1U);
    CHECK(fok.events[2].sequence == fok.events[1].sequence + 1U);

    CHECK(book.live_order_count() == 0);
    CHECK(book.find_order(1) == nullptr);
    CHECK(book.find_order(2) == nullptr);
    CHECK(book.find_order(3) == nullptr);
    CHECK(!book.best_bid().valid);
    CHECK(!book.best_ask().valid);
}

void test_ioc_partial_fill_cancels_remainder()
{
    const BookConfig config{90, 110, 16, 64, 1};
    OrderBook book(config);

    CHECK(book.add_limit_order(1, Side::Sell, 100, 5, 1).status == Status::Accepted);

    const AddOrderResult ioc = book.add_limit_order(2, Side::Buy, 100, 8, 2, TimeInForce::Ioc);
    CHECK(ioc.status == Status::PartiallyFilled);
    CHECK(ioc.executed_quantity == 5);
    CHECK(ioc.resting_quantity == 0);
    CHECK(ioc.fills == 1);
    CHECK(ioc.events_emitted == 3);

    check_order_event_fields(
        ioc.events[0], BookEvent::Kind::OrderAccepted, Status::Accepted, 2, Side::Buy, 100, 8, 2);
    check_trade_event_fields(ioc.events[1], 2, 1, Side::Buy, 100, 5, 2);
    check_order_event_fields(
        ioc.events[2], BookEvent::Kind::OrderCancelled, Status::Cancelled, 2, Side::Buy, 100, 3, 2);
    CHECK(ioc.events[0].time_in_force == TimeInForce::Ioc);
    CHECK(ioc.events[2].time_in_force == TimeInForce::Ioc);
    CHECK(ioc.events[1].sequence == ioc.events[0].sequence + 1U);
    CHECK(ioc.events[2].sequence == ioc.events[1].sequence + 1U);

    CHECK(book.live_order_count() == 0);
    CHECK(book.find_order(1) == nullptr);
    CHECK(book.find_order(2) == nullptr);
    CHECK(!book.best_bid().valid);
    CHECK(!book.best_ask().valid);
}

void test_ioc_no_liquidity_cancels_without_book_mutation()
{
    const BookConfig config{90, 110, 16, 64, 1};
    OrderBook book(config);

    CHECK(book.add_limit_order(1, Side::Sell, 101, 5, 1).status == Status::Accepted);

    const AddOrderResult ioc = book.add_limit_order(2, Side::Buy, 100, 8, 2, TimeInForce::Ioc);
    CHECK(ioc.status == Status::NoLiquidity);
    CHECK(ioc.executed_quantity == 0);
    CHECK(ioc.resting_quantity == 0);
    CHECK(ioc.events_emitted == 2);

    check_order_event_fields(
        ioc.events[0], BookEvent::Kind::OrderAccepted, Status::Accepted, 2, Side::Buy, 100, 8, 2);
    check_order_event_fields(
        ioc.events[1], BookEvent::Kind::OrderCancelled, Status::Cancelled, 2, Side::Buy, 100, 8, 2);
    CHECK(ioc.events[0].time_in_force == TimeInForce::Ioc);
    CHECK(ioc.events[1].time_in_force == TimeInForce::Ioc);
    CHECK(ioc.events[1].sequence == ioc.events[0].sequence + 1U);

    CHECK(book.live_order_count() == 1);
    CHECK(book.depth_at_price(Side::Sell, 101) == 5);
    CHECK(book.depth_at_price(Side::Buy, 100) == 0);
    CHECK(book.find_order(1) != nullptr);
    CHECK(book.find_order(1)->quantity == 5);
    CHECK(book.find_order(2) == nullptr);
    CHECK(!book.best_bid().valid);
    CHECK(book.best_ask().valid);
    CHECK(book.best_ask().price == 101);
}

void check_invalid_engine_fails_closed(MatchingEngine& engine,
                                       const std::uint32_t expected_capacity,
                                       const InstrumentId configured_instrument)
{
    CHECK(!engine.valid());
    CHECK(engine.initialization_error() != MatchingEngineInitError::None);
    CHECK(engine.max_instruments() == expected_capacity);
    CHECK(engine.instrument_count() == 0U);

    const AddOrderResult add =
        engine.add_limit_order(configured_instrument, 1, Side::Buy, 100, 10, 1);
    CHECK(add.status == Status::UnknownInstrument);
    CHECK(add.events.empty());

    const CancelResult cancel = engine.cancel(configured_instrument, 1, 2);
    CHECK(cancel.status == Status::UnknownInstrument);
    CHECK(cancel.events.empty());
    CHECK(engine.cancel_order(configured_instrument, 1, 3).status ==
          Status::UnknownInstrument);

    const ModifyResult modify = engine.modify(configured_instrument, 1, 5, 4);
    CHECK(modify.status == Status::UnknownInstrument);
    CHECK(modify.events.empty());
    CHECK(engine.modify_order(configured_instrument, 1, 5, 5).status ==
          Status::UnknownInstrument);

    const ReplaceResult replace =
        engine.replace(configured_instrument, 1, 101, 5, TimeInForce::Gtc);
    CHECK(replace.status == Status::UnknownInstrument);
    CHECK(replace.events.empty());
    CHECK(engine.replace(configured_instrument, 1, 101, 5, 6, TimeInForce::Gtc)
              .status == Status::UnknownInstrument);
    CHECK(engine.replace_order(configured_instrument, 1, 101, 5).status ==
          Status::UnknownInstrument);
    CHECK(engine.replace_order(
                    configured_instrument, 1, 101, 5, 7, TimeInForce::Gtc)
              .status == Status::UnknownInstrument);

    const MatchResult market =
        engine.match_market_order(configured_instrument, Side::Buy, 1, 2, 8);
    CHECK(market.status == Status::UnknownInstrument);
    CHECK(market.events.empty());

    const Command command{configured_instrument,
                          CommandOp::Add,
                          3,
                          Side::Buy,
                          100,
                          1,
                          TimeInForce::Gtc,
                          9};
    const DispatchResult dispatch = engine.dispatch(command);
    CHECK(dispatch.status == Status::UnknownInstrument);
    CHECK(dispatch.events.empty());

    std::array<std::byte, kCommandWireSize> encoded{};
    CHECK(encode(command, encoded) == Status::Accepted);
    const DispatchResult encoded_dispatch = engine.dispatch(encoded);
    CHECK(encoded_dispatch.status == Status::UnknownInstrument);
    CHECK(encoded_dispatch.events.empty());

    DepthLevel level{777, 888, 999};
    CHECK(engine.depth(configured_instrument, Side::Buy, 1, &level) == 0U);
    CHECK(level.price == 777);
    CHECK(level.aggregate_quantity == 888U);
    CHECK(level.order_count == 999U);
    CHECK(engine.depth_at_price(configured_instrument, Side::Buy, 100) == 0U);
    CHECK(engine.order_count_at_price(configured_instrument, Side::Buy, 100) ==
          0U);
    CHECK(engine.find_order(configured_instrument, 1) == nullptr);
    CHECK(engine.live_order_count(configured_instrument) == 0U);
    CHECK(engine.last_events(configured_instrument).empty());
    CHECK(engine.order_book(configured_instrument) == nullptr);
    CHECK(!engine.best_bid(configured_instrument).valid);
    CHECK(!engine.best_ask(configured_instrument).valid);

    const TopOfBook top = engine.top_of_book(configured_instrument);
    CHECK(top.status == Status::UnknownInstrument);
    CHECK(!top.bid.valid);
    CHECK(!top.ask.valid);

    const OrderBookStats book_stats = engine.stats(configured_instrument);
    CHECK(book_stats.live_order_count == 0U);
    CHECK(book_stats.order_capacity == 0U);

    const MatchingEngineStats engine_stats = engine.stats();
    CHECK(engine_stats.instrument_count == 0U);
    CHECK(engine_stats.max_instruments == expected_capacity);
    CHECK(engine_stats.total_live_order_count == 0U);

    std::array<std::byte, 256> snapshot{};
    const SnapshotWriteResult serialized = serialize(engine, snapshot);
    CHECK(serialized.status == Status::InvalidConfiguration);
    CHECK(serialized.bytes_written == 0U);

    MatchingEngine snapshot_source(
        expected_capacity, std::span<const InstrumentConfig>{});
    CHECK(snapshot_source.valid());
    const SnapshotWriteResult valid_snapshot =
        serialize(snapshot_source, snapshot);
    CHECK(valid_snapshot.status == Status::Accepted);
    CHECK(restore(engine,
                  std::span<const std::byte>(
                      snapshot.data(), valid_snapshot.bytes_written)) ==
          Status::SnapshotConfigurationMismatch);
}

void test_matching_engine_factory_success_and_api_contract()
{
    static_assert(
        std::is_same_v<
            decltype(MatchingEngine::create(
                std::declval<const InstrumentConfig (&)[1]>())),
            MatchingEngineCreateResult>);
    static_assert(noexcept(MatchingEngine::create(
        std::declval<const InstrumentConfig (&)[1]>())));
    static_assert(std::is_move_constructible_v<MatchingEngineCreateResult>);
    static_assert(!std::is_copy_constructible_v<MatchingEngineCreateResult>);

    const BookConfig config{90, 110, 16, 64, 1};
    const InstrumentConfig single_config[] = {
        InstrumentConfig{101, config},
    };

    MatchingEngineCreateResult single = MatchingEngine::create(single_config);
    CHECK(single);
    CHECK(single.has_value());
    CHECK(single.error == MatchingEngineInitError::None);
    CHECK(single.config_index == kInvalidInstrumentConfigIndex);
    CHECK(single.get() != nullptr);
    CHECK(single.engine->valid());
    CHECK(single.engine->instrument_count() == 1U);
    CHECK(single.engine->max_instruments() == 1U);
    CHECK(single.engine->add_limit_order(101, 1, Side::Buy, 100, 5).status ==
          Status::Accepted);

    const InstrumentConfig multiple_configs[] = {
        InstrumentConfig{201, config},
        InstrumentConfig{202, config},
    };
    MatchingEngineCreateResult multiple =
        MatchingEngine::create(4U, multiple_configs);
    CHECK(multiple);
    CHECK(multiple.engine->instrument_count() == 2U);
    CHECK(multiple.engine->max_instruments() == 4U);
    CHECK(multiple.engine->add_limit_order(201, 1, Side::Buy, 99, 3).status ==
          Status::Accepted);
    CHECK(multiple.engine->add_limit_order(202, 1, Side::Sell, 101, 4).status ==
          Status::Accepted);

    MatchingEngineCreateResult empty =
        MatchingEngine::create(std::span<const InstrumentConfig>{});
    CHECK(empty);
    CHECK(empty.engine->valid());
    CHECK(empty.engine->instrument_count() == 0U);
    CHECK(empty.engine->max_instruments() == 0U);
    CHECK(empty.engine->add_limit_order(1, 1, Side::Buy, 100, 1).status ==
          Status::UnknownInstrument);
}

void test_matching_engine_factory_reports_precise_initialization_failures()
{
    const BookConfig valid_book{90, 110, 16, 64, 1};
    const BookConfig invalid_book{110, 90, 16, 64, 1};

    const InstrumentConfig invalid_first[] = {
        InstrumentConfig{kInvalidInstrumentId, valid_book},
        InstrumentConfig{102, valid_book},
        InstrumentConfig{103, valid_book},
    };
    MatchingEngineCreateResult result = MatchingEngine::create(invalid_first);
    CHECK(!result);
    CHECK(result.engine == nullptr);
    CHECK(result.error == MatchingEngineInitError::InvalidInstrumentId);
    CHECK(result.config_index == 0U);

    const InstrumentConfig invalid_middle[] = {
        InstrumentConfig{101, valid_book},
        InstrumentConfig{102, invalid_book},
        InstrumentConfig{103, valid_book},
    };
    result = MatchingEngine::create(invalid_middle);
    CHECK(!result);
    CHECK(result.error ==
          MatchingEngineInitError::InvalidBookConfiguration);
    CHECK(result.config_index == 1U);

    const InstrumentConfig invalid_final[] = {
        InstrumentConfig{101, valid_book},
        InstrumentConfig{102, valid_book},
        InstrumentConfig{103, invalid_book},
    };
    result = MatchingEngine::create(invalid_final);
    CHECK(!result);
    CHECK(result.error ==
          MatchingEngineInitError::InvalidBookConfiguration);
    CHECK(result.config_index == 2U);

    const InstrumentConfig duplicates[] = {
        InstrumentConfig{101, valid_book},
        InstrumentConfig{102, valid_book},
        InstrumentConfig{101, valid_book},
    };
    result = MatchingEngine::create(duplicates);
    CHECK(!result);
    CHECK(result.error == MatchingEngineInitError::DuplicateInstrumentId);
    CHECK(result.config_index == 2U);

    const InstrumentConfig over_capacity[] = {
        InstrumentConfig{101, valid_book},
        InstrumentConfig{102, valid_book},
    };
    result = MatchingEngine::create(1U, over_capacity);
    CHECK(!result);
    CHECK(result.error ==
          MatchingEngineInitError::InstrumentCapacityExceeded);
    CHECK(result.config_index == 1U);

    CHECK(std::strcmp(matching_engine_init_error_name(
                          MatchingEngineInitError::DuplicateInstrumentId),
                      "duplicate instrument id") == 0);
}

void test_matching_engine_compatibility_constructor_fails_closed()
{
    const BookConfig valid_book{90, 110, 16, 64, 1};
    const BookConfig invalid_book{110, 90, 16, 64, 1};

    const InstrumentConfig invalid_first[] = {
        InstrumentConfig{kInvalidInstrumentId, valid_book},
        InstrumentConfig{102, valid_book},
        InstrumentConfig{103, valid_book},
    };
    MatchingEngine first(invalid_first);
    CHECK(first.initialization_error() ==
          MatchingEngineInitError::InvalidInstrumentId);
    CHECK(first.failed_config_index() == 0U);
    check_invalid_engine_fails_closed(first, 3U, 102);

    const InstrumentConfig invalid_middle[] = {
        InstrumentConfig{201, valid_book},
        InstrumentConfig{202, invalid_book},
        InstrumentConfig{203, valid_book},
    };
    MatchingEngine middle(invalid_middle);
    CHECK(middle.initialization_error() ==
          MatchingEngineInitError::InvalidBookConfiguration);
    CHECK(middle.failed_config_index() == 1U);
    check_invalid_engine_fails_closed(middle, 3U, 201);
    CHECK(middle.order_book(203) == nullptr);

    const InstrumentConfig invalid_final[] = {
        InstrumentConfig{301, valid_book},
        InstrumentConfig{302, valid_book},
        InstrumentConfig{301, valid_book},
    };
    MatchingEngine final(invalid_final);
    CHECK(final.initialization_error() ==
          MatchingEngineInitError::DuplicateInstrumentId);
    CHECK(final.failed_config_index() == 2U);
    check_invalid_engine_fails_closed(final, 3U, 301);
    CHECK(final.order_book(302) == nullptr);

    const InstrumentConfig over_capacity[] = {
        InstrumentConfig{401, valid_book},
        InstrumentConfig{402, valid_book},
    };
    MatchingEngine capacity(1U, over_capacity);
    CHECK(capacity.initialization_error() ==
          MatchingEngineInitError::InstrumentCapacityExceeded);
    CHECK(capacity.failed_config_index() == 1U);
    check_invalid_engine_fails_closed(capacity, 1U, 401);
}

void test_matching_engine_two_instruments_are_isolated()
{
    constexpr InstrumentId instrument_a = 101;
    constexpr InstrumentId instrument_b = 202;
    const BookConfig config{90, 110, 16, 64, 1};
    const InstrumentConfig configs[] = {
        InstrumentConfig{instrument_a, config},
        InstrumentConfig{instrument_b, config},
    };

    MatchingEngine actual(configs);
    ReferenceOrderBook expected_a(config, instrument_a);
    ReferenceOrderBook expected_b(config, instrument_b);

    CHECK(actual.valid());
    CHECK(actual.max_instruments() == 2);
    CHECK(actual.instrument_count() == 2);

    run_engine_add(actual, expected_a, instrument_a, 1, Side::Buy, 100, 10, 1);
    run_engine_add(actual, expected_b, instrument_b, 1, Side::Sell, 105, 20, 1);

    CHECK(actual.find_order(instrument_a, 1) != nullptr);
    CHECK(actual.find_order(instrument_a, 1)->side == Side::Buy);
    CHECK(actual.find_order(instrument_b, 1) != nullptr);
    CHECK(actual.find_order(instrument_b, 1)->side == Side::Sell);

    const AddOrderResult actual_fill =
        actual.add_limit_order(instrument_a, 2, Side::Sell, 100, 4, 2, TimeInForce::Gtc);
    const AddOrderResult expected_fill = expected_a.add_limit_order(2, Side::Sell, 100, 4, 2, TimeInForce::Gtc);
    check_add_result(actual_fill, expected_fill, "engine_isolated_fill");
    CHECK(actual_fill.status == Status::Filled);
    CHECK(actual_fill.events_emitted == 2);
    check_order_event_fields(actual_fill.events[0],
                             BookEvent::Kind::OrderAccepted,
                             Status::Accepted,
                             2,
                             Side::Sell,
                             100,
                             4,
                             2,
                             instrument_a);
    check_trade_event_fields(actual_fill.events[1], 2, 1, Side::Sell, 100, 4, 2, instrument_a);

    check_engine_book_equal(actual, instrument_a, expected_a);
    check_engine_book_equal(actual, instrument_b, expected_b);
    CHECK(actual.depth_at_price(instrument_a, Side::Buy, 100) == 6);
    CHECK(actual.depth_at_price(instrument_b, Side::Sell, 105) == 20);
}

void test_matching_engine_depth_after_adds_cancels_and_matches()
{
    constexpr InstrumentId instrument_a = 111;
    constexpr InstrumentId instrument_b = 222;
    const BookConfig config{90, 110, 32, 128, 1};
    const InstrumentConfig configs[] = {
        InstrumentConfig{instrument_a, config},
        InstrumentConfig{instrument_b, config},
    };

    MatchingEngine actual(configs);
    ReferenceOrderBook expected_a(config, instrument_a);
    ReferenceOrderBook expected_b(config, instrument_b);

    run_engine_add(actual, expected_a, instrument_a, 1, Side::Buy, 100, 5, 1);
    run_engine_add(actual, expected_a, instrument_a, 2, Side::Buy, 101, 3, 2);
    run_engine_add(actual, expected_a, instrument_a, 3, Side::Buy, 101, 7, 3);
    run_engine_add(actual, expected_a, instrument_a, 4, Side::Sell, 104, 4, 4);
    run_engine_add(actual, expected_a, instrument_a, 5, Side::Sell, 105, 6, 5);
    run_engine_add(actual, expected_b, instrument_b, 1, Side::Sell, 99, 9, 1);

    DepthLevel levels[4]{};
    std::uint32_t count = actual.depth(instrument_a, Side::Buy, 4, levels);
    CHECK(count == 2);
    check_depth_level(levels[0], 101, 10, 2);
    check_depth_level(levels[1], 100, 5, 1);

    count = actual.depth(instrument_a, Side::Buy, 1, levels);
    CHECK(count == 1);
    check_depth_level(levels[0], 101, 10, 2);

    count = actual.depth(instrument_a, Side::Sell, 4, levels);
    CHECK(count == 2);
    check_depth_level(levels[0], 104, 4, 1);
    check_depth_level(levels[1], 105, 6, 1);

    run_engine_cancel(actual, expected_a, instrument_a, 2, 6);
    count = actual.depth(instrument_a, Side::Buy, 4, levels);
    CHECK(count == 2);
    check_depth_level(levels[0], 101, 7, 1);
    check_depth_level(levels[1], 100, 5, 1);

    run_engine_market(actual, expected_a, instrument_a, Side::Sell, 8, 900, 7);
    count = actual.depth(instrument_a, Side::Buy, 4, levels);
    CHECK(count == 1);
    check_depth_level(levels[0], 100, 4, 1);

    const TopOfBook top = actual.top_of_book(instrument_a);
    CHECK(top.status == Status::Accepted);
    check_best_quote(top.bid, BestQuote{true, 100, 4, 1}, "depth_top_bid");
    check_best_quote(top.ask, BestQuote{true, 104, 4, 1}, "depth_top_ask");

    check_engine_book_equal(actual, instrument_a, expected_a);
    check_engine_book_equal(actual, instrument_b, expected_b);
}

void test_matching_engine_unknown_instrument_rejects_without_side_effects()
{
    constexpr InstrumentId instrument_a = 301;
    constexpr InstrumentId instrument_b = 302;
    constexpr InstrumentId unknown = 999;
    const BookConfig config{90, 110, 16, 64, 1};
    const InstrumentConfig configs[] = {
        InstrumentConfig{instrument_a, config},
        InstrumentConfig{instrument_b, config},
    };

    MatchingEngine actual(configs);
    ReferenceOrderBook expected_a(config, instrument_a);
    ReferenceOrderBook expected_b(config, instrument_b);

    run_engine_add(actual, expected_a, instrument_a, 1, Side::Buy, 100, 5, 1);
    run_engine_add(actual, expected_b, instrument_b, 1, Side::Sell, 105, 8, 1);

    const AddOrderResult add = actual.add_limit_order(unknown, 10, Side::Buy, 100, 1, 2);
    CHECK(add.status == Status::UnknownInstrument);
    CHECK(add.events_emitted == 0);
    CHECK(add.events.empty());

    const CancelResult cancel = actual.cancel(unknown, 1, 3);
    CHECK(cancel.status == Status::UnknownInstrument);
    CHECK(cancel.events_emitted == 0);

    const ModifyResult modify = actual.modify(unknown, 1, 2, 4);
    CHECK(modify.status == Status::UnknownInstrument);
    CHECK(modify.events_emitted == 0);

    const ReplaceResult replace = actual.replace(unknown, 1, 101, 2, 5, TimeInForce::Gtc);
    CHECK(replace.status == Status::UnknownInstrument);
    CHECK(replace.events_emitted == 0);

    const MatchResult market = actual.match_market_order(unknown, Side::Buy, 1, 900, 6);
    CHECK(market.status == Status::UnknownInstrument);
    CHECK(market.events_emitted == 0);

    DepthLevel levels[2]{};
    CHECK(actual.depth(unknown, Side::Buy, 2, levels) == 0);
    CHECK(actual.depth_at_price(unknown, Side::Buy, 100) == 0);
    CHECK(actual.order_count_at_price(unknown, Side::Buy, 100) == 0);
    CHECK(actual.find_order(unknown, 1) == nullptr);
    CHECK(actual.live_order_count(unknown) == 0);
    CHECK(actual.last_events(unknown).empty());
    CHECK(actual.top_of_book(unknown).status == Status::UnknownInstrument);

    check_engine_book_equal(actual, instrument_a, expected_a);
    check_engine_book_equal(actual, instrument_b, expected_b);
}

void test_book_snapshot_round_trip_random_ops()
{
    const BookConfig config{90, 130, 48, 256, 2};
    OrderBook actual(config);
    ReferenceOrderBook expected(config);
    SplitMix64 rng(0x7265636f76657279ULL);

    for (std::uint32_t step = 0; step < 750U; ++step) {
        const std::uint32_t action = rng.uniform(100U);
        if (action < 44U) {
            run_add(actual,
                    expected,
                    random_order_id(rng),
                    random_side(rng),
                    random_price(rng),
                    random_quantity(rng),
                    static_cast<Timestamp>(step + 1U),
                    random_time_in_force(rng));
        } else if (action < 60U) {
            run_cancel(actual, expected, random_order_id(rng));
        } else if (action < 74U) {
            run_modify(actual, expected, random_order_id(rng), random_quantity(rng));
        } else if (action < 90U) {
            run_replace(actual,
                        expected,
                        random_order_id(rng),
                        random_price(rng),
                        random_quantity(rng),
                        static_cast<Timestamp>(step + 1U),
                        random_time_in_force(rng));
        } else {
            run_market(actual, expected, random_side(rng), random_quantity(rng));
        }
    }

    std::array<std::byte, kSnapshotTestBufferSize> buffer{};
    const SnapshotWriteResult snapshot = serialize(actual, buffer);
    CHECK(snapshot.status == Status::Accepted);
    CHECK(snapshot.bytes_written > 0);

    OrderBook restored(config);
    const Status restore_status =
        restore(restored, std::span<const std::byte>(buffer.data(), snapshot.bytes_written));
    CHECK(restore_status == Status::Accepted);
    CHECK(restored.last_events().empty());
    check_books_equal(restored, expected);
    check_book_snapshots_equal(restored, actual, "random_book_snapshot_round_trip");
}

void test_snapshot_round_trip_preserves_public_order_state()
{
    constexpr std::array<PriceLevelMode, 2> modes{
        PriceLevelMode::Dense,
        PriceLevelMode::Sparse,
    };

    for (const PriceLevelMode mode : modes) {
        BookConfig config{90, 110, 8, 32, 1, 32, mode};
        OrderBook source(config);
        CHECK(source.add_limit_order(1, Side::Sell, 100, 3, 1).status ==
              Status::Accepted);
        CHECK(source.add_limit_order(2, Side::Buy, 100, 10, 2).status ==
              Status::PartiallyFilled);
        CHECK(source.match_market_order(Side::Sell, 2, 90, 3).status ==
              Status::Filled);
        CHECK(source.add_limit_order(3, Side::Sell, 105, 9, 4).status ==
              Status::Accepted);
        CHECK(source.modify_order(3, 4, 5).status == Status::Accepted);

        const Order* const partially_filled = source.find_order(2);
        CHECK(partially_filled != nullptr);
        CHECK(partially_filled->quantity == 5U);
        CHECK(partially_filled->initial_quantity == 7U);
        CHECK(partially_filled->state == OrderState::PartiallyFilled);

        const Order* const reduced = source.find_order(3);
        CHECK(reduced != nullptr);
        CHECK(reduced->quantity == 4U);
        CHECK(reduced->initial_quantity == 9U);
        CHECK(reduced->state == OrderState::Resting);

        std::array<std::byte, kSnapshotTestBufferSize> buffer{};
        const SnapshotWriteResult snapshot = serialize(source, buffer);
        CHECK(snapshot.status == Status::Accepted);

        OrderBook restored(config);
        CHECK(restore(restored,
                      std::span<const std::byte>(buffer.data(), snapshot.bytes_written)) ==
              Status::Accepted);

        const Order* const restored_partial = restored.find_order(2);
        CHECK(restored_partial != nullptr);
        CHECK(restored_partial->quantity == partially_filled->quantity);
        CHECK(restored_partial->initial_quantity == partially_filled->initial_quantity);
        CHECK(restored_partial->state == partially_filled->state);

        const Order* const restored_reduced = restored.find_order(3);
        CHECK(restored_reduced != nullptr);
        CHECK(restored_reduced->quantity == reduced->quantity);
        CHECK(restored_reduced->initial_quantity == reduced->initial_quantity);
        CHECK(restored_reduced->state == reduced->state);
        CHECK(restored.state_checksum() == source.state_checksum());
        check_book_snapshots_equal(restored, source, "public_order_state_round_trip");
    }
}

void test_book_snapshot_dense_and_sparse_round_trips_at_capacity()
{
    constexpr PriceLevelMode modes[] = {
        PriceLevelMode::Dense,
        PriceLevelMode::Sparse,
    };

    for (const PriceLevelMode mode : modes) {
        const BookConfig config{1, 2'048, 1'024, 2'048, 1, 0, mode};

        OrderBook empty(config);
        std::array<std::byte, kSnapshotTestBufferSize> empty_buffer{};
        const SnapshotWriteResult empty_snapshot = serialize(empty, empty_buffer);
        CHECK(empty_snapshot.status == Status::Accepted);
        OrderBook empty_restored(config);
        CHECK(restore(empty_restored,
                      std::span<const std::byte>(empty_buffer.data(), empty_snapshot.bytes_written)) ==
              Status::Accepted);
        check_book_snapshots_equal(empty_restored, empty, "empty_snapshot_round_trip");

        OrderBook partial(config);
        CHECK(partial.add_limit_order(1, Side::Buy, 500, 5, 1).status == Status::Accepted);
        CHECK(partial.add_limit_order(2, Side::Buy, 500, 7, 2).status == Status::Accepted);
        CHECK(partial.add_limit_order(3, Side::Buy, 499, 3, 3).status == Status::Accepted);
        CHECK(partial.add_limit_order(4, Side::Sell, 1'500, 11, 4).status == Status::Accepted);
        CHECK(partial.add_limit_order(5, Side::Sell, 1'501, 13, 5).status == Status::Accepted);

        std::array<std::byte, kSnapshotTestBufferSize> partial_buffer{};
        const SnapshotWriteResult partial_snapshot = serialize(partial, partial_buffer);
        CHECK(partial_snapshot.status == Status::Accepted);
        OrderBook partial_restored(config);
        CHECK(restore(partial_restored,
                      std::span<const std::byte>(partial_buffer.data(), partial_snapshot.bytes_written)) ==
              Status::Accepted);
        check_book_snapshots_equal(partial_restored, partial, "partial_snapshot_round_trip");

        OrderBook full(config);
        for (std::uint32_t index = 0; index < config.max_orders; ++index) {
            const bool bid = index < config.max_orders / 2U;
            const Side side = bid ? Side::Buy : Side::Sell;
            const Price price =
                bid ? static_cast<Price>(index + 1U)
                    : static_cast<Price>(1'025U + (index - config.max_orders / 2U));
            CHECK(full.add_limit_order(static_cast<OrderId>(index + 1U), side, price, 1, index + 1U).status ==
                  Status::Accepted);
        }
        CHECK(full.live_order_count() == config.max_orders);

        std::array<std::byte, kSnapshotTestBufferSize> full_buffer{};
        const SnapshotWriteResult full_snapshot = serialize(full, full_buffer);
        CHECK(full_snapshot.status == Status::Accepted);
        OrderBook full_restored(config);
        CHECK(restore(full_restored,
                      std::span<const std::byte>(full_buffer.data(), full_snapshot.bytes_written)) ==
              Status::Accepted);
        CHECK(full_restored.live_order_count() == config.max_orders);
        check_book_snapshots_equal(full_restored, full, "near_capacity_snapshot_round_trip");
    }
}

void test_snapshot_restore_then_encoded_command_replay_is_deterministic()
{
    constexpr InstrumentId instrument_id = 503;
    const BookConfig config{90, 110, 32, 128, 1};
    const InstrumentConfig instruments[] = {
        InstrumentConfig{instrument_id, config},
    };

    MatchingEngine original(instruments);
    CHECK(original.add_limit_order(instrument_id, 1, Side::Buy, 100, 10, 1).status ==
          Status::Accepted);
    CHECK(original.add_limit_order(instrument_id, 2, Side::Buy, 99, 6, 2).status ==
          Status::Accepted);
    CHECK(original.add_limit_order(instrument_id, 3, Side::Sell, 105, 8, 3).status ==
          Status::Accepted);

    std::array<std::byte, kSnapshotTestBufferSize> snapshot_buffer{};
    const SnapshotWriteResult snapshot = serialize(original, snapshot_buffer);
    CHECK(snapshot.status == Status::Accepted);

    MatchingEngine restored(instruments);
    CHECK(restore(restored,
                  std::span<const std::byte>(snapshot_buffer.data(), snapshot.bytes_written)) ==
          Status::Accepted);
    check_engine_snapshots_equal(restored, original, "encoded_replay_initial_restore");

    constexpr std::array<Command, 6> commands{
        Command{instrument_id, CommandOp::Add, 4, Side::Sell, 100, 4, TimeInForce::Gtc, 10},
        Command{instrument_id, CommandOp::Modify, 1, Side::Buy, 0, 3, TimeInForce::Gtc, 11},
        Command{instrument_id, CommandOp::Replace, 2, Side::Buy, 101, 6, TimeInForce::Gtc, 12},
        Command{instrument_id, CommandOp::Add, 5, Side::Sell, 101, 2, TimeInForce::Gtc, 13},
        Command{instrument_id, CommandOp::Cancel, 3, Side::Sell, 0, 0, TimeInForce::Gtc, 14},
        Command{instrument_id, CommandOp::Market, 6, Side::Sell, 0, 3, TimeInForce::Gtc, 15},
    };

    for (const Command& command : commands) {
        std::array<std::byte, kCommandWireSize> encoded{};
        CHECK(encode(command, encoded) == Status::Accepted);
        const DispatchResult expected = original.dispatch(encoded);
        const DispatchResult actual = restored.dispatch(encoded);
        check_dispatch_results_equal(actual, expected, "snapshot_encoded_command_replay");
        check_engine_snapshots_equal(restored, original, "snapshot_encoded_command_replay_state");
    }
}

void test_book_snapshot_continue_trading_matches_fresh()
{
    const BookConfig config{90, 130, 32, 128, 1};
    OrderBook fresh(config);

    CHECK(fresh.add_limit_order(1, Side::Buy, 99, 10, 1).status == Status::Accepted);
    CHECK(fresh.add_limit_order(2, Side::Buy, 98, 7, 2).status == Status::Accepted);
    CHECK(fresh.add_limit_order(3, Side::Sell, 105, 9, 3).status == Status::Accepted);
    CHECK(fresh.add_limit_order(4, Side::Sell, 106, 4, 4).status == Status::Accepted);
    CHECK(fresh.replace_order(2, 100, 7, 5, TimeInForce::Gtc).status == Status::Accepted);
    CHECK(fresh.modify_order(3, 6, 6).status == Status::Accepted);

    std::array<std::byte, kSnapshotTestBufferSize> buffer{};
    const SnapshotWriteResult snapshot = serialize(fresh, buffer);
    CHECK(snapshot.status == Status::Accepted);

    OrderBook restored(config);
    CHECK(restore(restored, std::span<const std::byte>(buffer.data(), snapshot.bytes_written)) == Status::Accepted);
    CHECK(restored.last_events().empty());
    check_book_snapshots_equal(restored, fresh, "continue_initial_restore");

    const AddOrderResult fresh_add = fresh.add_limit_order(10, Side::Sell, 99, 6, 10, TimeInForce::Gtc);
    const AddOrderResult restored_add = restored.add_limit_order(10, Side::Sell, 99, 6, 10, TimeInForce::Gtc);
    check_add_result(restored_add, fresh_add, "snapshot_continue_add");
    check_book_snapshots_equal(restored, fresh, "snapshot_continue_add_state");

    const ModifyResult fresh_modify = fresh.modify_order(1, 2, 11);
    const ModifyResult restored_modify = restored.modify_order(1, 2, 11);
    check_modify_result(restored_modify, fresh_modify, "snapshot_continue_modify");
    check_book_snapshots_equal(restored, fresh, "snapshot_continue_modify_state");

    const ReplaceResult fresh_replace = fresh.replace_order(4, 101, 4, 12, TimeInForce::Ioc);
    const ReplaceResult restored_replace = restored.replace_order(4, 101, 4, 12, TimeInForce::Ioc);
    check_replace_result(restored_replace, fresh_replace, "snapshot_continue_replace");
    check_book_snapshots_equal(restored, fresh, "snapshot_continue_replace_state");

    const MatchResult fresh_market = fresh.match_market_order(Side::Buy, 3, 900, 13);
    const MatchResult restored_market = restored.match_market_order(Side::Buy, 3, 900, 13);
    check_match_result(restored_market, fresh_market, "snapshot_continue_market");
    check_book_snapshots_equal(restored, fresh, "snapshot_continue_market_state");
}

void test_book_snapshot_restore_preserves_event_sequence_numbering()
{
    const BookConfig config{90, 130, 32, 128, 1};
    OrderBook fresh(config);

    const AddOrderResult first = fresh.add_limit_order(1, Side::Buy, 100, 5, 1);
    CHECK(first.status == Status::Accepted);
    CHECK(first.events.size() == 2U);
    const AddOrderResult second = fresh.add_limit_order(2, Side::Sell, 105, 7, 2);
    CHECK(second.status == Status::Accepted);
    CHECK(second.events.size() == 2U);
    CHECK(second.events.front().sequence == first.events.back().sequence + 1U);

    const SequenceNumber last_sequence_before_snapshot = fresh.last_events().back().sequence;

    std::array<std::byte, kSnapshotTestBufferSize> buffer{};
    const SnapshotWriteResult snapshot = serialize(fresh, buffer);
    CHECK(snapshot.status == Status::Accepted);

    OrderBook restored(config);
    CHECK(restore(restored, std::span<const std::byte>(buffer.data(), snapshot.bytes_written)) == Status::Accepted);
    CHECK(restored.last_events().empty());

    const AddOrderResult fresh_add = fresh.add_limit_order(3, Side::Buy, 99, 4, 3, TimeInForce::Gtc);
    const AddOrderResult restored_add = restored.add_limit_order(3, Side::Buy, 99, 4, 3, TimeInForce::Gtc);
    check_add_result(restored_add, fresh_add, "snapshot_restore_event_sequence");
    CHECK(restored_add.events.size() == 2U);
    CHECK(restored_add.events.front().sequence == last_sequence_before_snapshot + 1U);
    CHECK(restored_add.events.back().sequence == restored_add.events.front().sequence + 1U);
    check_book_snapshots_equal(restored, fresh, "snapshot_restore_event_sequence_state");
}

void test_engine_snapshot_round_trip_multi_instrument()
{
    constexpr InstrumentId instrument_a = 501;
    constexpr InstrumentId instrument_b = 502;
    const BookConfig config_a{90, 130, 32, 128, 1};
    const BookConfig config_b{50, 80, 24, 96, 1};
    const InstrumentConfig configs[] = {
        InstrumentConfig{instrument_a, config_a, 1, 1},
        InstrumentConfig{instrument_b, config_b, 1, 5},
    };

    MatchingEngine engine(configs);
    CHECK(engine.valid());
    CHECK(engine.add_limit_order(instrument_a, 1, Side::Buy, 100, 10, 1).status == Status::Accepted);
    CHECK(engine.add_limit_order(instrument_a, 2, Side::Sell, 105, 6, 2).status == Status::Accepted);
    CHECK(engine.add_limit_order(instrument_b, 1, Side::Sell, 60, 15, 1).status == Status::Accepted);
    CHECK(engine.add_limit_order(instrument_b, 2, Side::Buy, 55, 20, 2).status == Status::Accepted);
    CHECK(engine.modify(instrument_b, 2, 10, 3).status == Status::Accepted);
    CHECK(engine.replace(instrument_a, 1, 101, 8, 4, TimeInForce::Gtc).status == Status::Accepted);

    std::array<std::byte, kSnapshotTestBufferSize> buffer{};
    const SnapshotWriteResult snapshot = serialize(engine, buffer);
    CHECK(snapshot.status == Status::Accepted);
    CHECK(snapshot.bytes_written > 0);

    MatchingEngine restored(configs);
    const Status restore_status =
        restore(restored, std::span<const std::byte>(buffer.data(), snapshot.bytes_written));
    CHECK(restore_status == Status::Accepted);
    CHECK(restored.last_events(instrument_a).empty());
    CHECK(restored.last_events(instrument_b).empty());
    check_engine_snapshots_equal(restored, engine, "engine_snapshot_round_trip");

    const AddOrderResult fresh_add =
        engine.add_limit_order(instrument_a, 3, Side::Sell, 101, 3, 10, TimeInForce::Gtc);
    const AddOrderResult restored_add =
        restored.add_limit_order(instrument_a, 3, Side::Sell, 101, 3, 10, TimeInForce::Gtc);
    check_add_result(restored_add, fresh_add, "engine_snapshot_continue_add");
    check_engine_snapshots_equal(restored, engine, "engine_snapshot_continue_state");
}

void test_snapshot_rejects_corrupt_and_truncated_buffers()
{
    const BookConfig config{90, 130, 16, 64, 1};
    OrderBook book(config);
    CHECK(book.add_limit_order(1, Side::Buy, 100, 10, 1).status == Status::Accepted);
    CHECK(book.add_limit_order(2, Side::Sell, 105, 7, 2).status == Status::Accepted);

    std::array<std::byte, kSnapshotTestBufferSize> buffer{};
    const SnapshotWriteResult snapshot = serialize(book, buffer);
    CHECK(snapshot.status == Status::Accepted);

    for (std::size_t length = 0; length < snapshot.bytes_written; ++length) {
        OrderBook target(config);
        const Status status = restore(target, std::span<const std::byte>(buffer.data(), length));
        CHECK(status != Status::Accepted);
    }

    std::array<std::byte, kSnapshotTestBufferSize> corrupted = buffer;
    corrupted[0] = static_cast<std::byte>('X');
    OrderBook bad_magic_target(config);
    CHECK(restore(bad_magic_target, std::span<const std::byte>(corrupted.data(), snapshot.bytes_written)) ==
          Status::SnapshotFormatMismatch);

    corrupted = buffer;
    corrupted[4] = static_cast<std::byte>(kSnapshotFormatVersion + 1U);
    OrderBook bad_version_target(config);
    CHECK(restore(bad_version_target, std::span<const std::byte>(corrupted.data(), snapshot.bytes_written)) ==
          Status::SnapshotVersionMismatch);

    const BookConfig other_config{90, 131, 16, 64, 1};
    OrderBook wrong_config_target(other_config);
    CHECK(restore(wrong_config_target, std::span<const std::byte>(buffer.data(), snapshot.bytes_written)) ==
          Status::SnapshotConfigurationMismatch);

    std::array<std::byte, 4> tiny_buffer{};
    const SnapshotWriteResult tiny_result = serialize(book, tiny_buffer);
    CHECK(tiny_result.status == Status::BufferTooSmall);
    CHECK(tiny_result.bytes_written == 0);

    constexpr InstrumentId instrument_a = 601;
    constexpr InstrumentId instrument_b = 602;
    const InstrumentConfig configs[] = {
        InstrumentConfig{instrument_a, config},
        InstrumentConfig{instrument_b, config},
    };
    MatchingEngine engine(configs);
    CHECK(engine.add_limit_order(instrument_a, 1, Side::Buy, 100, 1, 1).status == Status::Accepted);
    CHECK(engine.add_limit_order(instrument_b, 1, Side::Sell, 101, 1, 1).status == Status::Accepted);
    const SnapshotWriteResult engine_snapshot = serialize(engine, buffer);
    CHECK(engine_snapshot.status == Status::Accepted);

    for (std::size_t length = 0; length < engine_snapshot.bytes_written; length += 7U) {
        MatchingEngine target(configs);
        const Status status = restore(target, std::span<const std::byte>(buffer.data(), length));
        CHECK(status != Status::Accepted);
    }
}

void test_snapshot_rejects_duplicate_and_inconsistent_records_atomically()
{
    const BookConfig config{90, 110, 16, 64, 1, 64, PriceLevelMode::Dense};
    OrderBook source(config);
    CHECK(source.add_limit_order(1, Side::Buy, 100, 5, 1).status == Status::Accepted);
    CHECK(source.add_limit_order(2, Side::Buy, 101, 7, 2).status == Status::Accepted);
    CHECK(source.add_limit_order(3, Side::Sell, 105, 11, 3).status == Status::Accepted);

    std::array<std::byte, kSnapshotTestBufferSize> buffer{};
    const SnapshotWriteResult snapshot = serialize(source, buffer);
    CHECK(snapshot.status == Status::Accepted);

    const auto check_atomic_rejection =
        [&](const std::array<std::byte, kSnapshotTestBufferSize>& corrupted,
            const char* const context) {
            OrderBook target(config);
            OrderBook expected(config);
            CHECK(target.add_limit_order(50, Side::Sell, 108, 3, 10).status ==
                  Status::Accepted);
            CHECK(expected.add_limit_order(50, Side::Sell, 108, 3, 10).status ==
                  Status::Accepted);
            CHECK(restore(target,
                          std::span<const std::byte>(corrupted.data(), snapshot.bytes_written)) ==
                  Status::SnapshotFormatMismatch);
            check_book_snapshots_equal(target, expected, context);
        };

    constexpr std::size_t order_side_offset = sizeof(OrderId);
    constexpr std::size_t order_price_offset = order_side_offset + sizeof(std::uint8_t);
    constexpr std::size_t order_quantity_offset = order_price_offset + sizeof(Price);
    constexpr std::size_t order_sequence_offset =
        order_quantity_offset + sizeof(Quantity) + sizeof(Timestamp);
    constexpr std::size_t order_initial_quantity_offset =
        order_sequence_offset + sizeof(SequenceNumber) + sizeof(ParticipantId) +
        sizeof(std::uint8_t);
    constexpr std::size_t order_state_offset =
        order_initial_quantity_offset + sizeof(Quantity);
    const std::size_t levels_offset =
        detail::kBookHeaderWireSize + 3U * detail::kBookOrderWireSize;
    constexpr std::size_t level_price_offset = sizeof(std::uint8_t);
    constexpr std::size_t level_aggregate_offset = level_price_offset + sizeof(Price);
    constexpr std::size_t level_order_count_offset =
        level_aggregate_offset + sizeof(Quantity);

    std::array<std::byte, kSnapshotTestBufferSize> corrupted = buffer;
    write_u64_le(corrupted,
                 detail::kBookHeaderWireSize + detail::kBookOrderWireSize,
                 2);
    check_atomic_rejection(corrupted, "duplicate_order_id_atomic_rejection");

    corrupted = buffer;
    write_u64_le(corrupted,
                 detail::kBookHeaderWireSize + detail::kBookOrderWireSize +
                     order_sequence_offset,
                 2);
    check_atomic_rejection(corrupted, "duplicate_sequence_atomic_rejection");

    corrupted = buffer;
    corrupted[detail::kBookHeaderWireSize + order_side_offset] =
        static_cast<std::byte>(2);
    check_atomic_rejection(corrupted, "invalid_order_side_atomic_rejection");

    corrupted = buffer;
    write_u64_le(corrupted,
                 detail::kBookHeaderWireSize + order_price_offset,
                 200);
    check_atomic_rejection(corrupted, "invalid_order_price_atomic_rejection");

    corrupted = buffer;
    write_u64_le(corrupted,
                 detail::kBookHeaderWireSize + order_quantity_offset,
                 0);
    check_atomic_rejection(corrupted, "invalid_order_quantity_atomic_rejection");

    corrupted = buffer;
    write_u64_le(corrupted,
                 detail::kBookHeaderWireSize + order_initial_quantity_offset,
                 0);
    check_atomic_rejection(corrupted, "invalid_initial_quantity_atomic_rejection");

    corrupted = buffer;
    corrupted[detail::kBookHeaderWireSize + order_state_offset] =
        static_cast<std::byte>(static_cast<std::uint8_t>(OrderState::Filled));
    check_atomic_rejection(corrupted, "non_resting_order_state_atomic_rejection");

    corrupted = buffer;
    corrupted[detail::kBookHeaderWireSize + order_state_offset] =
        static_cast<std::byte>(static_cast<std::uint8_t>(OrderState::PartiallyFilled));
    check_atomic_rejection(corrupted, "inconsistent_partial_state_atomic_rejection");

    corrupted = buffer;
    write_u64_le(corrupted,
                 levels_offset + detail::kLevelWireSize + level_price_offset,
                 101);
    check_atomic_rejection(corrupted, "duplicate_level_atomic_rejection");

    corrupted = buffer;
    write_u64_le(corrupted, levels_offset + level_aggregate_offset, 8);
    check_atomic_rejection(corrupted, "incorrect_level_aggregate_atomic_rejection");

    corrupted = buffer;
    write_u32_le(corrupted, levels_offset + level_order_count_offset, 2);
    check_atomic_rejection(corrupted, "incorrect_level_count_atomic_rejection");

    corrupted = buffer;
    write_u64_le(corrupted, levels_offset + level_price_offset, 102);
    check_atomic_rejection(corrupted, "order_missing_correct_level_atomic_rejection");

    corrupted = buffer;
    corrupted[levels_offset] = static_cast<std::byte>(Side::Sell);
    check_atomic_rejection(corrupted, "order_assigned_to_wrong_side_level_atomic_rejection");
}

void test_engine_snapshot_validation_failure_is_atomic()
{
    constexpr InstrumentId instrument_a = 611;
    constexpr InstrumentId instrument_b = 612;
    const BookConfig config{90, 110, 16, 64, 1};
    const InstrumentConfig instruments[] = {
        InstrumentConfig{instrument_a, config},
        InstrumentConfig{instrument_b, config},
    };

    MatchingEngine source(instruments);
    CHECK(source.add_limit_order(instrument_a, 1, Side::Buy, 100, 5, 1).status ==
          Status::Accepted);
    CHECK(source.add_limit_order(instrument_b, 2, Side::Sell, 105, 7, 2).status ==
          Status::Accepted);

    std::array<std::byte, kSnapshotTestBufferSize> buffer{};
    const SnapshotWriteResult snapshot = serialize(source, buffer);
    CHECK(snapshot.status == Status::Accepted);

    const std::size_t first_book_offset =
        detail::kEngineHeaderWireSize + detail::kInstrumentConfigWireSize;
    detail::BookHeaderInfo first_book{};
    CHECK(detail::parse_book_header(
              std::span<const std::byte>(buffer.data() + first_book_offset,
                                         snapshot.bytes_written - first_book_offset),
              first_book,
              false) == Status::Accepted);
    const std::size_t second_book_offset =
        first_book_offset + first_book.end_offset + detail::kInstrumentConfigWireSize;
    detail::BookHeaderInfo second_book{};
    CHECK(detail::parse_book_header(
              std::span<const std::byte>(buffer.data() + second_book_offset,
                                         snapshot.bytes_written - second_book_offset),
              second_book,
              false) == Status::Accepted);

    write_u64_le(buffer,
                 second_book_offset + second_book.levels_offset +
                     sizeof(std::uint8_t) + sizeof(Price),
                 8);

    MatchingEngine target(instruments);
    MatchingEngine expected(instruments);
    CHECK(target.add_limit_order(instrument_a, 50, Side::Buy, 95, 3, 10).status ==
          Status::Accepted);
    CHECK(target.add_limit_order(instrument_b, 51, Side::Sell, 108, 4, 11).status ==
          Status::Accepted);
    CHECK(expected.add_limit_order(instrument_a, 50, Side::Buy, 95, 3, 10).status ==
          Status::Accepted);
    CHECK(expected.add_limit_order(instrument_b, 51, Side::Sell, 108, 4, 11).status ==
          Status::Accepted);

    CHECK(restore(target,
                  std::span<const std::byte>(buffer.data(), snapshot.bytes_written)) ==
          Status::SnapshotFormatMismatch);
    check_engine_snapshots_equal(target, expected, "engine_snapshot_atomic_rejection");
}

void test_snapshot_rejects_crossed_book_without_mutating_target()
{
    const BookConfig config{90, 110, 16, 64, 1, 64, PriceLevelMode::Dense};
    OrderBook source(config);
    CHECK(source.add_limit_order(1, Side::Buy, 100, 5, 1).status == Status::Accepted);
    CHECK(source.add_limit_order(2, Side::Sell, 105, 7, 2).status == Status::Accepted);

    std::array<std::byte, kSnapshotTestBufferSize> buffer{};
    const SnapshotWriteResult snapshot = serialize(source, buffer);
    CHECK(snapshot.status == Status::Accepted);
    CHECK(snapshot.bytes_written == detail::kBookHeaderWireSize + 2U * detail::kBookOrderWireSize +
                                        2U * detail::kLevelWireSize);

    const std::size_t ask_order_price =
        detail::kBookHeaderWireSize + detail::kBookOrderWireSize + sizeof(OrderId) + sizeof(std::uint8_t);
    const std::size_t ask_level_price = detail::kBookHeaderWireSize + 2U * detail::kBookOrderWireSize +
                                        detail::kLevelWireSize + sizeof(std::uint8_t);
    const auto crossed_price = static_cast<std::uint64_t>(100);
    const std::span<std::byte> snapshot_bytes(buffer.data(), snapshot.bytes_written);
    write_u64_le(snapshot_bytes, ask_order_price, crossed_price);
    write_u64_le(snapshot_bytes, ask_level_price, crossed_price);

    OrderBook target(config);
    OrderBook expected(config);
    CHECK(target.add_limit_order(50, Side::Buy, 95, 3, 10).status == Status::Accepted);
    CHECK(expected.add_limit_order(50, Side::Buy, 95, 3, 10).status == Status::Accepted);

    CHECK(restore(target, snapshot_bytes) == Status::SnapshotFormatMismatch);
    check_book_snapshots_equal(target, expected, "crossed_snapshot_atomic_rejection");
}

void test_snapshot_rejects_fifo_sequence_regression_without_mutating_target()
{
    const BookConfig config{90, 110, 16, 64, 1, 64, PriceLevelMode::Sparse};
    OrderBook source(config);
    CHECK(source.add_limit_order(1, Side::Buy, 100, 5, 1).status == Status::Accepted);
    CHECK(source.add_limit_order(2, Side::Buy, 100, 7, 2).status == Status::Accepted);

    std::array<std::byte, kSnapshotTestBufferSize> buffer{};
    const SnapshotWriteResult snapshot = serialize(source, buffer);
    CHECK(snapshot.status == Status::Accepted);

    constexpr std::size_t sequence_field_offset =
        sizeof(OrderId) + sizeof(std::uint8_t) + sizeof(Price) + sizeof(Quantity) + sizeof(Timestamp);
    const std::size_t first_sequence = detail::kBookHeaderWireSize + sequence_field_offset;
    const std::size_t second_sequence =
        detail::kBookHeaderWireSize + detail::kBookOrderWireSize + sequence_field_offset;
    const std::span<std::byte> snapshot_bytes(buffer.data(), snapshot.bytes_written);
    write_u64_le(snapshot_bytes, first_sequence, 2);
    write_u64_le(snapshot_bytes, second_sequence, 1);

    OrderBook target(config);
    OrderBook expected(config);
    CHECK(target.add_limit_order(50, Side::Sell, 108, 3, 10).status == Status::Accepted);
    CHECK(expected.add_limit_order(50, Side::Sell, 108, 3, 10).status == Status::Accepted);

    CHECK(restore(target, snapshot_bytes) == Status::SnapshotFormatMismatch);
    check_book_snapshots_equal(target, expected, "fifo_sequence_snapshot_atomic_rejection");
}

void run_seeded_conformance(const std::uint64_t seed)
{
    const BookConfig config{90, 130, 48, 256, 2};
    OrderBook actual(config);
    ReferenceOrderBook expected(config);
    SplitMix64 rng(seed);

    for (std::uint32_t step = 0; step < 2'000U; ++step) {
        const std::uint32_t action = rng.uniform(100U);
        if (action < 44U) {
            run_add(actual,
                    expected,
                    random_order_id(rng),
                    random_side(rng),
                    random_price(rng),
                    random_quantity(rng),
                    static_cast<Timestamp>(step + 1U),
                    random_time_in_force(rng));
        } else if (action < 60U) {
            run_cancel(actual, expected, random_order_id(rng));
        } else if (action < 74U) {
            run_modify(actual, expected, random_order_id(rng), random_quantity(rng));
        } else if (action < 90U) {
            run_replace(actual,
                        expected,
                        random_order_id(rng),
                        random_price(rng),
                        random_quantity(rng),
                        static_cast<Timestamp>(step + 1U),
                        random_time_in_force(rng));
        } else {
            run_market(actual, expected, random_side(rng), random_quantity(rng));
        }
    }
}

void test_seeded_conformance()
{
    constexpr std::uint64_t seeds[] = {
        0x6a09e667f3bcc909ULL,
        0xbb67ae8584caa73bULL,
        0x3c6ef372fe94f82bULL,
        0xa54ff53a5f1d36f1ULL,
        0x510e527fade682d1ULL,
        0x9b05688c2b3e6c1fULL,
        0x1f83d9abfb41bd6bULL,
        0x5be0cd19137e2179ULL,
        0xcbbb9d5dc1059ed8ULL,
        0x629a292a367cd507ULL,
        0x9159015a3070dd17ULL,
        0x152fecd8f70e5939ULL,
        0x67332667ffc00b31ULL,
        0x8eb44a8768581511ULL,
        0xdb0c2e0d64f98fa7ULL,
        0x47b5481dbefa4fa4ULL,
    };

    for (const std::uint64_t seed : seeds) {
        run_seeded_conformance(seed);
    }
}

void run_seeded_sparse_conformance(const std::uint64_t seed)
{
    const BookConfig config{1, 1'000'000, 64, 256, 1, 0, PriceLevelMode::Sparse};
    OrderBook actual(config);
    ReferenceOrderBook expected(config);
    SplitMix64 rng(seed);

    CHECK(actual.stats().bids.mode == PriceLevelMode::Sparse);
    CHECK(actual.stats().asks.mode == PriceLevelMode::Sparse);

    for (std::uint32_t step = 0; step < 2'000U; ++step) {
        const std::uint32_t action = rng.uniform(100U);
        if (action < 44U) {
            run_add(actual,
                    expected,
                    random_order_id(rng),
                    random_side(rng),
                    random_sparse_price(rng),
                    random_quantity(rng),
                    static_cast<Timestamp>(step + 1U),
                    random_time_in_force(rng));
        } else if (action < 60U) {
            run_cancel(actual, expected, random_order_id(rng));
        } else if (action < 74U) {
            run_modify(actual, expected, random_order_id(rng), random_quantity(rng));
        } else if (action < 90U) {
            run_replace(actual,
                        expected,
                        random_order_id(rng),
                        random_sparse_price(rng),
                        random_quantity(rng),
                        static_cast<Timestamp>(step + 1U),
                        random_time_in_force(rng));
        } else {
            run_market(actual, expected, random_side(rng), random_quantity(rng));
        }
    }
}

void test_sparse_seeded_conformance()
{
    constexpr std::uint64_t seeds[] = {
        0x94d049bb133111ebULL,
        0xbf58476d1ce4e5b9ULL,
        0x9e3779b97f4a7c15ULL,
        0xd1b54a32d192ed03ULL,
    };

    for (const std::uint64_t seed : seeds) {
        run_seeded_sparse_conformance(seed);
    }
}

void run_seeded_multi_instrument_conformance(const std::uint64_t seed)
{
    constexpr InstrumentId instrument_a = 401;
    constexpr InstrumentId instrument_b = 402;
    const BookConfig config{90, 130, 48, 256, 2};
    const InstrumentConfig configs[] = {
        InstrumentConfig{instrument_a, config},
        InstrumentConfig{instrument_b, config},
    };

    MatchingEngine actual(configs);
    ReferenceOrderBook expected_a(config, instrument_a);
    ReferenceOrderBook expected_b(config, instrument_b);
    SplitMix64 rng(seed);

    CHECK(actual.valid());

    for (std::uint32_t step = 0; step < 2'000U; ++step) {
        const InstrumentId instrument_id = rng.uniform(2U) == 0U ? instrument_a : instrument_b;
        ReferenceOrderBook& expected = instrument_id == instrument_a ? expected_a : expected_b;
        const std::uint32_t action = rng.uniform(100U);
        if (action < 44U) {
            run_engine_add(actual,
                           expected,
                           instrument_id,
                           random_order_id(rng),
                           random_side(rng),
                           random_price(rng),
                           random_quantity(rng),
                           static_cast<Timestamp>(step + 1U),
                           random_time_in_force(rng));
        } else if (action < 60U) {
            run_engine_cancel(actual, expected, instrument_id, random_order_id(rng));
        } else if (action < 74U) {
            run_engine_modify(actual, expected, instrument_id, random_order_id(rng), random_quantity(rng));
        } else if (action < 90U) {
            run_engine_replace(actual,
                               expected,
                               instrument_id,
                               random_order_id(rng),
                               random_price(rng),
                               random_quantity(rng),
                               static_cast<Timestamp>(step + 1U),
                               random_time_in_force(rng));
        } else {
            run_engine_market(actual,
                              expected,
                              instrument_id,
                              random_side(rng),
                              random_quantity(rng),
                              static_cast<OrderId>(900'000U + step),
                              static_cast<Timestamp>(step + 1U));
        }

        check_engine_book_equal(actual, instrument_a, expected_a);
        check_engine_book_equal(actual, instrument_b, expected_b);
    }
}

void test_seeded_multi_instrument_conformance()
{
    constexpr std::uint64_t seeds[] = {
        0x243f6a8885a308d3ULL,
        0x13198a2e03707344ULL,
        0xa4093822299f31d0ULL,
        0x082efa98ec4e6c89ULL,
        0x452821e638d01377ULL,
        0xbe5466cf34e90c6cULL,
        0xc0ac29b7c97c50ddULL,
        0x3f84d5b5b5470917ULL,
    };

    for (const std::uint64_t seed : seeds) {
        run_seeded_multi_instrument_conformance(seed);
    }
}

[[nodiscard]] BookConfig venue_config(const PriceLevelMode mode = PriceLevelMode::Dense,
                                      const Quantity lot_size = 1,
                                      const SelfTradePolicy policy = SelfTradePolicy::Disabled,
                                      const std::uint32_t market_data_capacity = 0)
{
    BookConfig config{90, 110, 64, 128, 1, 256, mode};
    config.lot_size = lot_size;
    config.self_trade_policy = policy;
    config.market_data_capacity = market_data_capacity;
    return config;
}

template <typename Result>
void check_stp_result_flags(const Result& result, const SelfTradePolicy policy)
{
    CHECK(result.aggressor_cancelled_by_stp ==
          (policy == SelfTradePolicy::CancelAggressor ||
           policy == SelfTradePolicy::CancelBoth));
    CHECK(result.resting_orders_cancelled_by_stp ==
          ((policy == SelfTradePolicy::CancelResting ||
            policy == SelfTradePolicy::CancelBoth)
               ? 1U
               : 0U));
}

[[nodiscard]] constexpr Status expected_limit_stp_status(
    const SelfTradePolicy policy) noexcept
{
    return policy == SelfTradePolicy::CancelResting ? Status::Accepted
                                                    : Status::SelfTradePrevented;
}

[[nodiscard]] constexpr Status expected_market_stp_status(
    const SelfTradePolicy policy) noexcept
{
    return policy == SelfTradePolicy::CancelResting ? Status::NoLiquidity
                                                    : Status::SelfTradePrevented;
}

void test_lot_size_rules()
{
    OrderBook book(venue_config(PriceLevelMode::Dense, 10), 801);
    CHECK(book.add_limit_order(1, Side::Buy, 100, 9).status == Status::LotSizeViolation);
    CHECK(book.find_order(1) == nullptr);
    CHECK(book.add_limit_order(1, Side::Buy, 100, 10).status == Status::Accepted);
    CHECK(book.add_limit_order(2, Side::Buy, 99, 20).status == Status::Accepted);
    CHECK(book.modify_order(1, 5).status == Status::LotSizeViolation);
    CHECK(book.find_order(1)->quantity == 10);
    CHECK(book.replace_order(1, 101, 15).status == Status::LotSizeViolation);
    CHECK(book.find_order(1)->price == 100);
    CHECK(book.match_market_order(Side::Sell, 5).status == Status::LotSizeViolation);

    CHECK(book.add_limit_order(3, Side::Sell, 100, 20).status == Status::PartiallyFilled);
    CHECK(book.find_order(1) == nullptr);
    CHECK(book.find_order(3)->quantity == 10);
    CHECK(book.depth_at_price(Side::Buy, 99) == 20);
}

void test_post_only_entry_and_replace()
{
    OrderBook book(venue_config(PriceLevelMode::Dense, 1, SelfTradePolicy::Disabled, 64), 802);
    CHECK(book.add_limit_order(1, Side::Sell, 100, 10, 1).status == Status::Accepted);
    CHECK(book.add_limit_order(2, Side::Buy, 99, 5, 2, TimeInForce::Gtc, 7, true).status ==
          Status::Accepted);
    CHECK(book.find_order(2)->post_only);

    const SequenceNumber sequence_before = book.market_data_sequence();
    const AddOrderResult crossing =
        book.add_limit_order(3, Side::Buy, 100, 5, 3, TimeInForce::Gtc, 8, true);
    CHECK(crossing.status == Status::PostOnlyWouldCross);
    CHECK(book.last_market_data_events().empty());
    CHECK(book.market_data_sequence() == sequence_before);
    CHECK(book.find_order(3) == nullptr);
    CHECK(book.add_limit_order(3, Side::Buy, 98, 5, 4).status == Status::Accepted);

    const std::uint64_t before_replace = book.state_checksum();
    const ReplaceResult rejected =
        book.replace_order(2, 100, 5, 5, TimeInForce::Gtc, true);
    CHECK(rejected.status == Status::PostOnlyWouldCross);
    CHECK(book.state_checksum() == before_replace);
    CHECK(book.find_order(2)->price == 99);
    CHECK(book.replace_order(2, 99, 4, 6, TimeInForce::Gtc, true).status ==
          Status::Accepted);
    CHECK(book.find_order(2)->quantity == 4);
    CHECK(book.replace_order(2, 98, 4, 7, TimeInForce::Gtc, true).status ==
          Status::Accepted);
    CHECK(book.find_order(2)->price == 98);
    CHECK(book.replace_order(2, 98, 4, 8, TimeInForce::Ioc, true).status ==
          Status::InvalidPostOnlyTimeInForce);
}

void check_stp_policy(const PriceLevelMode mode, const SelfTradePolicy policy)
{
    OrderBook book(venue_config(mode, 1, policy, 128), 803);
    CHECK(book.add_limit_order(1, Side::Sell, 100, 10, 1, TimeInForce::Gtc, 42).status ==
          Status::Accepted);
    const AddOrderResult result =
        book.add_limit_order(2, Side::Buy, 100, 10, 2, TimeInForce::Gtc, 42);

    check_stp_result_flags(result, policy);

    if (policy == SelfTradePolicy::Disabled) {
        CHECK(result.status == Status::Filled);
        CHECK(result.executed_quantity == 10);
        CHECK(book.find_order(1) == nullptr);
        return;
    }
    if (policy == SelfTradePolicy::CancelAggressor) {
        CHECK(result.status == Status::SelfTradePrevented);
        CHECK(book.find_order(1) != nullptr);
        CHECK(book.find_order(2) == nullptr);
        return;
    }
    if (policy == SelfTradePolicy::CancelResting) {
        CHECK(result.status == Status::Accepted);
        CHECK(book.find_order(1) == nullptr);
        CHECK(book.find_order(2) != nullptr);
        CHECK(book.find_order(2)->quantity == 10);
        return;
    }

    CHECK(result.status == Status::SelfTradePrevented);
    CHECK(book.find_order(1) == nullptr);
    CHECK(book.find_order(2) == nullptr);
}

void test_all_stp_policies_dense_and_sparse()
{
    constexpr SelfTradePolicy policies[]{
        SelfTradePolicy::Disabled,
        SelfTradePolicy::CancelAggressor,
        SelfTradePolicy::CancelResting,
        SelfTradePolicy::CancelBoth,
    };
    for (const PriceLevelMode mode : {PriceLevelMode::Dense, PriceLevelMode::Sparse}) {
        for (const SelfTradePolicy policy : policies) {
            check_stp_policy(mode, policy);
        }
    }
}

void test_stp_partial_third_party_ioc_fok_and_fifo()
{
    constexpr SelfTradePolicy policies[]{
        SelfTradePolicy::CancelAggressor,
        SelfTradePolicy::CancelResting,
        SelfTradePolicy::CancelBoth,
    };
    for (const SelfTradePolicy policy : policies) {
        OrderBook book(venue_config(PriceLevelMode::Dense, 1, policy, 128), 804);
        CHECK(book.add_limit_order(1, Side::Sell, 99, 5, 1, TimeInForce::Gtc, 7).status ==
              Status::Accepted);
        CHECK(book.add_limit_order(2, Side::Sell, 100, 10, 2, TimeInForce::Gtc, 42).status ==
              Status::Accepted);
        const AddOrderResult result =
            book.add_limit_order(3, Side::Buy, 100, 10, 3, TimeInForce::Gtc, 42);
        CHECK(result.executed_quantity == 5);
        check_stp_result_flags(result, policy);
        CHECK(book.find_order(1) == nullptr);
        if (policy == SelfTradePolicy::CancelAggressor) {
            CHECK(result.status == Status::PartiallyFilled);
            CHECK(book.find_order(2) != nullptr);
            CHECK(book.find_order(3) == nullptr);
        } else if (policy == SelfTradePolicy::CancelResting) {
            CHECK(result.status == Status::PartiallyFilled);
            CHECK(book.find_order(2) == nullptr);
            CHECK(book.find_order(3) != nullptr);
            CHECK(book.find_order(3)->quantity == 5);
        } else {
            CHECK(result.status == Status::PartiallyFilled);
            CHECK(book.find_order(2) == nullptr);
            CHECK(book.find_order(3) == nullptr);
        }
    }

    OrderBook fok_cancel_aggressor(
        venue_config(PriceLevelMode::Dense, 1, SelfTradePolicy::CancelAggressor, 128), 805);
    CHECK(fok_cancel_aggressor.add_limit_order(1, Side::Sell, 99, 5, 1, TimeInForce::Gtc, 7).status ==
          Status::Accepted);
    CHECK(fok_cancel_aggressor.add_limit_order(2, Side::Sell, 100, 5, 2, TimeInForce::Gtc, 42).status ==
          Status::Accepted);
    const std::uint64_t before = fok_cancel_aggressor.state_checksum();
    CHECK(fok_cancel_aggressor
              .add_limit_order(3, Side::Buy, 100, 10, 3, TimeInForce::Fok, 42)
              .status == Status::FokRejected);
    CHECK(fok_cancel_aggressor.state_checksum() == before);

    OrderBook fok_cancel_resting(
        venue_config(PriceLevelMode::Dense, 1, SelfTradePolicy::CancelResting, 128), 806);
    CHECK(fok_cancel_resting.add_limit_order(1, Side::Sell, 99, 5, 1, TimeInForce::Gtc, 42).status ==
          Status::Accepted);
    CHECK(fok_cancel_resting.add_limit_order(2, Side::Sell, 100, 10, 2, TimeInForce::Gtc, 7).status ==
          Status::Accepted);
    const AddOrderResult fok =
        fok_cancel_resting.add_limit_order(3, Side::Buy, 100, 10, 3, TimeInForce::Fok, 42);
    CHECK(fok.status == Status::Filled);
    CHECK(fok.executed_quantity == 10);
    CHECK(fok_cancel_resting.find_order(1) == nullptr);
    CHECK(fok_cancel_resting.find_order(2) == nullptr);

    OrderBook ioc(venue_config(PriceLevelMode::Dense, 1, SelfTradePolicy::CancelResting, 128), 807);
    CHECK(ioc.add_limit_order(1, Side::Sell, 99, 5, 1, TimeInForce::Gtc, 42).status ==
          Status::Accepted);
    CHECK(ioc.add_limit_order(2, Side::Sell, 100, 3, 2, TimeInForce::Gtc, 7).status ==
          Status::Accepted);
    const AddOrderResult ioc_result =
        ioc.add_limit_order(3, Side::Buy, 100, 10, 3, TimeInForce::Ioc, 42);
    CHECK(ioc_result.status == Status::PartiallyFilled);
    CHECK(ioc_result.executed_quantity == 3);
    CHECK(ioc.find_order(1) == nullptr);
    CHECK(ioc.find_order(2) == nullptr);
    CHECK(ioc.find_order(3) == nullptr);

    OrderBook fifo(venue_config(PriceLevelMode::Dense, 1, SelfTradePolicy::CancelResting, 128), 808);
    CHECK(fifo.add_limit_order(1, Side::Sell, 100, 5, 1, TimeInForce::Gtc, 42).status ==
          Status::Accepted);
    CHECK(fifo.add_limit_order(2, Side::Sell, 100, 5, 2, TimeInForce::Gtc, 7).status ==
          Status::Accepted);
    CHECK(fifo.add_limit_order(3, Side::Sell, 100, 5, 3, TimeInForce::Gtc, 8).status ==
          Status::Accepted);
    const AddOrderResult fifo_result =
        fifo.add_limit_order(4, Side::Buy, 100, 5, 4, TimeInForce::Gtc, 42);
    CHECK(fifo_result.status == Status::Filled);
    CHECK(fifo.find_order(1) == nullptr);
    CHECK(fifo.find_order(2) == nullptr);
    CHECK(fifo.find_order(3) != nullptr);
}

void test_replace_preserves_stp_result_flags()
{
    constexpr SelfTradePolicy policies[]{
        SelfTradePolicy::CancelAggressor,
        SelfTradePolicy::CancelResting,
        SelfTradePolicy::CancelBoth,
    };

    for (const PriceLevelMode mode : {PriceLevelMode::Dense, PriceLevelMode::Sparse}) {
        for (const SelfTradePolicy policy : policies) {
            OrderBook book(venue_config(mode, 1, policy, 128), 812);
            CHECK(book.add_limit_order(1,
                                       Side::Sell,
                                       100,
                                       10,
                                       1,
                                       TimeInForce::Gtc,
                                       42)
                      .status == Status::Accepted);
            CHECK(book.add_limit_order(2,
                                       Side::Buy,
                                       99,
                                       10,
                                       2,
                                       TimeInForce::Gtc,
                                       42)
                      .status == Status::Accepted);

            const ReplaceResult result =
                book.replace_order(2, 100, 10, 3, TimeInForce::Gtc);

            CHECK(result.status == expected_limit_stp_status(policy));
            CHECK(result.executed_quantity == 0U);
            check_stp_result_flags(result, policy);
        }
    }
}

void test_dispatch_preserves_stp_result_flags()
{
    constexpr InstrumentId instrument_id = 813;
    constexpr ParticipantId participant_id = 42;
    constexpr SelfTradePolicy policies[]{
        SelfTradePolicy::CancelAggressor,
        SelfTradePolicy::CancelResting,
        SelfTradePolicy::CancelBoth,
    };

    for (const SelfTradePolicy policy : policies) {
        const BookConfig config =
            venue_config(PriceLevelMode::Dense, 1, policy, 128);
        const InstrumentConfig instruments[]{InstrumentConfig{instrument_id, config}};

        {
            MatchingEngine engine(instruments);
            CHECK(engine
                      .dispatch(VenueCommand{
                          Command{instrument_id,
                                  CommandOp::Add,
                                  1,
                                  Side::Sell,
                                  100,
                                  10,
                                  TimeInForce::Gtc,
                                  1},
                          participant_id,
                          false})
                      .status == Status::Accepted);

            const DispatchResult result = engine.dispatch(VenueCommand{
                Command{instrument_id,
                        CommandOp::Add,
                        2,
                        Side::Buy,
                        100,
                        10,
                        TimeInForce::Gtc,
                        2},
                participant_id,
                false});

            CHECK(result.status == expected_limit_stp_status(policy));
            check_stp_result_flags(result, policy);
        }

        {
            MatchingEngine engine(instruments);
            CHECK(engine
                      .dispatch(VenueCommand{
                          Command{instrument_id,
                                  CommandOp::Add,
                                  1,
                                  Side::Sell,
                                  100,
                                  10,
                                  TimeInForce::Gtc,
                                  1},
                          participant_id,
                          false})
                      .status == Status::Accepted);
            CHECK(engine
                      .dispatch(VenueCommand{
                          Command{instrument_id,
                                  CommandOp::Add,
                                  2,
                                  Side::Buy,
                                  99,
                                  10,
                                  TimeInForce::Gtc,
                                  2},
                          participant_id,
                          false})
                      .status == Status::Accepted);

            const DispatchResult result = engine.dispatch(Command{
                instrument_id,
                CommandOp::Replace,
                2,
                Side::Buy,
                100,
                10,
                TimeInForce::Gtc,
                3});

            CHECK(result.status == expected_limit_stp_status(policy));
            check_stp_result_flags(result, policy);
        }

        {
            MatchingEngine engine(instruments);
            CHECK(engine
                      .dispatch(VenueCommand{
                          Command{instrument_id,
                                  CommandOp::Add,
                                  1,
                                  Side::Sell,
                                  100,
                                  10,
                                  TimeInForce::Gtc,
                                  1},
                          participant_id,
                          false})
                      .status == Status::Accepted);

            const DispatchResult result = engine.dispatch(VenueCommand{
                Command{instrument_id,
                        CommandOp::Market,
                        2,
                        Side::Buy,
                        0,
                        10,
                        TimeInForce::Gtc,
                        2},
                participant_id,
                false});

            CHECK(result.status == expected_market_stp_status(policy));
            CHECK(result.requested_quantity == 10U);
            CHECK(result.remaining_quantity == 10U);
            check_stp_result_flags(result, policy);
        }
    }
}

void test_market_data_ordering_sequences_gap_and_capacity()
{
    OrderBook book(venue_config(PriceLevelMode::Dense, 1, SelfTradePolicy::Disabled, 64), 809);
    const AddOrderResult first = book.add_limit_order(1, Side::Sell, 100, 5, 1);
    CHECK(first.status == Status::Accepted);
    CHECK(book.last_market_data_events().size() == 2U);
    const MarketDataEvent first_level = book.last_market_data_events()[0];
    const MarketDataEvent first_best = book.last_market_data_events()[1];
    CHECK(first_level.kind == MarketDataEvent::Kind::LevelCreated);
    CHECK(first_level.sequence == 1U);
    CHECK(first_best.kind == MarketDataEvent::Kind::BestAskChanged);
    CHECK(first_best.sequence == 2U);

    const AddOrderResult second = book.add_limit_order(2, Side::Sell, 101, 5, 2);
    CHECK(second.status == Status::Accepted);
    CHECK(book.last_market_data_events().size() == 1U);
    const MarketDataEvent second_level = book.last_market_data_events()[0];
    CHECK(second_level.kind == MarketDataEvent::Kind::LevelCreated);
    CHECK(second_level.sequence == 3U);

    const AddOrderResult sweep = book.add_limit_order(3, Side::Buy, 101, 8, 3);
    CHECK(sweep.status == Status::Filled);
    const std::span<const MarketDataEvent> sweep_events = book.last_market_data_events();
    CHECK(sweep_events.size() == 6U);
    constexpr MarketDataEvent::Kind expected[]{
        MarketDataEvent::Kind::Trade,
        MarketDataEvent::Kind::LevelDeleted,
        MarketDataEvent::Kind::BestAskChanged,
        MarketDataEvent::Kind::Trade,
        MarketDataEvent::Kind::LevelQuantityChanged,
        MarketDataEvent::Kind::BestAskChanged,
    };
    for (std::size_t index = 0; index < sweep_events.size(); ++index) {
        CHECK(sweep_events[index].kind == expected[index]);
        CHECK(sweep_events[index].sequence == 4U + index);
    }
    CHECK(sweep_events[2].price == 101);
    CHECK(sweep_events[5].quantity == 2);
    const MarketDataEvent sweep_first = sweep_events[0];

    const SequenceNumber before_reject = book.market_data_sequence();
    CHECK(book.add_limit_order(4, Side::Buy, 200, 1).status == Status::InvalidPrice);
    CHECK(book.market_data_sequence() == before_reject);

    SequenceGapDetector detector(809);
    CHECK(detector.observe(first_level).status == SequenceCheck::First);
    CHECK(detector.observe(first_best).status == SequenceCheck::Contiguous);
    const SequenceCheckResult gap = detector.observe(sweep_first);
    CHECK(gap.status == SequenceCheck::Gap);
    CHECK(gap.expected == 3U);
    CHECK(gap.received == 4U);

    OrderBook constrained(
        venue_config(PriceLevelMode::Dense, 1, SelfTradePolicy::Disabled, 2), 810);
    CHECK(constrained.add_limit_order(1, Side::Sell, 100, 5).status == Status::Accepted);
    const std::uint64_t constrained_before = constrained.state_checksum();
    const AddOrderResult exhausted = constrained.add_limit_order(2, Side::Buy, 100, 5);
    CHECK(exhausted.status == Status::MarketDataLogFull);
    CHECK(exhausted.events.empty());
    CHECK(constrained.last_market_data_events().empty());
    CHECK(constrained.state_checksum() == constrained_before);
    CHECK(constrained.find_order(1) != nullptr);

    const BookConfig per_instrument =
        venue_config(PriceLevelMode::Dense, 1, SelfTradePolicy::Disabled, 8);
    const InstrumentConfig instruments[]{
        InstrumentConfig{8111, per_instrument},
        InstrumentConfig{8112, per_instrument},
    };
    MatchingEngine engine(instruments);
    const AddOrderResult instrument_a =
        engine.add_limit_order(8111, 1, Side::Buy, 100, 1);
    const AddOrderResult instrument_b =
        engine.add_limit_order(8112, 1, Side::Sell, 101, 1);
    CHECK(instrument_a.status == Status::Accepted);
    CHECK(instrument_b.status == Status::Accepted);
    CHECK(engine.last_market_data_events(8111).front().sequence == 1U);
    CHECK(engine.last_market_data_events(8112).front().sequence == 1U);
    CHECK(engine.market_data_sequence(8111) == 2U);
    CHECK(engine.market_data_sequence(8112) == 2U);
}

void test_checksum_dense_sparse_and_snapshot_sequence()
{
    BookConfig dense_config = venue_config(
        PriceLevelMode::Dense, 1, SelfTradePolicy::CancelResting, 64);
    BookConfig sparse_config = dense_config;
    sparse_config.price_level_mode = PriceLevelMode::Sparse;
    OrderBook dense(dense_config, 811);
    OrderBook sparse(sparse_config, 811);
    for (OrderBook* book : {&dense, &sparse}) {
        CHECK(book->add_limit_order(1, Side::Buy, 99, 5, 1, TimeInForce::Gtc, 7, true).status ==
              Status::Accepted);
        CHECK(book->add_limit_order(2, Side::Buy, 98, 7, 2, TimeInForce::Gtc, 8).status ==
              Status::Accepted);
        CHECK(book->add_limit_order(3, Side::Sell, 103, 9, 3, TimeInForce::Gtc, 9).status ==
              Status::Accepted);
        CHECK(book->modify_order(2, 6, 4).status == Status::Accepted);
    }
    CHECK(dense.state_checksum() == sparse.state_checksum());

    std::array<std::byte, kSnapshotTestBufferSize> snapshot{};
    const SnapshotWriteResult write = serialize(dense, snapshot);
    CHECK(write.status == Status::Accepted);
    OrderBook restored(dense_config, 811);
    CHECK(restore(restored, std::span<const std::byte>(snapshot.data(), write.bytes_written)) ==
          Status::Accepted);
    CHECK(restored.state_checksum() == dense.state_checksum());
    CHECK(restored.market_data_sequence() == dense.market_data_sequence());
    const SequenceNumber sequence = restored.market_data_sequence();
    const CancelResult cancel = restored.cancel_order(1, 5);
    CHECK(cancel.status == Status::Cancelled);
    CHECK(!restored.last_market_data_events().empty());
    CHECK(restored.last_market_data_events().front().sequence == sequence + 1U);
}

void test_journal_full_replay_snapshot_tail_and_corruption()
{
    constexpr InstrumentId dense_id = 901;
    constexpr InstrumentId sparse_id = 902;
    BookConfig dense = venue_config(
        PriceLevelMode::Dense, 5, SelfTradePolicy::CancelResting, 128);
    BookConfig sparse = dense;
    sparse.price_level_mode = PriceLevelMode::Sparse;
    const InstrumentConfig configs[]{
        InstrumentConfig{dense_id, dense},
        InstrumentConfig{sparse_id, sparse},
    };
    MatchingEngine original(configs);
    CHECK(original.valid());

    const std::array<VenueCommand, 9> commands{{
        VenueCommand{Command{dense_id, CommandOp::Add, 1, Side::Sell, 100, 10, TimeInForce::Gtc, 1},
                     42,
                     false},
        VenueCommand{Command{dense_id, CommandOp::Add, 2, Side::Buy, 99, 10, TimeInForce::Gtc, 2},
                     7,
                     true},
        VenueCommand{Command{dense_id, CommandOp::Add, 3, Side::Buy, 100, 5, TimeInForce::Gtc, 3},
                     42,
                     false},
        VenueCommand{Command{dense_id, CommandOp::Add, 4, Side::Buy, 101, 3, TimeInForce::Gtc, 4},
                     8,
                     false},
        VenueCommand{Command{sparse_id, CommandOp::Add, 10, Side::Buy, 95, 15, TimeInForce::Gtc, 5},
                     9,
                     false},
        VenueCommand{Command{sparse_id, CommandOp::Modify, 10, Side::Buy, 0, 10, TimeInForce::Gtc, 6},
                     0,
                     false},
        VenueCommand{Command{sparse_id, CommandOp::Replace, 10, Side::Buy, 96, 10, TimeInForce::Gtc, 7},
                     0,
                     true},
        VenueCommand{Command{sparse_id, CommandOp::Market, 20, Side::Sell, 0, 5, TimeInForce::Ioc, 8},
                     11,
                     false},
        VenueCommand{Command{sparse_id, CommandOp::Cancel, 999, Side::Buy, 0, 0, TimeInForce::Gtc, 9},
                     0,
                     false},
    }};

    std::array<std::byte, commands.size() * kJournalRecordLength> journal{};
    std::array<std::uint64_t, commands.size()> checksums{};
    std::array<std::byte, kSnapshotTestBufferSize> midpoint_snapshot{};
    std::size_t midpoint_snapshot_size = 0;
    constexpr std::size_t midpoint = 5;
    for (std::size_t index = 0; index < commands.size(); ++index) {
        JournalWriteResult write = dispatch_and_record(
            original,
            commands[index],
            index + 1U,
            std::span<std::byte>(journal).subspan(index * kJournalRecordLength,
                                                  kJournalRecordLength));
        CHECK(write.status == Status::Accepted);
        CHECK(write.bytes_written == kJournalRecordLength);
        checksums[index] = original.state_checksum();
        if (index + 1U == midpoint) {
            const SnapshotWriteResult snapshot = serialize(original, midpoint_snapshot);
            CHECK(snapshot.status == Status::Accepted);
            midpoint_snapshot_size = snapshot.bytes_written;
        }
    }

    MatchingEngine replayed(configs);
    JournalReplayCursor cursor{};
    for (std::size_t index = 0; index < commands.size(); ++index) {
        CHECK(replay_journal_record(
                  replayed,
                  std::span<const std::byte>(journal).subspan(index * kJournalRecordLength,
                                                              kJournalRecordLength),
                  cursor) == Status::Accepted);
        CHECK(replayed.state_checksum() == checksums[index]);
    }
    CHECK(replayed.state_checksum() == original.state_checksum());
    CHECK(replayed.market_data_sequence(dense_id) == original.market_data_sequence(dense_id));
    CHECK(replayed.market_data_sequence(sparse_id) == original.market_data_sequence(sparse_id));

    MatchingEngine tail(configs);
    CHECK(restore(tail,
                  std::span<const std::byte>(midpoint_snapshot.data(), midpoint_snapshot_size)) ==
          Status::Accepted);
    JournalReplayCursor tail_cursor{midpoint + 1U};
    const JournalReplayResult tail_result = replay_journal(
        tail,
        std::span<const std::byte>(journal).subspan(midpoint * kJournalRecordLength),
        tail_cursor);
    CHECK(tail_result.status == Status::Accepted);
    CHECK(tail_result.records_replayed == commands.size() - midpoint);
    CHECK(tail.state_checksum() == original.state_checksum());

    JournalRecordInfo info{};
    CHECK(decode_journal_record(
              std::span<const std::byte>(journal).first(kJournalRecordLength), info) ==
          Status::Accepted);
    CHECK(info.expected_status == Status::Accepted);
    CHECK(decode_journal_record(
              std::span<const std::byte>(journal).first(kJournalRecordLength - 1U), info) ==
          Status::JournalLengthMismatch);

    std::array<std::byte, kJournalRecordLength> corrupt{};
    std::copy_n(journal.begin(), kJournalRecordLength, corrupt.begin());
    corrupt[64] ^= std::byte{1};
    CHECK(decode_journal_record(corrupt, info) == Status::JournalChecksumMismatch);

    std::copy_n(journal.begin(), kJournalRecordLength, corrupt.begin());
    journal_detail::write_u16(corrupt, 4, kJournalVersion + 1U);
    CHECK(decode_journal_record(corrupt, info) == Status::JournalVersionMismatch);

    std::copy_n(journal.begin(), kJournalRecordLength, corrupt.begin());
    journal_detail::write_u32(corrupt, 20, 1U);
    journal_detail::write_u32(
        corrupt, journal_detail::kCrcOffset, journal_detail::crc32(corrupt));
    CHECK(decode_journal_record(corrupt, info) == Status::JournalInvalidField);
}

} // namespace

int main()
{
    test_command_encode_decode_round_trip();
    test_command_wire_format_exact_size_and_little_endian();
    test_dispatch_fixed_sequence_matches_oracle();
    test_dispatch_rejects_corrupt_and_truncated_buffers();
    test_dispatch_failure_results_have_empty_events_and_preserve_books();
    test_event_log_full_policy();
    test_reference_oracle_models_price_level_quantity_overflow();
    test_price_level_quantity_overflow_rejection_is_atomic();
    test_dispatch_seeded_replay_stream_matches_oracle();
    test_fifo_and_price_priority();
    test_reduce_keeps_priority_and_increase_rejects();
    test_replace_price_change_loses_priority();
    test_replace_crossing_price_executes_before_resting();
    test_replace_reuses_existing_order_slot_when_pool_full();
    test_replace_reuses_existing_order_id_when_id_map_full();
    test_sparse_replace_reuses_freed_level_when_level_storage_full();
    test_replace_rejects_when_residual_level_cannot_store_quantity();
    test_replace_event_log_full_preserves_book();
    test_replace_ioc_and_fok_paths();
    test_modify_reduce_equivalent_to_replace_reduce();
    test_pool_exhaustion_preserves_resting_liquidity();
    test_order_id_map_full_preflight();
    test_order_id_map_capacity_for_saturates();
    test_order_id_map_probe_stats();
    test_order_id_map_backward_shift_deletion();
    test_order_id_map_robin_hood_early_miss_and_full_table_bound();
    test_cancel_best_level_dense_boundaries();
    test_cancel_sparse_final_level_maintains_sorted_slots();
    test_book_and_engine_stats();
    test_invalid_and_unknown_inputs();
    test_invalid_side_direct_api_invalidates_event_spans();
    test_invalid_market_quantity_preserves_requested_and_remaining();
    test_event_single_fill();
    test_event_multi_level_sweep();
    test_event_partial_fill_and_rest();
    test_event_cancel_after_partial();
    test_fok_reject_insufficient_liquidity_preserves_book();
    test_fok_accept_exact_liquidity();
    test_ioc_partial_fill_cancels_remainder();
    test_ioc_no_liquidity_cancels_without_book_mutation();
    test_matching_engine_factory_success_and_api_contract();
    test_matching_engine_factory_reports_precise_initialization_failures();
    test_matching_engine_compatibility_constructor_fails_closed();
    test_matching_engine_two_instruments_are_isolated();
    test_matching_engine_depth_after_adds_cancels_and_matches();
    test_matching_engine_unknown_instrument_rejects_without_side_effects();
    test_book_snapshot_round_trip_random_ops();
    test_snapshot_round_trip_preserves_public_order_state();
    test_book_snapshot_dense_and_sparse_round_trips_at_capacity();
    test_snapshot_restore_then_encoded_command_replay_is_deterministic();
    test_book_snapshot_continue_trading_matches_fresh();
    test_book_snapshot_restore_preserves_event_sequence_numbering();
    test_engine_snapshot_round_trip_multi_instrument();
    test_snapshot_rejects_corrupt_and_truncated_buffers();
    test_snapshot_rejects_duplicate_and_inconsistent_records_atomically();
    test_engine_snapshot_validation_failure_is_atomic();
    test_snapshot_rejects_crossed_book_without_mutating_target();
    test_snapshot_rejects_fifo_sequence_regression_without_mutating_target();
    test_seeded_conformance();
    test_sparse_seeded_conformance();
    test_seeded_multi_instrument_conformance();
    test_lot_size_rules();
    test_post_only_entry_and_replace();
    test_all_stp_policies_dense_and_sparse();
    test_stp_partial_third_party_ioc_fok_and_fifo();
    test_replace_preserves_stp_result_flags();
    test_dispatch_preserves_stp_result_flags();
    test_market_data_ordering_sequences_gap_and_capacity();
    test_checksum_dense_sparse_and_snapshot_sequence();
    test_journal_full_replay_snapshot_tail_and_corruption();
    return 0;
}
