#include "Command.hpp"
#include "MatchingEngine.hpp"

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

const char* status_name(const Status status) noexcept
{
    switch (status) {
    case Status::Accepted:
        return "Accepted";
    case Status::Cancelled:
        return "Cancelled";
    case Status::Filled:
        return "Filled";
    case Status::PartiallyFilled:
        return "PartiallyFilled";
    case Status::NoLiquidity:
        return "NoLiquidity";
    case Status::UnknownOrderId:
        return "UnknownOrderId";
    case Status::UnknownInstrument:
        return "UnknownInstrument";
    case Status::InvalidCommand:
        return "InvalidCommand";
    case Status::EventLogFull:
        return "EventLogFull";
    default:
        return "Rejected";
    }
}

const char* op_name(const CommandOp op) noexcept
{
    switch (op) {
    case CommandOp::Add:
        return "Add";
    case CommandOp::Cancel:
        return "Cancel";
    case CommandOp::Modify:
        return "Modify";
    case CommandOp::Replace:
        return "Replace";
    case CommandOp::Market:
        return "Market";
    }

    return "Invalid";
}

std::uint32_t trade_count(std::span<const BookEvent> events) noexcept
{
    std::uint32_t count = 0;
    for (const BookEvent& event : events) {
        if (event.kind == BookEvent::Kind::Trade) {
            ++count;
        }
    }
    return count;
}

} // namespace

int main()
{
    constexpr InstrumentId instrument_a = 11;
    constexpr InstrumentId instrument_b = 22;
    const BookConfig config{90, 130, 32, 128, 1};
    const InstrumentConfig instruments[] = {
        InstrumentConfig{instrument_a, config},
        InstrumentConfig{instrument_b, config},
    };

    MatchingEngine engine(instruments);
    require(engine.valid(), "engine configuration rejected");

    constexpr std::array<Command, 12> commands{{
        Command{instrument_a, CommandOp::Add, 1, Side::Buy, 100, 10, TimeInForce::Gtc, 1},
        Command{instrument_a, CommandOp::Add, 2, Side::Sell, 105, 7, TimeInForce::Gtc, 2},
        Command{instrument_b, CommandOp::Add, 1, Side::Sell, 99, 4, TimeInForce::Gtc, 3},
        Command{instrument_b, CommandOp::Add, 2, Side::Buy, 95, 9, TimeInForce::Gtc, 4},
        Command{instrument_a, CommandOp::Replace, 2, Side::Sell, 101, 7, TimeInForce::Gtc, 5},
        Command{instrument_a, CommandOp::Market, 900, Side::Buy, 0, 3, TimeInForce::Gtc, 6},
        Command{instrument_b, CommandOp::Market, 901, Side::Buy, 0, 2, TimeInForce::Gtc, 7},
        Command{instrument_a, CommandOp::Modify, 1, Side::Buy, 0, 6, TimeInForce::Gtc, 8},
        Command{instrument_b, CommandOp::Replace, 2, Side::Buy, 100, 5, TimeInForce::Ioc, 9},
        Command{instrument_a, CommandOp::Cancel, 1, Side::Buy, 0, 0, TimeInForce::Gtc, 10},
        Command{instrument_b, CommandOp::Add, 3, Side::Buy, 98, 3, TimeInForce::Fok, 11},
        Command{instrument_a, CommandOp::Market, 902, Side::Sell, 0, 4, TimeInForce::Gtc, 12},
    }};

    std::array<std::array<std::byte, kCommandWireSize>, commands.size()> encoded_commands{};
    for (std::size_t i = 0; i < commands.size(); ++i) {
        require(encode(commands[i], encoded_commands[i]) == Status::Accepted, "failed to encode command");
    }

    for (std::size_t i = 0; i < encoded_commands.size(); ++i) {
        Command command{};
        const Status decode_status = decode(encoded_commands[i], command);
        require(decode_status == Status::Accepted, "failed to decode preloaded command");

        const DispatchResult result = engine.dispatch(command);
        const MatchingEngineStats stats = engine.stats();

        std::printf("#%zu instrument=%u op=%s status=%s events=%u trades=%u executed=%llu resting=%llu\n",
                    i + 1U,
                    command.instrument_id,
                    op_name(command.op),
                    status_name(result.status),
                    result.events_emitted,
                    trade_count(result.events),
                    static_cast<unsigned long long>(result.executed_quantity),
                    static_cast<unsigned long long>(result.resting_quantity));
        std::printf("   stats dispatch=%llu adds=%llu cancels=%llu modifies=%llu replaces=%llu markets=%llu rejects=%llu "
                    "high_water=%u live_orders=%u\n",
                    static_cast<unsigned long long>(stats.dispatch_count),
                    static_cast<unsigned long long>(stats.adds),
                    static_cast<unsigned long long>(stats.cancels),
                    static_cast<unsigned long long>(stats.modifies),
                    static_cast<unsigned long long>(stats.replaces),
                    static_cast<unsigned long long>(stats.market_matches),
                    static_cast<unsigned long long>(stats.rejects),
                    stats.event_log_high_water_mark,
                    stats.total_live_order_count);
    }

    return 0;
}
