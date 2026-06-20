#include "Command.hpp"
#include "MatchingEngine.hpp"
#include "Types.hpp"

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>

#include <cstring>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <type_traits>

namespace py = pybind11;

namespace eigenbook {
namespace {

static_assert(std::is_standard_layout_v<TradeEvent>);
static_assert(std::is_trivially_copyable_v<TradeEvent>);
static_assert(std::is_standard_layout_v<BookEvent>);
static_assert(std::is_trivially_copyable_v<BookEvent>);

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
        native_configs[index] = py::cast<InstrumentConfig>(configs[index]);
    }

    return std::make_unique<MatchingEngine>(
        static_cast<std::uint32_t>(count),
        std::span<const InstrumentConfig>(native_configs.get(), count));
}

[[nodiscard]] std::uint32_t dispatch_with_buffer(
    MatchingEngine& engine,
    const Command& command,
    py::array_t<BookEvent, py::array::c_style> event_buffer)
{
    if (event_buffer.ndim() != 1) {
        throw py::value_error("event_buffer must be one-dimensional");
    }
    if (!event_buffer.writeable()) {
        throw py::value_error("event_buffer must be writable");
    }

    // Requiring the full configured event-log capacity guarantees that the
    // buffer-size check happens before dispatch mutates the book.
    const OrderBook* const book = engine.order_book(command.instrument_id);
    if (book != nullptr &&
        static_cast<std::uint64_t>(event_buffer.shape(0)) < book->event_log_capacity()) {
        throw py::value_error("event_buffer is smaller than the instrument event-log capacity");
    }

    const DispatchResult result = engine.dispatch(command);
    if (!result.events.empty()) {
        std::memcpy(event_buffer.mutable_data(), result.events.data(), result.events.size_bytes());
    }
    return static_cast<std::uint32_t>(result.events.size());
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

    py::class_<DispatchResult>(module, "DispatchResult")
        .def_readonly("status", &DispatchResult::status)
        .def_readonly("accepted_quantity", &DispatchResult::accepted_quantity)
        .def_readonly("requested_quantity", &DispatchResult::requested_quantity)
        .def_readonly("executed_quantity", &DispatchResult::executed_quantity)
        .def_readonly("remaining_quantity", &DispatchResult::remaining_quantity)
        .def_readonly("resting_quantity", &DispatchResult::resting_quantity)
        .def_readonly("canceled_quantity", &DispatchResult::canceled_quantity)
        .def_readonly("old_quantity", &DispatchResult::old_quantity)
        .def_readonly("new_quantity", &DispatchResult::new_quantity)
        .def_readonly("old_price", &DispatchResult::old_price)
        .def_readonly("new_price", &DispatchResult::new_price)
        .def_readonly("fills", &DispatchResult::fills)
        .def_readonly("has_last_price", &DispatchResult::has_last_price)
        .def_readonly("last_price", &DispatchResult::last_price)
        .def_readonly("events_emitted", &DispatchResult::events_emitted);

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

PYBIND11_MODULE(eigenbook_py, module)
{
    module.doc() = "Python bindings for the fixed-capacity Eigen-Book matching engine";

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

    eigenbook::bind_enums(module);
    eigenbook::bind_configuration(module);
    eigenbook::bind_command_and_results(module);

    py::class_<eigenbook::MatchingEngine>(module, "MatchingEngine")
        .def(py::init(&eigenbook::make_matching_engine), py::arg("configs"))
        .def("dispatch",
             py::overload_cast<const eigenbook::Command&>(&eigenbook::MatchingEngine::dispatch),
             py::arg("command"))
        .def("dispatch_with_buffer",
             &eigenbook::dispatch_with_buffer,
             py::arg("command"),
             py::arg("event_buffer").noconvert())
        .def("top_of_book", &eigenbook::MatchingEngine::top_of_book, py::arg("instrument_id"));
}
