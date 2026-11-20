#include "shell/shell_icon.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace atrium {

namespace {

bool ReadBits(HBITMAP bitmap, int& width, int& height, std::vector<uint8_t>& pixels) {
    BITMAP info{};
    if (!GetObjectW(bitmap, sizeof(info), &info) || info.bmWidth <= 0 || info.bmHeight <= 0) return false;
    width = info.bmWidth;
    height = info.bmHeight;
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    pixels.assign(static_cast<size_t>(width) * height * 4, 0);
    HDC dc = GetDC(nullptr);
    const int lines = GetDIBits(dc, bitmap, 0, static_cast<UINT>(height), pixels.data(), &bmi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    return lines == height;
}

void MakePremultiplied(std::vector<uint8_t>& pixels) {
    bool anyAlpha = false;
    bool straight = false;
    for (size_t i = 0; i < pixels.size(); i += 4) {
        const uint8_t a = pixels[i + 3];
        anyAlpha |= a != 0;
        straight |= pixels[i] > a || pixels[i + 1] > a || pixels[i + 2] > a;
    }
    if (!anyAlpha) {
        for (size_t i = 3; i < pixels.size(); i += 4) pixels[i] = 255;
    } else if (straight) {
        for (size_t i = 0; i < pixels.size(); i += 4) {
            const unsigned a = pixels[i + 3];
            for (size_t c = 0; c < 3; ++c)
                pixels[i + c] = static_cast<uint8_t>((pixels[i + c] * a + 127) / 255);
        }
    }
}

}  // namespace

bool HIconToImage(HICON icon, IconImage& out) {
    ICONINFO info{};
    if (!GetIconInfo(icon, &info)) return false;
    bool ok = false;
    if (info.hbmColor) {
        std::vector<uint8_t> color;
        int width = 0, height = 0;
        if (ReadBits(info.hbmColor, width, height, color)) {
            bool hasAlpha = false;
            for (size_t i = 3; i < color.size() && !hasAlpha; i += 4) hasAlpha = color[i] != 0;
            if (hasAlpha) {
                MakePremultiplied(color);
            } else {
                std::vector<uint8_t> mask;
                int maskWidth = 0, maskHeight = 0;
                const bool haveMask = info.hbmMask && ReadBits(info.hbmMask, maskWidth, maskHeight, mask) &&
                                      maskWidth == width && maskHeight == height;
                for (size_t i = 0; i < color.size(); i += 4) {
                    const bool opaque = !haveMask || mask[i] == 0;
                    color[i + 3] = opaque ? 255 : 0;
                    if (!opaque) color[i] = color[i + 1] = color[i + 2] = 0;
                }
            }
            out.width = width;
            out.height = height;
            out.bgra = std::move(color);
            ok = true;
        }
    }
    if (info.hbmColor) DeleteObject(info.hbmColor);
    if (info.hbmMask) DeleteObject(info.hbmMask);
    return ok;
}

IconImage FitToSquare(const IconImage& src, int size) {
    if (src.width == size && src.height == size) return src;
    IconImage out;
    out.width = out.height = size;
    out.bgra.assign(static_cast<size_t>(size) * size * 4, 0);
    const double scale = static_cast<double>(size) / std::max(src.width, src.height);
    const int dstW = std::max(1, static_cast<int>(std::lround(src.width * scale)));
    const int dstH = std::max(1, static_cast<int>(std::lround(src.height * scale)));
    const int offsetX = (size - dstW) / 2;
    const int offsetY = (size - dstH) / 2;
    auto at = [&](int x, int y, int c) {
        x = std::clamp(x, 0, src.width - 1);
        y = std::clamp(y, 0, src.height - 1);
        return static_cast<double>(src.bgra[(static_cast<size_t>(y) * src.width + x) * 4 + c]);
    };
    for (int dy = 0; dy < dstH; ++dy) {
        const double y0 = static_cast<double>(dy) * src.height / dstH;
        const double y1 = static_cast<double>(dy + 1) * src.height / dstH;
        for (int dx = 0; dx < dstW; ++dx) {
            const double x0 = static_cast<double>(dx) * src.width / dstW;
            const double x1 = static_cast<double>(dx + 1) * src.width / dstW;
            uint8_t* d = &out.bgra[(static_cast<size_t>(dy + offsetY) * size + dx + offsetX) * 4];
            for (int c = 0; c < 4; ++c) {
                double value = 0;
                if (x1 - x0 <= 1.0 && y1 - y0 <= 1.0) {
                    const double fx = (x0 + x1) / 2 - 0.5;
                    const double fy = (y0 + y1) / 2 - 0.5;
                    const int ix = static_cast<int>(std::floor(fx));
                    const int iy = static_cast<int>(std::floor(fy));
                    const double tx = fx - ix, ty = fy - iy;
                    value = at(ix, iy, c) * (1 - tx) * (1 - ty) + at(ix + 1, iy, c) * tx * (1 - ty) +
                            at(ix, iy + 1, c) * (1 - tx) * ty + at(ix + 1, iy + 1, c) * tx * ty;
                } else {
                    double sum = 0, weight = 0;
                    for (int sy = static_cast<int>(std::floor(y0)); sy < static_cast<int>(std::ceil(y1)); ++sy) {
                        const double wy = std::min<double>(sy + 1, y1) - std::max<double>(sy, y0);
                        for (int sx = static_cast<int>(std::floor(x0)); sx < static_cast<int>(std::ceil(x1)); ++sx) {
                            const double wx = std::min<double>(sx + 1, x1) - std::max<double>(sx, x0);
                            sum += at(sx, sy, c) * wx * wy;
                            weight += wx * wy;
                        }
                    }
                    value = weight > 0 ? sum / weight : 0;
                }
                d[c] = static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(value)), 0, 255));
            }
        }
    }
    return out;
}

std::wstring AppExecutable(const std::wstring& path) {
    // The tray host only looks at process image paths (never .lnk shortcuts).
    return path;
}

}  // namespace atrium
