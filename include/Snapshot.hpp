#pragma once

#include "MatchingEngine.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>

namespace eigenbook {

inline constexpr std::uint8_t kSnapshotFormatVersion = 2;

template <std::uint32_t MaxOrders, std::uint32_t MaxLevels>
struct BookSnapshot final {
    std::uint8_t version{kSnapshotFormatVersion};
    InstrumentId instrument_id{kInvalidInstrumentId};
    BookConfig config{};
    SequenceNumber next_sequence{0};
    SequenceNumber event_next_sequence{0};
    std::uint32_t order_count{0};
    std::array<BookSnapshotOrder, MaxOrders> orders{};
    std::uint32_t level_count{0};
    std::array<BookSnapshotLevelAggregate, MaxLevels> levels{};
};

template <std::uint32_t MaxOrders, std::uint32_t MaxLevels>
struct EngineSnapshotInstrument final {
    InstrumentConfig config{};
    BookSnapshot<MaxOrders, MaxLevels> book{};
};

template <std::uint32_t MaxInstruments, std::uint32_t MaxOrdersPerBook, std::uint32_t MaxLevelsPerBook>
struct EngineSnapshot final {
    std::uint8_t version{kSnapshotFormatVersion};
    std::uint32_t max_instruments{0};
    std::uint32_t instrument_count{0};
    std::array<EngineSnapshotInstrument<MaxOrdersPerBook, MaxLevelsPerBook>, MaxInstruments> instruments{};
};

struct SnapshotAccess final {
    [[nodiscard]] static SequenceNumber next_sequence(const OrderBook& book) noexcept
    {
        return book.snapshot_next_sequence();
    }

    [[nodiscard]] static SequenceNumber event_next_sequence(const OrderBook& book) noexcept
    {
        return book.snapshot_event_next_sequence();
    }

    [[nodiscard]] static std::uint32_t level_count(const OrderBook& book) noexcept
    {
        return book.snapshot_level_count();
    }

    template <typename Fn>
    static void for_each_order(const OrderBook& book, Fn&& fn) noexcept
    {
        book.snapshot_for_each_order(fn);
    }

    template <typename Fn>
    static void for_each_level(const OrderBook& book, Fn&& fn) noexcept
    {
        book.snapshot_for_each_level(fn);
    }

    static void clear_book(OrderBook& book) noexcept
    {
        book.clear_for_snapshot_restore();
    }

    [[nodiscard]] static Status restore_order(OrderBook& book, const BookSnapshotOrder& order) noexcept
    {
        return book.restore_snapshot_order(order);
    }

    static void finish_book_restore(OrderBook& book,
                                    const SequenceNumber next_sequence,
                                    const SequenceNumber event_next_sequence) noexcept
    {
        book.finish_snapshot_restore(next_sequence, event_next_sequence);
    }

    [[nodiscard]] static bool valid(const MatchingEngine& engine) noexcept
    {
        return engine.valid_;
    }

    [[nodiscard]] static std::uint32_t max_instruments(const MatchingEngine& engine) noexcept
    {
        return engine.max_instruments_;
    }

    [[nodiscard]] static std::uint32_t instrument_count(const MatchingEngine& engine) noexcept
    {
        return engine.instrument_count_;
    }

    [[nodiscard]] static bool has_book_at(const MatchingEngine& engine, const std::uint32_t index) noexcept
    {
        return engine.instruments_ != nullptr && index < engine.instrument_count_ &&
               engine.instruments_[index].book != nullptr;
    }

    [[nodiscard]] static const InstrumentConfig& instrument_config_at(const MatchingEngine& engine,
                                                                      const std::uint32_t index) noexcept
    {
        return engine.instruments_[index].config;
    }

    [[nodiscard]] static const OrderBook& book_at(const MatchingEngine& engine, const std::uint32_t index) noexcept
    {
        return *engine.instruments_[index].book;
    }

    [[nodiscard]] static OrderBook& book_at(MatchingEngine& engine, const std::uint32_t index) noexcept
    {
        return *engine.instruments_[index].book;
    }
};

namespace detail {

inline constexpr std::size_t kBookOrderWireSize = 8 + 1 + 8 + 8 + 8 + 8;
inline constexpr std::size_t kLevelWireSize = 1 + 8 + 8 + 4;
inline constexpr std::size_t kConfigWireSize = 8 + 8 + 4 + 4 + 8 + 4 + 1;
inline constexpr std::size_t kInstrumentConfigWireSize = 4 + kConfigWireSize + 8 + 8;
inline constexpr std::size_t kBookHeaderWireSize = 4 + 1 + 3 + 4 + kConfigWireSize + 4 + 4 + 8 + 8;
inline constexpr std::size_t kEngineHeaderWireSize = 4 + 1 + 3 + 4 + 4 + 1 + 3;

class SnapshotWriter final {
public:
    explicit SnapshotWriter(std::span<std::byte> buffer) noexcept : buffer_(buffer) {}

    void write_u8(const std::uint8_t value) noexcept
    {
        if (!reserve(1)) {
            return;
        }
        buffer_[offset_] = static_cast<std::byte>(value);
        ++offset_;
    }

    void write_u32(const std::uint32_t value) noexcept
    {
        if (!reserve(4)) {
            return;
        }
        for (std::uint32_t i = 0; i < 4; ++i) {
            buffer_[offset_ + i] = static_cast<std::byte>((value >> (i * 8U)) & 0xffU);
        }
        offset_ += 4;
    }

    void write_u64(const std::uint64_t value) noexcept
    {
        if (!reserve(8)) {
            return;
        }
        for (std::uint32_t i = 0; i < 8; ++i) {
            buffer_[offset_ + i] = static_cast<std::byte>((value >> (i * 8U)) & 0xffU);
        }
        offset_ += 8;
    }

    void write_price(const Price value) noexcept
    {
        std::uint64_t raw = 0;
        std::memcpy(&raw, &value, sizeof(raw));
        write_u64(raw);
    }

    void write_magic(const char a, const char b, const char c, const char d) noexcept
    {
        write_u8(static_cast<std::uint8_t>(a));
        write_u8(static_cast<std::uint8_t>(b));
        write_u8(static_cast<std::uint8_t>(c));
        write_u8(static_cast<std::uint8_t>(d));
    }

    [[nodiscard]] bool ok() const noexcept
    {
        return ok_;
    }

    [[nodiscard]] std::size_t offset() const noexcept
    {
        return offset_;
    }

private:
    std::span<std::byte> buffer_;
    std::size_t offset_{0};
    bool ok_{true};

    [[nodiscard]] bool reserve(const std::size_t bytes) noexcept
    {
        if (!ok_ || bytes > buffer_.size() - offset_) {
            ok_ = false;
            return false;
        }
        return true;
    }
};

class SnapshotReader final {
public:
    explicit SnapshotReader(std::span<const std::byte> buffer) noexcept : buffer_(buffer) {}

    [[nodiscard]] bool read_u8(std::uint8_t& value) noexcept
    {
        if (!reserve(1)) {
            return false;
        }
        value = std::to_integer<std::uint8_t>(buffer_[offset_]);
        ++offset_;
        return true;
    }

    [[nodiscard]] bool read_u32(std::uint32_t& value) noexcept
    {
        if (!reserve(4)) {
            return false;
        }
        value = 0;
        for (std::uint32_t i = 0; i < 4; ++i) {
            value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(buffer_[offset_ + i])) << (i * 8U);
        }
        offset_ += 4;
        return true;
    }

    [[nodiscard]] bool read_u64(std::uint64_t& value) noexcept
    {
        if (!reserve(8)) {
            return false;
        }
        value = 0;
        for (std::uint32_t i = 0; i < 8; ++i) {
            value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(buffer_[offset_ + i])) << (i * 8U);
        }
        offset_ += 8;
        return true;
    }

    [[nodiscard]] bool read_price(Price& value) noexcept
    {
        std::uint64_t raw = 0;
        if (!read_u64(raw)) {
            return false;
        }
        std::memcpy(&value, &raw, sizeof(value));
        return true;
    }

    [[nodiscard]] bool read_magic(const char a, const char b, const char c, const char d) noexcept
    {
        std::uint8_t values[4]{};
        return read_u8(values[0]) && read_u8(values[1]) && read_u8(values[2]) && read_u8(values[3]) &&
               values[0] == static_cast<std::uint8_t>(a) && values[1] == static_cast<std::uint8_t>(b) &&
               values[2] == static_cast<std::uint8_t>(c) && values[3] == static_cast<std::uint8_t>(d);
    }

    [[nodiscard]] std::size_t offset() const noexcept
    {
        return offset_;
    }

private:
    std::span<const std::byte> buffer_;
    std::size_t offset_{0};

    [[nodiscard]] bool reserve(const std::size_t bytes) const noexcept
    {
        return bytes <= buffer_.size() - offset_;
    }
};

struct BookHeaderInfo final {
    InstrumentId instrument_id{kInvalidInstrumentId};
    BookConfig config{};
    std::uint32_t order_count{0};
    std::uint32_t level_count{0};
    SequenceNumber next_sequence{0};
    SequenceNumber event_next_sequence{0};
    std::size_t orders_offset{0};
    std::size_t levels_offset{0};
    std::size_t end_offset{0};
};

struct EngineHeaderInfo final {
    std::uint32_t max_instruments{0};
    std::uint32_t instrument_count{0};
    std::size_t payload_offset{0};
};

[[nodiscard]] inline bool checked_mul(const std::size_t lhs,
                                      const std::size_t rhs,
                                      std::size_t& out) noexcept
{
    if (lhs != 0 && rhs > std::numeric_limits<std::size_t>::max() / lhs) {
        return false;
    }
    out = lhs * rhs;
    return true;
}

[[nodiscard]] inline bool checked_add(const std::size_t lhs,
                                      const std::size_t rhs,
                                      std::size_t& out) noexcept
{
    if (rhs > std::numeric_limits<std::size_t>::max() - lhs) {
        return false;
    }
    out = lhs + rhs;
    return true;
}

[[nodiscard]] inline bool same_config(const BookConfig& lhs, const BookConfig& rhs) noexcept
{
    return lhs.min_price == rhs.min_price && lhs.max_price == rhs.max_price &&
           lhs.max_orders == rhs.max_orders && lhs.order_id_map_capacity == rhs.order_id_map_capacity &&
           lhs.tick_size == rhs.tick_size && lhs.event_log_capacity == rhs.event_log_capacity &&
           lhs.price_level_mode == rhs.price_level_mode;
}

[[nodiscard]] inline bool same_instrument_config(const InstrumentConfig& lhs,
                                                 const InstrumentConfig& rhs) noexcept
{
    return lhs.instrument_id == rhs.instrument_id && same_config(lhs.book_config, rhs.book_config) &&
           lhs.tick_size == rhs.tick_size && lhs.lot_size == rhs.lot_size;
}

[[nodiscard]] inline bool valid_side(const Side side) noexcept
{
    return side == Side::Buy || side == Side::Sell;
}

[[nodiscard]] inline bool valid_price_for_config(const BookConfig& config, const Price price) noexcept
{
    if (!config.valid() || price < config.min_price || price > config.max_price) {
        return false;
    }
    return price_distance(config.min_price, price) % static_cast<std::uint64_t>(config.tick_size) == 0U;
}

inline void write_config(SnapshotWriter& writer, const BookConfig& config) noexcept
{
    writer.write_price(config.min_price);
    writer.write_price(config.max_price);
    writer.write_u32(config.max_orders);
    writer.write_u32(config.order_id_map_capacity);
    writer.write_price(config.tick_size);
    writer.write_u32(config.event_log_capacity);
    writer.write_u8(static_cast<std::uint8_t>(config.price_level_mode));
}

[[nodiscard]] inline bool read_config(SnapshotReader& reader, BookConfig& config) noexcept
{
    std::uint8_t price_level_mode = 0;
    return reader.read_price(config.min_price) && reader.read_price(config.max_price) &&
           reader.read_u32(config.max_orders) && reader.read_u32(config.order_id_map_capacity) &&
           reader.read_price(config.tick_size) && reader.read_u32(config.event_log_capacity) &&
           reader.read_u8(price_level_mode) &&
           (price_level_mode <= static_cast<std::uint8_t>(PriceLevelMode::Sparse)) &&
           (config.price_level_mode = static_cast<PriceLevelMode>(price_level_mode), true);
}

inline void write_instrument_config(SnapshotWriter& writer, const InstrumentConfig& config) noexcept
{
    writer.write_u32(config.instrument_id);
    write_config(writer, config.book_config);
    writer.write_price(config.tick_size);
    writer.write_u64(config.lot_size);
}

[[nodiscard]] inline bool read_instrument_config(SnapshotReader& reader, InstrumentConfig& config) noexcept
{
    return reader.read_u32(config.instrument_id) && read_config(reader, config.book_config) &&
           reader.read_price(config.tick_size) && reader.read_u64(config.lot_size);
}

inline void write_order(SnapshotWriter& writer, const BookSnapshotOrder& order) noexcept
{
    writer.write_u64(order.id);
    writer.write_u8(static_cast<std::uint8_t>(order.side));
    writer.write_price(order.price);
    writer.write_u64(order.quantity);
    writer.write_u64(order.timestamp);
    writer.write_u64(order.sequence);
}

[[nodiscard]] inline bool read_order(SnapshotReader& reader, BookSnapshotOrder& order) noexcept
{
    std::uint8_t side = 0;
    if (!reader.read_u64(order.id) || !reader.read_u8(side) || side > static_cast<std::uint8_t>(Side::Sell) ||
        !reader.read_price(order.price) || !reader.read_u64(order.quantity) ||
        !reader.read_u64(order.timestamp) || !reader.read_u64(order.sequence)) {
        return false;
    }

    order.side = static_cast<Side>(side);
    return true;
}

inline void write_level(SnapshotWriter& writer, const BookSnapshotLevelAggregate& level) noexcept
{
    writer.write_u8(static_cast<std::uint8_t>(level.side));
    writer.write_price(level.price);
    writer.write_u64(level.aggregate_quantity);
    writer.write_u32(level.order_count);
}

[[nodiscard]] inline bool read_level(SnapshotReader& reader, BookSnapshotLevelAggregate& level) noexcept
{
    std::uint8_t side = 0;
    if (!reader.read_u8(side) || side > static_cast<std::uint8_t>(Side::Sell) ||
        !reader.read_price(level.price) || !reader.read_u64(level.aggregate_quantity) ||
        !reader.read_u32(level.order_count)) {
        return false;
    }

    level.side = static_cast<Side>(side);
    return true;
}

[[nodiscard]] inline bool read_order_at(std::span<const std::byte> buffer,
                                        const std::size_t offset,
                                        BookSnapshotOrder& order) noexcept
{
    if (offset > buffer.size() || kBookOrderWireSize > buffer.size() - offset) {
        return false;
    }
    SnapshotReader reader(buffer.subspan(offset, kBookOrderWireSize));
    return read_order(reader, order) && reader.offset() == kBookOrderWireSize;
}

[[nodiscard]] inline bool read_level_at(std::span<const std::byte> buffer,
                                        const std::size_t offset,
                                        BookSnapshotLevelAggregate& level) noexcept
{
    if (offset > buffer.size() || kLevelWireSize > buffer.size() - offset) {
        return false;
    }
    SnapshotReader reader(buffer.subspan(offset, kLevelWireSize));
    return read_level(reader, level) && reader.offset() == kLevelWireSize;
}

[[nodiscard]] inline Status parse_book_header(std::span<const std::byte> buffer,
                                              BookHeaderInfo& header,
                                              const bool require_exact) noexcept
{
    SnapshotReader reader(buffer);
    if (!reader.read_magic('E', 'B', 'O', 'K')) {
        return Status::SnapshotFormatMismatch;
    }

    std::uint8_t version = 0;
    std::uint8_t reserved0 = 0;
    std::uint8_t reserved1 = 0;
    std::uint8_t reserved2 = 0;
    if (!reader.read_u8(version) || !reader.read_u8(reserved0) || !reader.read_u8(reserved1) ||
        !reader.read_u8(reserved2)) {
        return Status::SnapshotFormatMismatch;
    }
    if (version != kSnapshotFormatVersion) {
        return Status::SnapshotVersionMismatch;
    }
    if (reserved0 != 0 || reserved1 != 0 || reserved2 != 0) {
        return Status::SnapshotFormatMismatch;
    }

    if (!reader.read_u32(header.instrument_id) || !read_config(reader, header.config) ||
        !reader.read_u32(header.order_count) || !reader.read_u32(header.level_count) ||
        !reader.read_u64(header.next_sequence) || !reader.read_u64(header.event_next_sequence)) {
        return Status::SnapshotFormatMismatch;
    }

    std::size_t orders_bytes = 0;
    std::size_t levels_bytes = 0;
    if (!checked_mul(header.order_count, kBookOrderWireSize, orders_bytes) ||
        !checked_mul(header.level_count, kLevelWireSize, levels_bytes)) {
        return Status::SnapshotFormatMismatch;
    }

    header.orders_offset = reader.offset();
    if (!checked_add(header.orders_offset, orders_bytes, header.levels_offset) ||
        !checked_add(header.levels_offset, levels_bytes, header.end_offset)) {
        return Status::SnapshotFormatMismatch;
    }

    if (header.end_offset > buffer.size()) {
        return Status::SnapshotFormatMismatch;
    }

    if (require_exact && header.end_offset != buffer.size()) {
        return Status::SnapshotFormatMismatch;
    }

    return Status::Accepted;
}

[[nodiscard]] inline Status validate_order_record(std::span<const std::byte> buffer,
                                                  const BookHeaderInfo& header,
                                                  const std::uint32_t index,
                                                  BookSnapshotOrder& order) noexcept
{
    const std::size_t offset = header.orders_offset + static_cast<std::size_t>(index) * kBookOrderWireSize;
    if (!read_order_at(buffer, offset, order)) {
        return Status::SnapshotFormatMismatch;
    }
    if (order.id == kInvalidOrderId || !valid_side(order.side) || order.quantity == 0 ||
        !valid_price_for_config(header.config, order.price) || order.sequence == 0 ||
        order.sequence > header.next_sequence) {
        return Status::SnapshotFormatMismatch;
    }

    for (std::uint32_t previous = 0; previous < index; ++previous) {
        BookSnapshotOrder previous_order{};
        const std::size_t previous_offset =
            header.orders_offset + static_cast<std::size_t>(previous) * kBookOrderWireSize;
        if (!read_order_at(buffer, previous_offset, previous_order)) {
            return Status::SnapshotFormatMismatch;
        }
        if (previous_order.id == order.id) {
            return Status::SnapshotFormatMismatch;
        }
        if (previous_order.sequence == order.sequence &&
            order.sequence != std::numeric_limits<SequenceNumber>::max()) {
            return Status::SnapshotFormatMismatch;
        }
        if (previous_order.side == order.side && previous_order.price == order.price &&
            previous_order.sequence > order.sequence) {
            return Status::SnapshotFormatMismatch;
        }
    }

    return Status::Accepted;
}

[[nodiscard]] inline Status validate_level_record(std::span<const std::byte> buffer,
                                                  const BookHeaderInfo& header,
                                                  const std::uint32_t index,
                                                  BookSnapshotLevelAggregate& level) noexcept
{
    const std::size_t offset = header.levels_offset + static_cast<std::size_t>(index) * kLevelWireSize;
    if (!read_level_at(buffer, offset, level)) {
        return Status::SnapshotFormatMismatch;
    }
    if (!valid_side(level.side) || !valid_price_for_config(header.config, level.price) ||
        level.aggregate_quantity == 0 || level.order_count == 0) {
        return Status::SnapshotFormatMismatch;
    }

    for (std::uint32_t previous = 0; previous < index; ++previous) {
        BookSnapshotLevelAggregate previous_level{};
        const std::size_t previous_offset =
            header.levels_offset + static_cast<std::size_t>(previous) * kLevelWireSize;
        if (!read_level_at(buffer, previous_offset, previous_level)) {
            return Status::SnapshotFormatMismatch;
        }
        if (previous_level.side == level.side && previous_level.price == level.price) {
            return Status::SnapshotFormatMismatch;
        }
    }

    Quantity aggregate = 0;
    std::uint32_t count = 0;
    for (std::uint32_t order_index = 0; order_index < header.order_count; ++order_index) {
        BookSnapshotOrder order{};
        const std::size_t order_offset =
            header.orders_offset + static_cast<std::size_t>(order_index) * kBookOrderWireSize;
        if (!read_order_at(buffer, order_offset, order)) {
            return Status::SnapshotFormatMismatch;
        }
        if (order.side == level.side && order.price == level.price) {
            if (std::numeric_limits<Quantity>::max() - aggregate < order.quantity) {
                return Status::SnapshotFormatMismatch;
            }
            aggregate += order.quantity;
            ++count;
        }
    }

    if (aggregate != level.aggregate_quantity || count != level.order_count) {
        return Status::SnapshotFormatMismatch;
    }

    return Status::Accepted;
}

[[nodiscard]] inline Status validate_order_has_level(std::span<const std::byte> buffer,
                                                     const BookHeaderInfo& header,
                                                     const BookSnapshotOrder& order) noexcept
{
    for (std::uint32_t level_index = 0; level_index < header.level_count; ++level_index) {
        BookSnapshotLevelAggregate level{};
        const std::size_t level_offset =
            header.levels_offset + static_cast<std::size_t>(level_index) * kLevelWireSize;
        if (!read_level_at(buffer, level_offset, level)) {
            return Status::SnapshotFormatMismatch;
        }
        if (level.side == order.side && level.price == order.price) {
            return Status::Accepted;
        }
    }

    return Status::SnapshotFormatMismatch;
}

[[nodiscard]] inline Status validate_book_snapshot(const OrderBook& target,
                                                   std::span<const std::byte> buffer,
                                                   BookHeaderInfo& header,
                                                   const bool require_exact) noexcept
{
    Status status = parse_book_header(buffer, header, require_exact);
    if (status != Status::Accepted) {
        return status;
    }

    if (!header.config.valid()) {
        return Status::SnapshotFormatMismatch;
    }

    if (header.instrument_id != target.instrument_id() || !same_config(header.config, target.config())) {
        return Status::SnapshotConfigurationMismatch;
    }

    if (header.order_count > header.config.max_orders ||
        header.order_count > OrderIdMap::capacity_for(header.config.order_id_map_capacity)) {
        return Status::SnapshotCapacityExceeded;
    }

    const std::uint32_t per_side_level_capacity =
        header.config.price_level_mode == PriceLevelMode::Sparse ? header.config.max_orders
                                                                 : header.config.price_level_count();
    const auto max_levels = static_cast<std::size_t>(per_side_level_capacity) * 2U;
    if (static_cast<std::size_t>(header.level_count) > max_levels) {
        return Status::SnapshotCapacityExceeded;
    }

    if (header.order_count == 0 && header.level_count != 0) {
        return Status::SnapshotFormatMismatch;
    }

    bool has_bid = false;
    bool has_ask = false;
    Price best_bid = 0;
    Price best_ask = 0;
    for (std::uint32_t order_index = 0; order_index < header.order_count; ++order_index) {
        BookSnapshotOrder order{};
        status = validate_order_record(buffer, header, order_index, order);
        if (status != Status::Accepted) {
            return status;
        }

        if (order.side == Side::Buy) {
            if (!has_bid || order.price > best_bid) {
                best_bid = order.price;
            }
            has_bid = true;
        } else {
            if (!has_ask || order.price < best_ask) {
                best_ask = order.price;
            }
            has_ask = true;
        }
    }

    if (has_bid && has_ask && best_bid >= best_ask) {
        return Status::SnapshotFormatMismatch;
    }

    for (std::uint32_t level_index = 0; level_index < header.level_count; ++level_index) {
        BookSnapshotLevelAggregate level{};
        status = validate_level_record(buffer, header, level_index, level);
        if (status != Status::Accepted) {
            return status;
        }
    }

    for (std::uint32_t order_index = 0; order_index < header.order_count; ++order_index) {
        BookSnapshotOrder order{};
        const std::size_t order_offset =
            header.orders_offset + static_cast<std::size_t>(order_index) * kBookOrderWireSize;
        if (!read_order_at(buffer, order_offset, order)) {
            return Status::SnapshotFormatMismatch;
        }
        status = validate_order_has_level(buffer, header, order);
        if (status != Status::Accepted) {
            return status;
        }
    }

    return Status::Accepted;
}

inline void write_book_snapshot(const OrderBook& book, SnapshotWriter& writer) noexcept
{
    writer.write_magic('E', 'B', 'O', 'K');
    writer.write_u8(kSnapshotFormatVersion);
    writer.write_u8(0);
    writer.write_u8(0);
    writer.write_u8(0);
    writer.write_u32(book.instrument_id());
    write_config(writer, book.config());
    writer.write_u32(book.live_order_count());
    writer.write_u32(SnapshotAccess::level_count(book));
    writer.write_u64(SnapshotAccess::next_sequence(book));
    writer.write_u64(SnapshotAccess::event_next_sequence(book));

    SnapshotAccess::for_each_order(book, [&writer](const Order& order) noexcept {
        write_order(writer,
                    BookSnapshotOrder{
                        order.id,
                        order.side,
                        order.price,
                        order.quantity,
                        order.timestamp,
                        order.sequence,
                    });
    });

    SnapshotAccess::for_each_level(book, [&writer](const BookSnapshotLevelAggregate& level) noexcept {
        write_level(writer, level);
    });
}

[[nodiscard]] inline Status parse_engine_header(std::span<const std::byte> buffer,
                                                EngineHeaderInfo& header) noexcept
{
    SnapshotReader reader(buffer);
    if (!reader.read_magic('E', 'B', 'E', 'N')) {
        return Status::SnapshotFormatMismatch;
    }

    std::uint8_t version = 0;
    std::uint8_t reserved0 = 0;
    std::uint8_t reserved1 = 0;
    std::uint8_t reserved2 = 0;
    if (!reader.read_u8(version) || !reader.read_u8(reserved0) || !reader.read_u8(reserved1) ||
        !reader.read_u8(reserved2)) {
        return Status::SnapshotFormatMismatch;
    }
    if (version != kSnapshotFormatVersion) {
        return Status::SnapshotVersionMismatch;
    }
    if (reserved0 != 0 || reserved1 != 0 || reserved2 != 0) {
        return Status::SnapshotFormatMismatch;
    }

    std::uint8_t valid = 0;
    std::uint8_t tail0 = 0;
    std::uint8_t tail1 = 0;
    std::uint8_t tail2 = 0;
    if (!reader.read_u32(header.max_instruments) || !reader.read_u32(header.instrument_count) ||
        !reader.read_u8(valid) || !reader.read_u8(tail0) || !reader.read_u8(tail1) || !reader.read_u8(tail2)) {
        return Status::SnapshotFormatMismatch;
    }
    if (valid != 1 || tail0 != 0 || tail1 != 0 || tail2 != 0) {
        return Status::SnapshotFormatMismatch;
    }

    header.payload_offset = reader.offset();
    return Status::Accepted;
}

[[nodiscard]] inline Status validate_engine_snapshot(const MatchingEngine& engine,
                                                     std::span<const std::byte> buffer,
                                                     EngineHeaderInfo& header) noexcept
{
    Status status = parse_engine_header(buffer, header);
    if (status != Status::Accepted) {
        return status;
    }

    if (!SnapshotAccess::valid(engine) || header.max_instruments != SnapshotAccess::max_instruments(engine) ||
        header.instrument_count != SnapshotAccess::instrument_count(engine) ||
        header.instrument_count > header.max_instruments) {
        return Status::SnapshotConfigurationMismatch;
    }

    std::size_t offset = header.payload_offset;
    for (std::uint32_t index = 0; index < header.instrument_count; ++index) {
        if (!SnapshotAccess::has_book_at(engine, index)) {
            return Status::SnapshotConfigurationMismatch;
        }

        if (offset > buffer.size() || kInstrumentConfigWireSize > buffer.size() - offset) {
            return Status::SnapshotFormatMismatch;
        }

        SnapshotReader instrument_reader(buffer.subspan(offset, kInstrumentConfigWireSize));
        InstrumentConfig snapshot_config{};
        if (!read_instrument_config(instrument_reader, snapshot_config) ||
            instrument_reader.offset() != kInstrumentConfigWireSize) {
            return Status::SnapshotFormatMismatch;
        }

        const InstrumentConfig& target_config = SnapshotAccess::instrument_config_at(engine, index);
        if (!same_instrument_config(snapshot_config, target_config)) {
            return Status::SnapshotConfigurationMismatch;
        }
        offset += kInstrumentConfigWireSize;

        const OrderBook& target_book = SnapshotAccess::book_at(engine, index);
        BookHeaderInfo book_header{};
        status = validate_book_snapshot(target_book, buffer.subspan(offset), book_header, false);
        if (status != Status::Accepted) {
            return status;
        }
        if (book_header.instrument_id != snapshot_config.instrument_id) {
            return Status::SnapshotConfigurationMismatch;
        }
        offset += book_header.end_offset;
    }

    if (offset != buffer.size()) {
        return Status::SnapshotFormatMismatch;
    }

    return Status::Accepted;
}

inline void write_engine_snapshot(const MatchingEngine& engine, SnapshotWriter& writer) noexcept
{
    writer.write_magic('E', 'B', 'E', 'N');
    writer.write_u8(kSnapshotFormatVersion);
    writer.write_u8(0);
    writer.write_u8(0);
    writer.write_u8(0);
    writer.write_u32(SnapshotAccess::max_instruments(engine));
    writer.write_u32(SnapshotAccess::instrument_count(engine));
    writer.write_u8(1);
    writer.write_u8(0);
    writer.write_u8(0);
    writer.write_u8(0);

    for (std::uint32_t index = 0; index < SnapshotAccess::instrument_count(engine); ++index) {
        write_instrument_config(writer, SnapshotAccess::instrument_config_at(engine, index));
        write_book_snapshot(SnapshotAccess::book_at(engine, index), writer);
    }
}

} // namespace detail

[[nodiscard]] inline SnapshotWriteResult serialize(const OrderBook& book,
                                                   std::span<std::byte> out_buffer) noexcept
{
    if (!book.config().valid()) {
        return SnapshotWriteResult{Status::InvalidConfiguration, 0};
    }

    detail::SnapshotWriter writer(out_buffer);
    detail::write_book_snapshot(book, writer);
    if (!writer.ok()) {
        return SnapshotWriteResult{Status::BufferTooSmall, 0};
    }

    return SnapshotWriteResult{Status::Accepted, writer.offset()};
}

[[nodiscard]] inline Status restore(OrderBook& book, std::span<const std::byte> buffer) noexcept
{
    detail::BookHeaderInfo header{};
    Status status = detail::validate_book_snapshot(book, buffer, header, true);
    if (status != Status::Accepted) {
        return status;
    }

    SnapshotAccess::clear_book(book);
    for (std::uint32_t index = 0; index < header.order_count; ++index) {
        BookSnapshotOrder order{};
        const std::size_t offset =
            header.orders_offset + static_cast<std::size_t>(index) * detail::kBookOrderWireSize;
        if (!detail::read_order_at(buffer, offset, order)) {
            SnapshotAccess::clear_book(book);
            return Status::SnapshotFormatMismatch;
        }
        status = SnapshotAccess::restore_order(book, order);
        if (status != Status::Accepted) {
            SnapshotAccess::clear_book(book);
            return status;
        }
    }

    SnapshotAccess::finish_book_restore(book, header.next_sequence, header.event_next_sequence);
    return Status::Accepted;
}

[[nodiscard]] inline SnapshotWriteResult serialize(const MatchingEngine& engine,
                                                   std::span<std::byte> out_buffer) noexcept
{
    if (!SnapshotAccess::valid(engine)) {
        return SnapshotWriteResult{Status::InvalidConfiguration, 0};
    }

    detail::SnapshotWriter writer(out_buffer);
    detail::write_engine_snapshot(engine, writer);
    if (!writer.ok()) {
        return SnapshotWriteResult{Status::BufferTooSmall, 0};
    }

    return SnapshotWriteResult{Status::Accepted, writer.offset()};
}

[[nodiscard]] inline Status restore(MatchingEngine& engine, std::span<const std::byte> buffer) noexcept
{
    detail::EngineHeaderInfo header{};
    Status status = detail::validate_engine_snapshot(engine, buffer, header);
    if (status != Status::Accepted) {
        return status;
    }

    std::size_t offset = header.payload_offset;
    for (std::uint32_t index = 0; index < header.instrument_count; ++index) {
        offset += detail::kInstrumentConfigWireSize;

        detail::BookHeaderInfo book_header{};
        status = detail::parse_book_header(buffer.subspan(offset), book_header, false);
        if (status != Status::Accepted) {
            return status;
        }

        OrderBook& target_book = SnapshotAccess::book_at(engine, index);
        status = restore(target_book, buffer.subspan(offset, book_header.end_offset));
        if (status != Status::Accepted) {
            return status;
        }
        offset += book_header.end_offset;
    }

    return Status::Accepted;
}

} // namespace eigenbook
