#pragma once

#include "common.h"
#include "gpu.h"

#include <dcomp.h>

#include <functional>

namespace sm {

// One DirectComposition-backed transparent window surface:
// live backdrop ROI -> mip chain -> liquid glass shader -> D2D content -> Present.
class GlassSurface {
public:
    using DrawFn = std::function<void(ID2D1DeviceContext2*)>;

    GlassSurface() = default;
    GlassSurface(const GlassSurface&) = delete;
    GlassSurface& operator=(const GlassSurface&) = delete;
    ~GlassSurface();

    bool Initialize(Gpu* gpu, HWND hwnd, const RECT& screenRect, int pad, float inset, const wchar_t* debugName);
    void Shutdown();

    // Call after SetWindowPos. Recomputes the clipped backdrop ROI when needed.
    void SetScreenRect(const RECT& screenRect);
    void ResizeBuffers(int width, int height);

    // Copies the surface's ROI from the monitor-sized capture frame and rebuilds
    // the blur mip chain (done in Render). Returns true if the source was usable.
    bool UpdateBackdrop(ID3D11Texture2D* monitorFrame, bool updateLuma);

    // Generates the blur mip chain and refreshes the luma grid. Call before
    // deciding text ink, then Render() uses the prepared states.
    void PrepareBackdrop();

    // Renders glass + D2D content and presents. Returns false on device loss.
    bool Render(const GlassParams& params, const DrawFn& draw);

    // Opens/closes using DirectComposition scale + opacity around (cx,cy) in
    // window px. Call once per animation tick; cheap.
    void SetVisualTransform(float scale, float opacity, float cx, float cy);

    // Luminance grid (0..1) of the ROI; region coordinates are window px.
    float RegionLuma(float left, float top, float right, float bottom) const;
    bool hasLuma() const { return !luma_.empty(); }
    float averageLuma() const;

    ID2D1DeviceContext2* d2d() const { return d2d_.get(); }
    IDWriteFactory* dwrite() const;
    HWND hwnd() const { return hwnd_; }
    UINT width() const { return width_; }
    UINT height() const { return height_; }
    const RECT& screenRect() const { return screenRect_; }
    bool ready() const { return swap_ != nullptr; }

    void SetOpacity(float opacity) { visualOpacity_ = opacity; }
    void CommitVisual();

private:
    bool CreateSwapChain();
    bool CreateSizeDependent();
    bool CreateDComp();
    void CreateD2DTarget();
    void ReleaseSizeDependent();
    bool UpdateLumaGrid(bool force);
    void ComputeRoi();

    Gpu* gpu_ = nullptr;
    HWND hwnd_ = nullptr;
    std::wstring debugName_;
    RECT screenRect_{};
    RECT monitorRect_{};
    RECT roiRect_{};       // monitor coords
    float inset_ = 12.0f;
    int pad_ = 24;
    UINT width_ = 0, height_ = 0;
    UINT roiW_ = 0, roiH_ = 0;
    UINT gridMip_ = 0, gridW_ = 0, gridH_ = 0;

    ComPtr<IDXGISwapChain1> swap_;
    ComPtr<ID3D11RenderTargetView> rtv_;
    ComPtr<ID2D1Bitmap1> d2dTarget_;
    ComPtr<IDCompositionDevice> dcomp_;
    ComPtr<IDCompositionTarget> dcompTarget_;
    ComPtr<IDCompositionVisual> visual_;
    ComPtr<IDCompositionScaleTransform> visualScale_;
    ComPtr<IDCompositionEffectGroup> visualEffect_;
    ComPtr<ID2D1DeviceContext2> d2d_;

    ComPtr<ID3D11Texture2D> roi_;
    ComPtr<ID3D11ShaderResourceView> roiSrv_;
    ComPtr<ID3D11ShaderResourceView> fallbackSrv_;
    ComPtr<ID3D11Texture2D> lumaStaging_;
    bool mipsDirty_ = false;
    bool hasBackdrop_ = false;
    bool deviceLost_ = false;
    float visualOpacity_ = 1.0f;

    std::vector<float> luma_;
    double lastLumaMs_ = 0;
};

}  // namespace sm
