#include "Command.hpp"
#include "FuzzConfig.hpp"
#include "Snapshot.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>

namespace {

using namespace eigenbook;

template <std::size_t Size>
[[nodiscard]] bool write_bytes(const std::filesystem::path& path,
                               const std::array<std::byte, Size>& bytes,
                               const std::size_t count) noexcept
{
    if (count > bytes.size()) {
        return false;
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return false;
    }
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(count));
    return output.good();
}

[[nodiscard]] bool write_command_seed(const std::filesystem::path& path, const Command& command) noexcept
{
    std::array<std::byte, kCommandWireSize> bytes{};
    return encode(command, bytes) == Status::Accepted && write_bytes(path, bytes, bytes.size());
}

[[nodiscard]] bool write_snapshot_seed(const std::filesystem::path& path,
                                       const PriceLevelMode mode) noexcept
{
    constexpr std::size_t kBufferSize = 4096;
    OrderBook book(fuzzing::config_for_mode(mode));
    if (!fuzzing::populate_seed_book(book, mode)) {
        return false;
    }

    std::array<std::byte, kBufferSize> snapshot{};
    const SnapshotWriteResult result = serialize(book, snapshot);
    if (result.status != Status::Accepted || result.bytes_written + 1U > snapshot.size()) {
        return false;
    }

    std::array<std::byte, kBufferSize> input{};
    input[0] = mode == PriceLevelMode::Dense ? std::byte{0} : std::byte{1};
    for (std::size_t index = 0; index < result.bytes_written; ++index) {
        input[index + 1U] = snapshot[index];
    }
    return write_bytes(path, input, result.bytes_written + 1U);
}

template <std::size_t Size>
[[nodiscard]] bool write_stateful_seed(
    const std::filesystem::path& path,
    const std::array<std::uint8_t, Size>& bytes) noexcept
{
    std::array<std::byte, Size> converted{};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        converted[index] = static_cast<std::byte>(bytes[index]);
    }
    return write_bytes(path, converted, converted.size());
}

} // namespace

int main(const int argc, const char* const argv[])
{
    if (argc != 2) {
        return 2;
    }

    const std::filesystem::path root(argv[1]);
    std::error_code error;
    std::filesystem::create_directories(root / "command", error);
    if (error) {
        return 3;
    }
    std::filesystem::create_directories(root / "snapshot", error);
    if (error) {
        return 3;
    }
    std::filesystem::create_directories(root / "stateful", error);
    if (error) {
        return 3;
    }

    const Command add{101, CommandOp::Add, 1, Side::Buy, 100, 10, TimeInForce::Gtc, 1};
    const Command replace{101, CommandOp::Replace, 1, Side::Sell, 105, 4, TimeInForce::Fok, 2};

    constexpr std::array<std::uint8_t, 54> command_classes{
        8, 4, 4, 0,
        0x80, 0, 1, 0, 8, 9, 0, 0, 1, 0,
        0x00, 0, 2, 1, 8, 3, 1, 0, 2, 0,
        0x03, 0, 1, 0, 4, 1, 0, 0, 3, 0,
        0x04, 0, 1, 0, 9, 2, 2, 0, 4, 0,
        0x01, 0, 3, 0, 4, 4, 0, 0, 5, 0,
    };
    constexpr std::array<std::uint8_t, 64> capacity_and_invalid{
        1, 0, 0, 1,
        0x00, 0, 1, 0, 4, 1, 0, 0, 1, 0,
        0x00, 0, 2, 0, 5, 2, 0, 0, 2, 0,
        0x00, 0, 1, 1, 6, 1, 0, 0, 3, 0,
        0x02, 0, 0, 0, 4, 0, 0, 0, 4, 0,
        0x03, 0, 1, 0, 4, 0, 0, 0, 5, 0,
        0x00, 2, 3, 0, 0, 5, 2, 0, 6, 0,
    };
    constexpr std::array<std::uint8_t, 54> snapshot_replay{
        6, 8, 3, 0,
        0xc3, 0, 1, 0, 8, 9, 0, 0, 1, 0,
        0xc3, 1, 1, 1, 7, 8, 0, 7, 2, 0,
        0xc7, 0, 1, 0, 9, 2, 1, 8, 3, 0,
        0xc4, 1, 4, 0, 4, 3, 0, 9, 4, 0,
        0xc5, 0, 1, 0, 4, 0, 0, 6, 5, 0,
    };
    constexpr std::array<std::uint8_t, 57> quantity_overflow{
        0xf3, 0xfb, 0xfb, 0xff,
        0xfe, 0x00, 0x36, 0x04, 0x00, 0x08, 0x2d, 0x2d, 0x2d, 0x2d,
        0x2d, 0x2d, 0x2d, 0x2d, 0x2d, 0x2d, 0x2d, 0x2d, 0x2d, 0x2d,
        0x2d, 0xad, 0xd3, 0xcb, 0x2d, 0x2d, 0x2d, 0x09, 0x01, 0x00,
        0x04, 0x80, 0x00, 0x00, 0x05, 0x00, 0x00, 0x02, 0x03, 0x01,
        0x00, 0x00, 0x05, 0x08, 0x06, 0x01, 0x00, 0x01, 0xf7, 0xf6,
        0xc9, 0x01, 0x01,
    };
    if (!write_command_seed(root / "command" / "add", add) ||
        !write_command_seed(root / "command" / "replace", replace) ||
        !write_snapshot_seed(root / "snapshot" / "dense", PriceLevelMode::Dense) ||
        !write_snapshot_seed(root / "snapshot" / "sparse", PriceLevelMode::Sparse) ||
        !write_stateful_seed(root / "stateful" / "command-classes", command_classes) ||
        !write_stateful_seed(root / "stateful" / "capacity-and-invalid", capacity_and_invalid) ||
        !write_stateful_seed(root / "stateful" / "snapshot-replay", snapshot_replay) ||
        !write_stateful_seed(root / "stateful" / "quantity-overflow", quantity_overflow)) {
        return 4;
    }

    return 0;
}
