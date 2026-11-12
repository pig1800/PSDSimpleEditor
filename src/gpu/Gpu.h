// D3D12 device, kernels and the per-document display state.
// One device and one direct queue for the whole app. The queue and device are free-threaded;
// each thread that submits work uses its own CommandContext (allocator, list, fence) and waits
// for it, so nothing is ever released while the GPU still uses it.
// Buffers are created in COMMON and decay back to it after every submission, so every command
// list starts from COMMON and transitions explicitly.
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>
#include <string>

namespace gpu {

using Microsoft::WRL::ComPtr;

struct Buffer {
    ComPtr<ID3D12Resource> res;
    uint64_t size = 0;
    D3D12_GPU_VIRTUAL_ADDRESS va() const { return res ? res->GetGPUVirtualAddress() : 0; }
    explicit operator bool() const { return res != nullptr; }
    void reset() {
        res.Reset();
        size = 0;
    }
};

// A decoded image: premultiplied RGBA8, `pitch` bytes per row.
struct Image {
    Buffer rgba;
    uint32_t width = 0, height = 0, pitch = 0;
};

// What a document shows: the decoded image of its current layer, and that image fitted to the
// canvas (rebuilt when the canvas size or fit mode changes).
struct DisplayState {
    std::shared_ptr<Image> image;
    Buffer fitted;
    uint32_t fittedW = 0, fittedH = 0;
    int scroll = 0;  // fit-width mode: first visible row of the fitted image (physical pixels)
    void release() {
        image.reset();
        fitted.reset();
        fittedW = fittedH = 0;
        scroll = 0;
    }
};

class CommandContext {
public:
    CommandContext() = default;
    ~CommandContext();
    CommandContext(const CommandContext&) = delete;
    CommandContext& operator=(const CommandContext&) = delete;

    bool init(std::wstring& error);
    ID3D12GraphicsCommandList* begin();         // reset allocator and list
    bool submitAndWait(std::wstring& error);    // close, execute, wait for the GPU
    void flush();                               // wait for everything queued so far (e.g. presents)

private:
    ComPtr<ID3D12CommandAllocator> alloc_;
    ComPtr<ID3D12GraphicsCommandList> list_;
    ComPtr<ID3D12Fence> fence_;
    uint64_t value_ = 0;
    HANDLE event_ = nullptr;
};

void transition(ID3D12GraphicsCommandList* cl, ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to);
void uavBarrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* r);
std::wstring hrText(HRESULT hr);

// Root parameter indices of the compute root signature (decode.hlsl PP_RS).
enum ComputeRoot : UINT { kConstants = 0, kT0 = 1, kT1 = 2, kU0 = 3, kU1 = 4 };

class Device {
public:
    static Device& get();

    bool init(std::wstring& error);  // once, at startup
    bool ok() const { return device_ != nullptr; }
    const std::wstring& description() const { return description_; }

    ID3D12Device* device() const { return device_.Get(); }
    ID3D12CommandQueue* queue() const { return queue_.Get(); }
    IDXGIFactory4* factory() const { return factory_.Get(); }

    bool createBuffer(uint64_t size, D3D12_HEAP_TYPE heap, bool uav, Buffer& out, std::wstring& error);

    // Kernels (decode.hlsl / present.hlsl).
    ID3D12RootSignature* computeRS() const { return computeRS_.Get(); }
    ID3D12PipelineState* clearPso() const { return clear_.Get(); }
    ID3D12PipelineState* unpackPso() const { return unpack_.Get(); }
    ID3D12PipelineState* mergePso() const { return merge_.Get(); }
    ID3D12RootSignature* presentRS() const { return presentRS_.Get(); }
    ID3D12PipelineState* presentPso() const { return present_.Get(); }
    const Buffer& dummy() const { return dummy_; }  // binds root descriptors a kernel doesn't use

    // Records `src` resized to dw x dh into `dst` (rows dw * 4 bytes). Leaves `dst` in
    // PIXEL_SHADER_RESOURCE.
    void recordResize(ID3D12GraphicsCommandList* cl, const Image& src, const Buffer& dst, uint32_t dw, uint32_t dh) const;

    static constexpr DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

private:
    ComPtr<IDXGIFactory4> factory_;
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<ID3D12RootSignature> computeRS_, presentRS_;
    ComPtr<ID3D12PipelineState> clear_, unpack_, merge_, resize_[5], present_;  // resize: 8/12/16/24 taps, any
    Buffer dummy_;
    std::wstring description_;
};

}  // namespace gpu
