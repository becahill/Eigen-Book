#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace eigenbook {

using OrderId = std::uint64_t;
using InstrumentId = std::uint32_t;
using Price = std::int64_t;
using Quantity = std::uint64_t;
using Timestamp = std::uint64_t;
using SequenceNumber = std::uint64_t;

inline constexpr OrderId kInvalidOrderId = 0;
inline constexpr InstrumentId kInvalidInstrumentId = 0;
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

enum class PriceLevelMode : std::uint8_t {
    Dense,
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

    // Compatibility aliases for older call sites.
    Ok = Accepted,
    OrderNotFound = UnknownOrderId,
};

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
    std::uint32_t event_log_capacity{0};
    PriceLevelMode price_level_mode{PriceLevelMode::Dense};

    [[nodiscard]] bool valid() const noexcept
    {
        if (min_price > max_price || max_orders == 0 || tick_size <= 0 ||
            (price_level_mode != PriceLevelMode::Dense && price_level_mode != PriceLevelMode::Sparse)) {
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
    std::uint32_t last_probe_count{0};
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
};

struct InstrumentConfig final {
    InstrumentId instrument_id{kInvalidInstrumentId};
    BookConfig book_config{};
    Price tick_size{0};
    Quantity lot_size{0};
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

struct BookSnapshotOrder final {
    OrderId id{kInvalidOrderId};
    Side side{Side::Buy};
    Price price{0};
    Quantity quantity{0};
    Timestamp timestamp{0};
    SequenceNumber sequence{0};
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
    // OrderBook-owned storage; valid until the next mutating call on that book.
    std::span<const BookEvent> events{};
};

struct CancelResult final {
    Status status{Status::InternalError};
    Quantity canceled_quantity{0};
    std::uint32_t events_emitted{0};
    // OrderBook-owned storage; valid until the next mutating call on that book.
    std::span<const BookEvent> events{};
};

struct ModifyResult final {
    Status status{Status::InternalError};
    Quantity old_quantity{0};
    Quantity new_quantity{0};
    std::uint32_t events_emitted{0};
    // OrderBook-owned storage; valid until the next mutating call on that book.
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
    // OrderBook-owned storage; valid until the next mutating call on that book.
    std::span<const BookEvent> events{};
};

struct MatchResult final {
    Status status{Status::InternalError};
    Quantity requested_quantity{0};
    Quantity executed_quantity{0};
    Quantity remaining_quantity{0};
    std::uint32_t fills{0};
    bool has_last_price{false};
    Price last_price{0};
    std::uint32_t events_emitted{0};
    // OrderBook-owned storage; valid until the next mutating call on that book.
    std::span<const BookEvent> events{};
};

struct SnapshotWriteResult final {
    Status status{Status::InternalError};
    std::size_t bytes_written{0};
};

class OrderBook;
class MatchingEngine;
struct SnapshotAccess;

[[nodiscard]] SnapshotWriteResult serialize(const OrderBook& book, std::span<std::byte> out_buffer) noexcept;
[[nodiscard]] SnapshotWriteResult serialize(const MatchingEngine& engine, std::span<std::byte> out_buffer) noexcept;
[[nodiscard]] Status restore(OrderBook& book, std::span<const std::byte> buffer) noexcept;
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
