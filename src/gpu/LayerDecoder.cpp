#include "LayerDecoder.h"

#include <zlib.h>

#include <algorithm>
#include <cstring>
#include <format>

namespace gpu {

namespace {

constexpr uint64_t kReadChunk = 16ull << 20;  // file reads between cancel checks
constexpr UINT kGroup = 64;                   // GROUP in pp_unpack / pp_merge
constexpr UINT kMaxGroupsX = 32768;

uint64_t alignUp(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

// How one plane's bytes get into the upload buffer.
struct PlanePlan {
    enum Kind { Rle, Raw, Zip, Memory, Zero } kind = Zero;
    uint64_t fileOffset = 0;      // Rle / Raw / Zip: source bytes in the file
    const uint8_t* memory = nullptr;  // Memory: rows already decoded on the CPU
    uint64_t fileBytes = 0;
    bool predicted = false;       // Zip with prediction
    std::vector<uint32_t> counts; // Rle: bytes per row
    uint64_t region = 0;          // offset in the upload buffer
    uint64_t regionBytes = 0;
};

struct Fail {
    std::wstring msg;
};

// Inflates zlib data until `outBytes` are produced. Consecutive streams are concatenated: a ZIP
// composite may be one stream for all channels or (ImageMagick) one per channel.
void inflateAll(const std::vector<uint8_t>& in, uint8_t* out, size_t outBytes) {
    size_t inPos = 0, outPos = 0;
    while (outPos < outBytes && inPos < in.size()) {
        z_stream z{};
        if (inflateInit(&z) != Z_OK) throw Fail{L"zlib init failed"};
        z.next_in = const_cast<uint8_t*>(in.data() + inPos);
        z.avail_in = uInt(in.size() - inPos);
        z.next_out = out + outPos;
        z.avail_out = uInt(outBytes - outPos);
        const int rc = inflate(&z, Z_FINISH);
        inPos = in.size() - z.avail_in;
        outPos = outBytes - z.avail_out;
        inflateEnd(&z);
        if (rc != Z_STREAM_END && outPos < outBytes) break;
    }
    if (outPos < outBytes) throw Fail{L"Corrupt ZIP-compressed data"};
}

// Undoes ZIP-with-prediction's per-row delta encoding (8 or 16-bit big-endian samples).
void unpredict(uint8_t* data, uint64_t rows, uint64_t width, uint32_t bytesPerSample) {
    for (uint64_t y = 0; y < rows; ++y) {
        uint8_t* row = data + y * width * bytesPerSample;
        if (bytesPerSample == 1) {
            for (uint64_t x = 1; x < width; ++x) row[x] = uint8_t(row[x] + row[x - 1]);
        } else {
            for (uint64_t x = 1; x < width; ++x) {
                const uint16_t prev = uint16_t(row[2 * x - 2] << 8 | row[2 * x - 1]);
                const uint16_t v = uint16_t((row[2 * x] << 8 | row[2 * x + 1]) + prev);
                row[2 * x] = uint8_t(v >> 8);
                row[2 * x + 1] = uint8_t(v);
            }
        }
    }
}

class Job {
public:
    Job(const DecodeSource& s, const std::atomic<bool>& cancel) : s_(s), cancel_(cancel) {}

    void run(DecodeResult& out) {
        try {
            if (!file_.openRead(s_.path, err_)) throw Fail{err_};
            psd::FileStamp now;
            if (!psd::getFileStamp(s_.path, now) || now != s_.stamp)
                throw Fail{L"The file was changed on disk; reopen it."};
            rowBytes_ = uint64_t(s_.width) * s_.bytesPerSample;
            if (s_.composite) planComposite();
            else planLayer();
            if (canceled()) return cancel(out);
            fillUpload();
            if (canceled()) return cancel(out);
            gpuDecode(out);
        } catch (const Fail& f) {
            out.error = f.msg;
        }
    }

private:
    bool canceled() const { return cancel_.load(std::memory_order_relaxed); }
    void cancel(DecodeResult& out) { out.canceled = true; }

    uint16_t readU16(uint64_t at) {
        uint8_t b[2];
        if (!file_.readAt(at, b, 2)) throw Fail{L"Read error (truncated file?)"};
        return uint16_t(b[0] << 8 | b[1]);
    }

    // RLE row byte counts: `rows` entries of 2 bytes (PSD) or 4 (PSB) at `at`.
    std::vector<uint32_t> readCounts(uint64_t at, uint64_t rows) {
        const int size = s_.psb ? 4 : 2;
        std::vector<uint8_t> raw(size_t(rows * size));
        if (!file_.readAt(at, raw.data(), raw.size())) throw Fail{L"Read error in RLE row table (truncated file?)"};
        std::vector<uint32_t> counts(static_cast<size_t>(rows));
        for (size_t i = 0; i < counts.size(); ++i) {
            const uint8_t* p = raw.data() + i * size;
            counts[i] = size == 2 ? uint32_t(p[0] << 8 | p[1]) : uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
        }
        return counts;
    }

    void planComposite() {
        const uint16_t compression = readU16(s_.compositeOffset);
        const uint64_t h = s_.height;
        const uint64_t data = s_.compositeOffset + 2;
        if (compression == 1) {
            const uint64_t tableBytes = uint64_t(s_.fileChannels) * h * (s_.psb ? 4 : 2);
            const std::vector<uint32_t> all = readCounts(data, uint64_t(s_.fileChannels) * h);
            std::vector<uint64_t> channelStart(s_.fileChannels + 1);
            channelStart[0] = data + tableBytes;
            for (uint32_t c = 0; c < s_.fileChannels; ++c) {
                uint64_t sum = 0;
                for (uint64_t y = 0; y < h; ++y) sum += all[size_t(c * h + y)];
                channelStart[c + 1] = channelStart[c] + sum;
            }
            for (uint32_t c : s_.compositeChannels) {
                PlanePlan p;
                p.kind = PlanePlan::Rle;
                p.counts.assign(all.begin() + ptrdiff_t(c * h), all.begin() + ptrdiff_t((c + 1) * h));
                p.fileOffset = channelStart[c];
                p.fileBytes = channelStart[c + 1] - channelStart[c];
                plans_.push_back(std::move(p));
            }
        } else if (compression == 0) {
            for (uint32_t c : s_.compositeChannels) {
                PlanePlan p;
                p.kind = PlanePlan::Raw;
                p.fileOffset = data + uint64_t(c) * h * rowBytes_;
                p.fileBytes = h * rowBytes_;
                plans_.push_back(std::move(p));
            }
        } else if (compression == 2 || compression == 3) {
            const uint64_t plane = h * rowBytes_;
            std::vector<uint8_t> in(size_t(file_.size() - data));
            if (!file_.readAt(data, in.data(), in.size())) throw Fail{L"Read error (truncated file?)"};
            inflated_.resize(size_t(plane * s_.fileChannels));
            inflateAll(in, inflated_.data(), inflated_.size());
            if (compression == 3) unpredict(inflated_.data(), h * s_.fileChannels, s_.width, s_.bytesPerSample);
            for (uint32_t c : s_.compositeChannels) {
                PlanePlan p;
                p.kind = PlanePlan::Memory;
                p.memory = inflated_.data() + c * plane;
                plans_.push_back(std::move(p));
            }
        } else {
            throw Fail{std::format(L"Unknown composite compression {}", compression)};
        }
    }

    void planLayer() {
        const uint64_t h = s_.height;
        for (const psd::ChannelInfo& ch : s_.layerChannels) {
            PlanePlan p;
            if (ch.length < 2) {  // no data at all: an empty plane
                plans_.push_back(std::move(p));
                continue;
            }
            const uint16_t compression = readU16(ch.dataOffset);
            const uint64_t data = ch.dataOffset + 2, avail = ch.length - 2;
            switch (compression) {
                case 0:
                    p.kind = PlanePlan::Raw;
                    p.fileOffset = data;
                    p.fileBytes = h * rowBytes_;
                    if (p.fileBytes > avail) throw Fail{L"Layer channel data is truncated"};
                    break;
                case 1: {
                    const uint64_t table = h * (s_.psb ? 4 : 2);
                    if (table > avail) throw Fail{L"Layer channel RLE table is truncated"};
                    p.kind = PlanePlan::Rle;
                    p.counts = readCounts(data, h);
                    p.fileOffset = data + table;
                    p.fileBytes = 0;
                    for (uint32_t c : p.counts) p.fileBytes += c;
                    if (p.fileBytes > avail - table) throw Fail{L"Layer channel RLE data is truncated"};
                    break;
                }
                case 2:
                case 3:
                    p.kind = PlanePlan::Zip;
                    p.predicted = compression == 3;
                    p.fileOffset = data;
                    p.fileBytes = avail;
                    break;
                default:
                    throw Fail{std::format(L"Unknown channel compression {}", compression)};
            }
            plans_.push_back(std::move(p));
        }
    }

    void fillUpload() {
        // Layout: each plane's bytes (16-aligned regions, 16 bytes of slack at the end), then the
        // row offset table (planes * height + 1 entries).
        uint64_t at = 0;
        for (PlanePlan& p : plans_) {
            p.region = at;
            p.regionBytes = p.kind == PlanePlan::Rle ? p.fileBytes : s_.height * rowBytes_;
            at = alignUp(at + p.regionBytes, 16);
        }
        compBytes_ = at + 16;
        offsetsAt_ = alignUp(compBytes_, 256);
        const uint64_t rows = uint64_t(plans_.size()) * s_.height;
        const uint64_t total = offsetsAt_ + 4 * (rows + 1);
        if (compBytes_ >= (1ull << 32)) throw Fail{L"Image too large for the GPU decoder"};

        Device& g = Device::get();
        if (!g.createBuffer(total, D3D12_HEAP_TYPE_UPLOAD, false, upload_, err_)) throw Fail{err_};
        uint8_t* base = nullptr;
        const D3D12_RANGE none{0, 0};
        HRESULT hr = upload_.res->Map(0, &none, reinterpret_cast<void**>(&base));
        if (FAILED(hr)) throw Fail{L"Map failed: " + hrText(hr)};
        struct Unmap {
            ID3D12Resource* r;
            ~Unmap() { r->Unmap(0, nullptr); }
        } unmap{upload_.res.Get()};

        for (PlanePlan& p : plans_) {
            uint8_t* dst = base + p.region;
            switch (p.kind) {
                case PlanePlan::Rle:
                case PlanePlan::Raw:
                    for (uint64_t done = 0; done < p.fileBytes; done += kReadChunk) {
                        if (canceled()) return;
                        const uint64_t n = std::min(kReadChunk, p.fileBytes - done);
                        if (!file_.readAt(p.fileOffset + done, dst + done, size_t(n)))
                            throw Fail{L"Read error (truncated file?)"};
                    }
                    break;
                case PlanePlan::Zip:
                    inflatePlane(p, dst);
                    break;
                case PlanePlan::Memory:
                    std::memcpy(dst, p.memory, size_t(p.regionBytes));
                    break;
                case PlanePlan::Zero:
                    std::memset(dst, 0, size_t(p.regionBytes));
                    break;
            }
        }
        // Row offsets.
        auto* offsets = reinterpret_cast<uint32_t*>(base + offsetsAt_);
        uint64_t r = 0;
        for (const PlanePlan& p : plans_) {
            uint64_t o = p.region;
            for (uint64_t y = 0; y < s_.height; ++y, ++r) {
                offsets[r] = uint32_t(o);
                o += p.kind == PlanePlan::Rle ? p.counts[size_t(y)] : rowBytes_;
            }
        }
        offsets[r] = uint32_t(compBytes_ - 16);
    }

    void inflatePlane(const PlanePlan& p, uint8_t* dst) {
        std::vector<uint8_t> in(size_t(p.fileBytes));
        if (!file_.readAt(p.fileOffset, in.data(), in.size())) throw Fail{L"Read error (truncated file?)"};
        std::vector<uint8_t> out(size_t(p.regionBytes));
        inflateAll(in, out.data(), out.size());
        if (p.predicted) unpredict(out.data(), s_.height, s_.width, s_.bytesPerSample);
        std::memcpy(dst, out.data(), out.size());  // the upload heap is write-combined: write once
    }

    void gpuDecode(DecodeResult& out) {
        Device& g = Device::get();
        const uint32_t w = s_.width, h = s_.height;
        const uint64_t pitch = alignUp(w, 16);
        const uint64_t planeStride = uint64_t(h) * pitch;
        const uint32_t quads = (w + 3) / 4;
        const uint64_t rgbaPitch = uint64_t(quads) * 16;
        const uint64_t planesBytes = planeStride * plans_.size();
        if (planesBytes >= (1ull << 32) || rgbaPitch * h >= (1ull << 32))
            throw Fail{L"Image too large for the GPU decoder"};

        Buffer comp, planes, status, readback;
        auto image = std::make_shared<Image>();
        if (!g.createBuffer(upload_.size, D3D12_HEAP_TYPE_DEFAULT, false, comp, err_) ||
            !g.createBuffer(planesBytes, D3D12_HEAP_TYPE_DEFAULT, true, planes, err_) ||
            !g.createBuffer(rgbaPitch * h, D3D12_HEAP_TYPE_DEFAULT, true, image->rgba, err_) ||
            !g.createBuffer(256, D3D12_HEAP_TYPE_DEFAULT, true, status, err_) ||
            !g.createBuffer(256, D3D12_HEAP_TYPE_READBACK, false, readback, err_))
            throw Fail{err_};

        CommandContext ctx;
        if (!ctx.init(err_)) throw Fail{err_};
        ID3D12GraphicsCommandList* cl = ctx.begin();
        transition(cl, comp.res.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        cl->CopyBufferRegion(comp.res.Get(), 0, upload_.res.Get(), 0, upload_.size);
        transition(cl, comp.res.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        transition(cl, planes.res.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        transition(cl, status.res.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        transition(cl, image->rgba.res.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        cl->SetComputeRootSignature(g.computeRS());
        cl->SetComputeRootShaderResourceView(kT0, comp.va());
        cl->SetComputeRootShaderResourceView(kT1, comp.va() + offsetsAt_);
        cl->SetComputeRootUnorderedAccessView(kU0, planes.va());
        cl->SetComputeRootUnorderedAccessView(kU1, status.va());
        cl->SetPipelineState(g.clearPso());
        cl->Dispatch(1, 1, 1);
        uavBarrier(cl, status.res.Get());

        cl->SetPipelineState(g.unpackPso());
        const UINT groups = (h + kGroup - 1) / kGroup;
        const UINT gx = std::min(groups, kMaxGroupsX), gy = (groups + gx - 1) / gx;
        for (size_t i = 0; i < plans_.size(); ++i) {
            const bool raw = plans_[i].kind != PlanePlan::Rle;
            const uint32_t c[8] = {h, uint32_t(rowBytes_), s_.bytesPerSample == 2 ? 1u : 0u, w, uint32_t(pitch), gx,
                                   uint32_t(i) * h, raw ? 1u : 0u};
            cl->SetComputeRoot32BitConstants(kConstants, 8, c, 0);
            cl->Dispatch(gx, gy, 1);
        }
        transition(cl, planes.res.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        const uint64_t threads = uint64_t(quads) * h;
        const UINT mgroups = UINT((threads + kGroup - 1) / kGroup);
        const UINT mx = std::min(mgroups, kMaxGroupsX), my = (mgroups + mx - 1) / mx;
        const uint32_t colorPlanes = s_.gray ? 1 : 3;
        const uint32_t m[9] = {w, h, uint32_t(pitch), uint32_t(planeStride), s_.gray ? 1u : 0u,
                               s_.hasAlpha ? colorPlanes : 0xFFFFFFFFu, uint32_t(rgbaPitch), quads, mx};
        cl->SetPipelineState(g.mergePso());
        cl->SetComputeRoot32BitConstants(kConstants, 9, m, 0);
        cl->SetComputeRootShaderResourceView(kT0, planes.va());
        cl->SetComputeRootShaderResourceView(kT1, planes.va());
        cl->SetComputeRootUnorderedAccessView(kU0, image->rgba.va());
        cl->SetComputeRootUnorderedAccessView(kU1, status.va());
        cl->Dispatch(mx, my, 1);

        transition(cl, status.res.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        cl->CopyBufferRegion(readback.res.Get(), 0, status.res.Get(), 0, 4);
        if (!ctx.submitAndWait(err_)) throw Fail{err_};

        uint32_t firstBad = 0xFFFFFFFFu;
        uint32_t* mapped = nullptr;
        const D3D12_RANGE range{0, 4}, none{0, 0};
        if (SUCCEEDED(readback.res->Map(0, &range, reinterpret_cast<void**>(&mapped)))) {
            firstBad = *mapped;
            readback.res->Unmap(0, &none);
        }
        if (firstBad != 0xFFFFFFFFu)
            out.warning = std::format(L"Corrupt pixel data from row {} of channel {} (shown as zeros)", firstBad % h,
                                      firstBad / h);
        image->width = w;
        image->height = h;
        image->pitch = uint32_t(rgbaPitch);
        out.image = std::move(image);
    }

    const DecodeSource& s_;
    const std::atomic<bool>& cancel_;
    psd::File file_;
    std::wstring err_;
    uint64_t rowBytes_ = 0;
    std::vector<PlanePlan> plans_;
    std::vector<uint8_t> inflated_;  // ZIP composite, all channels
    Buffer upload_;
    uint64_t compBytes_ = 0, offsetsAt_ = 0;
};

const wchar_t* modeName(psd::ColorMode m) {
    switch (m) {
        case psd::ColorMode::Bitmap: return L"Bitmap";
        case psd::ColorMode::Grayscale: return L"Grayscale";
        case psd::ColorMode::Indexed: return L"Indexed";
        case psd::ColorMode::RGB: return L"RGB";
        case psd::ColorMode::CMYK: return L"CMYK";
        case psd::ColorMode::Multichannel: return L"Multichannel";
        case psd::ColorMode::Duotone: return L"Duotone";
        case psd::ColorMode::Lab: return L"Lab";
    }
    return L"unknown-mode";
}

}  // namespace

SourceStatus makeSource(const psd::PsdFile& f, int record, DecodeSource& src, std::wstring& problem) {
    src = DecodeSource{};
    src.path = f.path;
    src.stamp = f.stamp;
    src.psb = f.psb;
    src.gray = f.mode == psd::ColorMode::Grayscale;
    src.bytesPerSample = f.depth / 8;
    const uint32_t colorPlanes = src.gray ? 1 : 3;
    if ((f.mode != psd::ColorMode::RGB && f.mode != psd::ColorMode::Grayscale) || (f.depth != 8 && f.depth != 16)) {
        problem = std::format(L"Display is not supported for {} {}-bit documents", modeName(f.mode), f.depth);
        return SourceStatus::Unsupported;
    }
    if (record < 0) {
        src.composite = true;
        src.width = f.width;
        src.height = f.height;
        src.compositeOffset = f.imageDataOffset;
        src.fileChannels = f.channels;
        for (uint32_t c = 0; c < colorPlanes; ++c) src.compositeChannels.push_back(c);
        // A negative layer count: the first extra channel is the merged transparency.
        if (f.layerCountRaw < 0 && f.channels > colorPlanes) {
            src.hasAlpha = true;
            src.compositeChannels.push_back(colorPlanes);
        }
        if (f.hasRealMergedData == 0) problem = L"This file has no flattened preview (saved with Maximize Compatibility off)";
        else if (f.imageDataOffset + 2 > f.fileSize) problem = L"This file has no flattened image data (truncated?)";
        else if (f.channels < colorPlanes) problem = L"Corrupt file: too few channels";
        return problem.empty() ? SourceStatus::Ok : SourceStatus::Unsupported;
    }
    const psd::LayerRecord& L = f.layers[size_t(record)];
    if (L.width() <= 0 || L.height() <= 0) return SourceStatus::Empty;
    src.width = uint32_t(L.width());
    src.height = uint32_t(L.height());
    for (uint32_t c = 0; c < colorPlanes; ++c) {
        const psd::ChannelInfo* ch = L.channel(int16_t(c));
        if (!ch) {
            problem = std::format(L"This layer has no colour channel {}", c);
            return SourceStatus::Unsupported;
        }
        src.layerChannels.push_back(*ch);
    }
    if (const psd::ChannelInfo* a = L.channel(-1)) {
        src.layerChannels.push_back(*a);
        src.hasAlpha = true;
    }
    return SourceStatus::Ok;
}

void decode(const DecodeSource& src, const std::atomic<bool>& cancel, DecodeResult& out) {
    out = DecodeResult{};
    if (!Device::get().ok()) {
        out.error = L"No GPU device";
        return;
    }
    if (src.width == 0 || src.height == 0) return;  // empty: nothing to show
    Job job(src, cancel);
    job.run(out);
}

}  // namespace gpu
