#pragma once

#include "Types.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace eigenbook {

/// Operation selector encoded as one byte in the command wire format.
enum class CommandOp : std::uint8_t {
    Add,
    Cancel,
    Modify,
    Replace,
    Market,
};

/// Fixed command wire size in bytes.
inline constexpr std::size_t kCommandWireSize = 39;

#pragma pack(push, 1)
/// Replay command payload.
///
/// `encode` and `decode` define the portable little-endian representation; do
/// not persist raw `Command` object bytes directly.
struct Command final {
    InstrumentId instrument_id{kInvalidInstrumentId};
    CommandOp op{CommandOp::Add};
    OrderId order_id{kInvalidOrderId};
    Side side{Side::Buy};
    Price price{0};
    Quantity quantity{0};
    TimeInForce time_in_force{TimeInForce::Gtc};
    Timestamp timestamp{0};
};
#pragma pack(pop)

static_assert(sizeof(Command) == kCommandWireSize);
static_assert(alignof(Command) == 1);

/// Venue-aware command used by journal/recovery and the extended dispatcher.
///
/// The legacy 39-byte `Command` wire contract remains unchanged. New venue
/// fields are encoded only by the versioned journal format.
struct VenueCommand final {
    Command command{};
    ParticipantId participant_id{kAnonymousParticipantId};
    bool post_only{false};
};

[[nodiscard]] constexpr bool valid_command_op(const CommandOp op) noexcept
{
    return op == CommandOp::Add || op == CommandOp::Cancel || op == CommandOp::Modify ||
           op == CommandOp::Replace || op == CommandOp::Market;
}

[[nodiscard]] constexpr bool valid_command_side(const Side side) noexcept
{
    return side == Side::Buy || side == Side::Sell;
}

[[nodiscard]] constexpr bool valid_command_time_in_force(const TimeInForce time_in_force) noexcept
{
    return time_in_force == TimeInForce::Gtc || time_in_force == TimeInForce::Ioc ||
           time_in_force == TimeInForce::Fok;
}

[[nodiscard]] constexpr bool valid_command(const Command& command) noexcept
{
    return valid_command_op(command.op) && valid_command_side(command.side) &&
           valid_command_time_in_force(command.time_in_force);
}

[[nodiscard]] constexpr bool valid_command(const VenueCommand& command) noexcept
{
    if (!valid_command(command.command)) {
        return false;
    }
    if (command.post_only &&
        command.command.op != CommandOp::Add && command.command.op != CommandOp::Replace) {
        return false;
    }
    return true;
}

namespace detail {

class CommandWriter final {
public:
    explicit CommandWriter(std::span<std::byte> buffer) noexcept : buffer_(buffer) {}

    void write_u8(const std::uint8_t value) noexcept
    {
        if (!reserve(1)) {
            return;
        }
        buffer_[offset_] = static_cast<std::byte>(value);
        ++offset_;
    }

    void write_u32(const std::uint32_t value) noexcept
    {
        if (!reserve(4)) {
            return;
        }
        for (std::uint32_t i = 0; i < 4; ++i) {
            buffer_[offset_ + i] = static_cast<std::byte>((value >> (i * 8U)) & 0xffU);
        }
        offset_ += 4;
    }

    void write_u64(const std::uint64_t value) noexcept
    {
        if (!reserve(8)) {
            return;
        }
        for (std::uint32_t i = 0; i < 8; ++i) {
            buffer_[offset_ + i] = static_cast<std::byte>((value >> (i * 8U)) & 0xffU);
        }
        offset_ += 8;
    }

    void write_price(const Price value) noexcept
    {
        std::uint64_t raw = 0;
        std::memcpy(&raw, &value, sizeof(raw));
        write_u64(raw);
    }

    [[nodiscard]] bool ok() const noexcept
    {
        return ok_;
    }

private:
    std::span<std::byte> buffer_;
    std::size_t offset_{0};
    bool ok_{true};

    [[nodiscard]] bool reserve(const std::size_t bytes) noexcept
    {
        if (!ok_ || bytes > buffer_.size() - offset_) {
            ok_ = false;
            return false;
        }
        return true;
    }
};

class CommandReader final {
public:
    explicit CommandReader(std::span<const std::byte> buffer) noexcept : buffer_(buffer) {}

    [[nodiscard]] bool read_u8(std::uint8_t& value) noexcept
    {
        if (!reserve(1)) {
            return false;
        }
        value = std::to_integer<std::uint8_t>(buffer_[offset_]);
        ++offset_;
        return true;
    }

    [[nodiscard]] bool read_u32(std::uint32_t& value) noexcept
    {
        if (!reserve(4)) {
            return false;
        }
        value = 0;
        for (std::uint32_t i = 0; i < 4; ++i) {
            value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(buffer_[offset_ + i])) << (i * 8U);
        }
        offset_ += 4;
        return true;
    }

    [[nodiscard]] bool read_u64(std::uint64_t& value) noexcept
    {
        if (!reserve(8)) {
            return false;
        }
        value = 0;
        for (std::uint32_t i = 0; i < 8; ++i) {
            value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(buffer_[offset_ + i])) << (i * 8U);
        }
        offset_ += 8;
        return true;
    }

    [[nodiscard]] bool read_price(Price& value) noexcept
    {
        std::uint64_t raw = 0;
        if (!read_u64(raw)) {
            return false;
        }
        std::memcpy(&value, &raw, sizeof(value));
        return true;
    }

private:
    std::span<const std::byte> buffer_;
    std::size_t offset_{0};

    [[nodiscard]] bool reserve(const std::size_t bytes) const noexcept
    {
        return bytes <= buffer_.size() - offset_;
    }
};

} // namespace detail

/// Encode exactly `kCommandWireSize` bytes into `out_buffer`.
///
/// The first 39 bytes are written in little-endian field order. Extra bytes in
/// the caller-provided buffer are left untouched.
[[nodiscard]] inline Status encode(const Command& command, std::span<std::byte> out_buffer) noexcept
{
    if (out_buffer.size() < kCommandWireSize) {
        return Status::BufferTooSmall;
    }
    if (!valid_command(command)) {
        return Status::InvalidCommand;
    }

    detail::CommandWriter writer(out_buffer.subspan(0, kCommandWireSize));
    writer.write_u32(command.instrument_id);
    writer.write_u8(static_cast<std::uint8_t>(command.op));
    writer.write_u64(command.order_id);
    writer.write_u8(static_cast<std::uint8_t>(command.side));
    writer.write_price(command.price);
    writer.write_u64(command.quantity);
    writer.write_u8(static_cast<std::uint8_t>(command.time_in_force));
    writer.write_u64(command.timestamp);
    return writer.ok() ? Status::Accepted : Status::BufferTooSmall;
}

/// Decode the first `kCommandWireSize` bytes of a little-endian command record.
///
/// Extra bytes are ignored. Short buffers return `Status::BufferTooSmall`;
/// invalid enum values return `Status::InvalidCommand`.
[[nodiscard]] inline Status decode(std::span<const std::byte> buffer, Command& command) noexcept
{
    if (buffer.size() < kCommandWireSize) {
        return Status::BufferTooSmall;
    }

    detail::CommandReader reader(buffer.subspan(0, kCommandWireSize));
    InstrumentId instrument_id = kInvalidInstrumentId;
    OrderId order_id = kInvalidOrderId;
    Price price = 0;
    Quantity quantity = 0;
    Timestamp timestamp = 0;
    std::uint8_t op = 0;
    std::uint8_t side = 0;
    std::uint8_t time_in_force = 0;
    if (!reader.read_u32(instrument_id) || !reader.read_u8(op) || !reader.read_u64(order_id) ||
        !reader.read_u8(side) || !reader.read_price(price) || !reader.read_u64(quantity) ||
        !reader.read_u8(time_in_force) || !reader.read_u64(timestamp)) {
        return Status::BufferTooSmall;
    }

    command.instrument_id = instrument_id;
    command.op = static_cast<CommandOp>(op);
    command.order_id = order_id;
    command.side = static_cast<Side>(side);
    command.price = price;
    command.quantity = quantity;
    command.time_in_force = static_cast<TimeInForce>(time_in_force);
    command.timestamp = timestamp;
    return valid_command(command) ? Status::Accepted : Status::InvalidCommand;
}

} // namespace eigenbook
