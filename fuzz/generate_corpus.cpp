#include "Command.hpp"
#include "FuzzConfig.hpp"
#include "Snapshot.hpp"

#include <array>
#include <cstddef>
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

    const Command add{101, CommandOp::Add, 1, Side::Buy, 100, 10, TimeInForce::Gtc, 1};
    const Command replace{101, CommandOp::Replace, 1, Side::Sell, 105, 4, TimeInForce::Fok, 2};
    if (!write_command_seed(root / "command" / "add", add) ||
        !write_command_seed(root / "command" / "replace", replace) ||
        !write_snapshot_seed(root / "snapshot" / "dense", PriceLevelMode::Dense) ||
        !write_snapshot_seed(root / "snapshot" / "sparse", PriceLevelMode::Sparse)) {
        return 4;
    }

    return 0;
}
