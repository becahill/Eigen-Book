#include "MatchingEngine.hpp"

#include <cstdio>
#include <cstdlib>

namespace {

[[noreturn]] void fail(const char* const message)
{
    std::fprintf(stderr, "%s\n", message);
    std::abort();
}

void require(const bool condition, const char* const message)
{
    if (!condition) {
        fail(message);
    }
}

} // namespace

int main()
{
    const eigenbook::BookConfig book_config{
        90,
        110,
        16,
        32,
        1,
    };
    const eigenbook::InstrumentConfig instruments[]{{101, book_config}};

    eigenbook::MatchingEngineCreateResult created =
        eigenbook::MatchingEngine::create(instruments);
    require(created.has_value(),
            eigenbook::matching_engine_init_error_name(created.error));

    eigenbook::MatchingEngine* const engine = created.get();
    require(engine != nullptr, "matching engine result had no value");

    const eigenbook::AddOrderResult bid =
        engine->add_limit_order(101, 1, eigenbook::Side::Buy, 100, 10, 1);
    require(bid.status == eigenbook::Status::Accepted,
            "failed to add resting bid");

    const eigenbook::AddOrderResult ask =
        engine->add_limit_order(101, 2, eigenbook::Side::Sell, 100, 4, 2);
    require(ask.status == eigenbook::Status::Filled,
            "failed to match crossing ask");
    require(ask.executed_quantity == 4, "unexpected executed quantity");

    std::puts("EigenBook consumer smoke ok");
    return 0;
}
