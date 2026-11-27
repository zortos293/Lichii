// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <vector>

#include "common/common_types.h"

namespace Dynarmic::Backend::Arm64 {

using CodePtr = std::byte*;

/// The code cache's map from a block's location descriptor to its entry point. Any thread may call Find at any
/// time, without a lock: the dispatcher of every Jit of a shared cache does, and with one lock around the map those
/// lookups waited behind every block being emitted (about 7 s per game load, summed over six cores). Everything
/// else needs the caller to hold the cache's lock exclusively.
///
/// An open-addressing table of atomic slots. A key, once written, stays in its slot: dropping a block clears the
/// slot's entry point, and the block compiled for the same location later takes the slot again. A full table is
/// replaced by a larger copy; the old one is kept, because a Find may still be walking it. Such a Find can miss a
/// block inserted since, or return one dropped since, exactly as a Find that ran a moment earlier would have.
class BlockTable {
public:
    BlockTable() {
        Publish(std::make_unique<Table>(initial_capacity));
    }

    CodePtr Find(u64 key) const {
        const Table& table = *current.load(std::memory_order_acquire);
        for (std::size_t index = Hash(key) & table.mask;; index = (index + 1) & table.mask) {
            const Slot& slot = table.slots[index];
            const u64 slot_key = slot.key.load(std::memory_order_acquire);
            if (slot_key == key) {
                return slot.entry_point.load(std::memory_order_acquire);
            }
            if (slot_key == empty_key) {
                return nullptr;
            }
        }
    }

    /// False if the key has an entry point already.
    bool Insert(u64 key, CodePtr entry_point) {
        if ((keys + 1) * 5 > Capacity() * 3) {
            Grow();
        }
        Table& table = *current.load(std::memory_order_relaxed);
        for (std::size_t index = Hash(key) & table.mask;; index = (index + 1) & table.mask) {
            Slot& slot = table.slots[index];
            const u64 slot_key = slot.key.load(std::memory_order_relaxed);
            if (slot_key == key) {
                if (slot.entry_point.load(std::memory_order_relaxed) != nullptr) {
                    return false;
                }
                slot.entry_point.store(entry_point, std::memory_order_release);
                return true;
            }
            if (slot_key == empty_key) {
                // The entry point first: a Find that sees the key then sees it too.
                slot.entry_point.store(entry_point, std::memory_order_release);
                slot.key.store(key, std::memory_order_release);
                ++keys;
                return true;
            }
        }
    }

    /// The entry point the key had, or nullptr.
    CodePtr Erase(u64 key) {
        Table& table = *current.load(std::memory_order_relaxed);
        for (std::size_t index = Hash(key) & table.mask;; index = (index + 1) & table.mask) {
            Slot& slot = table.slots[index];
            const u64 slot_key = slot.key.load(std::memory_order_relaxed);
            if (slot_key == key) {
                return slot.entry_point.exchange(nullptr, std::memory_order_acq_rel);
            }
            if (slot_key == empty_key) {
                return nullptr;
            }
        }
    }

    /// Forgets every block. No Find may run meanwhile: the cache is cleared only when no Jit runs its code.
    void Clear() {
        Table& table = *current.load(std::memory_order_relaxed);
        for (std::size_t index = 0; index <= table.mask; ++index) {
            table.slots[index].key.store(empty_key, std::memory_order_relaxed);
            table.slots[index].entry_point.store(nullptr, std::memory_order_relaxed);
        }
        keys = 0;
        // Tables replaced before this clear: nothing can still be walking them.
        if (tables.size() > 1) {
            tables.erase(tables.begin(), tables.end() - 1);
        }
    }

private:
    struct Slot {
        std::atomic<u64> key{empty_key};
        std::atomic<CodePtr> entry_point{nullptr};
    };
    struct Table {
        explicit Table(std::size_t capacity)
                : mask{capacity - 1}, slots{std::make_unique<Slot[]>(capacity)} {}
        std::size_t mask;
        std::unique_ptr<Slot[]> slots;
    };

    /// No location descriptor has this value (the Jits' own lookup tables use it the same way).
    static constexpr u64 empty_key = ~u64{0};
    static constexpr std::size_t initial_capacity = std::size_t{1} << 16;

    static std::size_t Hash(u64 key) {
        key ^= key >> 33;
        key *= 0xff51afd7ed558ccdULL;
        key ^= key >> 33;
        return static_cast<std::size_t>(key);
    }

    std::size_t Capacity() const {
        return current.load(std::memory_order_relaxed)->mask + 1;
    }

    void Publish(std::unique_ptr<Table> table) {
        current.store(table.get(), std::memory_order_release);
        tables.push_back(std::move(table));
    }

    /// Copies the blocks that still have an entry point into a table of twice the size.
    void Grow() {
        const Table& old = *current.load(std::memory_order_relaxed);
        auto table = std::make_unique<Table>((old.mask + 1) * 2);
        std::size_t kept = 0;
        for (std::size_t index = 0; index <= old.mask; ++index) {
            const u64 key = old.slots[index].key.load(std::memory_order_relaxed);
            const CodePtr entry_point = old.slots[index].entry_point.load(std::memory_order_relaxed);
            if (key == empty_key || entry_point == nullptr) {
                continue;
            }
            std::size_t to = Hash(key) & table->mask;
            while (table->slots[to].key.load(std::memory_order_relaxed) != empty_key) {
                to = (to + 1) & table->mask;
            }
            table->slots[to].entry_point.store(entry_point, std::memory_order_relaxed);
            table->slots[to].key.store(key, std::memory_order_relaxed);
            ++kept;
        }
        keys = kept;
        Publish(std::move(table));
    }

    std::atomic<Table*> current{nullptr};
    std::vector<std::unique_ptr<Table>> tables;  ///< The last one is `current`
    std::size_t keys = 0;  ///< Slots of `current` with a key
};

}  // namespace Dynarmic::Backend::Arm64
