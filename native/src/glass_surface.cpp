#include "glass_surface.h"

#include <algorithm>
#include <cmath>

namespace sm {
namespace {

bool CopyRoiFromFrame(ID3D11DeviceContext* ctx, ID3D11Texture2D* dst, ID3D11Texture2D* frame, POINT dest,
                      const RECT& srcMonitor, const RECT& monitor) {
    if (!dst || !frame) return false;
    D3D11_TEXTURE2D_DESC fd{};
    frame->GetDesc(&fd);
    const int left = std::max(static_cast<int>(srcMonitor.left - monitor.left), 0);
    const int top = std::max(static_cast<int>(srcMonitor.top - monitor.top), 0);
    const int right = std::min(static_cast<int>(srcMonitor.right - monitor.left), static_cast<int>(fd.Width));
    const int bottom = std::min(static_cast<int>(srcMonitor.bottom - monitor.top), static_cast<int>(fd.Height));
    if (right <= left || bottom <= top) return false;
    D3D11_BOX box{static_cast<UINT>(left), static_cast<UINT>(top), 0, static_cast<UINT>(right),
                  static_cast<UINT>(bottom), 1};
    ctx->CopySubresourceRegion(dst, 0, static_cast<UINT>(dest.x), static_cast<UINT>(dest.y), 0, frame, 0, &box);
    return true;
}

}  // namespace

GlassSurface::~GlassSurface() { Shutdown(); }

void GlassSurface::Shutdown() {
    d2dTarget_.reset();
    d2d_.reset();
    visualEffect_.reset();
    visualScale_.reset();
    visual_.reset();
    dcompTarget_.reset();
    dcomp_.reset();
    rtv_.reset();
    roiSrv_.reset();
    roi_.reset();
    lumaStaging_.reset();
    luma_.clear();
    swap_.reset();
}

IDWriteFactory* GlassSurface::dwrite() const { return gpu_ ? gpu_->dwrite() : nullptr; }

bool GlassSurface::Initialize(Gpu* gpu, HWND hwnd, const RECT& screenRect, int pad, float inset,
                              const wchar_t* debugName) {
    gpu_ = gpu;
    hwnd_ = hwnd;
    debugName_ = debugName ? debugName : L"surface";
    screenRect_ = screenRect;
    monitorRect_ = gpu->monitor().rect;
    pad_ = pad;
    inset_ = inset;
    width_ = static_cast<UINT>(screenRect.right - screenRect.left);
    height_ = static_cast<UINT>(screenRect.bottom - screenRect.top);
    ComputeRoi();
    if (!CreateSwapChain()) return false;
    if (!CreateDComp()) return false;
    // D2D device context is created lazily; direct from the shared D2D device.
    HRESULT hr = gpu_->d2dDevice()->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, d2d_.put());
    if (FAILED(hr)) {
        LogHr(L"CreateDeviceContext", hr);
        return false;
    }
    d2d_->SetDpi(96.0f, 96.0f);
    d2d_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    d2d_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    if (!CreateSizeDependent()) return false;
    Log(L"surface %ls: %ux%u roi=%ld,%ld %ux%u", debugName_.c_str(), width_, height_, roiRect_.left, roiRect_.top,
        roiW_, roiH_);
    return true;
}

void GlassSurface::ComputeRoi() {
    RECT mon = monitorRect_;
    roiRect_.left = std::max(mon.left, screenRect_.left - pad_);
    roiRect_.top = std::max(mon.top, screenRect_.top - pad_);
    roiRect_.right = std::min(mon.right, screenRect_.right + pad_);
    roiRect_.bottom = std::min(mon.bottom, screenRect_.bottom + pad_);
    if (roiRect_.right <= roiRect_.left) roiRect_.right = roiRect_.left + 1;
    if (roiRect_.bottom <= roiRect_.top) roiRect_.bottom = roiRect_.top + 1;
    roiW_ = static_cast<UINT>(roiRect_.right - roiRect_.left);
    roiH_ = static_cast<UINT>(roiRect_.bottom - roiRect_.top);
}

bool GlassSurface::CreateSwapChain() {
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = width_;
    desc.Height = height_;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    HRESULT hr = gpu_->factory()->CreateSwapChainForComposition(gpu_->device(), &desc, nullptr, swap_.put());
    if (FAILED(hr)) {
        LogHr(L"CreateSwapChainForComposition", hr);
        return false;
    }
    ComPtr<IDXGISwapChain2> swap2;
    if (SUCCEEDED(swap_.as(swap2))) swap2->SetMaximumFrameLatency(1);
    return true;
}

bool GlassSurface::CreateDComp() {
    HRESULT hr = DCompositionCreateDevice(gpu_->dxgiDevice(), __uuidof(IDCompositionDevice),
                                          reinterpret_cast<void**>(dcomp_.put()));
    if (FAILED(hr)) {
        LogHr(L"DCompositionCreateDevice", hr);
        return false;
    }
    hr = dcomp_->CreateTargetForHwnd(hwnd_, TRUE, dcompTarget_.put());
    if (FAILED(hr)) {
        LogHr(L"CreateTargetForHwnd", hr);
        return false;
    }
    hr = dcomp_->CreateVisual(visual_.put());
    if (FAILED(hr)) return false;
    hr = visual_->SetContent(swap_.get());
    if (FAILED(hr)) return false;
    hr = dcompTarget_->SetRoot(visual_.get());
    if (FAILED(hr)) return false;
    hr = dcomp_->CreateScaleTransform(visualScale_.put());
    if (FAILED(hr)) {
        LogHr(L"CreateScaleTransform", hr);
        return false;
    }
    hr = dcomp_->CreateEffectGroup(visualEffect_.put());
    if (FAILED(hr)) {
        LogHr(L"CreateEffectGroup", hr);
        return false;
    }
    visual_->SetEffect(visualEffect_.get());
    visualEffect_->SetOpacity(1.0f);
    hr = dcomp_->Commit();
    if (FAILED(hr)) return false;
    return true;
}

bool GlassSurface::CreateSizeDependent() {
    // ROI texture (clipped to the monitor; sampler CLAMP handles outside taps).
    D3D11_TEXTURE2D_DESC td{};
    td.Width = roiW_;
    td.Height = roiH_;
    td.MipLevels = 0;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    UINT mipSupport = 0;
    gpu_->device()->CheckFormatSupport(DXGI_FORMAT_B8G8R8A8_UNORM, &mipSupport);
    if (!(mipSupport & D3D11_FORMAT_SUPPORT_MIP_AUTOGEN)) {
        td.MipLevels = 1;
        td.MiscFlags = 0;
    }
    HRESULT hr = gpu_->device()->CreateTexture2D(&td, nullptr, roi_.put());
    if (FAILED(hr)) {
        LogHr(L"CreateTexture2D(roi)", hr);
        return false;
    }
    hr = gpu_->device()->CreateShaderResourceView(roi_.get(), nullptr, roiSrv_.put());
    if (FAILED(hr)) return false;
    D3D11_TEXTURE2D_DESC created{};
    roi_->GetDesc(&created);
    gridMip_ = 0;
    while (gridMip_ + 1 < created.MipLevels && (roiW_ >> gridMip_) > 160) ++gridMip_;
    gridW_ = std::max(1u, roiW_ >> gridMip_);
    gridH_ = std::max(1u, roiH_ >> gridMip_);
    D3D11_TEXTURE2D_DESC gd{};
    gd.Width = gridW_;
    gd.Height = gridH_;
    gd.MipLevels = 1;
    gd.ArraySize = 1;
    gd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    gd.SampleDesc.Count = 1;
    gd.Usage = D3D11_USAGE_STAGING;
    gd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr = gpu_->device()->CreateTexture2D(&gd, nullptr, lumaStaging_.put());
    if (FAILED(hr)) {
        LogHr(L"CreateTexture2D(luma)", hr);
        return false;
    }
    mipsDirty_ = true;
    hasBackdrop_ = false;
    return true;
}

void GlassSurface::ReleaseSizeDependent() {
    if (d2d_) d2d_->SetTarget(nullptr);
    d2dTarget_.reset();
    rtv_.reset();
    roiSrv_.reset();
    roi_.reset();
    lumaStaging_.reset();
    luma_.clear();
}

void GlassSurface::ResizeBuffers(int width, int height) {
    if (!swap_ || width <= 0 || height <= 0) return;
    if (static_cast<UINT>(width) == width_ && static_cast<UINT>(height) == height_) return;
    width_ = static_cast<UINT>(width);
    height_ = static_cast<UINT>(height);
    ReleaseSizeDependent();
    HRESULT hr = swap_->ResizeBuffers(2, width_, height_, DXGI_FORMAT_B8G8R8A8_UNORM, 0);
    if (FAILED(hr)) {
        LogHr(L"ResizeBuffers", hr);
        deviceLost_ = true;
        return;
    }
    CreateSizeDependent();
}

void GlassSurface::SetScreenRect(const RECT& screenRect) {
    if (EqualRect(&screenRect, &screenRect_)) return;
    screenRect_ = screenRect;
    RECT oldRoi = roiRect_;
    ComputeRoi();
    if (!EqualRect(&oldRoi, &roiRect_)) {
        ReleaseSizeDependent();
        CreateSizeDependent();
    }
    const int w = screenRect.right - screenRect.left;
    const int h = screenRect.bottom - screenRect.top;
    if (static_cast<UINT>(w) != width_ || static_cast<UINT>(h) != height_) ResizeBuffers(w, h);
}

bool GlassSurface::UpdateBackdrop(ID3D11Texture2D* monitorFrame, bool updateLuma) {
    if (!roi_ || !monitorFrame) return false;
    if (!CopyRoiFromFrame(gpu_->context(), roi_.get(), monitorFrame, POINT{0, 0}, roiRect_, monitorRect_)) return false;
    mipsDirty_ = true;
    hasBackdrop_ = true;
    if (updateLuma) UpdateLumaGrid(false);
    return true;
}

bool GlassSurface::UpdateLumaGrid(bool force) {
    if (!roi_ || !lumaStaging_ || mipsDirty_) {
        // mips are generated in Render; callers that need luma request it next frame.
        if (!force) return false;
    }
    const double now = NowMs();
    if (!force && now - lastLumaMs_ < 300.0) return !luma_.empty();
    auto* ctx = gpu_->context();
    D3D11_TEXTURE2D_DESC roiDesc{};
    roi_->GetDesc(&roiDesc);
    if (gridMip_ >= roiDesc.MipLevels) return false;
    ctx->CopySubresourceRegion(lumaStaging_.get(), 0, 0, 0, 0, roi_.get(), gridMip_, nullptr);
    D3D11_MAPPED_SUBRESOURCE map{};
    HRESULT hr = ctx->Map(lumaStaging_.get(), 0, D3D11_MAP_READ, 0, &map);
    if (FAILED(hr)) {
        LogHr(L"Map(luma)", hr);
        return false;
    }
    luma_.resize(static_cast<size_t>(gridW_) * gridH_);
    for (UINT y = 0; y < gridH_; ++y) {
        const auto* row = static_cast<const uint8_t*>(map.pData) + static_cast<size_t>(y) * map.RowPitch;
        for (UINT x = 0; x < gridW_; ++x) {
            const uint8_t* p = row + static_cast<size_t>(x) * 4;
            luma_[static_cast<size_t>(y) * gridW_ + x] =
                (0.0722f * p[0] + 0.7152f * p[1] + 0.2126f * p[2]) / 255.0f;
        }
    }
    ctx->Unmap(lumaStaging_.get(), 0);
    lastLumaMs_ = now;
    return true;
}

void GlassSurface::SetVisualTransform(float scale, float opacity, float cx, float cy) {
    if (!visualScale_ || !visualEffect_) return;
    visualScale_->SetCenterX(cx);
    visualScale_->SetCenterY(cy);
    visualScale_->SetScaleX(scale);
    visualScale_->SetScaleY(scale);
    visual_->SetTransform(visualScale_.get());
    visualEffect_->SetOpacity(opacity);
    visualOpacity_ = opacity;
    CommitVisual();
}

void GlassSurface::CommitVisual() {
    if (dcomp_) dcomp_->Commit();
}

void GlassSurface::CreateD2DTarget() {
    if (d2dTarget_ || !swap_ || !d2d_) return;
    ComPtr<IDXGISurface> surface;
    HRESULT hr = swap_->GetBuffer(0, __uuidof(IDXGISurface), reinterpret_cast<void**>(surface.put()));
    if (FAILED(hr)) {
        LogHr(L"swap->GetBuffer", hr);
        return;
    }
    D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
    hr = d2d_->CreateBitmapFromDxgiSurface(surface.get(), &props, d2dTarget_.put());
    if (FAILED(hr)) {
        LogHr(L"CreateBitmapFromDxgiSurface", hr);
        return;
    }
}

void GlassSurface::PrepareBackdrop() {
    if (!roiSrv_ || !gpu_) return;
    if (mipsDirty_) {
        gpu_->context()->GenerateMips(roiSrv_.get());
        mipsDirty_ = false;
    }
    if (luma_.empty() || NowMs() - lastLumaMs_ > 300.0) UpdateLumaGrid(true);
}

bool GlassSurface::Render(const GlassParams& input, const DrawFn& draw) {
    if (!swap_ || deviceLost_) return false;
    auto* ctx = gpu_->context();
    if (!rtv_) {
        ComPtr<ID3D11Texture2D> back;
        HRESULT hr = swap_->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(back.put()));
        if (FAILED(hr)) {
            LogHr(L"GetBuffer(rtv)", hr);
            return false;
        }
        hr = gpu_->device()->CreateRenderTargetView(back.get(), nullptr, rtv_.put());
        if (FAILED(hr)) {
            LogHr(L"CreateRenderTargetView", hr);
            return false;
        }
    }
    (void)hasBackdrop_;
    PrepareBackdrop();

    const float clear[4] = {0, 0, 0, 0};
    ctx->ClearRenderTargetView(rtv_.get(), clear);
    ID3D11RenderTargetView* rtv = rtv_.get();
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->OMSetBlendState(gpu_->blend(), nullptr, 0xFFFFFFFF);
    D3D11_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(width_), static_cast<float>(height_), 0.0f, 1.0f};
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(gpu_->rasterizer());
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetInputLayout(nullptr);
    ctx->VSSetShader(gpu_->glassVS(), nullptr, 0);
    ctx->PSSetShader(gpu_->glassPS(), nullptr, 0);

    GlassParams p = input;
    p.sizeX = static_cast<float>(width_);
    p.sizeY = static_cast<float>(height_);
    p.roiOriginX = static_cast<float>(screenRect_.left - roiRect_.left);
    p.roiOriginY = static_cast<float>(screenRect_.top - roiRect_.top);
    p.roiSizeX = static_cast<float>(roiW_);
    p.roiSizeY = static_cast<float>(roiH_);
    if (roiSrv_) {
        ID3D11Buffer* cb = gpu_->glassCB();
        ctx->UpdateSubresource(cb, 0, nullptr, &p, 0, 0);
        ctx->PSSetConstantBuffers(0, 1, &cb);
        ID3D11ShaderResourceView* srv = roiSrv_.get();
        ctx->PSSetShaderResources(0, 1, &srv);
        ID3D11SamplerState* sampler = gpu_->sampler();
        ctx->PSSetSamplers(0, 1, &sampler);
        ctx->Draw(3, 0);
    }
    ctx->RSSetState(nullptr);
    ID3D11ShaderResourceView* none = nullptr;
    ctx->PSSetShaderResources(0, 1, &none);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);

    if (d2d_ && draw) {
        CreateD2DTarget();
        if (d2dTarget_) {
            d2d_->SetTarget(d2dTarget_.get());
            d2d_->BeginDraw();
            d2d_->SetTransform(D2D1::Matrix3x2F::Identity());
            draw(d2d_.get());
            HRESULT hr = d2d_->EndDraw();
            d2d_->SetTarget(nullptr);
            if (hr == D2DERR_RECREATE_TARGET) {
                Log(L"surface %ls: D2D target lost", debugName_.c_str());
                d2dTarget_.reset();
                return false;
            }
        }
    }

    HRESULT hr = swap_->Present(0, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        deviceLost_ = true;
        LogHr(L"Present", hr);
        return false;
    }
    return true;
}

float GlassSurface::RegionLuma(float left, float top, float right, float bottom) const {
    if (luma_.empty() || !roiW_ || !roiH_) return 0.5f;
    const float sx = static_cast<float>(gridW_) / static_cast<float>(roiW_);
    const float sy = static_cast<float>(gridH_) / static_cast<float>(roiH_);
    const float ox = static_cast<float>(screenRect_.left - roiRect_.left);
    const float oy = static_cast<float>(screenRect_.top - roiRect_.top);
    int x0 = std::clamp(static_cast<int>((left + ox) * sx), 0, static_cast<int>(gridW_) - 1);
    int x1 = std::clamp(static_cast<int>((right + ox) * sx), 0, static_cast<int>(gridW_) - 1);
    int y0 = std::clamp(static_cast<int>((top + oy) * sy), 0, static_cast<int>(gridH_) - 1);
    int y1 = std::clamp(static_cast<int>((bottom + oy) * sy), 0, static_cast<int>(gridH_) - 1);
    if (x1 < x0) std::swap(x0, x1);
    if (y1 < y0) std::swap(y0, y1);
    float sum = 0;
    int n = 0;
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            sum += luma_[static_cast<size_t>(y) * gridW_ + x];
            ++n;
        }
    }
    return n ? sum / static_cast<float>(n) : 0.5f;
}

float GlassSurface::averageLuma() const {
    if (luma_.empty()) return 0.5f;
    float sum = 0;
    for (float v : luma_) sum += v;
    return sum / static_cast<float>(luma_.size());
}

}  // namespace sm
