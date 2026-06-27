#pragma once

#include "Command.hpp"
#include "MatchingEngine.hpp"
#include "Types.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace eigenbook {

inline constexpr std::uint16_t kJournalVersion = 1;
inline constexpr std::uint16_t kJournalHeaderLength = 24;
inline constexpr std::uint32_t kJournalRecordLength = 144;
inline constexpr std::uint32_t kJournalPayloadLength =
    kJournalRecordLength - kJournalHeaderLength;

struct JournalRecordInfo final {
    std::uint64_t journal_sequence{0};
    VenueCommand venue_command{};
    Status expected_status{Status::InternalError};
    SequenceNumber market_data_sequence_before{0};
    SequenceNumber market_data_sequence_after{0};
    std::uint64_t state_checksum_before{0};
    std::uint64_t state_checksum_after{0};
    std::uint32_t events_emitted{0};
    std::uint32_t market_data_events_emitted{0};
    std::uint64_t event_digest{0};
    std::uint64_t market_data_digest{0};
};

struct JournalWriteResult final {
    Status status{Status::InternalError};
    std::size_t bytes_written{0};
    DispatchResult dispatch{};
};

struct JournalReplayCursor final {
    std::uint64_t next_journal_sequence{1};
};

struct JournalReplayResult final {
    Status status{Status::InternalError};
    std::uint32_t records_replayed{0};
    std::size_t bytes_consumed{0};
};

namespace journal_detail {

inline constexpr std::size_t kCrcOffset = 16;

inline void write_u8(std::span<std::byte> buffer,
                     const std::size_t offset,
                     const std::uint8_t value) noexcept
{
    buffer[offset] = static_cast<std::byte>(value);
}

inline void write_u16(std::span<std::byte> buffer,
                      const std::size_t offset,
                      const std::uint16_t value) noexcept
{
    for (std::uint32_t byte = 0; byte < 2U; ++byte) {
        buffer[offset + byte] = static_cast<std::byte>((value >> (byte * 8U)) & 0xffU);
    }
}

inline void write_u32(std::span<std::byte> buffer,
                      const std::size_t offset,
                      const std::uint32_t value) noexcept
{
    for (std::uint32_t byte = 0; byte < 4U; ++byte) {
        buffer[offset + byte] = static_cast<std::byte>((value >> (byte * 8U)) & 0xffU);
    }
}

inline void write_u64(std::span<std::byte> buffer,
                      const std::size_t offset,
                      const std::uint64_t value) noexcept
{
    for (std::uint32_t byte = 0; byte < 8U; ++byte) {
        buffer[offset + byte] = static_cast<std::byte>((value >> (byte * 8U)) & 0xffU);
    }
}

[[nodiscard]] inline std::uint8_t read_u8(const std::span<const std::byte> buffer,
                                          const std::size_t offset) noexcept
{
    return std::to_integer<std::uint8_t>(buffer[offset]);
}

[[nodiscard]] inline std::uint16_t read_u16(const std::span<const std::byte> buffer,
                                            const std::size_t offset) noexcept
{
    std::uint16_t value = 0;
    for (std::uint32_t byte = 0; byte < 2U; ++byte) {
        value |= static_cast<std::uint16_t>(read_u8(buffer, offset + byte)) << (byte * 8U);
    }
    return value;
}

[[nodiscard]] inline std::uint32_t read_u32(const std::span<const std::byte> buffer,
                                            const std::size_t offset) noexcept
{
    std::uint32_t value = 0;
    for (std::uint32_t byte = 0; byte < 4U; ++byte) {
        value |= static_cast<std::uint32_t>(read_u8(buffer, offset + byte)) << (byte * 8U);
    }
    return value;
}

[[nodiscard]] inline std::uint64_t read_u64(const std::span<const std::byte> buffer,
                                            const std::size_t offset) noexcept
{
    std::uint64_t value = 0;
    for (std::uint32_t byte = 0; byte < 8U; ++byte) {
        value |= static_cast<std::uint64_t>(read_u8(buffer, offset + byte)) << (byte * 8U);
    }
    return value;
}

[[nodiscard]] inline std::uint32_t crc32(const std::span<const std::byte> buffer) noexcept
{
    std::uint32_t crc = 0xffffffffU;
    for (std::size_t index = 0; index < buffer.size(); ++index) {
        const std::uint8_t input =
            index >= kCrcOffset && index < kCrcOffset + 4U ? 0U : read_u8(buffer, index);
        crc ^= input;
        for (std::uint32_t bit = 0; bit < 8U; ++bit) {
            const std::uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0xedb88320U & mask);
        }
    }
    return ~crc;
}

inline void digest_u8(std::uint64_t& hash, const std::uint8_t value) noexcept
{
    hash ^= value;
    hash *= 1099511628211ULL;
}

inline void digest_u32(std::uint64_t& hash, const std::uint32_t value) noexcept
{
    for (std::uint32_t byte = 0; byte < 4U; ++byte) {
        digest_u8(hash, static_cast<std::uint8_t>((value >> (byte * 8U)) & 0xffU));
    }
}

inline void digest_u64(std::uint64_t& hash, const std::uint64_t value) noexcept
{
    for (std::uint32_t byte = 0; byte < 8U; ++byte) {
        digest_u8(hash, static_cast<std::uint8_t>((value >> (byte * 8U)) & 0xffU));
    }
}

[[nodiscard]] inline std::uint64_t digest_events(
    const std::span<const BookEvent> events) noexcept
{
    std::uint64_t hash = 14695981039346656037ULL;
    digest_u32(hash, static_cast<std::uint32_t>(events.size()));
    for (const BookEvent& event : events) {
        digest_u8(hash, static_cast<std::uint8_t>(event.kind));
        digest_u32(hash, event.instrument_id);
        digest_u8(hash, static_cast<std::uint8_t>(event.status));
        digest_u64(hash, event.order_id);
        digest_u8(hash, static_cast<std::uint8_t>(event.side));
        digest_u64(hash, static_cast<std::uint64_t>(event.price));
        digest_u64(hash, event.quantity);
        digest_u64(hash, event.old_quantity);
        digest_u64(hash, event.new_quantity);
        digest_u64(hash, event.timestamp);
        digest_u64(hash, event.sequence);
        digest_u8(hash, static_cast<std::uint8_t>(event.time_in_force));
        if (event.kind == BookEvent::Kind::Trade) {
            digest_u32(hash, event.trade.instrument_id);
            digest_u64(hash, event.trade.aggressor_id);
            digest_u64(hash, event.trade.resting_id);
            digest_u8(hash, static_cast<std::uint8_t>(event.trade.aggressor_side));
            digest_u64(hash, static_cast<std::uint64_t>(event.trade.price));
            digest_u64(hash, event.trade.quantity);
            digest_u64(hash, event.trade.timestamp);
            digest_u64(hash, event.trade.sequence);
        }
    }
    return hash;
}

[[nodiscard]] inline std::uint64_t digest_market_data(
    const std::span<const MarketDataEvent> events) noexcept
{
    std::uint64_t hash = 14695981039346656037ULL;
    digest_u32(hash, static_cast<std::uint32_t>(events.size()));
    for (const MarketDataEvent& event : events) {
        digest_u8(hash, static_cast<std::uint8_t>(event.kind));
        digest_u32(hash, event.instrument_id);
        digest_u64(hash, event.sequence);
        digest_u8(hash, static_cast<std::uint8_t>(event.side));
        digest_u64(hash, static_cast<std::uint64_t>(event.price));
        digest_u64(hash, event.previous_quantity);
        digest_u64(hash, event.quantity);
        digest_u32(hash, event.order_count);
        digest_u64(hash, event.aggressor_id);
        digest_u64(hash, event.resting_id);
        digest_u64(hash, event.trade_quantity);
        digest_u64(hash, event.timestamp);
    }
    return hash;
}

inline void encode_record(const JournalRecordInfo& record,
                          const std::span<std::byte> output) noexcept
{
    std::fill_n(output.begin(), kJournalRecordLength, std::byte{0});
    write_u8(output, 0, static_cast<std::uint8_t>('E'));
    write_u8(output, 1, static_cast<std::uint8_t>('B'));
    write_u8(output, 2, static_cast<std::uint8_t>('J'));
    write_u8(output, 3, static_cast<std::uint8_t>('R'));
    write_u16(output, 4, kJournalVersion);
    write_u16(output, 6, kJournalHeaderLength);
    write_u32(output, 8, kJournalRecordLength);
    write_u32(output, 12, kJournalPayloadLength);
    write_u64(output, 24, record.journal_sequence);

    const Command& command = record.venue_command.command;
    write_u32(output, 32, command.instrument_id);
    write_u8(output, 36, static_cast<std::uint8_t>(command.op));
    write_u8(output, 37, static_cast<std::uint8_t>(command.side));
    write_u8(output, 38, static_cast<std::uint8_t>(command.time_in_force));
    write_u8(output, 39, record.venue_command.post_only ? 1U : 0U);
    write_u64(output, 40, command.order_id);
    write_u64(output, 48, std::bit_cast<std::uint64_t>(command.price));
    write_u64(output, 56, command.quantity);
    write_u64(output, 64, command.timestamp);
    write_u64(output, 72, record.venue_command.participant_id);
    write_u8(output, 80, static_cast<std::uint8_t>(record.expected_status));
    write_u64(output, 88, record.market_data_sequence_before);
    write_u64(output, 96, record.market_data_sequence_after);
    write_u64(output, 104, record.state_checksum_before);
    write_u64(output, 112, record.state_checksum_after);
    write_u32(output, 120, record.events_emitted);
    write_u32(output, 124, record.market_data_events_emitted);
    write_u64(output, 128, record.event_digest);
    write_u64(output, 136, record.market_data_digest);
    write_u32(output, kCrcOffset, crc32(output.first(kJournalRecordLength)));
}

} // namespace journal_detail

[[nodiscard]] inline Status decode_journal_record(const std::span<const std::byte> input,
                                                  JournalRecordInfo& record) noexcept
{
    using namespace journal_detail;
    if (input.size() < kJournalHeaderLength) {
        return Status::JournalLengthMismatch;
    }
    if (read_u8(input, 0) != static_cast<std::uint8_t>('E') ||
        read_u8(input, 1) != static_cast<std::uint8_t>('B') ||
        read_u8(input, 2) != static_cast<std::uint8_t>('J') ||
        read_u8(input, 3) != static_cast<std::uint8_t>('R')) {
        return Status::JournalFormatMismatch;
    }
    if (read_u16(input, 4) != kJournalVersion) {
        return Status::JournalVersionMismatch;
    }
    if (read_u16(input, 6) != kJournalHeaderLength ||
        read_u32(input, 8) != kJournalRecordLength ||
        read_u32(input, 12) != kJournalPayloadLength ||
        input.size() < kJournalRecordLength) {
        return Status::JournalLengthMismatch;
    }

    const auto bytes = input.first(kJournalRecordLength);
    if (read_u32(bytes, 16) != crc32(bytes)) {
        return Status::JournalChecksumMismatch;
    }
    if (read_u32(bytes, 20) != 0U) {
        return Status::JournalInvalidField;
    }
    for (std::size_t offset = 81; offset < 88; ++offset) {
        if (read_u8(bytes, offset) != 0U) {
            return Status::JournalInvalidField;
        }
    }

    record = JournalRecordInfo{};
    record.journal_sequence = read_u64(bytes, 24);
    Command& command = record.venue_command.command;
    command.instrument_id = read_u32(bytes, 32);
    command.op = static_cast<CommandOp>(read_u8(bytes, 36));
    command.side = static_cast<Side>(read_u8(bytes, 37));
    command.time_in_force = static_cast<TimeInForce>(read_u8(bytes, 38));
    const std::uint8_t flags = read_u8(bytes, 39);
    record.venue_command.post_only = (flags & 1U) != 0U;
    command.order_id = read_u64(bytes, 40);
    command.price = std::bit_cast<Price>(read_u64(bytes, 48));
    command.quantity = read_u64(bytes, 56);
    command.timestamp = read_u64(bytes, 64);
    record.venue_command.participant_id = read_u64(bytes, 72);
    const std::uint8_t status = read_u8(bytes, 80);
    if (record.journal_sequence == 0 || (flags & 0xfeU) != 0U ||
        status > static_cast<std::uint8_t>(Status::ReplayDiverged) ||
        !valid_command(record.venue_command)) {
        return Status::JournalInvalidField;
    }

    record.expected_status = static_cast<Status>(status);
    record.market_data_sequence_before = read_u64(bytes, 88);
    record.market_data_sequence_after = read_u64(bytes, 96);
    record.state_checksum_before = read_u64(bytes, 104);
    record.state_checksum_after = read_u64(bytes, 112);
    record.events_emitted = read_u32(bytes, 120);
    record.market_data_events_emitted = read_u32(bytes, 124);
    record.event_digest = read_u64(bytes, 128);
    record.market_data_digest = read_u64(bytes, 136);
    return Status::Accepted;
}

/// Dispatch one command and produce one bounded journal record.
///
/// Buffer preflight occurs before dispatch, so a short output buffer does not
/// mutate the engine.
[[nodiscard]] inline JournalWriteResult dispatch_and_record(
    MatchingEngine& engine,
    const VenueCommand& command,
    const std::uint64_t journal_sequence,
    const std::span<std::byte> output) noexcept
{
    JournalWriteResult result{};
    if (output.size() < kJournalRecordLength) {
        result.status = Status::BufferTooSmall;
        return result;
    }
    if (journal_sequence == 0 || !valid_command(command)) {
        result.status = Status::JournalInvalidField;
        return result;
    }

    JournalRecordInfo record{};
    record.journal_sequence = journal_sequence;
    record.venue_command = command;
    record.market_data_sequence_before =
        engine.market_data_sequence(command.command.instrument_id);
    record.state_checksum_before = engine.state_checksum();
    result.dispatch = engine.dispatch(command);
    record.expected_status = result.dispatch.status;
    record.market_data_sequence_after =
        engine.market_data_sequence(command.command.instrument_id);
    record.state_checksum_after = engine.state_checksum();
    record.events_emitted = result.dispatch.events_emitted;
    record.market_data_events_emitted = result.dispatch.market_data_events_emitted;
    record.event_digest = journal_detail::digest_events(result.dispatch.events);
    record.market_data_digest =
        journal_detail::digest_market_data(result.dispatch.market_data_events);
    journal_detail::encode_record(record, output);
    result.status = Status::Accepted;
    result.bytes_written = kJournalRecordLength;
    return result;
}

[[nodiscard]] inline Status replay_journal_record(
    MatchingEngine& engine,
    const std::span<const std::byte> input,
    JournalReplayCursor& cursor) noexcept
{
    JournalRecordInfo record{};
    const Status decode_status = decode_journal_record(input, record);
    if (decode_status != Status::Accepted) {
        return decode_status;
    }
    if (record.journal_sequence != cursor.next_journal_sequence ||
        record.state_checksum_before != engine.state_checksum() ||
        record.market_data_sequence_before !=
            engine.market_data_sequence(record.venue_command.command.instrument_id)) {
        return Status::ReplayDiverged;
    }

    const DispatchResult result = engine.dispatch(record.venue_command);
    if (result.status != record.expected_status ||
        result.events_emitted != record.events_emitted ||
        result.market_data_events_emitted != record.market_data_events_emitted ||
        journal_detail::digest_events(result.events) != record.event_digest ||
        journal_detail::digest_market_data(result.market_data_events) != record.market_data_digest ||
        engine.market_data_sequence(record.venue_command.command.instrument_id) !=
            record.market_data_sequence_after ||
        engine.state_checksum() != record.state_checksum_after) {
        return Status::ReplayDiverged;
    }

    if (cursor.next_journal_sequence !=
        std::numeric_limits<std::uint64_t>::max()) {
        ++cursor.next_journal_sequence;
    }
    return Status::Accepted;
}

[[nodiscard]] inline JournalReplayResult replay_journal(
    MatchingEngine& engine,
    const std::span<const std::byte> input,
    JournalReplayCursor& cursor) noexcept
{
    JournalReplayResult result{};
    result.status = Status::Accepted;
    while (result.bytes_consumed < input.size()) {
        const std::size_t remaining = input.size() - result.bytes_consumed;
        if (remaining < kJournalRecordLength) {
            result.status = Status::JournalLengthMismatch;
            return result;
        }
        const Status status = replay_journal_record(
            engine, input.subspan(result.bytes_consumed, kJournalRecordLength), cursor);
        if (status != Status::Accepted) {
            result.status = status;
            return result;
        }
        result.bytes_consumed += kJournalRecordLength;
        ++result.records_replayed;
    }
    return result;
}

} // namespace eigenbook
