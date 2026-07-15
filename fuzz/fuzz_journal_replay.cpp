#include "Journal.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <span>

namespace {

using namespace eigenbook;

inline constexpr InstrumentId kDenseInstrument = 101;
inline constexpr InstrumentId kSparseInstrument = 202;
inline constexpr std::size_t kStructuredCommandBytes = 9;
inline constexpr std::size_t kMaxJournalRecords = 16;
inline constexpr std::size_t kMaxRawJournalBytes =
    kMaxJournalRecords * kJournalRecordLength;

[[noreturn]] void fail() noexcept
{
    std::abort();
}

[[nodiscard]] bool journal_status(const Status status) noexcept
{
    return status == Status::Accepted || status == Status::JournalFormatMismatch ||
           status == Status::JournalVersionMismatch ||
           status == Status::JournalLengthMismatch ||
           status == Status::JournalChecksumMismatch ||
           status == Status::JournalInvalidField || status == Status::ReplayDiverged;
}

[[nodiscard]] std::array<InstrumentConfig, 2> journal_configs() noexcept
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
    return {{
        InstrumentConfig{kDenseInstrument, dense},
        InstrumentConfig{kSparseInstrument, sparse},
    }};
}

[[nodiscard]] bool commands_equal(const VenueCommand& lhs,
                                  const VenueCommand& rhs) noexcept
{
    const Command& lhs_command = lhs.command;
    const Command& rhs_command = rhs.command;
    return lhs_command.instrument_id == rhs_command.instrument_id &&
           lhs_command.op == rhs_command.op &&
           lhs_command.order_id == rhs_command.order_id &&
           lhs_command.side == rhs_command.side &&
           lhs_command.price == rhs_command.price &&
           lhs_command.quantity == rhs_command.quantity &&
           lhs_command.time_in_force == rhs_command.time_in_force &&
           lhs_command.timestamp == rhs_command.timestamp &&
           lhs.participant_id == rhs.participant_id && lhs.post_only == rhs.post_only;
}

[[nodiscard]] VenueCommand decode_venue_command(
    const std::span<const std::uint8_t, kStructuredCommandBytes> bytes) noexcept
{
    constexpr std::array<CommandOp, 5> operations{
        CommandOp::Add,
        CommandOp::Cancel,
        CommandOp::Modify,
        CommandOp::Replace,
        CommandOp::Market,
    };
    constexpr std::array<InstrumentId, 4> instruments{
        kDenseInstrument,
        kSparseInstrument,
        303,
        kInvalidInstrumentId,
    };
    constexpr std::array<Price, 7> prices{-100, -50, 0, 50, 100, -101, 101};
    constexpr std::array<Quantity, 8> quantities{
        0,
        1,
        2,
        4,
        5,
        10,
        15,
        std::numeric_limits<Quantity>::max(),
    };
    constexpr std::array<TimeInForce, 3> time_in_force{
        TimeInForce::Gtc,
        TimeInForce::Ioc,
        TimeInForce::Fok,
    };
    constexpr std::array<ParticipantId, 4> participants{
        kAnonymousParticipantId,
        7,
        42,
        99,
    };

    const CommandOp operation =
        operations[static_cast<std::size_t>(bytes[0]) % operations.size()];
    const OrderId order_id =
        bytes[2] % 11U == 0U ? kInvalidOrderId : 1U + static_cast<OrderId>(bytes[2] % 32U);
    const bool supports_post_only =
        operation == CommandOp::Add || operation == CommandOp::Replace;
    return VenueCommand{
        Command{
            instruments[static_cast<std::size_t>(bytes[1]) % instruments.size()],
            operation,
            order_id,
            (bytes[3] & 1U) == 0U ? Side::Buy : Side::Sell,
            prices[static_cast<std::size_t>(bytes[4]) % prices.size()],
            quantities[static_cast<std::size_t>(bytes[5]) % quantities.size()],
            time_in_force[static_cast<std::size_t>(bytes[6]) % time_in_force.size()],
            static_cast<Timestamp>(bytes[7]) |
                (static_cast<Timestamp>(bytes[8]) << 8U),
        },
        participants[static_cast<std::size_t>(bytes[6] >> 2U) % participants.size()],
        supports_post_only && (bytes[6] & 0x10U) != 0U,
    };
}

void fuzz_raw_journal(const std::span<const std::uint8_t> input)
{
    const std::size_t bounded_size = std::min(input.size(), kMaxRawJournalBytes);
    const std::span<const std::byte> bytes(
        reinterpret_cast<const std::byte*>(input.data()), bounded_size);

    JournalRecordInfo decoded{};
    const Status decode_status = decode_journal_record(bytes, decoded);
    if (!journal_status(decode_status) || decode_status == Status::ReplayDiverged) {
        fail();
    }

    const std::array<InstrumentConfig, 2> configs = journal_configs();
    MatchingEngine replay_target{std::span<const InstrumentConfig>(configs)};
    if (!replay_target.valid()) {
        fail();
    }
    JournalReplayCursor cursor{};
    const JournalReplayResult replay = replay_journal(replay_target, bytes, cursor);
    if (!journal_status(replay.status) ||
        replay.bytes_consumed !=
            static_cast<std::size_t>(replay.records_replayed) * kJournalRecordLength ||
        replay.bytes_consumed > bytes.size()) {
        fail();
    }
}

struct CorruptedRecord final {
    std::array<std::byte, kJournalRecordLength> bytes{};
    std::size_t size{kJournalRecordLength};
    Status expected_decode_status{Status::Accepted};
    Status expected_replay_status{Status::Accepted};
};

void refresh_crc(std::array<std::byte, kJournalRecordLength>& bytes) noexcept
{
    const std::span<std::byte> output(bytes);
    journal_detail::write_u32(
        output, journal_detail::kCrcOffset, journal_detail::crc32(output));
}

[[nodiscard]] CorruptedRecord corrupt_record(
    const std::span<const std::byte, kJournalRecordLength> valid_record,
    const std::uint8_t selector) noexcept
{
    CorruptedRecord result{};
    std::memcpy(result.bytes.data(), valid_record.data(), valid_record.size());
    const std::span<std::byte> bytes(result.bytes);
    switch (selector % 20U) {
    case 0:
        result.size = kJournalHeaderLength - 1U;
        result.expected_decode_status = Status::JournalLengthMismatch;
        break;
    case 1:
        bytes[0] ^= std::byte{0x01};
        result.expected_decode_status = Status::JournalFormatMismatch;
        break;
    case 2:
        journal_detail::write_u16(bytes, 4, kJournalVersion + 1U);
        result.expected_decode_status = Status::JournalVersionMismatch;
        break;
    case 3:
        journal_detail::write_u16(bytes, 6, kJournalHeaderLength + 1U);
        result.expected_decode_status = Status::JournalLengthMismatch;
        break;
    case 4:
        journal_detail::write_u32(bytes, 8, kJournalRecordLength + 1U);
        result.expected_decode_status = Status::JournalLengthMismatch;
        break;
    case 5:
        journal_detail::write_u32(bytes, 12, kJournalPayloadLength - 1U);
        result.expected_decode_status = Status::JournalLengthMismatch;
        break;
    case 6:
        result.size = kJournalRecordLength - 1U;
        result.expected_decode_status = Status::JournalLengthMismatch;
        break;
    case 7:
        bytes[journal_detail::kCrcOffset] ^= std::byte{0x01};
        result.expected_decode_status = Status::JournalChecksumMismatch;
        break;
    case 8:
        journal_detail::write_u32(bytes, 20, 1U);
        refresh_crc(result.bytes);
        result.expected_decode_status = Status::JournalInvalidField;
        break;
    case 9:
        bytes[81] = std::byte{1};
        refresh_crc(result.bytes);
        result.expected_decode_status = Status::JournalInvalidField;
        break;
    case 10:
        bytes[39] = std::byte{0x80};
        refresh_crc(result.bytes);
        result.expected_decode_status = Status::JournalInvalidField;
        break;
    case 11:
        bytes[36] = std::byte{0xff};
        refresh_crc(result.bytes);
        result.expected_decode_status = Status::JournalInvalidField;
        break;
    case 12:
        bytes[37] = std::byte{0xff};
        refresh_crc(result.bytes);
        result.expected_decode_status = Status::JournalInvalidField;
        break;
    case 13:
        bytes[38] = std::byte{0xff};
        refresh_crc(result.bytes);
        result.expected_decode_status = Status::JournalInvalidField;
        break;
    case 14:
        journal_detail::write_u64(bytes, 24, 0U);
        refresh_crc(result.bytes);
        result.expected_decode_status = Status::JournalInvalidField;
        break;
    case 15:
        bytes[80] = static_cast<std::byte>(kStatusCount);
        refresh_crc(result.bytes);
        result.expected_decode_status = Status::JournalInvalidField;
        break;
    case 16:
        bytes[36] = static_cast<std::byte>(CommandOp::Cancel);
        bytes[39] = std::byte{1};
        refresh_crc(result.bytes);
        result.expected_decode_status = Status::JournalInvalidField;
        break;
    case 17:
        journal_detail::write_u64(bytes, 24, 2U);
        refresh_crc(result.bytes);
        result.expected_replay_status = Status::ReplayDiverged;
        break;
    case 18:
        journal_detail::write_u64(
            bytes, 104, journal_detail::read_u64(bytes, 104) ^ 1U);
        refresh_crc(result.bytes);
        result.expected_replay_status = Status::ReplayDiverged;
        break;
    case 19:
        journal_detail::write_u64(
            bytes, 128, journal_detail::read_u64(bytes, 128) ^ 1U);
        refresh_crc(result.bytes);
        result.expected_replay_status = Status::ReplayDiverged;
        break;
    }
    if (result.expected_decode_status != Status::Accepted) {
        result.expected_replay_status = result.expected_decode_status;
    }
    return result;
}

void fuzz_structured_journal(const std::span<const std::uint8_t> input)
{
    if (input.size() < 1U + kStructuredCommandBytes) {
        return;
    }

    const std::array<InstrumentConfig, 2> configs = journal_configs();
    MatchingEngine source{std::span<const InstrumentConfig>(configs)};
    if (!source.valid()) {
        fail();
    }

    std::array<std::byte, kMaxJournalRecords * kJournalRecordLength> journal{};
    std::size_t record_count = 0;
    std::size_t offset = 1;
    while (record_count < kMaxJournalRecords &&
           kStructuredCommandBytes <= input.size() - offset) {
        const std::span<const std::uint8_t, kStructuredCommandBytes> command_bytes(
            input.data() + offset, kStructuredCommandBytes);
        const VenueCommand command = decode_venue_command(command_bytes);
        const std::uint64_t checksum_before = source.state_checksum();
        const SequenceNumber market_data_before =
            source.market_data_sequence(command.command.instrument_id);
        const std::span<std::byte> output(journal);
        const std::span<std::byte> record_output =
            output.subspan(record_count * kJournalRecordLength, kJournalRecordLength);
        const JournalWriteResult write =
            dispatch_and_record(source, command, record_count + 1U, record_output);
        if (write.status != Status::Accepted ||
            write.bytes_written != kJournalRecordLength) {
            fail();
        }

        JournalRecordInfo decoded{};
        if (decode_journal_record(record_output, decoded) != Status::Accepted ||
            decoded.journal_sequence != record_count + 1U ||
            !commands_equal(decoded.venue_command, command) ||
            decoded.expected_status != write.dispatch.status ||
            decoded.market_data_sequence_before != market_data_before ||
            decoded.market_data_sequence_after !=
                source.market_data_sequence(command.command.instrument_id) ||
            decoded.state_checksum_before != checksum_before ||
            decoded.state_checksum_after != source.state_checksum() ||
            decoded.events_emitted != write.dispatch.events_emitted ||
            decoded.market_data_events_emitted !=
                write.dispatch.market_data_events_emitted) {
            fail();
        }

        ++record_count;
        offset += kStructuredCommandBytes;
    }
    if (record_count == 0) {
        return;
    }

    const std::span<const std::byte> encoded_journal(
        journal.data(), record_count * kJournalRecordLength);
    MatchingEngine replayed{std::span<const InstrumentConfig>(configs)};
    JournalReplayCursor replay_cursor{};
    const JournalReplayResult replay =
        replay_journal(replayed, encoded_journal, replay_cursor);
    if (replay.status != Status::Accepted || replay.records_replayed != record_count ||
        replay.bytes_consumed != encoded_journal.size() ||
        replay_cursor.next_journal_sequence != record_count + 1U ||
        replayed.state_checksum() != source.state_checksum() ||
        replayed.market_data_sequence(kDenseInstrument) !=
            source.market_data_sequence(kDenseInstrument) ||
        replayed.market_data_sequence(kSparseInstrument) !=
            source.market_data_sequence(kSparseInstrument)) {
        fail();
    }

    MatchingEngine wrong_cursor_target{std::span<const InstrumentConfig>(configs)};
    JournalReplayCursor wrong_cursor{2};
    const std::uint64_t before_wrong_cursor = wrong_cursor_target.state_checksum();
    if (replay_journal_record(
            wrong_cursor_target,
            encoded_journal.first(kJournalRecordLength),
            wrong_cursor) != Status::ReplayDiverged ||
        wrong_cursor_target.state_checksum() != before_wrong_cursor) {
        fail();
    }

    const std::span<const std::byte, kJournalRecordLength> first_record(
        encoded_journal.data(), kJournalRecordLength);
    const CorruptedRecord corrupted = corrupt_record(first_record, input[0]);
    const std::span<const std::byte> corrupted_bytes(
        corrupted.bytes.data(), corrupted.size);
    JournalRecordInfo decoded_corruption{};
    if (decode_journal_record(corrupted_bytes, decoded_corruption) !=
        corrupted.expected_decode_status) {
        fail();
    }

    MatchingEngine corruption_target{std::span<const InstrumentConfig>(configs)};
    JournalReplayCursor corruption_cursor{};
    if (replay_journal_record(corruption_target,
                              corrupted_bytes,
                              corruption_cursor) !=
        corrupted.expected_replay_status) {
        fail();
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, const std::size_t size)
{
    if (data == nullptr) {
        return 0;
    }
    const std::span<const std::uint8_t> input(data, size);
    fuzz_raw_journal(input);
    fuzz_structured_journal(input);
    return 0;
}
