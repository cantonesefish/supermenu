#pragma once

#include "common.h"
#include "gpu.h"

namespace sm {

// Applies the DDA-only exclusion attribute to one of our windows. Returns true
// if live duplication can safely be used. Falls back to WDA_EXCLUDEFROMCAPTURE
// (which also hides us from screenshots) only when the DDA attribute is unavailable.
bool ExcludeWindowFromCapture(HWND hwnd);

// A live desktop duplication for one monitor plus a static wallpaper fallback.
class Capture {
public:
    Capture() = default;
    ~Capture();

    bool Initialize(Gpu& gpu);
    void Shutdown();

    // Polls the duplication without blocking. Returns true when a new frame was
    // copied into Texture().
    bool Poll();

    // Rebuild the duplication after display changes / access loss.
    void RequestRestart();
    void TickRestart(double nowMs);

    ID3D11Texture2D* Texture() const;
    bool live() const { return duplication_ != nullptr && usingDda_; }
    UINT width() const { return width_; }
    UINT height() const { return height_; }
    const RECT& rect() const { return rect_; }
    bool usingWallpaper() const { return usingWallpaper_; }

    // Reload the wallpaper fallback / retry live capture at app start.
    bool TryStartDuplication();
    bool EnsureFallback();

private:
    void StopDuplication();
    bool CopyWallpaperToTexture();

    Gpu* gpu_ = nullptr;
    RECT rect_{};
    UINT width_ = 0, height_ = 0;
    ComPtr<IDXGIOutputDuplication> duplication_;
    bool usingDda_ = false;
    bool usingWallpaper_ = false;
    double restartAtMs_ = 0;
    int failedStarts_ = 0;
    ComPtr<ID3D11Texture2D> texture_;   // latest BGR A monitor-size frame
    ComPtr<ID3D11Texture2D> wallpaper_;
    bool haveFrame_ = false;
};

}  // namespace sm
