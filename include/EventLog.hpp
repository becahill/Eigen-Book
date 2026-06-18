#pragma once

#include "Types.hpp"

#include <cstdint>
#include <limits>
#include <memory>
#include <span>

namespace eigenbook {

// Fixed-capacity event ring. The last operation span is contiguous and remains
// valid until the next begin_operation() call.
class alignas(64) EventLog final {
public:
    explicit EventLog(const std::uint32_t capacity,
                      const InstrumentId instrument_id = kInvalidInstrumentId)
        : events_(capacity == 0 ? nullptr : std::make_unique<BookEvent[]>(capacity)),
          capacity_(capacity),
          instrument_id_(instrument_id)
    {
    }

    EventLog(const EventLog&) = delete;
    EventLog& operator=(const EventLog&) = delete;
    EventLog(EventLog&&) = delete;
    EventLog& operator=(EventLog&&) = delete;

    void reset(const SequenceNumber next_sequence = 0) noexcept
    {
        write_index_ = 0;
        last_begin_ = 0;
        last_count_ = 0;
        next_sequence_ = next_sequence;
    }

    void begin_operation(std::uint32_t max_event_count) noexcept
    {
        last_begin_ = write_index_;
        last_count_ = 0;

        if (capacity_ == 0) {
            return;
        }

        if (max_event_count > capacity_) {
            max_event_count = capacity_;
        }

        const std::uint32_t tail_capacity = capacity_ - write_index_;
        if (max_event_count > tail_capacity) {
            write_index_ = 0;
        }

        last_begin_ = write_index_;
    }

    void append_order(const BookEvent::Kind kind,
                      const Status status,
                      const OrderId order_id,
                      const Side side,
                      const Price price,
                      const Quantity quantity,
                      const Timestamp timestamp,
                      const TimeInForce time_in_force = TimeInForce::Gtc,
                      const Quantity old_quantity = 0,
                      const Quantity new_quantity = 0) noexcept
    {
        const SequenceNumber sequence = next_sequence();
        BookEvent event{};
        event.kind = kind;
        event.instrument_id = instrument_id_;
        event.status = status;
        event.order_id = order_id;
        event.side = side;
        event.price = price;
        event.quantity = quantity;
        event.old_quantity = old_quantity;
        event.new_quantity = new_quantity;
        event.timestamp = timestamp;
        event.sequence = sequence;
        event.time_in_force = time_in_force;
        append(event);
    }

    void append_trade(const OrderId aggressor_id,
                      const OrderId resting_id,
                      const Side aggressor_side,
                      const Price price,
                      const Quantity quantity,
                      const Timestamp timestamp) noexcept
    {
        const SequenceNumber sequence = next_sequence();
        TradeEvent trade{};
        trade.instrument_id = instrument_id_;
        trade.aggressor_id = aggressor_id;
        trade.resting_id = resting_id;
        trade.aggressor_side = aggressor_side;
        trade.price = price;
        trade.quantity = quantity;
        trade.timestamp = timestamp;
        trade.sequence = sequence;

        BookEvent event{};
        event.kind = BookEvent::Kind::Trade;
        event.instrument_id = instrument_id_;
        event.status = Status::Filled;
        event.order_id = aggressor_id;
        event.side = aggressor_side;
        event.price = price;
        event.quantity = quantity;
        event.timestamp = timestamp;
        event.sequence = sequence;
        event.trade = trade;
        append(event);
    }

    [[nodiscard]] std::span<const BookEvent> last_events() const noexcept
    {
        if (events_ == nullptr || last_count_ == 0) {
            return {};
        }

        return {events_.get() + last_begin_, last_count_};
    }

    [[nodiscard]] std::uint32_t last_count() const noexcept
    {
        return last_count_;
    }

    [[nodiscard]] std::uint32_t capacity() const noexcept
    {
        return capacity_;
    }

    [[nodiscard]] SequenceNumber next_sequence_value() const noexcept
    {
        return next_sequence_;
    }

private:
    std::unique_ptr<BookEvent[]> events_;
    std::uint32_t capacity_{0};
    std::uint32_t write_index_{0};
    std::uint32_t last_begin_{0};
    std::uint32_t last_count_{0};
    SequenceNumber next_sequence_{0};
    InstrumentId instrument_id_{kInvalidInstrumentId};

    [[nodiscard]] SequenceNumber next_event_sequence() noexcept
    {
        if (next_sequence_ != std::numeric_limits<SequenceNumber>::max()) {
            ++next_sequence_;
        }
        return next_sequence_;
    }

    [[nodiscard]] SequenceNumber next_sequence() noexcept
    {
        return next_event_sequence();
    }

    void append(const BookEvent& event) noexcept
    {
        if (events_ == nullptr || last_count_ == capacity_) {
            return;
        }

        events_[write_index_] = event;
        ++last_count_;
        ++write_index_;
        if (write_index_ == capacity_) {
            write_index_ = 0;
        }
    }
};

static_assert(alignof(EventLog) >= 64);

} // namespace eigenbook
