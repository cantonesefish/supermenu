#include "icons.h"

#include "sf_icons_data.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace sm {
namespace {

ID2D1PathGeometry* NewPath(ID2D1Factory* factory) {
    ID2D1PathGeometry* path = nullptr;
    if (FAILED(factory->CreatePathGeometry(&path))) return nullptr;
    return path;
}

ID2D1GeometrySink* OpenSink(ID2D1PathGeometry* path) {
    ID2D1GeometrySink* sink = nullptr;
    if (FAILED(path->Open(&sink))) return nullptr;
    return sink;
}

void AddArc(ID2D1GeometrySink* sink, D2D1_POINT_2F a, D2D1_POINT_2F b, float radius, D2D1_SWEEP_DIRECTION sweep) {
    (void)a;
    sink->AddArc(D2D1::ArcSegment(b, D2D1::SizeF(radius, radius), 0.0f, sweep, D2D1_ARC_SIZE_SMALL));
}

// Minimal SVG path parser for the open-source Font Awesome apple silhouette.
class SvgPath {
public:
    SvgPath(const char* d, ID2D1GeometrySink* sink, float sx, float sy, float tx, float ty)
        : d_(d), sink_(sink), sx_(sx), sy_(sy), tx_(tx), ty_(ty) {}

    bool Build() {
        char cmd = 0;
        bool haveFigure = false;
        while (SkipSeparators(), *d_) {
            if (IsCommand(*d_)) {
                cmd = *d_++;
            } else if (!cmd) {
                return false;
            }
            const bool rel = cmd >= 'a' && cmd <= 'z';
            const char c = static_cast<char>(cmd >= 'a' && cmd <= 'z' ? cmd - 'a' + 'A' : cmd);
            if (c == 'Z') {
                if (haveFigure) {
                    sink_->EndFigure(D2D1_FIGURE_END_CLOSED);
                    haveFigure = false;
                }
                cur_ = start_;
                cmd = 0;
                continue;
            }
            bool first = true;
            do {
                if (c == 'M') {
                    D2D1_POINT_2F p = Point(rel);
                    if (first) {
                        if (haveFigure) sink_->EndFigure(D2D1_FIGURE_END_OPEN);
                        start_ = p;
                        cur_ = p;
                        lastCtrl_ = p;
                        sink_->BeginFigure(p, D2D1_FIGURE_BEGIN_FILLED);
                        sink_->SetFillMode(D2D1_FILL_MODE_WINDING);
                        haveFigure = true;
                    } else {
                        cur_ = p;
                        sink_->AddLine(p);
                    }
                } else if (c == 'L') {
                    cur_ = Point(rel);
                    sink_->AddLine(cur_);
                } else if (c == 'H') {
                    float x = Number();
                    if (rel) x += (cur_.x - tx_) / sx_;
                    cur_.x = x * sx_ + tx_;
                    sink_->AddLine(cur_);
                } else if (c == 'V') {
                    float y = Number();
                    if (rel) y += (cur_.y - ty_) / sy_;
                    cur_.y = y * sy_ + ty_;
                    sink_->AddLine(cur_);
                } else if (c == 'C') {
                    D2D1_POINT_2F c1 = Point(rel);
                    D2D1_POINT_2F c2 = Point(rel);
                    D2D1_POINT_2F end = Point(rel);
                    sink_->AddBezier(D2D1::BezierSegment(c1, c2, end));
                    lastCtrl_ = c2;
                    cur_ = end;
                } else if (c == 'S') {
                    D2D1_POINT_2F c1 = {cur_.x * 2 - lastCtrl_.x, cur_.y * 2 - lastCtrl_.y};
                    D2D1_POINT_2F c2 = Point(rel);
                    D2D1_POINT_2F end = Point(rel);
                    sink_->AddBezier(D2D1::BezierSegment(c1, c2, end));
                    lastCtrl_ = c2;
                    cur_ = end;
                } else if (c == 'Q') {
                    D2D1_POINT_2F c1 = Point(rel);
                    D2D1_POINT_2F end = Point(rel);
                    sink_->AddQuadraticBezier(D2D1::QuadraticBezierSegment(c1, end));
                    lastCtrl_ = c1;
                    cur_ = end;
                } else if (c == 'T') {
                    D2D1_POINT_2F c1 = {cur_.x * 2 - lastCtrl_.x, cur_.y * 2 - lastCtrl_.y};
                    D2D1_POINT_2F end = Point(rel);
                    sink_->AddQuadraticBezier(D2D1::QuadraticBezierSegment(c1, end));
                    lastCtrl_ = c1;
                    cur_ = end;
                }
                first = false;
                SkipSeparators();
                if (cmd == 'M' || cmd == 'm') cmd = rel ? 'l' : 'L';
            } while (StartsNumber());
        }
        if (haveFigure) {
            sink_->EndFigure(D2D1_FIGURE_END_CLOSED);
            haveFigure = false;
        }
        return true;
    }

private:
    void SkipSeparators() {
        while (*d_ && (*d_ == ' ' || *d_ == ',' || *d_ == '\t' || *d_ == '\n' || *d_ == '\r')) ++d_;
    }
    static bool IsCommand(char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    }
    bool StartsNumber() const {
        const char c = *d_;
        return c == '-' || c == '+' || c == '.' || (c >= '0' && c <= '9');
    }
    float Number() {
        char* end = nullptr;
        const double v = std::strtod(d_, &end);
        if (end == d_) return 0.0f;
        d_ = end;
        SkipSeparators();
        return static_cast<float>(v);
    }
    D2D1_POINT_2F ToPath(float x, float y) const { return D2D1::Point2F(x * sx_ + tx_, y * sy_ + ty_); }
    D2D1_POINT_2F Point(bool relative) {
        const float x = Number();
        const float y = Number();
        D2D1_POINT_2F p = ToPath(x, y);
        if (relative) {
            // cur_ is already in path space (including the translation); add
            // only the untranslated current position to keep deltas correct.
            p.x += cur_.x - tx_;
            p.y += cur_.y - ty_;
        }
        return p;
    }

    const char* d_ = nullptr;
    ID2D1GeometrySink* sink_ = nullptr;
    float sx_ = 1, sy_ = 1, tx_ = 0, ty_ = 0;
    D2D1_POINT_2F cur_{0, 0}, start_{0, 0}, lastCtrl_{0, 0};
};

ID2D1PathGeometry* BuildApple(ID2D1Factory* factory) {
    static const char kApple[] =
        "M318.7 268.7c-.2-36.7 16.4-64.4 50-84.8-18.8-26.9-47.2-41.7-84.7-44.6-35.5-2.8-74.3 20.7-88.5 "
        "20.7-15 0-49.4-19.7-76.4-19.7C63.3 141.2 4 184.8 4 273.5q0 39.3 14.4 81.2c12.8 36.7 59 126.7 107.2 "
        "125.2 25.2-.6 43-17.9 75.8-17.9 31.8 0 48.3 17.9 76.4 17.9 48.6-.7 90.4-82.5 102.6-119.3-65.2-30.7-61.7-90-61.7-91.9zm-56.6-164.2c27.3-32.4 "
        "24.8-61.9 24-72.5-24.1 1.4-52 16.4-67.9 34.9-17.5 19.8-27.8 44.3-25.6 71.9 26.1 2 49.9-11.4 69.5-34.3z";
    ID2D1PathGeometry* path = NewPath(factory);
    if (!path) return nullptr;
    ID2D1GeometrySink* sink = OpenSink(path);
    if (!sink) {
        path->Release();
        return nullptr;
    }
    SvgPath parser(kApple, sink, 1.0f / 512.0f, 1.0f / 512.0f, 0.125f, 0.0f);
    if (!parser.Build()) {
        sink->Close();
        sink->Release();
        path->Release();
        return nullptr;
    }
    sink->Close();
    sink->Release();
    return path;
}

ID2D1PathGeometry* BuildSfIcon(ID2D1Factory* factory, const SfIconSpec& spec) {
    ID2D1PathGeometry* path = NewPath(factory);
    if (!path) return nullptr;
    ID2D1GeometrySink* sink = OpenSink(path);
    if (!sink) {
        path->Release();
        return nullptr;
    }
    const float sx = spec.width > 0 ? 1.0f / spec.width : 1.0f;
    const float sy = spec.height > 0 ? 1.0f / spec.height : 1.0f;
    SvgPath parser(spec.path, sink, sx, sy, -spec.x * sx, -spec.y * sy);
    if (!parser.Build()) {
        sink->Close();
        sink->Release();
        path->Release();
        return nullptr;
    }
    sink->Close();
    sink->Release();
    return path;
}

}  // namespace

Icons::Icons(Gpu* gpu, Ui* ui) : gpu_(gpu), ui_(ui) {
    appleBody_.attach(BuildApple(gpu_->d2dFactory()));
}

ID2D1PathGeometry* Icons::SfGeometry(const char* name) {
    if (!name) return nullptr;
    auto it = sfIcons_.find(name);
    if (it != sfIcons_.end()) return it->second.get();
    const SfIconSpec* spec = nullptr;
    for (const auto& candidate : kSfIcons) {
        if (std::strcmp(candidate.name, name) == 0) {
            spec = &candidate;
            break;
        }
    }
    if (!spec) return nullptr;
    ComPtr<ID2D1PathGeometry> geometry;
    geometry.attach(BuildSfIcon(gpu_->d2dFactory(), *spec));
    if (!geometry) return nullptr;
    auto [inserted, ok] = sfIcons_.emplace(name, std::move(geometry));
    return inserted->second.get();
}

void Icons::Filled(ID2D1DeviceContext2* d2d, const char* name, const D2D1_RECT_F& r, ID2D1Brush* brush) {
    if (!brush) return;
    ID2D1PathGeometry* geometry = SfGeometry(name);
    if (!geometry) return;
    D2D1_RECT_F bounds{};
    if (FAILED(geometry->GetBounds(nullptr, &bounds))) return;
    const float gw = std::max(0.001f, bounds.right - bounds.left);
    const float gh = std::max(0.001f, bounds.bottom - bounds.top);
    const float w = r.right - r.left;
    const float h = r.bottom - r.top;
    const float s = std::min(w / gw, h / gh) * 0.96f;
    const float cx = (r.left + r.right) * 0.5f;
    const float cy = (r.top + r.bottom) * 0.5f;
    const float tx = cx - (bounds.left + gw * 0.5f) * s;
    const float ty = cy - (bounds.top + gh * 0.5f) * s;
    d2d->SetTransform(D2D1::Matrix3x2F::Scale(s, s) * D2D1::Matrix3x2F::Translation(tx, ty));
    d2d->FillGeometry(geometry, brush);
    d2d->SetTransform(D2D1::Matrix3x2F::Identity());
}

void Icons::Sf(ID2D1DeviceContext2* d2d, const char* name, const D2D1_RECT_F& r, ID2D1Brush* brush) {
    if (!brush) return;
    ID2D1PathGeometry* geometry = SfGeometry(name);
    if (!geometry) return;
    // The source symbols have individual padding inside their 88.224 unit
    // viewBoxes. Fit each symbol's ink bounds into the same box, leaving a
    // consistent 8% optical margin, so Wi-Fi/battery/equalizer/search read at
    // the same size instead of inheriting different path whitespace.
    D2D1_RECT_F bounds{};
    if (FAILED(geometry->GetBounds(nullptr, &bounds))) return;
    const float gw = std::max(0.001f, bounds.right - bounds.left);
    const float gh = std::max(0.001f, bounds.bottom - bounds.top);
    const float w = r.right - r.left;
    const float h = r.bottom - r.top;
    const float s = std::min(w / gw, h / gh) * 0.92f;
    const float cx = (r.left + r.right) * 0.5f;
    const float cy = (r.top + r.bottom) * 0.5f;
    const float tx = cx - (bounds.left + gw * 0.5f) * s;
    const float ty = cy - (bounds.top + gh * 0.5f) * s;
    d2d->SetTransform(D2D1::Matrix3x2F::Scale(s, s) * D2D1::Matrix3x2F::Translation(tx, ty));
    d2d->FillGeometry(geometry, brush);
    // Slightly expand the silhouette so status/control icons read as the heavier
    // Apple Control Center weight rather than hairline Lucide outlines.
    d2d->DrawGeometry(geometry, brush, 0.055f, ui_ ? ui_->RoundStroke() : nullptr);
    d2d->SetTransform(D2D1::Matrix3x2F::Identity());
}

void Icons::Transform(ID2D1DeviceContext2* d2d, const D2D1_RECT_F& r) {
    const float w = r.right - r.left, h = r.bottom - r.top;
    const float s = std::min(w, h);
    const float cx = (r.left + r.right) * 0.5f - s * 0.5f;
    const float cy = (r.top + r.bottom) * 0.5f - s * 0.5f;
    d2d->SetTransform(D2D1::Matrix3x2F::Scale(s, s) * D2D1::Matrix3x2F::Translation(cx, cy));
}

void Icons::StrokePath(ID2D1DeviceContext2* d2d, ID2D1PathGeometry* path, ID2D1Brush* brush, float width) {
    d2d->DrawGeometry(path, brush, width, ui_->RoundStroke());
}

void Icons::Apple(ID2D1DeviceContext2* d2d, const D2D1_RECT_F& r, ID2D1Brush* brush, float opacity) {
    if (!appleBody_ || !brush) return;
    Transform(d2d, r);
    d2d->FillGeometry(appleBody_.get(), brush);
    d2d->SetTransform(D2D1::Matrix3x2F::Identity());
    (void)opacity;
}

void Icons::Windows(ID2D1DeviceContext2* d2d, const D2D1_RECT_F& r, ID2D1Brush* brush) {
    if (!brush) return;
    // Source SVG: assets/logos/windows-logo-2021.svg, viewBox 0 0 4875 4875.
    const float size = std::min(r.right - r.left, r.bottom - r.top);
    const float x = (r.left + r.right - size) * 0.5f;
    const float y = (r.top + r.bottom - size) * 0.5f;
    const float k = size / 4875.0f;
    const float a = 2311.0f * k;   // pane width
    const float b = 2310.0f * k;   // pane height
    const float gap = 253.0f * k;
    d2d->FillRectangle(D2D1::RectF(x, y, x + a, y + b), brush);
    d2d->FillRectangle(D2D1::RectF(x + a + gap, y, x + 2 * a + gap, y + b), brush);
    d2d->FillRectangle(D2D1::RectF(x, y + b + gap, x + a, y + 2 * b + gap), brush);
    d2d->FillRectangle(D2D1::RectF(x + a + gap, y + b + gap, x + 2 * a + gap, y + 2 * b + gap), brush);
}

void Icons::Wifi(ID2D1DeviceContext2* d2d, const D2D1_RECT_F& r, ID2D1Brush* brush, int strength, bool off) {
    Transform(d2d, r);
    const float w = 0.085f;
    (void)strength;
    for (int i = 0; i < 3; ++i) {
        const float rad = 0.20f + 0.20f * i;
        ComPtr<ID2D1PathGeometry> path(NewPath(gpu_->d2dFactory()));
        if (!path) continue;
        auto* sink = OpenSink(path.get());
        if (!sink) continue;
        const float a = 0.30f;
        const float y0 = 0.78f - rad * std::sin(a);
        sink->BeginFigure(D2D1::Point2F(0.5f - rad * std::cos(a), y0), D2D1_FIGURE_BEGIN_HOLLOW);
        AddArc(sink, D2D1::Point2F(0.5f - rad * std::cos(a), y0),
               D2D1::Point2F(0.5f + rad * std::cos(a), y0), rad, D2D1_SWEEP_DIRECTION_CLOCKWISE);
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        sink->Close();
        sink->Release();
        d2d->DrawGeometry(path.get(), brush, w, ui_->RoundStroke());
    }
    d2d->FillEllipse(D2D1::Ellipse(D2D1::Point2F(0.5f, 0.82f), 0.055f, 0.055f), brush);
    if (off) {
        d2d->DrawLine(D2D1::Point2F(0.18f, 0.84f), D2D1::Point2F(0.84f, 0.14f), brush, 0.085f, ui_->RoundStroke());
    }
    d2d->SetTransform(D2D1::Matrix3x2F::Identity());
}

void Icons::Battery(ID2D1DeviceContext2* d2d, const D2D1_RECT_F& r, ID2D1Brush* brush, int percent, bool charging,
                    bool low) {
    Transform(d2d, r);
    const D2D1_RECT_F body{0.10f, 0.30f, 0.82f, 0.70f};
    d2d->DrawRoundedRectangle(D2D1::RoundedRect(body, 0.08f, 0.08f), brush, 0.075f, ui_->RoundStroke());
    d2d->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0.88f, 0.43f, 0.95f, 0.57f), 0.025f, 0.025f), brush);
    const float level = sm::Clamp(percent, 0, 100) / 100.0f;
    if (level > 0.02f) {
        const float innerL = 0.14f, innerR = 0.78f;
        const float width = (innerR - innerL) * level;
        d2d->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(innerL, 0.36f, innerL + width, 0.64f), 0.03f, 0.03f),
                                  brush);
    }
    if (charging) {
        ComPtr<ID2D1PathGeometry> bolt(NewPath(gpu_->d2dFactory()));
        if (bolt) {
            auto* sink = OpenSink(bolt.get());
            if (sink) {
                sink->BeginFigure(D2D1::Point2F(0.52f, 0.18f), D2D1_FIGURE_BEGIN_FILLED);
                sink->AddLine(D2D1::Point2F(0.52f, 0.44f));
                sink->AddLine(D2D1::Point2F(0.36f, 0.44f));
                sink->AddLine(D2D1::Point2F(0.55f, 0.82f));
                sink->AddLine(D2D1::Point2F(0.49f, 0.52f));
                sink->AddLine(D2D1::Point2F(0.66f, 0.52f));
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                sink->Close();
                sink->Release();
                d2d->FillGeometry(bolt.get(), brush);
            }
        }
    }
    d2d->SetTransform(D2D1::Matrix3x2F::Identity());
    (void)low;
}

void Icons::ControlCenter(ID2D1DeviceContext2* d2d, const D2D1_RECT_F& r, ID2D1Brush* brush) {
    Transform(d2d, r);
    d2d->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0.10f, 0.22f, 0.90f, 0.46f), 0.12f, 0.12f), brush, 0.075f,
                              ui_->RoundStroke());
    d2d->FillEllipse(D2D1::Ellipse(D2D1::Point2F(0.28f, 0.34f), 0.09f, 0.09f), brush);
    d2d->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0.10f, 0.54f, 0.90f, 0.78f), 0.12f, 0.12f), brush, 0.075f,
                              ui_->RoundStroke());
    d2d->FillEllipse(D2D1::Ellipse(D2D1::Point2F(0.72f, 0.66f), 0.09f, 0.09f), brush);
    d2d->SetTransform(D2D1::Matrix3x2F::Identity());
}

void Icons::Search(ID2D1DeviceContext2* d2d, const D2D1_RECT_F& r, ID2D1Brush* brush) {
    Transform(d2d, r);
    d2d->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(0.44f, 0.42f), 0.25f, 0.25f), brush, 0.085f, ui_->RoundStroke());
    d2d->DrawLine(D2D1::Point2F(0.63f, 0.63f), D2D1::Point2F(0.86f, 0.86f), brush, 0.085f, ui_->RoundStroke());
    d2d->SetTransform(D2D1::Matrix3x2F::Identity());
}

void Icons::Volume(ID2D1DeviceContext2* d2d, const D2D1_RECT_F& r, ID2D1Brush* brush, int level, bool muted) {
    Transform(d2d, r);
    ComPtr<ID2D1PathGeometry> spk(NewPath(gpu_->d2dFactory()));
    if (spk) {
        auto* sink = OpenSink(spk.get());
        if (sink) {
            sink->BeginFigure(D2D1::Point2F(0.10f, 0.40f), D2D1_FIGURE_BEGIN_FILLED);
            sink->AddLine(D2D1::Point2F(0.30f, 0.40f));
            sink->AddLine(D2D1::Point2F(0.50f, 0.22f));
            sink->AddLine(D2D1::Point2F(0.50f, 0.78f));
            sink->AddLine(D2D1::Point2F(0.30f, 0.60f));
            sink->AddLine(D2D1::Point2F(0.10f, 0.60f));
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->Close();
            sink->Release();
            d2d->FillGeometry(spk.get(), brush);
        }
    }
    if (muted) {
        d2d->DrawLine(D2D1::Point2F(0.60f, 0.36f), D2D1::Point2F(0.90f, 0.66f), brush, 0.075f, ui_->RoundStroke());
        d2d->DrawLine(D2D1::Point2F(0.90f, 0.36f), D2D1::Point2F(0.60f, 0.66f), brush, 0.075f, ui_->RoundStroke());
    } else {
        const int waves = sm::Clamp(level / 34 + 1, 0, 2);
        ComPtr<ID2D1PathGeometry> arc(NewPath(gpu_->d2dFactory()));
        if (arc) {
            auto* sink = OpenSink(arc.get());
            if (sink) {
                sink->BeginFigure(D2D1::Point2F(0.58f, 0.36f), D2D1_FIGURE_BEGIN_HOLLOW);
                AddArc(sink, D2D1::Point2F(0.58f, 0.36f), D2D1::Point2F(0.58f, 0.64f), 0.18f,
                       D2D1_SWEEP_DIRECTION_CLOCKWISE);
                sink->EndFigure(D2D1_FIGURE_END_OPEN);
                if (waves > 0) {
                    sink->BeginFigure(D2D1::Point2F(0.70f, 0.28f), D2D1_FIGURE_BEGIN_HOLLOW);
                    AddArc(sink, D2D1::Point2F(0.70f, 0.28f), D2D1::Point2F(0.70f, 0.72f), 0.28f,
                           D2D1_SWEEP_DIRECTION_CLOCKWISE);
                    sink->EndFigure(D2D1_FIGURE_END_OPEN);
                }
                sink->Close();
                sink->Release();
                StrokePath(d2d, arc.get(), brush, 0.075f);
            }
        }
    }
    d2d->SetTransform(D2D1::Matrix3x2F::Identity());
}

void Icons::Bluetooth(ID2D1DeviceContext2* d2d, const D2D1_RECT_F& r, ID2D1Brush* brush) {
    Transform(d2d, r);
    ComPtr<ID2D1PathGeometry> path(NewPath(gpu_->d2dFactory()));
    if (path) {
        auto* sink = OpenSink(path.get());
        if (sink) {
            sink->BeginFigure(D2D1::Point2F(0.50f, 0.10f), D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddLine(D2D1::Point2F(0.50f, 0.90f));
            sink->AddLine(D2D1::Point2F(0.78f, 0.68f));
            sink->AddLine(D2D1::Point2F(0.22f, 0.32f));
            sink->AddLine(D2D1::Point2F(0.78f, 0.32f));
            sink->AddLine(D2D1::Point2F(0.22f, 0.68f));
            sink->AddLine(D2D1::Point2F(0.50f, 0.90f));
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            sink->Close();
            sink->Release();
            StrokePath(d2d, path.get(), brush, 0.08f);
        }
    }
    d2d->SetTransform(D2D1::Matrix3x2F::Identity());
}

void Icons::Bell(ID2D1DeviceContext2* d2d, const D2D1_RECT_F& r, ID2D1Brush* brush) {
    Transform(d2d, r);
    ComPtr<ID2D1PathGeometry> path(NewPath(gpu_->d2dFactory()));
    if (path) {
        auto* sink = OpenSink(path.get());
        if (sink) {
            sink->BeginFigure(D2D1::Point2F(0.22f, 0.70f), D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddLine(D2D1::Point2F(0.30f, 0.58f));
            sink->AddLine(D2D1::Point2F(0.30f, 0.42f));
            sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(0.30f, 0.22f), D2D1::Point2F(0.42f, 0.14f),
                                                D2D1::Point2F(0.50f, 0.14f)));
            sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(0.58f, 0.14f), D2D1::Point2F(0.70f, 0.22f),
                                                D2D1::Point2F(0.70f, 0.42f)));
            sink->AddLine(D2D1::Point2F(0.70f, 0.58f));
            sink->AddLine(D2D1::Point2F(0.78f, 0.70f));
            sink->AddLine(D2D1::Point2F(0.22f, 0.70f));
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            sink->Close();
            sink->Release();
            StrokePath(d2d, path.get(), brush, 0.075f);
        }
    }
    d2d->FillEllipse(D2D1::Ellipse(D2D1::Point2F(0.5f, 0.80f), 0.08f, 0.055f), brush);
    d2d->SetTransform(D2D1::Matrix3x2F::Identity());
}

void Icons::Chevron(ID2D1DeviceContext2* d2d, float x, float y, float size, ID2D1Brush* brush, bool right) {
    const float s = size;
    if (right) {
        d2d->DrawLine(D2D1::Point2F(x - s * 0.25f, y - s * 0.5f), D2D1::Point2F(x + s * 0.25f, y), brush, 1.4f,
                      ui_->RoundStroke());
        d2d->DrawLine(D2D1::Point2F(x + s * 0.25f, y), D2D1::Point2F(x - s * 0.25f, y + s * 0.5f), brush, 1.4f,
                      ui_->RoundStroke());
    } else {
        d2d->DrawLine(D2D1::Point2F(x + s * 0.25f, y - s * 0.5f), D2D1::Point2F(x - s * 0.25f, y), brush, 1.4f,
                      ui_->RoundStroke());
        d2d->DrawLine(D2D1::Point2F(x - s * 0.25f, y), D2D1::Point2F(x + s * 0.25f, y + s * 0.5f), brush, 1.4f,
                      ui_->RoundStroke());
    }
}

void Icons::Check(ID2D1DeviceContext2* d2d, const D2D1_RECT_F& r, ID2D1Brush* brush) {
    const float w = r.right - r.left, h = r.bottom - r.top;
    d2d->DrawLine(D2D1::Point2F(r.left + w * 0.15f, r.top + h * 0.55f), D2D1::Point2F(r.left + w * 0.42f, r.top + h * 0.80f),
                  brush, 1.8f, ui_->RoundStroke());
    d2d->DrawLine(D2D1::Point2F(r.left + w * 0.42f, r.top + h * 0.80f), D2D1::Point2F(r.left + w * 0.88f, r.top + h * 0.22f),
                  brush, 1.8f, ui_->RoundStroke());
}

void Icons::PlayPause(ID2D1DeviceContext2* d2d, const D2D1_RECT_F& r, ID2D1Brush* brush, bool playing) {
    const float w = r.right - r.left, h = r.bottom - r.top;
    if (playing) {
        d2d->FillRectangle(D2D1::RectF(r.left + w * 0.20f, r.top + h * 0.20f, r.left + w * 0.42f, r.top + h * 0.80f), brush);
        d2d->FillRectangle(D2D1::RectF(r.left + w * 0.58f, r.top + h * 0.20f, r.left + w * 0.80f, r.top + h * 0.80f), brush);
    } else {
        ComPtr<ID2D1PathGeometry> path(NewPath(gpu_->d2dFactory()));
        if (path) {
            auto* sink = OpenSink(path.get());
            if (sink) {
                sink->BeginFigure(D2D1::Point2F(r.left + w * 0.28f, r.top + h * 0.18f), D2D1_FIGURE_BEGIN_FILLED);
                sink->AddLine(D2D1::Point2F(r.left + w * 0.82f, r.top + h * 0.50f));
                sink->AddLine(D2D1::Point2F(r.left + w * 0.28f, r.top + h * 0.82f));
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                sink->Close();
                sink->Release();
                d2d->FillGeometry(path.get(), brush);
            }
        }
    }
}

void Icons::Previous(ID2D1DeviceContext2* d2d, const D2D1_RECT_F& r, ID2D1Brush* brush) {
    const float w = r.right - r.left, h = r.bottom - r.top;
    d2d->FillRectangle(D2D1::RectF(r.left + w * 0.18f, r.top + h * 0.20f, r.left + w * 0.30f, r.top + h * 0.80f), brush);
    ComPtr<ID2D1PathGeometry> path(NewPath(gpu_->d2dFactory()));
    if (path) {
        auto* sink = OpenSink(path.get());
        if (sink) {
            sink->BeginFigure(D2D1::Point2F(r.left + w * 0.78f, r.top + h * 0.20f), D2D1_FIGURE_BEGIN_FILLED);
            sink->AddLine(D2D1::Point2F(r.left + w * 0.36f, r.top + h * 0.50f));
            sink->AddLine(D2D1::Point2F(r.left + w * 0.78f, r.top + h * 0.80f));
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->Close();
            sink->Release();
            d2d->FillGeometry(path.get(), brush);
        }
    }
}

void Icons::Next(ID2D1DeviceContext2* d2d, const D2D1_RECT_F& r, ID2D1Brush* brush) {
    const float w = r.right - r.left, h = r.bottom - r.top;
    d2d->FillRectangle(D2D1::RectF(r.left + w * 0.70f, r.top + h * 0.20f, r.left + w * 0.82f, r.top + h * 0.80f), brush);
    ComPtr<ID2D1PathGeometry> path(NewPath(gpu_->d2dFactory()));
    if (path) {
        auto* sink = OpenSink(path.get());
        if (sink) {
            sink->BeginFigure(D2D1::Point2F(r.left + w * 0.22f, r.top + h * 0.20f), D2D1_FIGURE_BEGIN_FILLED);
            sink->AddLine(D2D1::Point2F(r.left + w * 0.64f, r.top + h * 0.50f));
            sink->AddLine(D2D1::Point2F(r.left + w * 0.22f, r.top + h * 0.80f));
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->Close();
            sink->Release();
            d2d->FillGeometry(path.get(), brush);
        }
    }
}

void Icons::Sun(ID2D1DeviceContext2* d2d, const D2D1_RECT_F& r, ID2D1Brush* brush) {
    Transform(d2d, r);
    d2d->FillEllipse(D2D1::Ellipse(D2D1::Point2F(0.5f, 0.5f), 0.18f, 0.18f), brush);
    for (int i = 0; i < 8; ++i) {
        const float a = i * 3.14159265f / 4.0f;
        d2d->DrawLine(D2D1::Point2F(0.5f + std::cos(a) * 0.30f, 0.5f + std::sin(a) * 0.30f),
                      D2D1::Point2F(0.5f + std::cos(a) * 0.42f, 0.5f + std::sin(a) * 0.42f), brush, 0.075f,
                      ui_->RoundStroke());
    }
    d2d->SetTransform(D2D1::Matrix3x2F::Identity());
}

void Icons::Speaker(ID2D1DeviceContext2* d2d, const D2D1_RECT_F& r, ID2D1Brush* brush) {
    Volume(d2d, r, brush, 100, false);
}

void Icons::BluetoothSettings(ID2D1DeviceContext2* d2d, const D2D1_RECT_F& r, ID2D1Brush* brush) {
    Bluetooth(d2d, r, brush);
}

void Icons::Globe(ID2D1DeviceContext2* d2d, const D2D1_RECT_F& r, ID2D1Brush* brush) {
    Transform(d2d, r);
    d2d->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(0.5f, 0.5f), 0.38f, 0.38f), brush, 0.075f, ui_->RoundStroke());
    d2d->DrawLine(D2D1::Point2F(0.12f, 0.5f), D2D1::Point2F(0.88f, 0.5f), brush, 0.065f, ui_->RoundStroke());
    d2d->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(0.5f, 0.5f), 0.16f, 0.38f), brush, 0.065f, ui_->RoundStroke());
    d2d->SetTransform(D2D1::Matrix3x2F::Identity());
}

}  // namespace sm
