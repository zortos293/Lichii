// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cmath>
#include <type_traits>
#include <cstring>
#include <fstream>
#include <set>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include <fmt/format.h>

#include "common/cityhash.h"
#include "video_core/nx2_precompile_build.h"

namespace VideoCommon::Nx2Precompile {
namespace {

constexpr u32 StageVertexB = 0;
constexpr u32 StageGeometry = 3;
constexpr u32 StageFragment = 4;
constexpr u32 StageCompute = 5;
constexpr u32 StageTask = 7;
constexpr u32 StageMesh = 8;
constexpr u64 NativeHashTag = 0x4F554E4345000002ULL;
constexpr size_t SphSize = 0x50;
constexpr size_t EnvHeaderSize = 6 * 8 + 7 * 4;
constexpr size_t ComputeKeySize = 24;
constexpr size_t GraphicsKeySize = 1056;
constexpr size_t StateOffset = 48;
constexpr size_t StateSize = 112;
constexpr size_t StateTail = GraphicsKeySize - StateOffset - StateSize;

template <typename T>
T Load(std::span<const u8> bytes, size_t offset) {
    if (offset + sizeof(T) > bytes.size()) {
        throw std::runtime_error(fmt::format("truncated at {}", offset));
    }
    T value;
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return value;
}

template <typename T>
void Put(std::vector<u8>& out, const T& value) {
    const auto* const bytes = reinterpret_cast<const u8*>(&value);
    out.insert(out.end(), bytes, bytes + sizeof(T));
}

u32 U32(std::span<const u8> bytes, size_t offset) {
    return Load<u32>(bytes, offset);
}

std::string Bytes(std::span<const u8> bytes) {
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

/// A collections.Counter: counts in insertion order, most_common by count, ties in insertion order.
template <typename K, typename V = u64>
class Counter {
public:
    V& operator[](const K& key) {
        const auto [it, added] = index.try_emplace(key, items.size());
        if (added) {
            items.emplace_back(key, V{});
        }
        return items[it->second].second;
    }

    [[nodiscard]] bool Empty() const {
        return items.empty();
    }

    /// sum(counter.values()): integers exactly; floats as Python 3.12 and later add them (Neumaier's
    /// compensated summation, CPython's builtin sum), so ties and near-ties break as the script's.
    [[nodiscard]] V Sum() const {
        if constexpr (std::is_floating_point_v<V>) {
            double sum = 0.0;
            double compensation = 0.0;
            for (const auto& [key, value] : items) {
                const double t = sum + value;
                if (std::fabs(sum) >= std::fabs(value)) {
                    compensation += (sum - t) + value;
                } else {
                    compensation += (value - t) + sum;
                }
                sum = t;
            }
            if (compensation != 0.0 && std::isfinite(compensation)) {
                sum += compensation;
            }
            return sum;
        } else {
            V sum{};
            for (const auto& [key, value] : items) {
                sum += value;
            }
            return sum;
        }
    }

    [[nodiscard]] std::vector<std::pair<K, V>> MostCommon(size_t limit) const {
        auto sorted = items;
        std::ranges::stable_sort(sorted, [](const auto& a, const auto& b) { return a.second > b.second; });
        if (sorted.size() > limit) {
            sorted.resize(limit);
        }
        return sorted;
    }

private:
    std::vector<std::pair<K, V>> items;
    std::unordered_map<K, size_t> index;
};

/// An insertion-ordered dict of counters (collections.defaultdict(Counter)).
template <typename K, typename C>
class CounterTable {
public:
    C& operator[](const K& key) {
        const auto [it, added] = index.try_emplace(key, items.size());
        if (added) {
            items.emplace_back(key, C{});
        }
        return items[it->second].second;
    }

    [[nodiscard]] const C* Find(const K& key) const {
        const auto it = index.find(key);
        return it == index.end() ? nullptr : &items[it->second].second;
    }

    [[nodiscard]] const std::vector<std::pair<K, C>>& Items() const {
        return items;
    }

    void Insert(const K& key, const C& value) {
        if (index.try_emplace(key, items.size()).second) {
            items.emplace_back(key, value);
        }
    }

private:
    std::vector<std::pair<K, C>> items;
    std::unordered_map<K, size_t> index;
};

// ------------------------------------------------------------------------------------------------
// statefields.py: the 112 hashed bytes of FixedPipelineState.

constexpr u32 Eds3Blend = 1U << 3;
constexpr u32 DynamicVertexInput = 1U << 5;
constexpr u32 XfbEnabled = 1U << 6;

u32 AttributeReadMask(std::span<const u8> sph) {
    u32 mask = 0;
    for (u32 i = 0; i < 16; ++i) {
        const u8 byte = sph[0x18 + i];
        for (u32 half = 0; half < 2; ++half) {
            if ((byte >> (4 * half)) & 0xF) {
                mask |= 1U << (i * 2 + half);
            }
        }
    }
    return mask;
}

bool HashesPastPrefix(std::span<const u8> state) {
    const u32 raw1 = U32(state, 0);
    return (raw1 & XfbEnabled) != 0 || (raw1 & DynamicVertexInput) == 0;
}

std::vector<u8> Canonical(std::span<const u8> state, u32 read_mask) {
    std::vector<u8> b(state.begin(), state.end());
    const u32 raw1 = U32(b, 0);
    if (raw1 & DynamicVertexInput) {
        u64 types;
        std::memcpy(&types, b.data() + 64, 8);
        u64 keep = 0;
        for (u32 a = 0; a < 32; ++a) {
            if ((read_mask >> a) & 1) {
                keep |= 3ULL << (2 * a);
            }
        }
        types &= keep;
        std::memcpy(b.data() + 64, &types, 8);
    }
    if (raw1 & Eds3Blend) {
        return b;
    }
    u32 used = 0;
    for (u32 i = 0; i < 8; ++i) {
        if (b[8 + i]) {
            used = i + 1;
        }
    }
    for (u32 i = 0; i < 8; ++i) {
        u32 att = U32(b, 80 + 4 * i);
        if (i >= used) {
            att = 0;
        } else if (!((att >> 30) & 1)) {
            att &= 0xF;
        }
        std::memcpy(b.data() + 80 + 4 * i, &att, 4);
    }
    return b;
}

// ------------------------------------------------------------------------------------------------
// state_model.py

/// A state's parts: vertex topology/flags (T), attribute types (A), render targets (F), scene (S).
/// T, F and S as their bytes, so they compare as the script's tuples do.
struct Parts {
    std::string t;
    u64 a{};
    std::string f;
    std::string s;
};

template <typename T>
void Append(std::string& out, const T& value) {
    out.append(reinterpret_cast<const char*>(&value), sizeof(value));
}

Parts Split(std::span<const u8> state) {
    const u32 raw1 = U32(state, 0);
    const u32 raw2 = U32(state, 4);
    Parts parts;
    Append(parts.t, raw1 & 0x0F0FFFC0U);
    Append(parts.t, U32(state, 28));
    std::memcpy(&parts.a, state.data() + 64, 8);
    Append(parts.f, raw1 & 0xF0200000U);
    Append(parts.f, raw2 & 0x000187FEU);
    parts.f.append(reinterpret_cast<const char*>(state.data() + 8), 8);
    Append(parts.f, U32(state, 24));
    parts.f.append(reinterpret_cast<const char*>(state.data() + 80), 32);
    Append(parts.s, raw1 & 0x0000003FU);
    Append(parts.s, raw1 & 0x00100000U);
    Append(parts.s, raw2 & 0xFFFE7801U);
    parts.s.append(reinterpret_cast<const char*>(state.data() + 16), 8);
    parts.s.append(reinterpret_cast<const char*>(state.data() + 32), 32);
    parts.s.append(reinterpret_cast<const char*>(state.data() + 72), 8);
    return parts;
}

u32 Field(const std::string& part, size_t offset) {
    u32 value;
    std::memcpy(&value, part.data() + offset, 4);
    return value;
}

std::vector<u8> Assemble(const std::string& t, u64 a, const std::string& f, const std::string& s) {
    std::vector<u8> b(StateSize);
    const u32 raw1 = Field(t, 0) | Field(f, 0) | Field(s, 0) | Field(s, 4);
    const u32 raw2 = Field(f, 4) | Field(s, 8);
    const u32 alpha_ref = Field(f, 16);
    const u32 point_size = Field(t, 4);
    std::memcpy(b.data(), &raw1, 4);
    std::memcpy(b.data() + 4, &raw2, 4);
    std::memcpy(b.data() + 8, f.data() + 8, 8);
    std::memcpy(b.data() + 16, s.data() + 12, 8);
    std::memcpy(b.data() + 24, &alpha_ref, 4);
    std::memcpy(b.data() + 28, &point_size, 4);
    std::memcpy(b.data() + 32, s.data() + 20, 32);
    std::memcpy(b.data() + 64, &a, 8);
    std::memcpy(b.data() + 72, s.data() + 52, 8);
    std::memcpy(b.data() + 80, f.data() + 20, 32);
    return b;
}

/// A shader as the model sees it: its hash, container and file, and the read mask (vertex) or the
/// render targets it writes (pixel).
struct ShaderInfo {
    std::string hash; ///< "%016x"
    std::string fine;
    std::string coarse;
    u32 read_mask{};
    u32 omap{};
};

enum class Level : u8 { Own, Fine, Coarse, Class, Global };
constexpr std::array<double, 5> LevelWeights{1.0, 0.6, 0.25, 0.25, 0.04};

std::string LevelKey(Level level, std::string_view key) {
    std::string out(1, static_cast<char>(level));
    out += key;
    return out;
}

class Model {
public:
    void Add(const ShaderInfo& vs, const ShaderInfo& fs, std::span<const u8> state) {
        const Parts parts = Split(state);
        for (const auto& key : {LevelKey(Level::Own, vs.hash), LevelKey(Level::Fine, vs.fine),
                                LevelKey(Level::Coarse, vs.coarse), LevelKey(Level::Global, "")}) {
            ++T[key][parts.t];
        }
        for (const auto& key :
             {LevelKey(Level::Own, fs.hash), LevelKey(Level::Fine, fs.fine),
              LevelKey(Level::Coarse, fs.coarse), LevelKey(Level::Class, std::to_string(fs.omap)),
              LevelKey(Level::Global, "")}) {
            ++F[key][parts.f];
        }
        ++S[parts.s];
        ++a_own[vs.hash][parts.a];
        for (u32 slot = 0; slot < 32; ++slot) {
            if ((vs.read_mask >> slot) & 1) {
                const u32 type = static_cast<u32>((parts.a >> (2 * slot)) & 3);
                for (const auto& key : {vs.fine, vs.coarse, std::string{}}) {
                    ++slots[SlotKey(key, slot)][type];
                }
            }
        }
    }

    std::vector<std::vector<u8>> Candidates(const ShaderInfo& vs, const ShaderInfo& fs,
                                            size_t k) const {
        const auto dt = Blend(T,
                              {{Level::Own, vs.hash},
                               {Level::Fine, vs.fine},
                               {Level::Coarse, vs.coarse},
                               {Level::Global, ""}},
                              4);
        const auto da = AttributeCandidates(vs);
        const auto df = Blend(F,
                              {{Level::Own, fs.hash},
                               {Level::Fine, fs.fine},
                               {Level::Coarse, fs.coarse},
                               {Level::Class, std::to_string(fs.omap)},
                               {Level::Global, ""}},
                              10);
        const u64 total = S.Sum();
        std::vector<std::pair<std::string, double>> ds;
        for (const auto& [s, n] : S.MostCommon(2)) {
            ds.emplace_back(s, static_cast<double>(n) / static_cast<double>(total));
        }
        struct Scored {
            double score;
            std::vector<u8> state;
        };
        std::vector<Scored> scored;
        for (const auto& [t, pt] : dt) {
            for (const auto& [a, pa] : da) {
                for (const auto& [f, pf] : df) {
                    for (const auto& [s, ps] : ds) {
                        scored.push_back({pt * pa * pf * ps, Assemble(t, a, f, s)});
                    }
                }
            }
        }
        std::ranges::stable_sort(scored, [](const Scored& x, const Scored& y) {
            return x.score > y.score;
        });
        std::vector<std::vector<u8>> out;
        for (size_t i = 0; i < scored.size() && i < k; ++i) {
            out.push_back(std::move(scored[i].state));
        }
        return out;
    }

    /// Mesh draws: the vertex model's render target tables cover pixel shaders no mesh draw used.
    void TakeMissingTargets(const Model& other) {
        for (const auto& [key, counter] : other.F.Items()) {
            F.Insert(key, counter);
        }
    }

private:
    using Table = CounterTable<std::string, Counter<std::string>>;

    static std::string SlotKey(const std::string& container, u32 slot) {
        std::string out = container;
        out += '\x01';
        out += static_cast<char>(slot);
        return out;
    }

    static std::vector<std::pair<std::string, double>> Blend(
        const Table& table, std::initializer_list<std::pair<Level, std::string>> keys,
        size_t limit) {
        Counter<std::string, double> out;
        for (const auto& [level, key] : keys) {
            const auto* const counter = table.Find(LevelKey(level, key));
            if (!counter || counter->Empty()) {
                continue;
            }
            const double total = static_cast<double>(counter->Sum());
            for (const auto& [x, n] : counter->MostCommon(limit)) {
                out[x] += LevelWeights[static_cast<size_t>(level)] * static_cast<double>(n) / total;
            }
            if (level == Level::Own) {
                break;
            }
        }
        double total = out.Sum();
        if (total == 0.0) {
            total = 1.0;
        }
        std::vector<std::pair<std::string, double>> result;
        for (const auto& [x, p] : out.MostCommon(limit)) {
            result.emplace_back(x, p / total);
        }
        return result;
    }

    std::vector<std::pair<u64, double>> AttributeCandidates(const ShaderInfo& vs) const {
        const auto own = a_own.Find(vs.hash);
        if (own && !own->Empty()) {
            const double total = static_cast<double>(own->Sum());
            std::vector<std::pair<u64, double>> out;
            for (const auto& [a, n] : own->MostCommon(3)) {
                out.emplace_back(a, static_cast<double>(n) / total);
            }
            return out;
        }
        u64 best = 0;
        u64 floats = 0;
        for (u32 slot = 0; slot < 32; ++slot) {
            if (!((vs.read_mask >> slot) & 1)) {
                continue;
            }
            floats |= 1ULL << (2 * slot);
            bool found = false;
            for (const auto& key : {vs.fine, vs.coarse, std::string{}}) {
                const auto* const counter = slots.Find(SlotKey(key, slot));
                if (counter && !counter->Empty()) {
                    best |= static_cast<u64>(counter->MostCommon(1).front().first) << (2 * slot);
                    found = true;
                    break;
                }
            }
            if (!found) {
                best |= 1ULL << (2 * slot);
            }
        }
        std::vector<std::pair<u64, double>> out{{best, 0.8}};
        if (floats != best) {
            out.emplace_back(floats, 0.2);
        }
        return out;
    }

    Table T;
    Table F;
    Counter<std::string> S;
    CounterTable<std::string, Counter<u64>> a_own;
    CounterTable<std::string, Counter<u32>> slots;
};

std::string Hex(u64 value) {
    return fmt::format("{:016x}", value);
}

/// The record's key with the bytes the pipeline doesn't depend on cleared (canonical_key).
std::vector<u8> CanonicalKey(const CachedPipeline& record) {
    const auto& key = record.key;
    const CachedEnv* vertex = nullptr;
    bool mesh = false;
    for (const auto& env : record.envs) {
        if (!vertex && env.stage == StageVertexB) {
            vertex = &env;
        }
        mesh |= env.stage == StageTask || env.stage == StageMesh;
    }
    const u32 mask = mesh ? 0 : (vertex ? AttributeReadMask(vertex->sph) : 0xFFFFFFFFU);
    const auto state =
        Canonical(std::span<const u8>(key).subspan(StateOffset, StateSize), mask);
    std::vector<u8> out(key.begin(), key.begin() + StateOffset);
    out.insert(out.end(), state.begin(), state.end());
    if (HashesPastPrefix(state)) {
        out.insert(out.end(), key.begin() + StateOffset + StateSize, key.end());
    } else {
        out.resize(GraphicsKeySize, 0);
    }
    return out;
}

/// The precompiled file's environments and records (build_precompiled.py Writer).
class Writer {
public:
    u32 Env(const std::vector<u8>& blob) {
        const auto [it, added] = env_ids.try_emplace(Bytes(blob), static_cast<u32>(envs.size()));
        if (added) {
            envs.push_back(&it->first);
        }
        return it->second;
    }

    bool Graphics(const std::vector<const std::vector<u8>*>& blobs, const std::vector<u8>& key) {
        if (key.size() != GraphicsKeySize) {
            throw std::logic_error("graphics key of another size");
        }
        if (!graphics_keys.insert(GraphicsKeyHash(key)).second) {
            return false;
        }
        Record record{.kind = 0, .key = key};
        for (const auto* const blob : blobs) {
            record.envs.push_back(Env(*blob));
        }
        records.push_back(std::move(record));
        return true;
    }

    bool Compute(const std::vector<u8>& blob, const std::vector<u8>& key) {
        if (key.size() != ComputeKeySize) {
            throw std::logic_error("compute key of another size");
        }
        if (!compute_keys.insert(Bytes(key) + Bytes(blob)).second) {
            return false;
        }
        records.push_back({.kind = 1, .envs = {Env(blob)}, .key = key});
        return true;
    }

    [[nodiscard]] size_t EnvCount() const {
        return envs.size();
    }

    [[nodiscard]] size_t RecordCount() const {
        return records.size();
    }

    void Write(const std::filesystem::path& path, u32 cache_version) const {
        std::vector<u8> head;
        head.insert(head.end(), {'N', 'X', '2', 'P', 'R', 'E', 'P', '1'});
        Put(head, cache_version);
        Put(head, static_cast<u32>(envs.size()));
        Put(head, static_cast<u32>(records.size()));
        Put(head, u32{0});
        std::vector<u8> body;
        for (const auto& record : records) {
            Put(body, record.kind);
            Put(body, static_cast<u8>(record.envs.size()));
            Put(body, u16{0});
            for (const u32 id : record.envs) {
                Put(body, id);
            }
            body.insert(body.end(), record.key.begin(), record.key.end());
        }
        u64 offset = head.size() + 16 * envs.size() + body.size();
        std::vector<u8> index;
        index.reserve(16 * envs.size());
        for (const auto* const blob : envs) {
            Put(index, offset);
            Put(index, static_cast<u64>(blob->size()));
            offset += blob->size();
        }
        auto partial = path;
        partial += ".partial";
        {
            std::ofstream file(partial, std::ios::binary | std::ios::trunc);
            file.exceptions(std::ofstream::failbit | std::ofstream::badbit);
            const auto write = [&file](const void* data, size_t size) {
                file.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
            };
            write(head.data(), head.size());
            write(index.data(), index.size());
            write(body.data(), body.size());
            for (const auto* const blob : envs) {
                write(blob->data(), blob->size());
            }
        }
        std::filesystem::rename(partial, path);
    }

private:
    struct Record {
        u8 kind{};
        std::vector<u32> envs;
        std::vector<u8> key;
    };

    std::unordered_map<std::string, u32> env_ids;
    std::vector<const std::string*> envs; ///< By id; the keys of env_ids, which stay put
    std::vector<Record> records;
    std::unordered_set<u64> graphics_keys;
    std::unordered_set<std::string> compute_keys;
};

// ------------------------------------------------------------------------------------------------
// Storage pointer states of compute programs (build_precompiled.py compute_states).

constexpr u64 Indirect = 1ULL << 63;
constexpr u64 HolderMask = (1ULL << 40) - 1;
constexpr size_t MaxFilledGroups = 4;
constexpr size_t MaxComputeStates = 32;

using SlotSet = std::set<u64>;

std::unordered_map<u64, u32> CbufValues(const CachedEnv& env) {
    std::unordered_map<u64, u32> values;
    for (const auto& [key, value] : env.cbuf_values) {
        values[key] = value;
    }
    return values;
}

SlotSet NullSlots(const CachedEnv& env) {
    const auto values = CbufValues(env);
    const auto value = [&values](u64 key) -> std::optional<u32> {
        const auto it = values.find(key);
        return it == values.end() ? std::nullopt : std::optional<u32>{it->second};
    };
    SlotSet out;
    for (const auto& [key, answer] : env.storage_pointer_types) {
        if (!(key & Indirect) && !answer && value(key) == 0u && value(key + 4).value_or(0) == 0) {
            out.insert(key);
        }
    }
    return out;
}

class ComputeStates {
public:
    explicit ComputeStates(const std::vector<std::pair<std::vector<u8>, std::vector<CachedEnv>>>& all,
                           PrecompiledCounts& counts_)
        : counts{counts_} {
        SlotSet seen_pointer, seen_null;
        for (const auto& [key, envs] : all) {
            for (const auto& env : envs) {
                std::map<u64, u8> answers;
                std::vector<u64> order;
                for (const auto& [k, v] : env.storage_pointer_types) {
                    if (answers.insert_or_assign(k, v).second) {
                        order.push_back(k);
                    }
                }
                for (const auto& [k, v] : answers) {
                    if (v && !(k & Indirect)) {
                        seen_pointer.insert(k);
                    }
                }
                const auto nulls = NullSlots(env);
                seen_null.insert(nulls.begin(), nulls.end());
                for (const u64 k : order) {
                    const u8 v = answers[k];
                    if (k & Indirect) {
                        const auto holder = answers.find(k & HolderMask);
                        if (holder != answers.end() && holder->second) {
                            ++table_answers[k][v];
                        }
                    }
                }
            }
        }
        std::ranges::set_intersection(seen_pointer, seen_null,
                                      std::inserter(fillable, fillable.end()));
        std::map<u64, SlotSet> together;
        for (const u64 a : fillable) {
            for (const u64 b : fillable) {
                if (b != a) {
                    together[a].insert(b);
                }
            }
        }
        std::set<std::pair<u64, u64>> both_pointers;
        for (const auto& [key, envs] : all) {
            for (const auto& env : envs) {
                std::map<u64, u8> answers;
                for (const auto& [k, v] : env.storage_pointer_types) {
                    if (fillable.contains(k)) {
                        answers.insert_or_assign(k, v);
                    }
                }
                for (const auto& [a, va] : answers) {
                    auto& with = together[a];
                    for (auto it = with.begin(); it != with.end();) {
                        const auto found = answers.find(*it);
                        if (found != answers.end()) {
                            if (found->second != va) {
                                it = with.erase(it);
                                continue;
                            }
                            if (va) {
                                both_pointers.insert({a, *it});
                            }
                        }
                        ++it;
                    }
                }
            }
        }
        for (const u64 a : fillable) {
            if (slot_group.contains(a)) {
                continue;
            }
            SlotSet group{a};
            for (const u64 b : together[a]) {
                if (together[b].contains(a) && both_pointers.contains({a, b})) {
                    group.insert(b);
                }
            }
            for (const u64 b : group) {
                slot_group.try_emplace(b, group);
            }
        }
    }

    /// The environments to prepare for one compute key, and how many of them are recorded.
    std::pair<std::vector<std::vector<u8>>, size_t> Of(const std::vector<CachedEnv>& envs) {
        // The last of those asking the most pointer questions (max over the reversed list).
        const CachedEnv* base = &envs.back();
        for (auto it = envs.rbegin(); it != envs.rend(); ++it) {
            if (it->storage_pointer_types.size() > base->storage_pointer_types.size()) {
                base = &*it;
            }
        }
        std::vector<u64> questions;
        for (const auto& [k, v] : base->storage_pointer_types) {
            questions.push_back(k);
        }
        using State = std::vector<u8>;
        std::vector<State> states;
        std::vector<std::vector<u8>> out;
        std::set<std::vector<std::pair<u64, u8>>> recorded_answers;
        const auto state_of = [&questions](const std::map<u64, u8>& answers) {
            State state;
            for (const u64 k : questions) {
                const auto it = answers.find(k);
                state.push_back(it == answers.end() ? u8{0} : it->second);
            }
            return state;
        };
        const auto add = [&states](State state) {
            if (std::ranges::find(states, state) == states.end()) {
                states.push_back(std::move(state));
            }
        };
        for (const auto& env : envs) {
            auto answers = env.storage_pointer_types;
            std::ranges::sort(answers);
            if (recorded_answers.insert(answers).second) {
                out.push_back(env.Serialize());
                std::map<u64, u8> as_map;
                for (const auto& [k, v] : answers) {
                    as_map.insert_or_assign(k, v);
                }
                add(state_of(as_map));
            }
        }
        const size_t recorded_states = states.size();
        const size_t recorded = out.size();
        for (const auto& env : envs) {
            const auto nulls = NullSlots(env);
            std::set<SlotSet> group_set;
            for (const u64 k : nulls) {
                if (!fillable.contains(k)) {
                    continue;
                }
                const SlotSet& group = slot_group.at(k);
                if (std::ranges::includes(nulls, group)) {
                    group_set.insert(group);
                }
            }
            std::vector<SlotSet> groups(group_set.begin(), group_set.end());
            std::ranges::stable_sort(groups, [](const SlotSet& a, const SlotSet& b) {
                return *a.begin() < *b.begin();
            });
            if (groups.size() > MaxFilledGroups) {
                SlotSet all;
                for (const auto& group : groups) {
                    all.insert(group.begin(), group.end());
                }
                groups = {all};
                Count("compute states with more null slot groups than enumerated");
            }
            for (u32 mask = 1; mask < (1U << groups.size()); ++mask) {
                SlotSet filled;
                for (size_t i = 0; i < groups.size(); ++i) {
                    if ((mask >> i) & 1) {
                        filled.insert(groups[i].begin(), groups[i].end());
                    }
                }
                std::map<u64, u8> answers;
                for (const auto& [k, v] : env.storage_pointer_types) {
                    answers.insert_or_assign(k, FilledAnswer(k, v, filled));
                }
                add(state_of(answers));
            }
        }
        bool grown = true;
        while (grown && states.size() < MaxComputeStates) {
            grown = false;
            const auto outer = states;
            for (const auto& a : outer) {
                const auto inner = states;
                for (const auto& b : inner) {
                    State state(a.size());
                    for (size_t i = 0; i < a.size(); ++i) {
                        state[i] = std::max(a[i], b[i]);
                    }
                    if (std::ranges::find(states, state) == states.end() &&
                        states.size() < MaxComputeStates) {
                        states.push_back(std::move(state));
                        grown = true;
                    }
                }
            }
        }
        if (grown) {
            Count("compute programs with more pointer states than prepared");
        }
        for (size_t i = recorded_states; i < states.size(); ++i) {
            CachedEnv env = *base;
            env.storage_pointer_types.clear();
            for (size_t q = 0; q < questions.size(); ++q) {
                env.storage_pointer_types.emplace_back(questions[q], states[i][q]);
            }
            out.push_back(env.Serialize());
        }
        return {std::move(out), recorded};
    }

private:
    u8 FilledAnswer(u64 key, u8 value, const SlotSet& filled) const {
        if (filled.contains(key)) {
            return 1;
        }
        if ((key & Indirect) && filled.contains(key & HolderMask)) {
            const auto* const seen = table_answers.Find(key);
            return seen && !seen->Empty() ? seen->MostCommon(1).front().first : u8{0};
        }
        return value;
    }

    void Count(const std::string& what) {
        for (auto& [name, n] : counts) {
            if (name == what) {
                ++n;
                return;
            }
        }
        counts.emplace_back(what, 1);
    }

    PrecompiledCounts& counts;
    SlotSet fillable;
    std::map<u64, SlotSet> slot_group;
    CounterTable<u64, Counter<u8>> table_answers;
};

} // Anonymous namespace

// ------------------------------------------------------------------------------------------------
// lichii_cache.py

std::vector<u8> CachedEnv::Serialize() const {
    std::vector<u8> out;
    out.reserve(EnvHeaderSize + code.size() + 64);
    Put(out, code_size);
    Put(out, static_cast<u64>(texture_types.size()));
    Put(out, static_cast<u64>(texture_pixel_formats.size()));
    Put(out, static_cast<u64>(cbuf_values.size()));
    Put(out, static_cast<u64>(storage_pointer_types.size()));
    Put(out, static_cast<u64>(cbuf_replacements.size()));
    Put(out, local_memory_size);
    Put(out, texture_bound);
    Put(out, start_address);
    Put(out, cached_lowest);
    Put(out, cached_highest);
    Put(out, viewport_transform_state);
    Put(out, stage);
    out.insert(out.end(), code.begin(), code.end());
    for (const auto& [k, v] : texture_types) {
        Put(out, k);
        Put(out, v);
    }
    for (const auto& [k, v] : texture_pixel_formats) {
        Put(out, k);
        Put(out, v);
    }
    for (const auto& [k, v] : cbuf_values) {
        Put(out, k);
        Put(out, v);
    }
    for (const auto& [k, v] : storage_pointer_types) {
        Put(out, k);
        Put(out, v);
    }
    for (const auto& [k, v] : cbuf_replacements) {
        Put(out, k);
        Put(out, v);
    }
    if (stage == StageCompute) {
        for (const u32 size : workgroup_size) {
            Put(out, size);
        }
        Put(out, shared_memory_size);
    } else {
        out.insert(out.end(), sph.begin(), sph.end());
        if (stage == StageGeometry) {
            for (const u32 word : gp_passthrough_mask) {
                Put(out, word);
            }
        }
        if (has_mesh_fields && (stage == StageTask || stage == StageMesh)) {
            for (const u32 word : mesh_registers) {
                Put(out, word);
            }
            out.insert(out.end(), mesh_primitive_header.begin(), mesh_primitive_header.end());
        }
    }
    return out;
}

size_t ParseCachedEnv(std::span<const u8> bytes, size_t offset, CachedEnv& env) {
    const size_t begin = offset;
    if (offset + EnvHeaderSize > bytes.size()) {
        throw std::runtime_error(fmt::format("truncated env header at {}", offset));
    }
    env.code_size = Load<u64>(bytes, offset);
    const u64 counts[5]{Load<u64>(bytes, offset + 8), Load<u64>(bytes, offset + 16),
                        Load<u64>(bytes, offset + 24), Load<u64>(bytes, offset + 32),
                        Load<u64>(bytes, offset + 40)};
    env.local_memory_size = Load<u32>(bytes, offset + 48);
    env.texture_bound = Load<u32>(bytes, offset + 52);
    env.start_address = Load<u32>(bytes, offset + 56);
    env.cached_lowest = Load<u32>(bytes, offset + 60);
    env.cached_highest = Load<u32>(bytes, offset + 64);
    env.viewport_transform_state = Load<u32>(bytes, offset + 68);
    env.stage = Load<u32>(bytes, offset + 72);
    offset += EnvHeaderSize;
    if (env.stage >= 9) {
        throw std::runtime_error(fmt::format("bad stage {} at {}", env.stage, begin));
    }
    if (env.code_size > (64ULL << 20) ||
        *std::max_element(std::begin(counts), std::end(counts)) > 1'000'000) {
        throw std::runtime_error(fmt::format("implausible sizes at {}", begin));
    }
    if (offset + env.code_size > bytes.size()) {
        throw std::runtime_error(fmt::format("truncated code at {}", begin));
    }
    env.code.assign(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                    bytes.begin() + static_cast<std::ptrdiff_t>(offset + env.code_size));
    offset += env.code_size;
    const auto take = [&](auto& list, u64 count) {
        using Pair = typename std::remove_reference_t<decltype(list)>::value_type;
        constexpr size_t size = sizeof(typename Pair::first_type) + sizeof(typename Pair::second_type);
        if (offset + count * size > bytes.size()) {
            throw std::runtime_error(fmt::format("truncated map at {}", offset));
        }
        list.resize(count);
        for (auto& [k, v] : list) {
            std::memcpy(&k, bytes.data() + offset, sizeof(k));
            std::memcpy(&v, bytes.data() + offset + sizeof(k), sizeof(v));
            offset += size;
        }
    };
    take(env.texture_types, counts[0]);
    take(env.texture_pixel_formats, counts[1]);
    take(env.cbuf_values, counts[2]);
    take(env.storage_pointer_types, counts[3]);
    take(env.cbuf_replacements, counts[4]);
    if (env.stage == StageCompute) {
        for (u32& size : env.workgroup_size) {
            size = Load<u32>(bytes, offset);
            offset += 4;
        }
        env.shared_memory_size = Load<u32>(bytes, offset);
        offset += 4;
    } else {
        if (offset + SphSize > bytes.size()) {
            throw std::runtime_error(fmt::format("truncated sph at {}", offset));
        }
        env.sph.assign(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                       bytes.begin() + static_cast<std::ptrdiff_t>(offset + SphSize));
        offset += SphSize;
        if (env.stage == StageGeometry) {
            for (u32& word : env.gp_passthrough_mask) {
                word = Load<u32>(bytes, offset);
                offset += 4;
            }
        }
        if (env.stage == StageTask || env.stage == StageMesh) {
            if (offset + 12 + SphSize > bytes.size()) {
                throw std::runtime_error(fmt::format("truncated mesh fields at {}", offset));
            }
            env.has_mesh_fields = true;
            for (u32& word : env.mesh_registers) {
                word = Load<u32>(bytes, offset);
                offset += 4;
            }
            env.mesh_primitive_header.assign(
                bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                bytes.begin() + static_cast<std::ptrdiff_t>(offset + SphSize));
            offset += SphSize;
        }
    }
    return offset;
}

std::vector<CachedPipeline> ParseCacheFile(std::span<const u8> bytes) {
    if (bytes.size() < 12 || std::memcmp(bytes.data(), "yuzucach", 8) != 0) {
        throw std::runtime_error("not a pipeline cache file");
    }
    std::vector<CachedPipeline> records;
    size_t offset = 12;
    while (offset != bytes.size()) {
        const u32 count = Load<u32>(bytes, offset);
        offset += 4;
        if (count < 1 || count > 6) {
            throw std::runtime_error(fmt::format("record {}: bad environment count {}",
                                                 records.size(), count));
        }
        CachedPipeline record;
        record.envs.resize(count);
        for (auto& env : record.envs) {
            offset = ParseCachedEnv(bytes, offset, env);
        }
        const size_t key_size = record.IsCompute() ? ComputeKeySize : GraphicsKeySize;
        if (offset + key_size > bytes.size()) {
            throw std::runtime_error(fmt::format("record {}: truncated key", records.size()));
        }
        record.key.assign(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                          bytes.begin() + static_cast<std::ptrdiff_t>(offset + key_size));
        offset += key_size;
        records.push_back(std::move(record));
    }
    return records;
}

u64 NativeShaderHash(std::span<const u8> code) {
    return Common::CityHash64(reinterpret_cast<const char*>(code.data()), code.size()) ^
           NativeHashTag;
}

u64 GraphicsKeyHash(std::span<const u8> key) {
    // FixedPipelineState::Size
    const u32 raw1 = U32(key, StateOffset);
    size_t size = 432; // xfb_state
    if (raw1 & XfbEnabled) {
        size = 1008;
    } else if ((raw1 & DynamicVertexInput) && (raw1 & Eds3Blend)) {
        size = 80; // attachments
    } else if (raw1 & DynamicVertexInput) {
        size = 112; // attributes
    } else if (raw1 & 1) {
        size = 368; // vertex_strides
    }
    return Common::CityHash64(reinterpret_cast<const char*>(key.data()), StateOffset + size);
}

// ------------------------------------------------------------------------------------------------
// learn_priors.py

std::vector<LearnedPrior> LearnPriors(std::span<const CachedPipeline> records) {
    // (stage, indirect, bank, offset, table) -> votes
    using VoteKey = std::tuple<u32, u32, u32, u32, u32>;
    std::map<VoteKey, Counter<u8>> votes;
    std::set<std::tuple<u32, u64, u64>> seen; // (stage, program hash, question)
    for (const auto& record : records) {
        for (const auto& env : record.envs) {
            const u64 program = NativeShaderHash(env.code);
            for (const auto& [key, answer] : env.storage_pointer_types) {
                if (!seen.insert({env.stage, program, key}).second) {
                    continue;
                }
                const u32 indirect = static_cast<u32>(key >> 63);
                const u32 bank = static_cast<u32>((key >> 32) & 0xFF);
                const u32 offset = static_cast<u32>(key & 0xFFFFFFFF);
                const u32 table = indirect ? static_cast<u32>((key >> 40) & 0x3FFFFF) : 0;
                ++votes[{env.stage, indirect, bank, offset, table}][answer];
            }
        }
    }
    std::vector<LearnedPrior> priors;
    for (const auto& [key, counter] : votes) {
        const u8 answer = counter.MostCommon(1).front().first;
        if (answer == 1) {
            continue;
        }
        const auto [stage, indirect, bank, offset, table] = key;
        priors.push_back({.stage = stage,
                          .indirect = indirect != 0,
                          .bank = bank,
                          .offset = offset,
                          .table = table,
                          .answer = answer});
    }
    return priors;
}

// ------------------------------------------------------------------------------------------------
// game_tables.py

namespace {
/// re.sub(r'\d+', '#', name)
std::string NumbersAsHash(std::string_view name) {
    std::string out;
    for (size_t i = 0; i < name.size();) {
        if (name[i] >= '0' && name[i] <= '9') {
            while (i < name.size() && name[i] >= '0' && name[i] <= '9') {
                ++i;
            }
            out += '#';
        } else {
            out += name[i++];
        }
    }
    return out;
}

/// [int(n) for n in re.findall(r'\d+', name)]
std::vector<u64> Numbers(std::string_view name) {
    std::vector<u64> out;
    for (size_t i = 0; i < name.size();) {
        if (name[i] >= '0' && name[i] <= '9') {
            u64 value = 0;
            while (i < name.size() && name[i] >= '0' && name[i] <= '9') {
                value = value * 10 + static_cast<u64>(name[i] - '0');
                ++i;
            }
            out.push_back(value);
        } else {
            ++i;
        }
    }
    return out;
}
} // Anonymous namespace

GameTables MakeGameTables(std::span<const ExtractedProgram> extracted,
                          std::span<const ExtractedVariation> variations,
                          std::span<const ScannedProgram> scanned,
                          std::span<const CachedPipeline> used_records) {
    if (extracted.size() != scanned.size()) {
        throw std::runtime_error("the scan and the extract hold different programs");
    }
    GameTables tables;
    tables.rows.resize(extracted.size());
    std::unordered_map<u64, u32> by_fingerprint;
    for (u32 i = 0; i < extracted.size(); ++i) {
        const std::string& origin = extracted[i].origin;
        auto& row = tables.rows[i];
        row.stage = scanned[i].stage;
        row.code_hash = scanned[i].code_hash;
        const size_t var = origin.find("#var");
        row.fine = var == std::string::npos ? origin : origin.substr(0, var);
        const size_t hash = origin.find('#');
        row.coarse = hash == std::string::npos ? origin : origin.substr(0, hash);
        by_fingerprint[extracted[i].fingerprint] = i;
    }
    std::unordered_set<u64> used;
    for (const auto& record : used_records) {
        if (record.IsCompute()) {
            used.insert(Load<u64>(record.key, 0));
        } else {
            for (u32 slot = 0; slot < 6; ++slot) {
                if (const u64 hash = Load<u64>(record.key, slot * 8)) {
                    used.insert(hash);
                }
            }
        }
    }
    // Variations as (file, member rows), the files in order of first appearance.
    std::vector<std::pair<std::string, std::vector<u32>>> files_of_variations;
    std::vector<std::string> file_order;
    std::unordered_map<std::string, std::set<u32>> files; // file -> member ordinals
    for (const auto& variation : variations) {
        const size_t hash = variation.label.find('#');
        std::string file = hash == std::string::npos ? variation.label : variation.label.substr(0, hash);
        std::vector<u32> members;
        for (const u32 program : variation.programs) {
            const auto it = by_fingerprint.find(extracted[program].fingerprint);
            if (it != by_fingerprint.end()) {
                members.push_back(it->second);
            }
        }
        const auto [it, added] = files.try_emplace(file);
        if (added) {
            file_order.push_back(file);
        }
        it->second.insert(members.begin(), members.end());
        files_of_variations.emplace_back(std::move(file), std::move(members));
    }
    // Unused archive versions.
    std::vector<std::pair<std::string, std::vector<std::string>>> groups;
    std::unordered_map<std::string, size_t> group_index;
    for (const auto& file : file_order) {
        if (file.find(".bfsha") == std::string::npos) {
            continue;
        }
        const auto name = NumbersAsHash(file);
        const auto [it, added] = group_index.try_emplace(name, groups.size());
        if (added) {
            groups.emplace_back(name, std::vector<std::string>{});
        }
        groups[it->second].second.push_back(file);
    }
    std::set<std::string> left_out;
    for (const auto& [name, group] : groups) {
        if (group.size() < 2) {
            continue;
        }
        const auto drawn = [&](const std::string& file) {
            std::unordered_set<u64> mine, others;
            for (const u32 row : files.at(file)) {
                mine.insert(tables.rows[row].code_hash);
            }
            for (const auto& other : group) {
                if (other != file) {
                    for (const u32 row : files.at(other)) {
                        others.insert(tables.rows[row].code_hash);
                    }
                }
            }
            size_t count = 0;
            for (const u64 hash : mine) {
                count += !others.contains(hash) && used.contains(hash);
            }
            return count;
        };
        std::vector<std::string> dropped;
        std::string why;
        bool any = false;
        std::vector<size_t> counts;
        for (const auto& file : group) {
            counts.push_back(drawn(file));
            any |= counts.back() != 0;
        }
        if (any) {
            for (size_t i = 0; i < group.size(); ++i) {
                if (counts[i] == 0) {
                    dropped.push_back(group[i]);
                }
            }
            why = "none of its own programs drawn, while another version's were";
        } else {
            const std::string* newest = &group.front();
            for (const auto& file : group) {
                if (Numbers(file) > Numbers(*newest)) {
                    newest = &file;
                }
            }
            for (const auto& file : group) {
                if (&file != newest) {
                    dropped.push_back(file);
                }
            }
            why = "older than " + *newest;
        }
        std::ranges::sort(dropped);
        for (const auto& file : dropped) {
            tables.left_out_files.push_back(fmt::format("{}: {}", file, why));
            left_out.insert(file);
        }
    }
    std::unordered_set<u32> kept;
    for (const auto& file : file_order) {
        if (!left_out.contains(file)) {
            kept.insert(files[file].begin(), files[file].end());
        }
    }
    for (u32 i = 0; i < tables.rows.size(); ++i) {
        bool in_left_out = false;
        for (const auto& file : left_out) {
            in_left_out |= files[file].contains(i);
        }
        tables.rows[i].left_out = !kept.contains(i) && in_left_out;
    }
    std::set<std::pair<u64, u64>> pairs;
    std::set<std::tuple<bool, u64, u64, u64>> combos;
    for (const auto& [file, members] : files_of_variations) {
        if (left_out.contains(file)) {
            continue;
        }
        std::vector<u64> vertex, fragment, mesh, task;
        for (const u32 member : members) {
            const auto& row = tables.rows[member];
            if (row.stage == "VertexB") {
                vertex.push_back(row.code_hash);
            } else if (row.stage == "Fragment") {
                fragment.push_back(row.code_hash);
            } else if (row.stage == "Mesh") {
                mesh.push_back(row.code_hash);
            } else if (row.stage == "Task") {
                task.push_back(row.code_hash);
            }
        }
        for (const u64 v : vertex) {
            for (const u64 p : fragment) {
                pairs.insert({v, p});
            }
        }
        for (const u64 m : mesh) {
            for (const u64 p : fragment) {
                if (task.empty()) {
                    combos.insert({false, 0, m, p});
                }
                for (const u64 t : task) {
                    combos.insert({true, t, m, p});
                }
            }
        }
    }
    tables.pairs.assign(pairs.begin(), pairs.end());
    for (const auto& [has_task, task, mesh, pixel] : combos) {
        tables.combos.push_back({.task = has_task ? std::optional<u64>{task} : std::nullopt,
                                 .mesh = mesh,
                                 .pixel = pixel});
    }
    return tables;
}

// ------------------------------------------------------------------------------------------------
// mesh_draws.py

std::vector<MeshDraw> MakeMeshDraws(const GameTables& tables,
                                    std::span<const std::span<const u8>> controls) {
    std::unordered_map<u64, u32> where;
    for (u32 i = 0; i < tables.rows.size(); ++i) {
        if (tables.rows[i].stage == "Mesh") {
            where.try_emplace(tables.rows[i].code_hash, i);
        }
    }
    // Sorted as the script's ("-" sorts before any hash).
    std::set<std::tuple<bool, u64, u64, u32>> draws;
    for (const auto& combo : tables.combos) {
        const auto control = controls[where.at(combo.mesh)];
        if (combo.task) {
            const u32 payload = Load<u16>(control, 0x5A);
            draws.insert({true, *combo.task, combo.mesh, std::max<u32>(1, payload / 128)});
        } else {
            for (const u32 lines : {25U, 2U, 9U}) {
                draws.insert({false, 0, combo.mesh, lines});
            }
        }
    }
    std::vector<MeshDraw> out;
    for (const auto& [has_task, task, mesh, lines] : draws) {
        out.push_back({.task = has_task ? std::optional<u64>{task} : std::nullopt,
                       .mesh = mesh,
                       .lines = lines});
    }
    return out;
}

// ------------------------------------------------------------------------------------------------
// build_precompiled.py

PrecompiledCounts WritePrecompiled(const std::filesystem::path& out, const GameTables& tables,
                                   const std::map<u64, std::vector<u8>>& oracle,
                                   std::span<const CachedPipeline> train,
                                   std::span<const MeshEnvironments> mesh_envs,
                                   const PrecompiledOptions& options) {
    PrecompiledCounts counts;
    const auto count = [&counts](const std::string& what, u64 n) {
        for (auto& [name, total] : counts) {
            if (name == what) {
                total += n;
                return;
            }
        }
        counts.emplace_back(what, n);
    };
    std::unordered_map<std::string, std::vector<const InventoryRow*>> by_hash;
    for (const auto& row : tables.rows) {
        by_hash[Hex(row.code_hash)].push_back(&row);
    }
    const auto containers = [&by_hash](const std::string& hash, ShaderInfo& info) {
        const auto it = by_hash.find(hash);
        info.fine = it == by_hash.end() ? "?" : it->second.front()->fine;
        info.coarse = it == by_hash.end() ? "?" : it->second.front()->coarse;
    };
    const auto shader_info = [&](const CachedEnv& env, bool vertex) {
        ShaderInfo info;
        info.hash = Hex(NativeShaderHash(env.code));
        containers(info.hash, info);
        if (vertex) {
            info.read_mask = AttributeReadMask(env.sph);
        } else {
            info.omap = U32(env.sph, 0x48);
        }
        return info;
    };

    Writer writer;
    Model model;
    Model mesh_model;
    std::set<std::pair<u64, u64>> pairs(tables.pairs.begin(), tables.pairs.end());
    // A value no translation reads, which builds since cache version 54 record in each
    // environment: left out, so it does not reach the precompiled file.
    [[maybe_unused]] constexpr u64 ReadPropertiesKey = 0xFFFFFFFFULL << 32;
    std::vector<std::pair<std::vector<u8>, std::vector<CachedEnv>>> recorded_compute;
    std::map<std::vector<u8>, size_t> recorded_compute_index;
    const auto add_compute = [&](const std::vector<u8>& key, CachedEnv env) {
        const auto [it, added] = recorded_compute_index.try_emplace(key, recorded_compute.size());
        if (added) {
            recorded_compute.emplace_back(key, std::vector<CachedEnv>{});
        }
        recorded_compute[it->second].second.push_back(std::move(env));
    };
    for (const auto& source : train) {
        CachedPipeline record = source;
        for (auto& env : record.envs) {
            std::erase_if(env.cbuf_values,
                          [](const auto& entry) { return entry.first == ReadPropertiesKey; });
        }
        std::vector<std::vector<u8>> blobs;
        for (const auto& env : record.envs) {
            blobs.push_back(env.Serialize());
        }
        if (record.IsCompute()) {
            add_compute(record.key, record.envs.front());
            continue;
        }
        const bool has_mesh = std::ranges::any_of(record.envs, [](const CachedEnv& env) {
            return env.stage == StageTask || env.stage == StageMesh;
        });
        if (has_mesh && std::ranges::any_of(record.envs, [](const CachedEnv& env) {
                return env.stage == StageMesh && (env.mesh_registers[0] & 0xF) != 4;
            })) {
            count("recorded mesh skipped (topology)", 1);
            continue;
        }
        const auto key = CanonicalKey(record);
        std::vector<const std::vector<u8>*> blob_ptrs;
        for (const auto& blob : blobs) {
            blob_ptrs.push_back(&blob);
        }
        count("recorded graphics", writer.Graphics(blob_ptrs, key) ? 1 : 0);
        const auto state = std::span<const u8>(key).subspan(StateOffset, StateSize);
        // The model learns from keys with dynamic vertex input only.
        if (HashesPastPrefix(state)) {
            continue;
        }
        const auto& envs = record.envs;
        if (envs.front().stage == StageVertexB && envs.back().stage == StageFragment &&
            envs.size() == 2) {
            const auto vs = shader_info(envs.front(), true);
            const auto fs = shader_info(envs.back(), false);
            model.Add(vs, fs, state);
            pairs.insert({std::stoull(vs.hash, nullptr, 16), std::stoull(fs.hash, nullptr, 16)});
        } else if (std::ranges::any_of(envs, [](const CachedEnv& e) { return e.stage == StageMesh; }) &&
                   envs.back().stage == StageFragment) {
            const auto& mesh_env = *std::ranges::find_if(
                envs, [](const CachedEnv& e) { return e.stage == StageMesh; });
            auto ms = shader_info(mesh_env, true);
            ms.read_mask = 0;
            mesh_model.Add(ms, shader_info(envs.back(), false), state);
        }
    }

    const auto oracle_info = [&](u64 hash, bool vertex) {
        ShaderInfo info;
        info.hash = Hex(hash);
        containers(info.hash, info);
        // The SPH is the start of the lowered code, right after the environment's header.
        const auto& blob = oracle.at(hash);
        const auto sph = std::span<const u8>(blob).subspan(EnvHeaderSize, SphSize);
        if (vertex) {
            info.read_mask = AttributeReadMask(sph);
        } else {
            info.omap = U32(sph, 0x48);
        }
        return info;
    };
    const auto unique_hashes = [](std::array<u64, 6> hashes) {
        std::vector<u8> out;
        for (const u64 hash : hashes) {
            Put(out, hash);
        }
        return out;
    };
    for (const auto& [vh, fh] : pairs) {
        if (!oracle.contains(vh) || !oracle.contains(fh)) {
            count("pair without oracle environment", 1);
            continue;
        }
        const auto vs = oracle_info(vh, true);
        const auto fs = oracle_info(fh, false);
        u32 added = 0;
        for (auto state : model.Candidates(vs, fs, options.k + 2)) {
            if (HashesPastPrefix(state)) {
                continue;
            }
            state = Canonical(state, vs.read_mask);
            auto key = unique_hashes({0, vh, 0, 0, 0, fh});
            key.insert(key.end(), state.begin(), state.end());
            key.resize(GraphicsKeySize, 0);
            if (writer.Graphics({&oracle.at(vh), &oracle.at(fh)}, key)) {
                ++added;
                count("predicted graphics", 1);
            }
            if (added >= options.k) {
                break;
            }
        }
    }

    if (!mesh_envs.empty() || !tables.combos.empty()) {
        mesh_model.TakeMissingTargets(model);
        std::map<std::pair<u64, u64>, std::vector<const MeshEnvironments*>> draws;
        for (const auto& entry : mesh_envs) {
            draws[{entry.task, entry.mesh}].push_back(&entry);
        }
        for (const auto& combo : tables.combos) {
            const u64 task = combo.task.value_or(0);
            const auto draw = draws.find({task, combo.mesh});
            if (!oracle.contains(combo.pixel) || draw == draws.end()) {
                count("mesh draw without environments", 1);
                continue;
            }
            ShaderInfo ms;
            ms.hash = Hex(combo.mesh);
            containers(ms.hash, ms);
            const auto fs = oracle_info(combo.pixel, false);
            const auto states = mesh_model.Candidates(ms, fs, options.mesh_k);
            for (const auto* const entry : draw->second) {
                std::array<u64, 6> hashes{};
                hashes[5] = combo.pixel;
                std::vector<const std::vector<u8>*> blobs;
                if (task) {
                    hashes[1] = task ^ entry->state_hash;
                    hashes[3] = combo.mesh ^ entry->state_hash;
                    blobs = {&entry->task_env, &entry->mesh_env, &oracle.at(combo.pixel)};
                } else {
                    hashes[1] = combo.mesh ^ entry->state_hash;
                    blobs = {&entry->mesh_env, &oracle.at(combo.pixel)};
                }
                for (const auto& state : states) {
                    if (HashesPastPrefix(state)) {
                        continue;
                    }
                    auto key = unique_hashes(hashes);
                    const auto canonical = Canonical(state, 0);
                    key.insert(key.end(), canonical.begin(), canonical.end());
                    key.resize(GraphicsKeySize, 0);
                    count("predicted mesh", writer.Graphics(blobs, key) ? 1 : 0);
                }
            }
        }
    }

    for (const auto& [hash, blob] : oracle) {
        const auto rows = by_hash.find(Hex(hash));
        if (rows == by_hash.end() || rows->second.front()->stage != "Compute" ||
            std::ranges::all_of(rows->second, [](const InventoryRow* row) { return row->left_out; })) {
            continue;
        }
        const auto tail = std::span<const u8>(blob).subspan(blob.size() - 16);
        std::vector<u8> key;
        Put(key, hash);
        Put(key, U32(tail, 12)); // shared memory
        Put(key, U32(tail, 0));
        Put(key, U32(tail, 4));
        Put(key, U32(tail, 8));
        if (!recorded_compute_index.contains(key)) {
            CachedEnv env;
            ParseCachedEnv(blob, 0, env);
            add_compute(key, std::move(env));
            count("oracle compute", 1);
        }
    }
    ComputeStates compute_states{recorded_compute, counts};
    for (const auto& [key, envs] : recorded_compute) {
        const auto [blobs, recorded] = compute_states.Of(envs);
        u64 recorded_added = 0, rebuilt_added = 0;
        for (size_t i = 0; i < blobs.size(); ++i) {
            const bool added = writer.Compute(blobs[i], key);
            (i < recorded ? recorded_added : rebuilt_added) += added ? 1 : 0;
        }
        count("compute (recorded or oracle states)", recorded_added);
        count("compute pointer states reached by rebuilds", rebuilt_added);
        if (blobs.size() > 1) {
            count("compute programs with several pointer states", 1);
        }
    }
    writer.Write(out, options.cache_version);
    counts.emplace_back("environments", writer.EnvCount());
    counts.emplace_back("records", writer.RecordCount());
    return counts;
}

} // namespace VideoCommon::Nx2Precompile
