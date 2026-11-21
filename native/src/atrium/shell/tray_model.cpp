// Pipeline and evidence contracts: docs/TRAY.md. All shell/UIA work stays on Worker.
// Ported from Atrium Desktop (MIT) and adapted to SuperMenu (no C++/WinRT, no
// Atrium-only modules, taskbar reveal instead of Atrium's system-chrome manager).
#include "common.h"

#include "shell/tray_model.h"
#include "shell/tray_match.h"
#include "shell/tray_click.h"
#include "shell/tray_image.h"

#include "atrium/log.h"
#include "shell/shell_icon.h"
#include "shell/shell_taskbar.h"
#include "shell/tray_pointer.h"
#include "shell/tray_taskbar.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <tlhelp32.h>
#include <uiautomation.h>
#include <wincodec.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <map>
#include <set>

namespace atrium {

namespace {

constexpr int kIconPx = 32; // SuperMenu draws tray icons at 16 logical px on a 32 px source
std::atomic<DWORD> g_syntheticInput{0}; // GetLastInputInfo time of the last pointer click SuperMenu sent
static_assert(tray::kProviderTimeout == int32_t(UIA_E_TIMEOUT));

// UI Automation identities (defined here: the MinGW headers declare the CLSIDs
// but SuperMenu does not link the SDK's uuid helpers).
constexpr GUID kClsidUia8 = {0xe22ad333, 0xb25f, 0x460c, {0x83, 0xd0, 0x05, 0x81, 0x10, 0x73, 0x95, 0xc9}};
constexpr GUID kClsidUia = {0xff48dba4, 0x60ef, 0x4201, {0xaa, 0x87, 0x54, 0x10, 0x3e, 0xef, 0x59, 0x4e}};
constexpr GUID kIidUia = {0x30cbe57d, 0xd9d0, 0x452a, {0xab, 0x13, 0x7a, 0xc5, 0xac, 0x48, 0x25, 0xee}};
constexpr GUID kIidInvoke = {0xfb377fbe, 0x8ea6, 0x46d5, {0x9c, 0x73, 0x64, 0x99, 0x64, 0x2d, 0x30, 0x59}};
constexpr GUID kIidLegacy = {0x828055ad, 0x355b, 0x4435, {0x86, 0xd5, 0x3b, 0x51, 0xc1, 0x4a, 0x9b, 0x1b}};

template <class U, class T>
ComPtr<U> TryAs(const ComPtr<T>& source)
{
    ComPtr<U> out;
    if (source)
        source.as(out);
    return out;
}

// The program's own picture, for a record whose snapshot is missing or invalid.
bool LoadProgramIcon(const std::wstring& exe, int sizePx, IconImage& out)
{
    if (exe.empty())
        return false;
    SHFILEINFOW info{};
    if (!SHGetFileInfoW(exe.c_str(), 0, &info, sizeof(info), SHGFI_ICON | SHGFI_LARGEICON) || !info.hIcon)
        return false;
    IconImage raw;
    const bool ok = HIconToImage(info.hIcon, raw);
    DestroyIcon(info.hIcon);
    if (!ok)
        return false;
    out = FitToSquare(raw, sizePx);
    return !out.Empty();
}

// SuperMenu registers a notification icon of its own; it must never appear in the
// menu bar's tray area (the same rule the old sm::TrayReader applied).
const std::wstring& OwnExecutable()
{
    static const std::wstring path = [] {
        wchar_t buffer[MAX_PATH * 2]{};
        const DWORD length = GetModuleFileNameW(nullptr, buffer, DWORD(std::size(buffer)));
        std::wstring path(buffer, length);
        CharLowerBuffW(path.data(), DWORD(path.size()));
        return path;
    }();
    return path;
}

std::wstring Lower(std::wstring s)
{
    if (!s.empty())
        LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, s.c_str(), int(s.size()), s.data(), int(s.size()), nullptr, nullptr, 0);
    return s;
}

std::wstring VersionString(const std::wstring& path, const wchar_t* field)
{
    DWORD handle = 0, size = GetFileVersionInfoSizeW(path.c_str(), &handle);
    if (!size)
        return {};
    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data()))
        return {};
    struct Lang {
        WORD lang, codepage;
    }* langs = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&langs), &len) || len < sizeof(Lang))
        return {};
    wchar_t sub[96];
    swprintf_s(sub, L"\\StringFileInfo\\%04x%04x\\%s", langs[0].lang, langs[0].codepage, field);
    wchar_t* value = nullptr;
    if (VerQueryValueW(data.data(), sub, reinterpret_cast<void**>(&value), &len) && value && len)
        return value;
    return {};
}

// "{6D809377-...}\Tailscale\tailscale-ipn.exe": a known folder, then the rest.
std::wstring ResolveKnownFolder(const std::wstring& path)
{
    if (path.size() < 39 || path[0] != L'{')
        return path;
    GUID id{};
    if (FAILED(CLSIDFromString(path.substr(0, 38).c_str(), &id)))
        return path;
    PWSTR base = nullptr;
    std::wstring out = path;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &base)))
        out = std::wstring(base) + path.substr(38);
    CoTaskMemFree(base);
    return out;
}

bool DecodePng(const std::vector<BYTE>& bytes, IconImage& out)
{
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> conv;
    ComPtr<IStream> stream;
    stream.attach(SHCreateMemStream(bytes.data(), UINT(bytes.size())));
    if (!stream || FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic.put()))) ||
        FAILED(wic->CreateDecoderFromStream(stream.get(), nullptr, WICDecodeMetadataCacheOnLoad, decoder.put())) || FAILED(decoder->GetFrame(0, frame.put())) ||
        FAILED(wic->CreateFormatConverter(conv.put())) ||
        FAILED(conv->Initialize(frame.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
        return false;
    UINT w = 0, h = 0;
    conv->GetSize(&w, &h);
    if (!w || !h || w > 512 || h > 512)
        return false;
    out.width = int(w);
    out.height = int(h);
    out.bgra.resize(size_t(w) * h * 4);
    if (FAILED(conv->CopyPixels(nullptr, w * 4, UINT(out.bgra.size()), out.bgra.data())))
        return false;
    for (size_t i = 3; i < out.bgra.size(); i += 4)
        if (out.bgra[i])
            return true;
    return false; // an empty startup snapshot must fall back to the executable's artwork
}

// Whether an icon is a glyph in one light (1) or dark (-1) neutral shade, or
// neither (0): most of its opaque pixels white, or black, without colour. A
// coloured badge on it (a sync arrow's dot) does not count against it; an icon
// on a plate of its own (NVIDIA's logo on black) is not a glyph, nor one drawn
// with both an outline and a fill (OneDrive's), which shows on either glass.
int GlyphTone(const IconImage& img)
{
    size_t opaque = 0, light = 0, dark = 0;
    for (size_t i = 0; i + 3 < img.bgra.size(); i += 4) {
        const int a = img.bgra[i + 3];
        if (a < 64)
            continue;
        ++opaque;
        // Premultiplied: the colour itself is the value over alpha.
        const int b = img.bgra[i] * 255 / a, g = img.bgra[i + 1] * 255 / a, r = img.bgra[i + 2] * 255 / a;
        const int chroma = std::max({r, g, b}) - std::min({r, g, b}), lum = (54 * r + 183 * g + 19 * b) >> 8;
        if (chroma < 40 && lum > 200)
            ++light;
        else if (chroma < 40 && lum < 60)
            ++dark;
    }
    if (opaque < 8 || opaque * 4 > img.bgra.size() / 4 * 3)
        return 0;
    return light * 10 >= opaque * 6 ? 1 : dark * 10 >= opaque * 6 ? -1 : 0;
}

// The glyph's neutral pixels (and their soft edges) in Atrium's ink of the
// other kind (DESIGN.md, ink): #111317 at 92 % for a white glyph, white at 96 %
// for a black one. Alpha and coloured pixels stay.
IconImage InkedGlyph(const IconImage& img, int tone)
{
    IconImage out = img;
    const int ink[3] = {tone > 0 ? 0x17 : 255, tone > 0 ? 0x13 : 255, tone > 0 ? 0x11 : 255}; // b, g, r
    const int strength = tone > 0 ? 235 : 245;
    for (size_t i = 0; i + 3 < out.bgra.size(); i += 4) {
        const int a = out.bgra[i + 3];
        if (!a)
            continue;
        const int b = out.bgra[i] * 255 / a, g = out.bgra[i + 1] * 255 / a, r = out.bgra[i + 2] * 255 / a;
        const int chroma = std::max({r, g, b}) - std::min({r, g, b}), lum = (54 * r + 183 * g + 19 * b) >> 8;
        if (chroma >= 48 || (tone > 0 ? lum < 128 : lum >= 128))
            continue;
        const int na = a * strength / 255;
        for (int k = 0; k < 3; ++k)
            out.bgra[i + size_t(k)] = uint8_t(ink[k] * na / 255);
        out.bgra[i + 3] = uint8_t(na);
    }
    return out;
}

// Windows' record of the icons that have been in the tray.
struct Record {
    std::wstring id;      // Windows' record identity; separates multiple icons from one program
    std::wstring exe;     // full path, lower case
    std::wstring tip;     // tooltip when first seen; often empty
    std::vector<BYTE> png; // icon snapshot
    bool promoted = false; // shown on the taskbar
    bool hasUid = false;   // the icon's id within its program (NOTIFYICONDATA.uID)
    UINT uid = 0;
    GUID guid{};
    size_t order = SIZE_MAX; // place in Windows' order of the icons (UIOrderList), if listed
};

// Reads the records; `ordered` tells whether Windows' order of them was there.
std::vector<Record> ReadRecords(bool& ordered)
{
    std::vector<Record> out;
    ordered = false;
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\NotifyIconSettings", 0, KEY_READ, &root) != ERROR_SUCCESS)
        return out;
    // UIOrderList: the records' ids (their key names) as 64-bit numbers, in
    // the order the taskbar and its overflow window show the icons.
    std::map<unsigned long long, size_t> order;
    DWORD bytes = 0;
    if (RegGetValueW(root, nullptr, L"UIOrderList", RRF_RT_REG_BINARY, nullptr, nullptr, &bytes) == ERROR_SUCCESS && bytes >= 8 && bytes < (1u << 16)) {
        std::vector<unsigned long long> ids(bytes / 8);
        DWORD got = DWORD(ids.size() * 8);
        if (RegGetValueW(root, nullptr, L"UIOrderList", RRF_RT_REG_BINARY, nullptr, ids.data(), &got) == ERROR_SUCCESS)
            for (size_t i = 0; i < ids.size(); ++i)
                order.try_emplace(ids[i], i);
    }
    ordered = !order.empty();
    for (DWORD i = 0;; ++i) {
        wchar_t name[128];
        DWORD len = 128;
        if (RegEnumKeyExW(root, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
            break;
        Record r;
        r.id = name;
        wchar_t buf[1024];
        DWORD size = sizeof(buf);
        if (RegGetValueW(root, name, L"ExecutablePath", RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS)
            r.exe = Lower(AppExecutable(ResolveKnownFolder(buf)));
        size = sizeof(buf);
        if (RegGetValueW(root, name, L"InitialTooltip", RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS)
            r.tip = buf;
        // Some components keep an untranslated resource id in the tooltip
        // ("IDS_MAIN_PAGE_TITLE"): treat it as none and fall back to the app's name.
        if (r.tip.rfind(L"IDS_", 0) == 0 || r.tip.rfind(L"ids_", 0) == 0)
            r.tip.clear();
        size = 0;
        if (RegGetValueW(root, name, L"IconSnapshot", RRF_RT_REG_BINARY, nullptr, nullptr, &size) == ERROR_SUCCESS && size && size < (1u << 20)) {
            r.png.resize(size);
            if (RegGetValueW(root, name, L"IconSnapshot", RRF_RT_REG_BINARY, nullptr, r.png.data(), &size) != ERROR_SUCCESS || size > r.png.size())
                r.png.clear();
            else
                r.png.resize(size);
        }
        DWORD uid = 0;
        size = sizeof(uid);
        if (RegGetValueW(root, name, L"UID", RRF_RT_REG_DWORD, nullptr, &uid, &size) == ERROR_SUCCESS)
            r.hasUid = true, r.uid = uid;
        size = sizeof(buf);
        if (RegGetValueW(root, name, L"IconGuid", RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS)
            CLSIDFromString(buf, &r.guid);
        DWORD promoted = 0;
        size = sizeof(promoted);
        if (RegGetValueW(root, name, L"IsPromoted", RRF_RT_REG_DWORD, nullptr, &promoted, &size) == ERROR_SUCCESS)
            r.promoted = promoted != 0;
        if (auto it = order.find(wcstoull(name, nullptr, 10)); it != order.end())
            r.order = it->second;
        if (!r.exe.empty())
            out.push_back(std::move(r));
    }
    RegCloseKey(root);
    return out;
}

struct Program {
    std::vector<std::wstring> titles; // of its visible windows
    std::vector<HWND> windows;        // all its top-level and message-only windows
};

// Running programs by path (lower case).
std::map<std::wstring, Program> RunningPrograms()
{
    std::map<DWORD, std::wstring> paths;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe{sizeof(pe)};
    if (snap != INVALID_HANDLE_VALUE) {
        for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
            // Elevated/service-owned icons still have a shell window and UID,
            // even when Windows will not expose their full process path.
            paths[pe.th32ProcessID] = L"|" + Lower(pe.szExeFile);
            if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID)) {
                wchar_t buf[MAX_PATH * 2];
                DWORD len = DWORD(std::size(buf));
                if (QueryFullProcessImageNameW(p, 0, buf, &len))
                    paths[pe.th32ProcessID] = Lower(buf);
                CloseHandle(p);
            }
        }
        CloseHandle(snap);
    }
    std::map<std::wstring, Program> out;
    for (const auto& [pid, path] : paths)
        out[path];
    struct Ctx {
        std::map<DWORD, std::wstring>* paths;
        std::map<std::wstring, Program>* out;
        bool titles;
    } ctx{&paths, &out, true};
    auto each = [](HWND h, LPARAM l) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(l);
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        auto it = c->paths->find(pid);
        if (it == c->paths->end())
            return TRUE;
        Program& p = (*c->out)[it->second];
        p.windows.push_back(h);
        wchar_t title[256];
        if (c->titles && IsWindowVisible(h) && InternalGetWindowText(h, title, 256) > 0) // sends no message (a hung window)
            p.titles.emplace_back(title);
        return TRUE;
    };
    EnumWindows(each, reinterpret_cast<LPARAM>(&ctx));
    // Tray icons often belong to message-only windows, which EnumWindows skips.
    ctx.titles = false;
    for (HWND h = FindWindowExW(HWND_MESSAGE, nullptr, nullptr, nullptr); h; h = FindWindowExW(HWND_MESSAGE, h, nullptr, nullptr))
        each(h, reinterpret_cast<LPARAM>(&ctx));
    return out;
}

// Whether one of a program's windows has the tray icon `uid` now (the shell
// knows the icon's place; it fails for icons that are not there).
bool HasIcon(const std::vector<HWND>& windows, UINT uid, RECT& bounds, std::wstring& identity, ShellProbe& probe)
{
    for (HWND h : windows) {
        if (!probe.responsive)
            return false;
        NOTIFYICONIDENTIFIER id{sizeof(id), h, uid, GUID_NULL};
        RECT r{};
        if (SUCCEEDED(ShellNotifyIconRect(id, r, probe))) {
            bounds = r;
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            identity = L"uid:" + std::to_wstring(pid) + L":" + std::to_wstring(reinterpret_cast<uintptr_t>(h)) + L":" + std::to_wstring(uid);
            return true;
        }
    }
    return false;
}

bool LiveIcon(const Record& record, const Program& program, RECT& bounds, std::wstring& identity, ShellProbe& probe)
{
    if (record.guid == GUID_NULL)
        return record.hasUid && HasIcon(program.windows, record.uid, bounds, identity, probe);
    NOTIFYICONIDENTIFIER id{sizeof(id), nullptr, 0, record.guid};
    if (FAILED(ShellNotifyIconRect(id, bounds, probe)))
        return false;
    wchar_t guid[40]{};
    StringFromGUID2(record.guid, guid, 40);
    identity = L"guid:" + std::wstring(guid); // liveness only, NOT process ownership
    return true;
}

struct Found {
    ComPtr<IUIAutomationElement> element;
    HWND host = nullptr;    // the XAML button's top-level window: the taskbar or the overflow popup
    HWND toolbar = nullptr; // a button of the classic tray's toolbar (Windows 10): clicked by messages to it
    std::wstring tip;
    bool promoted = false;
    int index = 0;
    RECT bounds{};
    std::wstring recordId;
    tray::Method method = tray::Method::Unknown;
    std::vector<int> runtimeId;
};

tray::ShellSession ReadShellSession()
{
    tray::ShellSession session;
    const HWND window = ShellTaskbar();
    DWORD pid = 0;
    if (!window || !GetWindowThreadProcessId(window, &pid))
        return session;
    if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        FILETIME created{}, exited{}, kernel{}, user{};
        if (GetProcessTimes(process, &created, &exited, &kernel, &user))
            session = {pid, (uint64_t(created.dwHighDateTime) << 32) | created.dwLowDateTime, uint64_t(reinterpret_cast<uintptr_t>(window))};
        CloseHandle(process);
    }
    return session;
}

std::vector<int> RuntimeId(IUIAutomationElement* element)
{
    SAFEARRAY* id = nullptr;
    std::vector<int> out;
    if (element && SUCCEEDED(element->GetRuntimeId(&id)) && id) {
        LONG first = 0, last = -1;
        if (SUCCEEDED(SafeArrayGetLBound(id, 1, &first)) && SUCCEEDED(SafeArrayGetUBound(id, 1, &last)))
            for (LONG i = first; i <= last && i - first < 64; ++i) {
                int value = 0;
                if (FAILED(SafeArrayGetElement(id, &i, &value))) {
                    out.clear();
                    break;
                }
                out.push_back(value);
            }
    }
    if (id)
        SafeArrayDestroy(id);
    return out;
}

// The tray buttons in the taskbar and in the overflow window, in order.
std::vector<Found> FindButtons(IUIAutomation* uia, bool& available)
{
    std::vector<Found> out;
    available = false;
    auto collect = [&](HWND host, bool promoted) {
        HWND bridge = host ? FindWindowExW(host, nullptr, L"Windows.UI.Composition.DesktopWindowContentBridge", nullptr) : nullptr;
        ComPtr<IUIAutomationElement> root;
        if (!host || FAILED(uia->ElementFromHandle(bridge ? bridge : host, root.put())) || !root)
            return;
        available = true;
        ComPtr<IUIAutomationCondition> cond;
        VARIANT v;
        v.vt = VT_BSTR;
        v.bstrVal = SysAllocString(L"NotifyItemIcon");
        HRESULT hr = uia->CreatePropertyCondition(UIA_AutomationIdPropertyId, v, cond.put());
        VariantClear(&v);
        ComPtr<IUIAutomationElementArray> all;
        if (FAILED(hr) || FAILED(root->FindAll(TreeScope_Descendants, cond.get(), all.put())) || !all)
            return;
        int n = 0;
        all->get_Length(&n);
        for (int i = 0; i < n; ++i) {
            Found f;
            all->GetElement(i, f.element.put());
            if (!f.element)
                continue;
            f.host = host;
            BSTR name = nullptr;
            if (f.element)
                f.element->get_CurrentName(&name);
            f.tip = name ? name : L"";
            SysFreeString(name);
            f.promoted = promoted;
            f.index = int(std::count_if(out.begin(), out.end(), [&](const Found& o) { return o.promoted == promoted; }));
            if (f.element)
                f.element->get_CurrentBoundingRectangle(&f.bounds);
            f.runtimeId = RuntimeId(f.element.get());
            out.push_back(std::move(f));
        }
    };
    collect(ShellTaskbar(), true);
    // The overflow's XAML tree can be materialised (and its buttons kept alive) by a
    // UIA query after the popup was closed once. Its buttons are only the system
    // tray's *current* overflow content while the popup is on screen; otherwise the
    // live records below stand for those icons (with their snapshot artwork).
    if (HWND overflow = FindWindowW(L"TopLevelWindowForOverflowXamlIsland", nullptr); overflow && IsWindowVisible(overflow))
        collect(overflow, false);
    if (!out.empty())
        return out;
    // No XAML tray (Windows 10, and Windows 11 before its new tray): the classic one, a toolbar of
    // buttons on the taskbar and another in the overflow window, named by their tooltips too.
    auto legacy = [&](HWND toolbar, bool promoted) {
        ComPtr<IUIAutomationElement> root;
        if (!toolbar || FAILED(uia->ElementFromHandle(toolbar, root.put())) || !root)
            return;
        available = true;
        VARIANT v;
        v.vt = VT_I4;
        v.lVal = UIA_ButtonControlTypeId;
        ComPtr<IUIAutomationCondition> cond;
        ComPtr<IUIAutomationElementArray> all;
        if (FAILED(uia->CreatePropertyCondition(UIA_ControlTypePropertyId, v, cond.put())) ||
            FAILED(root->FindAll(TreeScope_Children, cond.get(), all.put())) || !all)
            return;
        int n = 0;
        all->get_Length(&n);
        for (int i = 0; i < n; ++i) {
            Found f;
            all->GetElement(i, f.element.put());
            if (!f.element)
                continue;
            BSTR name = nullptr;
            f.element->get_CurrentName(&name);
            f.tip = name ? name : L"";
            SysFreeString(name);
            f.promoted = promoted;
            f.index = int(std::count_if(out.begin(), out.end(), [&](const Found& o) { return o.promoted == promoted; }));
            f.element->get_CurrentBoundingRectangle(&f.bounds);
            f.toolbar = toolbar;
            f.runtimeId = RuntimeId(f.element.get());
            out.push_back(std::move(f));
        }
    };
    HWND tray = ShellTaskbar();
    HWND notify = tray ? FindWindowExW(tray, nullptr, L"TrayNotifyWnd", nullptr) : nullptr;
    HWND pager = notify ? FindWindowExW(notify, nullptr, L"SysPager", nullptr) : nullptr;
    legacy(pager ? FindWindowExW(pager, nullptr, L"ToolbarWindow32", nullptr) : nullptr, true);
    HWND overflow = FindWindowW(L"NotifyIconOverflowWindow", nullptr);
    legacy(overflow ? FindWindowExW(overflow, nullptr, L"ToolbarWindow32", nullptr) : nullptr, false);
    return out;
}

// An explicit fallback opens ONLY the popup, never briefly shows/hides the taskbar.
// Verify realized content; HWND visibility alone does not imply a usable XAML popup.
HWND OverflowWindow()
{
    HWND h = FindWindowW(L"TopLevelWindowForOverflowXamlIsland", nullptr);
    return h ? h : FindWindowW(L"NotifyIconOverflowWindow", nullptr);
}
bool OverflowUsable(IUIAutomation* uia)
{
    HWND overflow = OverflowWindow();
    if (!uia || !overflow || !IsWindowVisible(overflow))
        return false;
    bool available = false;
    const auto buttons = FindButtons(uia, available);
    for (const auto& button : buttons) {
        if (button.promoted || !button.element || IsRectEmpty(&button.bounds))
            continue;
        BOOL offscreen = TRUE;
        if (FAILED(button.element->get_CurrentIsOffscreen(&offscreen)) || offscreen)
            continue;
        const POINT at{button.bounds.left + (button.bounds.right - button.bounds.left) / 2, button.bounds.top + (button.bounds.bottom - button.bounds.top) / 2};
        HWND hit = WindowFromPoint(at);
        if (hit == overflow || GetAncestor(hit, GA_ROOT) == overflow)
            return true;
    }
    return false;
}
bool ShowOverflow(IUIAutomation* uia)
{
    if (OverflowUsable(uia))
        return true;
    // Showing the HWND alone does NOT uncollapse XAML content. Invoke the shell's chevron.
    // Its non-localized structural signature is a NormalButton/SystemTrayIcon before the
    // first promoted NotifyItemIcon. Require that anchor and a later normal system button;
    // builds without this signature fail closed instead of guessing from translated names.
    HWND tray = ShellTaskbar();
    HWND bridge = tray ? FindWindowExW(tray, nullptr, L"Windows.UI.Composition.DesktopWindowContentBridge", nullptr) : nullptr;
    if (tray && !bridge) {
        // Classic shell: the unique Button directly inside TrayNotifyWnd is its chevron.
        // BM_CLICK is the documented Button action, not pointer injection or toolbar memory IO.
        HWND notify = FindWindowExW(tray, nullptr, L"TrayNotifyWnd", nullptr);
        HWND button = notify ? FindWindowExW(notify, nullptr, L"Button", nullptr) : nullptr;
        if (!button || FindWindowExW(notify, button, L"Button", nullptr))
            return false;
        const BOOL queued = PostMessageW(button, BM_CLICK, 0, 0);
        Log(L"tray: overflow classic BM_CLICK queued=%d error=%lu", queued, queued ? 0 : GetLastError());
        if (!queued)
            return false;
        for (int i = 0; i < 10; ++i) {
            if (OverflowUsable(uia))
                return true;
            Sleep(20);
        }
        return false;
    }
    ComPtr<IUIAutomationElement> root, chevron;
    ComPtr<IUIAutomationCondition> condition;
    ComPtr<IUIAutomationElementArray> elements;
    if (!uia || !bridge || FAILED(uia->ElementFromHandle(bridge, root.put())) || !root)
        return false;
    VARIANT value{};
    value.vt = VT_I4;
    value.lVal = UIA_ButtonControlTypeId;
    if (FAILED(uia->CreatePropertyCondition(UIA_ControlTypePropertyId, value, condition.put())) ||
        FAILED(root->FindAll(TreeScope_Descendants, condition.get(), elements.put())) || !elements)
        return false;
    int count = 0, before = 0, after = 0;
    bool anchor = false;
    elements->get_Length(&count);
    for (int i = 0; i < count; ++i) {
        ComPtr<IUIAutomationElement> element;
        elements->GetElement(i, element.put());
        if (!element)
            continue;
        BSTR id = nullptr, cls = nullptr;
        element->get_CurrentAutomationId(&id);
        element->get_CurrentClassName(&cls);
        if (id && wcscmp(id, L"NotifyItemIcon") == 0)
            anchor = true;
        if (id && cls && wcscmp(id, L"SystemTrayIcon") == 0 && wcscmp(cls, L"SystemTray.NormalButton") == 0) {
            if (!anchor) {
                ++before;
                chevron = element;
            } else
                ++after;
        }
        SysFreeString(id);
        SysFreeString(cls);
    }
    if (!anchor || before != 1 || after < 1 || !chevron) {
        Log(L"tray: overflow chevron ambiguous anchor=%d before=%d after=%d", anchor, before, after);
        return false;
    }
    ComPtr<IUIAutomationInvokePattern> invoke;
    HRESULT hr = chevron->GetCurrentPatternAs(UIA_InvokePatternId, kIidInvoke, invoke.put_void());
    if (SUCCEEDED(hr))
        hr = invoke ? invoke->Invoke() : E_NOINTERFACE;
    Log(L"tray: overflow chevron hr=0x%08X taskbarVisible=%d", unsigned(hr), IsWindowVisible(tray));
    if (FAILED(hr))
        return false;
    for (int i = 0; i < 10; ++i) {
        if (OverflowUsable(uia)) {
            Log(L"tray: overflow verified visible content and hit target");
            return true;
        }
        Sleep(20);
    }
    Log(L"tray: overflow accepted but content not usable");
    return false;
}

struct ReadDiagnostics {
    size_t records = 0, live = 0, aliases = 0, shared = 0, unknown = 0;
};
std::vector<int> MatchButtons(std::vector<Found>& buttons, std::vector<Record>& records, bool& ordered, ReadDiagnostics* diagnostics = nullptr)
{
    records = ReadRecords(ordered);
    const auto running = RunningPrograms();
    static std::map<std::wstring, std::pair<std::wstring, std::wstring>> version;
    if (version.size() > 512)
        version.clear();
    auto names = [&](const std::wstring& path, const Program& program, const std::wstring& tip, std::vector<std::wstring>& out, size_t& firstTitle) {
        auto [at, fresh] = version.try_emplace(path);
        if (fresh)
            at->second = {VersionString(path, L"FileDescription"), VersionString(path, L"ProductName")};
        std::wstring stem = PathFindFileNameW(path.c_str());
        if (auto dot = stem.rfind(L'.'); dot != std::wstring::npos)
            stem.resize(dot);
        out = {tip, at->second.first, at->second.second, stem};
        firstTitle = out.size();
        out.insert(out.end(), program.titles.begin(), program.titles.end());
    };
    auto rect = [](const RECT& r) { return tray::Rect{r.left, r.top, r.right, r.bottom}; };
    std::vector<tray::Button> bs;
    bs.reserve(buttons.size());
    for (const auto& b : buttons)
        bs.push_back({b.tip, b.promoted, rect(b.bounds)});
    std::vector<tray::Record> rs;
    const Program empty;
    ShellProbe probe;
    for (const auto& r : records) {
        auto run = running.find(r.exe);
        // A filename-only process result is not proof of a full executable path.
        const Program& program = run == running.end() ? empty : run->second;
        tray::Record value;
        value.id = r.id;
        value.exe = r.exe;
        value.tip = r.tip;
        value.promoted = r.promoted;
        value.order = r.order;
        RECT bounds{};
        value.live = LiveIcon(r, program, bounds, value.identity, probe);
        value.bounds = rect(bounds);
        if (value.live)
            names(r.exe, program, r.tip, value.names, value.firstTitle);
        rs.push_back(std::move(value));
    }
    std::vector<tray::Program> ps;
    const auto initial = tray::Match(bs, rs);
    const bool needPrograms = std::ranges::any_of(initial.items, [](const auto& item) { return item.button != tray::kNone && item.record == tray::kNone; });
    for (const auto& [path, program] : running) {
        if (!needPrograms || path.empty() || path[0] == L'|' || program.windows.empty() ||
            std::ranges::any_of(rs, [&](const auto& r) { return r.live && r.exe == path; }))
            continue;
        tray::Program value;
        value.exe = path;
        names(path, program, {}, value.names, value.firstTitle);
        ps.push_back(std::move(value));
    }
    const auto result = tray::Match(bs, rs, ps);
    if (diagnostics) {
        *diagnostics = {};
        diagnostics->records = rs.size();
        for (size_t r = 0; r < rs.size(); ++r) {
            diagnostics->live += rs[r].live;
            diagnostics->aliases += result.aliases[r] != tray::kNone && result.aliases[r] != r;
            diagnostics->shared += result.sharedPlace[r];
        }
        for (const auto& item : result.items)
            diagnostics->unknown += item.method == tray::Method::Unknown;
    }
    std::vector<int> match;
    for (const auto& item : result.items) {
        if (item.button == tray::kNone) {
            const auto& r = rs[item.record];
            Found f;
            f.tip = r.tip;
            if (f.tip.empty() && r.names.size() > 1)
                f.tip = r.names[1];
            if (f.tip.empty() && r.names.size() > 3)
                f.tip = r.names[3];
            f.promoted = r.promoted;
            f.index = int(std::count_if(buttons.begin(), buttons.end(), [&](const Found& b) { return b.promoted == f.promoted; }));
            buttons.push_back(std::move(f)); // no element or fake clickable rectangle
        }
        Found& f = buttons[match.size()];
        f.method = item.method;
        if (item.record != tray::kNone) {
            f.recordId = rs[item.record].identity;
            match.push_back(int(item.record));
        } else if (item.program != tray::kNone) {
            Record r;
            r.exe = ps[item.program].exe;
            r.id = L"run:" + r.exe;
            records.push_back(std::move(r));
            match.push_back(int(records.size() - 1));
        } else
            match.push_back(-1);
    }
    return match;
}

const wchar_t* RouteName(tray::Route route)
{
    switch (route) {
    case tray::Route::Invoke: return L"uia-invoke";
    case tray::Route::ContextMenu: return L"uia-context-menu";
    case tray::Route::LegacyDefault: return L"legacy-default";
    default: return L"toolbar-messages";
    }
}

HRESULT ToolbarClick(const Found& target, tray::Action action)
{
    wchar_t cls[80]{};
    if (!IsWindow(target.toolbar) || !GetClassNameW(target.toolbar, cls, 80) || wcscmp(cls, L"ToolbarWindow32") != 0)
        return E_INVALIDARG;
    HWND root = GetAncestor(target.toolbar, GA_ROOT);
    GetClassNameW(root, cls, 80);
    if (wcscmp(cls, L"Shell_TrayWnd") != 0 && wcscmp(cls, L"NotifyIconOverflowWindow") != 0)
        return E_INVALIDARG;
    DWORD pid = 0, shellPid = 0;
    GetWindowThreadProcessId(root, &pid);
    GetWindowThreadProcessId(ShellTaskbar(), &shellPid);
    if (!pid || pid != shellPid)
        return E_INVALIDARG;
    RECT bounds{}, client{};
    if (!target.element || FAILED(target.element->get_CurrentBoundingRectangle(&bounds)) || IsRectEmpty(&bounds) || !GetClientRect(target.toolbar, &client))
        return UIA_E_ELEMENTNOTAVAILABLE;
    POINT at{bounds.left + (bounds.right - bounds.left) / 2, bounds.top + (bounds.bottom - bounds.top) / 2};
    if (!ScreenToClient(target.toolbar, &at) || !PtInRect(&client, at) || at.x > 32767 || at.y > 32767)
        return E_INVALIDARG;
    const LPARAM position = MAKELPARAM(at.x, at.y);
    DWORD error = 0;
    const auto sent = tray::PostClassicGesture(action, [&](tray::MouseEvent event) {
        UINT message = WM_MOUSEMOVE;
        WPARAM keys = 0;
        switch (event) {
        case tray::MouseEvent::LeftDown:
            message = WM_LBUTTONDOWN;
            keys = MK_LBUTTON;
            break;
        case tray::MouseEvent::LeftUp: message = WM_LBUTTONUP; break;
        case tray::MouseEvent::LeftDouble:
            message = WM_LBUTTONDBLCLK;
            keys = MK_LBUTTON;
            break;
        case tray::MouseEvent::RightDown:
            message = WM_RBUTTONDOWN;
            keys = MK_RBUTTON;
            break;
        case tray::MouseEvent::RightUp: message = WM_RBUTTONUP; break;
        default: break;
        }
        if (PostMessageW(target.toolbar, message, keys, position))
            return true;
        error = GetLastError();
        return false;
    });
    Log(L"tray: toolbar queued=%zu complete=%d actionQueued=%d error=%lu", sent.queued, sent.complete, sent.actionQueued, error);
    // A failure AFTER DOWN is not a safe retry: Explorer may already have notified the app.
    // S_FALSE records partial delivery as uncertain, so Dispatch will not send another action.
    return sent.complete ? S_OK : sent.actionQueued ? S_FALSE : error ? HRESULT_FROM_WIN32(error) : E_FAIL;
}

struct ResponseState {
    std::set<HWND> visible, menus;
    HWND foreground = nullptr;
};
ResponseState CaptureResponse(const std::wstring& exe)
{
    ResponseState state;
    if (exe.empty())
        return state;
    struct Context {
        const std::wstring& exe;
        ResponseState& state;
        std::map<DWORD, bool> owned;
    } ctx{exe, state, {}};
    EnumWindows(
        [](HWND h, LPARAM param) -> BOOL {
            auto& c = *reinterpret_cast<Context*>(param);
            if (!IsWindowVisible(h))
                return TRUE;
            DWORD pid = 0;
            const DWORD thread = GetWindowThreadProcessId(h, &pid);
            auto [it, fresh] = c.owned.try_emplace(pid, false);
            if (fresh) {
                if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
                    wchar_t path[32768];
                    DWORD size = DWORD(std::size(path));
                    it->second = QueryFullProcessImageNameW(process, 0, path, &size) && _wcsicmp(path, c.exe.c_str()) == 0;
                    CloseHandle(process);
                }
            }
            if (!it->second)
                return TRUE;
            c.state.visible.insert(h);
            if (GetForegroundWindow() == h)
                c.state.foreground = h;
            GUITHREADINFO gui{sizeof(gui)};
            if (GetGUIThreadInfo(thread, &gui) && (gui.flags & GUI_INMENUMODE) && gui.hwndMenuOwner)
                c.state.menus.insert(gui.hwndMenuOwner);
            wchar_t cls[32]{};
            if (GetClassNameW(h, cls, 32) && wcscmp(cls, L"#32768") == 0)
                c.state.menus.insert(h);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    return state;
}
bool ResponseChanged(const ResponseState& before, const ResponseState& after, tray::Action action)
{
    for (HWND h : after.menus)
        if (!before.menus.contains(h))
            return true;
    for (HWND h : after.visible)
        if (!before.visible.contains(h))
            return true;
    return action != tray::Action::Right && after.foreground && before.foreground != after.foreground;
}
bool ForegroundExisting(const std::wstring& exe)
{
    const auto state = CaptureResponse(exe);
    for (HWND h : state.visible) {
        if (GetWindow(h, GW_OWNER) || (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW))
            continue;
        if (IsIconic(h))
            ShowWindowAsync(h, SW_RESTORE);
        SetForegroundWindow(h);
        for (int i = 0; i < 5; ++i) {
            if (GetForegroundWindow() == h && !IsIconic(h))
                return true;
            Sleep(20);
        }
    }
    return false;
}

// The last resort for a right click the shell accepted without a visible menu (#28):
// one real right click on the live button. Explorer's taskbar (and, for an overflow
// icon, its overflow popup) is shown for the moment of the click; the pointer is
// moved back only if the user has not moved it meanwhile. Any user input cancels it.
struct TaskbarClickReveal {
    tray::TaskbarReveal reveal;
    bool active = reveal.active();
};

struct TrayPointerDesktop {
    IUIAutomation* uia;
    IUIAutomationElement* element;
    HWND host;

    bool Cursor(POINT& p) const { return GetCursorPos(&p) != FALSE; }
    bool Move(POINT p) const { return SetCursorPos(p.x, p.y) != FALSE; }
    void Wait(DWORD ms) const { Sleep(ms); }
    bool Interacting() const
    {
        constexpr int kKeys[] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2, VK_SHIFT, VK_CONTROL, VK_MENU, VK_LWIN, VK_RWIN};
        return std::ranges::any_of(kKeys, [](int key) { return (GetAsyncKeyState(key) & 0x8000) != 0; });
    }
    bool Target(POINT& p) const
    {
        RECT bounds{}, on{};
        if (!element || !host || !IsWindowVisible(host) || FAILED(element->get_CurrentBoundingRectangle(&bounds)) || IsRectEmpty(&bounds) ||
            !GetWindowRect(host, &on))
            return false;
        p = {bounds.left + (bounds.right - bounds.left) / 2, bounds.top + (bounds.bottom - bounds.top) / 2};
        return PtInRect(&on, p) && MonitorFromPoint(p, MONITOR_DEFAULTTONULL);
    }
    bool TargetAt(POINT p) const
    {
        HWND under = WindowFromPoint(p);
        if (!under || (GetAncestor(under, GA_ROOT) != host && !IsChild(host, under)))
            return false;
        ComPtr<IUIAutomationElement> hit;
        ComPtr<IUIAutomationTreeWalker> walker;
        if (FAILED(uia->ElementFromPoint(p, hit.put())) || !hit || FAILED(uia->get_RawViewWalker(walker.put())) || !walker)
            return false;
        // The point may hit the icon's image or text: it must resolve to this exact live button.
        for (int depth = 0; hit && depth < 12; ++depth) {
            BOOL same = FALSE;
            if (SUCCEEDED(uia->CompareElements(element, hit.get(), &same)) && same)
                return true;
            ComPtr<IUIAutomationElement> parent;
            if (FAILED(walker->GetParentElement(hit.get(), parent.put())))
                break;
            hit = std::move(parent);
        }
        return false;
    }
    bool Send(POINT p) const
    {
        const LONG x = GetSystemMetrics(SM_XVIRTUALSCREEN), y = GetSystemMetrics(SM_YVIRTUALSCREEN);
        const RECT desktop{x, y, x + GetSystemMetrics(SM_CXVIRTUALSCREEN), y + GetSystemMetrics(SM_CYVIRTUALSCREEN)};
        std::array<INPUT, 3> input{};
        if (!tray::RightClickInputs(p, desktop, input))
            return false;
        const UINT sent = SendInput(UINT(input.size()), input.data(), sizeof(INPUT));
        if (sent == 2) {
            // Position and down were inserted, the release was not: never leave the button down.
            INPUT release{};
            release.type = INPUT_MOUSE;
            release.mi.dwFlags = MOUSEEVENTF_RIGHTUP;
            SendInput(1, &release, sizeof(INPUT));
        }
        LASTINPUTINFO last{sizeof(last)};
        if (GetLastInputInfo(&last))
            g_syntheticInput = last.dwTime; // the menu that follows is this click's
        return sent == input.size();
    }
};

HRESULT PointerRightClick(IUIAutomation* uia, const Found& target)
{
    TaskbarClickReveal reveal;
    if (!reveal.active)
        return E_ABORT;
    HWND opened = nullptr;
    if (!target.promoted) {
        HWND before = OverflowWindow();
        const bool visible = before && IsWindowVisible(before);
        if (!ShowOverflow(uia))
            return E_FAIL;
        if (!visible)
            opened = OverflowWindow();
    }
    // Not the chevron again: that is another activation and can dismiss the app's new menu.
    std::unique_ptr<HWND__, void (*)(HWND)> hide(opened, [](HWND h) {
        if (IsWindow(h))
            ShowWindowAsync(h, SW_HIDE);
    });
    // Showing the overflow can materialise and reorder its buttons: the same UIA element, never another.
    bool available = false;
    auto buttons = FindButtons(uia, available);
    auto it = std::ranges::find_if(buttons, [&](const Found& f) {
        BOOL same = FALSE;
        return f.element && SUCCEEDED(uia->CompareElements(target.element.get(), f.element.get(), &same)) && same;
    });
    if (it == buttons.end())
        return E_ABORT;
    TrayPointerDesktop desktop{uia, it->element.get(), it->host};
    const auto result = tray::RightClick(desktop);
    return result == tray::PointerClick::Sent ? S_OK : result == tray::PointerClick::Cancelled ? E_ABORT : E_FAIL;
}

} // namespace

DWORD TrayModel::LastSyntheticInput() { return g_syntheticInput; }

std::wstring TrayModel::Title(const std::wstring& tip) { return tray::Title(tip); }

TrayModel::TrayModel(HWND notify, UINT msg) : notify_(notify), msg_(msg)
{
    thread_ = std::thread([this] { Worker(); });
}

TrayModel::~TrayModel()
{
    {
        std::lock_guard g(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    thread_.join();
}

void TrayModel::Refresh()
{
    std::lock_guard g(mutex_);
    if (std::ranges::none_of(jobs_, [](const Job& j) { return j.refresh; }))
        jobs_.push_back({true, {}});
    cv_.notify_all();
}

void TrayModel::SetHosting(bool on)
{
    if (on == (host_ != nullptr))
        return;
    // Built and torn down outside the lock: the host's thread starts or stops meanwhile.
    std::unique_ptr<TrayHost> host = on ? std::make_unique<TrayHost>(notify_, msg_) : nullptr;
    if (host && !host->Running())
        host.reset();
    {
        std::lock_guard g(mutex_);
        std::swap(host, host_);
        hostInk_.clear();
    }
    host.reset();
    PostMessageW(notify_, msg_, 0, 0);
}

std::vector<std::wstring> TrayModel::Flashing() const
{
    std::lock_guard g(mutex_);
    return host_ && !host_->Yielded() ? host_->Flashing() : std::vector<std::wstring>{};
}

bool TrayModel::Hosting() const
{
    return host_ != nullptr;
}

bool TrayModel::Yielded() const
{
    return host_ && host_->Yielded();
}

void TrayModel::SetHostPlaces(RECT anchor)
{
    if (host_)
        host_->SetPlaces(anchor);
}

void TrayModel::MiddleClick(const TrayIcon& icon)
{
    POINT at{};
    GetCursorPos(&at);
    if (host_ && !icon.hostKey.empty()) {
        host_->Hover(L"", at); // a click ends the hover and any popup it asked for
        host_->Send(icon.hostKey, tray::HostAction::Middle, at);
    }
}

void TrayModel::Hover(const TrayIcon* icon)
{
    POINT at{};
    GetCursorPos(&at);
    if (host_)
        host_->Hover(icon ? icon->hostKey : L"", at);
}

bool TrayModel::ClickHosted(const TrayIcon& icon, tray::HostAction hostAction)
{
    if (icon.hostKey.empty())
        return false;
    POINT at{};
    GetCursorPos(&at);
    if (host_)
        host_->Hover(L"", at); // a click ends the hover and any popup it asked for
    if (host_ && host_->Send(icon.hostKey, hostAction, at))
        return true;
    const tray::Action action = hostAction == tray::HostAction::Right              ? tray::Action::Right :
                                hostAction == tray::HostAction::Double             ? tray::Action::Double :
                                hostAction == tray::HostAction::DoubleContinuation ? tray::Action::DoubleContinuation :
                                                                                     tray::Action::Left;
    // The host could not send it (an elevated app, the host off): Explorer's button, by program.
    TrayIcon fallback;
    {
        std::lock_guard g(mutex_);
        for (const TrayIcon& t : icons_)
            if (!icon.exe.empty() && t.exe == icon.exe) {
                fallback = t;
                break;
            }
    }
    if (fallback.exe.empty())
        fallback.exe = icon.exe;
    std::lock_guard g(mutex_);
    jobs_.insert(std::ranges::find_if(jobs_, [](const Job& j) { return j.refresh; }), {false, fallback, action});
    cv_.notify_all();
    return true;
}

void TrayModel::Click(const TrayIcon& icon, bool right)
{
    if (ClickHosted(icon, right ? tray::HostAction::Right : tray::HostAction::Left))
        return;
    std::lock_guard g(mutex_);
    jobs_.insert(std::ranges::find_if(jobs_, [](const Job& j) { return j.refresh; }), {false, icon, right ? tray::Action::Right : tray::Action::Left});
    cv_.notify_all();
}

void TrayModel::DoubleClick(const TrayIcon& icon, bool continuation)
{
    if (ClickHosted(icon, continuation ? tray::HostAction::DoubleContinuation : tray::HostAction::Double))
        return;
    std::lock_guard g(mutex_);
    jobs_.insert(std::ranges::find_if(jobs_, [](const Job& j) { return j.refresh; }),
                 {false, icon, continuation ? tray::Action::DoubleContinuation : tray::Action::Double});
    cv_.notify_all();
}
void TrayModel::OpenSystemTray()
{
    std::lock_guard g(mutex_);
    jobs_.insert(std::ranges::find_if(jobs_, [](const Job& j) { return j.refresh; }), {false, {}, tray::Action::Left, true});
    cv_.notify_all();
}

std::vector<TrayIcon> TrayModel::Icons() const
{
    std::lock_guard g(mutex_);
    // Windows' own order (the system tray's), whatever was built of Explorer's tray when
    // it was read: the same icon stays in the same place from one opening to the next.
    auto inWindowsOrder = [](std::vector<TrayIcon> list) {
        std::ranges::stable_sort(list, [](const TrayIcon& a, const TrayIcon& b) { return a.order < b.order; });
        for (size_t i = 0; i < list.size(); ++i)
            list[i].index = int(i);
        return list;
    };
    auto shown = [](std::vector<TrayIcon> list) {
        std::erase_if(list, [](const TrayIcon& icon) { return !icon.exe.empty() && icon.exe == OwnExecutable(); });
        return list;
    };
    if (!host_ || host_->Yielded())
        return inWindowsOrder(shown(icons_));
    // The hosted icons in Windows' order (where Explorer's tray has the program), the
    // rest in arrival order after them; Explorer-only icons keep their place.
    std::vector<HostIcon> hosted = host_->Icons();
    std::erase_if(hosted, [](const HostIcon& h) { return h.hidden; });
    // Pair every read-back icon with the host's live version of it. Identity first:
    // a program can own several icons and the host's arrival order need not match
    // Explorer's UIOrderList order, so pairing same-exe icons by arrival would attach
    // icon A's picture/key to icon B (and a click would then send B's callback).
    // The record's identity (GUID, or the HWND/uID that passed the shell probe) also
    // pairs an elevated app whose process path the host cannot read. Only then fall
    // back to the exact program path, and finally to a unique tooltip.
    std::vector<size_t> hostOf(icons_.size(), SIZE_MAX);
    std::vector<bool> used(hosted.size(), false);
    auto take = [&](size_t readback, size_t host) {
        hostOf[readback] = host;
        used[host] = true;
    };
    {
        auto hostHwndUid = [](const std::wstring& hostKey, unsigned long long& window, unsigned& uid) {
            if (hostKey.rfind(L"hwnd:", 0) != 0)
                return false;
            const wchar_t* p = hostKey.c_str() + 5;
            wchar_t* end = nullptr;
            window = wcstoull(p, &end, 16);
            if (!end || *end != L':' || window == 0)
                return false;
            const unsigned long long value = wcstoull(end + 1, &end, 10);
            if (!end || *end != L'\0' || value > 0xFFFFFFFFull)
                return false;
            uid = static_cast<unsigned>(value);
            return true;
        };
        // "uid:<pid>:<hwnd>:<uid>" (the identity the shell probe proved).
        auto recordHwndUid = [](const std::wstring& identity, unsigned long long& window, unsigned& uid) {
            if (identity.rfind(L"uid:", 0) != 0)
                return false;
            const wchar_t* p = identity.c_str() + 4;
            wchar_t* end = nullptr;
            wcstoull(p, &end, 10); // the owner PID: not part of the pairing
            if (!end || *end != L':')
                return false;
            window = wcstoull(end + 1, &end, 10);
            if (!end || *end != L':')
                return false;
            const unsigned long long value = wcstoull(end + 1, &end, 10);
            if (!end || *end != L'\0' || value > 0xFFFFFFFFull)
                return false;
            uid = static_cast<unsigned>(value);
            return true;
        };
        std::map<std::wstring, size_t> byGuid;                                  // one icon per GUID
        std::map<std::pair<unsigned long long, unsigned>, size_t> byHwndUid;   // one icon per window/uID
        auto add = [&](auto& map, const auto& key, size_t index) {
            const auto [it, fresh] = map.try_emplace(key, index);
            if (!fresh)
                it->second = SIZE_MAX; // an ambiguous identity never auto-pairs
        };
        for (size_t i = 0; i < icons_.size(); ++i) {
            if (icons_[i].recordId.empty())
                continue;
            unsigned long long window = 0;
            unsigned uid = 0;
            if (icons_[i].recordId.rfind(L"guid:", 0) == 0)
                add(byGuid, icons_[i].recordId, i);
            else if (recordHwndUid(icons_[i].recordId, window, uid))
                add(byHwndUid, std::make_pair(window, uid), i);
        }
        for (size_t h = 0; h < hosted.size(); ++h) {
            if (used[h])
                continue;
            size_t match = SIZE_MAX;
            unsigned long long window = 0;
            unsigned uid = 0;
            if (hosted[h].key.rfind(L"guid:", 0) == 0) {
                const auto it = byGuid.find(hosted[h].key);
                if (it != byGuid.end())
                    match = it->second;
            } else if (hostHwndUid(hosted[h].key, window, uid)) {
                const auto it = byHwndUid.find({window, uid});
                if (it != byHwndUid.end())
                    match = it->second;
            }
            if (match != SIZE_MAX && hostOf[match] == SIZE_MAX)
                take(match, h);
        }
    }
    for (size_t i = 0; i < icons_.size(); ++i) {
        if (hostOf[i] != SIZE_MAX || icons_[i].exe.empty())
            continue;
        for (size_t h = 0; h < hosted.size(); ++h)
            if (!used[h] && !hosted[h].exe.empty() && hosted[h].exe == icons_[i].exe) {
                take(i, h);
                break;
            }
    }
    {
        std::map<std::wstring, std::vector<size_t>> readbackByTitle, hostByTitle;
        for (size_t i = 0; i < icons_.size(); ++i)
            if (hostOf[i] == SIZE_MAX && !icons_[i].tip.empty())
                readbackByTitle[Title(icons_[i].tip)].push_back(i);
        for (size_t h = 0; h < hosted.size(); ++h)
            if (!used[h] && hosted[h].exe.empty() && !hosted[h].tip.empty())
                hostByTitle[Title(hosted[h].tip)].push_back(h);
        for (const auto& [title, hosts] : hostByTitle) {
            const auto it = readbackByTitle.find(title);
            if (hosts.size() == 1 && it != readbackByTitle.end() && it->second.size() == 1)
                take(it->second.front(), hosts.front());
        }
    }
    static const tray::ShellSession kHostShell{1, 1, 1};
    auto make = [&](const HostIcon& h) {
        TrayIcon t;
        t.tip = h.tip;
        t.exe = h.exe;
        t.hostKey = h.key;
        t.pid = h.pid;
        t.appTooltip = h.version >= 4 && !h.showTip;
        t.flashing = h.flashUntil > GetTickCount64();
        const std::wstring uidKey = L"uid:" + h.exe + L"|" + std::to_wstring(h.uid);
        for (const std::wstring& key : {h.key, uidKey, L"exe:" + h.exe})
            if (auto o = windowsOrder_.find(key); o != windowsOrder_.end()) {
                t.order = o->second;
                break;
            }
        // Explorer's own state for this icon (the tray's visible area or its
        // overflow) and the numeric record key SuperMenu's settings overrides use.
        for (const std::wstring& key : {h.key, uidKey, L"exe:" + h.exe})
            if (auto p = windowsPromoted_.find(key); p != windowsPromoted_.end()) {
                t.promoted = p->second;
                break;
            }
        for (const std::wstring& key : {h.key, uidKey, L"exe:" + h.exe})
            if (auto r = windowsRecordKey_.find(key); r != windowsRecordKey_.end()) {
                t.recordKey = r->second;
                break;
            }
        // An app adding its icon again (TaskbarCreated) often leaves its tooltip out, and
        // Explorer keeps the one it had: that one then.
        if (t.tip.empty())
            for (const TrayIcon& e : icons_)
                if (!h.exe.empty() && e.exe == h.exe && !e.tip.empty()) {
                    t.tip = e.tip;
                    break;
                }
        if (t.tip.empty() && !h.exe.empty()) { // none anywhere: the program's name
            auto [it, fresh] = hostNames_.try_emplace(h.exe);
            if (fresh) {
                it->second = VersionString(h.exe, L"FileDescription");
                if (it->second.empty())
                    it->second = PathFindFileNameW(h.exe.c_str());
            }
            t.tip = it->second;
        }
        t.recordId = L"host:" + h.key;
        t.shell = kHostShell;
        t.matchMethod = L"host";
        t.image = h.image;
        t.imageVersion = h.imageVersion;
        if (!t.image) { // no picture sent (or a monochrome one): Explorer's record of the program
            for (const TrayIcon& e : icons_)
                if (!h.exe.empty() && e.exe == h.exe && e.image) {
                    t.image = e.image;
                    t.imageVersion = e.imageVersion;
                    t.inked = e.inked;
                    t.inkedOnLight = e.inkedOnLight;
                    break;
                }
        } else {
            auto ink = hostInk_.find(h.imageVersion);
            if (ink == hostInk_.end()) {
                if (hostInk_.size() > 256)
                    hostInk_.clear(); // animated icons: every frame is a version
                const int tone = GlyphTone(*h.image);
                ink = hostInk_.emplace(h.imageVersion, std::make_pair(tone ? std::make_shared<const IconImage>(InkedGlyph(*h.image, tone)) : nullptr, tone > 0))
                          .first;
            }
            t.inked = ink->second.first;
            t.inkedOnLight = ink->second.second;
        }
        return t;
    };
    // An Explorer button whose program was not recognised but whose name is a hosted
    // icon's is that icon: not shown twice.
    std::set<std::wstring> hostedTitles;
    for (const HostIcon& h : hosted)
        if (!h.tip.empty())
            hostedTitles.insert(Title(h.tip));
    std::vector<TrayIcon> out;
    for (size_t i = 0; i < icons_.size(); ++i) {
        const TrayIcon& e = icons_[i];
        // An Explorer button whose program was not recognised but whose name is a hosted
        // icon's is that icon: not shown twice.
        if (hostOf[i] == SIZE_MAX && e.exe.empty() && !e.tip.empty() && hostedTitles.contains(Title(e.tip)))
            continue;
        if (hostOf[i] == SIZE_MAX) {
            out.push_back(e);
            continue;
        }
        // The live picture and protocol come from the host; Explorer's own state (on
        // the taskbar or in its overflow), the numeric record key, the real identity
        // and the provenance stay the ones the read-back proved.
        TrayIcon t = make(hosted[hostOf[i]]);
        t.promoted = e.promoted;
        t.recordKey = e.recordKey;
        t.recordId = e.recordId;
        if (t.exe.empty())
            t.exe = e.exe;
        if (t.tip.empty())
            t.tip = e.tip;
        if (t.order == SIZE_MAX)
            t.order = e.order;
        out.push_back(std::move(t));
    }
    for (size_t h = 0; h < hosted.size(); ++h)
        if (!used[h])
            out.push_back(make(hosted[h]));
    return inWindowsOrder(shown(std::move(out)));
}

bool TrayModel::Loaded() const
{
    std::lock_guard g(mutex_);
    return loaded_;
}

bool TrayModel::Available() const
{
    std::lock_guard g(mutex_);
    return available_;
}

void TrayModel::Worker()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IUIAutomation> uia;
    HRESULT created = CoCreateInstance(kClsidUia8, nullptr, CLSCTX_INPROC_SERVER, kIidUia, uia.put_void());
    if (FAILED(created))
        created = CoCreateInstance(kClsidUia, nullptr, CLSCTX_INPROC_SERVER, kIidUia, uia.put_void());
    if (FAILED(created))
        Log(L"tray: UI Automation unavailable");
    if (auto options = TryAs<IUIAutomation2>(uia)) {
        options->put_ConnectionTimeout(250);
        options->put_TransactionTimeout(250);
    }
    uint64_t nextImageVersion = 0;
    struct Picture {
        uint64_t version = 0;
        std::wstring exe;
        std::vector<BYTE> snapshot;
        std::shared_ptr<const IconImage> image, inked;
        bool inkedOnLight = false;
    };
    std::map<std::wstring, Picture> pictures;
    using Clock = std::chrono::steady_clock;
    auto retryAt = Clock::time_point::max();
    tray::ShellSession recoverySession;
    bool recoveryStarted = false;
    unsigned retries = 0;
    for (;;) {
        Job job;
        {
            std::unique_lock g(mutex_);
            if (retryAt == Clock::time_point::max())
                cv_.wait(g, [&] { return stop_ || !jobs_.empty(); });
            else
                cv_.wait_until(g, retryAt, [&] { return stop_ || !jobs_.empty(); });
            if (stop_)
                break;
            if (jobs_.empty()) {
                job.refresh = true;
                retryAt = Clock::time_point::max();
            } else {
                job = jobs_.front();
                jobs_.pop_front();
            }
        }
        if (!uia) {
            if (!job.refresh)
                PostMessageW(notify_, msg_, ShowOverflow(nullptr) ? 2 : 3, 0);
            else {
                std::lock_guard g(mutex_);
                loaded_ = true;
                available_ = false;
            }
            continue;
        }
        try {
            bool available = false;
            const ULONGLONG findStart = GetTickCount64();
            if (job.systemTray) {
                const bool shown = ShowOverflow(uia.get());
                PostMessageW(notify_, msg_, shown ? 5 : 3, 0);
                continue;
            }
            auto session = ReadShellSession();
            if (!recoveryStarted || session != recoverySession) {
                recoveryStarted = true;
                recoverySession = session;
                retries = 5;
            }
            if (job.refresh && retries) {
                const unsigned delay = 500u << (5 - retries--);
                retryAt = Clock::now() + std::chrono::milliseconds(delay);
                Log(L"tray: shell recovery refresh scheduled delayMs=%u remaining=%u", delay, retries);
            }
            std::vector<Found> buttons = FindButtons(uia.get(), available);
            const ULONGLONG readMs = GetTickCount64() - findStart;
            if (!job.refresh) {
                bool ordered = false;
                std::vector<Record> records;
                auto match = MatchButtons(buttons, records, ordered);
                auto resolve = [&]() -> Found* {
                    std::vector<tray::TargetKey> candidates;
                    candidates.reserve(buttons.size());
                    for (size_t b = 0; b < buttons.size(); ++b) {
                        candidates.push_back(
                            {session, buttons[b].runtimeId, buttons[b].recordId, match[b] >= 0 ? records[size_t(match[b])].exe : L"", buttons[b].tip});
                    }
                    const auto resolved = tray::ResolveTarget(job.icon.Target(), ReadShellSession(), candidates);
                    Log(L"tray: click resolve=%s shellPid=%lu shellStarted=%llu shellWindow=%llu", tray::ResolutionName(resolved.reason), DWORD(session.pid),
                        session.started, session.window);
                    return resolved.index == tray::kNone ? nullptr : &buttons[resolved.index];
                };
                Found* target = resolve();
                HWND taskbar = ShellTaskbar(), overflow = OverflowWindow();
                APPBARDATA abd{sizeof(abd)};
                BOOL offscreen = TRUE, enabled = FALSE;
                HRESULT state = target && target->element ? target->element->get_CurrentIsOffscreen(&offscreen) : UIA_E_ELEMENTNOTAVAILABLE;
                if (target && target->element)
                    target->element->get_CurrentIsEnabled(&enabled);
                Log(L"tray: click begin action=%s tip='%s' exe='%s' identity=%s source=%s match=%s taskbarVisible=%d autoHide=%d overflowExists=%d "
                    L"overflowVisible=%d offscreen=%d enabled=%d stateHr=0x%08X",
                    job.action == tray::Action::Right              ? L"right" :
                    job.action == tray::Action::Double             ? L"double" :
                    job.action == tray::Action::DoubleContinuation ? L"double-continuation" :
                                                                     L"left",
                    Title(job.icon.tip).c_str(), job.icon.exe.c_str(), job.icon.recordId.c_str(), target && target->element ? L"uia" : L"record",
                    target ? tray::MethodName(target->method) : L"missing", IsWindowVisible(taskbar), (AppBarMessage(ABM_GETSTATE, abd) & ABS_AUTOHIDE) != 0,
                    overflow != nullptr, IsWindowVisible(overflow), offscreen, enabled, unsigned(state));
                const auto before = CaptureResponse(job.icon.exe);
                auto observe = [&](bool accepted) {
                    // A context menu (Steam's, toolkits') can take longer to show than a window.
                    const ULONGLONG until = GetTickCount64() + (!accepted ? 40 : job.action == tray::Action::Right ? 500 : 280);
                    do {
                        if (ResponseChanged(before, CaptureResponse(job.icon.exe), job.action))
                            return true;
                        Sleep(20);
                    } while (GetTickCount64() < until);
                    return false;
                };
                auto invoke = [&](tray::Route route) -> int32_t {
                    HRESULT hr = UIA_E_ELEMENTNOTAVAILABLE;
                    if (target && target->element && session == ReadShellSession()) {
                        // Explorer performs the notification-area callback. Grant only that
                        // process, never ASFW_ANY or a PID inferred from a GUID rectangle.
                        const BOOL allowed = AllowSetForegroundWindow(session.pid);
                        Log(L"tray: click foreground-delegate pid=%lu allowed=%d error=%lu", DWORD(session.pid), allowed, allowed ? 0 : GetLastError());
                        if (route == tray::Route::Toolbar)
                            hr = ToolbarClick(*target, job.action);
                        else if (route == tray::Route::ContextMenu) {
                            auto e3 = TryAs<IUIAutomationElement3>(target->element);
                            hr = e3 ? e3->ShowContextMenu() : E_NOINTERFACE;
                        } else if (route == tray::Route::Invoke) {
                            ComPtr<IUIAutomationInvokePattern> pattern;
                            hr = target->element->GetCurrentPatternAs(UIA_InvokePatternId, kIidInvoke, pattern.put_void());
                            if (SUCCEEDED(hr))
                                hr = pattern ? pattern->Invoke() : E_NOINTERFACE;
                        } else {
                            ComPtr<IUIAutomationLegacyIAccessiblePattern> pattern;
                            hr = target->element->GetCurrentPatternAs(UIA_LegacyIAccessiblePatternId, kIidLegacy, pattern.put_void());
                            if (SUCCEEDED(hr))
                                hr = pattern ? pattern->DoDefaultAction() : E_NOINTERFACE;
                        }
                    }
                    Log(L"tray: click attempt route=%s hr=0x%08X", RouteName(route), unsigned(hr));
                    return int32_t(hr);
                };
                auto result = tray::Dispatch(job.action, target && target->toolbar, invoke, observe);
                // Reacquire only after a definite stale-element error. Generic E_FAIL is not evidence of staleness.
                if (target && target->element && result.outcome == tray::Outcome::Unavailable && !result.attempts.empty() &&
                    result.attempts.front().status == int32_t(UIA_E_ELEMENTNOTAVAILABLE)) {
                    session = ReadShellSession();
                    buttons = FindButtons(uia.get(), available);
                    match = MatchButtons(buttons, records, ordered);
                    target = resolve();
                    Log(L"tray: click reacquire resolved=%d", target != nullptr);
                    result = tray::Dispatch(job.action, target && target->toolbar, invoke, observe);
                }
                // A right click the shell took without a menu showing: one real click on the button (#28).
                if (job.action == tray::Action::Right && result.outcome != tray::Outcome::Observed && target && target->element && !target->toolbar &&
                    session == ReadShellSession()) {
                    const HRESULT hr = PointerRightClick(uia.get(), *target);
                    Log(L"tray: click attempt route=pointer hr=0x%08X", unsigned(hr));
                    if (hr == S_OK)
                        result.outcome = observe(true) ? tray::Outcome::Observed : tray::Outcome::AcceptedUnconfirmed;
                    else if (hr == E_ABORT && result.outcome == tray::Outcome::Unavailable)
                        result.outcome = tray::Outcome::AcceptedUnconfirmed; // the user moved on: no fallback popup
                }
                bool observed = result.outcome == tray::Outcome::Observed;
                for (const auto& attempt : result.attempts)
                    Log(L"tray: click evidence route=%s observed=%d", RouteName(attempt.route), attempt.observed);
                if (result.outcome == tray::Outcome::AcceptedUnconfirmed) {
                    Log(L"tray: click end result=accepted-unconfirmed elapsed=%llu ms", GetTickCount64() - findStart);
                    PostMessageW(notify_, msg_, 4, 0);
                    continue;
                }
                // Only an existing visible/minimized main window, and only for a left action.
                if (!observed && job.action != tray::Action::Right && !job.icon.exe.empty()) {
                    observed = ForegroundExisting(job.icon.exe);
                    Log(L"tray: click foreground-existing observed=%d", observed);
                }
                const bool shown = !observed && ShowOverflow(uia.get());
                Log(L"tray: click end result=%s elapsed=%llu ms",
                    observed ? L"observed" :
                    shown    ? L"system-tray" :
                               L"needs-user",
                    GetTickCount64() - findStart);
                PostMessageW(notify_, msg_, observed ? 1 : shown ? 2 : 3, 0);
                continue;
            }

            bool ordered = false;
            std::vector<Record> records;
            const ULONGLONG readStart = findStart;
            ReadDiagnostics diagnostics;
            std::vector<int> match = MatchButtons(buttons, records, ordered, &diagnostics);
            if (session != ReadShellSession()) {
                Log(L"tray: discarded refresh spanning shell replacement");
                Refresh();
                continue;
            }
            if (!session.Valid()) {
                buttons.clear();
                match.clear();
                available = false;
            }

            std::vector<TrayIcon> icons;
            std::set<std::wstring> livePictures;
            int recognised = 0;
            for (size_t b = 0; b < buttons.size(); ++b) {
                TrayIcon t;
                t.tip = buttons[b].tip;
                t.promoted = buttons[b].promoted;
                t.index = buttons[b].index;
                t.recordId = buttons[b].recordId;
                t.runtimeId = buttons[b].runtimeId;
                t.shell = session;
                t.matchMethod = tray::MethodName(buttons[b].method);
                t.fromRecord = !buttons[b].element;
                if (match[b] >= 0) {
                    const Record& r = records[size_t(match[b])];
                    t.exe = r.exe;
                    t.order = r.order;
                    t.recordKey = r.id;
                    ++recognised;
                    const std::wstring& key = r.id;
                    livePictures.insert(key);
                    auto& pic = pictures[key];
                    if (!pic.image || pic.exe != r.exe || pic.snapshot != r.png) {
                        IconImage img;
                        const auto art = tray::LoadArtwork([&] { return !r.png.empty() && DecodePng(r.png, img); },
                                                           [&] { return LoadProgramIcon(r.exe, kIconPx, img); });
                        if (art == tray::Artwork::Missing)
                            img = {};
                        pic.version = ++nextImageVersion;
                        Log(L"tray: artwork identity=%s source=%s version=%llu bytes=%zu", t.recordId.c_str(),
                            art == tray::Artwork::Snapshot    ? L"snapshot" :
                            art == tray::Artwork::Application ? L"application" :
                                                                L"missing",
                            pic.version, r.png.size());
                        pic.exe = r.exe;
                        pic.snapshot = r.png;
                        const int tone = img.Empty() ? 0 : GlyphTone(img);
                        pic.inked = tone ? std::make_shared<const IconImage>(InkedGlyph(img, tone)) : nullptr;
                        if (tone)
                            Log(L"tray: %s draws a %s glyph: it follows the ink", PathFindFileNameW(r.exe.c_str()), tone > 0 ? L"white" : L"black");
                        pic.inkedOnLight = tone > 0;
                        pic.image = img.Empty() ? nullptr : std::make_shared<const IconImage>(std::move(img));
                    }
                    t.imageVersion = pic.version;
                    t.image = pic.image;
                    t.inked = pic.inked;
                    t.inkedOnLight = pic.inkedOnLight;
                }
                icons.push_back(std::move(t));
            }
            std::erase_if(pictures, [&](const auto& item) { return !livePictures.contains(item.first); });
            // Windows' own order, promotion and numeric record key of the icons, for a list
            // that does not depend on how much of Explorer's tray has been built (and for
            // the hosted icons, which only know their identity).
            std::map<std::wstring, size_t> windowsOrder;
            std::map<std::wstring, bool> windowsPromoted;
            std::map<std::wstring, std::wstring> windowsRecordKey;
            for (const Record& r : records) {
                if (r.exe.empty() || r.id.rfind(L"run:", 0) == 0)
                    continue;
                std::vector<std::wstring> keys;
                if (r.guid != GUID_NULL) {
                    wchar_t guid[40]{};
                    StringFromGUID2(r.guid, guid, 40);
                    keys.push_back(L"guid:" + std::wstring(guid));
                }
                if (r.hasUid)
                    keys.push_back(L"uid:" + r.exe + L"|" + std::to_wstring(r.uid));
                keys.push_back(L"exe:" + r.exe);
                for (const std::wstring& key : keys) {
                    if (r.order != SIZE_MAX) {
                        auto [it, fresh] = windowsOrder.try_emplace(key, r.order);
                        if (!fresh && key == L"exe:" + r.exe)
                            it->second = std::min(it->second, r.order);
                    }
                    windowsPromoted.try_emplace(key, r.promoted);
                    windowsRecordKey.try_emplace(key, r.id);
                }
            }
            bool changed;
            {
                std::lock_guard g(mutex_);
                windowsOrder_ = std::move(windowsOrder);
                windowsPromoted_ = std::move(windowsPromoted);
                windowsRecordKey_ = std::move(windowsRecordKey);
                changed = !loaded_ || available != available_ || icons.size() != icons_.size() ||
                          !std::equal(icons.begin(), icons.end(), icons_.begin(), [](const TrayIcon& a, const TrayIcon& b) {
                              return a.tip == b.tip && a.exe == b.exe && a.image == b.image && a.promoted == b.promoted && a.recordId == b.recordId &&
                                     a.runtimeId == b.runtimeId && a.shell == b.shell && a.matchMethod == b.matchMethod && a.fromRecord == b.fromRecord;
                          });
                icons_ = std::move(icons);
                loaded_ = true;
                available_ = available;
            }
            if (changed) {
                Log(L"tray: reconcile records=%zu live=%zu aliases=%zu sharedPlace=%zu unknown=%zu uiaMs=%llu", diagnostics.records, diagnostics.live,
                    diagnostics.aliases, diagnostics.shared, diagnostics.unknown, readMs);
                Log(L"tray: %zu icons (%d recognised %s%s) in %llu ms%s", buttons.size(), recognised, ordered ? L"in Windows' order" : L"by name",
                    std::ranges::any_of(buttons, [](const Found& f) { return f.toolbar != nullptr; }) ? L", classic tray" : L"", GetTickCount64() - readStart,
                    available ? L"" : L", the shell's tray was not found");
                for (size_t b = 0; b < buttons.size(); ++b) {
                    std::wstring exe = match[b] >= 0 ? records[size_t(match[b])].exe : L"-";
                    Log(L"tray: item=%zu source=%s match=%s promoted=%d index=%d identity=%s tip='%s' exe='%s'", b, buttons[b].element ? L"uia" : L"record",
                        tray::MethodName(buttons[b].method), buttons[b].promoted, buttons[b].index, buttons[b].recordId.c_str(), Title(buttons[b].tip).c_str(),
                        exe.c_str());
                }
                PostMessageW(notify_, msg_, 0, 0);
            } else if (GetTickCount64() - readStart > 500)
                Log(L"tray: slow unchanged refresh totalMs=%llu uiaMs=%llu", GetTickCount64() - readStart, readMs);
        } catch (...) {
            Log(L"tray: worker exception job=%s hr=0x%08X", job.refresh ? L"refresh" : L"click", unsigned(E_UNEXPECTED));
            if (!job.refresh)
                PostMessageW(notify_, msg_, 3, 0);
        }
    }
    uia = nullptr;
    CoUninitialize();
}

} // namespace atrium
