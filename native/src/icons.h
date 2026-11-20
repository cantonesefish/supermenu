#pragma once

#include "common.h"
#include "ui_util.h"

#include <d2d1_3.h>

#include <map>
#include <string>

namespace sm {

// Vector glyphs drawn into a normalized 0..1 box. No icon font dependency.
class Icons {
public:
    Icons(Gpu* gpu, Ui* ui);
    void Apple(ID2D1DeviceContext2* d2d, const D2D1_RECT_F& r, ID2D1Brush* brush, float opacity = 1.0f);
    // Windows 11 logo from the Wikimedia Commons SVG (four panes, 507x528 gap).
    void Windows(ID2D1DeviceContext2* d2d, const D2D1_RECT_F& r, ID2D1Brush* brush);
    void Wifi(ID2D1DeviceContext2* d2d, const D2D1_RECT_F& r, ID2D1Brush* brush, int strength, bool off);
    void Battery(ID2D1DeviceContext2* d2d, const D2D1_RECT_F& r, ID2D1Brush* brush, int percent, bool charging,
                 bool low);
    void ControlCenter(ID2D1DeviceContext2* d2d, const D2D1_RECT_F& r, ID2D1Brush* brush);
    void Search(ID2D1DeviceContext2* d2d, const D2D1_RECT_F& r, ID2D1Brush* brush);
    void Volume(ID2D1DeviceContext2* d2d, const D2D1_RECT_F& r, ID2D1Brush* brush, int level, bool muted);
    void Bluetooth(ID2D1DeviceContext2* d2d, const D2D1_RECT_F& r, ID2D1Brush* brush);
    void Bell(ID2D1DeviceContext2* d2d, const D2D1_RECT_F& r, ID2D1Brush* brush);
    void Chevron(ID2D1DeviceContext2* d2d, float x, float y, float size, ID2D1Brush* brush, bool right);
    void Check(ID2D1DeviceContext2* d2d, const D2D1_RECT_F& r, ID2D1Brush* brush);
    void PlayPause(ID2D1DeviceContext2* d2d, const D2D1_RECT_F& r, ID2D1Brush* brush, bool playing);
    void Previous(ID2D1DeviceContext2* d2d, const D2D1_RECT_F& r, ID2D1Brush* brush);
    void Next(ID2D1DeviceContext2* d2d, const D2D1_RECT_F& r, ID2D1Brush* brush);
    void Sun(ID2D1DeviceContext2* d2d, const D2D1_RECT_F& r, ID2D1Brush* brush);
    void Speaker(ID2D1DeviceContext2* d2d, const D2D1_RECT_F& r, ID2D1Brush* brush);
    void BluetoothSettings(ID2D1DeviceContext2* d2d, const D2D1_RECT_F& r, ID2D1Brush* brush);
    void Globe(ID2D1DeviceContext2* d2d, const D2D1_RECT_F& r, ID2D1Brush* brush);
    // Open Symbols (SF Symbols-converted Lucide) glyphs: consistent Apple-style
    // silhouettes at a caller-controlled square size.
    void Sf(ID2D1DeviceContext2* d2d, const char* name, const D2D1_RECT_F& r, ID2D1Brush* brush);
    // Filled (solid) glyphs for Control Center: Remix Icon's fill set.
    void Filled(ID2D1DeviceContext2* d2d, const char* name, const D2D1_RECT_F& r, ID2D1Brush* brush);

private:
    ID2D1PathGeometry* SfGeometry(const char* name);
    void Transform(ID2D1DeviceContext2* d2d, const D2D1_RECT_F& r);
    void StrokePath(ID2D1DeviceContext2* d2d, ID2D1PathGeometry* path, ID2D1Brush* brush, float width);
    Gpu* gpu_ = nullptr;
    Ui* ui_ = nullptr;
    ComPtr<ID2D1PathGeometry> appleBody_;
    ComPtr<ID2D1PathGeometry> appleLeaf_;
    std::map<std::string, ComPtr<ID2D1PathGeometry>> sfIcons_;
};

}  // namespace sm
