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

    const ReplaceResult replace = book.replace_order(1, 100, 80);
    require(replace.status == Status::Accepted, "failed to replace resting bid");
    require(replace.old_price == 99, "unexpected replaced price");
    require(replace.resting_quantity == 80, "unexpected replaced quantity");

    const MatchResult match = book.match_market_order(Side::Buy, 30);
    require(match.status == Status::Filled, "failed to match market buy");
    require(match.executed_quantity == 30, "unexpected executed quantity");
    require(match.fills == 1, "unexpected fill count");
    require(match.last_price == 101, "unexpected execution price");

    const CancelResult cancel = book.cancel_order(1);
    require(cancel.status == Status::Cancelled, "failed to cancel resting bid");
    require(cancel.canceled_quantity == 80, "unexpected canceled quantity");

    const AddOrderResult fok = book.add_limit_order(3, Side::Buy, 101, 20, 1, TimeInForce::Fok);
    require(fok.status == Status::Filled, "failed to fill FOK order");
    require(fok.resting_quantity == 0, "FOK order should not rest");

    const AddOrderResult fresh_ask = book.add_limit_order(4, Side::Sell, 102, 10);
    require(fresh_ask.status == Status::Accepted, "failed to add fresh ask");

    const AddOrderResult ioc = book.add_limit_order(5, Side::Buy, 102, 15, 2, TimeInForce::Ioc);
    require(ioc.status == Status::PartiallyFilled, "failed to partially fill IOC order");
    require(ioc.executed_quantity == 10, "unexpected IOC executed quantity");
    require(ioc.resting_quantity == 0, "IOC order should not rest");

    print_quote(Side::Buy, book.best_bid());
    print_quote(Side::Sell, book.best_ask());
    return 0;
}
