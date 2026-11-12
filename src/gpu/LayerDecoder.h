// Decodes a layer's pixels or the composite into a GPU image (premultiplied RGBA8). Runs on a
// worker thread: reads the compressed channel data straight into an upload buffer (ZIP channels
// are inflated on the CPU), then the GPU unpacks every channel into 8-bit planes and merges them.
// Only the RGBA image survives; every intermediate buffer is released before returning.
#pragma once

#include "Gpu.h"
#include "psd/PsdFile.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace gpu {

struct DecodeSource {
    std::wstring path;
    psd::FileStamp stamp;
    bool psb = false;
    uint32_t width = 0, height = 0;
    uint32_t bytesPerSample = 1;  // 1 or 2
    bool gray = false;            // one colour plane instead of three
    bool hasAlpha = false;        // the last plane is transparency

    // Composite: channels `compositeChannels` of the image data section at `compositeOffset`.
    bool composite = false;
    uint64_t compositeOffset = 0;
    uint32_t fileChannels = 0;
    std::vector<uint32_t> compositeChannels;

    // Layer: one channel per plane (colour planes, then transparency).
    std::vector<psd::ChannelInfo> layerChannels;
};

struct DecodeResult {
    std::shared_ptr<Image> image;  // null on failure or cancel
    std::wstring error;
    std::wstring warning;
    bool canceled = false;
};

enum class SourceStatus { Ok, Empty, Unsupported };

// What to decode for layer record `record`, or the composite for record -1. Unsupported: the
// reason is in `problem`. Empty: the layer has no pixels.
SourceStatus makeSource(const psd::PsdFile& file, int record, DecodeSource& src, std::wstring& problem);

void decode(const DecodeSource& src, const std::atomic<bool>& cancel, DecodeResult& out);

}  // namespace gpu
