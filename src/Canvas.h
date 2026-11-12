// The view canvas: a native child window presented with a D3D12 flip-model swap chain. Qt never
// paints it. It is redrawn (on the GPU) only when its size, the shown image, the fit mode or the
// scroll position changes; window moves, overlaps and exposure keep the last presented frame.
#pragma once

#include "gpu/Gpu.h"

#include <QWidget>

class Canvas : public QWidget {
    Q_OBJECT
public:
    enum class Fit { Whole, Width };

    explicit Canvas(QWidget* parent = nullptr);
    ~Canvas() override;

    QPaintEngine* paintEngine() const override { return nullptr; }

    // What to show (not owned; null: background only). Doesn't redraw by itself.
    void setDisplay(gpu::DisplayState* display) { display_ = display; }
    void setFit(Fit fit);
    Fit fit() const { return fit_; }
    void scrollTo(int value);  // fit-width mode, physical pixels
    void redraw();

signals:
    // Fit-width scrolling range in physical pixels (maximum 0: nothing to scroll).
    void scrollChanged(int maximum, int pageStep, int value);
    void failed(const QString& message);

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void showEvent(QShowEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    bool event(QEvent*) override;

private:
    bool ensureSwapChain();
    bool resizeSwapChain();
    void getBuffers();
    void render(bool allowResize);
    void report(const std::wstring& error);

    static constexpr UINT kBuffers = 2;
    gpu::ComPtr<IDXGISwapChain3> swap_;
    gpu::ComPtr<ID3D12DescriptorHeap> rtvHeap_;
    gpu::ComPtr<ID3D12Resource> back_[kBuffers];
    UINT rtvSize_ = 0;
    UINT width_ = 0, height_ = 0;  // swap chain size, physical pixels
    gpu::CommandContext ctx_;
    bool ctxReady_ = false;
    bool broken_ = false;  // a D3D12 error was reported; stop trying
    gpu::DisplayState* display_ = nullptr;
    Fit fit_ = Fit::Whole;
};
