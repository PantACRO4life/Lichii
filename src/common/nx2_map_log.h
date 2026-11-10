// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <utility>
#include <vector>

#include "common/common_types.h"

/// Bounded history of Switch 2 GPU memory mapping events (GPU VA map/unmap, the device (SMMU)
/// range picked for them, the GPU-thread execution of GMMU changes, GPU-accessible memory create
/// and destroy, and guest heap/physical memory SVCs), plus the bad-mesh reports the Vulkan
/// rasterizer writes when an indexed draw reads past its vertex buffer ("Nx2BadMesh").
/// Recording takes one short mutex and copies 64 bytes; nothing is formatted until read.
namespace Common::Nx2MapLog {

enum class Kind : u8 {
    MemCreate,     ///< nx2:gpu 101 CreateGpuAccessibleMemory: gpu == cpu, handle
    MemDestroy,    ///< nx2:gpu 102 DestroyGpuAccessibleMemory
    RegMap,        ///< Nx2GpuMemoryRegistry::MapAt: gpu -> cpu, handle, offset in device field
    RegUnmap,      ///< Nx2GpuMemoryRegistry::Unmap
    RegRelease,    ///< Nx2GpuMemoryRegistry::ReleaseBacking: cpu range
    SparseCreate,  ///< sparse or GPU-only region created
    SparseDestroy, ///< sparse or GPU-only region destroyed
    SparseBack,    ///< a sparse block got memory (tile ring)
    SparseTile,    ///< nx2:gpu 216 tile map or unmap (tile ring)
    DeviceRange,   ///< nvhost_as_gpu::Nx2Map picked a device range; flags = DeviceSource
    GmmuMap,       ///< GPU thread ran the GMMU map: gpu -> device
    GmmuUnmap,     ///< GPU thread ran the GMMU unmap
    SmmuRelease,   ///< GPU thread dropped a device range (Nx2Release)
    GpuAttribute,  ///< svc 0x80 SetGpuMemoryAttribute: cpu range, handle = mask, flags = attr,
                   ///< device = result
    MemorySvc,     ///< heap/physical memory SVC: flags = svc id, gpu/cpu = its two addresses,
                   ///< device = its result
    Count,
};

/// How Nx2Map got the device range of a mapping (DeviceRange events).
enum DeviceSource : u8 {
    DeviceNew = 0,      ///< a new SMMU allocation, mapped to the CPU range now
    DeviceReused = 1,   ///< this address space already had one for the same CPU range and size
    DeviceHeap = 2,     ///< through the session's heap mapper (maps only pages not mapped yet)
    DeviceShared = 3,   ///< a reference to the process-wide range (mapped now only if new)
    DevicePrivate = 4,  ///< EDEN_NX2_PRIVATE_DEVICE_RANGES allocation
};

struct Event {
    u64 seq;
    u64 time_us; ///< since the first event
    u64 frame;   ///< presented frame count when recorded
    u64 gpu;
    u64 cpu;
    u64 device;
    u64 size;
    u32 handle;
    Kind kind;
    u8 flags;
};

[[nodiscard]] const char* KindName(Kind kind);

/// Updated once per presented frame (nx2 present), stamped on each event.
void SetFrame(u64 frame);
[[nodiscard]] u64 Frame();

void Record(Kind kind, u64 gpu, u64 cpu, u64 device, u64 size, u32 handle = 0, u8 flags = 0);

/// Events that touch [gpu, +span), [cpu, +span) or [device, +span) (a zero address is skipped),
/// newest last, at most `max` of them, from both rings.
[[nodiscard]] std::vector<Event> Find(u64 gpu, u64 cpu, u64 device, u64 span, size_t max);
/// The newest `max` events of the main ring (tile events not included), newest last.
[[nodiscard]] std::vector<Event> Recent(size_t max);

struct Totals {
    u64 events;      ///< main ring, ever recorded
    u64 tile_events; ///< tile ring, ever recorded
    u64 main_capacity;
    u64 tile_capacity;
};
[[nodiscard]] Totals GetTotals();

/// One event as a JSON object (no trailing comma).
[[nodiscard]] std::string ToJson(const Event& event);

/// Bad-mesh reports: one JSON object each, at most 256 kept (later ones count only).
void AddBadMesh(std::string json);
[[nodiscard]] std::vector<std::string> BadMeshes();
/// Draws skipped by EDEN_NX2_BAD_MESH_SKIP and draws seen bad (counted by the rasterizer).
void CountBadDraw(bool skipped);
[[nodiscard]] std::pair<u64, u64> BadDrawCounts();

} // namespace Common::Nx2MapLog
