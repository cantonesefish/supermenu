# Atrium Liquid Glass → SuperMenu top bar — port brief

Implementation brief for porting Atrium's liquid-glass material + live desktop backdrop to a
plain-C++ Win32 top menu bar. Written for GCC 16.2 / mingw-w64 (UCRT64), D3D11 + D2D1 +
DirectWrite + DirectComposition, HLSL compiled at runtime via `D3DCompile`, **no MSVC, no
C++/WinRT, no WinUI, no Windows SDK `fxc`**.

References are `path:line` relative to the read-only reference repo `C:\fish\git\atrium-desktop-main`
(MIT). Target repo: `C:\fish\git\kevison-supermenu`.

---

## 0. Verified target-toolchain facts (checked on this machine, 2026-11-19)

Toolchain: MSYS2 UCRT64, `g++.exe 16.2.0`, `mingw-w64-ucrt-x86_64-headers 14.0.0.r598`.
The team's own probe (`docs/dev/toolchain-check/`, results in
`docs/dev/toolchain-check/logs/summary.txt`) already proves this stack configures, compiles, links and
runs in three runtime-linking modes — `dynamic` (24 import DLLs incl. `libgcc_s_seh-1.dll`,
`libstdc++-6.dll`, `libwinpthread-1.dll`), `staticlibgcc` (`-static-libgcc -static-libstdc++`, 24)
and **`static` (`-static`, 23 import DLLs, no MinGW runtime DLLs at all)** — all PASS, including a
`d3dcompiler` link-check executable. **Ship the `static` variant** (`docs/dev/toolchain-check/build.ps1:8-15`).
Toolchain: g++/gcc 16.2.0, cmake 4.5.0, ninja 1.13.2; toolchain file
`docs/dev/toolchain-check/mingw-ucrt64.cmake` (UCRT64 root defaults to `C:/msys64/ucrt64`).
The independent probe below (2026-11-19) compiled/ran `D3DCompile` + `DCompositionCreateDevice` + a
D2D1/DXGI header/link check; output: `16 00000000 17` = GUID size 16, S_OK, `WDA_EXCLUDEFROMCAPTURE` = 17.

| Needed piece | Available? | Notes |
|---|---|---|
| `d3d11.h`…`d3d11_4.h`, `libd3d11.a` | yes | `ID3D11Multithread` present in `d3d11_4.h` |
| `dxgi1_2.h`…`dxgi1_6.h`, `libdxgi.a` | yes | `IDXGIOutput5::DuplicateOutput1` present in `dxgi1_5.h` |
| `d2d1_3.h`, `libd2d1.a` | yes | `ID2D1Factory3`, `ID2D1DeviceContext2` present |
| `dwrite_3.h`, `libdwrite.a` | yes | `IDWriteFactory3`, `IDWriteTextLayout::SetTypography` present |
| `dcomp.h`, `libdcomp.a` | yes (device funcs) | `DCompositionCreateDevice/2/3` link; **`DCompositionWaitForCompositorClock` is not declared/imported** — resolve at runtime via `GetProcAddress(LoadLibraryW(L"dcomp.dll"), …)`; the function is exported on the test machine (Win11 10.0.26300) |
| `d3dcompiler.h`, `libd3dcompiler.a` | yes | imports **`d3dcompiler_47.dll`**; `D3DCompile`, `D3DCOMPILE_OPTIMIZATION_LEVEL3` present |
| `wincodec.h` + `libwindowscodecs.a` | yes | `GUID_WICPixelFormat32bppPBGRA` present |
| `shobjidl.h` | yes | `IDesktopWallpaper`, `CLSID_DesktopWallpaper` present |
| `WDA_EXCLUDEFROMCAPTURE` | yes | `winuser.h` = `0x11` (17); verified at runtime |
| `__uuidof` in GCC | emulated | `_mingw.h`: `#define __uuidof(type) __mingw_uuidof<__typeof(type)>()`; `IID_PPV_ARGS` works. Use type operands only |
| `wrl/client.h` (`Microsoft::WRL::ComPtr`) | **no** | write a ~60-line `ComPtr<T>` (AddRef/Release/QI helper) |
| `winrt/*` headers | **no** | C++/WinRT pieces must be replaced — see §4.5 |

Also: when invoking `g++.exe` from a non-MSYS2 shell (e.g. PowerShell), **`C:\msys64\ucrt64\bin`
must be on `PATH`**, otherwise `cc1plus.exe` dies with `STATUS_DLL_NOT_FOUND` (0xC0000135) and the
driver exits 1 with no message (verified). Pass `-municode` for `wWinMain`.

Link line (verified): `-static` (23 DLLs, portable; see the three link modes above). If you ever need
the dynamic-runtime variant instead:
`-ld3dcompiler -ldcomp -ld3d11 -ldxgi -ld2d1 -ldwrite -lwindowscodecs -luuid -lole32 -luser32 -lgdi32 -lshell32 -lshlwapi -ldwmapi -lversion -lpropsys -ladvapi32 -luxtheme`

---

## 1. Architecture of one glass surface

```
                        ┌──────────────── one glass surface (bar window or popup window) ───────────────┐
 Desktop Duplication    │  IDXGIOutputDuplication::AcquireNextFrame()          (capture thread, §4)     │
 (DXGI output of the    │        │ ID3D11Texture2D monitorFrame  (BGRA, monitor px, monitor-local)    │
  bar's monitor)        │        ▼                                                                     │
                        │  UpdateBackdrop(monitorFrame, win.x-mon.left, win.y-mon.top)                 │
                        │    CopySubresourceRegion → roi_   (B8G8R8A8_UNORM, w+2·pad × h+2·pad)      │
                        │    [optional] SetWindowDisplayAffinity/WCA on all own windows (§4.3)        │
                        │        │ GenerateMips (blur pyramid, MISC_GENERATE_MIPS)                  │
                        │        │ ClearUAV + compare CS  roi_ vs presented_  → changed? (readback)     │
                        │        │ CopySubresource mip k → staging grid → Map(READ|DO_NOT_WAIT) → luma  │
                        │        ▼                                                                     │
                        │  swap chain back buffer (B8G8R8A8_UNORM, premultiplied, flip sequential)    │
                        │    Clear({0,0,0,0})                                                          │
                        │    for each shape back→front: UpdateSubresource cbuffer(400 B); Draw(3,0)    │
                        │    → D2D overlay on the SAME buffer (text, icons, separators)               │
                        │    → swapchain->Present(1,0)     (bar)                                      │
                        │        ▼                                                                     │
                        │  DirectComposition:  visual->SetContent(swapchain); target->SetRoot(visual)  │
                        └─────────────────────────────────────────────────────────────────────────────┘
                                                 ▼
                                     DWM composes the window's visual into the desktop
```

Notes / deliberate simplifications for SuperMenu:

- **One swap chain per window, glass + content in one present.** Atrium splits popups into *two*
  DirectComposition visuals/layers (glass layer from the render thread, content layer from the UI
  thread: `native/core/ui/surface.h:1-8`, `glass_panel.h:1-6`, `bar.cpp:2288`, `surface.cpp:785-807`)
  and passes `overlay = true|false` to the renderer to choose `Present(sync=1)` vs `Present(sync=0)`
  (`glass_renderer.cpp:84`). SuperMenu should draw the glass quad and then D2D text into the same
  back buffer (exactly Atrium's *bar* path, `glass_renderer.cpp:455-553`) — no two-layer handshake,
  no `WM_GLASS_READY`/`Genesis`/`InkGrid` cross-thread machinery.
- **Capture thread is optional.** DDA can be polled from the render thread with
  `AcquireNextFrame(0, …)`; Atrium uses a dedicated pump thread only because it also runs a
  per-monitor capture while the render thread is busy. A single render thread avoids
  `ID3D11Multithread` and frame-lifetime hazards (see §4.1 for the exact hold rules you must keep).
- All px values in the shader/`GlassParams` are **physical pixels**; settings are DIP and are
  multiplied by the monitor scale before upload (`bar.cpp:132,139-161`, `Px()`).

---

## 2. The shader (`native/core/render/shaders/glass.hlsl`, 311 lines)

One signed-distance shape; **Liquid and Frosted are parameter presets** of this same shader
(`glass.hlsl:1-2`). One fullscreen triangle per shape, all work in the pixel shader.

### 2.1 `GlassParams` cbuffer — exact 400-byte layout

Host mirror: `glass_renderer.h:22-42` (`static_assert(sizeof(GlassParams) == 400)`), HLSL:
`glass.hlsl:4-36`. All values float32; HLSL packing gives exactly one 16-byte register per row.
Offsets are byte offsets into the constant buffer.

| Off | Field | Semantics |
|---|---|---|
| 0 | `Size.xy` | window/back-buffer size in px (filled by `Present`, `glass_renderer.cpp:484-485`) |
| 8 | `RoiOrigin.xy` | where the window origin sits inside the ROI texture (px); normally `(pad,pad)`, smaller when the ROI is clipped at a screen edge |
| 16 | `RoiSize.xy` | ROI texture size px (mip0) |
| 24 | `Radius` | corner radius px, clamped by shader to `min(b.x,b.y)` |
| 28 | `Edge` | width of the refracting rim band px |
| 32 | `Refract` | max sample displacement at the rim px |
| 36 | `Dispersion` | relative channel spread of the displacement (0.3 = ±30 %) |
| 40 | `BlurLod` | mip LOD for the blurred backdrop (float, interpolated) |
| 44 | `TintMix` | 0 = untouched backdrop … 1 = solid `Tint` (= `1 - transparency`) |
| 48 | `Tint.rgba` | tint colour, rgb used; a unused (`bar.cpp:151-154` sets a=1) |
| 64 | `Highlight` | specular strength |
| 68 | `Shadow` | drop-shadow strength |
| 72 | `LightDir.xy` | unit vector **towards** the light, y down (`bar.cpp:157-158`: `(-0.6,-0.8)`) |
| 80 | `Inset` | shadow room around the shape px; controls shadow offset+softness |
| 84 | `BlurMix` | 0 = sharp backdrop, 1 = fully blurred (`bar.cpp:148`) |
| 88 | `Fade` | 0 = as-is, 1 = gone (dissolve); multiplies output alpha |
| 92 | `Pad0` | — |
| 96 | `Shape.xyzw` | glass rect in window px: L, T, R, B (0,0,0,0 ⇒ window minus `Inset`, `glass_renderer.cpp:490-494`) |
| 112 | `Ripple.xyzw` | press ripple: centre x,y (window px), ring radius, strength (0 = none) |
| 128 | `PartRect[6]` | optional melting parts, 6 × (L,T,R,B) window px |
| 224 | `PartRadii[6]` | 6 × (TL,TR,BR,BL) corner radii for the parts |
| 320 | `PartCount`, `Melt`, `PartMix`, `Pad1` | parts enable count; smooth-union width; 0=parts…1=rounded rect |
| 336 | `Tail.xyzw` | dock-label pointer: tip x, base half width, height below `Shape.w`, overlap |
| 352 | `Wave.xyzw` | dock hover bump: centre x, support half width, upward height |
| 368 | `FromArea.xyzw` | `D2D1_MATRIX_3X2_F` `_11,_12,_21,_22` — window px → layout px |
| 384 | `FromAreaOffset.xy` | `_31,_32`; then `Pad2` |
| **400** | end | |

The host `D2D1_MATRIX_3X2_F` is 6 floats (`_11,_12,_21,_22,_31,_32`) at offset 368, then 2 pad
floats: byte-compatible with the HLSL field order in `glass_renderer.h:39-40`.
`fromArea(px) = (px.x*_11 + px.y*_21 + _31, px.x*_12 + px.y*_22 + _32)` (`glass.hlsl:147`), i.e.
row-vector D2D convention.

### 2.2 Vertex shader and the full-screen triangle

Entry `VS`, profile `vs_5_0`. No vertex buffer, no input layout; `Draw(3,0)`:

```hlsl
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut VS(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);        // (0,0) (2,0) (0,2)
    o.uv = uv;
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
```
(`glass.hlsl:41-53`). Pixel-shader `px = uv * Size` (window px, y down) — no transform until
`ShapeDistance` applies `fromArea`.

### 2.3 Shape distance: G3 rounded rectangle

Corner family (`corner_profile.h:1-5, 9-28`; shader copy `glass.hlsl:55-65`):

```
R = clamp(radius, 0, min(b.x,b.y))                  b = half extents of the rect
L (reach) = min(1.6*R, min(b.x,b.y))                // 60 % extended transition like Figma iOS
v = 1 - 0.2928932188134525 * R / L                  // 1 - (1-1/√2) R/L = 45° inset preserved
a = max(0.02, (2 v^4 - v^2) / (1 - v^2))            // a=0 only for a whole circle
f(t) = t^4 / (a + (1-a) t^2)                        // f(0..3)(0)=0, f'(1)=2(1+a)
F(x,y) = f(x/L) + f(y/L) - 1 = 0                    // exact implicit outline, G3 at the joins
```

Local coords `q = (|px.x| - b.x + L, |px.y| - b.y + L)`. Two evaluations exist and **must not be
confused**:

- **Coverage** = distance to the *exact* implicit contour, first-order:
  `e = F * L / max(hypot(f'(qx/L), f'(qy/L)), 1e-12)` (`glass.hlsl:67-76`, `corner_profile.h:41-57`).
  No circle fallback, no fitted cubic. `q.x<=0 || q.y<=0` ⇒ straight-edge branch: `max(q)-L`.
- **Metric distance for shading/refraction** (`sdCorner`): a 2-step radial Newton solve of the same
  implicit equation, because the exact edge distance blows up away from the rim
  (`glass.hlsl:78-98`) — comment at `glass.hlsl:228-231` explains why the two paths are separate.

```hlsl
float2 cornerProfile(float2 b, float r) {           // reach L, exponent a
    r = clamp(r, 0, min(b.x, b.y));
    float reach = min(r * 1.6, min(b.x, b.y));
    float v = 1 - 0.2928932188134525 * r / max(reach, 1e-12);
    float v2 = v * v;
    float a = b.x == b.y && r == b.x ? 0
            : max(0.02, (2*v2*v2 - v2) / max(1 - v2, 1e-12));
    return float2(reach, a);
}
float cornerEdge(float2 q, float2 p) {              // exact implicit edge distance
    float r = p.x, a = p.y;
    if (r <= 0 || min(q.x, q.y) <= 0) return max(q.x, q.y) - r;
    float2 v = q / r, v2 = v*v;
    float2 den = a + (1-a)*v2;
    float2 df  = 2*v*v2*(2*a + (1-a)*v2) / (den*den);
    return (dot(v2*v2/den, 1) - 1) * r / max(length(df), 1e-12);
}
float sdCorner(float2 q, float r, float a) {        // metric distance (2 Newton steps)
    if (min(q.x,q.y) <= 0) return max(q.x,q.y) - r;
    if (r <= 0) return length(q);
    float n = length(q); float2 v, df;
    [unroll] for (int i = 0; i < 2; ++i) {
        v = q / n; float2 v2 = v*v;
        float2 den = a + (1-a)*v2;
        float2 f = v2*v2 / den;
        df = 2*v*v2*(2*a + (1-a)*v2) / (den*den);
        n *= 1 + (f.x + f.y - 1) / dot(v, df);
    }
    return (n - r) * dot(v, df) / length(df);
}
```

Rounded rect: `sdRoundRect(p,b,r,edge)`: `profile=cornerProfile(b,r); q=|p|-b+profile.x;`
`edge ? cornerEdge(q,profile) : sdCorner(q,profile.x,profile.y)` (`glass.hlsl:100-105`).
`ShapeDistance` (`glass.hlsl:155-189`) subtracts the shape centre, builds `b`, and optionally:
- `Wave` (C3 upward bump, inversed-y stretch `px.y = Shape.w-(Shape.w-px.y)*h/(h+lift)`, `:158-165`),
- `PartCount` parts with per-corner radii (`sdRoundRect4`, `:107-113`) melted with `smin` (`:173-185`),
- `Tail` triangle pointer (`sdTail`, `:129-145`).
**SuperMenu: delete parts/tail/wave** (leave the fields, send 0/empty) and also delete `PartMix`.

### 2.4 Backdrop sampling, blur LOD, dispersion

```hlsl
float3 bg(float2 px, float lod) { return Bg.SampleLevel(Linear, (px + RoiOrigin)/RoiSize, lod).rgb; }
float3 blurred(float2 px) {                        // 5 taps at a coarse mip hide box-filter structure
    float s = exp2(BlurLod) * 0.75;
    return bg(px,BlurLod)*0.4 + (bg(px+float2(s,0),BlurLod) + bg(px-float2(s,0),BlurLod)
                              +  bg(px+float2(0,s),BlurLod) + bg(px-float2(0,s),BlurLod)) * 0.15;
}
float3 sharpBackdrop(float2 px, float2 off) {      // dispersion: R and B sample at ±Dispersion
    float3 c;
    c.r = bg(px + off*(1+Dispersion), 0).r;
    c.g = bg(px + off,                0).g;
    c.b = bg(px + off*(1-Dispersion), 0).b;
    return c;
}
```
(`glass.hlsl:193-214`). Blur strength is mostly the mip level; the sharp/blurred mix only matters at
the low end to avoid double images (`bar.cpp:146-149`). `BlurLod` is fractional → hardware trilinear
between mip levels; sampler is `MIN_MAG_MIP_LINEAR`, address `CLAMP`
(`glass_renderer.cpp:110-114`), so ROI texture is generated with `MISC_GENERATE_MIPS` and
`GenerateMips(roiSrv)` once per dirty frame (`:346-352`).

### 2.5 Refraction, rim lighting, depth band, shadow, ripple, fade

In `PSImpl` (`glass.hlsl:216-299`), `d` = metric signed distance (negative inside):

```
depth = max(-d, 0)                                   // px inside the shape
t     = saturate(1 - depth/Edge)
profile = 1 - sqrt(saturate(1 - t*t))                // circular lens cross-section: 0 centre → 1 rim
n     = normalize( (d(px+e)-d(px-e), d(px+ey)-d(px-ey)) ), e = 1 px   // outward normal
off   = -n * Refract * profile                       // displacement points INWARD (lens)
```

The rim band therefore bends the backdrop most at the exact outline and not at all
`Edge` px inside; the direction is always inward along the gradient, which is what makes the border
look like a curled glass edge.

```
lit  = saturate(dot(n, LightDir));  back = saturate(dot(n, -LightDir));
rimLine = 1 - smoothstep(0, 1.6, depth);             // 1.6 px bright line on the outline
glow    = saturate(1 - depth/(Edge*1.6));
col += Highlight * rimLine * (0.22 + 0.78*lit + 0.35*back);
col += Highlight * glow*glow * lit * 0.16;
sheen: col += Highlight * 0.045 * s*s,  s = saturate(1 - (px.y-shapeTop)/(shapeBottom-shapeTop))
band  = smoothstep(0,Edge*0.5,depth) * (1-smoothstep(Edge*0.5,Edge*1.3,depth));
col  *= 1 - 0.05*band*(1 - lit);                     // slight shade where the lens bends most
```

Vibrancy and tint (before the light terms) — this is the "liquid" look: content under the glass gets
richer/brighter before the tint is mixed in (`glass.hlsl:272-277`):

```
luma = dot(col, (0.2126, 0.7152, 0.0722));           // sRGB-encoded values, not linearised
col  = lerp(luma.xxx, col, 1.0 + 0.22*Highlight) * (1.0 + 0.04*Highlight);
col  = lerp(col, Tint.rgb, TintMix);
```

Shadow (only evaluated outside; `glass.hlsl:221-226`): the shape is sampled shifted **down** by
`Inset*0.25`, exponential falloff with scale `Inset*0.35`:

```
if (d >= 0) { ds = Dist(px - (0, Inset*0.25)); shadowA = Shadow * exp(-max(ds,0)/max(Inset*0.35,1)); }
shadowA *= saturate(1 - Fade);
```

Press ripple (`glass.hlsl:250-262`): Gaussian ring at `Ripple.z` running outward, displacement
`+= dir*ring*(6 + Refract*0.35)`, dent `-= dv*0.06*Ripple.w*saturate(1-dl/Ripple.z)`; brightness
`col += ring*0.10*(0.4+Highlight)`. Host drives it over 720 ms as
`R=150*scale*(1-(1-u)^3)`, `A=0.8*(1-u)^2` (`bar.cpp:2394-2402`).

Fade/keep: `keep = saturate(1 - Fade)` multiplies the whole premultiplied result.

### 2.6 Coverage, output blending, `PS` vs `PSBasic`

Coverage is computed **from the analytic edge**, not from the metric distance, because the metric
field jumps by hundreds of px where a corner meets a straight side deep inside the shape
(`glass.hlsl:228-234`):

```
edgeDistance = ShapeDistance(px, /*edge=*/true, basic);
aa  = max(fwidth(edgeDistance), 1e-3);
cov = (d < -2) ? 1 : saturate(0.5 - edgeDistance / aa);
if (cov <= 0) return float4(0,0,0, shadowA);
...
float4 glass = float4(saturate(col) * cov, cov) * keep;
return glass + (1 - cov) * float4(0, 0, 0, shadowA);
```

So the output is **premultiplied**: interior = opaque lensed backdrop (`a = 1`), outside = shadow
premultiplied by `(1-cov)`. `TintMix`/`transparency` do **not** make the glass see-through; the
backdrop already contains the windows behind, and the lens/blur/tint produce the transparency look.

`PSBasic` (`glass.hlsl:302-311`) is an optimisation: when the fragment is farther than
`max(Edge,2)` px from the shape's bounding box, it takes the `basic=true` path (no wave/parts/tail,
metric distance via `sdCorner`); at the rim it falls back to the exact path to avoid one-8-bit-level
sampling differences. SuperMenu's bar is only 28 px tall → almost every pixel is "near the rim"; either
compile only `PS` (and drop the `basic` bool), or keep both and use the same selection rule
(`glass_renderer.cpp:481-483`).

### 2.7 Compilation (runtime `D3DCompile`)

Atrium precompiles with SDK `fxc /nologo /O3 /WX /T <profile> /E <entry>`
(`native/core/CMakeLists.txt:3-27`). SuperMenu compiles the same entry points at startup:

| source | entry | profile | used for |
|---|---|---|---|
| `glass.hlsl` | `VS` | `vs_5_0` | fullscreen triangle |
| `glass.hlsl` | `PS` | `ps_5_0` | full material |
| `glass.hlsl` | `PSBasic` | `ps_5_0` | optional fast path |
| `compare.hlsl` | `CS` | `cs_5_0` | ROI comparison (§3.10) |
| `backdrop_edge.hlsl` | `PS` | `ps_5_0` | optional monitor-edge replication — **not needed** with the clipped-ROI design (§6.1) |

```c
typedef HRESULT (WINAPI *D3DCompileFn)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*,
                                       LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
// HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll"); pD3DCompile = (D3DCompileFn)GetProcAddress(m, "D3DCompile");
HRESULT hr = pD3DCompile(src, srcLen, "glass.hlsl", NULL, NULL, "PS", "ps_5_0",
                         D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_ENABLE_STRICTNESS,
                         0, &code, &errs);          /* then CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), NULL, &ps) */
```
`D3DCompile` (via `-ld3dcompiler`) also works and imports `d3dcompiler_47.dll`; dynamic loading is
preferred so a missing DLL degrades to "no glass" instead of failing process start. `ps_5_0`
requires D3D feature level 11_0 → require FL 11_0 for the glass device (WARP 11_0 is also fine).
Embed the HLSL as a string literal (`shaders/glass_hlsl.h`, generated by a small build step or by
hand) so no files are needed at runtime.

---

## 3. D3D11 / DXGI / DirectComposition / Direct2D setup

Source: `native/core/render/glass_renderer.cpp:79-164` (constructor), `:188-258` (size-dependent
resources/resize), `:450-553` (Present); struct at `glass_renderer.h:107-170`.

### 3.1 Device

`gpu.cpp:11-29`: `D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, {11_1,11_0}, 2, D3D11_SDK_VERSION, &device, NULL, &context)`.
`BGRA_SUPPORT` is required for the D2D1 interop. Pick the adapter that owns the bar monitor
(`gpu.cpp:63-115`: `IDXGIFactory2`, `EnumAdapters1` → `EnumOutputs` → `DXGI_OUTPUT_DESC.Monitor`,
compare with `GetMonitorInfo`; optionally use display-config `QueryDisplayConfig` to resolve the
scan-out adapter on hybrid laptops, `gpu.cpp:34-59`). For v1 one monitor + one hardware device.

### 3.2 Constant buffer / sampler / states (exact)

| object | setting |
|---|---|
| cbuffer | `D3D11_BUFFER_DESC{sizeof(GlassParams)=400, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER}`, `PS/VS SetConstantBuffers(0,1,&cb)`, upload per shape with `UpdateSubresource` (`glass_renderer.cpp:93-94, 475-476, 522`) |
| compare cbuffer | 16 bytes `{uint Width, Height, TilesX, TileSize}` (`:95, 237-238`) |
| sampler | `D3D11_FILTER_MIN_MAG_MIP_LINEAR`, `AddressU/V/W = CLAMP`, `MaxLOD = D3D11_FLOAT32_MAX` (`:110-114`) |
| blend | premultiplied over: `SrcBlend = SrcBlendAlpha = ONE`, `DestBlend = DestBlendAlpha = INV_SRC_ALPHA`, `BlendOp(Alpha) = ADD`, write mask all (`:115-123`) — several shapes + their shadows layer back→front |
| rasterizer | `FillMode = SOLID`, `CullMode = NONE`, `DepthClipEnable = TRUE`, `ScissorEnable = TRUE` (`:124-129`) |
| topology | `D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST`, `IASetInputLayout(nullptr)`, `VSSetShader(vs)`, `Draw(3,0)` |
| viewport | `{0,0,w,h,0,1}` for the glass pass; `{0,0,roiW,roiH,0,1}` for edge-replication pass |
| scissor | one rect per shape: transformed shape bounds inflated by `m = 2*max(inset,1)+2`, clamped to the window (`glass_renderer.cpp:497-521`) |

### 3.3 Backdrop ROI texture and mips

`glass_renderer.cpp:188-244`:

- `roi_`: `MipLevels = 0` (full chain), `ArraySize = 1`, `DXGI_FORMAT_B8G8R8A8_UNORM`,
  `BindFlags = SHADER_RESOURCE | RENDER_TARGET`, `MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS`;
  SRV default (all mips), RTV for the edge pass. Dimensions = `w + 2*kPad` × `h + 2*kPad`,
  `kPad = 64` (`glass_renderer.h:66`); for the widget desktop-host case both origin and extent are
  rounded to a 64-px grid so the mip texel phase is stable across frames (`:193`, `:265-268`).
- `presented_`: `MipLevels = 1`, `BIND_SHADER_RESOURCE`, same format; holds `roi_` mip 0 at the time
  of the last `Present` (copied at `:548`) — the "did the backdrop change?" reference.
- `GenerateMips(roiSrv)` only when `mipsDirty_` (`:346-352`); `mipsDirty_` is set by
  `UpdateBackdrop` and coalesced with the comparison dispatch (`:354-384`).
- Luma grid staging: first mip whose width ≤ 96 texels (`while (rw >> gridMip) > 96 ++gridMip`),
  `gridW/gridH = max(1, rw >> gridMip)`, staging texture `B8G8R8A8_UNORM`, `CPU_ACCESS_READ`,
  one per readback slot (3 slots) (`:215-227`).
- Comparison: `cmpOut_` = structured buffer of 4 `uint` (`BIND_UNORDERED_ACCESS`), plus 3 staging
  read buffers, cleared with `ClearUnorderedAccessViewUint`, dispatched
  `(roiW+15)/16 × (roiH+15)/16`, then `CopyResource` counters + `CopySubresourceRegion` grid mip into
  the slot, queued in an in-flight ring (`:98-108`, `:354-384`).

**SuperMenu simplification:** clip the ROI to the monitor rect and rely on the sampler's `CLAMP`
addressing for off-screen padding (see §6.1); then `ExtendBackdropEdges` + `backdrop_edge.hlsl`
(`glass_renderer.cpp:286-344`, `shaders/backdrop_edge.hlsl:1-14`) can be deleted. Keep
`pad ≈ max(ceil(0.75*2^BlurLod) + 16, 16)` so the blur taps stay inside the ROI (Atrium uses a fixed
64; taps that still fall outside are clamped by the sampler to border texels — only a slight border
smear, invisible at the rim).

### 3.4 Swap chain (exact desc)

`glass_renderer.cpp:131-145`:

```
DXGI_SWAP_CHAIN_DESC1 desc = {};
desc.Width  = w;  desc.Height = h;
desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
desc.SampleDesc.Count = 1;
desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
desc.BufferCount = 2;
desc.Scaling = DXGI_SCALING_STRETCH;
desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;      // must match the premultiplied shader
desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
factory->CreateSwapChainForComposition(device, &desc, nullptr, &swap);
swap->QueryInterface(IDXGISwapChain2) → SetMaximumFrameLatency(1);
waitable = IDXGISwapChain2::GetFrameLatencyWaitableObject();
```

No `DXGI_PRESENT_*` flags; `Present(sync, 0)`, `sync = 1` for the bar (vsync; the waitable object
then gates the next frame) and `0` for popup glass that must land in the same composition as another
visual (`:84`, `:550`). Single-swap-chain popups: use `sync = 1` and drop the distinction.

### 3.5 DirectComposition creation order

`glass_renderer.cpp:147-153`:

```
IDXGIDevice* dxgi = device->QueryInterface(IDXGIDevice);
DCompositionCreateDevice(dxgi, IID_PPV_ARGS(&dcomp));
dcomp->CreateTargetForHwnd(hwnd, /*topmost=*/TRUE, &target);   // bar: TRUE, popup glass: FALSE, popup content: TRUE
dcomp->CreateVisual(&visual);
visual->SetContent(swap);
target->SetRoot(visual);
dcomp->Commit();
```

`topmost=TRUE` puts the visual above the window's content; Atrium uses two targets per popup HWND
(glass `FALSE`, content `TRUE`) — one target per window with the single-swap-chain design.
Window style: `WS_POPUP`, `WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST |
WS_EX_NOREDIRECTIONBITMAP` (+`WS_EX_LAYERED` + `SetLayeredWindowAttributes(0,255,LWA_ALPHA)` as in
`bar.cpp:339-343`; `WS_EX_NOREDIRECTIONBITMAP` is required for a composition swap chain on a window).
Return `MA_NOACTIVATE` from `WM_MOUSEACTIVATE` so clicking the bar never steals focus. Commit after
every visual change; recreate `dcomp`/target/visual together with the device on loss.

### 3.6 D2D1 overlay on the same back buffer

`glass_renderer.cpp:155-161` (factory/device/context creation) and `:530-546` (per present):

- `D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory3), &opts, &factory)`;
  `factory->CreateDevice(dxgiDevice, &d2dDevice)`; `d2dDevice->CreateDeviceContext(
  D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2d)`; `DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
  __uuidof(IDWriteFactory3), &dwrite)`.
- Per present: get swap chain buffer 0 as `IDXGISurface`, `d2d->CreateBitmapFromDxgiSurface(surface,
  D2D1_BITMAP_PROPERTIES1{ D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
  {DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED} }, &target)`;
  `d2d->SetTarget(target); d2d->BeginDraw(); d2d->SetTransform(Identity); overlay(d2d);
  hr = d2d->EndDraw(); d2d->SetTarget(nullptr);` — `D2DERR_RECREATE_TARGET` ⇒ device lost.
- Ordering on the same D3D device: unbind the RTV and the PS SRVs before D2D
  (`OMSetRenderTargets(0,nullptr,nullptr)`, `PSSetShaderResources(0,2,none)`), as in
  `:525-528`; D2D then draws on top of the glass. Use `D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE`
  (premultiplied target ⇒ no ClearType, `bar_painter.cpp:404`).
- Resize: `d2d->SetTarget(nullptr)`, release RTV + D2D bitmap, `ResizeBuffers(2,w,h,
  DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)`, recreate
  size-dependent resources (`:246-258`; flags must match creation).
- Cache D2D content: Atrium renders the bar content into an `ID2D1Bitmap1` layer and only redraws it
  when the content key changes or an animation runs (`bar_painter.cpp:356-384`) — do the same.

### 3.7 Device loss

`Present` returns `false` on `DXGI_ERROR_DEVICE_REMOVED/RESET` (`glass_renderer.cpp:552`) and on
`D2DERR_RECREATE_TARGET` (`:544-545`). Host (`bar.cpp:1976-1991`) tears down the whole render session
(device, swap chains, DComp device/visuals, capture object) and rebuilds after 1 s.
The DDA `IDXGIOutputDuplication` is bound to the device → recreate it too; `DXGI_ERROR_ACCESS_LOST`
is the normal "display changed / secure desktop" signal, not just device loss.

### 3.8 Frame pacing / present-on-demand (the part that makes it cheap)

Atrium's rules (`bar.cpp:2469-2772`):

1. Wake on `{quit, wake, capture-frame-event}`. When animating or owed a frame, wait on the compositor
   clock (`DCompositionWaitForCompositorClock`, dynamically loaded from `dcomp.dll`, `bar.cpp:35-38,
   2481-2488`); idle: `WaitForMultipleObjects` with 1 s timeout (or next whole second when a seconds
   clock is shown); readback pending: 3–6 ms.
2. `Ready()`: `WaitForSingleObject(waitable, 0) == WAIT_OBJECT_0`; after `Present`, `ready_ = false`
   (`glass_renderer.cpp:173-178`, `:551`). **Never present a surface whose previous frame is still
   queued** — mark it "owed" and retry next tick.
3. Present only if something changed: backdrop ROI differs (`compare` result), a new capture frame,
   content/text/ink changed, an animation runs, or the last present is older than `kStaleMs = 2000`
   (`bar.cpp:60, 2726-2736`). "Echoes" (own present appearing in the capture) are skipped this way —
   but this is only a safety net, not a substitute for window exclusion (§4.3).
4. `Changed` is computed against `presented_`, the ROI copy from the last present — cheap and exact
   because both are the same texture `roi_` mip 0 (`glass_renderer.cpp:548-550`).

### 3.9 ROI copy (exact)

`glass_renderer.cpp:260-284`: `roiOrigin_ = (win.x - roi.x0, win.y - roi.y0)`; copy the intersection
of the ROI with the capture frame:
`CopySubresourceRegion(roi_, 0, cx0-x0, cy0-y0, 0, frame, 0, &box{D3D11_BOX{cx0,cy0,0,cx1,cy1,1}})`.
Only the rectangle is copied — never the whole 3200×2000 monitor frame. With clipped ROIs this
becomes a single always-in-bounds copy. Mark mips dirty, queue comparison.

### 3.10 Compare compute shader (`shaders/compare.hlsl`, 47 lines)

`compare.hlsl`: `Texture2D<float4> Cur : t0`, `Prev : t1`, `RWStructuredBuffer<uint> Out : u0`,
cbuffer `{uint Width, Height, TilesX, TileSize}`, `[numthreads(16,16,1)]`; per pixel
`m = round(max(|d.r|,|d.g|,|d.b|)*255)`, atomically counts pixels where `m != 0` (Out[0]) and tracks
the max difference (Out[1]); tile slots Out[2+…] are used only by the heat-map fixture. Production
passes one tile covering the whole ROI (`cmpCb = {rw, rh, 1, 1u<<30}`, `glass_renderer.cpp:237-238`).
Readback is non-blocking: `Map(..., D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT)` and
`DXGI_ERROR_WAS_STILL_DRAWING` means "not ready"; 3-slot ring so the GPU never stalls
(`glass_renderer.cpp:386-448`). **SuperMenu can simplify Out to a single `uint` counter** (no tiles).

For SuperMenu the compare is still worth keeping: the DDA frame is full-monitor, so a video playing
at the bottom of the screen would otherwise force 30–60 fps redraws of a bar whose backdrop did not
change. If you drop it, fall back to "present on each DDA frame + 2 s stale timer" and accept that
cost.

### 3.11 Multi-surface capture discipline

When one captured frame feeds the bar and several popups, copy every surface's ROI from the frame,
`context->Flush()`, then release the frame (`bar.cpp:2579-2587, 2642-2644`) — with a shared device the
capture thread must not wait for the GPU while holding the duplication lock. This applies if you
keep a capture pump thread; with in-thread polling, call `AcquireNextFrame` early in the frame, do
all copies, then `ReleaseFrame` before the next acquire.

---

## 4. Backdrop capture

### 4.1 Desktop Duplication flow (plain COM)

Reference implementation: `native/core/render/capture.cpp:145-301`.

```
create:
  factory = CreateDXGIFactory2(0, IID_IDXGIFactory2)
  find the IDXGIOutput whose DXGI_OUTPUT_DESC.Monitor == bar's HMONITOR
      (enumerate adapters → EnumOutputs; the bar's monitor may be listed under another adapter on
       hybrid laptops, gpu.cpp:63-115; then create the D3D device on that adapter)
  out5 = output->QI(IDXGIOutput5)
  if (out5) out5->DuplicateOutput1(device, 0, 1, {DXGI_FORMAT_B8G8R8A8_UNORM}, &dup)   // SDR frame even on HDR desktop
  else      output->QI(IDXGIOutput1)->DuplicateOutput(device, &dup)                    // format = desktop format
  dup->GetDesc(&d); require d.Rotation == IDENTITY || UNSPECIFIED                       // capture.cpp:203-208
  context->QI(ID3D11Multithread)->SetMultithreadProtected(TRUE)                        // if using a pump thread
  pump thread:
    AcquireNextFrame(timeout, &info, &resource)
      WAIT_TIMEOUT          → sleep 5 ms, retry
      FAILED                → lost = true; signal frame event; stop      // ACCESS_LOST / E_ACCESSDENIED
      info.LastPresentTime == 0 → ReleaseFrame(); continue               // pointer-only update
      held = resource (ID3D11Texture2D); ready = true; signal event
      wait {stop, released} before next AcquireNextFrame                 // hold exactly one frame
  render thread:
    Acquire() → the held ID3D11Texture2D* (valid until Release())
    Release() → texture=null; dup->ReleaseFrame(); signal released
```

Rules that matter: **at most one acquired frame at a time**, `ReleaseFrame` after all
`CopySubresourceRegion` calls from that frame, never block the render thread in `AcquireNextFrame`
(use timeout 0 from the pump, or call it early in the render loop), one duplication per output
system-wide (opening it fails with `DXGI_ERROR_NOT_CURRENTLY_AVAILABLE` if another process duplicates
that output), and the duplicated frame carries the composed desktop **minus windows excluded from
DDA** (§4.3) and without the hardware cursor.

### 4.2 Monitors, formats, failure modes

- Format: request `B8G8R8A8_UNORM` via `DuplicateOutput1`; for HDR desktops DXGI converts to 8-bit.
  Fallback `DuplicateOutput` may return `R16G16B16A16_FLOAT` on HDR — the shader's `SampleLevel`
  works either way, but the compare/luma code (BGRA byte math) does not; keep a BGRA path or
  convert. v1: require `DuplicateOutput1` (Win10 1803+) and fall back to the wallpaper on HDR.
- Rotation: rotated displays deliver frame pixels unrotated; reject (`DXGI_ERROR_UNSUPPORTED`) and use
  the wallpaper, or rotate the ROI copy. A top bar on the primary landscape monitor makes this rare.
- Table (`capture.cpp:147-151`, `bar.cpp:2030-2090, 2533-2551`):

| HRESULT / condition | meaning | action |
|---|---|---|
| `DXGI_ERROR_WAIT_TIMEOUT` | no change since last frame | no new frame; keep last one; keep idle loop asleep |
| `info.LastPresentTime == 0` | pointer moved only | `ReleaseFrame`, skip |
| `DXGI_ERROR_ACCESS_LOST` | display mode change / secure desktop transition / session change | recreate duplication (after the desktop is back) |
| `E_ACCESSDENIED` | secure desktop, lock screen, UAC prompt | temporary: show wallpaper/plain panel, retry every 1 s |
| `DXGI_ERROR_SESSION_DISCONNECTED` | RDP session gone | temporary, retry |
| `DXGI_ERROR_NOT_CURRENTLY_AVAILABLE` | another duplicator holds the output | retry with backoff (e.g. 30 s) |
| `DXGI_ERROR_DEVICE_REMOVED/RESET` | GPU reset/driver update | rebuild device + duplication |
| desktop frame black over a lit screen | driver bug after sleep | optional: GDI-sample the strip (`bar.cpp:1993-2026`) and restart duplication / go wallpaper |

Monitor/display changes: handle `WM_DISPLAYCHANGE`, `WM_SETTINGCHANGE`, `WM_DPICHANGED`,
`WM_WTSSESSION_CHANGE` (or `WTSRegisterSessionNotification`) → recreate capture + resize windows.
The bar's monitor changes if the user moves it / primary changes (`bar.cpp:2550-2551` returns
"rebuild session" on monitor change).

### 4.3 Excluding our own windows (mandatory, no way around it)

The glass samples the screen. If our bar/popups appear in that capture, the next frame samples the
previous glass → infinite feedback ring (this is exactly why Atrium has two capture backends with
different exclusion policies, `capture.h:1-10`).

1. **Primary: `SetWindowCompositionAttribute(hwnd, WCA_EXCLUDED_FROM_DDA)`** — undocumented user32
   export, attribute id **24**, payload `BOOL on`, struct
   `{ DWORD Attrib; PVOID pvData; SIZE_T cbData; }` (`capture.cpp:18-35, 57-70`). Effect: the window
   is left out of **Desktop Duplication only**; normal screenshots/recordings still show the bar
   (desired). Resolve with `GetProcAddress(GetModuleHandleW(L"user32.dll"),
   "SetWindowCompositionAttribute")`; no header needed. Apply to **every** own visible window: bar +
   each popup, because a popup overlapping the bar's ROI would otherwise be sampled by the bar and
   vice versa.
2. **Fallback: `SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE /*0x11*/)`** — documented,
   Win10 2004+. Excludes from capture APIs (incl. Desktop Duplication) but **also hides the app from
   the user's screenshots/recordings** — acceptable as a degraded mode, not as the default.
3. **If both fail** (very old Windows, API blocked): do **not** show live DDA glass. Degrade to the
   wallpaper backdrop (§4.5) or a static tinted panel. Never run DDA sampling while own windows can
   appear in it.
4. Re-apply the attribute after session unlock / display change / window recreation; apply it before
   the first `Present` (create the window, set the attribute, then start duplication). Atrium also
   starts every window hidden from *all* captures until the render thread has a DDA frame it trusts
   (`bar.cpp:352-359, 2080-2085`).
5. Debug aid: Atrium's `--debug-capturable` freezes the first captured frame because with the
   WinRT fallback the windows must be visible (`bar.cpp:2086-2087`) — a useful test hook.

### 4.4 Wallpaper fallback (plain COM, no WinRT) — `desktop_backdrop.cpp:44-99, 406-533`

Use it when (a) exclusion unavailable, (b) duplication unavailable/black, (c) user selects a
"wallpaper" backdrop mode.

```
worker thread (COM MTA, poll every 3 s):
  CoCreateInstance(CLSID_DesktopWallpaper, ..., IID_IDesktopWallpaper, &dw)
  CoCreateInstance(CLSID_WICImagingFactory, ..., IID_IWICImagingFactory, &wic)
  dw->GetPosition(&pos); dw->GetBackgroundColor(&bg);
  dw->GetMonitorDevicePathCount(&n); per monitor:
      GetMonitorDevicePathAt(i,&id); GetMonitorRECT(id,&rect); GetWallpaper(id,&path)
      if path missing/unreadable → %APPDATA%\Microsoft\Windows\Themes\TranscodedWallpaper
  canvas = virtual screen (SM_XVIRTUALSCREEN/Y, SM_CXVIRTUALSCREEN/CY), fill with bg colour, alpha 255
  per monitor: decode with WIC → IWICFormatConverter to GUID_WICPixelFormat32bppPBGRA
    FILL/SPAN : s = max(tw/iw, th/ih);  FIT: s = min(tw/iw, th/ih);  STRETCH: sx,sy separately;
    CENTRE    : 1:1;                      TILE: repeat, else centre-blit
    (IWICBitmapScaler, WICBitmapInterpolationModeHighQualityCubic)
  signature = pos|bg|virtual-rect|per-monitor rect:path@filetime:size  → recompose only if changed
  publish BGRA picture + origin; signal frame event
render thread:
  on new picture: create/keep a monitor-sized D3D11 texture (B8G8R8A8_UNORM, BIND_SHADER_RESOURCE),
  CopySubresourceRegion the monitor's part; then use it as the "monitorFrame" for UpdateBackdrop
```
Atrium's `WallpaperCapture` does exactly that (`desktop_backdrop.cpp:502-533`). Very small code,
pure COM; `IDesktopWallpaper` + `CLSID_DesktopWallpaper` + WIC are all in the verified MinGW headers.
Simpler single-monitor v1: `SystemParametersInfoW(SPI_GETDESKWALLPAPER)` + `GetSysColor(COLOR_DESKTOP)`
if `IDesktopWallpaper` creation fails.

### 4.5 WinRT inventory → replacements

| Atrium (WinRT) | where | plain-Win32/COM replacement |
|---|---|---|
| `Windows.Graphics.Capture` (`MonitorCapture`, desktop-window capture) | `capture.cpp:74-143`, `desktop_backdrop.cpp:103-360` | **drop**. DDA only; wallpaper fallback covers DDA failure. (Plain replacement exists — `IGraphicsCaptureItemInterop` via `RoGetActivationFactory` + `IGraphicsCaptureItemInterop::CreateForMonitor` + `IDirect3D11CaptureFramePool` internals — but it is substantially more COM plumbing and the frame pool's COM interfaces are not in MinGW headers. Not worth it.) |
| `CreateDirect3D11DeviceFromDXGIDevice`, `IDirect3DDevice`, `IDirect3DDxgiInterfaceAccess` | `capture.cpp:80-88, 130-134` | not needed with DDA (return the `ID3D11Texture2D` directly) |
| `winrt::com_ptr<T>` | everywhere | small `ComPtr<T>` template: `Microsoft::WRL`-style `put()/get()/as()/copy_from`; `AddRef/Release`, `QueryInterface` helper macro `IID_PPV_ARGS` |
| `winrt::init_apartment` / `uninit_apartment` | `bar.cpp:1978, 1990` | `CoInitializeEx(nullptr, COINIT_MULTITHREADED)` on worker threads, `COINIT_APARTMENTTHREADED` on the UI thread; balanced `CoUninitialize` |
| `winrt::check_hresult` | everywhere | `if (FAILED(hr))` + log + return |
| `winrt::Windows::Data::Json` | `settings.cpp` | no JSON lib in MinGW by default; write a ~100-line parser or vendor `cJSON`/`nlohmann` (header-only works with GCC 16) |
| `Windows.Graphics.Capture` desktop-window mode for widgets | `desktop_backdrop.cpp` | **drop**; SuperMenu widgets are out of scope for v1 |
| `IDWriteFactory3`/`CreateFontSetBuilder` for MiSans | `ui_font.cpp:39-90` | verified present in MinGW headers: `IDWriteFactory3/5`, `CreateFontSetBuilder`, `IDWriteFontSetBuilder1` (`dwrite_3.h`), `IDWriteTypography` (`dwrite_2.h`), `DWRITE_FONT_FEATURE_TAG_TABULAR_FIGURES` (`dwrite.h`) — the private-font path compiles. Fallback: `AddFontResourceExW(path, FR_PRIVATE, 0)` + `CreateTextFormat(L"MiSans", …)` (test that DirectWrite sees it) or `Segoe UI Variable Text`/`Segoe UI` |
| `DCompositionWaitForCompositorClock` | `bar.cpp:35-38` | `GetProcAddress(LoadLibraryW(L"dcomp.dll"), "DCompositionWaitForCompositorClock")` (not in MinGW headers; exported on Win11, absent pre-22000 → fallback timer wait) |
| `SHAppBarMessage` / `RegisterWindowMessage` / WinEvent hooks | `bar.cpp:390-399` | plain user32 — fine |

---

## 5. Continuous-corner math (G3)

Sources: `corner_profile.h:1-58`, `continuous_corners.h:1-168`, `docs/DESIGN.md:92-109`.

### 5.1 Formula and constants

For a rectangle of half-extents `b = (w/2, h/2)` and corner radius `R`:

```
R = clamp(radius, 0, min(b.x, b.y))                       // px
L = min(1.6*R, min(b.x, b.y))                            // transition reach ("L" = profile.reach)
v = 1 - (1 - 1/√2) * R/L = 1 - 0.2928932188134525*R/L
a = max(0.02, (2v⁴ - v²)/(1 - v²))                       // a > 0 whenever straight joins exist
f(t) = t⁴ / (a + (1-a)t²)                                 // t ∈ [0,∞)
F(x,y) = f(x/L) + f(y/L) - 1                             // implicit curve, quadrant-local
```

`f(0)=f'(0)=f''(0)=f'''(0)=0` and `f'(1)=2(1+a)>0`, so at the join with the straight side the curve
has `κ=0`, `dκ/ds=0` — G3 (proof in `docs/DESIGN.md:96`). Special cases (`corner_profile.h:14-28`):

| case | values |
|---|---|
| square rect (`R=0`) | `L=0`; the shader branches to the box SDF (`max(q)-L`) and never uses `a`; the CPU helper `CornerFor` returns `{reach=0, a=0.02}` |
| pill (`R = shortside/2`, not a circle) | `L = min(1.6R, shortside/2) = shortside/2`, `v = 0.7071…`, `a → 0` clamped to `0.02`; e.g. a 60 px-high bar with R30 gets `a=0.02` |
| whole circle (`w==h && R==w/2`) | `L=w/2`, `a=0` exactly — a circle, no straight joins |
| `0 < R < shortside/2` | both `L` and `a` as above; the visible "squircle" corner. E.g. 32 px bar with R10: `b.y=16`, `L=min(16,16)=16`, `v=0.81694`, `a=0.672`; a 28 px bar with R10: `L=14`, `v=0.79079`, `a=0.418` |

### 5.2 Shader coverage vs D2D coverage

- **In the pixel shader** (glass): exact implicit distance `cornerEdge` for coverage
  (`cov = saturate(0.5 - edgeDistance/aa)`, `aa = fwidth(edgeDistance)`), and the Newton metric
  distance `sdCorner` only for shading/refraction (§2.3–2.6).
- **In D2D** (hover plates, popup content fills, plain fallback panels, input region):
  `continuous_corners.h`:
  - `CornerCoverage(x,y,w,h,p)` = `clamp(0.5 - CornerEdge(q), 0, 1)` with
    `q = (|x-w/2| - w/2 + reach, |y-h/2| - h/2 + reach)` (`corner_profile.h:52-57`).
  - `ContinuousMask(target,w,h,radius,stroke)`: rasterize that coverage into an **A8** bitmap of size
    `2*(ceil(max(reach, stroke)) + 1) + 1` (a corner stamp with a stretchable centre), subtract the
    inner coverage when `stroke > 0`, cache by `(outer.reach, outer.a, inner.reach, inner.a, stroke)`
    per D2D resource domain, 64 entries / 4 MiB budget (`continuous_corners.h:57-96`).
  - `PaintContinuousRect`: **9-slice** `FillOpacityMask` over a 3×3 grid split at
    `cap = max(reach, stroke)`, destination and source split identically; set
    `D2D1_ANTIALIAS_MODE_ALIASED` while blitting the mask (`:98-129`).
  - `ContinuousCornerPoint(theta, a)`: 8 Newton iterations → the outline sample used for
    `ContinuousRegion(rect, radius)` = `CreatePolygonRgn` of 4×65 points (WINDING) for the **input
    region / hit test** (`:131-167`). Rounding to integer pixels is fine for input, never for drawing.
  - Callout pointer (dock label): `callout_profile.h:1-30` — body = `CornerEdge`, tail = min distance
    to a 3-segment triangle with an inside test; `super.min(body, tail)`.
- Do **not** replace the analytic outline with a fitted cubic/arc path (design contract,
  `docs/DESIGN.md:94-100`): 129-sample parameter checks exist in `native/tests`.

---

## 6. Adaptation plan: top menu bar + dropdowns

### 6.1 Geometry, window and ROI

- One bar window per monitor (v1: `settings.monitor`, default 0): `x = rcMonitor.left`,
  `y = rcMonitor.top`, `w = rcMonitor.width`, `h = barHeight*scale + inset` (the extra is shadow
  room **below** the shape; the bar is flush with the top screen edge).
- Shape: `shapeL = -1`, `shapeT = -1`, `shapeR = w+1`, `shapeB = barHeight*scale` (overhang by 1 px
  so analytic coverage reaches 1.0 at the left/right screen edges instead of 0.5). For rounded
  **bottom** corners only, set `PartCount=1`, `PartRect[0]=shape`, `PartRadii[0]={0,0,r,r}`, `Melt=0`,
  `PartMix=0` — but note `sdRoundRect4` feeds each quadrant a different `R`, and `sdCorner` with
  `r<=0` returns a circular metric distance for the square corners (harmless: coverage still uses the
  exact `cornerEdge` box path; only rim shading near a square corner is slightly round). Simpler v1:
  one `Radius = 10–12` for all four corners — the top two are on the screen edge and invisible.
- **Clipped ROI** (the simplification that removes `ExtendBackdropEdges` + `backdrop_edge.hlsl`):
  compute the ROI as the intersection of `{win - pad … win + size + pad}` with the monitor rect, size
  the ROI texture to that intersection, and rely on the sampler's `CLAMP` addressing for the parts we
  cannot capture. Sampling above the screen's top row then returns row 0 (exactly the "extend the
  edge" behaviour) and mips are generated from already-clamped content. `RoiOrigin` becomes
  `(win.x - roi.x, win.y - roi.y)` (0 at the left/top screen edges, `pad` in the middle) — always
  read it from the actual clip, never assume `(pad,pad)`.
- `pad`: the only consumer is the blur gather radius (refraction samples inward and the shadow is
  inside the window's `inset`). `pad ≈ max(ceil(0.75*2^BlurLod) + 16, 16)`; Liquid@28 px needs ~10 px
  (round to 24), Frosted needs ~35 px (round to 48), Clear ~3 px (16). For a 1920 px bar with
  `h = barH+inset = 38` and pad 48: ROI = 1920×86 ≈ 0.66 MB, +mips ≈ 0.88 MB.
- DPI: per-monitor V2 (`SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)`,
  dynamically loaded if MinGW misses the symbol); `scale = GetDpiForWindow(hwnd)/96.0`; multiply
  every `GlassParams` length (`Radius`, `Edge`, `Refract`, `Inset`) by `scale`; positions are physical
  px; recompute on `WM_DPICHANGED`.
- Work area: if `settings.reserveWorkArea` (default true), register an appbar — `SHAppBarMessage(
  ABM_NEW,…)`, then `ABM_QUERYPOS` + `ABM_SETPOS` with `{mon.left, mon.top, mon.right,
  mon.top + barHeight}` — so maximized windows stop below the bar (`bar.cpp:1135-1139` for Atrium's
  equivalent). Handle `ABN_POSCHANGED` and re-set on DPI/display changes. With reservation off,
  maximized windows slide under the glass (nicer lensing, but bar text sits on window content).
- Auto-hide / hideInFullscreen: out of scope for v1 or implement as a `SetWindowPos` slide +
  `DWM`/WinEvent foreground check; Atrium's slide is `kSlideOutMs=300`, `kSlideInMs=420` with a small
  overshoot (`bar_painter.h:107-110`).

### 6.2 Where shadows/highlights go for a top bar

| element | setting |
|---|---|
| drop shadow | only the **bottom** edge and bottom corners are visible; shape shadow is offset **downward** by `Inset*0.25` in the shader (`glass.hlsl:224` = `Dist(px - (0, Inset*0.25))`), so the bar naturally casts onto the desktop below. Use `Inset=10–14 px`, `Shadow=0.30–0.35` |
| rim highlight | the visible lit edge is the bottom rim. LightDir `(-0.6,-0.8)` (Atrium) lights the **top-left** rim; for a top bar use `(-0.2,-0.98)` or `(0,-1)`: the bottom rim then gets the `back` term `Highlight*(0.22+0.35*back)≈0.57*Highlight`, a bright 1.6 px line, plus the sheen from the top edge |
| screen-edge treatment | left/right/top are flush: no visible border there because the shape overhangs by 1 px and the backdrop clamps to the screen edge; refraction at those edges reads clamped border content, which is visually "the desktop continuing past the bezel": subtle and acceptable. Optionally suppress the rim line on the three flush edges by passing `LightDir` mostly downward and keeping `Edge` small |
| glass over windows | with `reserveWorkArea=false`, maximized windows run under the bar and the live capture lenses them — this is the main "liquid" payoff; test both modes |

### 6.3 Popup windows (dropdown panels)

- Same `GlassRenderer` code; window = content rect + `inset` margin (12–16 px for shadow room);
  ROI = window + `pad`, clipped to monitor (sampler clamp at screen edges).
- Draw glass and D2D content into the same swap chain and `Present(1,0)` once: no second visual, no
  `WM_GLASS_READY`/`InkGrid` handshake. Fallback: if the first capture frame has not arrived 300 ms
  after showing (`surface.cpp:93`), draw a plain tinted panel (`PanelToneFor`, §7.4) and swap to glass
  when it arrives.
- Position: below the bar with a gap `gap = kPopupGapDip*scale` (Atrium 12 DIP, `surface.cpp:38,58-61`;
  for a 28–34 px top bar 6–10 DIP looks tighter/macOS-like), clamp into `rcWork`
  (`surface.cpp:31-64`); align Left/Right/Center to the bar's end or the work area.
- Open animation: DirectComposition scale+opacity on the visual (`PopVisual`,
  `glass_renderer.cpp:49-75`): open `scale 0.92 → 1.0` spring (12 cubic segments over 0.6 s, spring
  `motion::kPop`) + opacity `0→1` ease-out 160 ms (`AddCubic(begin,from,3d/t,-3d/t²,d/t³)`);
  close `scale 1→0.95` 130 ms, opacity `1→0` 120 ms; `dcomp->Commit()` after. v1 can just show/hide.
- Popups must also be excluded from DDA (§4.3), and popup glass must not overlap the bar's ROI with
  visible popup pixels — exclusion handles this; do not rely on z-order.

### 6.4 What to simplify (keep the look)

| Atrium feature | v1 decision |
|---|---|
| melting parts (`PartRect/PartRadii/Melt/PartMix`), dock tail (`Tail`), hover wave (`Wave`), ripple (`Ripple`) | **drop** (send 0); keep `Fade` for menu close dissolve |
| `PSBasic` fast path | optional; bar is thin so the savings are tiny — compile `PS` only |
| multiple glass shapes per window (vector<GlassParams>) | one shape per window; loop still useful if a popup needs a detached card later |
| `fromArea` matrix (`EdgeFrame`, 4 bar edges, vertical bars) | **identity**; keep the cbuffer fields (pass 0,0,1,1 and 0,0) |
| `ExtendBackdropEdges` + `backdrop_edge.hlsl` | **delete** (clipped ROI, §6.1) |
| desktop widgets, Stage Manager, remote-control mode, multiple displays with per-display bars | out of scope |
| `alignedBackdrop` 64-px ROI phase alignment | drop (fixed bar; ROI only changes on resize) |
| WGC fallback capture | drop (§4.5) |
| compare tile heatmap | drop; keep the single-counter changed-pixel test (§3.10) |
| ink region grid for the bar (2 regions: clock/apps) | keep, it is cheap (§6.6); popups can compute their own cell grid from their own readback |

### 6.5 Performance budget (target: 60 Hz, no work when idle)

| item | cost |
|---|---|
| ROI copy 1920×86 BGRA | 0.66 MB GPU copy, ~0.05 ms |
| `GenerateMips` on 1920×86 | ~0.88 MB of filtered writes, ~0.1 ms, **only when the backdrop changed** |
| full-screen triangle 1920×32 = 61 k px, ≤ 8 texture taps + rim math | ~0.1–0.2 ms GPU |
| compare CS 1920×86 = 165 k px | ~0.05 ms |
| D2D overlay (cached layer, only redrawn on text/ink/animation change) | < 0.5 ms when redrawn, 0 when cached |
| present-on-demand | 0 presents while the desktop is static under the bar (only every 2 s stale frame) |
| own GPU memory | 2 swap buffers (1920×32×4 ≈ 0.25 MB each) + ROI with mips ≈ 0.88 MB + presented copy 0.66 MB + D2D layer 0.25 MB ≈ **2.3 MB**; the DDA frame is monitor-sized (25.6 MB at 3200×2000) and owned by the duplication |
| CPU per present | ~1–3 ms (copy + mips + compare + D2D); measure with `QueryPerformanceCounter` and log p50/p95 like `bar.cpp:2503-2521` |

### 6.6 Text/ink adaptation (luminance classification)

Ink is chosen per region from the backdrop luminance (no shader support needed; `compare.hlsl` is not
an edge detector — it is the change detector; the luma grid comes from a mip readback):

1. Read back the ROI mip whose width is ≤ 96 texels (`glass_renderer.cpp:215-227`), convert BGRA to
   luma `0.0722*B + 0.7152*G + 0.2126*R` (`:409-410`) — **sRGB-encoded weights**, same space as the
   shader's `luma` dot; do not linearise.
2. `RegionLuma(x0,y0,x1,y1)` averages the cells under a text rect (`:417-429`); bar passes
   `rb.origin + rect` (`bar.cpp:2694-2704`).
3. Mix with the tint exactly as the shader does: `lum = backdrop*(1-tintMix) + tintLuma*tintMix`
   (`bar.cpp:207-212`).
4. Hysteresis: dark ink when `lum > 0.50`, light ink when `lum < 0.42` (`bar.cpp:137`);
   fade between them over `kInkFadeMs = 450` with an ease-out cubic (`glass_panel.h:96-107`,
   `glass_panel.cpp:28-47`).
5. Ink colours: light = white 96 %; dark = near-black (Atrium `1-0.93*dark, 1-0.93*dark,
   1-0.91*dark, 0.96-0.04*dark`, `bar_painter.cpp:29`); halo = inverse ink at 32–35 % alpha drawn
   0.6 px below (`bar_painter.cpp:395, 458-466`); hover wash = black 10 % / white 20 %
   (`bar_painter.cpp:396`); text antialias grayscale.
6. Bar regions: clock + right cluster use the clock's luma, app strip uses the apps' luma
   (`bar.cpp:2705-2712`). Text metrics from `bar.cpp:1041-1065`: clock time `0.25*barH` Semibold,
   date `0.19*barH` Normal, plate `≈1.55*icon`, plate corner radius concentric
   `clamp(barRadius - (barH-plate)/2, 3, plate/2)` (`bar.cpp:1111,1177`); separator height
   `0.46*barH` (`bar_painter.cpp:429-430`).

---

## 7. Concrete defaults

### 7.1 Atrium presets (source of truth)

`settings/Atrium.Settings/BarSettings.cs:111-124`, `native/core/settings.h:21-29`,
`docs/DESIGN.md:245`; mapping to shader params from `bar.cpp:139-161`.

| shader param | Liquid | Frosted | derivation |
|---|---|---|---|
| `transparency` | 0.95 | 0.80 | setting |
| `TintMix` | 0.05 | 0.20 | `1 - transparency` |
| `tint` | `#FFFFFF` | `#F5F5FA` | setting |
| `blur` | 0.50 | 1.0 | setting |
| `BlurMix` | 1.0 | 1.0 | `min(1, blur*4)` |
| `BlurLod` | 3.25 | 5.5 | `1 + 4.5*blur` |
| `Refract` (px @1×) | 30 | 6 | setting × scale |
| `Edge` (px @1×) | 24 | 10 | setting × scale |
| `Dispersion` | 0.30 | 0.04 | setting |
| `Highlight` | 0.80 | 0.45 | setting |
| `Shadow` | 0.30 | 0.30 | setting |
| `Inset` | 14 px | 14 px | `kInsetDip = 14` (`bar.cpp:59`), × scale |
| `LightDir` | (-0.6,-0.8) | (-0.6,-0.8) | hardcoded (`bar.cpp:157-158`) |
| `Radius` | 30 | 30 | `cornerRadius` (bar height 60) |
| bar height / margin | 60 / 10 DIP | 60 / 10 DIP | settings |

Clamp ranges (`settings.cpp:27-49`): transparency 0–1, blur 0–1, refraction 0–60, edge 1–80,
dispersion 0–0.5, highlight 0–1, shadow 0–1, cornerRadius 0–200, height 32–160, tint `& 0xFFFFFF`.

### 7.2 SuperMenu presets (authoritative) → shader values for a 28–34 px top bar

The settings app already defines the three presets (`settings/SuperMenu.Settings/Models/AppSettings.cs:253-291`,
mirrored in `docs/settings-schema.md:40-50`); those values are authoritative. They are Atrium-60 px-bar
numbers and **must be clamped** for a 28 px bar: `edge 18` would make both rim bands meet across the
whole bar, and `refraction 26` (Clear) would make the lens sample almost the whole bar height. Upload
the derived values below (28 px reference; `scale` = DPI scale; `barH = barHeight*scale`).

| setting (DIP) | Liquid | Frosted | Clear |
|---|---|---|---|
| `transparency` | 0.94 | 0.82 | 0.99 |
| `blur` | 0.52 | 0.92 | 0.10 |
| `refraction` | 18 | 6 | 26 |
| `edge` | 18 | 12 | 22 |
| `dispersion` | 0.30 | 0.12 | 0.55 |
| `highlight` | 0.80 | 0.55 | 1.10 |
| `shadow` | 0.35 | 0.30 | 0.22 |
| `saturation` | 1.15 | 1.05 | 1.30 |
| `tint` | #FFFFFF | #FFFFFF | #FFFFFF |

Derived `GlassParams` after clamping — the values to actually upload:

| param | Liquid | Frosted | Clear |
|---|---|---|---|
| `TintMix` = 1-transparency | 0.06 | 0.18 | 0.01 |
| `BlurMix` = min(1, 4·blur) | 1.0 | 1.0 | 0.40 |
| `BlurLod` = 1 + 4.5·blur (then ROI cap, §8 item 10) | 3.34 | 5.14 → cap 3.5–4 | 1.45 |
| `Refract` = min(refraction, 0.6·barH)·scale | 16.8 | 6 | 16.8 |
| `Edge` = clamp(edge, 1, 0.45·barH)·scale | 12.6 | 12 | 12.6 |
| `Dispersion` (shader allows 0–0.95) | 0.30 | 0.12 | 0.55 |
| `Highlight` | 0.80 | 0.55 | 1.10 |
| `Shadow` | 0.35 | 0.30 | 0.22 |
| `vibrancy` = saturation | 1.15 | 1.05 | 1.30 |
| `Radius` (px, all corners) | 10 | 10 | 10 |
| `Inset` (shadow room, px) | 10 | 10 | 10 |
| `LightDir` | (-0.20, -0.98) | (-0.20, -0.98) | (-0.20, -0.98) |
| ROI `pad` (px) | 24 | 48 | 16 |
| popup `Radius` / `Inset` (px) | 14 / 12–16 | 14 / 12–16 | 14 / 12–16 |

If the settings app's schema may still be edited: Liquid at a 28 px bar reads better with
`edge = 12, refraction = 12`; Clear with `edge = 12, refraction = 14`; Frosted's `edge = 12` is
already right. Atrium's own clamps (`refraction 0–60`, `edge 1–80`, `settings.cpp:36-37`) assume a
60 px capsule and must not be reused unchanged.

### 7.3 SuperMenu settings → shader mapping (with clamps)

| settings key | target | mapping / clamp |
|---|---|---|
| `glassPreset` liquid/frosted/clear | informational | selects the table above; individual keys still apply (as in Atrium) |
| `transparency` 0–1, default .94 | `TintMix` | `1 - transparency`, clamp 0–1 |
| `blur` 0–1, default .52 | `BlurMix`, `BlurLod` | `BlurMix = min(1,4*blur)`; `BlurLod = 1+4.5*blur`, then `min(BlurLod, log2(roiH)-3)` and ≥ 1 |
| `refraction` 0–40, default 18 | `Refract` | `min(refraction, 0.6*barH)*scale` (28 px bar: ≤16.8; presets 18/6/26 → 16.8/6/16.8) |
| `edge` 0–32, default 18 | `Edge` | `clamp(edge, 1, 0.45*barH)*scale` (28 px bar: ≤12.6; presets 18/12/22 → 12.6/12/12.6) |
| `dispersion` 0–1, default .30 | `Dispersion` | clamp 0–0.95 so `1±D` stays positive (Atrium clamps 0–0.5, but SuperMenu's Clear preset uses 0.55 — the shader handles it) |
| `highlight` 0–1.5, default .80 | `Highlight` | clamp 0–1.3; >1 overbrightens/rings |
| `shadow` 0–1, default .35 | `Shadow` | clamp 0–1 |
| `tint` `#RRGGBB` | `Tint.rgb` | bytes/255 |
| `saturation` .5–2, default 1.15 | vibrancy | replace hardcoded `1+0.22*Highlight` with `saturation`; keep `(1+0.04*Highlight)` brightness. Presets 1.15/1.05/1.30; Atrium's 1.22*0.8 ≈ 1.18 |
| `noise` 0–.1, default .015 | optional dither | `col += (hash(px)-0.5) * noise` before output (±noise/2 in 0–1 units ≈ ±2 8-bit levels at .015) to hide mip/tint banding; Atrium renders none (reserved) |
| `theme` auto/light/dark | fallback panel + ink default | only used when glass is unavailable / textColor forced |
| `fontScale` | D2D text px | multiply clock/date text sizes |
| `monitor` 0–15 | window + capture monitor | clamp to connected monitors |
| `reserveWorkArea` | appbar | §6.1 |

### 7.4 Plain-panel fallback (when glass is not ready/unavailable)

Atrium draws a solid panel for the first 300 ms (`surface.cpp:93`) and its tone is
`PanelToneFor(tint, transparency)` (`glass_panel.cpp:110-123`): if `1-transparency ≥ 0.45` use the
user's tint and pick light/dark by its luma (threshold 0.55); otherwise follow the Windows app theme
(light `0xF7F7FA`, dark `0x1D1E23`). The bar keeps drawing text over it with the last ink.

---

## 8. Gotchas (things that will bite)

1. **Premultiplied alpha everywhere.** Shader output is premultiplied (`rgb*cov, cov`); swap chain
   `DXGI_ALPHA_MODE_PREMULTIPLIED`; D2D target `D2D1_ALPHA_MODE_PREMULTIPLIED`; clear
   `{0,0,0,0}`; blend `ONE / INV_SRC_ALPHA`. Mixing straight-alpha D2D bitmaps (icons) into a
   premultiplied stack gives dark fringes — convert icon bitmaps to `GUID_WICPixelFormat32bppPBGRA`
   or use `D2D1_ALPHA_MODE_PREMULTIPLIED` for every `CreateBitmap`.
2. **BGRA support**: `D3D11_CREATE_DEVICE_BGRA_SUPPORT` is required for `B8G8R8A8_UNORM` +
   D2D interop; don't use `_SRGB` formats anywhere (`B8G8R8A8_UNORM` only). Auto-mip generation on
   BGRA works but check `D3D11_FORMAT_SUPPORT_MIP_AUTOGEN` and fail to the sharp path if absent.
3. **sRGB / "linear" trap.** All math (blur, luma, dispersion, tint) happens on sRGB-encoded values
   sampled from a UNORM texture; the shader deliberately does not linearise. Using `_SRGB` views or
   linearising for luma will shift every threshold and the Liquid look. Luma weights in both shader
   and readback must use the same space (`.2126/.7152/.0722`).
4. **DXGI composition flags**: `FLIP_SEQUENTIAL` + `PREMULTIPLIED` + `FRAME_LATENCY_WAITABLE_OBJECT`,
   `SetMaximumFrameLatency(1)`. `ResizeBuffers` must pass the **same flag**; release/unbind
   RTV + D2D target before it. `WS_EX_NOREDIRECTIONBITMAP` on the window or DWM shows a black
   backing layer. Never `Present` while the waitable object hasn't been signalled (`Ready()` gate).
5. **DPI**: per-monitor V2; every `GlassParams` length × `scale`; window/capture in physical px;
   re-create ROI on `WM_DPICHANGED`; popup sizes in DIP; cached D2D text formats keyed by px·4
   (`bar_painter.cpp:67`) so fractional scales don't thrash formats.
6. **Per-monitor capture**: capture exactly the monitor the bar is on (`DXGI_OUTPUT_DESC.Monitor ==
   HMONITOR`), create the device on that output's adapter (hybrid GPU: enumerate adapters, and
   optionally write `HKCU\Software\Microsoft\DirectX\UserGpuPreferences` `GpuPreference=1;` for the
   exe — `capture.cpp:37-50`). Multi-monitor bars = one renderer + one capture per monitor, but one
   duplication per output only.
7. **MinGW specifics** (verified): headers/import libs exist (table §0); `__uuidof(T)` is emulated —
   pass types not expressions; no `wrl/com_ptr`; use `-municode` for `wWinMain`; `PATH` must contain
   `C:\msys64\ucrt64\bin` when building (cc1plus DLLs); **link with `-static`** (the team's toolchain
   probe: 23 import DLLs, no MinGW runtime DLLs; `-static-libgcc -static-libstdc++` still leaves
   `libwinpthread-1.dll`); `DCompositionWaitForCompositorClock` missing from headers but
   exported by `dcomp.dll` on Win11 (dynamic); `D3DCompile` links via `-ld3dcompiler` to
   `d3dcompiler_47.dll` (present in System32 on Win10/11); **no `fxc`/`dxc` needed**.
8. **C++/WinRT replacements** — see the table in §4.5; the only real loss is WGC, and DDA + wallpaper
   covers it.
9. **Never capture your own windows.** `WCA_EXCLUDED_FROM_DDA` (attribute 24) on the bar **and**
   every popup, applied before the first present; fallback `WDA_EXCLUDEFROMCAPTURE`; if neither
   works, show the wallpaper backdrop instead of live DDA. Re-apply after unlock/display change, and
   never show live glass while any own window is capturable. Without this you get an infinite
   feedback ring; the ROI comparison ("echo skip") is not a fix, only an optimisation. Also note the
   first duplicated frame after opening the duplication may already contain the bar if exclusion was
   set too late — set it first, then duplicate.
10. **Mip regeneration cost / LOD sanity**: `GenerateMips` on every backdrop change is fine, but do
    it once per frame (coalesce with the compare submission, `glass_renderer.cpp:354-384`), never per
    surface per frame. `BlurLod` must be ≤ mip count; for a *short* bar ROI (height ≈ 86 px) the
    vertical mip chain collapses fast — cap `BlurLod ≈ log2(roiH)-3 ≈ 3.4` for the bar, otherwise
    frosted sampling lands in a 3-row mip and the vertical detail is a single average. Keep
    `pad ≈ max(ceil(0.75·2^BlurLod)+16, 16)`; taps past the ROI edge are sampler-clamped.
11. **DComp layering & lifetime**: one `IDCompositionTarget` per window (topmost flag selects above/
    below content); `Commit()` after `SetRoot`/`SetContent`/transform/effect changes; recreate the
    DComp device with the D3D device; close the frame-latency waitable handle on teardown.
12. **D2D interop order**: unbind the RTV/SRVs before `d2d->SetTarget`; same `ID3D11Device`;
    `CANNOT_DRAW` on the target bitmap; balanced `BeginDraw`/`EndDraw`; treat `D2DERR_RECREATE_TARGET`
    as device loss; grayscale text AA.
13. **Capture frame lifetime / threading**: exactly one acquired frame; `ReleaseFrame` after the
    copies; call `context->Flush()` before `ReleaseFrame` when copies are submitted from another
    thread; never wait inside `AcquireNextFrame` while holding the shared device lock (pump with
    timeout 0 + event). If sharing the immediate context across threads, set
    `ID3D11Multithread::SetMultithreadProtected(TRUE)`.
14. **Failure modes are normal**: secure desktop (`E_ACCESSDENIED`), lock/unlock
    (`DXGI_ERROR_ACCESS_LOST`), driver resets, sleep, display mode changes, HDR, rotated monitors,
    full-screen-exclusive apps, another duplicator. Treat all as "recreate capture, show
    wallpaper/tint until back". A black capture over a lit screen is a known driver bug after
    sleep — Atrium detects it with a GDI sanity sample (`bar.cpp:1993-2026`) and restarts duplication
    or goes wallpaper.
15. **Shape/AA edge cases**: coverage at the exact outline is 0.5 → overhang the shape 1 px past the
    window at screen edges; `TintMix`/`transparency` are not alpha; `Dispersion ≥ 1` makes the blue
    channel sample backwards (`1-D < 0`) — SuperMenu's Clear preset 0.55 is fine, clamp 0–0.95;
    `Refract`/`Edge` configured for a 60 px bar overshoot a 28 px bar (clamp!); the
    shader's `Radius` is clamped to `min(b.x,b.y)` so a 28 px bar with R30 becomes a pill.
16. **cbuffer packing**: 400 bytes exactly; keep the host struct all-float and add
    `static_assert(sizeof(GlassParams)==400)`; `D2D1_MATRIX_3X2_F` is 6 floats and maps to
    `FromArea.xyzw + FromAreaOffset.xy`; upload per shape *before* `Draw(3,0)`.
17. **Fonts/icons**: ship MiSans or fall back to `Segoe UI Variable Text`; icon glyphs
    `Segoe Fluent Icons` → `Segoe MDL2 Assets`; tabular figures need `IDWriteTypography`
    (`ui_font.cpp:16-37, 94-104`); win32 text is DIP-scaled.
18. **Settings clamps**: Atrium's ranges assume a 60 px bar (`edge` 1–80, `refraction` 0–60,
    `height` 32–160). SuperMenu's schema allows `edge` 18 / `refraction` 18 on a 28 px bar — clamp
    at the point of upload as in §7.3; a settings file must never crash the bar
    (`docs/settings-schema.md:12`).
19. **Testing hooks** worth porting: p50/p95 frame timing + present/skip counters (`bar.cpp:2503-2521`),
    `--debug-capturable` frozen-frame mode, logging every capture restart with the HRESULT.
20. **Verification (acceptance)**: static desktop ⇒ 0 presents after settle; a window animating far
    from the bar ⇒ still 0 presents (compare works); video *under* the bar ⇒ ≤ display-rate presents;
    white window under the bar ⇒ ink flips dark within 450 ms; UAC/lock/unlock ⇒ glass recovers;
    unplug/replug monitor + DPI change ⇒ correct size/scale; Snipping Tool screenshot shows the bar
    with the WCA path (and hides it with the WDA fallback); no feedback ring when a popup overlaps
    the bar; GPU reset ⇒ rebuild; 30 min run ⇒ private bytes flat.

---

## 9. File/line index (reference repo)

| topic | file:line |
|---|---|
| material shader | `native/core/render/shaders/glass.hlsl:1-311` |
| cbuffer host mirror | `native/core/render/glass_renderer.h:21-43` |
| corner family (CPU) | `native/core/render/corner_profile.h:1-58` |
| corner masks / 9-slice / HRGN | `native/core/render/continuous_corners.h:1-168` |
| callout tail | `native/core/render/callout_profile.h:1-30` |
| renderer ctor / states / swapchain / DComp / D2D | `native/core/render/glass_renderer.cpp:79-164` |
| size-dependent resources / resize | `native/core/render/glass_renderer.cpp:188-258` |
| ROI copy | `native/core/render/glass_renderer.cpp:260-284` |
| edge replication (dropped) | `native/core/render/glass_renderer.cpp:286-344` |
| mips / compare / readback / poll | `native/core/render/glass_renderer.cpp:346-448` |
| present (glass + D2D + present) | `native/core/render/glass_renderer.cpp:450-553` |
| compare shader | `native/core/render/shaders/compare.hlsl:1-47` |
| edge shader (dropped) | `native/core/render/shaders/backdrop_edge.hlsl:1-14` |
| GPU/adapter selection | `native/core/render/gpu.cpp:11-117` |
| DDA capture + exclusion | `native/core/render/capture.cpp:145-301`, `:18-70` |
| wallpaper fallback | `native/core/render/desktop_backdrop.cpp:44-99, 406-533` |
| params mapping / bar ink / render loop | `native/core/bar.cpp:139-212, 1030-1140, 1976-2090, 2296-2467, 2469-2772` |
| bar content drawing metrics | `native/core/render/bar_painter.cpp:29, 328-384, 386-626` |
| popup layer protocol | `native/core/render/glass_panel.h:1-127`, `native/core/ui/surface.h:1-8, 154-306` |
| popup swapchain/D2D | `native/core/ui/surface.cpp:250-277, 776-856` |
| presets | `native/core/settings.h:21-46`, `settings/Atrium.Settings/BarSettings.cs:111-124`, `docs/DESIGN.md:245` |
| G3 design contract/proof | `docs/DESIGN.md:92-109` |
| shader build (fxc, profiles) | `native/core/CMakeLists.txt:3-27`; link libs `:97-100` |
| settings clamp/JSON | `native/core/settings.cpp:27-91` |

Target-repo (SuperMenu) files referenced:

| topic | file:line |
|---|---|
| settings contract (all keys/ranges) | `docs/settings-schema.md:15-52` |
| authoritative glass presets (settings app) | `settings/SuperMenu.Settings/Models/AppSettings.cs:253-291` |
| MinGW toolchain proof (3 link modes, DLL lists) | `docs/dev/toolchain-check/logs/summary.txt`, `build.ps1:8-15`, `mingw-ucrt64.cmake:1-23` |
