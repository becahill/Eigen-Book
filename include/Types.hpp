#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace eigenbook {

using OrderId = std::uint64_t;
using InstrumentId = std::uint32_t;
using ParticipantId = std::uint64_t;
using Price = std::int64_t;
using Quantity = std::uint64_t;
using Timestamp = std::uint64_t;
using SequenceNumber = std::uint64_t;

inline constexpr OrderId kInvalidOrderId = 0;
inline constexpr InstrumentId kInvalidInstrumentId = 0;
inline constexpr ParticipantId kAnonymousParticipantId = 0;
inline constexpr std::uint32_t kInvalidIndex = std::numeric_limits<std::uint32_t>::max();
inline constexpr std::size_t kProbeHistogramBucketCount = 16;

enum class Side : std::uint8_t {
    Buy,
    Sell,
};

enum class TimeInForce : std::uint8_t {
    Gtc,
    Ioc,
    Fok,
};

/// Self-trade prevention policy applied by one instrument.
///
/// Participant id zero is anonymous and never triggers STP.
enum class SelfTradePolicy : std::uint8_t {
    Disabled,
    CancelAggressor,
    CancelResting,
    CancelBoth,
};

enum class PriceLevelMode : std::uint8_t {
    /// Flat price-indexed storage over every configured tick.
    Dense,
    /// Fixed-capacity sparse storage for wide price ranges.
    ///
    /// Each side can occupy at most `BookConfig::max_orders` price levels.
    /// Level creation/removal is bounded by that capacity; order FIFO within a
    /// level remains intrusive and allocation-free after construction.
    Sparse,
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
    UnknownInstrument,
    InvalidQuantity,
    InvalidPrice,
    DuplicateOrderId,
    PoolExhausted,
    OrderIdMapFull,
    QuantityIncreaseRejected,
    InvalidConfiguration,
    InternalError,
    FokRejected,
    BufferTooSmall,
    SnapshotFormatMismatch,
    SnapshotVersionMismatch,
    SnapshotConfigurationMismatch,
    SnapshotCapacityExceeded,
    InvalidCommand,
    EventLogFull,
    LotSizeViolation,
    PostOnlyWouldCross,
    InvalidPostOnlyTimeInForce,
    SelfTradePrevented,
    MarketDataLogFull,
    JournalFormatMismatch,
    JournalVersionMismatch,
    JournalLengthMismatch,
    JournalChecksumMismatch,
    JournalInvalidField,
    ReplayDiverged,
    /// A resting residual would overflow the fixed-width aggregate quantity
    /// stored by its target price level.
    PriceLevelQuantityOverflow,

    // Compatibility aliases for older call sites.
    Ok = Accepted,
    OrderNotFound = UnknownOrderId,
};

inline constexpr std::size_t kStatusCount =
    static_cast<std::size_t>(Status::PriceLevelQuantityOverflow) + 1U;

struct TradeEvent final {
    InstrumentId instrument_id{kInvalidInstrumentId};
    OrderId aggressor_id{kInvalidOrderId};
    OrderId resting_id{kInvalidOrderId};
    Side aggressor_side{Side::Buy};
    Price price{0};
    Quantity quantity{0};
    Timestamp timestamp{0};
    SequenceNumber sequence{0};
};

struct BookEvent final {
    enum class Kind : std::uint8_t {
        Trade,
        OrderAccepted,
        OrderResting,
        OrderCancelled,
        OrderModified,
        OrderRejected,
    };

    Kind kind{Kind::OrderRejected};
    InstrumentId instrument_id{kInvalidInstrumentId};
    Status status{Status::InternalError};
    OrderId order_id{kInvalidOrderId};
    Side side{Side::Buy};
    Price price{0};
    Quantity quantity{0};
    Quantity old_quantity{0};
    Quantity new_quantity{0};
    Timestamp timestamp{0};
    SequenceNumber sequence{0};
    TimeInForce time_in_force{TimeInForce::Gtc};
    TradeEvent trade{};
};

/// One sequenced incremental market-data update.
///
/// A command may emit zero or more events. Sequence numbers are contiguous and
/// per instrument; rejected commands and commands with no visible book change
/// emit no market data and consume no market-data sequence number.
struct MarketDataEvent final {
    enum class Kind : std::uint8_t {
        LevelCreated,
        LevelQuantityChanged,
        LevelDeleted,
        Trade,
        BestBidChanged,
        BestAskChanged,
    };

    Kind kind{Kind::LevelQuantityChanged};
    InstrumentId instrument_id{kInvalidInstrumentId};
    SequenceNumber sequence{0};
    Side side{Side::Buy};
    Price price{0};
    Quantity previous_quantity{0};
    Quantity quantity{0};
    std::uint32_t order_count{0};
    OrderId aggressor_id{kInvalidOrderId};
    OrderId resting_id{kInvalidOrderId};
    Quantity trade_quantity{0};
    Timestamp timestamp{0};
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
    /// `0` selects the default capacity sized for the configured order limit.
    std::uint32_t event_log_capacity{0};
    /// Selects dense or sparse fixed storage for price levels.
    PriceLevelMode price_level_mode{PriceLevelMode::Dense};
    /// `0` or `1` disables quantity-increment enforcement.
    Quantity lot_size{0};
    /// Instrument-wide self-trade prevention policy.
    SelfTradePolicy self_trade_policy{SelfTradePolicy::Disabled};
    /// `0` disables incremental market-data generation.
    std::uint32_t market_data_capacity{0};

    [[nodiscard]] bool valid() const noexcept
    {
        if (min_price > max_price || max_orders == 0 || tick_size <= 0 ||
            (price_level_mode != PriceLevelMode::Dense && price_level_mode != PriceLevelMode::Sparse) ||
            self_trade_policy > SelfTradePolicy::CancelBoth) {
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

struct OrderIdMapStats final {
    std::uint32_t size{0};
    std::uint32_t capacity{0};
    std::uint32_t tombstones{0};
    /// Lookup probes; for erase, includes backward-shifted entries as work.
    std::uint32_t last_probe_count{0};
    /// Histogram of the same probe/work count, saturated into the final bucket.
    std::array<std::uint64_t, kProbeHistogramBucketCount> probe_histogram{};
};

struct BookSideStats final {
    PriceLevelMode mode{PriceLevelMode::Dense};
    std::uint32_t configured_level_count{0};
    std::uint32_t level_storage_capacity{0};
    std::uint32_t occupied_level_count{0};
    std::uint32_t level_map_capacity{0};
    double level_utilization{0.0};
};

struct OrderBookStats final {
    std::uint32_t live_order_count{0};
    std::uint32_t order_capacity{0};
    double order_pool_utilization{0.0};
    double order_id_map_utilization{0.0};
    OrderIdMapStats order_id_map{};
    BookSideStats bids{};
    BookSideStats asks{};
};

struct MatchingEngineStats final {
    std::uint32_t instrument_count{0};
    std::uint32_t max_instruments{0};
    std::uint32_t total_live_order_count{0};
    std::uint32_t total_order_capacity{0};
    double order_pool_utilization{0.0};
    double order_id_map_utilization{0.0};
    OrderIdMapStats aggregate_order_id_map{};
    std::uint64_t dispatch_count{0};
    std::uint64_t adds{0};
    std::uint64_t cancels{0};
    std::uint64_t modifies{0};
    std::uint64_t replaces{0};
    std::uint64_t market_matches{0};
    std::uint64_t rejects{0};
    std::array<std::uint64_t, kStatusCount> rejects_by_status{};
    std::uint32_t event_log_high_water_mark{0};
    std::uint64_t decode_errors{0};
};

struct InstrumentConfig final {
    InstrumentId instrument_id{kInvalidInstrumentId};
    BookConfig book_config{};
    Price tick_size{0};
    Quantity lot_size{0};
    SelfTradePolicy self_trade_policy{SelfTradePolicy::Disabled};
    std::uint32_t market_data_capacity{0};
};

struct BestQuote final {
    bool valid{false};
    Price price{0};
    Quantity quantity{0};
    std::uint32_t order_count{0};
};

struct TopOfBook final {
    Status status{Status::Accepted};
    BestQuote bid{};
    BestQuote ask{};
};

struct DepthLevel final {
    Price price{0};
    Quantity aggregate_quantity{0};
    std::uint32_t order_count{0};
};

struct MatchPreflight final {
    Quantity executable_quantity{0};
    std::uint32_t fill_count{0};
    std::uint32_t resting_cancel_count{0};
    bool aggressor_cancelled{false};
};

struct BookSnapshotOrder final {
    OrderId id{kInvalidOrderId};
    Side side{Side::Buy};
    Price price{0};
    Quantity quantity{0};
    Timestamp timestamp{0};
    SequenceNumber sequence{0};
    ParticipantId participant_id{kAnonymousParticipantId};
    bool post_only{false};
    /// Quantity present when this order most recently joined the resting FIFO.
    Quantity initial_quantity{0};
    /// Persistent lifecycle state for a live resting order.
    OrderState state{OrderState::Inactive};
};

struct BookSnapshotLevelAggregate final {
    Side side{Side::Buy};
    Price price{0};
    Quantity aggregate_quantity{0};
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
    std::uint32_t events_emitted{0};
    /// OrderBook-owned storage; valid until the next mutating call on that book.
    std::span<const BookEvent> events{};
    bool aggressor_cancelled_by_stp{false};
    std::uint32_t resting_orders_cancelled_by_stp{0};
};

struct CancelResult final {
    Status status{Status::InternalError};
    Quantity canceled_quantity{0};
    std::uint32_t events_emitted{0};
    /// OrderBook-owned storage; valid until the next mutating call on that book.
    std::span<const BookEvent> events{};
};

struct ModifyResult final {
    Status status{Status::InternalError};
    Quantity old_quantity{0};
    Quantity new_quantity{0};
    std::uint32_t events_emitted{0};
    /// OrderBook-owned storage; valid until the next mutating call on that book.
    std::span<const BookEvent> events{};
};

struct ReplaceResult final {
    Status status{Status::InternalError};
    Price old_price{0};
    Price new_price{0};
    Quantity old_quantity{0};
    Quantity new_quantity{0};
    Quantity executed_quantity{0};
    Quantity resting_quantity{0};
    std::uint32_t fills{0};
    bool has_last_price{false};
    Price last_price{0};
    std::uint32_t events_emitted{0};
    /// OrderBook-owned storage; valid until the next mutating call on that book.
    std::span<const BookEvent> events{};
    bool aggressor_cancelled_by_stp{false};
    std::uint32_t resting_orders_cancelled_by_stp{0};
};

struct MatchResult final {
    Status status{Status::InternalError};
    /// Always echoes the direct market request, including validation failures.
    Quantity requested_quantity{0};
    Quantity executed_quantity{0};
    /// Unexecuted requested quantity, including the full request on rejection.
    Quantity remaining_quantity{0};
    std::uint32_t fills{0};
    bool has_last_price{false};
    Price last_price{0};
    std::uint32_t events_emitted{0};
    /// OrderBook-owned storage; valid until the next mutating call on that book.
    std::span<const BookEvent> events{};
    bool aggressor_cancelled_by_stp{false};
    std::uint32_t resting_orders_cancelled_by_stp{0};
};

/// Common return shape for `MatchingEngine::dispatch`.
///
/// Fields are populated according to the routed operation. Decode failures,
/// invalid commands, and unknown instruments return an empty `events` span and
/// do not begin a book event-log operation.
struct DispatchResult final {
    Status status{Status::InternalError};
    Quantity accepted_quantity{0};
    Quantity requested_quantity{0};
    Quantity executed_quantity{0};
    Quantity remaining_quantity{0};
    Quantity resting_quantity{0};
    Quantity canceled_quantity{0};
    Quantity old_quantity{0};
    Quantity new_quantity{0};
    Price old_price{0};
    Price new_price{0};
    std::uint32_t fills{0};
    bool has_last_price{false};
    Price last_price{0};
    std::uint32_t events_emitted{0};
    /// OrderBook-owned storage; valid until the next mutating call on that book.
    std::span<const BookEvent> events{};
    bool aggressor_cancelled_by_stp{false};
    std::uint32_t resting_orders_cancelled_by_stp{0};
    std::uint32_t market_data_events_emitted{0};
    std::span<const MarketDataEvent> market_data_events{};
};

struct SnapshotWriteResult final {
    Status status{Status::InternalError};
    std::size_t bytes_written{0};
};

class OrderBook;
class MatchingEngine;
struct SnapshotAccess;

/// Serialize a book snapshot into caller-owned storage.
///
/// The snapshot is a deterministic little-endian byte stream. `bytes_written`
/// is nonzero only on `Status::Accepted`.
[[nodiscard]] SnapshotWriteResult serialize(const OrderBook& book, std::span<std::byte> out_buffer) noexcept;

/// Serialize an engine snapshot into caller-owned storage.
///
/// The engine snapshot includes each configured instrument in construction
/// order and embeds deterministic book snapshots for their current state.
[[nodiscard]] SnapshotWriteResult serialize(const MatchingEngine& engine, std::span<std::byte> out_buffer) noexcept;

/// Restore an already constructed book from a validated snapshot.
///
/// Validation failures return before clearing the target. Accepted restores emit
/// no events, clear `last_events()`, and preserve subsequent order and event
/// sequence numbering.
[[nodiscard]] Status restore(OrderBook& book, std::span<const std::byte> buffer) noexcept;

/// Restore an already constructed engine with matching instrument configuration.
///
/// The full engine snapshot is validated before any contained book is restored.
/// Accepted restores emit no events for restored books.
[[nodiscard]] Status restore(MatchingEngine& engine, std::span<const std::byte> buffer) noexcept;

[[nodiscard]] constexpr bool is_buy(const Side side) noexcept
{
    return side == Side::Buy;
}

[[nodiscard]] constexpr bool is_sell(const Side side) noexcept
{
    return side == Side::Sell;
}

} // namespace eigenbook
