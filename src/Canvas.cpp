#include "Canvas.h"

#include <QEvent>
#include <QResizeEvent>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace {
constexpr uint32_t kBackground = 0xFF303030;  // 0xAABBGGRR
constexpr uint32_t kCheckerLight = 0xFFFFFFFF;
constexpr uint32_t kCheckerDark = 0xFFCCCCCC;
}  // namespace

Canvas::Canvas(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_NativeWindow);
    setAttribute(Qt::WA_PaintOnScreen);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAutoFillBackground(false);
    setFocusPolicy(Qt::NoFocus);
    setMinimumSize(64, 64);
}

Canvas::~Canvas() {
    if (ctxReady_) ctx_.flush();  // queued presents reference the back buffers
}

void Canvas::report(const std::wstring& error) {
    broken_ = true;
    emit failed(QString::fromStdWString(error));
}

bool Canvas::ensureSwapChain() {
    if (swap_) return true;
    gpu::Device& g = gpu::Device::get();
    if (broken_ || !g.ok()) return false;
    const HWND hwnd = reinterpret_cast<HWND>(winId());
    RECT rc{};
    GetClientRect(hwnd, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return false;
    std::wstring error;
    if (!ctxReady_) {
        if (!ctx_.init(error)) {
            report(error);
            return false;
        }
        ctxReady_ = true;
    }
    DXGI_SWAP_CHAIN_DESC1 d{};
    d.Width = UINT(rc.right);
    d.Height = UINT(rc.bottom);
    d.Format = gpu::Device::kBackBufferFormat;
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = kBuffers;
    d.Scaling = DXGI_SCALING_STRETCH;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    gpu::ComPtr<IDXGISwapChain1> sc;
    HRESULT hr = g.factory()->CreateSwapChainForHwnd(g.queue(), hwnd, &d, nullptr, nullptr, &sc);
    if (SUCCEEDED(hr)) hr = sc.As(&swap_);
    if (SUCCEEDED(hr)) g.factory()->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
    if (SUCCEEDED(hr)) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = kBuffers;
        hr = g.device()->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap_));
    }
    if (FAILED(hr)) {
        swap_.Reset();
        report(L"Cannot create the D3D12 swap chain: " + gpu::hrText(hr));
        return false;
    }
    rtvSize_ = g.device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    width_ = d.Width;
    height_ = d.Height;
    getBuffers();
    return true;
}

void Canvas::getBuffers() {
    D3D12_CPU_DESCRIPTOR_HANDLE h = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < kBuffers; ++i) {
        swap_->GetBuffer(i, IID_PPV_ARGS(&back_[i]));
        gpu::Device::get().device()->CreateRenderTargetView(back_[i].Get(), nullptr, h);
        h.ptr += rtvSize_;
    }
}

bool Canvas::resizeSwapChain() {
    RECT rc{};
    GetClientRect(reinterpret_cast<HWND>(winId()), &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return false;
    if (UINT(rc.right) == width_ && UINT(rc.bottom) == height_) return true;
    ctx_.flush();  // presents still queued reference the back buffers
    for (auto& b : back_) b.Reset();
    const HRESULT hr = swap_->ResizeBuffers(kBuffers, UINT(rc.right), UINT(rc.bottom), DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
        report(L"Cannot resize the D3D12 swap chain: " + gpu::hrText(hr));
        return false;
    }
    width_ = UINT(rc.right);
    height_ = UINT(rc.bottom);
    getBuffers();
    return true;
}

void Canvas::setFit(Fit fit) {
    if (fit_ == fit) return;
    fit_ = fit;
    redraw();
}

void Canvas::scrollTo(int value) {
    if (!display_ || display_->scroll == value) return;
    display_->scroll = value;
    redraw();
}

void Canvas::redraw() {
    if (!isVisible() || !ensureSwapChain() || !resizeSwapChain()) return;
    gpu::Device& g = gpu::Device::get();
    std::wstring error;
    ID3D12GraphicsCommandList* cl = ctx_.begin();

    const gpu::Image* img = display_ && display_->image ? display_->image.get() : nullptr;
    int x = 0, y = 0;
    uint32_t dw = 0, dh = 0;
    int scrollMax = 0;
    if (img) {
        // Fitted size: the whole image, or the canvas width (scrolling vertically).
        if (fit_ == Fit::Whole) {
            const double s = std::min(double(width_) / img->width, double(height_) / img->height);
            dw = std::max(1u, uint32_t(std::lround(img->width * s)));
            dh = std::max(1u, uint32_t(std::lround(img->height * s)));
        } else {
            dw = width_;
            dh = std::max(1u, uint32_t(std::lround(double(img->height) * width_ / img->width)));
        }
        if (!display_->fitted || display_->fittedW != dw || display_->fittedH != dh) {
            display_->fitted.reset();
            if (!g.createBuffer(uint64_t(dw) * dh * 4, D3D12_HEAP_TYPE_DEFAULT, true, display_->fitted, error)) {
                emit failed(QString::fromStdWString(error));
                display_->release();
                img = nullptr;
            } else {
                display_->fittedW = dw;
                display_->fittedH = dh;
                g.recordResize(cl, *img, display_->fitted, dw, dh);
            }
        } else {
            gpu::transition(cl, display_->fitted.res.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
    }
    if (img) {
        x = (int(width_) - int(dw)) / 2;
        if (dh <= height_) {
            y = (int(height_) - int(dh)) / 2;
            display_->scroll = 0;
        } else {
            scrollMax = int(dh - height_);
            display_->scroll = std::clamp(display_->scroll, 0, scrollMax);
            y = -display_->scroll;
        }
    }

    const UINT index = swap_->GetCurrentBackBufferIndex();
    ID3D12Resource* back = back_[index].Get();
    gpu::transition(cl, back, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += SIZE_T(index) * rtvSize_;
    cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    const D3D12_VIEWPORT vp{0.0f, 0.0f, float(width_), float(height_), 0.0f, 1.0f};
    const D3D12_RECT scissor{0, 0, LONG(width_), LONG(height_)};
    cl->RSSetViewports(1, &vp);
    cl->RSSetScissorRects(1, &scissor);
    cl->SetGraphicsRootSignature(g.presentRS());
    cl->SetPipelineState(g.presentPso());
    const uint32_t checker = std::max(4u, uint32_t(std::lround(8 * devicePixelRatioF())));
    const uint32_t c[10] = {uint32_t(x), uint32_t(y), img ? dw : 0, img ? dh : 0, dw * 4, checker,
                            kBackground, kCheckerLight, kCheckerDark, 0};
    cl->SetGraphicsRoot32BitConstants(0, 10, c, 0);
    cl->SetGraphicsRootShaderResourceView(1, img ? display_->fitted.va() : g.dummy().va());
    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cl->DrawInstanced(3, 1, 0, 0);
    gpu::transition(cl, back, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    if (!ctx_.submitAndWait(error)) {
        report(error);
        return;
    }
    const HRESULT hr = swap_->Present(1, 0);
    if (FAILED(hr)) {
        report(L"Present failed: " + gpu::hrText(hr));
        return;
    }
    emit scrollChanged(fit_ == Fit::Width ? scrollMax : 0, int(height_), img ? display_->scroll : 0);
}

void Canvas::paintEvent(QPaintEvent*) {
    // Only when Windows asks (first show, restore from a lost surface): the fitted image is
    // cached, so this is a single draw call.
    redraw();
}

void Canvas::resizeEvent(QResizeEvent*) { redraw(); }

void Canvas::showEvent(QShowEvent*) { redraw(); }

void Canvas::wheelEvent(QWheelEvent* e) {
    if (fit_ != Fit::Width || !display_ || !display_->image) return;
    const int step = std::max(int(height_) / 8, int(48 * devicePixelRatioF()));
    display_->scroll -= e->angleDelta().y() * step / 120;
    redraw();
    e->accept();
}

bool Canvas::event(QEvent* e) {
    switch (e->type()) {
        case QEvent::WinIdChange:  // the native window was recreated: start over
            if (ctxReady_) ctx_.flush();
            for (auto& b : back_) b.Reset();
            swap_.Reset();
            rtvHeap_.Reset();
            width_ = height_ = 0;
            break;
        case QEvent::DevicePixelRatioChange:
            redraw();
            break;
        default:
            break;
    }
    return QWidget::event(e);
}
