#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <type_traits>

template <typename T, std::size_t C>
class MPMCQ {
    constexpr static size_t hardware_interference_size = 64;
    static_assert(std::is_trivially_copyable<T>::value);

    struct Slot {
        alignas(std::max(hardware_interference_size, alignof(T))) std::array<std::byte, sizeof(T)> data;
        std::atomic_size_t turn;
    };
    alignas(hardware_interference_size) std::atomic_size_t m_head;
    alignas(hardware_interference_size) std::atomic_size_t m_tail;
    alignas(hardware_interference_size) std::array<Slot, C + 1> m_slots;

public:
    MPMCQ()
        : m_head(0)
        , m_tail(0)
    {
    }
    MPMCQ(const MPMCQ&) = delete;
    MPMCQ& operator=(const MPMCQ&) = delete;

    void push(const T& item)
    {
        size_t head = m_head.fetch_add(1, std::memory_order_relaxed);
        auto& slot = m_slots[head % C];
        size_t turn = (head / C) * 2, current_turn;
        while ((current_turn = slot.turn.load(std::memory_order_acquire)) != turn)
            slot.turn.wait(current_turn, std::memory_order_relaxed);

        memcpy(slot.data.data(), &item, sizeof(T));
        slot.turn.store((head / C) * 2 + 1, std::memory_order_release);
        slot.turn.notify_one();
    }
    bool try_push(const T& item)
    {
        size_t head = m_head.load(std::memory_order_acquire);
        while (true) {
            auto& slot = m_slots[head % C];
            if ((head / C) * 2 == slot.turn.load(std::memory_order_acquire)) {
                if (m_head.compare_exchange_strong(head, head + 1)) {
                    memcpy(slot.data.data(), &item, sizeof(T));
                    slot.turn.store((head / C) * 2 + 1, std::memory_order_release);
                    slot.turn.notify_one();
                    return true;
                } // else try again asap; the slot is correct but someone else pushed ahead of us
            } else {
                const size_t prev_head = head;
                head = m_head.load(std::memory_order_acquire);
                if (head == prev_head)
                    return false;
            }
        }
    }
    void pop(T& item)
    {
        size_t tail = m_tail.fetch_add(1, std::memory_order_relaxed);
        auto& slot = m_slots[tail % C];
        size_t turn = (tail / C) * 2 + 1, current_turn;
        while ((current_turn = slot.turn.load(std::memory_order_acquire)) != turn)
            slot.turn.wait(current_turn, std::memory_order_relaxed);

        memcpy(&item, slot.data.data(), sizeof(T));
        slot.turn.store((tail / C) * 2 + 2, std::memory_order_release);
        slot.turn.notify_all();
    }
    bool try_pop(T& item)
    {
        size_t tail = m_tail.load(std::memory_order_acquire);
        while (true) {
            auto& slot = m_slots[tail % C];
            if ((tail / C) * 2 + 1 == slot.turn.load(std::memory_order_acquire)) {
                if (m_tail.compare_exchange_strong(tail, tail + 1)) {
                    memcpy(&item, slot.data.data(), sizeof(T));
                    slot.turn.store((tail / C) * 2 + 2, std::memory_order_release);
                    slot.turn.notify_all();
                    return true;
                }
            } else {
                const size_t prev_tail = tail;
                tail = m_tail.load(std::memory_order_acquire);
                if (tail == prev_tail)
                    return false;
            }
        }
    }
    bool empty() const
    {
        return m_head.load(std::memory_order_relaxed) - m_tail.load(std::memory_order_relaxed) <= 0;
    }
};
