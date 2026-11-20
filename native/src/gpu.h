#pragma once

#include "common.h"
#include "glass_params.h"

#include <d3d11.h>
#include <dxgi1_6.h>
#include <d2d1_3.h>
#include <dwrite_3.h>

#include <string>
#include <vector>

namespace sm {

struct MonitorInfo {
    HMONITOR handle = nullptr;
    RECT rect{};   // physical screen px, virtual-screen coordinates
    RECT work{};   // physical work area px
    UINT dpi = 96;
    bool primary = false;
    std::wstring device;  // \\.\DISPLAY1
};

std::vector<MonitorInfo> EnumerateMonitors();
const MonitorInfo* FindMonitor(const std::vector<MonitorInfo>& list, int index);
double DpiScaleForWindow(HWND hwnd, UINT fallbackDpi);

class Gpu {
public:
    Gpu() = default;
    Gpu(const Gpu&) = delete;
    Gpu& operator=(const Gpu&) = delete;
    ~Gpu();

    bool Initialize(HMONITOR monitor);

    ID3D11Device* device() const { return device_.get(); }
    ID3D11DeviceContext* context() const { return context_.get(); }
    IDXGIDevice* dxgiDevice() const { return dxgiDevice_.get(); }
    IDXGIFactory2* factory() const { return factory_.get(); }
    IDXGIAdapter1* adapter() const { return adapter_.get(); }
    IDXGIOutput* output() const { return output_.get(); }
    ID2D1Factory3* d2dFactory() const { return d2dFactory_.get(); }
    ID2D1Device2* d2dDevice() const { return d2dDevice_.get(); }
    IDWriteFactory* dwrite() const { return dwrite_.get(); }
    IDWriteFontCollection* uiFontCollection() const { return fontCollection_.get(); }
    IDWriteFontCollection* iconFontCollection() const { return iconFontCollection_.get(); }
    IDWriteTypography* tabularTypography() const { return tabularTypography_.get(); }
    bool hasUiFont() const { return fontCollection_ != nullptr; }
    const std::wstring& fontFamily() const { return fontFamily_; }
    // Segoe Fluent Icons (Windows 11) or Segoe MDL2 Assets, used for native
    // system glyphs in the status area.
    const std::wstring& iconFontFamily() const { return iconFontFamily_; }

    ID3D11VertexShader* glassVS() const { return glassVS_.get(); }
    ID3D11PixelShader* glassPS() const { return glassPS_.get(); }
    ID3D11Buffer* glassCB() const { return glassCB_.get(); }
    ID3D11SamplerState* sampler() const { return sampler_.get(); }
    ID3D11BlendState* blend() const { return blend_.get(); }
    ID3D11RasterizerState* rasterizer() const { return rasterizer_.get(); }

    const MonitorInfo& monitor() const { return monitor_; }
    double scale() const { return scale_; }
    const wchar_t* adapterName() const { return adapterName_.c_str(); }

    bool deviceLost = false;
    void Release();

private:
    bool CreateDeviceOnAdapter(IDXGIAdapter1* adapter);
    bool CreateD2DAndDWrite();
    bool CompileGlass();
    bool LoadUiFont();
    bool LoadIconFont();

    MonitorInfo monitor_{};
    double scale_ = 1.0;
    std::wstring adapterName_;
    ComPtr<IDXGIFactory2> factory_;
    ComPtr<IDXGIAdapter1> adapter_;
    ComPtr<IDXGIOutput> output_;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGIDevice> dxgiDevice_;
    ComPtr<ID2D1Factory3> d2dFactory_;
    ComPtr<ID2D1Device2> d2dDevice_;
    ComPtr<IDWriteFactory> dwrite_;
    ComPtr<IDWriteFontCollection> fontCollection_;
    ComPtr<IDWriteFontCollection> iconFontCollection_;
    ComPtr<IDWriteTypography> tabularTypography_;
    std::wstring fontFamily_ = L"Segoe UI Variable Text";
    std::wstring iconFontFamily_ = L"Segoe MDL2 Assets";

    ComPtr<ID3D11VertexShader> glassVS_;
    ComPtr<ID3D11PixelShader> glassPS_;
    ComPtr<ID3D11Buffer> glassCB_;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11BlendState> blend_;
    ComPtr<ID3D11RasterizerState> rasterizer_;
};

}  // namespace sm
