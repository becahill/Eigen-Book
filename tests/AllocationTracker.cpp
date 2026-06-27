#include "AllocationTracker.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <new>

namespace eigenbook::test::allocation_tracker {
namespace {

std::atomic<bool> tracking_enabled{false};
std::atomic<std::uint64_t> allocation_count{0};

void record_allocation() noexcept
{
    if (tracking_enabled.load(std::memory_order_relaxed)) {
        allocation_count.fetch_add(1, std::memory_order_relaxed);
    }
}

[[nodiscard]] std::size_t nonzero_size(const std::size_t size) noexcept
{
    return size == 0U ? 1U : size;
}

[[nodiscard]] void* try_allocate_unaligned(const std::size_t size) noexcept
{
    return std::malloc(nonzero_size(size));
}

[[nodiscard]] void* try_allocate_aligned(const std::size_t size,
                                         const std::size_t alignment) noexcept
{
    const std::size_t allocation_size = nonzero_size(size);
    if (alignment < alignof(void*) || (alignment & (alignment - 1U)) != 0U) {
        return nullptr;
    }

    constexpr std::size_t pointer_storage = sizeof(void*);
    if (allocation_size >
        std::numeric_limits<std::size_t>::max() - pointer_storage - (alignment - 1U)) {
        return nullptr;
    }

    const std::size_t raw_size = allocation_size + pointer_storage + alignment - 1U;
    void* const raw = std::malloc(raw_size);
    if (raw == nullptr) {
        return nullptr;
    }

    const auto begin = reinterpret_cast<std::uintptr_t>(raw) + pointer_storage;
    const auto aligned = (begin + alignment - 1U) & ~(static_cast<std::uintptr_t>(alignment) - 1U);
    void* const result = reinterpret_cast<void*>(aligned);
    reinterpret_cast<void**>(result)[-1] = raw;
    return result;
}

template <typename Allocate>
[[nodiscard]] void* allocate_throwing(Allocate&& allocate)
{
    record_allocation();
    while (true) {
        if (void* const pointer = allocate(); pointer != nullptr) {
            return pointer;
        }

        const std::new_handler handler = std::get_new_handler();
        if (handler == nullptr) {
            throw std::bad_alloc();
        }
        handler();
    }
}

template <typename Allocate>
[[nodiscard]] void* allocate_nothrow(Allocate&& allocate) noexcept
{
    try {
        return allocate_throwing(static_cast<Allocate&&>(allocate));
    } catch (...) {
        return nullptr;
    }
}

void deallocate_aligned(void* const pointer) noexcept
{
    if (pointer != nullptr) {
        std::free(reinterpret_cast<void**>(pointer)[-1]);
    }
}

} // namespace

void reset() noexcept
{
    allocation_count.store(0, std::memory_order_relaxed);
}

void enable() noexcept
{
    tracking_enabled.store(true, std::memory_order_relaxed);
}

void disable() noexcept
{
    tracking_enabled.store(false, std::memory_order_relaxed);
}

std::uint64_t count() noexcept
{
    return allocation_count.load(std::memory_order_relaxed);
}

ScopedCounter::ScopedCounter() noexcept
{
    reset();
    enable();
}

ScopedCounter::~ScopedCounter() noexcept
{
    if (active_) {
        disable();
    }
}

std::uint64_t ScopedCounter::finish() noexcept
{
    if (active_) {
        disable();
        active_ = false;
    }
    return count();
}

} // namespace eigenbook::test::allocation_tracker

void* operator new(const std::size_t size)
{
    return eigenbook::test::allocation_tracker::allocate_throwing(
        [size]() noexcept {
            return eigenbook::test::allocation_tracker::try_allocate_unaligned(size);
        });
}

void* operator new[](const std::size_t size)
{
    return eigenbook::test::allocation_tracker::allocate_throwing(
        [size]() noexcept {
            return eigenbook::test::allocation_tracker::try_allocate_unaligned(size);
        });
}

void* operator new(const std::size_t size, const std::nothrow_t&) noexcept
{
    return eigenbook::test::allocation_tracker::allocate_nothrow(
        [size]() noexcept {
            return eigenbook::test::allocation_tracker::try_allocate_unaligned(size);
        });
}

void* operator new[](const std::size_t size, const std::nothrow_t&) noexcept
{
    return eigenbook::test::allocation_tracker::allocate_nothrow(
        [size]() noexcept {
            return eigenbook::test::allocation_tracker::try_allocate_unaligned(size);
        });
}

void* operator new(const std::size_t size, const std::align_val_t alignment)
{
    return eigenbook::test::allocation_tracker::allocate_throwing(
        [size, alignment]() noexcept {
            return eigenbook::test::allocation_tracker::try_allocate_aligned(
                size, static_cast<std::size_t>(alignment));
        });
}

void* operator new[](const std::size_t size, const std::align_val_t alignment)
{
    return eigenbook::test::allocation_tracker::allocate_throwing(
        [size, alignment]() noexcept {
            return eigenbook::test::allocation_tracker::try_allocate_aligned(
                size, static_cast<std::size_t>(alignment));
        });
}

void* operator new(const std::size_t size,
                   const std::align_val_t alignment,
                   const std::nothrow_t&) noexcept
{
    return eigenbook::test::allocation_tracker::allocate_nothrow(
        [size, alignment]() noexcept {
            return eigenbook::test::allocation_tracker::try_allocate_aligned(
                size, static_cast<std::size_t>(alignment));
        });
}

void* operator new[](const std::size_t size,
                     const std::align_val_t alignment,
                     const std::nothrow_t&) noexcept
{
    return eigenbook::test::allocation_tracker::allocate_nothrow(
        [size, alignment]() noexcept {
            return eigenbook::test::allocation_tracker::try_allocate_aligned(
                size, static_cast<std::size_t>(alignment));
        });
}

void operator delete(void* const pointer) noexcept
{
    std::free(pointer);
}

void operator delete[](void* const pointer) noexcept
{
    std::free(pointer);
}

void operator delete(void* const pointer, const std::size_t) noexcept
{
    std::free(pointer);
}

void operator delete[](void* const pointer, const std::size_t) noexcept
{
    std::free(pointer);
}

void operator delete(void* const pointer, const std::nothrow_t&) noexcept
{
    std::free(pointer);
}

void operator delete[](void* const pointer, const std::nothrow_t&) noexcept
{
    std::free(pointer);
}

void operator delete(void* const pointer, const std::align_val_t) noexcept
{
    eigenbook::test::allocation_tracker::deallocate_aligned(pointer);
}

void operator delete[](void* const pointer, const std::align_val_t) noexcept
{
    eigenbook::test::allocation_tracker::deallocate_aligned(pointer);
}

void operator delete(void* const pointer,
                     const std::size_t,
                     const std::align_val_t) noexcept
{
    eigenbook::test::allocation_tracker::deallocate_aligned(pointer);
}

void operator delete[](void* const pointer,
                       const std::size_t,
                       const std::align_val_t) noexcept
{
    eigenbook::test::allocation_tracker::deallocate_aligned(pointer);
}

void operator delete(void* const pointer,
                     const std::align_val_t,
                     const std::nothrow_t&) noexcept
{
    eigenbook::test::allocation_tracker::deallocate_aligned(pointer);
}

void operator delete[](void* const pointer,
                       const std::align_val_t,
                       const std::nothrow_t&) noexcept
{
    eigenbook::test::allocation_tracker::deallocate_aligned(pointer);
}
