#pragma once

#include "Order.hpp"

#include <algorithm>
#include <limits>

namespace eigenbook {

class alignas(64) PriceLevel final {
public:
    PriceLevel() noexcept = default;

    void reset(const Price new_price) noexcept
    {
        price_ = new_price;
        total_quantity_ = 0;
        order_count_ = 0;
        head_ = nullptr;
        tail_ = nullptr;
    }

    [[nodiscard]] Price price() const noexcept
    {
        return price_;
    }

    [[nodiscard]] Quantity total_quantity() const noexcept
    {
        return total_quantity_;
    }

    [[nodiscard]] std::uint32_t order_count() const noexcept
    {
        return order_count_;
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return head_ == nullptr;
    }

    [[nodiscard]] Order* front() noexcept
    {
        return head_;
    }

    [[nodiscard]] const Order* front() const noexcept
    {
        return head_;
    }

    [[nodiscard]] Order* back() noexcept
    {
        return tail_;
    }

    [[nodiscard]] const Order* back() const noexcept
    {
        return tail_;
    }

    [[nodiscard]] bool append(Order& order) noexcept
    {
        if (order.quantity == 0 || order.level != nullptr || order.prev != nullptr || order.next != nullptr) {
            return false;
        }

        if (std::numeric_limits<Quantity>::max() - total_quantity_ < order.quantity) {
            return false;
        }

        order.level = this;
        order.prev = tail_;
        order.next = nullptr;
        order.state = OrderState::Resting;

        if (tail_ != nullptr) {
            tail_->next = &order;
        } else {
            head_ = &order;
        }

        tail_ = &order;
        total_quantity_ += order.quantity;
        ++order_count_;
        return true;
    }

    [[nodiscard]] bool remove(Order& order) noexcept
    {
        if (order.level != this || order_count_ == 0 || order.quantity > total_quantity_) {
            return false;
        }

        if (order.prev != nullptr) {
            order.prev->next = order.next;
        } else {
            head_ = order.next;
        }

        if (order.next != nullptr) {
            order.next->prev = order.prev;
        } else {
            tail_ = order.prev;
        }

        total_quantity_ -= order.quantity;
        --order_count_;

        order.prev = nullptr;
        order.next = nullptr;
        order.level = nullptr;
        return true;
    }

    [[nodiscard]] bool set_quantity_keep_priority(Order& order, const Quantity new_quantity) noexcept
    {
        if (order.level != this || new_quantity == 0 || new_quantity > order.quantity) {
            return false;
        }

        const Quantity delta = order.quantity - new_quantity;
        if (delta > total_quantity_) {
            return false;
        }

        order.quantity = new_quantity;
        total_quantity_ -= delta;
        return true;
    }

    [[nodiscard]] Quantity fill_front(const Quantity requested_quantity) noexcept
    {
        if (head_ == nullptr || requested_quantity == 0) {
            return 0;
        }

        const Quantity executed_quantity = std::min(requested_quantity, head_->quantity);
        head_->quantity -= executed_quantity;
        total_quantity_ -= executed_quantity;
        head_->state = head_->quantity == 0 ? OrderState::Filled : OrderState::PartiallyFilled;
        return executed_quantity;
    }

private:
    Price price_{0};
    Quantity total_quantity_{0};
    std::uint32_t order_count_{0};
    Order* head_{nullptr};
    Order* tail_{nullptr};
};

static_assert(alignof(PriceLevel) >= 64);

} // namespace eigenbook
