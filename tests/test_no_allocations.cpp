#include "AllocationTracker.hpp"
#include "BookSide.hpp"
#include "Command.hpp"
#include "EventLog.hpp"
#include "Journal.hpp"
#include "MarketData.hpp"
#include "MatchingEngine.hpp"
#include "MemoryPool.hpp"
#include "Order.hpp"
#include "OrderBook.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <span>

namespace {

using namespace eigenbook;
namespace tracker = eigenbook::test::allocation_tracker;

[[noreturn]] void fail(const char* const file,
                       const int line,
                       const char* const expression)
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

void check_no_allocations(const std::uint64_t allocations,
                          const char* const operation)
{
    if (allocations != 0U) {
        std::fprintf(stderr,
                     "zero-allocation contract failed for %s: %llu C++ allocation call(s)\n",
                     operation,
                     static_cast<unsigned long long>(allocations));
        std::abort();
    }
}

template <typename Result, typename Operation>
[[nodiscard]] std::uint64_t measure(Result& result, Operation&& operation) noexcept
{
    tracker::ScopedCounter counter;
    result = operation();
    return counter.finish();
}

[[nodiscard]] constexpr BookConfig book_config(const PriceLevelMode mode,
                                               const std::uint32_t max_orders = 32U,
                                               const std::uint32_t id_capacity = 128U,
                                               const std::uint32_t event_capacity = 64U) noexcept
{
    return BookConfig{1, 1'000, max_orders, id_capacity, 1, event_capacity, mode};
}

void warm_book(OrderBook& book)
{
    const AddOrderResult add = book.add_limit_order(9'000, Side::Buy, 50, 1, 1);
    CHECK(add.status == Status::Accepted);
    const CancelResult cancel = book.cancel_order(9'000, 2);
    CHECK(cancel.status == Status::Cancelled);
    CHECK(book.live_order_count() == 0U);
}

void add_resting(OrderBook& book,
                 const OrderId id,
                 const Side side,
                 const Price price,
                 const Quantity quantity)
{
    const AddOrderResult result = book.add_limit_order(id, side, price, quantity);
    CHECK(result.status == Status::Accepted);
    CHECK(result.resting_quantity == quantity);
}

void cancel_resting(OrderBook& book, const OrderId id)
{
    const CancelResult result = book.cancel_order(id);
    CHECK(result.status == Status::Cancelled);
}

void verify_tracker_positive_control()
{
    constexpr std::size_t alignment = 64U;
    constexpr std::align_val_t aligned{alignment};
    const std::nothrow_t& nothrow = std::nothrow;

    void* scalar = nullptr;
    void* array = nullptr;
    void* nothrow_scalar = nullptr;
    void* nothrow_array = nullptr;
    void* aligned_scalar = nullptr;
    void* aligned_array = nullptr;
    void* aligned_nothrow_scalar = nullptr;
    void* aligned_nothrow_array = nullptr;

    tracker::reset();
    CHECK(tracker::count() == 0U);

    std::uint64_t detected = 0;
    {
        tracker::ScopedCounter counter;
        scalar = ::operator new(8U);
        array = ::operator new[](8U);
        nothrow_scalar = ::operator new(8U, nothrow);
        nothrow_array = ::operator new[](8U, nothrow);
        aligned_scalar = ::operator new(8U, aligned);
        aligned_array = ::operator new[](8U, aligned);
        aligned_nothrow_scalar = ::operator new(8U, aligned, nothrow);
        aligned_nothrow_array = ::operator new[](8U, aligned, nothrow);
        detected = counter.finish();
    }

    CHECK(detected == 8U);
    CHECK(scalar != nullptr);
    CHECK(array != nullptr);
    CHECK(nothrow_scalar != nullptr);
    CHECK(nothrow_array != nullptr);
    CHECK(aligned_scalar != nullptr);
    CHECK(aligned_array != nullptr);
    CHECK(aligned_nothrow_scalar != nullptr);
    CHECK(aligned_nothrow_array != nullptr);
    CHECK(reinterpret_cast<std::uintptr_t>(aligned_scalar) % alignment == 0U);
    CHECK(reinterpret_cast<std::uintptr_t>(aligned_array) % alignment == 0U);
    CHECK(reinterpret_cast<std::uintptr_t>(aligned_nothrow_scalar) % alignment == 0U);
    CHECK(reinterpret_cast<std::uintptr_t>(aligned_nothrow_array) % alignment == 0U);

    ::operator delete(scalar);
    ::operator delete[](array);
    ::operator delete(nothrow_scalar, nothrow);
    ::operator delete[](nothrow_array, nothrow);
    ::operator delete(aligned_scalar, aligned);
    ::operator delete[](aligned_array, aligned);
    ::operator delete(aligned_nothrow_scalar, aligned, nothrow);
    ::operator delete[](aligned_nothrow_array, aligned, nothrow);

    const std::new_handler previous_handler = std::set_new_handler(nullptr);
    void* const failed_nothrow =
        ::operator new(std::numeric_limits<std::size_t>::max(), aligned, nothrow);
    bool threw_bad_alloc = false;
    try {
        void* const unexpected =
            ::operator new(std::numeric_limits<std::size_t>::max(), aligned);
        ::operator delete(unexpected, aligned);
    } catch (const std::bad_alloc&) {
        threw_bad_alloc = true;
    }
    std::set_new_handler(previous_handler);
    CHECK(failed_nothrow == nullptr);
    CHECK(threw_bad_alloc);

    std::printf("allocation tracker positive control: detected %llu/8 intentional allocations\n",
                static_cast<unsigned long long>(detected));
}

void verify_book_hot_path(const PriceLevelMode mode)
{
    OrderBook book(book_config(mode), mode == PriceLevelMode::Dense ? 101U : 202U);
    warm_book(book);

    AddOrderResult add_result{};
    std::uint64_t allocations = measure(add_result, [&book]() noexcept {
        return book.add_limit_order(1, Side::Buy, 100, 10, 10);
    });
    check_no_allocations(allocations, "resting limit-order insertion");
    CHECK(add_result.status == Status::Accepted);
    CHECK(add_result.resting_quantity == 10U);
    CHECK(add_result.events_emitted == 2U);
    CHECK(add_result.events[0].kind == BookEvent::Kind::OrderAccepted);
    CHECK(add_result.events[1].kind == BookEvent::Kind::OrderResting);
    CHECK(book.depth_at_price(Side::Buy, 100) == 10U);

    ModifyResult modify_result{};
    allocations = measure(modify_result, [&book]() noexcept {
        return book.modify_order(1, 6, 11);
    });
    check_no_allocations(allocations, "quantity reduction");
    CHECK(modify_result.status == Status::Accepted);
    CHECK(modify_result.old_quantity == 10U);
    CHECK(modify_result.new_quantity == 6U);
    CHECK(modify_result.events_emitted == 1U);
    CHECK(book.depth_at_price(Side::Buy, 100) == 6U);

    ReplaceResult replace_result{};
    allocations = measure(replace_result, [&book]() noexcept {
        return book.replace_order(1, 100, 5, 12, TimeInForce::Gtc);
    });
    check_no_allocations(allocations, "same-price priority-preserving replace");
    CHECK(replace_result.status == Status::Accepted);
    CHECK(replace_result.old_quantity == 6U);
    CHECK(replace_result.new_quantity == 5U);
    CHECK(replace_result.resting_quantity == 5U);
    CHECK(replace_result.events_emitted == 1U);
    CHECK(book.depth_at_price(Side::Buy, 100) == 5U);

    allocations = measure(replace_result, [&book]() noexcept {
        return book.replace_order(1, 99, 7, 13, TimeInForce::Gtc);
    });
    check_no_allocations(allocations, "lose-priority replace");
    CHECK(replace_result.status == Status::Accepted);
    CHECK(replace_result.old_price == 100);
    CHECK(replace_result.new_price == 99);
    CHECK(replace_result.resting_quantity == 7U);
    CHECK(replace_result.events_emitted == 3U);
    CHECK(book.depth_at_price(Side::Buy, 100) == 0U);
    CHECK(book.depth_at_price(Side::Buy, 99) == 7U);

    CancelResult cancel_result{};
    allocations = measure(cancel_result, [&book]() noexcept {
        return book.cancel_order(1, 14);
    });
    check_no_allocations(allocations, "cancellation by order id");
    CHECK(cancel_result.status == Status::Cancelled);
    CHECK(cancel_result.canceled_quantity == 7U);
    CHECK(cancel_result.events_emitted == 1U);
    CHECK(book.live_order_count() == 0U);

    add_resting(book, 2, Side::Sell, 100, 5);
    allocations = measure(add_result, [&book]() noexcept {
        return book.add_limit_order(3, Side::Buy, 100, 5, 20);
    });
    check_no_allocations(allocations, "full limit-order match");
    CHECK(add_result.status == Status::Filled);
    CHECK(add_result.executed_quantity == 5U);
    CHECK(add_result.resting_quantity == 0U);
    CHECK(add_result.fills == 1U);
    CHECK(add_result.events_emitted == 2U);
    CHECK(book.live_order_count() == 0U);

    add_resting(book, 4, Side::Sell, 100, 3);
    allocations = measure(add_result, [&book]() noexcept {
        return book.add_limit_order(5, Side::Buy, 100, 5, 21);
    });
    check_no_allocations(allocations, "partial limit-order match");
    CHECK(add_result.status == Status::PartiallyFilled);
    CHECK(add_result.executed_quantity == 3U);
    CHECK(add_result.resting_quantity == 2U);
    CHECK(add_result.events_emitted == 3U);
    CHECK(book.depth_at_price(Side::Buy, 100) == 2U);
    cancel_resting(book, 5);

    add_resting(book, 6, Side::Sell, 100, 5);
    MatchResult match_result{};
    allocations = measure(match_result, [&book]() noexcept {
        return book.match_market_order(Side::Buy, 2, 7, 22);
    });
    check_no_allocations(allocations, "fully filled market order");
    CHECK(match_result.status == Status::Filled);
    CHECK(match_result.executed_quantity == 2U);
    CHECK(match_result.remaining_quantity == 0U);
    CHECK(match_result.events_emitted == 1U);
    CHECK(book.depth_at_price(Side::Sell, 100) == 3U);
    cancel_resting(book, 6);

    add_resting(book, 8, Side::Sell, 100, 2);
    allocations = measure(match_result, [&book]() noexcept {
        return book.match_market_order(Side::Buy, 5, 9, 23);
    });
    check_no_allocations(allocations, "partially filled market order");
    CHECK(match_result.status == Status::PartiallyFilled);
    CHECK(match_result.executed_quantity == 2U);
    CHECK(match_result.remaining_quantity == 3U);
    CHECK(match_result.events_emitted == 1U);
    CHECK(book.live_order_count() == 0U);

    allocations = measure(add_result, [&book]() noexcept {
        return book.add_limit_order(10, Side::Buy, 100, 5, 24, TimeInForce::Ioc);
    });
    check_no_allocations(allocations, "IOC no-liquidity rejection");
    CHECK(add_result.status == Status::NoLiquidity);
    CHECK(add_result.executed_quantity == 0U);
    CHECK(add_result.resting_quantity == 0U);
    CHECK(add_result.events_emitted == 2U);
    CHECK(book.live_order_count() == 0U);

    add_resting(book, 11, Side::Sell, 100, 2);
    allocations = measure(add_result, [&book]() noexcept {
        return book.add_limit_order(12, Side::Buy, 100, 5, 25, TimeInForce::Ioc);
    });
    check_no_allocations(allocations, "IOC partial fill");
    CHECK(add_result.status == Status::PartiallyFilled);
    CHECK(add_result.executed_quantity == 2U);
    CHECK(add_result.resting_quantity == 0U);
    CHECK(add_result.events_emitted == 3U);
    CHECK(book.live_order_count() == 0U);

    add_resting(book, 13, Side::Sell, 100, 3);
    allocations = measure(add_result, [&book]() noexcept {
        return book.add_limit_order(14, Side::Buy, 100, 3, 26, TimeInForce::Ioc);
    });
    check_no_allocations(allocations, "IOC full fill");
    CHECK(add_result.status == Status::Filled);
    CHECK(add_result.executed_quantity == 3U);
    CHECK(add_result.events_emitted == 2U);
    CHECK(book.live_order_count() == 0U);

    add_resting(book, 15, Side::Sell, 100, 2);
    allocations = measure(add_result, [&book]() noexcept {
        return book.add_limit_order(16, Side::Buy, 100, 3, 27, TimeInForce::Fok);
    });
    check_no_allocations(allocations, "FOK rejection");
    CHECK(add_result.status == Status::FokRejected);
    CHECK(add_result.executed_quantity == 0U);
    CHECK(add_result.events_emitted == 1U);
    CHECK(book.depth_at_price(Side::Sell, 100) == 2U);
    cancel_resting(book, 15);

    add_resting(book, 17, Side::Sell, 100, 1);
    add_resting(book, 18, Side::Sell, 101, 2);
    allocations = measure(add_result, [&book]() noexcept {
        return book.add_limit_order(19, Side::Buy, 101, 3, 28, TimeInForce::Fok);
    });
    check_no_allocations(allocations, "FOK full execution");
    CHECK(add_result.status == Status::Filled);
    CHECK(add_result.executed_quantity == 3U);
    CHECK(add_result.fills == 2U);
    CHECK(add_result.events_emitted == 3U);
    CHECK(book.live_order_count() == 0U);
}

void verify_event_logging_is_allocation_free()
{
    EventLog event_log(4U, 303U);
    event_log.begin_operation(1U);
    event_log.append_order(
        BookEvent::Kind::OrderAccepted, Status::Accepted, 1, Side::Buy, 100, 1, 1);
    CHECK(event_log.last_count() == 1U);
    event_log.reset();

    constexpr std::uint32_t repetitions = 256U;
    std::uint64_t allocations = 0;
    {
        tracker::ScopedCounter counter;
        for (std::uint32_t iteration = 0; iteration < repetitions; ++iteration) {
            event_log.begin_operation(2U);
            event_log.append_order(BookEvent::Kind::OrderAccepted,
                                   Status::Accepted,
                                   iteration + 1U,
                                   Side::Buy,
                                   100,
                                   2,
                                   iteration);
            event_log.append_trade(
                iteration + 1U, iteration + 2U, Side::Buy, 100, 1, iteration);
        }
        allocations = counter.finish();
    }

    check_no_allocations(allocations, "fixed-capacity event logging");
    CHECK(event_log.last_count() == 2U);
    const std::span<const BookEvent> events = event_log.last_events();
    CHECK(events.size() == 2U);
    CHECK(events[0].kind == BookEvent::Kind::OrderAccepted);
    CHECK(events[1].kind == BookEvent::Kind::Trade);
    CHECK(events[0].instrument_id == 303U);
    CHECK(events[1].trade.instrument_id == 303U);
}

void verify_repeated_multi_instrument_dispatch()
{
    constexpr InstrumentId dense_id = 401U;
    constexpr InstrumentId sparse_id = 402U;
    constexpr InstrumentConfig instruments[] = {
        InstrumentConfig{dense_id, book_config(PriceLevelMode::Dense)},
        InstrumentConfig{sparse_id, book_config(PriceLevelMode::Sparse)},
    };
    MatchingEngine engine(instruments);
    CHECK(engine.valid());

    AddOrderResult warm_add =
        engine.add_limit_order(dense_id, 9'001, Side::Buy, 50, 1);
    CHECK(warm_add.status == Status::Accepted);
    CHECK(engine.cancel_order(dense_id, 9'001).status == Status::Cancelled);
    warm_add = engine.add_limit_order(sparse_id, 9'002, Side::Buy, 50, 1);
    CHECK(warm_add.status == Status::Accepted);
    CHECK(engine.cancel_order(sparse_id, 9'002).status == Status::Cancelled);

    constexpr std::array<Command, 6U> commands{{
        Command{dense_id, CommandOp::Add, 1, Side::Buy, 90, 2, TimeInForce::Gtc, 1},
        Command{sparse_id, CommandOp::Add, 2, Side::Sell, 110, 3, TimeInForce::Gtc, 2},
        Command{dense_id, CommandOp::Modify, 1, Side::Buy, 0, 1, TimeInForce::Gtc, 3},
        Command{sparse_id, CommandOp::Replace, 2, Side::Sell, 111, 2, TimeInForce::Gtc, 4},
        Command{dense_id, CommandOp::Cancel, 1, Side::Buy, 0, 0, TimeInForce::Gtc, 5},
        Command{sparse_id, CommandOp::Market, 3, Side::Buy, 0, 2, TimeInForce::Gtc, 6},
    }};
    constexpr std::array<Status, commands.size()> expected{{
        Status::Accepted,
        Status::Accepted,
        Status::Accepted,
        Status::Accepted,
        Status::Cancelled,
        Status::Filled,
    }};

    std::array<std::array<std::byte, kCommandWireSize>, commands.size()> encoded{};
    for (std::size_t index = 0; index < commands.size(); ++index) {
        CHECK(encode(commands[index], encoded[index]) == Status::Accepted);
    }

    constexpr std::size_t cycles = 128U;
    std::array<Status, cycles * commands.size()> statuses{};
    DispatchResult result{};
    std::uint64_t allocations = 0;
    {
        tracker::ScopedCounter counter;
        for (std::size_t cycle = 0; cycle < cycles; ++cycle) {
            for (std::size_t index = 0; index < commands.size(); ++index) {
                if ((cycle & 1U) == 0U) {
                    result = engine.dispatch(commands[index]);
                } else {
                    result = engine.dispatch(std::span<const std::byte>(encoded[index]));
                }
                statuses[(cycle * commands.size()) + index] = result.status;
            }
        }
        allocations = counter.finish();
    }

    check_no_allocations(allocations, "repeated multi-instrument command dispatch");
    for (std::size_t cycle = 0; cycle < cycles; ++cycle) {
        for (std::size_t index = 0; index < commands.size(); ++index) {
            CHECK(statuses[(cycle * commands.size()) + index] == expected[index]);
        }
    }
    CHECK(engine.live_order_count(dense_id) == 0U);
    CHECK(engine.live_order_count(sparse_id) == 0U);
    CHECK(engine.stats().dispatch_count == cycles * commands.size());
}

void verify_order_book_capacity_exhaustion(const PriceLevelMode mode)
{
    OrderBook pool_book(book_config(mode, 2U, 8U, 8U));
    warm_book(pool_book);
    add_resting(pool_book, 1, Side::Buy, 100, 1);
    add_resting(pool_book, 2, Side::Buy, 101, 1);

    AddOrderResult result{};
    std::uint64_t allocations = measure(result, [&pool_book]() noexcept {
        return pool_book.add_limit_order(3, Side::Buy, 102, 1);
    });
    check_no_allocations(allocations, "order-pool capacity rejection");
    CHECK(result.status == Status::PoolExhausted);
    CHECK(result.events_emitted == 1U);
    CHECK(pool_book.live_order_count() == 2U);
    CHECK(pool_book.find_order(3) == nullptr);

    OrderBook map_book(book_config(mode, 4U, 2U, 8U));
    warm_book(map_book);
    add_resting(map_book, 11, Side::Buy, 100, 1);
    add_resting(map_book, 12, Side::Buy, 101, 1);
    CHECK(map_book.stats().order_id_map.size == map_book.stats().order_id_map.capacity);

    allocations = measure(result, [&map_book]() noexcept {
        return map_book.add_limit_order(13, Side::Buy, 102, 1);
    });
    check_no_allocations(allocations, "order-id-map capacity rejection");
    CHECK(result.status == Status::OrderIdMapFull);
    CHECK(result.events_emitted == 1U);
    CHECK(map_book.live_order_count() == 2U);
    CHECK(map_book.find_order(13) == nullptr);

    OrderBook event_book(book_config(mode, 4U, 16U, 1U));
    const CancelResult warm_rejection = event_book.cancel_order(9'003);
    CHECK(warm_rejection.status == Status::UnknownOrderId);
    CHECK(warm_rejection.events_emitted == 1U);

    allocations = measure(result, [&event_book]() noexcept {
        return event_book.add_limit_order(21, Side::Buy, 100, 1);
    });
    check_no_allocations(allocations, "event-log capacity rejection");
    CHECK(result.status == Status::EventLogFull);
    CHECK(result.events_emitted == 0U);
    CHECK(result.events.empty());
    CHECK(event_book.live_order_count() == 0U);
    CHECK(event_book.find_order(21) == nullptr);
}

void verify_component_capacity_exhaustion()
{
    MemoryPool<Order> pool(1U);
    Order* const first = pool.allocate();
    CHECK(first != nullptr);
    Order* exhausted = first;
    std::uint64_t allocations = measure(exhausted, [&pool]() noexcept {
        return pool.allocate();
    });
    check_no_allocations(allocations, "memory-pool component exhaustion");
    CHECK(exhausted == nullptr);
    CHECK(pool.size() == pool.capacity());

    Order first_map_order{};
    Order second_map_order{};
    Order rejected_map_order{};
    first_map_order.reset(1, Side::Buy, 100, 1);
    second_map_order.reset(2, Side::Buy, 101, 1);
    rejected_map_order.reset(3, Side::Buy, 102, 1);
    OrderIdMap order_ids(2U);
    CHECK(order_ids.insert(1, &first_map_order) == Status::Accepted);
    CHECK(order_ids.insert(2, &second_map_order) == Status::Accepted);

    Status status = Status::Accepted;
    allocations = measure(status, [&order_ids, &rejected_map_order]() noexcept {
        return order_ids.insert(3, &rejected_map_order);
    });
    check_no_allocations(allocations, "order-id-map component exhaustion");
    CHECK(status == Status::OrderIdMapFull);
    CHECK(order_ids.size() == order_ids.capacity());

    const BookConfig sparse_config = book_config(PriceLevelMode::Sparse, 2U, 8U, 8U);
    SparseBookSide sparse_side(Side::Buy, sparse_config);
    Order warm_order{};
    warm_order.reset(90, Side::Buy, 90, 1);
    CHECK(sparse_side.add_order(warm_order) == Status::Accepted);
    CHECK(sparse_side.remove_order(warm_order) == Status::Accepted);

    Order first_level{};
    Order second_level{};
    Order rejected_level{};
    first_level.reset(91, Side::Buy, 100, 1);
    second_level.reset(92, Side::Buy, 200, 1);
    rejected_level.reset(93, Side::Buy, 300, 1);
    CHECK(sparse_side.add_order(first_level) == Status::Accepted);
    CHECK(sparse_side.add_order(second_level) == Status::Accepted);
    CHECK(sparse_side.occupied_level_count() == 2U);

    allocations = measure(status, [&sparse_side, &rejected_level]() noexcept {
        return sparse_side.add_order(rejected_level);
    });
    check_no_allocations(allocations, "sparse price-level capacity rejection");
    CHECK(status == Status::PoolExhausted);
    CHECK(sparse_side.occupied_level_count() == 2U);
    CHECK(rejected_level.level == nullptr);
}

[[nodiscard]] constexpr BookConfig venue_allocation_config(const PriceLevelMode mode) noexcept
{
    BookConfig config = book_config(mode, 32U, 128U, 128U);
    config.lot_size = 5;
    config.self_trade_policy = SelfTradePolicy::CancelResting;
    config.market_data_capacity = 128;
    return config;
}

void verify_venue_market_data_and_recovery_are_allocation_free(const PriceLevelMode mode)
{
    OrderBook book(venue_allocation_config(mode), 501U);
    AddOrderResult add{};
    ModifyResult modify{};
    ReplaceResult replace{};
    MatchResult match{};
    std::uint64_t allocations = 0;
    {
        tracker::ScopedCounter counter;
        add = book.add_limit_order(1, Side::Sell, 100, 10, 1, TimeInForce::Gtc, 42);
        CHECK(add.status == Status::Accepted);
        add = book.add_limit_order(2, Side::Buy, 100, 5, 2, TimeInForce::Gtc, 7, true);
        CHECK(add.status == Status::PostOnlyWouldCross);
        add = book.add_limit_order(3, Side::Buy, 100, 5, 3, TimeInForce::Gtc, 42);
        CHECK(add.status == Status::Accepted);
        modify = book.modify_order(3, 5, 4);
        CHECK(modify.status == Status::Accepted);
        replace = book.replace_order(3, 99, 5, 5, TimeInForce::Gtc, true);
        CHECK(replace.status == Status::Accepted);
        match = book.match_market_order(Side::Sell, 5, 4, 6, 7);
        CHECK(match.status == Status::Filled);
        allocations = counter.finish();
    }
    check_no_allocations(allocations, "venue rules and incremental market data");

    constexpr InstrumentId instrument_id = 502U;
    const InstrumentConfig instruments[]{
        InstrumentConfig{instrument_id, venue_allocation_config(mode)},
    };
    MatchingEngine original(instruments);
    MatchingEngine replayed(instruments);
    constexpr std::array<VenueCommand, 4> commands{{
        VenueCommand{Command{instrument_id, CommandOp::Add, 11, Side::Sell, 100, 10, TimeInForce::Gtc, 1},
                     42,
                     false},
        VenueCommand{Command{instrument_id, CommandOp::Add, 12, Side::Buy, 100, 5, TimeInForce::Gtc, 2},
                     42,
                     false},
        VenueCommand{Command{instrument_id, CommandOp::Replace, 12, Side::Buy, 99, 5, TimeInForce::Gtc, 3},
                     0,
                     true},
        VenueCommand{Command{instrument_id, CommandOp::Cancel, 12, Side::Buy, 0, 0, TimeInForce::Gtc, 4},
                     0,
                     false},
    }};
    std::array<std::byte, commands.size() * kJournalRecordLength> records{};
    {
        tracker::ScopedCounter counter;
        for (std::size_t index = 0; index < commands.size(); ++index) {
            const JournalWriteResult result = dispatch_and_record(
                original,
                commands[index],
                index + 1U,
                std::span<std::byte>(records).subspan(
                    index * kJournalRecordLength, kJournalRecordLength));
            CHECK(result.status == Status::Accepted);
        }
        allocations = counter.finish();
    }
    check_no_allocations(allocations, "bounded journal record production");

    JournalReplayCursor cursor{};
    {
        tracker::ScopedCounter counter;
        const JournalReplayResult result = replay_journal(replayed, records, cursor);
        CHECK(result.status == Status::Accepted);
        CHECK(result.records_replayed == commands.size());
        allocations = counter.finish();
    }
    check_no_allocations(allocations, "journal replay and deterministic verification");
    CHECK(replayed.state_checksum() == original.state_checksum());
}

} // namespace

int main()
{
    verify_tracker_positive_control();
    verify_book_hot_path(PriceLevelMode::Dense);
    verify_book_hot_path(PriceLevelMode::Sparse);
    verify_event_logging_is_allocation_free();
    verify_repeated_multi_instrument_dispatch();
    verify_order_book_capacity_exhaustion(PriceLevelMode::Dense);
    verify_order_book_capacity_exhaustion(PriceLevelMode::Sparse);
    verify_component_capacity_exhaustion();
    verify_venue_market_data_and_recovery_are_allocation_free(PriceLevelMode::Dense);
    verify_venue_market_data_and_recovery_are_allocation_free(PriceLevelMode::Sparse);
    return 0;
}
