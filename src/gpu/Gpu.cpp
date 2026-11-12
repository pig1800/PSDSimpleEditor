#include "Gpu.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <format>

#include "kClearCs.h"
#include "kMergeCs.h"
#include "kPresentPs.h"
#include "kPresentVs.h"
#include "kResize12Cs.h"
#include "kResize16Cs.h"
#include "kResize24Cs.h"
#include "kResize8Cs.h"
#include "kResizeCs.h"
#include "kUnpackCs.h"

namespace gpu {

std::wstring hrText(HRESULT hr) { return std::format(L"HRESULT 0x{:08X}", uint32_t(hr)); }

void transition(ID3D12GraphicsCommandList* cl, ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    cl->ResourceBarrier(1, &b);
}

void uavBarrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* r) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = r;
    cl->ResourceBarrier(1, &b);
}

// ---- CommandContext --------------------------------------------------------------------------

CommandContext::~CommandContext() {
    if (event_) CloseHandle(event_);
}

bool CommandContext::init(std::wstring& error) {
    ID3D12Device* dev = Device::get().device();
    HRESULT hr = dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc_));
    if (SUCCEEDED(hr))
        hr = dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc_.Get(), nullptr, IID_PPV_ARGS(&list_));
    if (SUCCEEDED(hr)) hr = list_->Close();
    if (SUCCEEDED(hr)) hr = dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
    if (SUCCEEDED(hr)) {
        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event_) hr = HRESULT_FROM_WIN32(GetLastError());
    }
    if (FAILED(hr)) {
        error = L"D3D12 command context: " + hrText(hr);
        return false;
    }
    return true;
}

ID3D12GraphicsCommandList* CommandContext::begin() {
    alloc_->Reset();
    list_->Reset(alloc_.Get(), nullptr);
    return list_.Get();
}

bool CommandContext::submitAndWait(std::wstring& error) {
    HRESULT hr = list_->Close();
    if (FAILED(hr)) {
        error = L"D3D12 command list: " + hrText(hr);
        return false;
    }
    ID3D12CommandList* lists[] = {list_.Get()};
    ID3D12CommandQueue* q = Device::get().queue();
    q->ExecuteCommandLists(1, lists);
    hr = q->Signal(fence_.Get(), ++value_);
    if (SUCCEEDED(hr) && fence_->GetCompletedValue() < value_) {
        hr = fence_->SetEventOnCompletion(value_, event_);
        if (SUCCEEDED(hr)) WaitForSingleObject(event_, INFINITE);
    }
    if (SUCCEEDED(hr)) hr = Device::get().device()->GetDeviceRemovedReason();
    if (FAILED(hr)) {
        error = L"D3D12 execution failed: " + hrText(hr);
        return false;
    }
    return true;
}

void CommandContext::flush() {
    if (!fence_ || FAILED(Device::get().queue()->Signal(fence_.Get(), ++value_))) return;
    if (fence_->GetCompletedValue() < value_ && SUCCEEDED(fence_->SetEventOnCompletion(value_, event_)))
        WaitForSingleObject(event_, INFINITE);
}

// ---- Device ----------------------------------------------------------------------------------

Device& Device::get() {
    static Device d;
    return d;
}

namespace {

HRESULT computePso(ID3D12Device* dev, ID3D12RootSignature* rs, const unsigned char* code, size_t size,
                   ComPtr<ID3D12PipelineState>& out) {
    D3D12_COMPUTE_PIPELINE_STATE_DESC d{};
    d.pRootSignature = rs;
    d.CS = {code, size};
    return dev->CreateComputePipelineState(&d, IID_PPV_ARGS(&out));
}

bool envSet(const wchar_t* name) { return GetEnvironmentVariableW(name, nullptr, 0) > 0; }

// PSE_D3D12_DEBUG=1: debug layer messages on stderr (needs Windows' Graphics Tools feature).
void CALLBACK onDebugMessage(D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity, D3D12_MESSAGE_ID, LPCSTR text, void*) {
    const char* level = severity <= D3D12_MESSAGE_SEVERITY_ERROR ? "ERROR" : severity == D3D12_MESSAGE_SEVERITY_WARNING ? "WARNING" : "INFO";
    std::fprintf(stderr, "[D3D12 %s] %s\n", level, text);
    std::fflush(stderr);
}

}  // namespace

bool Device::init(std::wstring& error) {
    const bool debugLayer = envSet(L"PSE_D3D12_DEBUG");
    if (debugLayer) {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
    }
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory_));
    if (FAILED(hr)) {
        error = L"DXGI: " + hrText(hr);
        return false;
    }
    // The fastest hardware adapter, else WARP.
    ComPtr<IDXGIFactory6> f6;
    ComPtr<IDXGIAdapter1> adapter;
    if (!envSet(L"PSE_WARP") && SUCCEEDED(factory_.As(&f6))) {
        for (UINT i = 0; f6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) !=
                         DXGI_ERROR_NOT_FOUND;
             ++i) {
            DXGI_ADAPTER_DESC1 d{};
            adapter->GetDesc1(&d);
            if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
                SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_)))) {
                description_ = d.Description;
                break;
            }
            adapter.Reset();
        }
    }
    if (!device_) {
        ComPtr<IDXGIAdapter> warp;
        hr = factory_->EnumWarpAdapter(IID_PPV_ARGS(&warp));
        if (SUCCEEDED(hr)) hr = D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_));
        if (FAILED(hr)) {
            error = L"No D3D12 device: " + hrText(hr);
            return false;
        }
        description_ = L"WARP (software)";
    }
    if (debugLayer) {
        ComPtr<ID3D12InfoQueue1> q;
        DWORD cookie = 0;
        if (SUCCEEDED(device_.As(&q))) q->RegisterMessageCallback(onDebugMessage, D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &cookie);
    }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = device_->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue_));
    ID3D12Device* dev = device_.Get();
    if (SUCCEEDED(hr)) hr = dev->CreateRootSignature(0, kClearCs, sizeof kClearCs, IID_PPV_ARGS(&computeRS_));
    if (SUCCEEDED(hr)) hr = computePso(dev, computeRS_.Get(), kClearCs, sizeof kClearCs, clear_);
    if (SUCCEEDED(hr)) hr = computePso(dev, computeRS_.Get(), kUnpackCs, sizeof kUnpackCs, unpack_);
    if (SUCCEEDED(hr)) hr = computePso(dev, computeRS_.Get(), kMergeCs, sizeof kMergeCs, merge_);
    if (SUCCEEDED(hr)) hr = computePso(dev, computeRS_.Get(), kResize8Cs, sizeof kResize8Cs, resize_[0]);
    if (SUCCEEDED(hr)) hr = computePso(dev, computeRS_.Get(), kResize12Cs, sizeof kResize12Cs, resize_[1]);
    if (SUCCEEDED(hr)) hr = computePso(dev, computeRS_.Get(), kResize16Cs, sizeof kResize16Cs, resize_[2]);
    if (SUCCEEDED(hr)) hr = computePso(dev, computeRS_.Get(), kResize24Cs, sizeof kResize24Cs, resize_[3]);
    if (SUCCEEDED(hr)) hr = computePso(dev, computeRS_.Get(), kResizeCs, sizeof kResizeCs, resize_[4]);
    if (SUCCEEDED(hr)) hr = dev->CreateRootSignature(0, kPresentVs, sizeof kPresentVs, IID_PPV_ARGS(&presentRS_));
    if (SUCCEEDED(hr)) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
        d.pRootSignature = presentRS_.Get();
        d.VS = {kPresentVs, sizeof kPresentVs};
        d.PS = {kPresentPs, sizeof kPresentPs};
        d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        d.SampleMask = UINT_MAX;
        d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        d.RasterizerState.DepthClipEnable = TRUE;
        d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        d.NumRenderTargets = 1;
        d.RTVFormats[0] = kBackBufferFormat;
        d.SampleDesc.Count = 1;
        hr = dev->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&present_));
    }
    if (FAILED(hr)) {
        error = L"D3D12 initialisation failed: " + hrText(hr);
        device_.Reset();
        return false;
    }
    if (!createBuffer(256, D3D12_HEAP_TYPE_DEFAULT, true, dummy_, error)) {
        device_.Reset();
        return false;
    }
    return true;
}

bool Device::createBuffer(uint64_t size, D3D12_HEAP_TYPE heap, bool uav, Buffer& out, std::wstring& error) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = heap;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = size < 256 ? 256 : size;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    const D3D12_RESOURCE_STATES state = heap == D3D12_HEAP_TYPE_UPLOAD     ? D3D12_RESOURCE_STATE_GENERIC_READ
                                        : heap == D3D12_HEAP_TYPE_READBACK ? D3D12_RESOURCE_STATE_COPY_DEST
                                                                           : D3D12_RESOURCE_STATE_COMMON;
    out.reset();
    const HRESULT hr = device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&out.res));
    if (FAILED(hr)) {
        error = std::format(L"Cannot allocate {} MB of GPU memory ({})", (size + (1 << 20) - 1) >> 20, hrText(hr));
        return false;
    }
    out.size = size;
    return true;
}

void Device::recordResize(ID3D12GraphicsCommandList* cl, const Image& src, const Buffer& dst, uint32_t dw, uint32_t dh) const {
    transition(cl, src.rgba.res.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(cl, dst.res.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    // PsdPreview's filters: sharp Mitchell-Netravali (fitted to Photoshop's Bicubic Sharper) when
    // shrinking, Catmull-Rom when enlarging.
    const bool shrink = dw < src.width;
    const float B = shrink ? 0.3f : 0.0f, C = shrink ? 2.2f : 0.5f;
    uint32_t c[8] = {src.width, src.height, src.pitch, dw, dh, dw * 4, 0, 0};
    std::memcpy(&c[6], &B, 4);
    std::memcpy(&c[7], &C, 4);
    // The fixed-tap variant that covers the horizontal reduction (else the generic one).
    const float fx = std::max(float(src.width) / float(dw), 1.0f);
    const int need = int(4.0f * fx) + 2;
    const int variant = need <= 8 ? 0 : need <= 12 ? 1 : need <= 16 ? 2 : need <= 24 ? 3 : 4;
    cl->SetComputeRootSignature(computeRS_.Get());
    cl->SetPipelineState(resize_[variant].Get());
    cl->SetComputeRoot32BitConstants(kConstants, 8, c, 0);
    cl->SetComputeRootShaderResourceView(kT0, src.rgba.va());
    cl->SetComputeRootShaderResourceView(kT1, src.rgba.va());
    cl->SetComputeRootUnorderedAccessView(kU0, dst.va());
    cl->SetComputeRootUnorderedAccessView(kU1, dummy_.va());
    cl->Dispatch((dw + 31) / 32, (dh + 7) / 8, 1);
    transition(cl, dst.res.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

}  // namespace gpu
