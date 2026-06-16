#include "OrderBook.hpp"

#include <cstdio>
#include <cstdlib>

namespace {

using namespace eigenbook;

[[noreturn]] void fail(const char* message)
{
    std::fprintf(stderr, "%s\n", message);
    std::abort();
}

void require(const bool condition, const char* message)
{
    if (!condition) {
        fail(message);
    }
}

const char* side_name(const Side side) noexcept
{
    return side == Side::Buy ? "bid" : "ask";
}

void print_quote(const Side side, const BestQuote quote)
{
    if (!quote.valid) {
        std::printf("best %s: empty\n", side_name(side));
        return;
    }

    std::printf("best %s: price=%lld quantity=%llu orders=%u\n",
                side_name(side),
                static_cast<long long>(quote.price),
                static_cast<unsigned long long>(quote.quantity),
                quote.order_count);
}

} // namespace

int main()
{
    const BookConfig config{
        90,  // min_price
        110, // max_price
        64,  // max_orders
        128, // order_id_map_capacity
        1,   // tick_size
    };

    OrderBook book(config);

    const AddOrderResult bid = book.add_limit_order(1, Side::Buy, 99, 100);
    require(bid.status == Status::Accepted, "failed to add resting bid");

    const AddOrderResult ask = book.add_limit_order(2, Side::Sell, 101, 50);
    require(ask.status == Status::Accepted, "failed to add resting ask");

    print_quote(Side::Buy, book.best_bid());
    print_quote(Side::Sell, book.best_ask());

    const MatchResult match = book.match_market_order(Side::Buy, 30);
    require(match.status == Status::Filled, "failed to match market buy");
    require(match.executed_quantity == 30, "unexpected executed quantity");
    require(match.fills == 1, "unexpected fill count");
    require(match.last_price == 101, "unexpected execution price");

    const CancelResult cancel = book.cancel_order(1);
    require(cancel.status == Status::Cancelled, "failed to cancel resting bid");
    require(cancel.canceled_quantity == 100, "unexpected canceled quantity");

    print_quote(Side::Buy, book.best_bid());
    print_quote(Side::Sell, book.best_ask());
    return 0;
}
