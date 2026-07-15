#include "Command.hpp"
#include "FuzzConfig.hpp"
#include "Journal.hpp"
#include "Snapshot.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
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

void refresh_journal_crc(std::array<std::byte, kJournalRecordLength>& bytes) noexcept
{
    const std::span<std::byte> output(bytes);
    journal_detail::write_u32(
        output, journal_detail::kCrcOffset, journal_detail::crc32(output));
}

[[nodiscard]] bool write_journal_corpus(const std::filesystem::path& root) noexcept
{
    BookConfig dense{-100,
                     100,
                     16,
                     64,
                     1,
                     128,
                     PriceLevelMode::Dense,
                     2,
                     SelfTradePolicy::CancelResting,
                     128};
    BookConfig sparse = dense;
    sparse.price_level_mode = PriceLevelMode::Sparse;
    sparse.lot_size = 5;
    const std::array<InstrumentConfig, 2> configs{{
        InstrumentConfig{101, dense},
        InstrumentConfig{202, sparse},
    }};
    MatchingEngine engine{std::span<const InstrumentConfig>(configs)};
    if (!engine.valid()) {
        return false;
    }

    constexpr std::array<VenueCommand, 3> commands{{
        VenueCommand{
            Command{101, CommandOp::Add, 1, Side::Sell, 0, 10, TimeInForce::Gtc, 1},
            42,
            false},
        VenueCommand{
            Command{101, CommandOp::Add, 2, Side::Buy, 0, 10, TimeInForce::Gtc, 2},
            42,
            false},
        VenueCommand{
            Command{202, CommandOp::Add, 3, Side::Buy, -50, 10, TimeInForce::Gtc, 3},
            7,
            true},
    }};
    std::array<std::byte, commands.size() * kJournalRecordLength> journal{};
    const std::span<std::byte> journal_output(journal);
    for (std::size_t index = 0; index < commands.size(); ++index) {
        const JournalWriteResult write = dispatch_and_record(
            engine,
            commands[index],
            index + 1U,
            journal_output.subspan(index * kJournalRecordLength, kJournalRecordLength));
        if (write.status != Status::Accepted ||
            write.bytes_written != kJournalRecordLength) {
            return false;
        }
    }

    std::array<std::byte, kJournalRecordLength> first{};
    std::memcpy(first.data(), journal.data(), first.size());
    if (!write_bytes(root / "valid-single", first, first.size()) ||
        !write_bytes(root / "valid-sequence", journal, journal.size()) ||
        !write_bytes(root / "truncated-header", first, kJournalHeaderLength - 1U)) {
        return false;
    }

    std::array<std::byte, kJournalRecordLength> corrupted = first;
    corrupted[0] ^= std::byte{0x01};
    if (!write_bytes(root / "bad-magic", corrupted, corrupted.size())) {
        return false;
    }

    corrupted = first;
    journal_detail::write_u16(corrupted, 4, kJournalVersion + 1U);
    if (!write_bytes(root / "bad-version", corrupted, corrupted.size())) {
        return false;
    }

    corrupted = first;
    journal_detail::write_u32(corrupted, 8, kJournalRecordLength + 1U);
    if (!write_bytes(root / "bad-length", corrupted, corrupted.size())) {
        return false;
    }

    corrupted = first;
    corrupted[journal_detail::kCrcOffset] ^= std::byte{0x01};
    if (!write_bytes(root / "bad-checksum", corrupted, corrupted.size())) {
        return false;
    }

    corrupted = first;
    journal_detail::write_u32(corrupted, 20, 1U);
    refresh_journal_crc(corrupted);
    if (!write_bytes(root / "reserved-field", corrupted, corrupted.size())) {
        return false;
    }

    corrupted = first;
    corrupted[39] = std::byte{0x80};
    refresh_journal_crc(corrupted);
    if (!write_bytes(root / "invalid-flags", corrupted, corrupted.size())) {
        return false;
    }

    corrupted = first;
    corrupted[36] = std::byte{0xff};
    refresh_journal_crc(corrupted);
    if (!write_bytes(root / "invalid-op", corrupted, corrupted.size())) {
        return false;
    }

    corrupted = first;
    journal_detail::write_u64(corrupted, 24, 0U);
    refresh_journal_crc(corrupted);
    if (!write_bytes(root / "zero-sequence", corrupted, corrupted.size())) {
        return false;
    }

    corrupted = first;
    journal_detail::write_u64(
        corrupted, 104, journal_detail::read_u64(corrupted, 104) ^ 1U);
    refresh_journal_crc(corrupted);
    return write_bytes(root / "replay-divergence", corrupted, corrupted.size());
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
    std::filesystem::create_directories(root / "journal", error);
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
    constexpr std::array<std::uint8_t, 84> venue_features{
        8, 4, 4, 7,
        0x00, 0, 1, 1, 4, 9, 0, 0x40, 1, 0,
        0x00, 0, 2, 0, 4, 9, 0, 0x40, 2, 0,
        0x00, 0, 3, 1, 8, 9, 0, 0x20, 3, 0,
        0x00, 0, 4, 0, 8, 9, 0, 0x70, 4, 0,
        0x00, 0, 5, 0, 7, 8, 0, 0x00, 5, 0,
        0x00, 1, 6, 1, 4, 9, 0, 0x40, 6, 0,
        0x00, 1, 7, 0, 7, 2, 0, 0x20, 7, 0,
        0x01, 1, 8, 0, 4, 9, 1, 0x40, 8, 0,
    };
    if (!write_command_seed(root / "command" / "add", add) ||
        !write_command_seed(root / "command" / "replace", replace) ||
        !write_snapshot_seed(root / "snapshot" / "dense", PriceLevelMode::Dense) ||
        !write_snapshot_seed(root / "snapshot" / "sparse", PriceLevelMode::Sparse) ||
        !write_stateful_seed(root / "stateful" / "command-classes", command_classes) ||
        !write_stateful_seed(root / "stateful" / "capacity-and-invalid", capacity_and_invalid) ||
        !write_stateful_seed(root / "stateful" / "snapshot-replay", snapshot_replay) ||
        !write_stateful_seed(root / "stateful" / "quantity-overflow", quantity_overflow) ||
        !write_stateful_seed(root / "stateful" / "venue-features", venue_features) ||
        !write_journal_corpus(root / "journal")) {
        return 4;
    }

    return 0;
}
