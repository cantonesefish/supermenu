#include "capture.h"

#include "json.h"

#include <dxgi1_6.h>
#include <wincodec.h>

#include <vector>

namespace sm {
namespace {

struct CompositionData {
    DWORD attrib;
    PVOID data;
    SIZE_T size;
};
constexpr DWORD kWcaExcludedFromDda = 24;
using SetCompositionFn = BOOL(WINAPI*)(HWND, CompositionData*);

SetCompositionFn GetSetComposition() {
    static auto fn = reinterpret_cast<SetCompositionFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowCompositionAttribute"));
    return fn;
}

std::wstring ReadWallpaperStyle() {
    wchar_t style[64]{};
    DWORD size = sizeof(style);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"WallpaperStyle", RRF_RT_REG_SZ, nullptr, style,
                     &size) == ERROR_SUCCESS) {
        return style;
    }
    return L"10";
}

bool ReadWallpaperPath(std::wstring& path) {
    wchar_t buf[MAX_PATH * 4]{};
    if (!SystemParametersInfoW(SPI_GETDESKWALLPAPER, static_cast<UINT>(std::size(buf)), buf, 0)) return false;
    path = buf;
    return !path.empty();
}

ComPtr<IWICImagingFactory> CreateWic() {
    ComPtr<IWICImagingFactory> wic;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory),
                     reinterpret_cast<void**>(wic.put()));
    return wic;
}

struct Image {
    UINT w = 0, h = 0;
    std::vector<uint8_t> bgra;
};

bool LoadImage(IWICImagingFactory* wic, const std::wstring& path, Image& out) {
    ComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr = wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad,
                                                decoder.put());
    if (FAILED(hr)) return false;
    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, frame.put());
    if (FAILED(hr)) return false;
    ComPtr<IWICFormatConverter> converter;
    hr = wic->CreateFormatConverter(converter.put());
    if (FAILED(hr)) return false;
    hr = converter->Initialize(frame.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                               WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) return false;
    UINT w = 0, h = 0;
    converter->GetSize(&w, &h);
    if (!w || !h) return false;
    out.w = w;
    out.h = h;
    out.bgra.resize(static_cast<size_t>(w) * h * 4);
    hr = converter->CopyPixels(nullptr, w * 4, static_cast<UINT>(out.bgra.size()), out.bgra.data());
    return SUCCEEDED(hr);
}

void BlitImage(const Image& src, std::vector<uint8_t>& dst, int dstW, int dstH, int dx, int dy, int scaledW,
               int scaledH, bool scale) {
    // WIC scales; for simplicity here we do a high quality-ish bilinear CPU scale.
    if (scaledW <= 0 || scaledH <= 0) return;
    for (int y = 0; y < scaledH; ++y) {
        int ty = dy + y;
        if (ty < 0 || ty >= dstH) continue;
        int sy = scale ? int((int64_t)y * src.h / scaledH) : y;
        if (sy < 0 || sy >= int(src.h)) sy = std::clamp(sy, 0, int(src.h) - 1);
        for (int x = 0; x < scaledW; ++x) {
            int tx = dx + x;
            if (tx < 0 || tx >= dstW) continue;
            int sx = scale ? int((int64_t)x * src.w / scaledW) : x;
            if (sx < 0 || sx >= int(src.w)) sx = std::clamp(sx, 0, int(src.w) - 1);
            const uint8_t* p = src.bgra.data() + (static_cast<size_t>(sy) * src.w + sx) * 4;
            uint8_t* q = dst.data() + (static_cast<size_t>(ty) * dstW + tx) * 4;
            q[0] = p[0];
            q[1] = p[1];
            q[2] = p[2];
            q[3] = 255;
        }
    }
}

}  // namespace

bool ExcludeWindowFromCapture(HWND hwnd) {
    if (!hwnd) return false;
    BOOL on = TRUE;
    bool ok = false;
    if (auto fn = GetSetComposition()) {
        CompositionData d{kWcaExcludedFromDda, &on, sizeof(on)};
        ok = fn(hwnd, &d) != FALSE;
    }
    if (!ok) {
        // Last resort: hide from all capture APIs, including screenshots. Better
        // than sampling our own glass forever.
        if (SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE)) {
            Log(L"capture: WCA excluded-from-DDA unavailable, using WDA_EXCLUDEFROMCAPTURE on %p", hwnd);
            ok = true;
        } else {
            Log(L"capture: cannot exclude own window %p from capture (err %lu)", hwnd, GetLastError());
        }
    }
    return ok;
}

Capture::~Capture() { Shutdown(); }

void Capture::Shutdown() {
    StopDuplication();
    texture_.reset();
    wallpaper_.reset();
    usingWallpaper_ = false;
}

void Capture::StopDuplication() {
    duplication_.reset();
    usingDda_ = false;
    haveFrame_ = false;
}

bool Capture::Initialize(Gpu& gpu) {
    gpu_ = &gpu;
    rect_ = gpu.monitor().rect;
    width_ = static_cast<UINT>(rect_.right - rect_.left);
    height_ = static_cast<UINT>(rect_.bottom - rect_.top);
    usingWallpaper_ = false;
    if (!EnsureFallback()) Log(L"capture: wallpaper fallback could not be loaded; using a tint");
    TryStartDuplication();
    // Give duplication a first frame without blocking long.
    for (int i = 0; i < 6; ++i) {
        Poll();
        if (haveFrame_) break;
        Sleep(16);
    }
    if (usingDda_) Log(L"capture: Desktop Duplication live %ux%u", width_, height_);
    else Log(L"capture: Desktop Duplication unavailable, wallpaper backdrop");
    return true;
}

bool Capture::TryStartDuplication() {
    if (!gpu_ || !gpu_->output()) return false;
    StopDuplication();
    ComPtr<IDXGIOutput5> out5;
    gpu_->output()->QueryInterface(__uuidof(IDXGIOutput5), reinterpret_cast<void**>(out5.put()));
    HRESULT hr = E_NOINTERFACE;
    ComPtr<IDXGIOutputDuplication> dup;
    if (out5) {
        const DXGI_FORMAT formats[] = {DXGI_FORMAT_B8G8R8A8_UNORM};
        hr = out5->DuplicateOutput1(gpu_->device(), 0, 1, formats, dup.put());
    } else {
        ComPtr<IDXGIOutput1> out1;
        if (SUCCEEDED(gpu_->output()->QueryInterface(IID_PPV_ARGS(out1.put())))) {
            hr = out1->DuplicateOutput(gpu_->device(), dup.put());
        }
    }
    if (FAILED(hr)) {
        ++failedStarts_;
        LogHr(L"DuplicateOutput", hr);
        usingDda_ = false;
        return false;
    }
    DXGI_OUTDUPL_DESC desc{};
    dup->GetDesc(&desc);
    if (desc.Rotation != DXGI_MODE_ROTATION_IDENTITY && desc.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED) {
        Log(L"capture: rotated output not supported by the glass, using wallpaper");
        usingDda_ = false;
        return false;
    }
    duplication_ = dup;
    usingDda_ = true;
    usingWallpaper_ = false;
    haveFrame_ = false;
    Log(L"capture: duplication started (%ux%u)", desc.ModeDesc.Width, desc.ModeDesc.Height);
    return true;
}

bool Capture::EnsureFallback() {
    if (wallpaper_) return true;
    if (!gpu_) return false;
    auto wic = CreateWic();
    ComPtr<IWICBitmap> bitmap;
    std::wstring path;
    Image img;
    bool loaded = false;
    if (wic && ReadWallpaperPath(path)) {
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
            // Transcoded wallpaper is the live copy for slideshow themes.
            std::wstring transcoded = LocalAppData() + L"\\Microsoft\\Windows\\Themes\\TranscodedWallpaper";
            if (GetFileAttributesW(transcoded.c_str()) != INVALID_FILE_ATTRIBUTES) path = transcoded;
        }
        loaded = LoadImage(wic.get(), path, img);
    }
    std::vector<uint8_t> pixels(static_cast<size_t>(width_) * height_ * 4);
    const COLORREF desk = GetSysColor(COLOR_DESKTOP);
    for (size_t i = 0; i < pixels.size(); i += 4) {
        pixels[i + 0] = GetBValue(desk);
        pixels[i + 1] = GetGValue(desk);
        pixels[i + 2] = GetRValue(desk);
        pixels[i + 3] = 255;
    }
    if (!loaded) {
        // A tasteful fallback gradient (deep blue to violet) so the bar still has
        // something to refract when the wallpaper cannot be read.
        for (UINT y = 0; y < height_; ++y) {
            for (UINT x = 0; x < width_; ++x) {
                const float fx = float(x) / std::max(1u, width_);
                const float fy = float(y) / std::max(1u, height_);
                uint8_t* p = pixels.data() + (static_cast<size_t>(y) * width_ + x) * 4;
                p[0] = static_cast<uint8_t>(std::clamp(56.0f + 100.0f * fx + 30.0f * fy, 0.0f, 255.0f));
                p[1] = static_cast<uint8_t>(std::clamp(32.0f + 52.0f * fx + 24.0f * fy, 0.0f, 255.0f));
                p[2] = static_cast<uint8_t>(std::clamp(28.0f + 24.0f * fy + 60.0f * fx, 0.0f, 255.0f));
            }
        }
    } else {
        const std::wstring style = ReadWallpaperStyle();
        const int monW = static_cast<int>(width_), monH = static_cast<int>(height_);
        if (style == L"2") {  // stretch
            BlitImage(img, pixels, monW, monH, 0, 0, monW, monH, true);
        } else if (style == L"0") {  // centre or tile
            const bool tile = GetPrivateProfileIntW(L"Desktop", L"TileWallpaper", 0, L"win.ini") != 0;
            if (tile) {
                for (int y = 0; y < monH; y += int(img.h))
                    for (int x = 0; x < monW; x += int(img.w))
                        BlitImage(img, pixels, monW, monH, x, y, int(img.w), int(img.h), false);
            } else {
                BlitImage(img, pixels, monW, monH, (monW - int(img.w)) / 2, (monH - int(img.h)) / 2, int(img.w),
                          int(img.h), false);
            }
        } else if (style == L"6") {  // fit
            const double s = std::min(double(monW) / img.w, double(monH) / img.h);
            const int w = int(img.w * s), h = int(img.h * s);
            BlitImage(img, pixels, monW, monH, (monW - w) / 2, (monH - h) / 2, w, h, true);
        } else {  // fill (10) and span-ish fallback
            const double s = std::max(double(monW) / img.w, double(monH) / img.h);
            const int w = int(img.w * s), h = int(img.h * s);
            BlitImage(img, pixels, monW, monH, (monW - w) / 2, (monH - h) / 2, w, h, true);
        }
    }

    D3D11_TEXTURE2D_DESC td{};
    td.Width = width_;
    td.Height = height_;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{};
    data.pSysMem = pixels.data();
    data.SysMemPitch = width_ * 4;
    HRESULT hr = gpu_->device()->CreateTexture2D(&td, &data, wallpaper_.put());
    if (FAILED(hr)) {
        LogHr(L"CreateTexture2D(wallpaper)", hr);
        return false;
    }
    usingWallpaper_ = true;
    haveFrame_ = true;
    return true;
}

void Capture::RequestRestart() {
    restartAtMs_ = NowMs() + 1500;
}

void Capture::TickRestart(double nowMs) {
    if (!gpu_) return;
    if (!duplication_ && nowMs >= restartAtMs_) {
        if (TryStartDuplication()) {
            for (int i = 0; i < 4; ++i) {
                Poll();
                if (haveFrame_) break;
                Sleep(8);
            }
        }
        restartAtMs_ = nowMs + 4000;
    }
}

bool Capture::Poll() {
    if (!duplication_ || !gpu_) return false;
    DXGI_OUTDUPL_FRAME_INFO info{};
    ComPtr<IDXGIResource> resource;
    HRESULT hr = duplication_->AcquireNextFrame(0, &info, resource.put());
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return false;
    if (FAILED(hr)) {
        LogHr(L"AcquireNextFrame", hr);
        if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_INVALID_CALL || hr == E_ACCESSDENIED ||
            hr == DXGI_ERROR_SESSION_DISCONNECTED || hr == DXGI_ERROR_DEVICE_REMOVED) {
            StopDuplication();
            usingWallpaper_ = wallpaper_ != nullptr;
            RequestRestart();
        }
        return false;
    }
    if (info.LastPresentTime.QuadPart == 0) {
        duplication_->ReleaseFrame();
        return false;
    }
    ComPtr<ID3D11Texture2D> frame;
    hr = resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(frame.put()));
    if (FAILED(hr)) {
        duplication_->ReleaseFrame();
        return false;
    }
    D3D11_TEXTURE2D_DESC fd{};
    frame->GetDesc(&fd);
    if (fd.Format != DXGI_FORMAT_B8G8R8A8_UNORM || fd.Width != width_ || fd.Height != height_) {
        Log(L"capture: unsupported frame %ux%u fmt=%d", fd.Width, fd.Height, int(fd.Format));
        duplication_->ReleaseFrame();
        StopDuplication();
        usingWallpaper_ = wallpaper_ != nullptr;
        RequestRestart();
        return false;
    }
    if (!texture_) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = width_;
        td.Height = height_;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        hr = gpu_->device()->CreateTexture2D(&td, nullptr, texture_.put());
        if (FAILED(hr)) {
            duplication_->ReleaseFrame();
            return false;
        }
    }
    gpu_->context()->CopyResource(texture_.get(), frame.get());
    duplication_->ReleaseFrame();
    haveFrame_ = true;
    usingWallpaper_ = false;
    return true;
}

ID3D11Texture2D* Capture::Texture() const {
    if (usingDda_ && texture_) return texture_.get();
    if (wallpaper_) return wallpaper_.get();
    return nullptr;
}

}  // namespace sm
