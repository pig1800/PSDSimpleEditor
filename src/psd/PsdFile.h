// PSD/PSB structure parser. Reads only what the editor needs: the header, the merged-data flag
// from the image resources, every layer record (with the byte positions of the parts a rename
// touches) and the positions of the channel data and the composite. Pixel data is never read here.
#pragma once

#include "FileIO.h"

#include <cstdint>
#include <string>
#include <vector>

namespace psd {

enum class ColorMode : uint16_t {
    Bitmap = 0, Grayscale = 1, Indexed = 2, RGB = 3, CMYK = 4, Multichannel = 7, Duotone = 8, Lab = 9
};

enum class LayerKind { Normal, GroupOpen, GroupClosed, GroupEnd };

struct ChannelInfo {
    int16_t id = 0;           // 0.. colour, -1 transparency, -2 user mask, -3 real user mask
    uint64_t length = 0;      // bytes, including the 2-byte compression field
    uint64_t dataOffset = 0;  // file offset of the compression field
};

struct LayerRecord {
    int32_t top = 0, left = 0, bottom = 0, right = 0;
    std::vector<ChannelInfo> channels;
    char blendKey[4] = {};
    uint8_t opacity = 255, clipping = 0, flags = 0;
    LayerKind kind = LayerKind::Normal;

    // Byte layout (absolute file offsets).
    uint64_t start = 0, end = 0;     // whole record
    uint64_t extraLenOffset = 0;     // the 4-byte "extra data" length field
    uint64_t nameStart = 0, nameEnd = 0;  // Pascal name field including its padding
    uint64_t dataStart = 0, dataEnd = 0;  // this layer's channel image data (all channels, contiguous)
    int64_t luniStart = -1, luniEnd = -1; // the 'luni' block, up to the next block (-1: none)

    std::string pascalName;      // raw bytes, legacy encoding
    std::u16string unicodeName;  // from 'luni'
    bool hasLuni = false;

    bool hidden() const { return (flags & 2) != 0; }
    bool clipped() const { return clipping != 0; }
    int64_t width() const { return int64_t(right) - left; }
    int64_t height() const { return int64_t(bottom) - top; }
    const ChannelInfo* channel(int16_t id) const {
        for (const ChannelInfo& c : channels)
            if (c.id == id) return &c;
        return nullptr;
    }
};

struct ImageResource {
    uint16_t id = 0;
    uint64_t offset = 0;  // data
    uint32_t size = 0;
};

class PsdFile {
public:
    bool open(const std::wstring& path, std::wstring& error);

    // Name as shown to the user: 'luni' if present, else the Pascal name decoded with `codepage`.
    std::u16string displayName(size_t layer) const;

    std::wstring path;
    FileStamp stamp;
    uint64_t fileSize = 0;

    // Header.
    bool psb = false;
    uint16_t channels = 0;
    uint32_t width = 0, height = 0;
    uint16_t depth = 0;
    ColorMode mode = ColorMode::RGB;
    int hasRealMergedData = -1;  // from the version-info resource; -1: resource absent
    std::vector<ImageResource> resources;
    const ImageResource* resource(uint16_t id) const {
        for (const ImageResource& r : resources)
            if (r.id == id) return &r;
        return nullptr;
    }

    // Layer and mask information section.
    uint64_t lmLenOffset = 0;  // its length field (4 bytes, 8 in PSB)
    uint64_t lmLen = 0;
    // The length field of the block that holds the layer records: the "layer info" length, or
    // for 16/32-bit documents the 'Lr16'/'Lr32' tagged block's length.
    uint64_t containerLenOffset = 0;
    uint64_t containerLen = 0;
    bool layersInTaggedBlock = false;
    bool hasLayerInfo = false;   // false: no layer records anywhere (flat file)
    int16_t layerCountRaw = 0;   // negative: first alpha channel is the merged transparency
    uint64_t recordsStart = 0;   // first record (after the layer count)
    uint64_t recordsEnd = 0;     // start of the channel image data
    uint64_t channelDataEnd = 0; // end of the channel image data
    std::vector<LayerRecord> layers;  // file order: bottom-most first

    uint64_t imageDataOffset = 0;  // composite: its compression field

    int codepage = 0;  // guessed encoding of the Pascal names

    int lenSize() const { return psb ? 8 : 4; }
};

// Tagged-block keys whose length field is 8 bytes in PSB files.
bool isWideKey(const char key[4]);

}  // namespace psd
