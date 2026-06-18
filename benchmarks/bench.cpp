#include "OrderBook.hpp"
#include "Snapshot.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <span>
#include <vector>

namespace {

using namespace eigenbook;
using Clock = std::chrono::steady_clock;

constexpr std::uint32_t kSampleBlockSize = 64;

struct BenchmarkResult final {
    const char* name{nullptr};
    std::uint64_t operations{0};
    double total_time_ms{0.0};
    double operations_per_second{0.0};
    double average_ns{0.0};
    std::uint64_t p50_ns{0};
    std::uint64_t p95_ns{0};
    std::uint64_t p99_ns{0};
};

[[nodiscard]] BookConfig make_config(const std::uint32_t max_orders) noexcept
{
    return BookConfig{1, 1'000, max_orders, max_orders * 4U};
}

[[nodiscard]] BookConfig make_wide_config(const std::uint32_t max_orders,
                                          const PriceLevelMode mode) noexcept
{
    return BookConfig{1, 1'000'000, max_orders, max_orders * 4U, 1, 0, mode};
}

template <typename Fn>
[[nodiscard]] BenchmarkResult run_benchmark(const char* name, const std::uint32_t operations, Fn&& fn)
{
    const std::uint32_t sample_count = (operations + kSampleBlockSize - 1U) / kSampleBlockSize;
    std::vector<std::uint64_t> latencies(sample_count);

    const auto wall_start = Clock::now();
    std::uint32_t operation_index = 0;
    for (std::uint32_t sample = 0; sample < sample_count; ++sample) {
        const std::uint32_t block_begin = operation_index;
        const std::uint32_t block_end = std::min(operations, block_begin + kSampleBlockSize);

        const auto block_start = Clock::now();
        for (; operation_index < block_end; ++operation_index) {
            fn(operation_index);
        }
        const auto block_end_time = Clock::now();

        const auto block_ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(block_end_time - block_start).count());
        latencies[sample] = block_ns / static_cast<std::uint64_t>(block_end - block_begin);
    }
    const auto wall_end = Clock::now();

    std::sort(latencies.begin(), latencies.end());
    const auto wall_ns = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(wall_end - wall_start).count());

    const auto percentile = [&](const double p) noexcept {
        const auto index = static_cast<std::uint32_t>((static_cast<double>(sample_count - 1U)) * p);
        return latencies[index];
    };

    BenchmarkResult result{};
    result.name = name;
    result.operations = operations;
    result.total_time_ms = wall_ns / 1'000'000.0;
    result.operations_per_second = static_cast<double>(operations) / (wall_ns / 1'000'000'000.0);
    result.average_ns = wall_ns / static_cast<double>(operations);
    result.p50_ns = percentile(0.50);
    result.p95_ns = percentile(0.95);
    result.p99_ns = percentile(0.99);
    return result;
}

[[nodiscard]] BenchmarkResult benchmark_add_orders(const std::uint32_t operations)
{
    OrderBook book(make_config(operations + 16U));
    std::vector<OrderId> ids(operations);
    std::vector<Price> prices(operations);

    for (std::uint32_t i = 0; i < operations; ++i) {
        ids[i] = static_cast<OrderId>(i + 1U);
        prices[i] = 100 + static_cast<Price>(i % 50U);
    }

    return run_benchmark("Add N limit orders", operations, [&](const std::uint32_t i) {
        const AddOrderResult result = book.add_limit_order(ids[i], Side::Buy, prices[i], 100);
        if (result.status != Status::Accepted) {
            std::abort();
        }
    });
}

[[nodiscard]] BenchmarkResult benchmark_dense_wide_sparse_prices(const std::uint32_t operations)
{
    OrderBook book(make_wide_config(operations + 16U, PriceLevelMode::Dense));
    constexpr std::array<Price, 20> prices{
        10,
        50'000,
        100'000,
        150'000,
        200'000,
        250'000,
        300'000,
        350'000,
        400'000,
        450'000,
        500'000,
        550'000,
        600'000,
        650'000,
        700'000,
        750'000,
        800'000,
        850'000,
        900'000,
        950'000,
    };

    return run_benchmark("Dense wide 20 prices", operations, [&](const std::uint32_t i) {
        const AddOrderResult result =
            book.add_limit_order(static_cast<OrderId>(40'000'000ULL + i), Side::Buy, prices[i % prices.size()], 100);
        if (result.status != Status::Accepted) {
            std::abort();
        }
    });
}

[[nodiscard]] BenchmarkResult benchmark_sparse_wide_sparse_prices(const std::uint32_t operations)
{
    OrderBook book(make_wide_config(operations + 16U, PriceLevelMode::Sparse));
    constexpr std::array<Price, 20> prices{
        10,
        50'000,
        100'000,
        150'000,
        200'000,
        250'000,
        300'000,
        350'000,
        400'000,
        450'000,
        500'000,
        550'000,
        600'000,
        650'000,
        700'000,
        750'000,
        800'000,
        850'000,
        900'000,
        950'000,
    };

    return run_benchmark("Sparse wide 20 prices", operations, [&](const std::uint32_t i) {
        const AddOrderResult result =
            book.add_limit_order(static_cast<OrderId>(50'000'000ULL + i), Side::Buy, prices[i % prices.size()], 100);
        if (result.status != Status::Accepted) {
            std::abort();
        }
    });
}

[[nodiscard]] BenchmarkResult benchmark_cancel_orders(const std::uint32_t operations)
{
    OrderBook book(make_config(operations + 16U));
    std::vector<OrderId> ids(operations);

    for (std::uint32_t i = 0; i < operations; ++i) {
        ids[i] = static_cast<OrderId>(i + 1U);
        const AddOrderResult result = book.add_limit_order(ids[i], Side::Buy, 100, 100);
        if (result.status != Status::Accepted) {
            std::abort();
        }
    }

    return run_benchmark("Cancel N orders", operations, [&](const std::uint32_t i) {
        const CancelResult result = book.cancel_order(ids[i]);
        if (result.status != Status::Cancelled) {
            std::abort();
        }
    });
}

[[nodiscard]] BenchmarkResult benchmark_modify_orders(const std::uint32_t operations)
{
    OrderBook book(make_config(operations + 16U));
    std::vector<OrderId> ids(operations);

    for (std::uint32_t i = 0; i < operations; ++i) {
        ids[i] = static_cast<OrderId>(i + 1U);
        const AddOrderResult result = book.add_limit_order(ids[i], Side::Buy, 100, 100);
        if (result.status != Status::Accepted) {
            std::abort();
        }
    }

    return run_benchmark("Modify N orders", operations, [&](const std::uint32_t i) {
        const ModifyResult result = book.modify_order(ids[i], 99);
        if (result.status != Status::Accepted) {
            std::abort();
        }
    });
}

[[nodiscard]] BenchmarkResult benchmark_replace_orders(const std::uint32_t operations)
{
    OrderBook book(make_config(operations + 16U));
    std::vector<OrderId> ids(operations);

    for (std::uint32_t i = 0; i < operations; ++i) {
        ids[i] = static_cast<OrderId>(i + 1U);
        const AddOrderResult result = book.add_limit_order(ids[i], Side::Buy, 100, 100);
        if (result.status != Status::Accepted) {
            std::abort();
        }
    }

    return run_benchmark("Replace N orders", operations, [&](const std::uint32_t i) {
        const ReplaceResult result = book.replace_order(ids[i], 101, 100);
        if (result.status != Status::Accepted) {
            std::abort();
        }
    });
}

[[nodiscard]] BenchmarkResult benchmark_market_matches(const std::uint32_t operations)
{
    OrderBook book(make_config(operations + 16U));

    for (std::uint32_t i = 0; i < operations; ++i) {
        const AddOrderResult result = book.add_limit_order(static_cast<OrderId>(i + 1U), Side::Sell, 100, 1);
        if (result.status != Status::Accepted) {
            std::abort();
        }
    }

    return run_benchmark("Match market orders", operations, [&](const std::uint32_t) {
        const MatchResult result = book.match_market_order(Side::Buy, 1);
        if (result.status != Status::Filled) {
            std::abort();
        }
    });
}

[[nodiscard]] BenchmarkResult benchmark_ioc_partial_matches(const std::uint32_t operations)
{
    OrderBook book(make_config(operations + 16U));

    for (std::uint32_t i = 0; i < operations; ++i) {
        const AddOrderResult result = book.add_limit_order(static_cast<OrderId>(i + 1U), Side::Sell, 100, 1);
        if (result.status != Status::Accepted) {
            std::abort();
        }
    }

    return run_benchmark("IOC partial matches", operations, [&](const std::uint32_t i) {
        const AddOrderResult result =
            book.add_limit_order(10'000'000ULL + i, Side::Buy, 100, 2, 0, TimeInForce::Ioc);
        if (result.status != Status::PartiallyFilled || result.resting_quantity != 0) {
            std::abort();
        }
    });
}

[[nodiscard]] BenchmarkResult benchmark_fok_rejects(const std::uint32_t operations)
{
    OrderBook book(make_config(16U));

    return run_benchmark("FOK rejects", operations, [&](const std::uint32_t i) {
        const AddOrderResult result =
            book.add_limit_order(20'000'000ULL + i, Side::Buy, 100, 1, 0, TimeInForce::Fok);
        if (result.status != Status::FokRejected) {
            std::abort();
        }
    });
}

[[nodiscard]] BenchmarkResult benchmark_fok_accepts(const std::uint32_t operations)
{
    OrderBook book(make_config(operations + 16U));

    for (std::uint32_t i = 0; i < operations; ++i) {
        const AddOrderResult result = book.add_limit_order(static_cast<OrderId>(i + 1U), Side::Sell, 100, 1);
        if (result.status != Status::Accepted) {
            std::abort();
        }
    }

    return run_benchmark("FOK full matches", operations, [&](const std::uint32_t i) {
        const AddOrderResult result =
            book.add_limit_order(30'000'000ULL + i, Side::Buy, 100, 1, 0, TimeInForce::Fok);
        if (result.status != Status::Filled || result.resting_quantity != 0) {
            std::abort();
        }
    });
}

enum class EventKind : std::uint8_t {
    Add,
    Cancel,
    Modify,
    Execute,
};

struct Event final {
    EventKind kind{EventKind::Add};
    OrderId id{0};
    Price price{0};
    Quantity quantity{0};
};

[[nodiscard]] BenchmarkResult benchmark_mixed_workload(const std::uint32_t operations)
{
    constexpr std::uint32_t pattern = 20;
    const std::uint32_t add_count = operations / 2U;
    const std::uint32_t cancel_count = operations / 4U;
    const std::uint32_t modify_count = (operations * 15U) / 100U;
    const std::uint32_t execute_count = operations - add_count - cancel_count - modify_count;

    OrderBook book(make_config(operations + 64U));
    std::vector<Event> events(operations);

    std::uint32_t add_index = 0;
    std::uint32_t cancel_index = 0;
    std::uint32_t modify_index = 0;
    std::uint32_t execute_index = 0;

    for (std::uint32_t i = 0; i < cancel_count; ++i) {
        const OrderId id = 10'000'000ULL + i;
        const AddOrderResult result = book.add_limit_order(id, Side::Buy, 90, 100);
        if (result.status != Status::Accepted) {
            std::abort();
        }
    }

    for (std::uint32_t i = 0; i < modify_count; ++i) {
        const OrderId id = 20'000'000ULL + i;
        const AddOrderResult result = book.add_limit_order(id, Side::Buy, 91, 100);
        if (result.status != Status::Accepted) {
            std::abort();
        }
    }

    for (std::uint32_t i = 0; i < execute_count; ++i) {
        const OrderId id = 30'000'000ULL + i;
        const AddOrderResult result = book.add_limit_order(id, Side::Sell, 100, 1);
        if (result.status != Status::Accepted) {
            std::abort();
        }
    }

    for (std::uint32_t i = 0; i < operations; ++i) {
        const std::uint32_t slot = i % pattern;
        Event& event = events[i];
        if (slot < 10U) {
            event = Event{EventKind::Add, 40'000'000ULL + add_index, 92, 100};
            ++add_index;
        } else if (slot < 15U) {
            event = Event{EventKind::Cancel, 10'000'000ULL + cancel_index, 0, 0};
            ++cancel_index;
        } else if (slot < 18U) {
            event = Event{EventKind::Modify, 20'000'000ULL + modify_index, 0, 99};
            ++modify_index;
        } else {
            event = Event{EventKind::Execute, 30'000'000ULL + execute_index, 0, 1};
            ++execute_index;
        }
    }

    return run_benchmark("Mixed 50/25/15/10 workload", operations, [&](const std::uint32_t i) {
        const Event& event = events[i];
        switch (event.kind) {
        case EventKind::Add: {
            const AddOrderResult result = book.add_limit_order(event.id, Side::Buy, event.price, event.quantity);
            if (result.status != Status::Accepted) {
                std::abort();
            }
            break;
        }
        case EventKind::Cancel: {
            const CancelResult result = book.cancel_order(event.id);
            if (result.status != Status::Cancelled) {
                std::abort();
            }
            break;
        }
        case EventKind::Modify: {
            const ModifyResult result = book.modify_order(event.id, event.quantity);
            if (result.status != Status::Accepted) {
                std::abort();
            }
            break;
        }
        case EventKind::Execute: {
            const MatchResult result = book.match_market_order(Side::Buy, event.quantity);
            if (result.status != Status::Filled) {
                std::abort();
            }
            break;
        }
        }
    });
}

[[nodiscard]] BenchmarkResult benchmark_snapshot_serialize(const std::uint32_t operations)
{
    constexpr std::uint32_t live_orders = 256;
    OrderBook book(make_config(live_orders + 16U));
    std::vector<std::byte> buffer(64U * 1024U);

    for (std::uint32_t i = 0; i < live_orders; ++i) {
        const Side side = i % 2U == 0U ? Side::Buy : Side::Sell;
        const Price price = side == Side::Buy ? 100 + static_cast<Price>(i % 20U)
                                              : 200 + static_cast<Price>(i % 20U);
        const AddOrderResult result = book.add_limit_order(static_cast<OrderId>(i + 1U), side, price, 100);
        if (result.status != Status::Accepted) {
            std::abort();
        }
    }

    return run_benchmark("Serialize book snapshot", operations, [&](const std::uint32_t) {
        const SnapshotWriteResult result = serialize(book, buffer);
        if (result.status != Status::Accepted) {
            std::abort();
        }
    });
}

[[nodiscard]] BenchmarkResult benchmark_snapshot_restore(const std::uint32_t operations)
{
    constexpr std::uint32_t live_orders = 256;
    const BookConfig config = make_config(live_orders + 16U);
    OrderBook source(config);
    OrderBook target(config);
    std::vector<std::byte> buffer(64U * 1024U);

    for (std::uint32_t i = 0; i < live_orders; ++i) {
        const Side side = i % 2U == 0U ? Side::Buy : Side::Sell;
        const Price price = side == Side::Buy ? 100 + static_cast<Price>(i % 20U)
                                              : 200 + static_cast<Price>(i % 20U);
        const AddOrderResult result = source.add_limit_order(static_cast<OrderId>(i + 1U), side, price, 100);
        if (result.status != Status::Accepted) {
            std::abort();
        }
    }

    const SnapshotWriteResult snapshot = serialize(source, buffer);
    if (snapshot.status != Status::Accepted) {
        std::abort();
    }

    return run_benchmark("Restore book snapshot", operations, [&](const std::uint32_t) {
        const Status status = restore(target, std::span<const std::byte>(buffer.data(), snapshot.bytes_written));
        if (status != Status::Accepted) {
            std::abort();
        }
    });
}

void print_result(const BenchmarkResult& result)
{
    std::printf("| %-29s | %10llu | %10.3f | %14.0f | %8.1f | %6llu | %6llu | %6llu |\n",
                result.name,
                static_cast<unsigned long long>(result.operations),
                result.total_time_ms,
                result.operations_per_second,
                result.average_ns,
                static_cast<unsigned long long>(result.p50_ns),
                static_cast<unsigned long long>(result.p95_ns),
                static_cast<unsigned long long>(result.p99_ns));
}

} // namespace

int main()
{
    constexpr std::uint32_t operations = 50'000;

    const BenchmarkResult add = benchmark_add_orders(operations);
    const BenchmarkResult dense_wide = benchmark_dense_wide_sparse_prices(operations);
    const BenchmarkResult sparse_wide = benchmark_sparse_wide_sparse_prices(operations);
    const BenchmarkResult cancel = benchmark_cancel_orders(operations);
    const BenchmarkResult modify = benchmark_modify_orders(operations);
    const BenchmarkResult replace = benchmark_replace_orders(operations);
    const BenchmarkResult match = benchmark_market_matches(operations);
    const BenchmarkResult ioc = benchmark_ioc_partial_matches(operations);
    const BenchmarkResult fok_reject = benchmark_fok_rejects(operations);
    const BenchmarkResult fok_accept = benchmark_fok_accepts(operations);
    const BenchmarkResult mixed = benchmark_mixed_workload(operations);
    const BenchmarkResult snapshot_serialize = benchmark_snapshot_serialize(operations);
    const BenchmarkResult snapshot_restore = benchmark_snapshot_restore(operations);

    std::printf("Eigen-Book microbenchmarks (%u operations per scenario)\n", operations);
    std::printf("| Scenario                      | Operations | Total ms   | Ops/sec        | Avg ns   | p50 ns | p95 ns | p99 ns |\n");
    std::printf("|-------------------------------|------------|------------|----------------|----------|--------|--------|--------|\n");
    print_result(add);
    print_result(dense_wide);
    print_result(sparse_wide);
    print_result(cancel);
    print_result(modify);
    print_result(replace);
    print_result(match);
    print_result(ioc);
    print_result(fok_reject);
    print_result(fok_accept);
    print_result(mixed);
    print_result(snapshot_serialize);
    print_result(snapshot_restore);
    return 0;
}
