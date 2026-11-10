// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>

#include <fmt/format.h>

#include "common/nx2_map_log.h"

namespace Common::Nx2MapLog {

namespace {

/// Mapping changes are rare next to tile maps (nx2:gpu 216 streams textures tile by tile), so
/// they get a ring of their own and keep minutes to hours of history.
constexpr size_t MAIN_CAPACITY = size_t{1} << 16;
constexpr size_t TILE_CAPACITY = size_t{1} << 15;
constexpr size_t MAX_BAD_MESHES = 256;

struct Ring {
    explicit Ring(size_t capacity) : events(capacity) {}
    std::vector<Event> events;
    u64 count{}; ///< ever recorded; the newest is at (count - 1) % capacity
};

struct State {
    std::mutex mutex;
    Ring primary{MAIN_CAPACITY};
    Ring tile{TILE_CAPACITY};
    u64 seq{};
    std::chrono::steady_clock::time_point start{std::chrono::steady_clock::now()};
    std::vector<std::string> bad_meshes;
};

State& GetState() {
    // Built on first use and never destroyed: services and the GPU thread may record while
    // the emulator shuts down.
    static State* const state = new State;
    return *state;
}

std::atomic<u64> g_frame{};
std::atomic<u64> g_bad_draws{};
std::atomic<u64> g_skipped_draws{};

bool IsTileKind(Kind kind) {
    return kind == Kind::SparseBack || kind == Kind::SparseTile;
}

bool Overlaps(u64 start, u64 size, u64 address, u64 span) {
    if (start == 0 || address == 0) {
        return false;
    }
    const u64 length = (std::max)(size, u64{1});
    return start < address + span && address < start + length;
}

/// Whether the event's `device` field is a device address (else an offset or a result).
bool HasDevice(Kind kind) {
    return kind == Kind::DeviceRange || kind == Kind::GmmuMap || kind == Kind::SmmuRelease;
}

bool Touches(const Event& event, u64 gpu, u64 cpu, u64 device, u64 span) {
    if (event.kind == Kind::MemorySvc) {
        // gpu and cpu hold the SVC's two addresses: compare them as CPU addresses.
        return Overlaps(event.gpu, event.size, cpu, span) ||
               Overlaps(event.cpu, event.size, cpu, span);
    }
    return Overlaps(event.gpu, event.size, gpu, span) ||
           Overlaps(event.cpu, event.size, cpu, span) ||
           (HasDevice(event.kind) && Overlaps(event.device, event.size, device, span));
}

void Collect(const Ring& ring, std::vector<Event>& out, u64 gpu, u64 cpu, u64 device, u64 span,
             size_t max) {
    const size_t capacity = ring.events.size();
    const u64 kept = (std::min<u64>)(ring.count, capacity);
    size_t found = 0;
    for (u64 i = 0; i < kept && found < max; ++i) {
        const Event& event = ring.events[(ring.count - 1 - i) % capacity];
        if (Touches(event, gpu, cpu, device, span)) {
            out.push_back(event);
            ++found;
        }
    }
}

} // Anonymous namespace

const char* KindName(Kind kind) {
    static constexpr std::array<const char*, static_cast<size_t>(Kind::Count)> names{
        "mem_create",   "mem_destroy", "reg_map",     "reg_unmap",    "reg_release",
        "sparse_create", "sparse_destroy", "sparse_back", "sparse_tile", "device_range",
        "gmmu_map",     "gmmu_unmap",  "smmu_release", "gpu_attribute", "memory_svc",
    };
    const auto index = static_cast<size_t>(kind);
    return index < names.size() ? names[index] : "unknown";
}

void SetFrame(u64 frame) {
    g_frame.store(frame, std::memory_order_relaxed);
}

u64 Frame() {
    return g_frame.load(std::memory_order_relaxed);
}

void Record(Kind kind, u64 gpu, u64 cpu, u64 device, u64 size, u32 handle, u8 flags) {
    State& state = GetState();
    const u64 time_us = static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
                                             std::chrono::steady_clock::now() - state.start)
                                             .count());
    const u64 frame = Frame();
    std::scoped_lock lock{state.mutex};
    Ring& ring = IsTileKind(kind) ? state.tile : state.primary;
    ring.events[ring.count % ring.events.size()] = Event{
        .seq = ++state.seq,
        .time_us = time_us,
        .frame = frame,
        .gpu = gpu,
        .cpu = cpu,
        .device = device,
        .size = size,
        .handle = handle,
        .kind = kind,
        .flags = flags,
    };
    ++ring.count;
}

std::vector<Event> Find(u64 gpu, u64 cpu, u64 device, u64 span, size_t max) {
    State& state = GetState();
    std::vector<Event> out;
    {
        std::scoped_lock lock{state.mutex};
        Collect(state.primary, out, gpu, cpu, device, span, max);
        Collect(state.tile, out, gpu, cpu, device, span, max);
    }
    std::ranges::sort(out, {}, &Event::seq);
    if (out.size() > max) {
        out.erase(out.begin(), out.end() - static_cast<std::ptrdiff_t>(max));
    }
    return out;
}

std::vector<Event> Recent(size_t max) {
    State& state = GetState();
    std::vector<Event> out;
    std::scoped_lock lock{state.mutex};
    const Ring& ring = state.primary;
    const u64 kept = (std::min<u64>)({ring.count, u64{ring.events.size()}, u64{max}});
    out.reserve(kept);
    for (u64 i = kept; i > 0; --i) {
        out.push_back(ring.events[(ring.count - i) % ring.events.size()]);
    }
    return out;
}

Totals GetTotals() {
    State& state = GetState();
    std::scoped_lock lock{state.mutex};
    return {state.primary.count, state.tile.count, state.primary.events.size(),
            state.tile.events.size()};
}

std::string ToJson(const Event& event) {
    if (event.kind == Kind::MemorySvc) {
        return fmt::format(
            R"({{"seq":{},"time_ms":{:.3f},"frame":{},"kind":"{}","svc":"{:#04x}","a":"{:#x}","b":"{:#x}","result":"{:#x}","size":"{:#x}"}})",
            event.seq, event.time_us / 1000.0, event.frame, KindName(event.kind), event.flags,
            event.gpu, event.cpu, event.device, event.size);
    }
    // The third address is a device address, the offset in the memory handle (reg_map) or a
    // result (gpu_attribute).
    const char* const third = HasDevice(event.kind)              ? "device"
                              : event.kind == Kind::RegMap       ? "offset"
                              : event.kind == Kind::GpuAttribute ? "result"
                                                                 : "unused";
    return fmt::format(
        R"({{"seq":{},"time_ms":{:.3f},"frame":{},"kind":"{}","gpu":"{:#x}","cpu":"{:#x}","{}":"{:#x}","size":"{:#x}","handle":{},"flags":{}}})",
        event.seq, event.time_us / 1000.0, event.frame, KindName(event.kind), event.gpu,
        event.cpu, third, event.device, event.size, event.handle, event.flags);
}

void AddBadMesh(std::string json) {
    State& state = GetState();
    std::scoped_lock lock{state.mutex};
    if (state.bad_meshes.size() < MAX_BAD_MESHES) {
        state.bad_meshes.push_back(std::move(json));
    }
}

std::vector<std::string> BadMeshes() {
    State& state = GetState();
    std::scoped_lock lock{state.mutex};
    return state.bad_meshes;
}

void CountBadDraw(bool skipped) {
    g_bad_draws.fetch_add(1, std::memory_order_relaxed);
    if (skipped) {
        g_skipped_draws.fetch_add(1, std::memory_order_relaxed);
    }
}

std::pair<u64, u64> BadDrawCounts() {
    return {g_bad_draws.load(std::memory_order_relaxed),
            g_skipped_draws.load(std::memory_order_relaxed)};
}

} // namespace Common::Nx2MapLog
