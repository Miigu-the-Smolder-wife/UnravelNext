#pragma once
// Cluster streaming's glue (visibility.cluster_streaming): the cook's cluster pages (clusterbuilder::StreamPages) written
// to a page file, C's streamer opened on it (Passes/Streaming: NVMe -> RAM cache -> a VRAM pool by DirectStorage, the
// pool's budget and its eviction), and that streamer given to V as its page source (Visibility.h ClusterPageSource).
// For the owner of the renderer - the host, a gate: a translation unit that sees both modules. V's module does not
// include this (modules do not link each other); include it under __has_include("unx/streaming/Streaming.h").
//   const auto pages = visibility::openClusterPages(device, quality, streamPages, directory, framesInFlight);
//   ... for every renderer on the scene: visibility::installClusterPages(renderer.trackState(), pages);
// The page file is <directory>/unx_cluster_pages_<process id>_<n>.unxp and is removed with the last owner.
#include "unx/clusterbuilder/ClusterStream.h"
#include "unx/core/Config.h"
#include "unx/streaming/Streaming.h"
#include "unx/visibility/Visibility.h"

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>

#include <windows.h>

namespace unx::visibility
{
struct ClusterPageFile
{
    std::unique_ptr<streaming::Streamer> streamer;
    uint32_t file = 0, pages = 0;
    std::string path;
    ClusterPageFile() = default;
    ClusterPageFile(const ClusterPageFile&) = delete;
    ClusterPageFile& operator=(const ClusterPageFile&) = delete;
    ~ClusterPageFile()
    {
        streamer.reset();  // (its reads are done before the file goes)
        std::error_code ignored;
        if (!path.empty()) std::filesystem::remove(path, ignored);
    }
};

// Null when the build made no page (the setting off, nothing compressed).
//   visibility.cluster_streaming_pool_mb    the VRAM pool (0: a quarter of the device's free budget, the streamer's rule)
//   visibility.cluster_streaming_upload_mb  page bytes uploaded per frame at most
inline std::shared_ptr<ClusterPageFile> openClusterPages(render::Device& device, const QualityConfig& quality, const clusterbuilder::StreamPages& pages,
                                                         const std::string& directory, uint32_t framesInFlight)
{
    if (pages.pages.empty()) return nullptr;
    static std::atomic<uint32_t> serial{ 0 };
    auto out = std::make_shared<ClusterPageFile>();
    const std::filesystem::path dir = directory.empty() ? std::filesystem::temp_directory_path() : std::filesystem::path(directory);
    out->path = (dir / ("unx_cluster_pages_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(serial++) + ".unxp")).string();
    streaming::PageFileWriter::write(out->path, pages.pages);
    streaming::Settings settings;
    settings.slotBytes = clusterbuilder::kStreamPageBytes;
    settings.framesInFlight = framesInFlight;
    if (quality.has("visibility.cluster_streaming_pool_mb")) settings.vramPoolBytes = (uint64_t)std::max<int64_t>(quality.integer("visibility.cluster_streaming_pool_mb"), 0) << 20;
    if (quality.has("visibility.cluster_streaming_upload_mb"))
        settings.uploadBytesPerFrame = (uint64_t)std::max<int64_t>(quality.integer("visibility.cluster_streaming_upload_mb"), 1) << 20;
    out->streamer = std::make_unique<streaming::Streamer>(device, settings);
    out->file = out->streamer->addFile(out->path);
    out->pages = (uint32_t)pages.pages.size();
    return out;
}

inline void installClusterPages(render::TrackState& trackState, const std::shared_ptr<ClusterPageFile>& pages)
{
    if (!pages) return;
    ClusterPageSource source;
    source.pageCount = pages->pages;
    source.slotBytes = pages->streamer->settings().slotBytes;
    source.heapSlots = pages->streamer->settings().heapSlots;
    source.request = [pages](uint32_t page, float priority) { pages->streamer->request(pages->file, page, priority); };
    source.update = [pages](uint64_t frameIndex) { pages->streamer->update(frameIndex); };
    source.residentSlot = [pages](uint32_t page) { return pages->streamer->residentSlot(pages->file, page); };
    source.heapCount = [pages]() { return pages->streamer->heapCount(); };
    source.heapBuffer = [pages](uint32_t heap) { return pages->streamer->heapResident(heap) ? pages->streamer->heapBuffer(heap) : nullptr; };
    setClusterPageSource(trackState, std::move(source));
}
} // namespace unx::visibility
