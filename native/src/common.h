#pragma once

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <d2d1.h>
#include <dwrite.h>

#include <cstdint>
#include <cstdio>
#include <cmath>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>
#include <chrono>

#include "log.h"

// ---- COM smart pointer (no WRL / C++/WinRT on MinGW) -------------------------

template <class T>
class ComPtr {
public:
    ComPtr() = default;
    ComPtr(std::nullptr_t) {}
    explicit ComPtr(T* p) : p_(p) {}
    ComPtr(const ComPtr& o) : p_(o.p_) {
        if (p_) p_->AddRef();
    }
    ComPtr(ComPtr&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
    ~ComPtr() { reset(); }

    ComPtr& operator=(const ComPtr& o) {
        if (this != &o) {
            ComPtr tmp(o);
            swap(tmp);
        }
        return *this;
    }
    ComPtr& operator=(ComPtr&& o) noexcept {
        if (this != &o) {
            reset();
            p_ = o.p_;
            o.p_ = nullptr;
        }
        return *this;
    }
    ComPtr& operator=(std::nullptr_t) {
        reset();
        return *this;
    }

    void reset() {
        if (p_) {
            p_->Release();
            p_ = nullptr;
        }
    }
    void swap(ComPtr& o) noexcept { std::swap(p_, o.p_); }
    void attach(T* p) {
        reset();
        p_ = p;
    }
    T* detach() {
        T* p = p_;
        p_ = nullptr;
        return p;
    }
    T* get() const { return p_; }
    T** put() {
        reset();
        return &p_;
    }
    void** put_void() {
        reset();
        return reinterpret_cast<void**>(&p_);
    }
    T* operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }
    bool operator==(std::nullptr_t) const { return p_ == nullptr; }
    bool operator!=(std::nullptr_t) const { return p_ != nullptr; }

    template <class U>
    HRESULT as(ComPtr<U>& out) const {
        if (!p_) return E_POINTER;
        return p_->QueryInterface(__uuidof(U), reinterpret_cast<void**>(out.put()));
    }
    template <class U>
    HRESULT as(U** out) const {
        if (!p_) return E_POINTER;
        return p_->QueryInterface(__uuidof(U), reinterpret_cast<void**>(out));
    }

private:
    T* p_ = nullptr;
};

inline void SafeRelease(IUnknown* p) {
    if (p) p->Release();
}

// ---- diagnostics -------------------------------------------------------------

#ifndef SM_HR
#define SM_HR(expr)                          \
    do {                                     \
        HRESULT sm_hr_ = (expr);             \
        if (FAILED(sm_hr_)) {                \
            sm::LogHr(L"" #expr, sm_hr_);    \
        }                                    \
    } while (0)
#endif

inline bool Checked(HRESULT hr, const wchar_t* what) {
    if (FAILED(hr)) {
        sm::LogHr(what, hr);
        return false;
    }
    return true;
}

// ---- small helpers -----------------------------------------------------------

namespace sm {

inline int Px(double dip, double scale) {
    return static_cast<int>(std::lround(dip * scale));
}
inline float Pxf(double dip, double scale) {
    return static_cast<float>(dip * scale);
}

inline std::wstring ExeDirectory() {
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    std::wstring s(buf, n);
    const auto slash = s.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : s.substr(0, slash + 1);
}

inline std::wstring LocalAppData() {
    PWSTR p = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p)) && p) {
        std::wstring s(p);
        CoTaskMemFree(p);
        return s;
    }
    return ExeDirectory();
}

inline std::wstring DataDirectory() {
    static const std::wstring dir = [] {
        std::wstring d = LocalAppData() + L"\\SuperMenu";
        CreateDirectoryW(d.c_str(), nullptr);
        CreateDirectoryW((d + L"\\logs").c_str(), nullptr);
        return d;
    }();
    return dir;
}

inline double NowMs() {
    static const LARGE_INTEGER freq = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER c{};
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
}

inline std::wstring Lower(std::wstring s) {
    CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
    return s;
}

inline bool EndsWith(std::wstring_view s, std::wstring_view suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

inline float Clamp(float v, float lo, float hi) { return std::max(lo, std::min(hi, v)); }
inline double Clamp(double v, double lo, double hi) { return std::max(lo, std::min(hi, v)); }
inline int Clamp(int v, int lo, int hi) { return std::max(lo, std::min(hi, v)); }

inline float Lerp(float a, float b, float t) { return a + (b - a) * t; }

inline float EaseOutCubic(float t) {
    t = Clamp(t, 0.0f, 1.0f);
    const float u = 1.0f - t;
    return 1.0f - u * u * u;
}

// exponential approach towards a target; dt in seconds, rate per second
inline float Approach(float value, float target, float rate, float dt) {
    const float k = 1.0f - std::exp(-rate * dt);
    return value + (target - value) * k;
}

inline D2D1_COLOR_F ColorF(float r, float g, float b, float a = 1.0f) {
    return D2D1::ColorF(r, g, b, a);
}

}  // namespace sm
