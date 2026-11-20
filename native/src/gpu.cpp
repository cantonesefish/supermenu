#include "gpu.h"

#include "glass_shader_data.h"
#include "json.h"

#include <d3dcompiler.h>
#include <shellscalingapi.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace sm {
namespace {

struct MonitorEnumCtx {
    std::vector<MonitorInfo> list;
};

BOOL CALLBACK MonitorEnumProc(HMONITOR hmon, HDC, LPRECT, LPARAM data) {
    auto* ctx = reinterpret_cast<MonitorEnumCtx*>(data);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hmon, &mi)) return TRUE;
    MonitorInfo info;
    info.handle = hmon;
    info.rect = mi.rcMonitor;
    info.work = mi.rcWork;
    info.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    info.device = mi.szDevice;
    UINT dpiX = 96, dpiY = 96;
    if (SUCCEEDED(GetDpiForMonitor(hmon, MDT_EFFECTIVE_DPI, &dpiX, &dpiY))) info.dpi = dpiX;
    ctx->list.push_back(info);
    return TRUE;
}

using D3DCompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR,
                                      UINT, UINT, ID3DBlob**, ID3DBlob**);

D3DCompileFn GetCompiler() {
    static D3DCompileFn fn = [] {
        HMODULE dll = LoadLibraryW(L"d3dcompiler_47.dll");
        if (!dll) dll = LoadLibraryW(L"d3dcompiler_46.dll");
        if (!dll) dll = LoadLibraryW(L"d3dcompiler_43.dll");
        return dll ? reinterpret_cast<D3DCompileFn>(GetProcAddress(dll, "D3DCompile")) : nullptr;
    }();
    return fn;
}

ComPtr<ID3DBlob> CompileShader(const char* source, const char* entry, const char* target) {
    ComPtr<ID3DBlob> code, errors;
    auto compile = GetCompiler();
    if (!compile) {
        Log(L"shader: d3dcompiler_47.dll not available");
        return nullptr;
    }
    const UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
    HRESULT hr = compile(source, std::strlen(source), "glass.hlsl", nullptr, nullptr, entry, target, flags, 0, code.put(),
                         errors.put());
    if (FAILED(hr)) {
        if (errors) {
            const char* text = static_cast<const char*>(errors->GetBufferPointer());
            std::wstring wide = json::Utf8ToWide(std::string_view(text, errors->GetBufferSize()));
            Log(L"shader compile (%hs) failed: %ls", entry, wide.c_str());
        } else {
            LogHr(L"shader compile", hr);
        }
        return nullptr;
    }
    if (errors) {
        const char* text = static_cast<const char*>(errors->GetBufferPointer());
        if (text && *text) Log(L"shader warnings: %hs", text);
    }
    return code;
}

}  // namespace

std::vector<MonitorInfo> EnumerateMonitors() {
    MonitorEnumCtx ctx;
    EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, reinterpret_cast<LPARAM>(&ctx));
    std::stable_sort(ctx.list.begin(), ctx.list.end(), [](const MonitorInfo& a, const MonitorInfo& b) {
        if (a.primary != b.primary) return a.primary;
        return a.rect.left < b.rect.left;
    });
    return ctx.list;
}

const MonitorInfo* FindMonitor(const std::vector<MonitorInfo>& list, int index) {
    if (list.empty()) return nullptr;
    if (index <= 0) return &list.front();
    if (index < static_cast<int>(list.size())) return &list[index];
    return &list.front();
}

double DpiScaleForWindow(HWND hwnd, UINT fallbackDpi) {
    if (hwnd) {
        using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
        static auto fn = reinterpret_cast<GetDpiForWindowFn>(
            GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
        if (fn) {
            UINT dpi = fn(hwnd);
            if (dpi >= 72 && dpi <= 480) return dpi / 96.0;
        }
    }
    return fallbackDpi / 96.0;
}

Gpu::~Gpu() { Release(); }

void Gpu::Release() {
    glassVS_.reset();
    glassPS_.reset();
    glassCB_.reset();
    sampler_.reset();
    blend_.reset();
    rasterizer_.reset();
    fontCollection_.reset();
    tabularTypography_.reset();
    d2dDevice_.reset();
    d2dFactory_.reset();
    dwrite_.reset();
    dxgiDevice_.reset();
    context_.reset();
    device_.reset();
    output_.reset();
    adapter_.reset();
    factory_.reset();
}

bool Gpu::Initialize(HMONITOR monitor) {
    const auto monitors = EnumerateMonitors();
    monitor_ = {};
    for (const auto& m : monitors) {
        if (m.handle == monitor) {
            monitor_ = m;
            break;
        }
    }
    if (!monitor_.handle) {
        if (monitors.empty()) {
            Log(L"gpu: no monitors");
            return false;
        }
        monitor_ = monitors.front();
    }
    scale_ = monitor_.dpi / 96.0;

    HRESULT hr = CreateDXGIFactory2(0, __uuidof(IDXGIFactory2), reinterpret_cast<void**>(factory_.put()));
    if (FAILED(hr)) {
        LogHr(L"CreateDXGIFactory2", hr);
        return false;
    }

    // Find the adapter that lists this monitor's output.
    ComPtr<IDXGIAdapter1> chosen;
    ComPtr<IDXGIOutput> chosenOutput;
    for (UINT a = 0; factory_->EnumAdapters1(a, chosen.put()) == S_OK; ++a, chosen.reset()) {
        ComPtr<IDXGIAdapter1> adapter;
        chosen.as(adapter);
        if (!adapter) continue;
        for (UINT o = 0;; ++o) {
            ComPtr<IDXGIOutput> out;
            if (adapter->EnumOutputs(o, out.put()) != S_OK) break;
            DXGI_OUTPUT_DESC desc{};
            if (SUCCEEDED(out->GetDesc(&desc)) && desc.Monitor == monitor_.handle) {
                chosen = adapter;
                chosenOutput = out;
                break;
            }
        }
        if (chosenOutput) break;
    }

    if (!chosen) {
        Log(L"gpu: no adapter lists monitor, falling back to default adapter");
        if (!CreateDeviceOnAdapter(nullptr)) return false;
    } else {
        adapter_ = chosen;
        output_ = chosenOutput;
        DXGI_ADAPTER_DESC1 desc{};
        chosen->GetDesc1(&desc);
        adapterName_ = desc.Description;
        if (!CreateDeviceOnAdapter(chosen.get())) return false;
    }
    if (!CreateD2DAndDWrite()) return false;
    if (!CompileGlass()) return false;
    if (!LoadUiFont()) Log(L"gpu: no private HarmonyOS Sans font, using Segoe UI Variable Text");
    Log(L"gpu: %ls, monitor %ld,%ld %ldx%ld dpi=%u scale=%.2f", adapterName_.c_str(), monitor_.rect.left,
        monitor_.rect.top, monitor_.rect.right - monitor_.rect.left, monitor_.rect.bottom - monitor_.rect.top,
        monitor_.dpi, scale_);
    return true;
}

bool Gpu::CreateDeviceOnAdapter(IDXGIAdapter1* adapter) {
    D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_11_0;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    HRESULT hr = D3D11CreateDevice(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2, D3D11_SDK_VERSION, device_.put(), &got,
                                   context_.put());
    if (FAILED(hr)) {
        LogHr(L"D3D11CreateDevice", hr);
        return false;
    }
    dxgiDevice_.reset();
    hr = device_->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(dxgiDevice_.put()));
    if (FAILED(hr)) {
        LogHr(L"IDXGIDevice", hr);
        return false;
    }
    return true;
}

bool Gpu::CreateD2DAndDWrite() {
    D2D1_FACTORY_OPTIONS fo{};
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory3), &fo,
                                   reinterpret_cast<void**>(d2dFactory_.put()));
    if (FAILED(hr)) {
        LogHr(L"D2D1CreateFactory", hr);
        return false;
    }
    hr = d2dFactory_->CreateDevice(dxgiDevice_.get(), d2dDevice_.put());
    if (FAILED(hr)) {
        LogHr(L"ID2D1Factory3::CreateDevice", hr);
        return false;
    }

    ComPtr<IDWriteFactory> base;
    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                             reinterpret_cast<IUnknown**>(base.put()));
    if (FAILED(hr)) {
        LogHr(L"DWriteCreateFactory", hr);
        return false;
    }
    dwrite_ = base;

    // Prefer the copy shipped next to the exe so the icon set is stable and
    // does not depend on a particular Windows installation.
    if (!LoadIconFont()) {
        ComPtr<IDWriteFontCollection> systemFonts;
        if (SUCCEEDED(dwrite_->GetSystemFontCollection(systemFonts.put(), FALSE))) {
            for (const wchar_t* family : {L"Segoe Fluent Icons", L"Segoe MDL2 Assets"}) {
                UINT32 index = 0;
                BOOL exists = FALSE;
                if (SUCCEEDED(systemFonts->FindFamilyName(family, &index, &exists)) && exists) {
                    iconFontFamily_ = family;
                    break;
                }
            }
        }
    }
    Log(L"gpu: icon font %ls", iconFontFamily_.c_str());
    return true;
}

bool Gpu::LoadIconFont() {
    const std::wstring path = ExeDirectory() + L"Fonts\\SegoeIcons.ttf";
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        Log(L"font: private Segoe Fluent Icons missing");
        return false;
    }
    ComPtr<IDWriteFactory5> f5;
    if (FAILED(dwrite_->QueryInterface(__uuidof(IDWriteFactory5), reinterpret_cast<void**>(f5.put())))) {
        return false;
    }
    ComPtr<IDWriteFontSetBuilder1> builder;
    if (FAILED(f5->CreateFontSetBuilder(builder.put()))) return false;
    ComPtr<IDWriteFontFile> fontFile;
    if (FAILED(f5->CreateFontFileReference(path.c_str(), nullptr, fontFile.put()))) return false;
    if (FAILED(builder->AddFontFile(fontFile.get()))) return false;
    ComPtr<IDWriteFontSet> set;
    ComPtr<IDWriteFontCollection1> collection;
    if (FAILED(builder->CreateFontSet(set.put())) || FAILED(f5->CreateFontCollectionFromFontSet(set.get(), collection.put()))) {
        return false;
    }
    UINT32 index = 0;
    BOOL exists = FALSE;
    if (FAILED(collection->FindFamilyName(L"Segoe Fluent Icons", &index, &exists)) || !exists) {
        Log(L"font: Segoe Fluent Icons family missing in private file");
        return false;
    }
    iconFontCollection_.reset();
    collection.as(iconFontCollection_);
    iconFontFamily_ = L"Segoe Fluent Icons";
    Log(L"font: private Segoe Fluent Icons loaded");
    return true;
}

bool Gpu::CompileGlass() {
    ComPtr<ID3DBlob> vsBlob = CompileShader(kGlassHlsl, "VS", "vs_5_0");
    ComPtr<ID3DBlob> psBlob = CompileShader(kGlassHlsl, "PS", "ps_5_0");
    if (!vsBlob || !psBlob) {
        Log(L"gpu: glass shader failed to compile");
        return false;
    }
    HRESULT hr = device_->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, glassVS_.put());
    if (FAILED(hr)) return false;
    hr = device_->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, glassPS_.put());
    if (FAILED(hr)) return false;

    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth = sizeof(GlassParams);
    cbd.Usage = D3D11_USAGE_DEFAULT;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(device_->CreateBuffer(&cbd, nullptr, glassCB_.put()))) return false;

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device_->CreateSamplerState(&sd, sampler_.put()))) return false;

    D3D11_BLEND_DESC bd{};
    auto& rt = bd.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = rt.SrcBlendAlpha = D3D11_BLEND_ONE;
    rt.DestBlend = rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device_->CreateBlendState(&bd, blend_.put()))) return false;

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    rd.ScissorEnable = FALSE;
    if (FAILED(device_->CreateRasterizerState(&rd, rasterizer_.put()))) return false;
    return true;
}

bool Gpu::LoadUiFont() {
    const std::wstring dir = ExeDirectory() + L"Fonts\\";
    const wchar_t* files[] = {L"HarmonyOS_Sans_SC_Light.ttf", L"HarmonyOS_Sans_SC_Regular.ttf",
                              L"HarmonyOS_Sans_SC_Medium.ttf", L"HarmonyOS_Sans_SC_Bold.ttf"};
    for (const wchar_t* file : files) {
        if (GetFileAttributesW((dir + file).c_str()) == INVALID_FILE_ATTRIBUTES) {
            Log(L"font: missing %ls", (dir + file).c_str());
            return false;
        }
    }
    ComPtr<IDWriteFactory5> f5;
    if (FAILED(dwrite_->QueryInterface(__uuidof(IDWriteFactory5), reinterpret_cast<void**>(f5.put())))) {
        Log(L"font: IDWriteFactory5 unavailable");
        return false;
    }
    ComPtr<IDWriteFontSetBuilder1> builder;
    HRESULT hr = f5->CreateFontSetBuilder(builder.put());
    if (FAILED(hr)) {
        LogHr(L"CreateFontSetBuilder", hr);
        return false;
    }
    for (const wchar_t* file : files) {
        ComPtr<IDWriteFontFile> fontFile;
        hr = f5->CreateFontFileReference((dir + file).c_str(), nullptr, fontFile.put());
        if (FAILED(hr)) {
            LogHr(L"CreateFontFileReference", hr);
            return false;
        }
        hr = builder->AddFontFile(fontFile.get());
        if (FAILED(hr)) {
            LogHr(L"AddFontFile", hr);
            return false;
        }
    }
    ComPtr<IDWriteFontSet> set;
    ComPtr<IDWriteFontCollection1> collection;
    hr = builder->CreateFontSet(set.put());
    if (FAILED(hr)) return false;
    hr = f5->CreateFontCollectionFromFontSet(set.get(), collection.put());
    if (FAILED(hr)) return false;
    UINT32 index = 0;
    BOOL exists = FALSE;
    hr = collection->FindFamilyName(L"HarmonyOS Sans SC", &index, &exists);
    if (FAILED(hr) || !exists) {
        Log(L"font: HarmonyOS Sans SC family missing");
        return false;
    }
    fontCollection_.reset();
    collection.as(fontCollection_);
    fontFamily_ = L"HarmonyOS Sans SC";
    if (SUCCEEDED(f5->CreateTypography(tabularTypography_.put()))) {
        DWRITE_FONT_FEATURE feature{DWRITE_FONT_FEATURE_TAG_TABULAR_FIGURES, 1};
        tabularTypography_->AddFontFeature(feature);
    }
    Log(L"font: HarmonyOS Sans SC private collection loaded");
    return true;
}

}  // namespace sm
