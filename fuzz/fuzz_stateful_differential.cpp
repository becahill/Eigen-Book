// Keep the deterministic test oracle as the single matching-logic source.
// Renaming its test entry point makes the oracle and comparison helpers
// available to this libFuzzer translation unit without copying the model.
#define main eigenbook_deterministic_test_main
#include "../tests/test_eigenbook.cpp"
#undef main

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <span>

namespace {

using namespace eigenbook;

inline constexpr InstrumentId kInstrumentA = 101;
inline constexpr InstrumentId kInstrumentB = 202;
inline constexpr std::size_t kSnapshotBufferSize = 16'384;
inline constexpr std::size_t kCommandBytes = 10;
inline constexpr std::uint32_t kMaxCommands = 64;
inline constexpr OrderId kMaxDecodedOrderId = 64;

class InputReader final {
public:
    explicit InputReader(const std::span<const std::uint8_t> input) noexcept : input_(input) {}

    [[nodiscard]] bool read_u8(std::uint8_t& value) noexcept
    {
        if (offset_ >= input_.size()) {
            return false;
        }
        value = input_[offset_];
        ++offset_;
        return true;
    }

    [[nodiscard]] bool read_u16(std::uint16_t& value) noexcept
    {
        std::uint8_t low = 0;
        std::uint8_t high = 0;
        if (!read_u8(low) || !read_u8(high)) {
            return false;
        }
        value = static_cast<std::uint16_t>(low) |
                static_cast<std::uint16_t>(static_cast<std::uint16_t>(high) << 8U);
        return true;
    }

    [[nodiscard]] std::size_t remaining() const noexcept
    {
        return input_.size() - offset_;
    }

private:
    std::span<const std::uint8_t> input_;
    std::size_t offset_{0};
};

struct DecodedCommand final {
    Command command{};
    bool force_snapshot{false};
    bool test_corrupt_snapshot{false};
    std::uint8_t mutation_selector{0};
};

struct HarnessConfig final {
    BookConfig dense{};
    BookConfig sparse{};
    std::uint8_t snapshot_period{1};
    std::uint8_t snapshot_phase{0};
};

[[nodiscard]] HarnessConfig decode_config(InputReader& reader) noexcept
{
    std::uint8_t capacity_byte = 0;
    std::uint8_t map_byte = 0;
    std::uint8_t tick_byte = 0;
    std::uint8_t snapshot_byte = 0;
    static_cast<void>(reader.read_u8(capacity_byte));
    static_cast<void>(reader.read_u8(map_byte));
    static_cast<void>(reader.read_u8(tick_byte));
    static_cast<void>(reader.read_u8(snapshot_byte));

    constexpr std::array<Price, 5> ticks{1, 2, 4, 5, 10};
    const std::uint32_t max_orders = 1U + static_cast<std::uint32_t>(capacity_byte % 16U);
    const std::uint32_t max_requested_map_capacity =
        std::max(1U, std::min(32U, max_orders * 2U));
    const std::uint32_t requested_map_capacity =
        1U + static_cast<std::uint32_t>(map_byte) % max_requested_map_capacity;
    const Price tick = ticks[static_cast<std::size_t>(tick_byte) % ticks.size()];
    std::uint32_t event_log_capacity = 0;
    switch ((map_byte >> 5U) % 4U) {
    case 0:
        break;
    case 1:
        event_log_capacity = 1;
        break;
    case 2:
        event_log_capacity = 2;
        break;
    case 3:
        event_log_capacity = max_orders + 2U;
        break;
    }

    HarnessConfig result{};
    result.dense = BookConfig{
        -100,
        100,
        max_orders,
        requested_map_capacity,
        tick,
        event_log_capacity,
        PriceLevelMode::Dense,
    };
    result.sparse = result.dense;
    result.sparse.price_level_mode = PriceLevelMode::Sparse;
    result.snapshot_period = static_cast<std::uint8_t>(1U + snapshot_byte % 8U);
    result.snapshot_phase =
        static_cast<std::uint8_t>((snapshot_byte >> 3U) % result.snapshot_period);
    return result;
}

[[nodiscard]] InstrumentId decode_instrument(const std::uint8_t value) noexcept
{
    switch (value % 4U) {
    case 0:
        return kInstrumentA;
    case 1:
        return kInstrumentB;
    case 2:
        return 303;
    default:
        return kInvalidInstrumentId;
    }
}

[[nodiscard]] OrderId decode_order_id(const std::uint8_t value) noexcept
{
    if ((value & 0x0fU) == 0U) {
        return kInvalidOrderId;
    }
    return 1U + static_cast<OrderId>(value % kMaxDecodedOrderId);
}

[[nodiscard]] Price decode_price(const BookConfig& config, const std::uint8_t value) noexcept
{
    const std::uint32_t selector = value % 12U;
    switch (selector) {
    case 0:
        return config.min_price;
    case 1:
        return config.max_price;
    case 2:
        return config.min_price - config.tick_size;
    case 3:
        return config.max_price + config.tick_size;
    case 4:
        return 0;
    case 5:
        return -config.tick_size;
    case 6:
        return config.tick_size;
    case 7:
        return -50;
    case 8:
        return 50;
    default: {
        const std::uint32_t level =
            static_cast<std::uint32_t>(value) % config.price_level_count();
        return config.min_price + static_cast<Price>(level) * config.tick_size;
    }
    }
}

[[nodiscard]] Quantity decode_quantity(const std::uint8_t value,
                                       const std::uint32_t max_orders) noexcept
{
    switch (value % 10U) {
    case 0:
        return 0;
    case 1:
        return 1;
    case 2:
        return 2;
    case 3:
        return static_cast<Quantity>(max_orders);
    case 4:
        return static_cast<Quantity>(max_orders + 1U);
    case 5:
        return std::numeric_limits<Quantity>::max();
    case 6:
        return std::numeric_limits<Quantity>::max() / 2U;
    default:
        return 1U + static_cast<Quantity>(value % 32U);
    }
}

[[nodiscard]] bool decode_command(InputReader& reader,
                                  const BookConfig& config,
                                  DecodedCommand& decoded) noexcept
{
    if (reader.remaining() < kCommandBytes) {
        return false;
    }

    std::uint8_t control = 0;
    std::uint8_t instrument = 0;
    std::uint8_t id = 0;
    std::uint8_t side = 0;
    std::uint8_t price = 0;
    std::uint8_t quantity = 0;
    std::uint8_t tif = 0;
    std::uint8_t mutation = 0;
    std::uint16_t timestamp = 0;
    if (!reader.read_u8(control) || !reader.read_u8(instrument) || !reader.read_u8(id) ||
        !reader.read_u8(side) || !reader.read_u8(price) || !reader.read_u8(quantity) ||
        !reader.read_u8(tif) || !reader.read_u8(mutation) || !reader.read_u16(timestamp)) {
        return false;
    }

    constexpr std::array<CommandOp, 5> operations{
        CommandOp::Add,
        CommandOp::Market,
        CommandOp::Cancel,
        CommandOp::Modify,
        CommandOp::Replace,
    };
    constexpr std::array<TimeInForce, 3> time_in_force{
        TimeInForce::Gtc,
        TimeInForce::Ioc,
        TimeInForce::Fok,
    };

    decoded.command = Command{
        decode_instrument(instrument),
        operations[static_cast<std::size_t>(control) % operations.size()],
        decode_order_id(id),
        (side & 1U) == 0U ? Side::Buy : Side::Sell,
        decode_price(config, price),
        decode_quantity(quantity, config.max_orders),
        time_in_force[static_cast<std::size_t>(tif) % time_in_force.size()],
        static_cast<Timestamp>(timestamp),
    };
    decoded.force_snapshot = (control & 0x80U) != 0U;
    decoded.test_corrupt_snapshot = (control & 0x40U) != 0U;
    decoded.mutation_selector = mutation;
    return true;
}

[[nodiscard]] bool configured_instrument(const InstrumentId instrument_id) noexcept
{
    return instrument_id == kInstrumentA || instrument_id == kInstrumentB;
}

template <typename Source>
[[nodiscard]] DispatchResult make_dispatch_result(const Source& source) noexcept
{
    DispatchResult result{};
    result.status = source.status;
    result.events_emitted = source.events_emitted;
    result.events = source.events;
    return result;
}

template <>
[[nodiscard]] DispatchResult make_dispatch_result(const AddOrderResult& source) noexcept
{
    DispatchResult result{};
    result.status = source.status;
    result.accepted_quantity = source.accepted_quantity;
    result.executed_quantity = source.executed_quantity;
    result.resting_quantity = source.resting_quantity;
    result.fills = source.fills;
    result.has_last_price = source.has_last_price;
    result.last_price = source.last_price;
    result.events_emitted = source.events_emitted;
    result.events = source.events;
    return result;
}

template <>
[[nodiscard]] DispatchResult make_dispatch_result(const CancelResult& source) noexcept
{
    DispatchResult result{};
    result.status = source.status;
    result.canceled_quantity = source.canceled_quantity;
    result.events_emitted = source.events_emitted;
    result.events = source.events;
    return result;
}

template <>
[[nodiscard]] DispatchResult make_dispatch_result(const ModifyResult& source) noexcept
{
    DispatchResult result{};
    result.status = source.status;
    result.old_quantity = source.old_quantity;
    result.new_quantity = source.new_quantity;
    result.events_emitted = source.events_emitted;
    result.events = source.events;
    return result;
}

template <>
[[nodiscard]] DispatchResult make_dispatch_result(const ReplaceResult& source) noexcept
{
    DispatchResult result{};
    result.status = source.status;
    result.old_price = source.old_price;
    result.new_price = source.new_price;
    result.old_quantity = source.old_quantity;
    result.new_quantity = source.new_quantity;
    result.executed_quantity = source.executed_quantity;
    result.resting_quantity = source.resting_quantity;
    result.fills = source.fills;
    result.has_last_price = source.has_last_price;
    result.last_price = source.last_price;
    result.events_emitted = source.events_emitted;
    result.events = source.events;
    return result;
}

template <>
[[nodiscard]] DispatchResult make_dispatch_result(const MatchResult& source) noexcept
{
    DispatchResult result{};
    result.status = source.status;
    result.requested_quantity = source.requested_quantity;
    result.executed_quantity = source.executed_quantity;
    result.remaining_quantity = source.remaining_quantity;
    result.fills = source.fills;
    result.has_last_price = source.has_last_price;
    result.last_price = source.last_price;
    result.events_emitted = source.events_emitted;
    result.events = source.events;
    return result;
}

[[nodiscard]] DispatchResult dispatch_reference(ReferenceOrderBook& reference,
                                                const Command& command)
{
    switch (command.op) {
    case CommandOp::Add:
        return make_dispatch_result(reference.add_limit_order(command.order_id,
                                                              command.side,
                                                              command.price,
                                                              command.quantity,
                                                              command.timestamp,
                                                              command.time_in_force));
    case CommandOp::Cancel:
        return make_dispatch_result(
            reference.cancel_order(command.order_id, command.timestamp));
    case CommandOp::Modify:
        return make_dispatch_result(
            reference.modify_order(command.order_id, command.quantity, command.timestamp));
    case CommandOp::Replace:
        return make_dispatch_result(reference.replace_order(command.order_id,
                                                            command.price,
                                                            command.quantity,
                                                            command.timestamp,
                                                            command.time_in_force));
    case CommandOp::Market:
        return make_dispatch_result(reference.match_market_order(command.side,
                                                                 command.quantity,
                                                                 command.order_id,
                                                                 command.timestamp));
    }

    std::abort();
}

void check_order_links(const OrderBook& actual, const ReferenceOrderBook& expected)
{
    for (OrderId id = 0; id <= kMaxDecodedOrderId; ++id) {
        const Order* const actual_order = actual.find_order(id);
        const ReferenceOrder* const expected_order = expected.find_order(id);
        CHECK((actual_order != nullptr) == (expected_order != nullptr));
        if (actual_order == nullptr || expected_order == nullptr) {
            continue;
        }

        CHECK(actual_order->id == expected_order->id);
        CHECK(actual_order->side == expected_order->side);
        CHECK(actual_order->price == expected_order->price);
        CHECK(actual_order->quantity == expected_order->quantity);
        CHECK(actual_order->timestamp == expected_order->timestamp);
        CHECK(actual_order->sequence == expected_order->sequence);
        CHECK(actual_order->active);
        CHECK(actual_order->state == OrderState::Resting ||
              actual_order->state == OrderState::PartiallyFilled);
        CHECK(actual_order->level != nullptr);

        const ReferenceOrder* previous = nullptr;
        const ReferenceOrder* next = nullptr;
        for (OrderId candidate_id = 1; candidate_id <= kMaxDecodedOrderId; ++candidate_id) {
            const ReferenceOrder* const candidate = expected.find_order(candidate_id);
            if (candidate == nullptr || candidate->side != expected_order->side ||
                candidate->price != expected_order->price) {
                continue;
            }
            if (candidate->sequence < expected_order->sequence &&
                (previous == nullptr || candidate->sequence > previous->sequence)) {
                previous = candidate;
            }
            if (candidate->sequence > expected_order->sequence &&
                (next == nullptr || candidate->sequence < next->sequence)) {
                next = candidate;
            }
        }

        CHECK((actual_order->prev == nullptr) == (previous == nullptr));
        CHECK((actual_order->next == nullptr) == (next == nullptr));
        if (previous != nullptr) {
            CHECK(actual_order->prev->id == previous->id);
            CHECK(actual_order->prev->next == actual_order);
        }
        if (next != nullptr) {
            CHECK(actual_order->next->id == next->id);
            CHECK(actual_order->next->prev == actual_order);
        }
    }
}

void check_engine_state(const MatchingEngine& engine,
                        const ReferenceOrderBook& reference_a,
                        const ReferenceOrderBook& reference_b)
{
    CHECK(engine.valid());
    CHECK(engine.instrument_count() == 2U);
    check_engine_book_equal(engine, kInstrumentA, reference_a);
    check_engine_book_equal(engine, kInstrumentB, reference_b);

    const OrderBook* const book_a = engine.order_book(kInstrumentA);
    const OrderBook* const book_b = engine.order_book(kInstrumentB);
    CHECK(book_a != nullptr);
    CHECK(book_b != nullptr);
    check_order_links(*book_a, reference_a);
    check_order_links(*book_b, reference_b);

    for (const InstrumentId unknown : {kInvalidInstrumentId, InstrumentId{303}}) {
        CHECK(engine.order_book(unknown) == nullptr);
        CHECK(engine.live_order_count(unknown) == 0U);
        CHECK(engine.find_order(unknown, 1) == nullptr);
        CHECK(!engine.best_bid(unknown).valid);
        CHECK(!engine.best_ask(unknown).valid);
        CHECK(engine.top_of_book(unknown).status == Status::UnknownInstrument);
    }
}

[[nodiscard]] SnapshotWriteResult snapshot_engine(
    const MatchingEngine& engine,
    std::array<std::byte, kSnapshotBufferSize>& buffer) noexcept
{
    const SnapshotWriteResult result = serialize(engine, buffer);
    CHECK(result.status == Status::Accepted);
    CHECK(result.bytes_written > 0U);
    CHECK(result.bytes_written <= buffer.size());
    return result;
}

void mutate_snapshot(std::array<std::byte, kSnapshotBufferSize>& bytes,
                     std::size_t& size,
                     const std::uint8_t selector)
{
    CHECK(size >= detail::kEngineHeaderWireSize);
    switch (selector % 10U) {
    case 0:
        bytes[0] ^= std::byte{0x01};
        break;
    case 1:
        bytes[4] = static_cast<std::byte>(kSnapshotFormatVersion + 1U);
        break;
    case 2:
        bytes[5] = std::byte{1};
        break;
    case 3:
        bytes[8] ^= std::byte{0x01};
        break;
    case 4:
        bytes[16] = std::byte{0};
        break;
    case 5:
        --size;
        break;
    case 6:
        CHECK(size < bytes.size());
        bytes[size] = std::byte{0xa5};
        ++size;
        break;
    case 7:
    case 8:
    case 9: {
        detail::EngineHeaderInfo engine_header{};
        const std::span<const std::byte> snapshot(bytes.data(), size);
        CHECK(detail::parse_engine_header(snapshot, engine_header) == Status::Accepted);

        std::size_t book_offset = engine_header.payload_offset;
        detail::BookHeaderInfo selected_book{};
        bool found_order = false;
        for (std::uint32_t index = 0; index < engine_header.instrument_count; ++index) {
            CHECK(book_offset <= size);
            CHECK(detail::kInstrumentConfigWireSize <= size - book_offset);
            book_offset += detail::kInstrumentConfigWireSize;
            CHECK(detail::parse_book_header(
                      snapshot.subspan(book_offset), selected_book, false) ==
                  Status::Accepted);
            if (selected_book.order_count != 0U) {
                found_order = true;
                break;
            }
            book_offset += selected_book.end_offset;
        }

        if (!found_order) {
            bytes[5] = std::byte{1};
            break;
        }

        const std::size_t first_order = book_offset + selected_book.orders_offset;
        CHECK(first_order <= size);
        CHECK(detail::kBookOrderWireSize <= size - first_order);
        if (selector % 10U == 7U) {
            bytes[first_order + sizeof(OrderId)] = std::byte{0xff};
            break;
        }
        if (selector % 10U == 8U) {
            constexpr std::size_t kQuantityOffset =
                sizeof(OrderId) + sizeof(std::uint8_t) + sizeof(Price);
            for (std::size_t index = 0; index < sizeof(Quantity); ++index) {
                bytes[first_order + kQuantityOffset + index] = std::byte{0};
            }
            break;
        }

        if (selected_book.order_count < 2U) {
            for (std::size_t index = 0; index < sizeof(OrderId); ++index) {
                bytes[first_order + index] = std::byte{0};
            }
            break;
        }
        const std::size_t second_order = first_order + detail::kBookOrderWireSize;
        CHECK(second_order <= size);
        CHECK(detail::kBookOrderWireSize <= size - second_order);
        for (std::size_t index = 0; index < sizeof(OrderId); ++index) {
            bytes[second_order + index] = bytes[first_order + index];
        }
        break;
    }
    }
}

void verify_failed_restore_is_atomic(MatchingEngine& target,
                                     const std::span<const std::byte> valid_snapshot,
                                     const std::uint8_t selector)
{
    std::array<std::byte, kSnapshotBufferSize> before{};
    const SnapshotWriteResult before_result = snapshot_engine(target, before);

    std::array<std::byte, kSnapshotBufferSize> corrupted{};
    CHECK(valid_snapshot.size() <= corrupted.size());
    std::memcpy(corrupted.data(), valid_snapshot.data(), valid_snapshot.size());
    std::size_t corrupted_size = valid_snapshot.size();
    mutate_snapshot(corrupted, corrupted_size, selector);

    const Status status =
        restore(target, std::span<const std::byte>(corrupted.data(), corrupted_size));
    CHECK(status != Status::Accepted);

    std::array<std::byte, kSnapshotBufferSize> after{};
    const SnapshotWriteResult after_result = snapshot_engine(target, after);
    CHECK(after_result.bytes_written == before_result.bytes_written);
    CHECK(std::memcmp(before.data(), after.data(), before_result.bytes_written) == 0);
}

void checkpoint_engine(const MatchingEngine& source,
                       MatchingEngine& restored,
                       const bool test_corrupt_snapshot,
                       const std::uint8_t mutation_selector)
{
    std::array<std::byte, kSnapshotBufferSize> snapshot{};
    const SnapshotWriteResult snapshot_result = snapshot_engine(source, snapshot);
    const std::span<const std::byte> bytes(snapshot.data(), snapshot_result.bytes_written);

    if (test_corrupt_snapshot) {
        verify_failed_restore_is_atomic(restored, bytes, mutation_selector);
    }

    CHECK(restore(restored, bytes) == Status::Accepted);
    CHECK(restored.last_events(kInstrumentA).empty());
    CHECK(restored.last_events(kInstrumentB).empty());
    check_engine_snapshots_equal(source, restored, "stateful_snapshot_restore");
}

void execute_command(MatchingEngine& dense,
                     MatchingEngine& sparse,
                     MatchingEngine& restored_dense,
                     MatchingEngine& restored_sparse,
                     const bool restored_active,
                     ReferenceOrderBook& reference_a,
                     ReferenceOrderBook& reference_b,
                     const Command& command,
                     const std::uint32_t command_index)
{
    std::array<DispatchResult, 4> actual_results{};
    actual_results[0] = dense.dispatch(command);
    actual_results[1] = sparse.dispatch(command);
    std::size_t actual_count = 2;
    if (restored_active) {
        actual_results[2] = restored_dense.dispatch(command);
        actual_results[3] = restored_sparse.dispatch(command);
        actual_count = actual_results.size();
    }

    DispatchResult expected{};
    if (configured_instrument(command.instrument_id)) {
        ReferenceOrderBook& reference =
            command.instrument_id == kInstrumentA ? reference_a : reference_b;
        expected = dispatch_reference(reference, command);
    } else {
        expected.status = Status::UnknownInstrument;
    }

    for (std::size_t index = 0; index < actual_count; ++index) {
        if (actual_results[index].status != expected.status) {
            std::fprintf(stderr,
                         "command=%u engine=%zu op=%u instrument=%u id=%llu side=%u "
                         "price=%lld quantity=%llu tif=%u timestamp=%llu\n",
                         command_index,
                         index,
                         static_cast<unsigned>(command.op),
                         command.instrument_id,
                         static_cast<unsigned long long>(command.order_id),
                         static_cast<unsigned>(command.side),
                         static_cast<long long>(command.price),
                         static_cast<unsigned long long>(command.quantity),
                         static_cast<unsigned>(command.time_in_force),
                         static_cast<unsigned long long>(command.timestamp));
        }
        check_dispatch_results_equal(
            actual_results[index], expected, "stateful_differential_dispatch");
    }
    check_dispatch_results_equal(
        actual_results[0], actual_results[1], "dense_sparse_dispatch");
    if (restored_active) {
        check_dispatch_results_equal(
            actual_results[0], actual_results[2], "dense_restored_dispatch");
        check_dispatch_results_equal(
            actual_results[1], actual_results[3], "sparse_restored_dispatch");
    }

    check_engine_state(dense, reference_a, reference_b);
    check_engine_state(sparse, reference_a, reference_b);
    if (restored_active) {
        check_engine_state(restored_dense, reference_a, reference_b);
        check_engine_state(restored_sparse, reference_a, reference_b);
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, const std::size_t size)
{
    if (data == nullptr) {
        return 0;
    }

    InputReader reader(std::span<const std::uint8_t>(data, size));
    const HarnessConfig config = decode_config(reader);
    CHECK(config.dense.valid());
    CHECK(config.sparse.valid());

    const std::array<InstrumentConfig, 2> dense_configs{{
        InstrumentConfig{kInstrumentA, config.dense, config.dense.tick_size, 1},
        InstrumentConfig{kInstrumentB, config.dense, config.dense.tick_size, 1},
    }};
    const std::array<InstrumentConfig, 2> sparse_configs{{
        InstrumentConfig{kInstrumentA, config.sparse, config.sparse.tick_size, 1},
        InstrumentConfig{kInstrumentB, config.sparse, config.sparse.tick_size, 1},
    }};

    MatchingEngine dense(dense_configs);
    MatchingEngine sparse(sparse_configs);
    MatchingEngine restored_dense(dense_configs);
    MatchingEngine restored_sparse(sparse_configs);
    ReferenceOrderBook reference_a(config.dense, kInstrumentA);
    ReferenceOrderBook reference_b(config.dense, kInstrumentB);
    CHECK(dense.valid());
    CHECK(sparse.valid());
    CHECK(restored_dense.valid());
    CHECK(restored_sparse.valid());

    bool restored_active = false;
    std::uint32_t command_index = 0;
    while (command_index < kMaxCommands) {
        DecodedCommand decoded{};
        if (!decode_command(reader, config.dense, decoded)) {
            break;
        }

        const bool periodic_snapshot =
            command_index % config.snapshot_period == config.snapshot_phase;
        if (periodic_snapshot || decoded.force_snapshot) {
            checkpoint_engine(dense,
                              restored_dense,
                              decoded.test_corrupt_snapshot,
                              decoded.mutation_selector);
            checkpoint_engine(sparse,
                              restored_sparse,
                              decoded.test_corrupt_snapshot,
                              decoded.mutation_selector);
            restored_active = true;
            check_engine_state(restored_dense, reference_a, reference_b);
            check_engine_state(restored_sparse, reference_a, reference_b);
        }

        execute_command(dense,
                        sparse,
                        restored_dense,
                        restored_sparse,
                        restored_active,
                        reference_a,
                        reference_b,
                        decoded.command,
                        command_index);
        ++command_index;
    }

    return 0;
}
