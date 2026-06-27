#include "Journal.hpp"

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <span>

namespace {

using namespace eigenbook;

void require(const bool condition, const char* const message)
{
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        std::abort();
    }
}

} // namespace

int main()
{
    constexpr InstrumentId instrument_id = 11;
    BookConfig book_config{90, 110, 32, 128, 1};
    book_config.lot_size = 5;
    book_config.self_trade_policy = SelfTradePolicy::CancelResting;
    book_config.market_data_capacity = 64;
    const InstrumentConfig instruments[]{
        InstrumentConfig{instrument_id, book_config},
    };

    MatchingEngine live(instruments);
    MatchingEngine recovered(instruments);
    constexpr std::array<VenueCommand, 3> commands{{
        VenueCommand{Command{instrument_id, CommandOp::Add, 1, Side::Sell, 100, 10, TimeInForce::Gtc, 1},
                     42,
                     false},
        VenueCommand{Command{instrument_id, CommandOp::Add, 2, Side::Buy, 100, 5, TimeInForce::Gtc, 2},
                     42,
                     false},
        VenueCommand{Command{instrument_id, CommandOp::Replace, 2, Side::Buy, 99, 5, TimeInForce::Gtc, 3},
                     0,
                     true},
    }};

    std::array<std::byte, commands.size() * kJournalRecordLength> journal{};
    for (std::size_t index = 0; index < commands.size(); ++index) {
        const JournalWriteResult result = dispatch_and_record(
            live,
            commands[index],
            index + 1U,
            std::span<std::byte>(journal).subspan(
                index * kJournalRecordLength, kJournalRecordLength));
        require(result.status == Status::Accepted, "failed to produce journal record");
    }

    JournalReplayCursor cursor{};
    const JournalReplayResult replay = replay_journal(recovered, journal, cursor);
    require(replay.status == Status::Accepted, "journal replay diverged");
    require(recovered.state_checksum() == live.state_checksum(), "recovered state mismatch");

    std::printf("records=%u bytes=%zu market_data_sequence=%llu checksum=%llu\n",
                replay.records_replayed,
                replay.bytes_consumed,
                static_cast<unsigned long long>(recovered.market_data_sequence(instrument_id)),
                static_cast<unsigned long long>(recovered.state_checksum()));
    return 0;
}
