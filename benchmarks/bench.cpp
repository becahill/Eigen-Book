#include "BenchmarkBuildInfo.hpp"
#include "Command.hpp"
#include "MatchingEngine.hpp"
#include "OrderBook.hpp"
#include "Snapshot.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <limits>
#include <span>
#include <vector>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

#if defined(__unix__) || defined(__APPLE__)
#include <sys/utsname.h>
#endif

namespace {

using namespace eigenbook;
using Clock = std::chrono::steady_clock;

constexpr std::uint32_t kSampleBlockSize = 64;
constexpr std::uint32_t kDefaultOperations = 50'000;
constexpr std::uint32_t kDefaultIterations = 1;
constexpr std::uint32_t kMaximumOperations = 1'000'000;
constexpr std::uint32_t kSnapshotLiveOrders = 256;
constexpr std::uint32_t kSnapshotRestoreCapacity = 4'096;
constexpr std::uint32_t kSnapshotRestorePriceLevels = kSnapshotRestoreCapacity * 2U;
constexpr std::array<std::uint32_t, 4> kSnapshotRestoreSizes{64, 256, 1'024, 4'096};
constexpr std::array<const char*, 4> kSnapshotRestoreNames{
    "Restore snapshot (64 orders)",
    "Restore snapshot (256 orders)",
    "Restore snapshot (1024 orders)",
    "Restore snapshot (4096 orders)",
};
constexpr std::uint32_t kWidePriceCount = 20;

struct BenchmarkOptions final {
    std::uint32_t operations{kDefaultOperations};
    std::uint32_t iterations{kDefaultIterations};
};

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

[[nodiscard]] bool parse_positive_u32(const char* const text, std::uint32_t& value) noexcept
{
    if (text == nullptr || text[0] == '\0' || text[0] == '-') {
        return false;
    }

    errno = 0;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed == 0 ||
        parsed > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }

    value = static_cast<std::uint32_t>(parsed);
    return true;
}

[[nodiscard]] bool parse_options(const int argc,
                                 char* const argv[],
                                 BenchmarkOptions& options) noexcept
{
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--operations") == 0) {
            if (index + 1 >= argc || !parse_positive_u32(argv[index + 1], options.operations)) {
                return false;
            }
            ++index;
            continue;
        }

        if (std::strcmp(argv[index], "--iterations") == 0) {
            if (index + 1 >= argc || !parse_positive_u32(argv[index + 1], options.iterations)) {
                return false;
            }
            ++index;
            continue;
        }

        return false;
    }

    return options.operations <= kMaximumOperations && options.operations % 20U == 0U &&
           options.iterations <= 100U;
}

void print_usage(const char* const program)
{
    std::fprintf(stderr,
                 "Usage: %s [--operations N] [--iterations N]\n"
                 "  operations must be a multiple of 20 in [20, %u]\n"
                 "  iterations must be in [1, 100]\n",
                 program,
                 kMaximumOperations);
}

void read_utc_timestamp(char* const output, const std::size_t capacity) noexcept
{
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
#if defined(_WIN32)
    if (gmtime_s(&utc, &now) != 0) {
        std::snprintf(output, capacity, "unavailable");
        return;
    }
#else
    if (gmtime_r(&now, &utc) == nullptr) {
        std::snprintf(output, capacity, "unavailable");
        return;
    }
#endif
    if (std::strftime(output, capacity, "%Y-%m-%dT%H:%M:%SZ", &utc) == 0) {
        std::snprintf(output, capacity, "unavailable");
    }
}

void read_cpu_model(char* const output, const std::size_t capacity) noexcept
{
    std::snprintf(output, capacity, "unavailable");
#if defined(__APPLE__)
    std::size_t size = capacity;
    if (sysctlbyname("machdep.cpu.brand_string", output, &size, nullptr, 0) != 0) {
        std::snprintf(output, capacity, "unavailable");
    }
#elif defined(__linux__)
    std::FILE* const cpuinfo = std::fopen("/proc/cpuinfo", "r");
    if (cpuinfo == nullptr) {
        return;
    }

    char line[512]{};
    while (std::fgets(line, static_cast<int>(sizeof(line)), cpuinfo) != nullptr) {
        if (std::strncmp(line, "model name", 10) != 0 && std::strncmp(line, "Hardware", 8) != 0) {
            continue;
        }

        const char* value = std::strchr(line, ':');
        if (value == nullptr) {
            continue;
        }
        ++value;
        while (*value == ' ' || *value == '\t') {
            ++value;
        }
        const std::size_t length = std::strcspn(value, "\r\n");
        std::snprintf(output, capacity, "%.*s", static_cast<int>(length), value);
        break;
    }
    std::fclose(cpuinfo);
#endif
}

void print_run_context(const BenchmarkOptions& options)
{
    char timestamp[32]{};
    char cpu_model[256]{};
    read_utc_timestamp(timestamp, sizeof(timestamp));
    read_cpu_model(cpu_model, sizeof(cpu_model));

    std::printf("Eigen-Book benchmark run context\n");
    std::printf("Timestamp (UTC): %s\n", timestamp);
    std::printf("CPU: %s\n", cpu_model);
#if defined(__unix__) || defined(__APPLE__)
    utsname system{};
    if (uname(&system) == 0) {
        std::printf("OS: %s %s (%s)\n", system.sysname, system.release, system.machine);
    } else {
        std::printf("OS: unavailable\n");
    }
#else
    std::printf("OS: unavailable\n");
#endif
    std::printf("Compiler: %s %s\n",
                benchmark_build::kCompilerId,
                benchmark_build::kCompilerVersion);
#if defined(__clang__)
    std::printf("Compiler build: %s\n", __clang_version__);
#elif defined(__GNUC__)
    std::printf("Compiler build: %s\n", __VERSION__);
#elif defined(_MSC_VER)
    std::printf("Compiler build: MSVC %d\n", _MSC_VER);
#endif
    std::printf("Compiler path: %s\n", benchmark_build::kCompilerPath);
    std::printf("Build type: %s\n", benchmark_build::kBuildType);
    std::printf("Optimization flags: %s\n", benchmark_build::kOptimizationFlags);
    std::printf("CMake: %s (%s)\n",
                benchmark_build::kCMakeVersion,
                benchmark_build::kCMakeGenerator);
    std::printf("Workload iterations: %u\n", options.iterations);
    std::printf("Operations per workload iteration: %u operations\n", options.operations);
    std::printf("Latency sampling: %u-operation blocks\n", kSampleBlockSize);
    std::printf("Wide-price workload: %u occupied prices over [1, 1000000]\n", kWidePriceCount);
    std::printf("Snapshot serialize workload: %u live orders\n", kSnapshotLiveOrders);
    std::printf("Snapshot restore workloads: 64, 256, 1024, and 4096 live orders; one occupied level per order\n");
    std::printf("Snapshot restore configuration: dense [1, 8192], 4096 order slots, 8192 id-map slots\n");
    std::printf("Units: total=milliseconds, throughput=operations/second, latency=nanoseconds/operation\n\n");
}

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

[[nodiscard]] BenchmarkResult benchmark_add_with_venue_checks(const std::uint32_t operations)
{
    BookConfig config = make_config(operations + 16U);
    config.lot_size = 10;
    config.self_trade_policy = SelfTradePolicy::CancelResting;
    OrderBook book(config);
    std::vector<OrderId> ids(operations);
    std::vector<Price> prices(operations);
    for (std::uint32_t i = 0; i < operations; ++i) {
        ids[i] = static_cast<OrderId>(i + 1U);
        prices[i] = 100 + static_cast<Price>(i % 50U);
    }

    return run_benchmark("Add with venue checks", operations, [&](const std::uint32_t i) {
        const AddOrderResult result = book.add_limit_order(ids[i],
                                                           Side::Buy,
                                                           prices[i],
                                                           100,
                                                           i,
                                                           TimeInForce::Gtc,
                                                           static_cast<ParticipantId>(i + 1U));
        if (result.status != Status::Accepted) {
            std::abort();
        }
    });
}

[[nodiscard]] BenchmarkResult benchmark_add_with_market_data(const std::uint32_t operations)
{
    BookConfig config = make_config(operations + 16U);
    config.market_data_capacity = 4;
    OrderBook book(config, 1U);
    std::vector<OrderId> ids(operations);
    std::vector<Price> prices(operations);
    for (std::uint32_t i = 0; i < operations; ++i) {
        ids[i] = static_cast<OrderId>(i + 1U);
        prices[i] = 100 + static_cast<Price>(i % 50U);
    }

    return run_benchmark("Add with market data", operations, [&](const std::uint32_t i) {
        const AddOrderResult result =
            book.add_limit_order(ids[i], Side::Buy, prices[i], 100, i);
        if (result.status != Status::Accepted || book.last_market_data_events().empty()) {
            std::abort();
        }
    });
}

[[nodiscard]] BenchmarkResult benchmark_dense_wide_sparse_prices(const std::uint32_t operations)
{
    OrderBook book(make_wide_config(operations + 16U, PriceLevelMode::Dense));
    constexpr std::array<Price, kWidePriceCount> prices{
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
    constexpr std::array<Price, kWidePriceCount> prices{
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
    const BookConfig config{1, static_cast<Price>(operations + 100U), operations + 16U, (operations + 16U) * 4U};
    OrderBook book(config);

    for (std::uint32_t i = 0; i < operations; ++i) {
        const AddOrderResult result =
            book.add_limit_order(static_cast<OrderId>(i + 1U), Side::Sell, static_cast<Price>(100U + i), 1);
        if (result.status != Status::Accepted) {
            std::abort();
        }
    }

    return run_benchmark("IOC partial matches", operations, [&](const std::uint32_t i) {
        const AddOrderResult result =
            book.add_limit_order(10'000'000ULL + i, Side::Buy, static_cast<Price>(100U + i), 2, 0, TimeInForce::Ioc);
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

[[nodiscard]] BenchmarkResult benchmark_replay_dispatch(const std::uint32_t operations)
{
    constexpr InstrumentId instrument_id = 1;
    constexpr std::uint32_t pattern = 20;
    const std::uint32_t add_count = (operations * 8U) / pattern;
    const std::uint32_t cancel_count = (operations * 4U) / pattern;
    const std::uint32_t modify_count = (operations * 3U) / pattern;
    const std::uint32_t replace_count = (operations * 3U) / pattern;
    const std::uint32_t execute_count = operations - add_count - cancel_count - modify_count - replace_count;
    const BookConfig config = make_config(operations + 256U);
    const InstrumentConfig instruments[] = {
        InstrumentConfig{instrument_id, config},
    };
    MatchingEngine engine(instruments);
    std::vector<std::array<std::byte, kCommandWireSize>> encoded(operations);
    std::vector<Status> expected_statuses(operations);

    for (std::uint32_t i = 0; i < cancel_count; ++i) {
        const AddOrderResult result =
            engine.add_limit_order(instrument_id, static_cast<OrderId>(10'000'000ULL + i), Side::Buy, 90, 100);
        if (result.status != Status::Accepted) {
            std::abort();
        }
    }

    for (std::uint32_t i = 0; i < modify_count; ++i) {
        const AddOrderResult result =
            engine.add_limit_order(instrument_id, static_cast<OrderId>(20'000'000ULL + i), Side::Buy, 91, 100);
        if (result.status != Status::Accepted) {
            std::abort();
        }
    }

    for (std::uint32_t i = 0; i < replace_count; ++i) {
        const AddOrderResult result =
            engine.add_limit_order(instrument_id, static_cast<OrderId>(30'000'000ULL + i), Side::Buy, 92, 100);
        if (result.status != Status::Accepted) {
            std::abort();
        }
    }

    for (std::uint32_t i = 0; i < execute_count; ++i) {
        const AddOrderResult result =
            engine.add_limit_order(instrument_id, static_cast<OrderId>(40'000'000ULL + i), Side::Sell, 100, 1);
        if (result.status != Status::Accepted) {
            std::abort();
        }
    }

    std::uint32_t add_index = 0;
    std::uint32_t cancel_index = 0;
    std::uint32_t modify_index = 0;
    std::uint32_t replace_index = 0;
    std::uint32_t execute_index = 0;
    for (std::uint32_t i = 0; i < operations; ++i) {
        const std::uint32_t slot = i % pattern;
        Command command{};
        command.instrument_id = instrument_id;
        command.side = Side::Buy;
        command.time_in_force = TimeInForce::Gtc;
        command.timestamp = i + 1U;

        if (slot < 8U) {
            command.op = CommandOp::Add;
            command.order_id = static_cast<OrderId>(50'000'000ULL + add_index);
            command.price = 93;
            command.quantity = 100;
            expected_statuses[i] = Status::Accepted;
            ++add_index;
        } else if (slot < 12U) {
            command.op = CommandOp::Cancel;
            command.order_id = static_cast<OrderId>(10'000'000ULL + cancel_index);
            expected_statuses[i] = Status::Cancelled;
            ++cancel_index;
        } else if (slot < 15U) {
            command.op = CommandOp::Modify;
            command.order_id = static_cast<OrderId>(20'000'000ULL + modify_index);
            command.quantity = 99;
            expected_statuses[i] = Status::Accepted;
            ++modify_index;
        } else if (slot < 18U) {
            command.op = CommandOp::Replace;
            command.order_id = static_cast<OrderId>(30'000'000ULL + replace_index);
            command.price = 94;
            command.quantity = 100;
            expected_statuses[i] = Status::Accepted;
            ++replace_index;
        } else {
            command.op = CommandOp::Market;
            command.order_id = static_cast<OrderId>(60'000'000ULL + execute_index);
            command.side = Side::Buy;
            command.quantity = 1;
            expected_statuses[i] = Status::Filled;
            ++execute_index;
        }

        if (encode(command, encoded[i]) != Status::Accepted) {
            std::abort();
        }
    }

    return run_benchmark("Replay dispatch commands", operations, [&](const std::uint32_t i) {
        const DispatchResult result = engine.dispatch(
            std::span<const std::byte>(encoded[i].data(), encoded[i].size()));
        if (result.status != expected_statuses[i]) {
            std::abort();
        }
    });
}

[[nodiscard]] BenchmarkResult benchmark_snapshot_serialize(const std::uint32_t operations)
{
    OrderBook book(make_config(kSnapshotLiveOrders + 16U));
    std::vector<std::byte> buffer(64U * 1024U);

    for (std::uint32_t i = 0; i < kSnapshotLiveOrders; ++i) {
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

[[nodiscard]] std::uint32_t snapshot_restore_operations(const std::uint32_t operations,
                                                        const std::uint32_t order_count) noexcept
{
    constexpr std::uint64_t reference_orders = 256;
    constexpr std::uint64_t minimum_operations = 10;
    const std::uint64_t reference_operations = std::max<std::uint64_t>(1U, operations / 10U);
    const std::uint64_t scaled =
        (reference_operations * reference_orders * reference_orders) /
        (static_cast<std::uint64_t>(order_count) * static_cast<std::uint64_t>(order_count));
    return static_cast<std::uint32_t>(std::max(minimum_operations, scaled));
}

[[nodiscard]] BenchmarkResult benchmark_snapshot_restore(const char* const name,
                                                         const std::uint32_t operations,
                                                         const std::uint32_t order_count)
{
    const BookConfig config{
        1,
        static_cast<Price>(kSnapshotRestorePriceLevels),
        kSnapshotRestoreCapacity,
        kSnapshotRestoreCapacity * 2U,
    };
    OrderBook source(config);
    OrderBook target(config);
    std::vector<std::byte> buffer(
        detail::kBookHeaderWireSize +
        static_cast<std::size_t>(order_count) *
            (detail::kBookOrderWireSize + detail::kLevelWireSize));

    const std::uint32_t bid_count = order_count / 2U;
    for (std::uint32_t i = 0; i < order_count; ++i) {
        const Side side = i < bid_count ? Side::Buy : Side::Sell;
        const Price price =
            side == Side::Buy
                ? static_cast<Price>(i + 1U)
                : static_cast<Price>(kSnapshotRestoreCapacity + (i - bid_count) + 1U);
        const AddOrderResult result = source.add_limit_order(static_cast<OrderId>(i + 1U), side, price, 100);
        if (result.status != Status::Accepted) {
            std::abort();
        }
    }

    const SnapshotWriteResult snapshot = serialize(source, buffer);
    if (snapshot.status != Status::Accepted) {
        std::abort();
    }

    return run_benchmark(name, operations, [&](const std::uint32_t) {
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

int main(const int argc, char* const argv[])
{
    BenchmarkOptions options{};
    if (!parse_options(argc, argv, options)) {
        print_usage(argv[0]);
        return 2;
    }

    print_run_context(options);
    for (std::uint32_t iteration = 0; iteration < options.iterations; ++iteration) {
        const BenchmarkResult add = benchmark_add_orders(options.operations);
        const BenchmarkResult venue_checks = benchmark_add_with_venue_checks(options.operations);
        const BenchmarkResult market_data = benchmark_add_with_market_data(options.operations);
        const BenchmarkResult dense_wide = benchmark_dense_wide_sparse_prices(options.operations);
        const BenchmarkResult sparse_wide = benchmark_sparse_wide_sparse_prices(options.operations);
        const BenchmarkResult cancel = benchmark_cancel_orders(options.operations);
        const BenchmarkResult modify = benchmark_modify_orders(options.operations);
        const BenchmarkResult replace = benchmark_replace_orders(options.operations);
        const BenchmarkResult match = benchmark_market_matches(options.operations);
        const BenchmarkResult ioc = benchmark_ioc_partial_matches(options.operations);
        const BenchmarkResult fok_reject = benchmark_fok_rejects(options.operations);
        const BenchmarkResult fok_accept = benchmark_fok_accepts(options.operations);
        const BenchmarkResult mixed = benchmark_mixed_workload(options.operations);
        const BenchmarkResult replay_dispatch = benchmark_replay_dispatch(options.operations);
        const BenchmarkResult snapshot_serialize = benchmark_snapshot_serialize(options.operations);
        std::array<BenchmarkResult, kSnapshotRestoreSizes.size()> snapshot_restores{};
        for (std::size_t index = 0; index < kSnapshotRestoreSizes.size(); ++index) {
            const std::uint32_t order_count = kSnapshotRestoreSizes[index];
            snapshot_restores[index] =
                benchmark_snapshot_restore(kSnapshotRestoreNames[index],
                                           snapshot_restore_operations(options.operations, order_count),
                                           order_count);
        }

        std::printf("Eigen-Book microbenchmarks (iteration %u/%u, %u operations per workload)\n",
                    iteration + 1U,
                    options.iterations,
                    options.operations);
        std::printf("| Scenario                      | Operations | Total ms   | Ops/sec        | Avg ns   | p50 ns | p95 ns | p99 ns |\n");
        std::printf("|-------------------------------|------------|------------|----------------|----------|--------|--------|--------|\n");
        print_result(add);
        print_result(venue_checks);
        print_result(market_data);
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
        print_result(replay_dispatch);
        print_result(snapshot_serialize);
        for (const BenchmarkResult& snapshot_restore : snapshot_restores) {
            print_result(snapshot_restore);
        }
        std::printf("\n");
    }
    return 0;
}
