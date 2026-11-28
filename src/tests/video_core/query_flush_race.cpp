// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// The fencing thread lands query results (QueryCacheBase::PopAsyncFlushes, a streamer's
// PopUnsyncedQueries) and frees the queries (FreeMany) while the thread that records reports
// builds new queries and reserves banks, reusing those whose references are all dropped. The two
// must not race on the streamer's storage or on a bank being reused: Vulkan's SamplesStreamer
// reads a query by id and walks its banks by index there (BankPool::ForEachBankOf), while the
// recording thread appends to both and resets reused banks. The streamer below records, lands
// and frees as SamplesStreamer does (a set of query ids per fence, queued and taken under the
// streamer's flush lock; frees under its lock), on SimpleStreamer's queries and a BankPool, and
// the sets go through the real QueryCacheBase. These races rarely show in a plain build: run the
// tests under ThreadSanitizer or AddressSanitizer, or with MallocScribble=1 on macOS, for them to
// catch one.

#include <array>
#include <atomic>
#include <deque>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/core.h"
#include "core/device_memory.h"
#include "tests/core/system_test_context.h"
#include "video_core/gpu.h"
#include "video_core/query_cache/bank_base.h"
#include "video_core/query_cache/query_cache.h"
#include "video_core/query_cache/query_stream.h"
#include "video_core/query_cache/stable_slots.h"
#include "video_core/renderer_null/null_rasterizer.h"

namespace {

using VideoCommon::QueryType;

class TestBank : public VideoCommon::BankBase {
public:
    static constexpr size_t Slots = 8;

    explicit TestBank(size_t index_) : BankBase{Slots}, index{index_} {}

    void Reset() override {
        BankBase::Reset();
        results.fill(0);
        next_bank = 0;
    }

    std::array<u64, Slots> results{};
    size_t index;
    size_t next_bank{}; ///< The bank a query continues in, plus one, as SamplesQueryBank's
};

/// Records each report into slots_per_query slots of the current banks, continuing into a new
/// bank when one is full, lands the results at the fence and frees the queries, as
/// SamplesStreamer does: the queued sets are taken under flush_guard, each query is read by id
/// and its banks walked by index without it, and the frees drop the banks' references under the
/// streamer's lock.
class FlushStreamer : public VideoCommon::SimpleStreamer<VideoCommon::HostQueryBase> {
public:
    explicit FlushStreamer(size_t slots_per_query_)
        : SimpleStreamer{static_cast<size_t>(QueryType::ZPassPixelCount64)},
          slots_per_query{slots_per_query_} {}

    size_t WriteCounter(VAddr address, bool has_timestamp, u32 value, std::optional<u32>) override {
        const size_t query_id = BuildQuery(has_timestamp, address);
        auto* const query = GetQuery(query_id);
        for (size_t taken = 0; taken < slots_per_query; ++taken) {
            if (current_bank == nullptr || current_bank->IsClosed()) {
                ReserveBank();
            }
            const size_t slot = current_bank->Reserve().second;
            current_bank->results[slot] = value;
            // Dropped by the free: a closed bank is reused only once every query in it is freed.
            current_bank->AddReference();
            if (taken == 0) {
                query->start_bank_id = static_cast<u32>(current_bank_id);
                query->start_slot = slot;
                query->size_banks = 1;
            } else if (slot == 0) {
                ++query->size_banks;
            }
        }
        query->size_slots = slots_per_query;
        running = {query->start_bank_id, query->size_banks, query->start_slot, query->size_slots};
        pending_flush_queries.push_back(query_id);
        return query_id;
    }

    /// A report over the slots of the last one, reserving none: SamplesStreamer::WriteCounter
    /// with the running query kept over a flush (PresyncWrites' early return on tiler drivers).
    /// The next flush closes that bank again.
    size_t RepeatLast(VAddr address) {
        const size_t query_id = BuildQuery(false, address);
        auto* const query = GetQuery(query_id);
        query->start_bank_id = running.start_bank_id;
        query->size_banks = running.size_banks;
        query->start_slot = running.start_slot;
        query->size_slots = running.size_slots;
        bank_pool.ForEachBankOf(*query, [](TestBank* bank, size_t, size_t amount) {
            bank->AddReference(amount);
        });
        pending_flush_queries.push_back(query_id);
        return query_id;
    }

    bool HasUnsyncedQueries() const override {
        return !pending_flush_queries.empty();
    }

    void PushUnsyncedQueries() override {
        // As SamplesStreamer: a set ends its bank early, and the next report starts a new one.
        current_bank->Close();
        std::scoped_lock lk(flush_guard);
        pending_flush_sets.emplace_back(std::move(pending_flush_queries));
        pending_flush_queries = {};
    }

    void PopUnsyncedQueries() override {
        std::vector<size_t> current;
        {
            std::scoped_lock lk(flush_guard);
            current = std::move(pending_flush_sets.front());
            pending_flush_sets.pop_front();
        }
        for (const size_t query_id : current) {
            auto* const query = GetQuery(query_id);
            u64 total = 0;
            bank_pool.ForEachBankOf(*query, [&total](TestBank* bank, size_t start, size_t amount) {
                for (size_t slot = start; slot < start + amount; ++slot) {
                    total += bank->results[slot];
                }
            });
            query->value = total;
            query->flags |= VideoCommon::QueryFlagBits::IsFinalValueSynced;
            if (total != slots_per_query * (query->guest_address >> 4)) {
                wrong.fetch_add(1, std::memory_order_relaxed);
            }
        }
        landed.fetch_add(current.size(), std::memory_order_relaxed);
        // The query cache frees landed reports on the fencing thread (UnregisterPending).
        FreeMany(current);
    }

    void FreeMany(std::span<const size_t> query_ids) override {
        std::scoped_lock lk(guard);
        for (const size_t query_id : query_ids) {
            bank_pool.ForEachBankOf(*GetQuery(query_id), [](TestBank* bank, size_t, size_t amount) {
                bank->CloseReference(amount);
            });
            ReleaseQuery(query_id);
        }
    }

    std::atomic<size_t> landed{};
    std::atomic<size_t> wrong{};

private:
    void ReserveBank() {
        current_bank_id =
            bank_pool.ReserveBank([](auto& queue, size_t index) { queue.emplace_back(index); });
        if (current_bank) {
            current_bank->next_bank = current_bank_id + 1;
        }
        current_bank = &bank_pool.GetBank(current_bank_id);
    }

    struct Range {
        u32 start_bank_id;
        u32 size_banks;
        size_t start_slot;
        size_t size_slots;
    };

    const size_t slots_per_query;
    Range running{};
    VideoCommon::BankPool<TestBank> bank_pool;
    TestBank* current_bank{};
    size_t current_bank_id{};
    std::vector<size_t> pending_flush_queries;
    std::deque<std::vector<size_t>> pending_flush_sets;
    std::mutex flush_guard;
};

class FlushRuntime {
public:
    explicit FlushRuntime(size_t slots_per_query) : streamer{slots_per_query} {}

    VideoCommon::StreamerInterface* GetStreamerInterface(QueryType type) {
        return type == QueryType::ZPassPixelCount64 ? &streamer : nullptr;
    }

    void Barriers(bool) {}
    void Bind3DEngine(Tegra::Engines::Maxwell3D*) {}

    FlushStreamer streamer;
};

struct FlushTraits {
    using RuntimeType = FlushRuntime;
};

/// A GPU, device memory and the null rasterizer for the query cache to be built over.
struct CacheFixture {
    ScopedSystemTestContext context;
    Core::System system;
    Core::DeviceMemory physical;
    Tegra::MaxwellDeviceMemoryManager device{physical};
    Tegra::GPU gpu{system, false, false};
    Null::RasterizerNull rasterizer{gpu};
    FlushRuntime runtime;
    std::optional<VideoCommon::QueryCacheBase<FlushTraits>> cache;

    explicit CacheFixture(size_t slots_per_query) : runtime{slots_per_query} {
        system.Initialize();
        cache.emplace(gpu, rasterizer, device, runtime);
    }
};

/// Records `reports` reports, committing a set of them every `per_fence` while a second thread
/// lands the committed sets through PopAsyncFlushes, as the fencing thread does.
void RecordWhileLanding(CacheFixture& f, size_t reports, size_t per_fence) {
    std::atomic<size_t> committed{};
    std::atomic<bool> recording{true};
    std::thread fencing([&] {
        size_t popped = 0;
        while (true) {
            const bool last = !recording.load(std::memory_order_acquire);
            while (popped < committed.load(std::memory_order_acquire)) {
                f.cache->PopAsyncFlushes();
                ++popped;
            }
            if (last) {
                break;
            }
            std::this_thread::yield();
        }
    });
    for (size_t i = 0; i < reports; ++i) {
        const VAddr address = static_cast<VAddr>(i) << 4;
        f.runtime.streamer.WriteCounter(address, false, static_cast<u32>(i), std::nullopt);
        if ((i + 1) % per_fence == 0) {
            f.cache->CommitAsyncFlushes();
            committed.fetch_add(1, std::memory_order_release);
        }
    }
    f.cache->CommitAsyncFlushes();
    committed.fetch_add(1, std::memory_order_release);
    recording.store(false, std::memory_order_release);
    fencing.join();
}

} // Anonymous namespace

TEST_CASE("Query results land while new queries and banks are built", "[video_core][query_cache]") {
    CacheFixture f{1};
    // Small sets make the fencing thread read while the storage grows, as when a game loads.
    RecordWhileLanding(f, 200'000, 4);
    CHECK(f.runtime.streamer.landed == 200'000);
    CHECK(f.runtime.streamer.wrong == 0);
}

TEST_CASE("Queries over two banks are freed while their banks are reused",
          "[video_core][query_cache]") {
    // Three slots a query in banks of eight, each set of four closing its bank: one query in four
    // continues into the next bank, whose references the free drops while the recording thread
    // reuses the banks freed before.
    CacheFixture f{3};
    RecordWhileLanding(f, 200'000, 4);
    CHECK(f.runtime.streamer.landed == 200'000);
    CHECK(f.runtime.streamer.wrong == 0);
}

TEST_CASE("A bank closed again by a second report is read while the first flush lands",
          "[video_core][query_cache]") {
    // Each report is flushed twice: once as recorded, closing its bank, and once more as a report
    // over the same slots, which closes the same bank again while the fencing thread reads the
    // first flush's banks (Size).
    CacheFixture f{1};
    std::atomic<size_t> committed{};
    std::atomic<bool> recording{true};
    std::thread fencing([&] {
        size_t popped = 0;
        while (true) {
            const bool last = !recording.load(std::memory_order_acquire);
            while (popped < committed.load(std::memory_order_acquire)) {
                f.cache->PopAsyncFlushes();
                ++popped;
            }
            if (last) {
                break;
            }
            std::this_thread::yield();
        }
    });
    constexpr size_t Reports = 100'000;
    for (size_t i = 0; i < Reports; ++i) {
        const VAddr address = static_cast<VAddr>(i) << 4;
        f.runtime.streamer.WriteCounter(address, false, static_cast<u32>(i), std::nullopt);
        f.cache->CommitAsyncFlushes();
        committed.fetch_add(1, std::memory_order_release);
        f.runtime.streamer.RepeatLast(address);
        f.cache->CommitAsyncFlushes();
        committed.fetch_add(1, std::memory_order_release);
    }
    recording.store(false, std::memory_order_release);
    fencing.join();
    CHECK(f.runtime.streamer.landed == 2 * Reports);
    CHECK(f.runtime.streamer.wrong == 0);
}

TEST_CASE("Query banks are read by index while new banks are added", "[video_core][query_cache]") {
    // While results land slower than reports come (a game loading), every bank is still
    // referenced and the pool grows: the fencing thread reads the banks it was handed meanwhile.
    VideoCommon::BankPool<TestBank> pool;
    constexpr size_t Banks = 20'000;
    std::atomic<size_t> published{};
    std::atomic<size_t> wrong{};
    std::thread fencing([&] {
        size_t read = 0;
        while (read < Banks) {
            const size_t ready = published.load(std::memory_order_acquire);
            for (; read < ready; ++read) {
                if (pool.GetBank(read).index != read) {
                    wrong.fetch_add(1, std::memory_order_relaxed);
                }
            }
            std::this_thread::yield();
        }
    });
    for (size_t i = 0; i < Banks; ++i) {
        const size_t index =
            pool.ReserveBank([](auto& queue, size_t new_index) { queue.emplace_back(new_index); });
        pool.GetBank(index).AddReference();
        published.store(i + 1, std::memory_order_release);
    }
    fencing.join();
    CHECK(pool.BankCount() == Banks);
    CHECK(wrong == 0);
}

TEST_CASE("Stable slots keep every element in place as they grow", "[video_core][query_cache]") {
    struct Counted {
        explicit Counted(size_t value_, size_t& destroyed_)
            : value{value_}, destroyed{destroyed_} {}
        ~Counted() {
            ++destroyed;
        }
        size_t value;
        size_t& destroyed;
    };
    size_t destroyed = 0;
    constexpr size_t Count = 100'000;
    {
        VideoCommon::StableSlots<Counted, 4> slots;
        std::vector<const Counted*> addresses;
        for (size_t i = 0; i < Count; ++i) {
            addresses.push_back(&slots.emplace_back(i, destroyed));
            REQUIRE(slots.size() == i + 1);
        }
        size_t moved = 0;
        size_t wrong = 0;
        for (size_t i = 0; i < Count; ++i) {
            moved += &slots[i] != addresses[i] ? 1 : 0;
            wrong += slots[i].value != i ? 1 : 0;
        }
        CHECK(moved == 0);
        CHECK(wrong == 0);
    }
    CHECK(destroyed == Count);
}

TEST_CASE("A throwing append leaves stable slots as they were", "[video_core][query_cache]") {
    struct Throwing {
        explicit Throwing(bool fail, size_t value_) : value{value_} {
            if (fail) {
                throw std::runtime_error{"pool creation failed"};
            }
        }
        size_t value;
    };
    VideoCommon::StableSlots<Throwing, 4> slots;
    for (size_t i = 0; i < 4; ++i) {
        slots.emplace_back(false, i);
    }
    // The first element of the second segment fails, then is constructed in the same place.
    CHECK_THROWS_AS(slots.emplace_back(true, 4), std::runtime_error);
    CHECK(slots.size() == 4);
    const Throwing& retried = slots.emplace_back(false, 4);
    CHECK(slots.size() == 5);
    CHECK(&slots[4] == &retried);
    CHECK(slots[4].value == 4);
    CHECK(slots[3].value == 3);
}
