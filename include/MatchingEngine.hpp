#pragma once

#include "Command.hpp"
#include "OrderBook.hpp"
#include "Types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <utility>

namespace eigenbook {

enum class MatchingEngineInitError : std::uint8_t {
    None,
    InstrumentCapacityExceeded,
    InvalidInstrumentId,
    InvalidBookConfiguration,
    DuplicateInstrumentId,
    AllocationFailure,
    InternalInsertionFailure,
};

inline constexpr std::size_t kInvalidInstrumentConfigIndex =
    std::numeric_limits<std::size_t>::max();

[[nodiscard]] constexpr const char* matching_engine_init_error_name(
    const MatchingEngineInitError error) noexcept
{
    switch (error) {
    case MatchingEngineInitError::None:
        return "none";
    case MatchingEngineInitError::InstrumentCapacityExceeded:
        return "instrument capacity exceeded";
    case MatchingEngineInitError::InvalidInstrumentId:
        return "invalid instrument id";
    case MatchingEngineInitError::InvalidBookConfiguration:
        return "invalid order-book configuration";
    case MatchingEngineInitError::DuplicateInstrumentId:
        return "duplicate instrument id";
    case MatchingEngineInitError::AllocationFailure:
        return "allocation failure";
    case MatchingEngineInitError::InternalInsertionFailure:
        return "internal instrument insertion failure";
    }

    return "unknown matching-engine initialization error";
}

struct MatchingEngineCreateResult;

class alignas(64) MatchingEngine final {
    friend struct SnapshotAccess;

public:
    /// Construct only a fully initialized engine.
    ///
    /// Failed results contain no engine and identify the first invalid
    /// configuration entry. The `max_instruments` overload permits spare
    /// preallocated instrument capacity.
    template <std::size_t N>
    [[nodiscard]] static MatchingEngineCreateResult create(
        const InstrumentConfig (&configs)[N]) noexcept;

    template <std::size_t N>
    [[nodiscard]] static MatchingEngineCreateResult create(
        std::uint32_t max_instruments,
        const InstrumentConfig (&configs)[N]) noexcept;

    [[nodiscard]] static MatchingEngineCreateResult create(
        std::span<const InstrumentConfig> configs) noexcept;

    [[nodiscard]] static MatchingEngineCreateResult create(
        std::uint32_t max_instruments,
        std::span<const InstrumentConfig> configs) noexcept;

    /// Compatibility constructor. Prefer `create()` for new code.
    ///
    /// On failure this object retains only the initialization diagnostic. All
    /// instrument and lookup state is destroyed, so commands fail closed.
    template <std::size_t N>
    explicit MatchingEngine(const InstrumentConfig (&configs)[N])
        : MatchingEngine(span_size_to_u32(N), std::span<const InstrumentConfig, N>(configs))
    {
    }

    template <std::size_t N>
    MatchingEngine(const std::uint32_t max_instruments, const InstrumentConfig (&configs)[N])
        : MatchingEngine(max_instruments, std::span<const InstrumentConfig, N>(configs))
    {
    }

    explicit MatchingEngine(const std::span<const InstrumentConfig> configs)
        : MatchingEngine(span_size_to_u32(configs.size()), configs)
    {
    }

    MatchingEngine(const std::uint32_t max_instruments,
                   const std::span<const InstrumentConfig> configs)
        : max_instruments_(max_instruments),
          lookup_capacity_(lookup_capacity_for(max_instruments))
    {
        const InitializationDiagnostic validation =
            validate_configurations(max_instruments_, configs);
        if (validation.error != MatchingEngineInitError::None) {
            fail_initialization(validation.error, validation.config_index);
            return;
        }

        std::size_t initializing_index = kInvalidInstrumentConfigIndex;
        try {
            instruments_ =
                max_instruments_ == 0 ? nullptr : std::make_unique<InstrumentState[]>(max_instruments_);
            lookup_ =
                lookup_capacity_ == 0 ? nullptr : std::make_unique<LookupSlot[]>(lookup_capacity_);

            for (std::size_t index = 0; index < configs.size(); ++index) {
                initializing_index = index;
                const MatchingEngineInitError error = insert_instrument(configs[index]);
                if (error != MatchingEngineInitError::None) {
                    fail_initialization(error, index);
                    return;
                }
            }
        } catch (const std::bad_alloc&) {
            fail_initialization(
                MatchingEngineInitError::AllocationFailure, initializing_index);
        }
    }

    MatchingEngine(const MatchingEngine&) = delete;
    MatchingEngine& operator=(const MatchingEngine&) = delete;
    MatchingEngine(MatchingEngine&&) = delete;
    MatchingEngine& operator=(MatchingEngine&&) = delete;

    [[nodiscard]] AddOrderResult add_limit_order(const InstrumentId instrument_id,
                                                 const OrderId id,
                                                 const Side side,
                                                 const Price price,
                                                 const Quantity quantity,
                                                 const Timestamp timestamp = 0,
                                                 const TimeInForce time_in_force = TimeInForce::Gtc,
                                                 const ParticipantId participant_id =
                                                     kAnonymousParticipantId,
                                                 const bool post_only = false) noexcept
    {
        OrderBook* book = find_book(instrument_id);
        if (book == nullptr) {
            return unknown_result<AddOrderResult>();
        }

        AddOrderResult result =
            book->add_limit_order(id, side, price, quantity, timestamp, time_in_force, participant_id, post_only);
        record_event_high_water(result.events_emitted);
        return result;
    }

    [[nodiscard]] CancelResult cancel(const InstrumentId instrument_id,
                                      const OrderId id,
                                      const Timestamp timestamp = 0) noexcept
    {
        OrderBook* book = find_book(instrument_id);
        if (book == nullptr) {
            return unknown_result<CancelResult>();
        }

        CancelResult result = book->cancel_order(id, timestamp);
        record_event_high_water(result.events_emitted);
        return result;
    }

    [[nodiscard]] CancelResult cancel_order(const InstrumentId instrument_id,
                                            const OrderId id,
                                            const Timestamp timestamp = 0) noexcept
    {
        return cancel(instrument_id, id, timestamp);
    }

    [[nodiscard]] ModifyResult modify(const InstrumentId instrument_id,
                                      const OrderId id,
                                      const Quantity new_quantity,
                                      const Timestamp timestamp = 0) noexcept
    {
        OrderBook* book = find_book(instrument_id);
        if (book == nullptr) {
            return unknown_result<ModifyResult>();
        }

        ModifyResult result = book->modify_order(id, new_quantity, timestamp);
        record_event_high_water(result.events_emitted);
        return result;
    }

    [[nodiscard]] ModifyResult modify_order(const InstrumentId instrument_id,
                                            const OrderId id,
                                            const Quantity new_quantity,
                                            const Timestamp timestamp = 0) noexcept
    {
        return modify(instrument_id, id, new_quantity, timestamp);
    }

    [[nodiscard]] ReplaceResult replace(const InstrumentId instrument_id,
                                        const OrderId id,
                                        const Price new_price,
                                        const Quantity new_quantity,
                                        const TimeInForce time_in_force = TimeInForce::Gtc,
                                        const bool post_only = false) noexcept
    {
        return replace(instrument_id, id, new_price, new_quantity, 0, time_in_force, post_only);
    }

    [[nodiscard]] ReplaceResult replace(const InstrumentId instrument_id,
                                        const OrderId id,
                                        const Price new_price,
                                        const Quantity new_quantity,
                                        const Timestamp timestamp,
                                        const TimeInForce time_in_force,
                                        const bool post_only = false) noexcept
    {
        OrderBook* book = find_book(instrument_id);
        if (book == nullptr) {
            return unknown_result<ReplaceResult>();
        }

        ReplaceResult result =
            book->replace_order(id, new_price, new_quantity, timestamp, time_in_force, post_only);
        record_event_high_water(result.events_emitted);
        return result;
    }

    /// Route a replace request to one instrument.
    ///
    /// Unknown instruments return `Status::UnknownInstrument` with an empty
    /// event span and do not mutate any book.
    [[nodiscard]] ReplaceResult replace_order(const InstrumentId instrument_id,
                                              const OrderId id,
                                              const Price new_price,
                                              const Quantity new_quantity,
                                              const TimeInForce time_in_force = TimeInForce::Gtc,
                                              const bool post_only = false) noexcept
    {
        return replace(instrument_id, id, new_price, new_quantity, time_in_force, post_only);
    }

    /// Route a timestamped replace request to one instrument.
    [[nodiscard]] ReplaceResult replace_order(const InstrumentId instrument_id,
                                              const OrderId id,
                                              const Price new_price,
                                              const Quantity new_quantity,
                                              const Timestamp timestamp,
                                              const TimeInForce time_in_force,
                                              const bool post_only = false) noexcept
    {
        return replace(instrument_id, id, new_price, new_quantity, timestamp, time_in_force, post_only);
    }

    [[nodiscard]] MatchResult match_market_order(const InstrumentId instrument_id,
                                                 const Side aggressor_side,
                                                 const Quantity quantity,
                                                 const OrderId aggressor_id = kInvalidOrderId,
                                                 const Timestamp timestamp = 0,
                                                 const ParticipantId participant_id =
                                                     kAnonymousParticipantId) noexcept
    {
        OrderBook* book = find_book(instrument_id);
        if (book == nullptr) {
            return unknown_result<MatchResult>();
        }

        MatchResult result =
            book->match_market_order(aggressor_side, quantity, aggressor_id, timestamp, participant_id);
        record_event_high_water(result.events_emitted);
        return result;
    }

    /// Decode and dispatch one fixed-size command record.
    ///
    /// Decode failures increment dispatch/decode statistics, return an empty
    /// event span, and do not mutate any book. Extra bytes after
    /// `kCommandWireSize` are ignored by `decode`.
    [[nodiscard]] DispatchResult dispatch(std::span<const std::byte> buffer) noexcept
    {
        Command command{};
        const Status status = decode(buffer, command);
        if (status != Status::Accepted) {
            ++dispatch_count_;
            ++decode_errors_;
            return finish_dispatch(make_dispatch_result(status));
        }

        return dispatch(command);
    }

    /// Dispatch one validated command object to its target instrument.
    ///
    /// Invalid command enums and unknown instruments return explicit statuses
    /// with empty event spans. Unknown instruments do not mutate any book.
    [[nodiscard]] DispatchResult dispatch(const Command& command) noexcept
    {
        return dispatch(VenueCommand{command});
    }

    /// Dispatch a venue-aware command. Rejected commands do not consume
    /// order/FIFO or market-data sequence numbers.
    [[nodiscard]] DispatchResult dispatch(const VenueCommand& venue_command) noexcept
    {
        ++dispatch_count_;
        const Command& command = venue_command.command;
        if (!valid_command_op(command.op)) {
            return finish_dispatch(make_dispatch_result(Status::InvalidCommand));
        }

        count_dispatch_op(command.op);
        if (!valid_command(venue_command)) {
            return finish_dispatch(make_dispatch_result(Status::InvalidCommand));
        }

        switch (command.op) {
        case CommandOp::Add:
            return finish_dispatch(with_market_data(
                command.instrument_id,
                to_dispatch_result(add_limit_order(command.instrument_id,
                                                   command.order_id,
                                                   command.side,
                                                   command.price,
                                                   command.quantity,
                                                   command.timestamp,
                                                   command.time_in_force,
                                                   venue_command.participant_id,
                                                   venue_command.post_only))));
        case CommandOp::Cancel:
            return finish_dispatch(with_market_data(
                command.instrument_id,
                to_dispatch_result(cancel(command.instrument_id, command.order_id, command.timestamp))));
        case CommandOp::Modify:
            return finish_dispatch(with_market_data(
                command.instrument_id,
                to_dispatch_result(
                    modify(command.instrument_id, command.order_id, command.quantity, command.timestamp))));
        case CommandOp::Replace:
            return finish_dispatch(with_market_data(
                command.instrument_id,
                to_dispatch_result(replace(command.instrument_id,
                                           command.order_id,
                                           command.price,
                                           command.quantity,
                                           command.timestamp,
                                           command.time_in_force,
                                           venue_command.post_only))));
        case CommandOp::Market:
            return finish_dispatch(with_market_data(
                command.instrument_id,
                to_dispatch_result(match_market_order(command.instrument_id,
                                                      command.side,
                                                      command.quantity,
                                                      command.order_id,
                                                      command.timestamp,
                                                      venue_command.participant_id))));
        }

        return finish_dispatch(make_dispatch_result(Status::InvalidCommand));
    }

    [[nodiscard]] std::uint32_t depth(const InstrumentId instrument_id,
                                      const Side side,
                                      const std::uint32_t max_levels,
                                      DepthLevel* const out_buffer) const noexcept
    {
        const OrderBook* book = find_book(instrument_id);
        if (book == nullptr) {
            return 0;
        }

        return book->depth(side, max_levels, out_buffer);
    }

    [[nodiscard]] TopOfBook top_of_book(const InstrumentId instrument_id) const noexcept
    {
        const OrderBook* book = find_book(instrument_id);
        if (book == nullptr) {
            return TopOfBook{Status::UnknownInstrument, {}, {}};
        }

        return book->top_of_book();
    }

    [[nodiscard]] BestQuote best_bid(const InstrumentId instrument_id) const noexcept
    {
        const OrderBook* book = find_book(instrument_id);
        return book == nullptr ? BestQuote{} : book->best_bid();
    }

    [[nodiscard]] BestQuote best_ask(const InstrumentId instrument_id) const noexcept
    {
        const OrderBook* book = find_book(instrument_id);
        return book == nullptr ? BestQuote{} : book->best_ask();
    }

    [[nodiscard]] Quantity depth_at_price(const InstrumentId instrument_id,
                                          const Side side,
                                          const Price price) const noexcept
    {
        const OrderBook* book = find_book(instrument_id);
        return book == nullptr ? 0 : book->depth_at_price(side, price);
    }

    [[nodiscard]] std::uint32_t order_count_at_price(const InstrumentId instrument_id,
                                                     const Side side,
                                                     const Price price) const noexcept
    {
        const OrderBook* book = find_book(instrument_id);
        return book == nullptr ? 0 : book->order_count_at_price(side, price);
    }

    [[nodiscard]] const Order* find_order(const InstrumentId instrument_id,
                                          const OrderId id) const noexcept
    {
        const OrderBook* book = find_book(instrument_id);
        return book == nullptr ? nullptr : book->find_order(id);
    }

    [[nodiscard]] std::uint32_t live_order_count(const InstrumentId instrument_id) const noexcept
    {
        const OrderBook* book = find_book(instrument_id);
        return book == nullptr ? 0 : book->live_order_count();
    }

    [[nodiscard]] std::span<const BookEvent> last_events(const InstrumentId instrument_id) const noexcept
    {
        const OrderBook* book = find_book(instrument_id);
        return book == nullptr ? std::span<const BookEvent>{} : book->last_events();
    }

    [[nodiscard]] std::span<const MarketDataEvent> last_market_data_events(
        const InstrumentId instrument_id) const noexcept
    {
        const OrderBook* book = find_book(instrument_id);
        return book == nullptr ? std::span<const MarketDataEvent>{}
                               : book->last_market_data_events();
    }

    [[nodiscard]] SequenceNumber market_data_sequence(const InstrumentId instrument_id) const noexcept
    {
        const OrderBook* book = find_book(instrument_id);
        return book == nullptr ? 0 : book->market_data_sequence();
    }

    [[nodiscard]] const OrderBook* order_book(const InstrumentId instrument_id) const noexcept
    {
        return find_book(instrument_id);
    }

    [[nodiscard]] std::uint32_t max_instruments() const noexcept
    {
        return max_instruments_;
    }

    [[nodiscard]] std::uint32_t instrument_count() const noexcept
    {
        return instrument_count_;
    }

    /// Canonical checksum of all instruments ordered by instrument id.
    [[nodiscard]] std::uint64_t state_checksum() const noexcept
    {
        constexpr std::uint64_t offset_basis = 14695981039346656037ULL;
        constexpr std::uint64_t prime = 1099511628211ULL;
        std::uint64_t hash = offset_basis;
        const auto append_u64 = [&hash](const std::uint64_t value) noexcept {
            for (std::uint32_t byte = 0; byte < 8U; ++byte) {
                hash ^= static_cast<std::uint8_t>((value >> (byte * 8U)) & 0xffU);
                hash *= prime;
            }
        };
        append_u64(instrument_count_);

        InstrumentId previous = kInvalidInstrumentId;
        for (std::uint32_t rank = 0; rank < instrument_count_; ++rank) {
            InstrumentId selected = std::numeric_limits<InstrumentId>::max();
            const OrderBook* selected_book = nullptr;
            for (std::uint32_t index = 0; index < instrument_count_; ++index) {
                const InstrumentId candidate = instruments_[index].config.instrument_id;
                if (candidate > previous && candidate <= selected) {
                    selected = candidate;
                    selected_book = instruments_[index].book.get();
                }
            }
            if (selected_book == nullptr) {
                break;
            }
            append_u64(selected);
            append_u64(selected_book->state_checksum());
            previous = selected;
        }
        return hash;
    }

    [[nodiscard]] bool valid() const noexcept
    {
        return valid_;
    }

    [[nodiscard]] MatchingEngineInitError initialization_error() const noexcept
    {
        return initialization_error_;
    }

    [[nodiscard]] std::size_t failed_config_index() const noexcept
    {
        return failed_config_index_;
    }

    [[nodiscard]] OrderBookStats stats(const InstrumentId instrument_id) const noexcept
    {
        const OrderBook* book = find_book(instrument_id);
        return book == nullptr ? OrderBookStats{} : book->stats();
    }

    [[nodiscard]] MatchingEngineStats stats() const noexcept
    {
        MatchingEngineStats result{};
        result.instrument_count = instrument_count_;
        result.max_instruments = max_instruments_;

        for (std::uint32_t index = 0; index < instrument_count_; ++index) {
            if (instruments_[index].book == nullptr) {
                continue;
            }

            const OrderBookStats book_stats = instruments_[index].book->stats();
            result.total_live_order_count += book_stats.live_order_count;
            result.total_order_capacity += book_stats.order_capacity;
            result.aggregate_order_id_map.size += book_stats.order_id_map.size;
            result.aggregate_order_id_map.capacity += book_stats.order_id_map.capacity;
            result.aggregate_order_id_map.tombstones += book_stats.order_id_map.tombstones;
            result.aggregate_order_id_map.last_probe_count = book_stats.order_id_map.last_probe_count;
            for (std::size_t bucket = 0; bucket < kProbeHistogramBucketCount; ++bucket) {
                result.aggregate_order_id_map.probe_histogram[bucket] +=
                    book_stats.order_id_map.probe_histogram[bucket];
            }
        }

        result.order_pool_utilization = utilization(result.total_live_order_count, result.total_order_capacity);
        result.order_id_map_utilization =
            utilization(result.aggregate_order_id_map.size, result.aggregate_order_id_map.capacity);
        result.dispatch_count = dispatch_count_;
        result.adds = adds_;
        result.cancels = cancels_;
        result.modifies = modifies_;
        result.replaces = replaces_;
        result.market_matches = market_matches_;
        result.rejects = rejects_;
        result.rejects_by_status = rejects_by_status_;
        result.event_log_high_water_mark = event_log_high_water_mark_;
        result.decode_errors = decode_errors_;
        return result;
    }

private:
    struct InitializationDiagnostic final {
        MatchingEngineInitError error{MatchingEngineInitError::None};
        std::size_t config_index{kInvalidInstrumentConfigIndex};
    };

    struct InstrumentState final {
        InstrumentConfig config{};
        std::unique_ptr<OrderBook> book{};
    };

    struct LookupSlot final {
        InstrumentId instrument_id{kInvalidInstrumentId};
        std::uint32_t instrument_index{kInvalidIndex};
        bool occupied{false};
    };

    std::uint32_t max_instruments_{0};
    std::unique_ptr<InstrumentState[]> instruments_;
    std::uint32_t instrument_count_{0};
    std::uint32_t lookup_capacity_{0};
    std::unique_ptr<LookupSlot[]> lookup_;
    bool valid_{true};
    std::uint64_t dispatch_count_{0};
    std::uint64_t adds_{0};
    std::uint64_t cancels_{0};
    std::uint64_t modifies_{0};
    std::uint64_t replaces_{0};
    std::uint64_t market_matches_{0};
    std::uint64_t rejects_{0};
    std::array<std::uint64_t, kStatusCount> rejects_by_status_{};
    std::uint32_t event_log_high_water_mark_{0};
    std::uint64_t decode_errors_{0};
    MatchingEngineInitError initialization_error_{MatchingEngineInitError::None};
    std::size_t failed_config_index_{kInvalidInstrumentConfigIndex};

    [[nodiscard]] static DispatchResult make_dispatch_result(const Status status) noexcept
    {
        DispatchResult result{};
        result.status = status;
        return result;
    }

    [[nodiscard]] static DispatchResult to_dispatch_result(const AddOrderResult& source) noexcept
    {
        DispatchResult result{};
        result.status = source.status;
        result.accepted_quantity = source.accepted_quantity;
        result.executed_quantity = source.executed_quantity;
        result.resting_quantity = source.resting_quantity;
        result.fills = source.fills;
        result.has_last_price = source.has_last_price;
        result.last_price = source.last_price;
        result.events_emitted = source.events_emitted;
        result.events = source.events;
        return result;
    }

    [[nodiscard]] static DispatchResult to_dispatch_result(const CancelResult& source) noexcept
    {
        DispatchResult result{};
        result.status = source.status;
        result.canceled_quantity = source.canceled_quantity;
        result.events_emitted = source.events_emitted;
        result.events = source.events;
        return result;
    }

    [[nodiscard]] static DispatchResult to_dispatch_result(const ModifyResult& source) noexcept
    {
        DispatchResult result{};
        result.status = source.status;
        result.old_quantity = source.old_quantity;
        result.new_quantity = source.new_quantity;
        result.events_emitted = source.events_emitted;
        result.events = source.events;
        return result;
    }

    [[nodiscard]] static DispatchResult to_dispatch_result(const ReplaceResult& source) noexcept
    {
        DispatchResult result{};
        result.status = source.status;
        result.old_price = source.old_price;
        result.new_price = source.new_price;
        result.old_quantity = source.old_quantity;
        result.new_quantity = source.new_quantity;
        result.executed_quantity = source.executed_quantity;
        result.resting_quantity = source.resting_quantity;
        result.fills = source.fills;
        result.has_last_price = source.has_last_price;
        result.last_price = source.last_price;
        result.events_emitted = source.events_emitted;
        result.events = source.events;
        return result;
    }

    [[nodiscard]] static DispatchResult to_dispatch_result(const MatchResult& source) noexcept
    {
        DispatchResult result{};
        result.status = source.status;
        result.requested_quantity = source.requested_quantity;
        result.executed_quantity = source.executed_quantity;
        result.remaining_quantity = source.remaining_quantity;
        result.fills = source.fills;
        result.has_last_price = source.has_last_price;
        result.last_price = source.last_price;
        result.events_emitted = source.events_emitted;
        result.events = source.events;
        result.aggressor_cancelled_by_stp = source.aggressor_cancelled_by_stp;
        result.resting_orders_cancelled_by_stp = source.resting_orders_cancelled_by_stp;
        return result;
    }

    [[nodiscard]] DispatchResult with_market_data(const InstrumentId instrument_id,
                                                  DispatchResult result) const noexcept
    {
        const OrderBook* const book = find_book(instrument_id);
        if (book != nullptr) {
            result.market_data_events = book->last_market_data_events();
            result.market_data_events_emitted =
                static_cast<std::uint32_t>(result.market_data_events.size());
        }
        return result;
    }

    [[nodiscard]] DispatchResult finish_dispatch(DispatchResult result) noexcept
    {
        record_event_high_water(result.events_emitted);

        if (reject_status(result.status)) {
            ++rejects_;
            const std::size_t index = static_cast<std::size_t>(result.status);
            if (index < rejects_by_status_.size()) {
                ++rejects_by_status_[index];
            }
        }

        return result;
    }

    void record_event_high_water(const std::uint32_t events_emitted) noexcept
    {
        if (events_emitted > event_log_high_water_mark_) {
            event_log_high_water_mark_ = events_emitted;
        }
    }

    void count_dispatch_op(const CommandOp op) noexcept
    {
        switch (op) {
        case CommandOp::Add:
            ++adds_;
            break;
        case CommandOp::Cancel:
            ++cancels_;
            break;
        case CommandOp::Modify:
            ++modifies_;
            break;
        case CommandOp::Replace:
            ++replaces_;
            break;
        case CommandOp::Market:
            ++market_matches_;
            break;
        }
    }

    [[nodiscard]] static bool reject_status(const Status status) noexcept
    {
        switch (status) {
        case Status::Accepted:
        case Status::Cancelled:
        case Status::Filled:
        case Status::PartiallyFilled:
        case Status::NoLiquidity:
            return false;
        case Status::Rejected:
        case Status::InvalidOrderId:
        case Status::UnknownOrderId:
        case Status::UnknownInstrument:
        case Status::InvalidQuantity:
        case Status::InvalidPrice:
        case Status::DuplicateOrderId:
        case Status::PoolExhausted:
        case Status::OrderIdMapFull:
        case Status::QuantityIncreaseRejected:
        case Status::InvalidConfiguration:
        case Status::InternalError:
        case Status::FokRejected:
        case Status::BufferTooSmall:
        case Status::SnapshotFormatMismatch:
        case Status::SnapshotVersionMismatch:
        case Status::SnapshotConfigurationMismatch:
        case Status::SnapshotCapacityExceeded:
        case Status::InvalidCommand:
        case Status::EventLogFull:
        case Status::LotSizeViolation:
        case Status::PostOnlyWouldCross:
        case Status::InvalidPostOnlyTimeInForce:
        case Status::SelfTradePrevented:
        case Status::MarketDataLogFull:
        case Status::JournalFormatMismatch:
        case Status::JournalVersionMismatch:
        case Status::JournalLengthMismatch:
        case Status::JournalChecksumMismatch:
        case Status::JournalInvalidField:
        case Status::ReplayDiverged:
            return true;
        }

        return true;
    }

    [[nodiscard]] OrderBook* find_book(const InstrumentId instrument_id) noexcept
    {
        return const_cast<OrderBook*>(static_cast<const MatchingEngine&>(*this).find_book(instrument_id));
    }

    [[nodiscard]] const OrderBook* find_book(const InstrumentId instrument_id) const noexcept
    {
        const std::uint32_t index = lookup_index(instrument_id);
        if (index == kInvalidIndex) {
            return nullptr;
        }

        return instruments_[index].book.get();
    }

    [[nodiscard]] static InitializationDiagnostic validate_configurations(
        const std::uint32_t max_instruments,
        const std::span<const InstrumentConfig> configs) noexcept
    {
        if (configs.size() > static_cast<std::size_t>(max_instruments)) {
            return InitializationDiagnostic{
                MatchingEngineInitError::InstrumentCapacityExceeded,
                static_cast<std::size_t>(max_instruments),
            };
        }

        for (std::size_t index = 0; index < configs.size(); ++index) {
            const InstrumentConfig& config = configs[index];
            if (config.instrument_id == kInvalidInstrumentId) {
                return InitializationDiagnostic{
                    MatchingEngineInitError::InvalidInstrumentId,
                    index,
                };
            }
            if (!config.book_config.valid()) {
                return InitializationDiagnostic{
                    MatchingEngineInitError::InvalidBookConfiguration,
                    index,
                };
            }
            if (config.self_trade_policy > SelfTradePolicy::CancelBoth) {
                return InitializationDiagnostic{
                    MatchingEngineInitError::InvalidBookConfiguration,
                    index,
                };
            }

            for (std::size_t prior = 0; prior < index; ++prior) {
                if (configs[prior].instrument_id == config.instrument_id) {
                    return InitializationDiagnostic{
                        MatchingEngineInitError::DuplicateInstrumentId,
                        index,
                    };
                }
            }
        }

        return {};
    }

    void fail_initialization(const MatchingEngineInitError error,
                             const std::size_t config_index) noexcept
    {
        instruments_.reset();
        lookup_.reset();
        instrument_count_ = 0;
        lookup_capacity_ = 0;
        valid_ = false;
        initialization_error_ = error;
        failed_config_index_ = config_index;
    }

    [[nodiscard]] MatchingEngineInitError insert_instrument(
        const InstrumentConfig& config)
    {
        if (instrument_count_ >= max_instruments_) {
            return MatchingEngineInitError::InstrumentCapacityExceeded;
        }
        if (config.instrument_id == kInvalidInstrumentId) {
            return MatchingEngineInitError::InvalidInstrumentId;
        }
        if (!config.book_config.valid()) {
            return MatchingEngineInitError::InvalidBookConfiguration;
        }

        bool duplicate = false;
        const std::uint32_t slot = lookup_insert_slot(config.instrument_id, duplicate);
        if (duplicate) {
            return MatchingEngineInitError::DuplicateInstrumentId;
        }
        if (slot == kInvalidIndex) {
            return MatchingEngineInitError::InternalInsertionFailure;
        }

        InstrumentConfig stored = config;
        if (stored.tick_size == 0) {
            stored.tick_size = stored.book_config.tick_size;
        }
        if (stored.lot_size != 0) {
            stored.book_config.lot_size = stored.lot_size;
        } else {
            stored.lot_size = stored.book_config.lot_size;
        }
        if (stored.self_trade_policy != SelfTradePolicy::Disabled ||
            stored.book_config.self_trade_policy == SelfTradePolicy::Disabled) {
            stored.book_config.self_trade_policy = stored.self_trade_policy;
        } else {
            stored.self_trade_policy = stored.book_config.self_trade_policy;
        }
        if (stored.market_data_capacity != 0) {
            stored.book_config.market_data_capacity = stored.market_data_capacity;
        } else {
            stored.market_data_capacity = stored.book_config.market_data_capacity;
        }

        const std::uint32_t index = instrument_count_;
        instruments_[index].config = stored;
        instruments_[index].book = std::make_unique<OrderBook>(stored.book_config, stored.instrument_id);

        lookup_[slot].instrument_id = stored.instrument_id;
        lookup_[slot].instrument_index = index;
        lookup_[slot].occupied = true;
        ++instrument_count_;
        return MatchingEngineInitError::None;
    }

    [[nodiscard]] std::uint32_t lookup_index(const InstrumentId instrument_id) const noexcept
    {
        if (lookup_capacity_ == 0 || instrument_id == kInvalidInstrumentId) {
            return kInvalidIndex;
        }

        const std::uint32_t mask = lookup_capacity_ - 1U;
        std::uint32_t slot = hash(instrument_id) & mask;
        for (std::uint32_t probe = 0; probe < lookup_capacity_; ++probe) {
            const LookupSlot& entry = lookup_[slot];
            if (!entry.occupied) {
                return kInvalidIndex;
            }
            if (entry.instrument_id == instrument_id) {
                return entry.instrument_index;
            }
            slot = (slot + 1U) & mask;
        }

        return kInvalidIndex;
    }

    [[nodiscard]] std::uint32_t lookup_insert_slot(const InstrumentId instrument_id,
                                                   bool& duplicate) const noexcept
    {
        duplicate = false;
        if (lookup_capacity_ == 0) {
            return kInvalidIndex;
        }

        const std::uint32_t mask = lookup_capacity_ - 1U;
        std::uint32_t slot = hash(instrument_id) & mask;
        for (std::uint32_t probe = 0; probe < lookup_capacity_; ++probe) {
            const LookupSlot& entry = lookup_[slot];
            if (!entry.occupied) {
                return slot;
            }
            if (entry.instrument_id == instrument_id) {
                duplicate = true;
                return slot;
            }
            slot = (slot + 1U) & mask;
        }

        return kInvalidIndex;
    }

    [[nodiscard]] static constexpr std::uint32_t span_size_to_u32(const std::size_t size) noexcept
    {
        return size > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())
                   ? std::numeric_limits<std::uint32_t>::max()
                   : static_cast<std::uint32_t>(size);
    }

    [[nodiscard]] static constexpr std::uint32_t lookup_capacity_for(const std::uint32_t max_instruments) noexcept
    {
        constexpr std::uint32_t kMaxLookupCapacity = (std::numeric_limits<std::uint32_t>::max() / 2U) + 1U;
        if (max_instruments == 0) {
            return 0;
        }

        const std::uint32_t required =
            max_instruments > (kMaxLookupCapacity / 2U) ? kMaxLookupCapacity : max_instruments * 2U;
        std::uint32_t capacity = 2;
        while (capacity < required && capacity < kMaxLookupCapacity) {
            capacity <<= 1U;
        }
        return capacity;
    }

    [[nodiscard]] static double utilization(const std::uint32_t used,
                                            const std::uint32_t capacity) noexcept
    {
        return capacity == 0 ? 0.0 : static_cast<double>(used) / static_cast<double>(capacity);
    }

    [[nodiscard]] static constexpr std::uint64_t splitmix64(std::uint64_t value) noexcept
    {
        value += 0x9e3779b97f4a7c15ULL;
        value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
        value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
        return value ^ (value >> 31U);
    }

    [[nodiscard]] static constexpr std::uint32_t hash(const InstrumentId instrument_id) noexcept
    {
        return static_cast<std::uint32_t>(splitmix64(instrument_id));
    }

    template <typename Result>
    [[nodiscard]] static Result unknown_result() noexcept
    {
        Result result{};
        result.status = Status::UnknownInstrument;
        return result;
    }
};

struct MatchingEngineCreateResult final {
    MatchingEngineInitError error{MatchingEngineInitError::None};
    std::size_t config_index{kInvalidInstrumentConfigIndex};
    std::unique_ptr<MatchingEngine> engine{};

    [[nodiscard]] bool has_value() const noexcept
    {
        return engine != nullptr;
    }

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return has_value();
    }

    [[nodiscard]] MatchingEngine* get() noexcept
    {
        return engine.get();
    }

    [[nodiscard]] const MatchingEngine* get() const noexcept
    {
        return engine.get();
    }
};

template <std::size_t N>
MatchingEngineCreateResult MatchingEngine::create(
    const InstrumentConfig (&configs)[N]) noexcept
{
    return create(span_size_to_u32(N), std::span<const InstrumentConfig, N>(configs));
}

template <std::size_t N>
MatchingEngineCreateResult MatchingEngine::create(
    const std::uint32_t max_instruments,
    const InstrumentConfig (&configs)[N]) noexcept
{
    return create(max_instruments, std::span<const InstrumentConfig, N>(configs));
}

inline MatchingEngineCreateResult MatchingEngine::create(
    const std::span<const InstrumentConfig> configs) noexcept
{
    return create(span_size_to_u32(configs.size()), configs);
}

inline MatchingEngineCreateResult MatchingEngine::create(
    const std::uint32_t max_instruments,
    const std::span<const InstrumentConfig> configs) noexcept
{
    try {
        std::unique_ptr<MatchingEngine> engine =
            std::make_unique<MatchingEngine>(max_instruments, configs);
        if (!engine->valid()) {
            const MatchingEngineInitError error = engine->initialization_error();
            const std::size_t config_index = engine->failed_config_index();
            return MatchingEngineCreateResult{error, config_index, nullptr};
        }

        return MatchingEngineCreateResult{
            MatchingEngineInitError::None,
            kInvalidInstrumentConfigIndex,
            std::move(engine),
        };
    } catch (const std::bad_alloc&) {
        return MatchingEngineCreateResult{
            MatchingEngineInitError::AllocationFailure,
            kInvalidInstrumentConfigIndex,
            nullptr,
        };
    }
}

static_assert(alignof(MatchingEngine) >= 64);

} // namespace eigenbook
