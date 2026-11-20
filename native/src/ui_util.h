#pragma once

#include "common.h"
#include "gpu.h"

#include <d2d1_3.h>
#include <dwrite_3.h>

#include <map>
#include <string>

namespace sm {

struct Ink {
    // 0 = white ink for a dark backdrop, 1 = near-black ink for a light backdrop.
    float dark = 0;

    D2D1_COLOR_F text(float alpha = 1.0f) const;
    D2D1_COLOR_F secondary(float alpha = 1.0f) const;
    D2D1_COLOR_F halo() const;
    D2D1_COLOR_F wash(float extra = 0) const;
    bool isDark() const { return dark > 0.5f; }
};

D2D1_COLOR_F MixColor(D2D1_COLOR_F a, D2D1_COLOR_F b, float t);
D2D1_COLOR_F ColorFromHex(unsigned rgb, float a = 1);
D2D1_COLOR_F AccentColor();

class Ui {
public:
    explicit Ui(Gpu* gpu);

    IDWriteTextFormat* Format(float px, DWRITE_FONT_WEIGHT weight);
    IDWriteTextFormat* IconFormat(float px);
    // Draws one Segoe Fluent Icons glyph, centered in `rect`.
    void DrawGlyph(ID2D1DeviceContext2* d2d, wchar_t glyph, float px, const D2D1_RECT_F& rect, ID2D1Brush* brush);
    IDWriteTextLayout* CreateLayout(const std::wstring& text, float px, DWRITE_FONT_WEIGHT weight, float maxWidth,
                                    float maxHeight, DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING,
                                    DWRITE_PARAGRAPH_ALIGNMENT paragraph = DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    void DrawText(ID2D1DeviceContext2* d2d, const std::wstring& text, float px, DWRITE_FONT_WEIGHT weight,
                  const D2D1_RECT_F& rect, ID2D1Brush* brush,
                  DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING,
                  DWRITE_PARAGRAPH_ALIGNMENT paragraph = DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
                  D2D1_DRAW_TEXT_OPTIONS options = D2D1_DRAW_TEXT_OPTIONS_NONE);
    void DrawTextWithHalo(ID2D1DeviceContext2* d2d, const std::wstring& text, float px, DWRITE_FONT_WEIGHT weight,
                          const D2D1_RECT_F& rect, const Ink& ink,
                          DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING,
                          DWRITE_PARAGRAPH_ALIGNMENT paragraph = DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    float Measure(const std::wstring& text, float px, DWRITE_FONT_WEIGHT weight);
    ID2D1SolidColorBrush* Brush(ID2D1DeviceContext2* d2d, const D2D1_COLOR_F& c);
    ID2D1StrokeStyle* RoundStroke() const { return stroke_.get(); }

    DWRITE_FONT_WEIGHT UiWeight(DWRITE_FONT_WEIGHT weight) const;

private:
    struct Key {
        int px4 = 0;
        int weight = 0;
        bool operator<(const Key& o) const { return px4 != o.px4 ? px4 < o.px4 : weight < o.weight; }
    };
    Gpu* gpu_ = nullptr;
    std::map<Key, ComPtr<IDWriteTextFormat>> formats_;
    std::map<int, ComPtr<IDWriteTextFormat>> iconFormats_;
    ComPtr<ID2D1StrokeStyle> stroke_;
    std::wstring family_;
    std::wstring iconFamily_;
};

// A small animation helper that eases a 0..1 value.
struct HoverValue {
    float value = 0;
    void Approach(float target, double nowMs, float rate);
    void Set(float v) { value = v; }
};

}  // namespace sm
