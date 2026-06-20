#include "FuzzConfig.hpp"
#include "Snapshot.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <span>

namespace {

using namespace eigenbook;

inline constexpr std::size_t kSnapshotBufferSize = 4096;

[[noreturn]] void fail() noexcept
{
    std::abort();
}

[[nodiscard]] bool valid_price(const BookConfig& config, const Price price) noexcept
{
    return price >= config.min_price && price <= config.max_price &&
           price_distance(config.min_price, price) % static_cast<std::uint64_t>(config.tick_size) == 0U;
}

[[nodiscard]] bool valid_depth(const OrderBook& book,
                               const Side side,
                               std::uint32_t& order_count) noexcept
{
    std::array<DepthLevel, fuzzing::kMaxOrders> levels{};
    const std::uint32_t written = book.depth(side, static_cast<std::uint32_t>(levels.size()), levels.data());
    if (written > levels.size()) {
        return false;
    }

    const BestQuote best = side == Side::Buy ? book.best_bid() : book.best_ask();
    if (written == 0) {
        return !best.valid;
    }
    if (!best.valid || best.price != levels[0].price || best.quantity != levels[0].aggregate_quantity ||
        best.order_count != levels[0].order_count) {
        return false;
    }

    order_count = 0;
    for (std::uint32_t index = 0; index < written; ++index) {
        const DepthLevel& level = levels[index];
        if (!valid_price(book.config(), level.price) || level.aggregate_quantity == 0 || level.order_count == 0 ||
            book.depth_at_price(side, level.price) != level.aggregate_quantity ||
            book.order_count_at_price(side, level.price) != level.order_count ||
            std::numeric_limits<std::uint32_t>::max() - order_count < level.order_count) {
            return false;
        }
        if (index != 0) {
            const bool ordered = side == Side::Buy ? levels[index - 1U].price > level.price
                                                    : levels[index - 1U].price < level.price;
            if (!ordered) {
                return false;
            }
        }
        order_count += level.order_count;
    }

    return true;
}

[[nodiscard]] bool valid_order_metadata(const OrderBook& book) noexcept
{
    std::array<std::byte, kSnapshotBufferSize> buffer{};
    const SnapshotWriteResult snapshot = serialize(book, buffer);
    if (snapshot.status != Status::Accepted) {
        return false;
    }

    const std::span<const std::byte> bytes(buffer.data(), snapshot.bytes_written);
    detail::BookHeaderInfo header{};
    if (detail::parse_book_header(bytes, header, true) != Status::Accepted ||
        header.order_count != book.live_order_count()) {
        return false;
    }

    for (std::uint32_t index = 0; index < header.order_count; ++index) {
        BookSnapshotOrder order{};
        const std::size_t offset =
            header.orders_offset + static_cast<std::size_t>(index) * detail::kBookOrderWireSize;
        if (!detail::read_order_at(bytes, offset, order)) {
            return false;
        }

        const Order* const live_order = book.find_order(order.id);
        if (live_order == nullptr || live_order->side != order.side || live_order->price != order.price ||
            live_order->quantity != order.quantity || live_order->timestamp != order.timestamp ||
            live_order->sequence != order.sequence) {
            return false;
        }

        for (std::uint32_t previous_index = 0; previous_index < index; ++previous_index) {
            BookSnapshotOrder previous{};
            const std::size_t previous_offset =
                header.orders_offset + static_cast<std::size_t>(previous_index) * detail::kBookOrderWireSize;
            if (!detail::read_order_at(bytes, previous_offset, previous) || previous.id == order.id ||
                (previous.sequence == order.sequence &&
                 order.sequence != std::numeric_limits<SequenceNumber>::max()) ||
                (previous.side == order.side && previous.price == order.price &&
                 previous.sequence > order.sequence)) {
                return false;
            }
        }
    }

    return true;
}

[[nodiscard]] bool valid_book(const OrderBook& book) noexcept
{
    const OrderBookStats stats = book.stats();
    if (!book.config().valid() || stats.live_order_count != book.live_order_count() ||
        stats.live_order_count > stats.order_capacity || stats.order_id_map.size != stats.live_order_count ||
        stats.bids.occupied_level_count + stats.asks.occupied_level_count > stats.live_order_count) {
        return false;
    }

    std::uint32_t bid_orders = 0;
    std::uint32_t ask_orders = 0;
    if (!valid_depth(book, Side::Buy, bid_orders) || !valid_depth(book, Side::Sell, ask_orders) ||
        !valid_order_metadata(book) ||
        bid_orders + ask_orders != stats.live_order_count) {
        return false;
    }

    const TopOfBook top = book.top_of_book();
    return top.status == Status::Accepted &&
           (!top.bid.valid || !top.ask.valid || top.bid.price < top.ask.price);
}

void verify_serializable_round_trip(const OrderBook& book) noexcept
{
    std::array<std::byte, kSnapshotBufferSize> first{};
    const SnapshotWriteResult first_result = serialize(book, first);
    if (first_result.status != Status::Accepted || first_result.bytes_written == 0) {
        fail();
    }

    OrderBook verifier(book.config(), book.instrument_id());
    if (restore(verifier, std::span<const std::byte>(first.data(), first_result.bytes_written)) != Status::Accepted ||
        !valid_book(verifier)) {
        fail();
    }

    std::array<std::byte, kSnapshotBufferSize> second{};
    const SnapshotWriteResult second_result = serialize(verifier, second);
    if (second_result.status != Status::Accepted || second_result.bytes_written != first_result.bytes_written ||
        std::memcmp(first.data(), second.data(), first_result.bytes_written) != 0) {
        fail();
    }
}

void verify_continued_trading(OrderBook& book, const PriceLevelMode mode) noexcept
{
    const Quantity sweep_quantity = std::numeric_limits<Quantity>::max();
    const MatchResult buy_sweep = book.match_market_order(Side::Buy, sweep_quantity, 30'001, 101);
    const MatchResult sell_sweep = book.match_market_order(Side::Sell, sweep_quantity, 30'002, 102);
    if (buy_sweep.status == Status::InternalError || buy_sweep.status == Status::EventLogFull ||
        sell_sweep.status == Status::InternalError || sell_sweep.status == Status::EventLogFull ||
        book.live_order_count() != 0) {
        fail();
    }

    const Price price = mode == PriceLevelMode::Sparse ? 0 : 100;
    if (book.add_limit_order(1, Side::Buy, price, 5, 103).status != Status::Accepted ||
        book.add_limit_order(2, Side::Sell, price, 5, 104).status != Status::Filled ||
        book.add_limit_order(3, Side::Buy, price, 2, 105).status != Status::Accepted ||
        book.cancel_order(3, 106).status != Status::Cancelled || book.live_order_count() != 0 ||
        !valid_book(book)) {
        fail();
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, const std::size_t size)
{
    if (size == 0) {
        return 0;
    }

    const PriceLevelMode mode = (data[0] & 1U) == 0U ? PriceLevelMode::Dense : PriceLevelMode::Sparse;
    const BookConfig config = fuzzing::config_for_mode(mode);
    OrderBook destination(config);
    if (!fuzzing::populate_destination_book(destination, mode)) {
        fail();
    }

    std::array<std::byte, kSnapshotBufferSize> before{};
    const SnapshotWriteResult before_result = serialize(destination, before);
    if (before_result.status != Status::Accepted) {
        fail();
    }

    const std::span<const std::byte> snapshot(reinterpret_cast<const std::byte*>(data + 1U), size - 1U);
    const Status status = restore(destination, snapshot);
    if (status != Status::Accepted) {
        std::array<std::byte, kSnapshotBufferSize> after{};
        const SnapshotWriteResult after_result = serialize(destination, after);
        const std::span<const BookEvent> last_events = destination.last_events();
        if (after_result.status != Status::Accepted || after_result.bytes_written != before_result.bytes_written ||
            std::memcmp(before.data(), after.data(), before_result.bytes_written) != 0 || last_events.size() != 2U ||
            last_events.back().kind != BookEvent::Kind::OrderResting || last_events.back().order_id != 20'002) {
            fail();
        }
        return 0;
    }

    if (!destination.last_events().empty() || !valid_book(destination)) {
        fail();
    }
    verify_serializable_round_trip(destination);
    verify_continued_trading(destination, mode);
    return 0;
}
