#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace eigenbook {

template <typename T>
class alignas(64) MemoryPool final {
public:
    explicit MemoryPool(const std::uint32_t capacity)
        : slots_(capacity == 0 ? nullptr : std::make_unique<Slot[]>(capacity)),
          capacity_(capacity)
    {
        reset_free_list();
    }

    MemoryPool(const MemoryPool&) = delete;
    MemoryPool& operator=(const MemoryPool&) = delete;
    MemoryPool(MemoryPool&&) = delete;
    MemoryPool& operator=(MemoryPool&&) = delete;

    ~MemoryPool() noexcept
    {
        destroy_live_objects();
    }

    void clear() noexcept
    {
        destroy_live_objects();
        reset_free_list();
    }

    template <typename... Args>
    [[nodiscard]] T* allocate(Args&&... args) noexcept
    {
        static_assert(std::is_nothrow_constructible_v<T, Args...>,
                      "MemoryPool hot-path allocation requires nothrow construction");

        if (free_head_ == kNoSlot) {
            return nullptr;
        }

        const std::uint32_t index = free_head_;
        Slot& slot = slots_[index];
        free_head_ = slot.next;
        slot.in_use = true;
        ++size_;

        return std::construct_at(ptr_at(index), std::forward<Args>(args)...);
    }

    void release(T* object) noexcept
    {
        static_assert(std::is_nothrow_destructible_v<T>,
                      "MemoryPool hot-path release requires nothrow destruction");

        const std::uint32_t index = slot_index(object);
        if (index == kNoSlot || !slots_[index].in_use) {
            return;
        }

        std::destroy_at(object);
        slots_[index].in_use = false;
        slots_[index].next = free_head_;
        free_head_ = index;
        --size_;
    }

    [[nodiscard]] bool owns(const T* object) const noexcept
    {
        return slot_index(object) != kNoSlot;
    }

    [[nodiscard]] std::uint32_t capacity() const noexcept
    {
        return capacity_;
    }

    [[nodiscard]] std::uint32_t size() const noexcept
    {
        return size_;
    }

    [[nodiscard]] std::uint32_t available() const noexcept
    {
        return capacity_ - size_;
    }

private:
    static constexpr std::uint32_t kNoSlot = std::numeric_limits<std::uint32_t>::max();

    struct alignas(T) Slot final {
        alignas(T) std::byte storage[sizeof(T)]{};
        std::uint32_t next{kNoSlot};
        bool in_use{false};
    };

    std::unique_ptr<Slot[]> slots_;
    std::uint32_t capacity_{0};
    std::uint32_t free_head_{kNoSlot};
    std::uint32_t size_{0};

    void reset_free_list() noexcept
    {
        size_ = 0;
        if (capacity_ == 0) {
            free_head_ = kNoSlot;
            return;
        }

        for (std::uint32_t i = 0; i < capacity_; ++i) {
            slots_[i].next = (i + 1U < capacity_) ? i + 1U : kNoSlot;
            slots_[i].in_use = false;
        }

        free_head_ = 0;
    }

    void destroy_live_objects() noexcept
    {
        if (slots_ == nullptr) {
            return;
        }

        for (std::uint32_t i = 0; i < capacity_; ++i) {
            if (slots_[i].in_use) {
                std::destroy_at(ptr_at(i));
                slots_[i].in_use = false;
            }
        }
    }

    [[nodiscard]] T* ptr_at(const std::uint32_t index) noexcept
    {
        return std::launder(reinterpret_cast<T*>(slots_[index].storage));
    }

    [[nodiscard]] const T* ptr_at(const std::uint32_t index) const noexcept
    {
        return std::launder(reinterpret_cast<const T*>(slots_[index].storage));
    }

    [[nodiscard]] std::uint32_t slot_index(const T* object) const noexcept
    {
        if (object == nullptr || slots_ == nullptr || capacity_ == 0) {
            return kNoSlot;
        }

        const auto base = reinterpret_cast<std::uintptr_t>(slots_.get());
        const auto value = reinterpret_cast<std::uintptr_t>(object);
        const auto storage_offset = static_cast<std::uintptr_t>(offsetof(Slot, storage));
        const auto stride = static_cast<std::uintptr_t>(sizeof(Slot));
        const auto begin = base + storage_offset;
        const auto end = begin + stride * static_cast<std::uintptr_t>(capacity_);

        if (value < begin || value >= end) {
            return kNoSlot;
        }

        const auto delta = value - begin;
        if (delta % stride != 0) {
            return kNoSlot;
        }

        const auto index = static_cast<std::uint32_t>(delta / stride);
        return ptr_at(index) == object ? index : kNoSlot;
    }
};

static_assert(alignof(MemoryPool<int>) >= 64);

} // namespace eigenbook
