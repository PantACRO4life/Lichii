// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <barrier>
#include <future>
#include <stdexcept>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/vk_pipeline_library.h"

TEST_CASE("Concurrent library failures finish once and release all callers", "[video_core][gpl]") {
    Vulkan::GraphicsPipelineLibraryCache cache;
    std::atomic_int builds{};
    std::barrier start{8};
    std::vector<std::future<std::shared_ptr<const Vulkan::GplLibrary>>> callers;
    for (int index = 0; index < 8; ++index) {
        callers.push_back(std::async(std::launch::async, [&] {
            start.arrive_and_wait();
            return cache.GetOrCreate(Vulkan::GplPart::Fragment, "failed", [&]() -> Vulkan::GplLibrary {
                ++builds;
                throw std::runtime_error("synthetic driver failure");
            }, 0);
        }));
    }
    for (auto& caller : callers) {
        CHECK_FALSE(caller.get());
    }
    CHECK(builds == 1);
    CHECK_FALSE(cache.GetOrCreate(Vulkan::GplPart::Fragment, "failed", [&] {
        ++builds;
        return Vulkan::GplLibrary{};
    }, 0));
    CHECK(builds == 1);
}

TEST_CASE("Warm-only library entries do not poison a later retry", "[video_core][gpl]") {
    Vulkan::GraphicsPipelineLibraryCache cache;
    int builds{};
    const auto build = [&] {
        ++builds;
        return Vulkan::GplLibrary{};
    };
    Vulkan::GraphicsPipelineLibraryCache::SetNoRetainOnThisThread(true);
    const auto warmed = cache.GetOrCreate(Vulkan::GplPart::PreRasterization, "warm", build, 0);
    Vulkan::GraphicsPipelineLibraryCache::SetNoRetainOnThisThread(false);
    CHECK_FALSE(warmed);
    CHECK_FALSE(cache.GetOrCreate(Vulkan::GplPart::PreRasterization, "warm", build, 0));
    CHECK(builds == 2);
    CHECK_FALSE(cache.GetOrCreate(Vulkan::GplPart::PreRasterization, "warm", build, 0));
    CHECK(builds == 2);
}

TEST_CASE("Library keys length-prefix bytes and preserve stage identity", "[video_core][gpl]") {
    Vulkan::GplKeyWriter first;
    Vulkan::GplKeyWriter second;
    first.AddBytes("a");
    first.AddBytes("bc");
    second.AddBytes("ab");
    second.AddBytes("c");
    CHECK(first.Data() != second.Data());
    Vulkan::GplKeyWriter vertex;
    Vulkan::GplKeyWriter fragment;
    vertex.Add(Vulkan::GplPart::PreRasterization);
    fragment.Add(Vulkan::GplPart::Fragment);
    CHECK(vertex.Data() != fragment.Data());
}
