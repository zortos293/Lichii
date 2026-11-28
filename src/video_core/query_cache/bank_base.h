// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <deque>
#include <utility>

#include "common/assert.h"
#include "common/common_types.h"
#include "video_core/query_cache/stable_slots.h"

namespace VideoCommon {

class BankBase {
protected:
    const size_t base_bank_size{};
    size_t bank_size{};
    size_t current_slot{};
    // Queries take their slots' references where they are recorded (the GPU thread) and drop them
    // where their results land (the fencing thread), a report at a time: one counter each, on its
    // own cache line, so neither thread pulls the line from the other at every report.
    alignas(64) std::atomic<size_t> added{};
    alignas(64) std::atomic<size_t> dropped{};
    /// Keeps a derived bank's members off dropped's line (MSVC's layout puts them right after).
    std::array<char, 64 - sizeof(std::atomic<size_t>)> dropped_padding{};

public:
    explicit BankBase(size_t bank_size_) : base_bank_size{bank_size_}, bank_size(bank_size_) {}

    virtual ~BankBase() = default;

    virtual std::pair<bool, size_t> Reserve() {
        if (IsClosed()) {
            return {false, bank_size};
        }
        const size_t result = current_slot++;
        return {true, result};
    }

    virtual void Reset() {
        current_slot = 0;
        added = 0;
        dropped = 0;
        bank_size = base_bank_size;
    }

    size_t Size() const {
        return bank_size;
    }

    void AddReference(size_t how_many = 1) {
        added.fetch_add(how_many, std::memory_order_relaxed);
    }

    /// Release: the bank's results the dropping thread read come before a reuse that sees the
    /// references gone (References, IsDead).
    void CloseReference(size_t how_many = 1) {
        dropped.fetch_add(how_many, std::memory_order_release);
    }

    /// The references taken and not dropped yet. Called where references are taken, so every
    /// reference dropped was taken before: more dropped than taken is a double free.
    size_t References() const {
        const size_t dropped_now = dropped.load(std::memory_order_acquire);
        const size_t added_now = added.load(std::memory_order_acquire);
        ASSERT(dropped_now <= added_now);
        return added_now - dropped_now;
    }

    /// Only the recording thread writes bank_size; the fencing thread reads it (Size) for flushes
    /// handed over after the first close. A closed bank reserves no slot, so a later close (a
    /// second report flushed over the same slots on tiler drivers) would store the same value
    /// while a flush is read: it only reads the field.
    void Close() {
        if (bank_size != current_slot) {
            bank_size = current_slot;
        }
    }

    bool IsClosed() const {
        return current_slot >= bank_size;
    }

    bool IsDead() const {
        return IsClosed() && References() == 0;
    }
};

template <typename BankType>
class BankPool {
private:
    /// Read by index on the fencing thread while ReserveBank appends.
    StableSlots<BankType> bank_pool;
    std::deque<size_t> bank_indices;

public:
    BankPool() = default;
    ~BankPool() = default;

    // Reserve a bank from the pool and return its index
    template <typename Func>
    size_t ReserveBank(Func&& builder) {
        if (!bank_indices.empty() && bank_pool[bank_indices.front()].IsDead()) {
            size_t new_index = bank_indices.front();
            bank_indices.pop_front();
            bank_pool[new_index].Reset();
            bank_indices.push_back(new_index);
            return new_index;
        }
        size_t new_index = bank_pool.size();
        builder(bank_pool, new_index);
        bank_indices.push_back(new_index);
        return new_index;
    }

    // Get a reference to a bank using its index
    BankType& GetBank(size_t index) {
        return bank_pool[index];
    }

    // Get the total number of banks in the pool
    size_t BankCount() const {
        return bank_pool.size();
    }

    /// Calls func(bank, first slot, slots) for each bank a query's slots are in, from its first
    /// bank along the banks' next_bank (an index plus one). A bank's next_bank is read before
    /// func runs, which may drop its last references (a free) and let the recording thread reset
    /// and reuse it, and only when the query continues: the recording thread sets the last bank's
    /// next_bank when it reserves the bank after it.
    template <typename Query, typename Func>
    void ForEachBankOf(const Query& query, Func&& func) {
        size_t size_slots = query.size_slots;
        if (size_slots == 0) {
            return;
        }
        size_t bank_id = query.start_bank_id;
        const size_t banks_set = query.size_banks;
        size_t start_slot = query.start_slot;
        for (size_t i = 0; i < banks_set; i++) {
            auto& the_bank = GetBank(bank_id);
            const size_t amount = (std::min)(the_bank.Size() - start_slot, size_slots);
            const size_t next_id = i + 1 < banks_set ? the_bank.next_bank - 1 : 0;
            func(&the_bank, start_slot, amount);
            bank_id = next_id;
            start_slot = 0;
            size_slots -= amount;
        }
    }
};

} // namespace VideoCommon
