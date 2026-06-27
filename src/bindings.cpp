#include "Command.hpp"
#include "MatchingEngine.hpp"
#include "Types.hpp"

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace py = pybind11;

namespace eigenbook {
namespace {

static_assert(std::is_standard_layout_v<TradeEvent>);
static_assert(std::is_trivially_copyable_v<TradeEvent>);
static_assert(std::is_standard_layout_v<BookEvent>);
static_assert(std::is_trivially_copyable_v<BookEvent>);

inline constexpr std::size_t kPythonDepthLevelCapacity = 64;

struct PythonDispatchResult final {
    Status status{Status::InternalError};
    Quantity accepted_quantity{0};
    Quantity requested_quantity{0};
    Quantity executed_quantity{0};
    Quantity remaining_quantity{0};
    Quantity resting_quantity{0};
    Quantity canceled_quantity{0};
    Quantity old_quantity{0};
    Quantity new_quantity{0};
    Price old_price{0};
    Price new_price{0};
    std::uint32_t fills{0};
    bool has_last_price{false};
    Price last_price{0};
    std::uint32_t events_emitted{0};
};

[[nodiscard]] PythonDispatchResult copy_dispatch_result(
    const DispatchResult& source) noexcept
{
    return PythonDispatchResult{
        source.status,
        source.accepted_quantity,
        source.requested_quantity,
        source.executed_quantity,
        source.remaining_quantity,
        source.resting_quantity,
        source.canceled_quantity,
        source.old_quantity,
        source.new_quantity,
        source.old_price,
        source.new_price,
        source.fills,
        source.has_last_price,
        source.last_price,
        source.events_emitted,
    };
}

[[nodiscard]] std::unique_ptr<MatchingEngine> make_matching_engine(const py::sequence& configs)
{
    const py::ssize_t python_count = py::len(configs);
    if (python_count < 0 ||
        static_cast<std::uint64_t>(python_count) > std::numeric_limits<std::uint32_t>::max()) {
        throw py::value_error("instrument config count exceeds uint32 capacity");
    }

    const auto count = static_cast<std::size_t>(python_count);

    // This exact-size conversion array exists only during engine construction.
    // dispatch() and top_of_book() perform no Python container conversion.
    std::unique_ptr<InstrumentConfig[]> native_configs =
        count == 0 ? nullptr : std::make_unique<InstrumentConfig[]>(count);
    for (std::size_t index = 0; index < count; ++index) {
        try {
            native_configs[index] = py::cast<InstrumentConfig>(configs[index]);
        } catch (const py::cast_error&) {
            throw py::type_error(
                std::string("instrument config at index ") +
                std::to_string(index) + " is not an InstrumentConfig");
        }
    }

    MatchingEngineCreateResult result = MatchingEngine::create(
        static_cast<std::uint32_t>(count),
        std::span<const InstrumentConfig>(native_configs.get(), count));
    if (!result) {
        const std::string message =
            std::string("matching engine configuration rejected at index ") +
            std::to_string(result.config_index) + ": " +
            matching_engine_init_error_name(result.error);
        if (result.error == MatchingEngineInitError::AllocationFailure) {
            PyErr_NoMemory();
            throw py::error_already_set();
        }
        if (result.error == MatchingEngineInitError::InternalInsertionFailure) {
            throw std::runtime_error(message);
        }
        throw py::value_error(message);
    }

    return std::move(result.engine);
}

template <typename Element>
void require_aligned_buffer(const py::array_t<Element, py::array::c_style>& buffer,
                            const char* const name)
{
    const auto address = reinterpret_cast<std::uintptr_t>(buffer.data());
    if (address % alignof(Element) != 0U) {
        throw py::value_error(std::string(name) + " data is not naturally aligned");
    }
}

void validate_event_buffer(
    const MatchingEngine& engine,
    const InstrumentId instrument_id,
    const py::array_t<BookEvent, py::array::c_style>& event_buffer)
{
    if (event_buffer.ndim() != 1) {
        throw py::value_error("event_buffer must be one-dimensional");
    }
    if (!event_buffer.writeable()) {
        throw py::value_error("event_buffer must be writable");
    }
    require_aligned_buffer(event_buffer, "event_buffer");

    // Requiring the full configured event-log capacity guarantees that the
    // buffer-size check happens before dispatch mutates the book.
    const OrderBook* const book = engine.order_book(instrument_id);
    if (book != nullptr &&
        static_cast<std::uint64_t>(event_buffer.shape(0)) < book->event_log_capacity()) {
        throw py::value_error("event_buffer is smaller than the instrument event-log capacity");
    }
}

[[nodiscard]] DispatchResult dispatch_and_copy_events(
    MatchingEngine& engine,
    const Command& command,
    py::array_t<BookEvent, py::array::c_style> event_buffer)
{
    validate_event_buffer(engine, command.instrument_id, event_buffer);

    const DispatchResult result = engine.dispatch(command);
    if (!result.events.empty()) {
        std::memcpy(event_buffer.mutable_data(), result.events.data(), result.events.size_bytes());
    }
    return result;
}

[[nodiscard]] std::uint32_t dispatch_with_buffer(
    MatchingEngine& engine,
    const Command& command,
    py::array_t<BookEvent, py::array::c_style> event_buffer)
{
    return dispatch_and_copy_events(engine, command, std::move(event_buffer)).events_emitted;
}

[[nodiscard]] PythonDispatchResult dispatch_result_with_buffer(
    MatchingEngine& engine,
    const Command& command,
    py::array_t<BookEvent, py::array::c_style> event_buffer)
{
    return copy_dispatch_result(
        dispatch_and_copy_events(engine, command, std::move(event_buffer)));
}

[[nodiscard]] PythonDispatchResult dispatch_command(
    MatchingEngine& engine,
    const Command& command)
{
    return copy_dispatch_result(engine.dispatch(command));
}

[[nodiscard]] std::uint32_t event_buffer_capacity(
    const MatchingEngine& engine,
    const InstrumentId instrument_id)
{
    const OrderBook* const book = engine.order_book(instrument_id);
    if (book == nullptr) {
        throw py::key_error(
            std::string("unknown instrument id: ") + std::to_string(instrument_id));
    }
    return book->event_log_capacity();
}

void depth_with_buffer(
    const MatchingEngine& engine,
    const InstrumentId instrument_id,
    const Side side,
    py::array_t<float, py::array::c_style> depth_buffer)
{
    if (side != Side::Buy && side != Side::Sell) {
        throw py::value_error("side must be BUY or SELL");
    }
    if (depth_buffer.ndim() != 2 || depth_buffer.shape(1) != 2) {
        throw py::value_error("depth_buffer must have shape (levels, 2)");
    }
    if (!depth_buffer.writeable()) {
        throw py::value_error("depth_buffer must be writable");
    }
    require_aligned_buffer(depth_buffer, "depth_buffer");

    const py::ssize_t requested_levels = depth_buffer.shape(0);
    if (requested_levels < 0 ||
        static_cast<std::size_t>(requested_levels) > kPythonDepthLevelCapacity) {
        throw py::value_error("depth_buffer supports at most 64 levels");
    }

    float* const output = depth_buffer.mutable_data();
    std::fill_n(output, static_cast<std::size_t>(requested_levels) * 2U, 0.0F);

    std::array<DepthLevel, kPythonDepthLevelCapacity> levels{};
    const auto max_levels = static_cast<std::uint32_t>(requested_levels);
    const std::uint32_t written = engine.depth(instrument_id, side, max_levels, levels.data());
    for (std::uint32_t index = 0; index < written; ++index) {
        const std::size_t output_index = static_cast<std::size_t>(index) * 2U;
        output[output_index] = static_cast<float>(levels[index].price);
        output[output_index + 1U] = static_cast<float>(levels[index].aggregate_quantity);
    }
}

[[nodiscard]] double reward_from_events(
    const MatchingEngine&,
    py::array_t<BookEvent, py::array::c_style> event_buffer,
    const std::uint32_t event_count,
    const Quantity requested_quantity)
{
    if (event_buffer.ndim() != 1) {
        throw py::value_error("event_buffer must be one-dimensional");
    }
    require_aligned_buffer(event_buffer, "event_buffer");
    if (static_cast<std::uint64_t>(event_count) >
        static_cast<std::uint64_t>(event_buffer.shape(0))) {
        throw py::value_error("event_count exceeds event_buffer capacity");
    }

    Quantity executed_quantity = 0;
    const BookEvent* const events = event_buffer.data();
    for (std::uint32_t index = 0; index < event_count; ++index) {
        if (events[index].kind != BookEvent::Kind::Trade) {
            continue;
        }

        const Quantity fill_quantity = events[index].trade.quantity;
        const Quantity remaining = requested_quantity - executed_quantity;
        executed_quantity += fill_quantity > remaining ? remaining : fill_quantity;
    }

    const Quantity residual_quantity = requested_quantity - executed_quantity;
    return static_cast<double>(executed_quantity) - static_cast<double>(residual_quantity);
}

void bind_enums(py::module_& module)
{
    py::enum_<Side>(module, "Side")
        .value("BUY", Side::Buy)
        .value("SELL", Side::Sell);

    py::enum_<TimeInForce>(module, "TimeInForce")
        .value("GTC", TimeInForce::Gtc)
        .value("IOC", TimeInForce::Ioc)
        .value("FOK", TimeInForce::Fok);

    py::enum_<PriceLevelMode>(module, "PriceLevelMode")
        .value("DENSE", PriceLevelMode::Dense)
        .value("SPARSE", PriceLevelMode::Sparse);

    py::enum_<CommandOp>(module, "CommandOp")
        .value("ADD", CommandOp::Add)
        .value("CANCEL", CommandOp::Cancel)
        .value("MODIFY", CommandOp::Modify)
        .value("REPLACE", CommandOp::Replace)
        .value("MARKET", CommandOp::Market);

    py::enum_<BookEvent::Kind>(module, "BookEventKind")
        .value("TRADE", BookEvent::Kind::Trade)
        .value("ORDER_ACCEPTED", BookEvent::Kind::OrderAccepted)
        .value("ORDER_RESTING", BookEvent::Kind::OrderResting)
        .value("ORDER_CANCELLED", BookEvent::Kind::OrderCancelled)
        .value("ORDER_MODIFIED", BookEvent::Kind::OrderModified)
        .value("ORDER_REJECTED", BookEvent::Kind::OrderRejected);

    py::enum_<Status>(module, "Status")
        .value("ACCEPTED", Status::Accepted)
        .value("REJECTED", Status::Rejected)
        .value("CANCELLED", Status::Cancelled)
        .value("FILLED", Status::Filled)
        .value("PARTIALLY_FILLED", Status::PartiallyFilled)
        .value("NO_LIQUIDITY", Status::NoLiquidity)
        .value("INVALID_ORDER_ID", Status::InvalidOrderId)
        .value("UNKNOWN_ORDER_ID", Status::UnknownOrderId)
        .value("UNKNOWN_INSTRUMENT", Status::UnknownInstrument)
        .value("INVALID_QUANTITY", Status::InvalidQuantity)
        .value("INVALID_PRICE", Status::InvalidPrice)
        .value("DUPLICATE_ORDER_ID", Status::DuplicateOrderId)
        .value("POOL_EXHAUSTED", Status::PoolExhausted)
        .value("ORDER_ID_MAP_FULL", Status::OrderIdMapFull)
        .value("QUANTITY_INCREASE_REJECTED", Status::QuantityIncreaseRejected)
        .value("INVALID_CONFIGURATION", Status::InvalidConfiguration)
        .value("INTERNAL_ERROR", Status::InternalError)
        .value("FOK_REJECTED", Status::FokRejected)
        .value("BUFFER_TOO_SMALL", Status::BufferTooSmall)
        .value("SNAPSHOT_FORMAT_MISMATCH", Status::SnapshotFormatMismatch)
        .value("SNAPSHOT_VERSION_MISMATCH", Status::SnapshotVersionMismatch)
        .value("SNAPSHOT_CONFIGURATION_MISMATCH", Status::SnapshotConfigurationMismatch)
        .value("SNAPSHOT_CAPACITY_EXCEEDED", Status::SnapshotCapacityExceeded)
        .value("INVALID_COMMAND", Status::InvalidCommand)
        .value("EVENT_LOG_FULL", Status::EventLogFull);
}

void bind_configuration(py::module_& module)
{
    py::class_<BookConfig>(module, "BookConfig")
        .def(py::init<>())
        .def_readwrite("min_price", &BookConfig::min_price)
        .def_readwrite("max_price", &BookConfig::max_price)
        .def_readwrite("max_orders", &BookConfig::max_orders)
        .def_readwrite("order_id_map_capacity", &BookConfig::order_id_map_capacity)
        .def_readwrite("tick_size", &BookConfig::tick_size)
        .def_readwrite("event_log_capacity", &BookConfig::event_log_capacity)
        .def_readwrite("price_level_mode", &BookConfig::price_level_mode);

    py::class_<InstrumentConfig>(module, "InstrumentConfig")
        .def(py::init<>())
        .def_readwrite("instrument_id", &InstrumentConfig::instrument_id)
        .def_readwrite("book_config", &InstrumentConfig::book_config)
        .def_readwrite("tick_size", &InstrumentConfig::tick_size)
        .def_readwrite("lot_size", &InstrumentConfig::lot_size);
}

void bind_command_and_results(py::module_& module)
{
    py::class_<Command>(module, "Command")
        .def(py::init<>())
        .def_readwrite("instrument_id", &Command::instrument_id)
        .def_readwrite("op", &Command::op)
        .def_readwrite("order_id", &Command::order_id)
        .def_readwrite("side", &Command::side)
        .def_readwrite("price", &Command::price)
        .def_readwrite("quantity", &Command::quantity)
        .def_readwrite("time_in_force", &Command::time_in_force)
        .def_readwrite("timestamp", &Command::timestamp);

    py::class_<PythonDispatchResult>(module, "DispatchResult")
        .def_readonly("status", &PythonDispatchResult::status)
        .def_readonly("accepted_quantity", &PythonDispatchResult::accepted_quantity)
        .def_readonly("requested_quantity", &PythonDispatchResult::requested_quantity)
        .def_readonly("executed_quantity", &PythonDispatchResult::executed_quantity)
        .def_readonly("remaining_quantity", &PythonDispatchResult::remaining_quantity)
        .def_readonly("resting_quantity", &PythonDispatchResult::resting_quantity)
        .def_readonly("canceled_quantity", &PythonDispatchResult::canceled_quantity)
        .def_readonly("old_quantity", &PythonDispatchResult::old_quantity)
        .def_readonly("new_quantity", &PythonDispatchResult::new_quantity)
        .def_readonly("old_price", &PythonDispatchResult::old_price)
        .def_readonly("new_price", &PythonDispatchResult::new_price)
        .def_readonly("fills", &PythonDispatchResult::fills)
        .def_readonly("has_last_price", &PythonDispatchResult::has_last_price)
        .def_readonly("last_price", &PythonDispatchResult::last_price)
        .def_readonly("events_emitted", &PythonDispatchResult::events_emitted);

    py::class_<BestQuote>(module, "BestQuote")
        .def_readonly("valid", &BestQuote::valid)
        .def_readonly("price", &BestQuote::price)
        .def_readonly("quantity", &BestQuote::quantity)
        .def_readonly("order_count", &BestQuote::order_count);

    py::class_<TopOfBook>(module, "TopOfBook")
        .def_readonly("status", &TopOfBook::status)
        .def_readonly("bid", &TopOfBook::bid)
        .def_readonly("ask", &TopOfBook::ask);
}

} // namespace
} // namespace eigenbook

PYBIND11_MODULE(_eigenbook, module)
{
    module.doc() = "Python bindings for the fixed-capacity Eigen-Book matching engine";
    module.attr("__version__") = EIGENBOOK_PYTHON_VERSION;
    module.attr("__compiler__") = EIGENBOOK_PYTHON_COMPILER;
    module.attr("__build_type__") = EIGENBOOK_PYTHON_BUILD_TYPE;

    PYBIND11_NUMPY_DTYPE(eigenbook::TradeEvent,
                         instrument_id,
                         aggressor_id,
                         resting_id,
                         aggressor_side,
                         price,
                         quantity,
                         timestamp,
                         sequence);
    PYBIND11_NUMPY_DTYPE(eigenbook::BookEvent,
                         kind,
                         instrument_id,
                         status,
                         order_id,
                         side,
                         price,
                         quantity,
                         old_quantity,
                         new_quantity,
                         timestamp,
                         sequence,
                         time_in_force,
                         trade);

    module.attr("TRADE_EVENT_DTYPE") = py::dtype::of<eigenbook::TradeEvent>();
    module.attr("BOOK_EVENT_DTYPE") = py::dtype::of<eigenbook::BookEvent>();
    module.attr("TRADE_EVENT_ALIGNMENT") = alignof(eigenbook::TradeEvent);
    module.attr("BOOK_EVENT_ALIGNMENT") = alignof(eigenbook::BookEvent);

    eigenbook::bind_enums(module);
    eigenbook::bind_configuration(module);
    eigenbook::bind_command_and_results(module);

    py::class_<eigenbook::MatchingEngine>(module, "MatchingEngine")
        .def(py::init(&eigenbook::make_matching_engine), py::arg("configs"))
        .def("dispatch",
             &eigenbook::dispatch_command,
             py::arg("command"))
        .def("dispatch_with_buffer",
             &eigenbook::dispatch_with_buffer,
             py::arg("command"),
             py::arg("event_buffer").noconvert())
        .def("dispatch_result_with_buffer",
             &eigenbook::dispatch_result_with_buffer,
             py::arg("command"),
             py::arg("event_buffer").noconvert())
        .def("event_buffer_capacity",
             &eigenbook::event_buffer_capacity,
             py::arg("instrument_id"))
        .def("depth",
             &eigenbook::depth_with_buffer,
             py::arg("instrument_id"),
             py::arg("side"),
             py::arg("depth_buffer").noconvert())
        .def("reward_from_events",
             &eigenbook::reward_from_events,
             py::arg("event_buffer").noconvert(),
             py::arg("event_count"),
             py::arg("requested_quantity"))
        .def("top_of_book", &eigenbook::MatchingEngine::top_of_book, py::arg("instrument_id"));

    module.def(
        "_binding_round_trip",
        [](const std::uint64_t value) noexcept { return value; },
        py::arg("value"));
}
