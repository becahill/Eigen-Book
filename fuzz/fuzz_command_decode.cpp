#include "Command.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>

namespace {

using namespace eigenbook;

[[noreturn]] void fail() noexcept
{
    std::abort();
}

[[nodiscard]] bool commands_equal(const Command& lhs, const Command& rhs) noexcept
{
    return lhs.instrument_id == rhs.instrument_id && lhs.op == rhs.op && lhs.order_id == rhs.order_id &&
           lhs.side == rhs.side && lhs.price == rhs.price && lhs.quantity == rhs.quantity &&
           lhs.time_in_force == rhs.time_in_force && lhs.timestamp == rhs.timestamp;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, const std::size_t size)
{
    const auto* const bytes = reinterpret_cast<const std::byte*>(data);
    const std::span<const std::byte> input(bytes, size);

    Command decoded{};
    const Status status = decode(input, decoded);
    if (status == Status::Accepted) {
        std::array<std::byte, kCommandWireSize> encoded{};
        if (encode(decoded, encoded) != Status::Accepted) {
            fail();
        }

        Command round_trip{};
        if (decode(encoded, round_trip) != Status::Accepted || !commands_equal(decoded, round_trip)) {
            fail();
        }
    } else if (status != Status::BufferTooSmall && status != Status::InvalidCommand) {
        fail();
    }

    return 0;
}
