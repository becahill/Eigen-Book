#pragma once

#include "Types.hpp"

namespace eigenbook {

class PriceLevel;

struct alignas(64) Order final {
    OrderId id{kInvalidOrderId};
    Price price{0};
    Quantity quantity{0};
    /// Quantity with which this order most recently joined its resting FIFO.
    /// Persistent across fills/reductions and snapshot round trips.
    Quantity initial_quantity{0};
    Timestamp timestamp{0};
    SequenceNumber sequence{0};
    ParticipantId participant_id{kAnonymousParticipantId};
    Side side{Side::Buy};
    /// Persistent lifecycle state for a live order. Snapshot restore preserves
    /// `Resting` and `PartiallyFilled` exactly.
    OrderState state{OrderState::Inactive};
    Order* prev{nullptr};
    Order* next{nullptr};
    PriceLevel* level{nullptr};
    bool post_only{false};
    bool active{false};

    Order() noexcept = default;

    void reset(const OrderId new_id,
               const Side new_side,
               const Price new_price,
               const Quantity new_quantity,
               const Timestamp new_timestamp = 0,
               const SequenceNumber new_sequence = 0,
               const ParticipantId new_participant_id = kAnonymousParticipantId,
               const bool new_post_only = false) noexcept
    {
        id = new_id;
        side = new_side;
        price = new_price;
        quantity = new_quantity;
        initial_quantity = new_quantity;
        timestamp = new_timestamp;
        sequence = new_sequence;
        participant_id = new_participant_id;
        state = OrderState::Resting;
        prev = nullptr;
        next = nullptr;
        level = nullptr;
        post_only = new_post_only;
        active = true;
    }

    void clear() noexcept
    {
        id = kInvalidOrderId;
        price = 0;
        quantity = 0;
        initial_quantity = 0;
        timestamp = 0;
        sequence = 0;
        participant_id = kAnonymousParticipantId;
        side = Side::Buy;
        state = OrderState::Inactive;
        prev = nullptr;
        next = nullptr;
        level = nullptr;
        post_only = false;
        active = false;
    }

    [[nodiscard]] bool linked() const noexcept
    {
        return level != nullptr;
    }
};

static_assert(alignof(Order) >= 64);

} // namespace eigenbook
