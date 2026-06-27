#pragma once

#include "Types.hpp"

#include <cstdint>
#include <limits>
#include <memory>
#include <span>

namespace eigenbook {

/// Fixed-capacity, per-instrument incremental market-data ring.
///
/// Storage is allocated once during book construction. Each operation reserves
/// enough contiguous capacity before any book mutation.
class alignas(64) MarketDataLog final {
public:
    explicit MarketDataLog(const std::uint32_t capacity,
                           const InstrumentId instrument_id = kInvalidInstrumentId)
        : events_(capacity == 0 ? nullptr : std::make_unique<MarketDataEvent[]>(capacity)),
          capacity_(capacity),
          instrument_id_(instrument_id)
    {
    }

    MarketDataLog(const MarketDataLog&) = delete;
    MarketDataLog& operator=(const MarketDataLog&) = delete;
    MarketDataLog(MarketDataLog&&) = delete;
    MarketDataLog& operator=(MarketDataLog&&) = delete;

    [[nodiscard]] bool enabled() const noexcept
    {
        return capacity_ != 0;
    }

    void reset(const SequenceNumber current_sequence = 0) noexcept
    {
        write_index_ = 0;
        last_begin_ = 0;
        last_count_ = 0;
        current_sequence_ = current_sequence;
    }

    void begin_operation(std::uint32_t max_event_count) noexcept
    {
        last_count_ = 0;
        if (!enabled()) {
            last_begin_ = 0;
            return;
        }

        if (max_event_count > capacity_) {
            max_event_count = capacity_;
        }
        if (max_event_count > capacity_ - write_index_) {
            write_index_ = 0;
        }
        last_begin_ = write_index_;
    }

    [[nodiscard]] bool can_record(const std::uint32_t event_count) const noexcept
    {
        if (!enabled() || event_count == 0) {
            return true;
        }
        return event_count <= capacity_ &&
               event_count <= std::numeric_limits<SequenceNumber>::max() - current_sequence_;
    }

    void append_level(const MarketDataEvent::Kind kind,
                      const Side side,
                      const Price price,
                      const Quantity previous_quantity,
                      const Quantity quantity,
                      const std::uint32_t order_count,
                      const Timestamp timestamp) noexcept
    {
        MarketDataEvent event{};
        event.kind = kind;
        event.side = side;
        event.price = price;
        event.previous_quantity = previous_quantity;
        event.quantity = quantity;
        event.order_count = order_count;
        event.timestamp = timestamp;
        append(event);
    }

    void append_trade(const Side aggressor_side,
                      const Price price,
                      const Quantity quantity,
                      const OrderId aggressor_id,
                      const OrderId resting_id,
                      const Timestamp timestamp) noexcept
    {
        MarketDataEvent event{};
        event.kind = MarketDataEvent::Kind::Trade;
        event.side = aggressor_side;
        event.price = price;
        event.quantity = quantity;
        event.aggressor_id = aggressor_id;
        event.resting_id = resting_id;
        event.trade_quantity = quantity;
        event.timestamp = timestamp;
        append(event);
    }

    void append_best(const Side side,
                     const BestQuote& quote,
                     const Timestamp timestamp) noexcept
    {
        MarketDataEvent event{};
        event.kind =
            side == Side::Buy ? MarketDataEvent::Kind::BestBidChanged : MarketDataEvent::Kind::BestAskChanged;
        event.side = side;
        event.price = quote.valid ? quote.price : 0;
        event.quantity = quote.valid ? quote.quantity : 0;
        event.order_count = quote.valid ? quote.order_count : 0;
        event.timestamp = timestamp;
        append(event);
    }

    [[nodiscard]] std::span<const MarketDataEvent> last_events() const noexcept
    {
        if (!enabled() || last_count_ == 0) {
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

    [[nodiscard]] SequenceNumber current_sequence() const noexcept
    {
        return current_sequence_;
    }

private:
    std::unique_ptr<MarketDataEvent[]> events_;
    std::uint32_t capacity_{0};
    std::uint32_t write_index_{0};
    std::uint32_t last_begin_{0};
    std::uint32_t last_count_{0};
    SequenceNumber current_sequence_{0};
    InstrumentId instrument_id_{kInvalidInstrumentId};

    void append(MarketDataEvent event) noexcept
    {
        if (!enabled() || last_count_ == capacity_ ||
            current_sequence_ == std::numeric_limits<SequenceNumber>::max()) {
            return;
        }

        ++current_sequence_;
        event.instrument_id = instrument_id_;
        event.sequence = current_sequence_;
        events_[write_index_] = event;
        ++last_count_;
        ++write_index_;
        if (write_index_ == capacity_) {
            write_index_ = 0;
        }
    }
};

static_assert(alignof(MarketDataLog) >= 64);

enum class SequenceCheck : std::uint8_t {
    First,
    Contiguous,
    Gap,
    DuplicateOrOld,
    WrongInstrument,
};

struct SequenceCheckResult final {
    SequenceCheck status{SequenceCheck::First};
    SequenceNumber expected{0};
    SequenceNumber received{0};
};

/// Allocation-free consumer-side detector for one instrument stream.
class SequenceGapDetector final {
public:
    explicit SequenceGapDetector(const InstrumentId instrument_id,
                                 const SequenceNumber snapshot_sequence = 0) noexcept
        : instrument_id_(instrument_id), last_sequence_(snapshot_sequence), initialized_(snapshot_sequence != 0)
    {
    }

    [[nodiscard]] SequenceCheckResult observe(const MarketDataEvent& event) noexcept
    {
        if (event.instrument_id != instrument_id_) {
            return {SequenceCheck::WrongInstrument, next_expected(), event.sequence};
        }

        if (!initialized_) {
            initialized_ = true;
            last_sequence_ = event.sequence;
            return {SequenceCheck::First, event.sequence, event.sequence};
        }

        const SequenceNumber expected = next_expected();
        const SequenceCheck status =
            event.sequence == expected
                ? SequenceCheck::Contiguous
                : (event.sequence <= last_sequence_ ? SequenceCheck::DuplicateOrOld : SequenceCheck::Gap);
        if (event.sequence > last_sequence_) {
            last_sequence_ = event.sequence;
        }
        return {status, expected, event.sequence};
    }

    [[nodiscard]] SequenceNumber last_sequence() const noexcept
    {
        return last_sequence_;
    }

    void reset(const SequenceNumber snapshot_sequence = 0) noexcept
    {
        last_sequence_ = snapshot_sequence;
        initialized_ = snapshot_sequence != 0;
    }

private:
    InstrumentId instrument_id_{kInvalidInstrumentId};
    SequenceNumber last_sequence_{0};
    bool initialized_{false};

    [[nodiscard]] SequenceNumber next_expected() const noexcept
    {
        return last_sequence_ == std::numeric_limits<SequenceNumber>::max() ? last_sequence_
                                                                           : last_sequence_ + 1U;
    }
};

} // namespace eigenbook
