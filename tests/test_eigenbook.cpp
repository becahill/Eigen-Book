#include "OrderBook.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <limits>
#include <map>

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

void check_add_result(const AddOrderResult& actual, const AddOrderResult& expected, const char* context)
{
    check_status(actual.status, expected.status, context);
    CHECK(actual.accepted_quantity == expected.accepted_quantity);
    CHECK(actual.executed_quantity == expected.executed_quantity);
    CHECK(actual.resting_quantity == expected.resting_quantity);
    CHECK(actual.fills == expected.fills);
    CHECK(actual.has_last_price == expected.has_last_price);
    CHECK(actual.last_price == expected.last_price);
}

void check_cancel_result(const CancelResult& actual, const CancelResult& expected, const char* context)
{
    check_status(actual.status, expected.status, context);
    CHECK(actual.canceled_quantity == expected.canceled_quantity);
}

void check_modify_result(const ModifyResult& actual, const ModifyResult& expected, const char* context)
{
    check_status(actual.status, expected.status, context);
    CHECK(actual.old_quantity == expected.old_quantity);
    CHECK(actual.new_quantity == expected.new_quantity);
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
    explicit ReferenceOrderBook(const BookConfig& config)
        : config_(normalize_config(config)),
          id_capacity_(OrderIdMap::capacity_for(config_.order_id_map_capacity))
    {
    }

    [[nodiscard]] AddOrderResult add_limit_order(const OrderId id,
                                                 const Side side,
                                                 const Price price,
                                                 const Quantity quantity,
                                                 const Timestamp timestamp = 0)
    {
        AddOrderResult result{};
        result.accepted_quantity = quantity;

        const Status validation_status = validate_new_order(id, price, quantity);
        if (validation_status != Status::Accepted) {
            result.status = validation_status;
            return result;
        }

        if (is_active(id)) {
            result.status = Status::DuplicateOrderId;
            return result;
        }

        const Status id_slot_status = can_insert_id();
        if ((id_slot_status != Status::Accepted || live_order_count_ == config_.max_orders) &&
            executable_quantity(side, quantity, price) < quantity) {
            result.status = residual_reject_status(id_slot_status);
            return result;
        }

        const MatchResult match_result = match_against(side, quantity, true, price);
        result.executed_quantity = match_result.executed_quantity;
        result.fills = match_result.fills;
        result.has_last_price = match_result.has_last_price;
        result.last_price = match_result.last_price;

        if (match_result.remaining_quantity == 0) {
            result.status = Status::Filled;
            return result;
        }

        if (id_slot_status != Status::Accepted) {
            result.status = id_slot_status;
            return result;
        }

        if (live_order_count_ == config_.max_orders) {
            result.status = Status::PoolExhausted;
            return result;
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
        return result;
    }

    [[nodiscard]] CancelResult cancel_order(const OrderId id)
    {
        CancelResult result{};
        auto order_it = orders_.find(id);
        if (order_it == orders_.end() || !order_it->second.active) {
            result.status = Status::UnknownOrderId;
            return result;
        }

        ReferenceOrder& order = order_it->second;
        result.canceled_quantity = order.quantity;
        remove_from_level(order.side, order.price, id);
        order.active = false;
        order.quantity = 0;
        --live_order_count_;
        result.status = Status::Cancelled;
        return result;
    }

    [[nodiscard]] ModifyResult modify_order(const OrderId id, const Quantity new_quantity)
    {
        ModifyResult result{};
        if (new_quantity == 0) {
            result.status = Status::InvalidQuantity;
            return result;
        }

        auto order_it = orders_.find(id);
        if (order_it == orders_.end() || !order_it->second.active) {
            result.status = Status::UnknownOrderId;
            return result;
        }

        ReferenceOrder& order = order_it->second;
        result.old_quantity = order.quantity;
        result.new_quantity = new_quantity;

        if (new_quantity == order.quantity) {
            result.status = Status::Accepted;
            return result;
        }

        if (new_quantity > order.quantity) {
            result.status = Status::QuantityIncreaseRejected;
            return result;
        }

        order.quantity = new_quantity;
        result.status = Status::Accepted;
        return result;
    }

    [[nodiscard]] MatchResult match_market_order(const Side aggressor_side, const Quantity quantity)
    {
        if (quantity == 0) {
            MatchResult result{};
            result.status = Status::InvalidQuantity;
            return result;
        }

        return match_against(aggressor_side, quantity, false, 0);
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
        return normalized;
    }

    [[nodiscard]] static std::uint32_t saturated_double(const std::uint32_t value) noexcept
    {
        if (value > std::numeric_limits<std::uint32_t>::max() / 2U) {
            return std::numeric_limits<std::uint32_t>::max();
        }
        return std::max(2U, value * 2U);
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
                                            const Price limit_price)
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

    for (std::uint32_t i = 0; i < config.price_level_count(); ++i) {
        const Price price = config.min_price + static_cast<Price>(i) * config.tick_size;
        CHECK(actual.depth_at_price(Side::Buy, price) == expected.depth_at_price(Side::Buy, price));
        CHECK(actual.depth_at_price(Side::Sell, price) == expected.depth_at_price(Side::Sell, price));
        CHECK(actual.order_count_at_price(Side::Buy, price) == expected.order_count_at_price(Side::Buy, price));
        CHECK(actual.order_count_at_price(Side::Sell, price) == expected.order_count_at_price(Side::Sell, price));
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

void run_add(OrderBook& actual,
             ReferenceOrderBook& expected,
             const OrderId id,
             const Side side,
             const Price price,
             const Quantity quantity,
             const Timestamp timestamp)
{
    const AddOrderResult actual_result = actual.add_limit_order(id, side, price, quantity, timestamp);
    const AddOrderResult expected_result = expected.add_limit_order(id, side, price, quantity, timestamp);
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

void run_market(OrderBook& actual, ReferenceOrderBook& expected, const Side side, const Quantity quantity)
{
    const MatchResult actual_result = actual.match_market_order(side, quantity);
    const MatchResult expected_result = expected.match_market_order(side, quantity);
    check_match_result(actual_result, expected_result, "match_market_order");
    check_books_equal(actual, expected);
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
    run_market(actual, expected, Side::Buy, 0);
}

void run_seeded_conformance(const std::uint64_t seed)
{
    const BookConfig config{90, 130, 48, 256, 2};
    OrderBook actual(config);
    ReferenceOrderBook expected(config);
    SplitMix64 rng(seed);

    for (std::uint32_t step = 0; step < 2'000U; ++step) {
        const std::uint32_t action = rng.uniform(100U);
        if (action < 50U) {
            run_add(actual,
                    expected,
                    random_order_id(rng),
                    random_side(rng),
                    random_price(rng),
                    random_quantity(rng),
                    static_cast<Timestamp>(step + 1U));
        } else if (action < 68U) {
            run_cancel(actual, expected, random_order_id(rng));
        } else if (action < 86U) {
            run_modify(actual, expected, random_order_id(rng), random_quantity(rng));
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

} // namespace

int main()
{
    test_fifo_and_price_priority();
    test_reduce_keeps_priority_and_increase_rejects();
    test_pool_exhaustion_preserves_resting_liquidity();
    test_order_id_map_full_preflight();
    test_order_id_map_capacity_for_saturates();
    test_invalid_and_unknown_inputs();
    test_seeded_conformance();
    return 0;
}
