#include "Command.hpp"
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
                       ? Status::InternalError
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

constexpr std::size_t kSnapshotTestBufferSize = 65'536;

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

    const ReplaceResult actual_result = actual.replace_order(1, 101, 6, 3, TimeInForce::Gtc);
    const ReplaceResult expected_result = expected.replace_order(1, 101, 6, 3, TimeInForce::Gtc);
    check_replace_result(actual_result, expected_result, "replace_residual_quantity_capacity");
    check_books_equal(actual, expected);

    CHECK(actual_result.status == Status::InternalError);
    CHECK(actual_result.events_emitted == 1);
    check_order_event_fields(
        actual_result.events[0], BookEvent::Kind::OrderRejected, Status::InternalError, 1, Side::Buy, 101, 6, 3);
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
    CHECK(stats.tombstones == 1);
    CHECK(histogram_total(stats) == 5);
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
    CHECK(engine.modify(instrument_b, 2, 12, 3).status == Status::Accepted);
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

} // namespace

int main()
{
    test_command_encode_decode_round_trip();
    test_command_wire_format_exact_size_and_little_endian();
    test_dispatch_fixed_sequence_matches_oracle();
    test_dispatch_rejects_corrupt_and_truncated_buffers();
    test_dispatch_failure_results_have_empty_events_and_preserve_books();
    test_event_log_full_policy();
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
    test_book_and_engine_stats();
    test_invalid_and_unknown_inputs();
    test_event_single_fill();
    test_event_multi_level_sweep();
    test_event_partial_fill_and_rest();
    test_event_cancel_after_partial();
    test_fok_reject_insufficient_liquidity_preserves_book();
    test_fok_accept_exact_liquidity();
    test_ioc_partial_fill_cancels_remainder();
    test_ioc_no_liquidity_cancels_without_book_mutation();
    test_matching_engine_two_instruments_are_isolated();
    test_matching_engine_depth_after_adds_cancels_and_matches();
    test_matching_engine_unknown_instrument_rejects_without_side_effects();
    test_book_snapshot_round_trip_random_ops();
    test_book_snapshot_continue_trading_matches_fresh();
    test_book_snapshot_restore_preserves_event_sequence_numbering();
    test_engine_snapshot_round_trip_multi_instrument();
    test_snapshot_rejects_corrupt_and_truncated_buffers();
    test_snapshot_rejects_crossed_book_without_mutating_target();
    test_snapshot_rejects_fifo_sequence_regression_without_mutating_target();
    test_seeded_conformance();
    test_sparse_seeded_conformance();
    test_seeded_multi_instrument_conformance();
    return 0;
}
