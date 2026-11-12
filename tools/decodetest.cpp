// decodetest <file> <record|-1> <out.pam>
// Decodes a layer (or the composite, -1) with the editor's GPU decoder and writes the result,
// premultiplied RGBA8, as a PAM file, for comparison against a reference decoder.
#include "gpu/LayerDecoder.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int fail(const std::wstring& e) {
    std::fwprintf(stderr, L"error: %ls\n", e.c_str());
    return 1;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 4) {
        std::printf("usage: decodetest <file> <record|-1> <out.pam>\n");
        return 2;
    }
    std::wstring e;
    psd::PsdFile f;
    if (!f.open(argv[1], e)) return fail(e);
    if (!gpu::Device::get().init(e)) return fail(e);
    const int record = std::stoi(argv[2]);
    if (record >= int(f.layers.size())) return fail(L"no such layer");

    gpu::DecodeSource src;
    switch (gpu::makeSource(f, record, src, e)) {
        case gpu::SourceStatus::Empty: std::printf("empty\n"); return 3;
        case gpu::SourceStatus::Unsupported: return fail(e);
        case gpu::SourceStatus::Ok: break;
    }
    std::atomic<bool> cancel{false};
    gpu::DecodeResult r;
    gpu::decode(src, cancel, r);
    if (!r.error.empty()) return fail(r.error);
    if (!r.warning.empty()) std::fwprintf(stderr, L"warning: %ls\n", r.warning.c_str());
    const gpu::Image& img = *r.image;

    gpu::Device& g = gpu::Device::get();
    gpu::Buffer rb;
    const uint64_t bytes = uint64_t(img.pitch) * img.height;
    if (!g.createBuffer(bytes, D3D12_HEAP_TYPE_READBACK, false, rb, e)) return fail(e);
    gpu::CommandContext ctx;
    if (!ctx.init(e)) return fail(e);
    ID3D12GraphicsCommandList* cl = ctx.begin();
    gpu::transition(cl, img.rgba.res.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cl->CopyBufferRegion(rb.res.Get(), 0, img.rgba.res.Get(), 0, bytes);
    if (!ctx.submitAndWait(e)) return fail(e);
    const uint8_t* data = nullptr;
    const D3D12_RANGE range{0, size_t(bytes)};
    if (FAILED(rb.res->Map(0, &range, reinterpret_cast<void**>(const_cast<uint8_t**>(&data))))) return fail(L"map");

    FILE* out = nullptr;
    if (_wfopen_s(&out, argv[3], L"wb") != 0 || !out) return fail(L"cannot write output");
    std::fprintf(out, "P7\nWIDTH %u\nHEIGHT %u\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n", img.width, img.height);
    for (uint32_t y = 0; y < img.height; ++y) std::fwrite(data + uint64_t(y) * img.pitch, 4, img.width, out);
    std::fclose(out);
    std::printf("ok %ux%u\n", img.width, img.height);
    return 0;
}
