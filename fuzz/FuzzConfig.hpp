#pragma once

#include "OrderBook.hpp"

#include <cstdint>

namespace eigenbook::fuzzing {

inline constexpr std::uint32_t kMaxOrders = 16;
inline constexpr std::uint32_t kEventLogCapacity = 128;

[[nodiscard]] inline BookConfig config_for_mode(const PriceLevelMode mode) noexcept
{
    if (mode == PriceLevelMode::Sparse) {
        return BookConfig{-1'000'000,
                          1'000'000,
                          kMaxOrders,
                          64,
                          5,
                          kEventLogCapacity,
                          PriceLevelMode::Sparse};
    }

    return BookConfig{90, 110, kMaxOrders, 64, 1, kEventLogCapacity, PriceLevelMode::Dense};
}

[[nodiscard]] inline Price seed_bid_price(const PriceLevelMode mode) noexcept
{
    return mode == PriceLevelMode::Sparse ? -100 : 99;
}

[[nodiscard]] inline Price seed_second_bid_price(const PriceLevelMode mode) noexcept
{
    return mode == PriceLevelMode::Sparse ? -500'000 : 98;
}

[[nodiscard]] inline Price seed_ask_price(const PriceLevelMode mode) noexcept
{
    return mode == PriceLevelMode::Sparse ? 250 : 103;
}

[[nodiscard]] inline Price seed_second_ask_price(const PriceLevelMode mode) noexcept
{
    return mode == PriceLevelMode::Sparse ? 750'000 : 105;
}

[[nodiscard]] inline bool populate_seed_book(OrderBook& book, const PriceLevelMode mode) noexcept
{
    return book.add_limit_order(10'001, Side::Buy, seed_bid_price(mode), 7, 1).status == Status::Accepted &&
           book.add_limit_order(10'002, Side::Buy, seed_second_bid_price(mode), 5, 2).status == Status::Accepted &&
           book.add_limit_order(10'003, Side::Sell, seed_ask_price(mode), 4, 3).status == Status::Accepted &&
           book.add_limit_order(10'004, Side::Sell, seed_second_ask_price(mode), 8, 4).status == Status::Accepted;
}

[[nodiscard]] inline bool populate_destination_book(OrderBook& book, const PriceLevelMode mode) noexcept
{
    const Price bid = mode == PriceLevelMode::Sparse ? -750'000 : 95;
    const Price ask = mode == PriceLevelMode::Sparse ? 900'000 : 108;
    return book.add_limit_order(20'001, Side::Buy, bid, 3, 11).status == Status::Accepted &&
           book.add_limit_order(20'002, Side::Sell, ask, 6, 12).status == Status::Accepted;
}

} // namespace eigenbook::fuzzing
