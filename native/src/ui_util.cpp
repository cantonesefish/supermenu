#include "ui_util.h"

#include <algorithm>
#include <cmath>

namespace sm {

D2D1_COLOR_F Ink::text(float alpha) const {
    const float v = 1.0f - 0.93f * dark;
    return D2D1::ColorF(v, v, 1.0f - 0.91f * dark, (0.96f - 0.04f * dark) * alpha);
}
D2D1_COLOR_F Ink::secondary(float alpha) const {
    const float v = 1.0f - 0.80f * dark;
    return D2D1::ColorF(v, v, 1.0f - 0.78f * dark, (0.74f - 0.06f * dark) * alpha);
}
D2D1_COLOR_F Ink::halo() const {
    return dark > 0.5f ? D2D1::ColorF(1, 1, 1, 0.35f) : D2D1::ColorF(0, 0, 0, 0.32f);
}
D2D1_COLOR_F Ink::wash(float extra) const {
    if (dark > 0.5f) return D2D1::ColorF(0, 0, 0, 0.10f + extra);
    return D2D1::ColorF(1, 1, 1, 0.20f + extra);
}

D2D1_COLOR_F MixColor(D2D1_COLOR_F a, D2D1_COLOR_F b, float t) {
    t = sm::Clamp(t, 0.0f, 1.0f);
    return D2D1::ColorF(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t,
                        a.a + (b.a - a.a) * t);
}

D2D1_COLOR_F ColorFromHex(unsigned rgb, float a) {
    return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, a);
}

D2D1_COLOR_F AccentColor() { return ColorFromHex(0x0A84FF); }

void HoverValue::Approach(float target, double nowMs, float rate) {
    static thread_local double last = 0;
    double dt = last > 0 ? (nowMs - last) / 1000.0 : 1.0 / 60.0;
    last = nowMs;
    dt = std::clamp(dt, 0.0, 0.1);
    value = sm::Approach(value, target, rate, static_cast<float>(dt));
    if (std::abs(value - target) < 0.002f) value = target;
}

Ui::Ui(Gpu* gpu) : gpu_(gpu) {
    family_ = gpu_->hasUiFont() ? gpu_->fontFamily() : L"Segoe UI Variable Text";
    iconFamily_ = gpu_->iconFontFamily();
    // Shared round-capped stroke for hand-drawn icons.
    D2D1_STROKE_STYLE_PROPERTIES props{};
    props.startCap = D2D1_CAP_STYLE_ROUND;
    props.endCap = D2D1_CAP_STYLE_ROUND;
    props.dashCap = D2D1_CAP_STYLE_ROUND;
    props.lineJoin = D2D1_LINE_JOIN_ROUND;
    gpu_->d2dFactory()->CreateStrokeStyle(&props, nullptr, 0, stroke_.put());
}

DWRITE_FONT_WEIGHT Ui::UiWeight(DWRITE_FONT_WEIGHT weight) const {
    if (!gpu_ || !gpu_->hasUiFont()) return weight;
    if (weight <= 320) return DWRITE_FONT_WEIGHT_LIGHT;   // HarmonyOS Sans SC Light 300
    if (weight <= 400) return DWRITE_FONT_WEIGHT_NORMAL;  // Regular 400
    if (weight <= 480) return DWRITE_FONT_WEIGHT_MEDIUM;  // Medium 500
    return DWRITE_FONT_WEIGHT_BOLD;                       // Bold 700
}

IDWriteTextFormat* Ui::Format(float px, DWRITE_FONT_WEIGHT weight) {
    const int px4 = static_cast<int>(std::lround(px * 4.0f));
    const Key key{px4, static_cast<int>(weight)};
    auto it = formats_.find(key);
    if (it != formats_.end()) return it->second.get();
    ComPtr<IDWriteTextFormat> fmt;
    const DWRITE_FONT_WEIGHT mapped = UiWeight(weight);
    HRESULT hr = gpu_->dwrite()->CreateTextFormat(family_.c_str(), gpu_->uiFontCollection(), mapped,
                                                  DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, px,
                                                  L"zh-CN", fmt.put());
    if (FAILED(hr)) {
        hr = gpu_->dwrite()->CreateTextFormat(L"Segoe UI Variable Text", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                              DWRITE_FONT_STRETCH_NORMAL, px, L"zh-CN", fmt.put());
    }
    if (FAILED(hr)) return nullptr;
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    auto [ins, ok] = formats_.emplace(key, std::move(fmt));
    return ins->second.get();
}

IDWriteTextFormat* Ui::IconFormat(float px) {
    const int px4 = static_cast<int>(std::lround(px * 4.0f));
    auto it = iconFormats_.find(px4);
    if (it != iconFormats_.end()) return it->second.get();
    ComPtr<IDWriteTextFormat> fmt;
    HRESULT hr = gpu_->dwrite()->CreateTextFormat(iconFamily_.c_str(), gpu_->iconFontCollection(),
                                                  DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                                  DWRITE_FONT_STRETCH_NORMAL, px, L"zh-CN", fmt.put());
    if (FAILED(hr)) return nullptr;
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    auto [ins, ok] = iconFormats_.emplace(px4, std::move(fmt));
    return ins->second.get();
}

void Ui::DrawGlyph(ID2D1DeviceContext2* d2d, wchar_t glyph, float px, const D2D1_RECT_F& rect, ID2D1Brush* brush) {
    auto* format = IconFormat(px);
    if (!format || !brush) return;
    const wchar_t text[2] = {glyph, 0};
    d2d->DrawText(text, 1, format, rect, brush, D2D1_DRAW_TEXT_OPTIONS_NONE);
}

IDWriteTextLayout* Ui::CreateLayout(const std::wstring& text, float px, DWRITE_FONT_WEIGHT weight, float maxWidth,
                                    float maxHeight, DWRITE_TEXT_ALIGNMENT align,
                                    DWRITE_PARAGRAPH_ALIGNMENT paragraph) {
    auto* format = Format(px, weight);
    if (!format) return nullptr;
    format->SetTextAlignment(align);
    format->SetParagraphAlignment(paragraph);
    IDWriteTextLayout* layout = nullptr;
    if (FAILED(gpu_->dwrite()->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format, maxWidth,
                                                maxHeight, &layout))) {
        return nullptr;
    }
    if (gpu_->tabularTypography()) {
        layout->SetTypography(gpu_->tabularTypography(), DWRITE_TEXT_RANGE{0, static_cast<UINT32>(text.size())});
    }
    return layout;
}

void Ui::DrawText(ID2D1DeviceContext2* d2d, const std::wstring& text, float px, DWRITE_FONT_WEIGHT weight,
                  const D2D1_RECT_F& rect, ID2D1Brush* brush, DWRITE_TEXT_ALIGNMENT align,
                  DWRITE_PARAGRAPH_ALIGNMENT paragraph, D2D1_DRAW_TEXT_OPTIONS options) {
    if (text.empty() || !brush) return;
    ComPtr<IDWriteTextLayout> layout;
    layout.attach(CreateLayout(text, px, weight, rect.right - rect.left, rect.bottom - rect.top, align, paragraph));
    if (!layout) return;
    d2d->DrawTextLayout(D2D1::Point2F(rect.left, rect.top), layout.get(), brush, options);
}

void Ui::DrawTextWithHalo(ID2D1DeviceContext2* d2d, const std::wstring& text, float px, DWRITE_FONT_WEIGHT weight,
                          const D2D1_RECT_F& rect, const Ink& ink, DWRITE_TEXT_ALIGNMENT align,
                          DWRITE_PARAGRAPH_ALIGNMENT paragraph) {
    if (text.empty()) return;
    ComPtr<ID2D1SolidColorBrush> halo(Brush(d2d, ink.halo()));
    ComPtr<ID2D1SolidColorBrush> inkBrush(Brush(d2d, ink.text()));
    if (!halo || !inkBrush) return;
    const float o = std::max(0.6f, px * 0.05f);
    const D2D1_RECT_F haloRect{rect.left, rect.top + o, rect.right, rect.bottom + o};
    DrawText(d2d, text, px, weight, haloRect, halo.get(), align, paragraph);
    DrawText(d2d, text, px, weight, rect, inkBrush.get(), align, paragraph);
}

float Ui::Measure(const std::wstring& text, float px, DWRITE_FONT_WEIGHT weight) {
    IDWriteTextLayout* raw = CreateLayout(text, px, weight, 100000.0f, 1000.0f);
    if (!raw) return px * static_cast<float>(text.size()) * 0.6f;
    DWRITE_TEXT_METRICS metrics{};
    raw->GetMetrics(&metrics);
    const float width = metrics.widthIncludingTrailingWhitespace;
    raw->Release();
    return width;
}

ID2D1SolidColorBrush* Ui::Brush(ID2D1DeviceContext2* d2d, const D2D1_COLOR_F& c) {
    ID2D1SolidColorBrush* brush = nullptr;
    d2d->CreateSolidColorBrush(c, &brush);
    return brush;
}

}  // namespace sm
