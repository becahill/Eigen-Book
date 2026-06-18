#include "Snapshot.hpp"

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <span>

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

    OrderBook live(config);
    require(live.add_limit_order(1, Side::Buy, 99, 100, 1).status == Status::Accepted,
            "failed to add bid");
    require(live.add_limit_order(2, Side::Sell, 101, 50, 2).status == Status::Accepted,
            "failed to add ask");

    std::array<std::byte, 4096> snapshot_buffer{};
    const SnapshotWriteResult snapshot = serialize(live, snapshot_buffer);
    require(snapshot.status == Status::Accepted, "snapshot buffer was too small");

    OrderBook recovered(config);
    const Status restored =
        restore(recovered, std::span<const std::byte>(snapshot_buffer.data(), snapshot.bytes_written));
    require(restored == Status::Accepted, "failed to restore snapshot");
    require(recovered.last_events().empty(), "restore must not emit book events");

    const AddOrderResult fill = recovered.add_limit_order(3, Side::Buy, 101, 25, 3);
    require(fill.status == Status::Filled, "restored book did not continue trading");
    require(fill.executed_quantity == 25, "unexpected fill quantity after restore");

    std::printf("snapshot bytes=%zu best_ask_qty=%llu\n",
                snapshot.bytes_written,
                static_cast<unsigned long long>(recovered.best_ask().quantity));
    return 0;
}
