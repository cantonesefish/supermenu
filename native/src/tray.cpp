#include "tray.h"

#include "atrium/shell/shell_icon.h"
#include "atrium/shell/shell_taskbar.h"
#include "atrium/shell/tray_host.h"
#include "settings.h"

#include <combaseapi.h>
#include <objbase.h>
#include <tlhelp32.h>
#include <winver.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace sm {
namespace {

constexpr wchar_t kNotifyIconSettingsKey[] = L"Control Panel\\NotifyIconSettings";
constexpr UINT kIconSize = 32;          // output edge in pixels
constexpr UINT kMaxSourceSide = 1024;   // sanity bound for a decoded source image

// ---------------------------------------------------------------------------
// Registry access
// ---------------------------------------------------------------------------

// Reads one value's raw bytes. Missing/unreadable values are reported as false
// and never throw or log: stale or partially written tray records are normal.
bool QueryRawValue(HKEY key, const wchar_t* name, DWORD& type, std::vector<uint8_t>& value) {
    value.clear();
    DWORD queryType = 0;
    DWORD bytes = 0;
    LONG result = RegQueryValueExW(key, name, nullptr, &queryType, nullptr, &bytes);
    if (result != ERROR_SUCCESS) return false;
    type = queryType;
    if (bytes == 0) return true;
    for (int attempt = 0; attempt < 3; ++attempt) {
        std::vector<uint8_t> buffer(bytes);
        DWORD readType = queryType;
        DWORD readBytes = bytes;
        result = RegQueryValueExW(key, name, nullptr, &readType, buffer.data(), &readBytes);
        if (result == ERROR_SUCCESS) {
            buffer.resize(readBytes);
            value.swap(buffer);
            type = readType;
            return true;
        }
        if (result != ERROR_MORE_DATA || readBytes <= bytes) return false;
        bytes = readBytes;  // the value grew between the two calls; retry larger
    }
    return false;
}

bool QueryString(HKEY key, const wchar_t* name, std::wstring& out) {
    DWORD type = 0;
    std::vector<uint8_t> raw;
    if (!QueryRawValue(key, name, type, raw)) return false;
    if (type != REG_SZ && type != REG_EXPAND_SZ) return false;
    const size_t characterCount = raw.size() / sizeof(wchar_t);
    if (characterCount == 0) {
        out.clear();
        return true;
    }
    const wchar_t* text = reinterpret_cast<const wchar_t*>(raw.data());
    size_t length = 0;
    while (length < characterCount && text[length] != L'\0') ++length;
    out.assign(text, length);
    return true;
}

bool QueryDword(HKEY key, const wchar_t* name, DWORD& out) {
    DWORD type = 0;
    std::vector<uint8_t> raw;
    if (!QueryRawValue(key, name, type, raw)) return false;
    if (type != REG_DWORD || raw.size() < sizeof(DWORD)) return false;
    DWORD value = 0;
    std::memcpy(&value, raw.data(), sizeof(value));
    out = value;
    return true;
}

struct RegistryRecord {
    std::wstring id;
    std::wstring executablePath;
    std::wstring tooltip;
    std::vector<uint8_t> snapshot;
    bool hasSnapshot = false;
    bool promoted = false;
    bool hasUid = false;
    unsigned uid = 0;
    GUID guid{};
    unsigned long long order = ~0ull;
};

// UIOrderList is a plain sequence of little-endian uint64 record ids; the index
// of an id is its position in Explorer's display order.
bool ReadOrderList(HKEY root, std::unordered_map<unsigned long long, unsigned long long>& positions) {
    DWORD type = 0;
    std::vector<uint8_t> raw;
    if (!QueryRawValue(root, L"UIOrderList", type, raw)) return false;
    if (type != REG_BINARY) return false;
    const size_t count = raw.size() / 8;
    for (size_t index = 0; index < count; ++index) {
        unsigned long long id = 0;
        for (size_t byte = 0; byte < 8; ++byte) {
            id |= static_cast<unsigned long long>(raw[index * 8 + byte]) << (byte * 8);
        }
        positions.emplace(id, static_cast<unsigned long long>(index));
    }
    return true;
}

std::vector<RegistryRecord> ReadRegistryRecords() {
    std::vector<RegistryRecord> records;
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kNotifyIconSettingsKey, 0, KEY_READ, &root) != ERROR_SUCCESS) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            sm::Log(L"TrayReader: cannot open HKCU\\%ls (no tray icons available)", kNotifyIconSettingsKey);
        }
        return records;
    }

    std::unordered_map<unsigned long long, unsigned long long> orderPositions;
    ReadOrderList(root, orderPositions);

    DWORD index = 0;
    wchar_t name[256];
    for (;;) {
        DWORD nameLength = static_cast<DWORD>(std::size(name));
        const LONG result = RegEnumKeyExW(root, index, name, &nameLength, nullptr, nullptr, nullptr, nullptr);
        if (result == ERROR_NO_MORE_ITEMS) break;
        ++index;
        if (result != ERROR_SUCCESS || nameLength == 0) continue;

        RegistryRecord record;
        record.id.assign(name, nameLength);
        wchar_t* end = nullptr;
        const unsigned long long numericId = std::wcstoull(record.id.c_str(), &end, 10);
        if (end != record.id.c_str() && end != nullptr && *end == L'\0') {
            const auto position = orderPositions.find(numericId);
            if (position != orderPositions.end()) record.order = position->second;
        }

        HKEY subkey = nullptr;
        if (RegOpenKeyExW(root, record.id.c_str(), 0, KEY_READ, &subkey) == ERROR_SUCCESS) {
            QueryString(subkey, L"ExecutablePath", record.executablePath);
            QueryString(subkey, L"InitialTooltip", record.tooltip);
            DWORD promoted = 0;
            if (QueryDword(subkey, L"IsPromoted", promoted)) record.promoted = promoted != 0;
            DWORD uid = 0;
            if (QueryDword(subkey, L"UID", uid)) {
                record.hasUid = true;
                record.uid = uid;
            }
            std::wstring guidText;
            if (QueryString(subkey, L"IconGuid", guidText)) {
                while (!guidText.empty() && std::iswspace(guidText.front())) guidText.erase(guidText.begin());
                while (!guidText.empty() && std::iswspace(guidText.back())) guidText.pop_back();
                if (!guidText.empty() && CLSIDFromString(guidText.c_str(), &record.guid) != NOERROR) record.guid = GUID{};
            }
            DWORD snapshotType = 0;
            if (QueryRawValue(subkey, L"IconSnapshot", snapshotType, record.snapshot) &&
                snapshotType == REG_BINARY && !record.snapshot.empty()) {
                record.hasSnapshot = true;
            }
            RegCloseKey(subkey);
        }
        records.push_back(std::move(record));
    }
    RegCloseKey(root);
    return records;
}

// ---------------------------------------------------------------------------
// Text helpers
// ---------------------------------------------------------------------------

bool IsSpaceCharacter(wchar_t character) {
    if (character == 0x00A0 || character == 0x3000) return true;  // NBSP, ideographic space
    return std::iswspace(static_cast<wint_t>(character)) != 0;
}

std::wstring TrimWhitespace(const std::wstring& text) {
    size_t begin = 0;
    while (begin < text.size() && IsSpaceCharacter(text[begin])) ++begin;
    size_t end = text.size();
    while (end > begin && IsSpaceCharacter(text[end - 1])) --end;
    return text.substr(begin, end - begin);
}

bool IsDirectionControl(wchar_t character) {
    return (character >= 0x202A && character <= 0x202E) ||  // LRE/RLE/PDF/LRO/RLO
           character == 0x200E || character == 0x200F ||    // LRM/RLM
           (character >= 0x2066 && character <= 0x2069);    // LRI/RLI/FSI/PDI
}

// Tray tooltips can be multi-line and can embed Unicode bidi controls (for
// example Office Click-to-Run's "installation in progress" text). Keep the
// first line and drop the invisible controls so the label can be drawn as-is.
std::wstring CleanLabel(const std::wstring& raw) {
    std::wstring cleaned;
    cleaned.reserve(raw.size());
    for (const wchar_t character : raw) {
        if (IsDirectionControl(character)) continue;
        if (character == L'\r' || character == L'\n') break;
        cleaned.push_back(character == L'\t' ? L' ' : character);
    }
    return TrimWhitespace(cleaned);
}

// ---------------------------------------------------------------------------
// Path resolution
// ---------------------------------------------------------------------------

bool IsAbsolutePath(const std::wstring& path) {
    if (path.size() >= 3 && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/')) return true;
    return path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\';  // UNC
}

std::wstring LastPathComponent(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring FileStem(const std::wstring& path) {
    std::wstring name = LastPathComponent(path);
    const size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0) name.resize(dot);
    return name;
}

// Some ExecutablePath values begin with a known-folder GUID in braces instead
// of the volume path, e.g. "{1AC14E77-...}\Taskmgr.exe". Resolve that prefix
// through the shell; the GUID is a FOLDERID, not a CLSID.
std::wstring ResolveKnownFolderPrefix(const std::wstring& raw,
                                      std::unordered_map<std::wstring, std::wstring>& folderCache) {
    if (raw.size() < 2 || raw[0] != L'{') return raw;
    const size_t closingBrace = raw.find(L'}');
    if (closingBrace == std::wstring::npos || closingBrace > 40) return raw;
    const std::wstring guidText = raw.substr(0, closingBrace + 1);
    auto cached = folderCache.find(guidText);
    if (cached == folderCache.end()) {
        std::wstring folder;
        GUID guid{};
        if (SUCCEEDED(IIDFromString(guidText.c_str(), &guid))) {
            PWSTR knownPath = nullptr;
            if (SUCCEEDED(SHGetKnownFolderPath(guid, KF_FLAG_DEFAULT, nullptr, &knownPath)) && knownPath) {
                folder.assign(knownPath);
            }
            if (knownPath) CoTaskMemFree(knownPath);
        }
        cached = folderCache.emplace(guidText, std::move(folder)).first;
    }
    if (cached->second.empty()) return raw;
    std::wstring remainder = raw.substr(closingBrace + 1);
    if (!remainder.empty() && remainder[0] != L'\\' && remainder[0] != L'/') {
        remainder.insert(remainder.begin(), L'\\');
    }
    return cached->second + remainder;
}

// Returns the absolute lower-case executable path, or an empty string when the
// value is missing or cannot be resolved.
std::wstring ResolveExecutablePath(const std::wstring& raw,
                                   std::unordered_map<std::wstring, std::wstring>& folderCache) {
    std::wstring path = ResolveKnownFolderPrefix(raw, folderCache);
    if (path.find(L'%') != std::wstring::npos) {
        const DWORD required = ExpandEnvironmentStringsW(path.c_str(), nullptr, 0);
        if (required > 1) {
            std::wstring expanded(required, L'\0');
            const DWORD written = ExpandEnvironmentStringsW(path.c_str(), expanded.data(), required);
            if (written > 0 && written <= required) {
                expanded.resize(written - 1);
                path = std::move(expanded);
            }
        }
    }
    path = TrimWhitespace(path);
    if (!IsAbsolutePath(path)) return std::wstring();
    return sm::Lower(path);
}

// FileDescription from the executable's version resource; empty when absent.
std::wstring QueryFileDescription(const std::wstring& exe) {
    if (exe.empty()) return {};
    DWORD ignoredHandle = 0;
    const DWORD blockSize = GetFileVersionInfoSizeW(exe.c_str(), &ignoredHandle);
    if (blockSize == 0) return {};
    std::vector<uint8_t> block(blockSize);
    if (!GetFileVersionInfoW(exe.c_str(), 0, blockSize, block.data())) return {};

    std::vector<std::pair<WORD, WORD>> translations;
    void* translationData = nullptr;
    UINT translationBytes = 0;
    if (VerQueryValueW(block.data(), L"\\VarFileInfo\\Translation", &translationData, &translationBytes) &&
        translationData != nullptr && translationBytes >= 4) {
        const WORD* words = static_cast<const WORD*>(translationData);
        const size_t count = translationBytes / 4;
        translations.reserve(count);
        for (size_t i = 0; i < count; ++i) translations.emplace_back(words[i * 2], words[i * 2 + 1]);
    }
    if (translations.empty()) translations.emplace_back(0x0409, 0x04B0);  // en-US, Unicode

    for (const auto& [language, codePage] : translations) {
        wchar_t query[64];
        std::swprintf(query, std::size(query), L"\\StringFileInfo\\%04x%04x\\FileDescription", language, codePage);
        void* description = nullptr;
        UINT descriptionBytes = 0;
        if (!VerQueryValueW(block.data(), query, &description, &descriptionBytes)) continue;
        if (description == nullptr) continue;
        const wchar_t* text = static_cast<const wchar_t*>(description);
        const size_t limit = descriptionBytes > 0 ? descriptionBytes : 512;
        size_t length = 0;
        while (length < limit && text[length] != L'\0') ++length;
        if (length == 0) continue;
        return std::wstring(text, length);
    }
    return {};
}

bool IsOwnExecutableName(const std::wstring& path) {
    const std::wstring name = sm::Lower(LastPathComponent(path));
    return name == L"supermenubar.exe" || name == L"supermenu.settings.exe";
}

// SuperMenu registers its own notification icon; it must never appear in the
// menu bar's tray area. Test both the raw and the resolved path so an
// unresolved known-folder redirect cannot leak the entry back in.
bool IsOwnExecutable(const RegistryRecord& record, const std::wstring& resolvedExe) {
    return IsOwnExecutableName(record.executablePath) || IsOwnExecutableName(resolvedExe);
}

std::wstring BuildLabel(const RegistryRecord& record, const std::wstring& resolvedExe,
                        std::unordered_map<std::wstring, std::wstring>& descriptionCache) {
    std::wstring label = CleanLabel(record.tooltip);
    // Some Windows components store an untranslated resource id in the tooltip
    // ("IDS_MAIN_PAGE_TITLE"). Treat that as empty and fall back to the app name.
    if (label.size() >= 4 && (label.rfind(L"IDS_", 0) == 0 || label.rfind(L"ids_", 0) == 0)) label.clear();
    if (!label.empty()) return label;
    if (!resolvedExe.empty()) {
        auto cached = descriptionCache.find(resolvedExe);
        if (cached == descriptionCache.end()) {
            cached = descriptionCache.emplace(resolvedExe, QueryFileDescription(resolvedExe)).first;
        }
        label = CleanLabel(cached->second);
        if (!label.empty()) return label;
    }
    return CleanLabel(FileStem(!resolvedExe.empty() ? resolvedExe : record.executablePath));
}

// ---------------------------------------------------------------------------
// Icon decoding (WIC)
// ---------------------------------------------------------------------------

// Converts any WIC source to a 32x32 premultiplied-BGRA canvas. The source is
// scaled with aspect preserved until its longer edge matches the canvas; a
// non-square source is centred on a transparent background.
bool ComposeSquareIcon(IWICImagingFactory* factory, IWICBitmapSource* source, UINT sourceWidth, UINT sourceHeight,
                       std::vector<uint8_t>& pixels) {
    pixels.clear();
    if (!factory || !source || sourceWidth == 0 || sourceHeight == 0 || sourceWidth > kMaxSourceSide ||
        sourceHeight > kMaxSourceSide) {
        return false;
    }

    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(converter.put()))) return false;
    if (FAILED(converter->Initialize(source, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom))) {
        return false;
    }

    UINT scaledWidth = kIconSize;
    UINT scaledHeight = kIconSize;
    if (sourceWidth >= sourceHeight) {
        const double height = static_cast<double>(sourceHeight) * kIconSize / static_cast<double>(sourceWidth);
        scaledHeight = static_cast<UINT>(std::max<long>(1, std::lround(height)));
    } else {
        const double width = static_cast<double>(sourceWidth) * kIconSize / static_cast<double>(sourceHeight);
        scaledWidth = static_cast<UINT>(std::max<long>(1, std::lround(width)));
    }

    ComPtr<IWICBitmapScaler> scaler;
    if (FAILED(factory->CreateBitmapScaler(scaler.put()))) return false;
    if (FAILED(scaler->Initialize(converter.get(), scaledWidth, scaledHeight, WICBitmapInterpolationModeFant))) {
        return false;
    }

    const UINT scaledStride = scaledWidth * 4;
    std::vector<uint8_t> scaled(static_cast<size_t>(scaledStride) * scaledHeight);
    if (FAILED(scaler->CopyPixels(nullptr, scaledStride, static_cast<UINT>(scaled.size()), scaled.data()))) {
        return false;
    }

    pixels.assign(static_cast<size_t>(kIconSize) * kIconSize * 4, 0);
    const UINT offsetX = (kIconSize - scaledWidth) / 2;
    const UINT offsetY = (kIconSize - scaledHeight) / 2;
    for (UINT row = 0; row < scaledHeight; ++row) {
        std::memcpy(pixels.data() + (static_cast<size_t>(offsetY + row) * kIconSize + offsetX) * 4,
                    scaled.data() + static_cast<size_t>(row) * scaledStride, scaledStride);
    }
    return true;
}

bool DecodeSnapshotPng(IWICImagingFactory* factory, const std::vector<uint8_t>& png, std::vector<uint8_t>& pixels) {
    static constexpr uint8_t kPngSignature[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    if (!factory || png.size() < sizeof(kPngSignature) ||
        std::memcmp(png.data(), kPngSignature, sizeof(kPngSignature)) != 0) {
        return false;
    }
    if (png.size() > 0xFFFFFFFFull) return false;

    ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(stream.put()))) return false;
    if (FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(png.data()), static_cast<DWORD>(png.size())))) {
        return false;
    }
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromStream(stream.get(), nullptr, WICDecodeMetadataCacheOnLoad, decoder.put()))) {
        return false;
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, frame.put()))) return false;
    UINT sourceWidth = 0;
    UINT sourceHeight = 0;
    if (FAILED(frame->GetSize(&sourceWidth, &sourceHeight))) return false;
    return ComposeSquareIcon(factory, frame.get(), sourceWidth, sourceHeight, pixels);
}

// Fallback for records without a usable IconSnapshot: pull the executable's
// small shell icon and run it through the same 32x32 composition path.
bool DecodeExecutableIcon(IWICImagingFactory* factory, const std::wstring& exe, std::vector<uint8_t>& pixels) {
    if (!factory || exe.empty()) return false;
    SHFILEINFOW fileInfo{};
    UINT flags = SHGFI_ICON | SHGFI_SMALLICON;
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) flags |= SHGFI_USEFILEATTRIBUTES;
    if (SHGetFileInfoW(exe.c_str(), FILE_ATTRIBUTE_NORMAL, &fileInfo, sizeof(fileInfo), flags) == 0 ||
        fileInfo.hIcon == nullptr) {
        return false;
    }
    ComPtr<IWICBitmap> bitmap;
    const HRESULT result = factory->CreateBitmapFromHICON(fileInfo.hIcon, bitmap.put());
    DestroyIcon(fileInfo.hIcon);
    if (FAILED(result)) return false;
    UINT sourceWidth = 0;
    UINT sourceHeight = 0;
    if (FAILED(bitmap->GetSize(&sourceWidth, &sourceHeight))) return false;
    return ComposeSquareIcon(factory, bitmap.get(), sourceWidth, sourceHeight, pixels);
}

unsigned long long HashPixels(const std::vector<uint8_t>& pixels, int width, int height) {
    unsigned long long hash = 14695981039346656037ull;  // FNV-1a offset basis
    const auto mix = [&hash](uint8_t byte) {
        hash ^= static_cast<unsigned long long>(byte);
        hash *= 1099511628211ull;  // FNV prime
    };
    const unsigned widthBits = static_cast<unsigned>(width);
    const unsigned heightBits = static_cast<unsigned>(height);
    for (int byte = 0; byte < 4; ++byte) mix(static_cast<uint8_t>((widthBits >> (byte * 8)) & 0xFFu));
    for (int byte = 0; byte < 4; ++byte) mix(static_cast<uint8_t>((heightBits >> (byte * 8)) & 0xFFu));
    for (const uint8_t byte : pixels) mix(byte);
    return hash;
}

unsigned long long HashBytes(const std::vector<uint8_t>& bytes) {
    unsigned long long hash = 14695981039346656037ull;
    for (const uint8_t byte : bytes) {
        hash ^= static_cast<unsigned long long>(byte);
        hash *= 1099511628211ull;
    }
    return hash;
}

// ---------------------------------------------------------------------------
// Ordering / duplicate detection
// ---------------------------------------------------------------------------

bool TaskbarOwnsPoint(const RECT& rect) {
    HWND tray = atrium::ShellTaskbar();
    if (!tray) return false;
    POINT point{(rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2};
    HWND window = WindowFromPoint(point);
    for (int depth = 0; window && depth < 12; ++depth) {
        if (window == tray) return true;
        window = GetParent(window);
    }
    return false;
}

// Explorer's persisted IconSnapshot can be older than the icon an application
// currently registered (Tailscale is a common example). For a live tray icon
// with a real screen rect, sample the taskbar pixels and key the local taskbar
// background out so the bar shows the actual current glyph/state overlay.
bool CaptureScreenIcon(const RECT& rect, std::vector<uint8_t>& pixels) {
    const int width = static_cast<int>(rect.right - rect.left);
    const int height = static_cast<int>(rect.bottom - rect.top);
    if (width < 8 || height < 8 || width > 96 || height > 96) return false;

    HDC screen = GetDC(nullptr);
    if (!screen) return false;
    HDC memory = CreateCompatibleDC(screen);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;  // top-down
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap || !bits) {
        if (bitmap) DeleteObject(bitmap);
        DeleteDC(memory);
        ReleaseDC(nullptr, screen);
        return false;
    }
    HGDIOBJ previous = SelectObject(memory, bitmap);
    const BOOL copied = BitBlt(memory, 0, 0, width, height, screen, rect.left, rect.top, SRCCOPY);
    SelectObject(memory, previous);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    struct BitmapGuard {
        HBITMAP bitmap;
        ~BitmapGuard() { DeleteObject(bitmap); }
    } guard{bitmap};
    if (!copied) return false;

    const auto* source = static_cast<const uint32_t*>(bits);
    int backgroundB = 0, backgroundG = 0, backgroundR = 0, samples = 0;
    auto addSample = [&](int x, int y) {
        const uint32_t p = source[y * width + x];
        backgroundB += p & 0xFF;
        backgroundG += (p >> 8) & 0xFF;
        backgroundR += (p >> 16) & 0xFF;
        ++samples;
    };
    const int corner = std::min(4, std::min(width, height) / 4);
    for (int y = 0; y < corner; ++y) {
        for (int x = 0; x < corner; ++x) {
            addSample(x, y);
            addSample(width - 1 - x, y);
            addSample(x, height - 1 - y);
            addSample(width - 1 - x, height - 1 - y);
        }
    }
    if (samples == 0) return false;
    backgroundB /= samples;
    backgroundG /= samples;
    backgroundR /= samples;

    std::vector<uint8_t> keyed(static_cast<size_t>(width) * height * 4, 0);
    int minX = width, minY = height, maxX = -1, maxY = -1;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const uint32_t p = source[y * width + x];
            const int b = p & 0xFF, g = (p >> 8) & 0xFF, r = (p >> 16) & 0xFF;
            const int distance = std::max({std::abs(b - backgroundB), std::abs(g - backgroundG),
                                           std::abs(r - backgroundR)});
            // The taskbar background is translucent over the wallpaper and is
            // not perfectly flat; require a clear contrast against it so noise
            // does not survive as a large opaque square.
            const int alpha = std::clamp((distance - 28) * 255 / 64, 0, 255);
            // Drop the faint taskbar/noise fringe as well as transparent
            // background; otherwise a single stray pixel stretches the tight
            // icon crop and makes the tray glyph tiny in the bar.
            if (alpha < 48) continue;
            uint8_t* dst = keyed.data() + (static_cast<size_t>(y) * width + x) * 4;
            dst[0] = static_cast<uint8_t>(b * alpha / 255);
            dst[1] = static_cast<uint8_t>(g * alpha / 255);
            dst[2] = static_cast<uint8_t>(r * alpha / 255);
            dst[3] = static_cast<uint8_t>(alpha);
            minX = std::min(minX, x);
            minY = std::min(minY, y);
            maxX = std::max(maxX, x);
            maxY = std::max(maxY, y);
        }
    }
    if (maxX < minX || maxY < minY) return false;
    const int sourceW = maxX - minX + 1;
    const int sourceH = maxY - minY + 1;
    if (sourceW < 5 || sourceH < 5) return false;

    // Fit the tight icon into a transparent 32x32 frame without distortion.
    const float scale = std::min(static_cast<float>(kIconSize) / sourceW,
                                 static_cast<float>(kIconSize) / sourceH);
    const int targetW = std::max(1, static_cast<int>(std::lround(sourceW * scale)));
    const int targetH = std::max(1, static_cast<int>(std::lround(sourceH * scale)));
    const int offsetX = (static_cast<int>(kIconSize) - targetW) / 2;
    const int offsetY = (static_cast<int>(kIconSize) - targetH) / 2;
    pixels.assign(static_cast<size_t>(kIconSize) * kIconSize * 4, 0);
    for (int y = 0; y < targetH; ++y) {
        for (int x = 0; x < targetW; ++x) {
            const float sx = std::clamp((x + 0.5f) / scale - 0.5f, 0.0f, float(sourceW - 1));
            const float sy = std::clamp((y + 0.5f) / scale - 0.5f, 0.0f, float(sourceH - 1));
            const int x0 = static_cast<int>(sx), y0 = static_cast<int>(sy);
            const int x1 = std::min(x0 + 1, sourceW - 1), y1 = std::min(y0 + 1, sourceH - 1);
            const float fx = sx - x0, fy = sy - y0;
            float channel[4]{};
            const int xs[2] = {x0, x1}, ys[2] = {y0, y1};
            const float wx[2] = {1 - fx, fx}, wy[2] = {1 - fy, fy};
            for (int iy = 0; iy < 2; ++iy) {
                for (int ix = 0; ix < 2; ++ix) {
                    const int srcX = minX + xs[ix], srcY = minY + ys[iy];
                    const uint8_t* s = keyed.data() + (static_cast<size_t>(srcY) * width + srcX) * 4;
                    const float weight = wx[ix] * wy[iy];
                    for (int c = 0; c < 4; ++c) channel[c] += s[c] * weight;
                }
            }
            uint8_t* d = pixels.data() +
                         (static_cast<size_t>(offsetY + y) * kIconSize + offsetX + x) * 4;
            for (int c = 0; c < 4; ++c) d[c] = static_cast<uint8_t>(std::clamp(channel[c], 0.0f, 255.0f));
        }
    }
    return true;
}

bool SortKeyLess(const TrayIcon& left, const TrayIcon& right) {
    if (left.order != right.order) return left.order < right.order;
    const int labelComparison = _wcsicmp(left.label.c_str(), right.label.c_str());
    if (labelComparison != 0) return labelComparison < 0;
    return left.id < right.id;
}

bool SameIcon(const TrayIcon& left, const TrayIcon& right) {
    return left.id == right.id && left.hostKey == right.hostKey && left.hosted == right.hosted &&
           left.label == right.label && left.exe == right.exe &&
           left.promoted == right.promoted && left.order == right.order && left.imageId == right.imageId &&
           left.width == right.width && left.height == right.height && left.pixels == right.pixels;
}

}  // namespace

// ---------------------------------------------------------------------------
// TrayReader
// ---------------------------------------------------------------------------

namespace {

bool ShellTrayResponsive() {
    HWND shell = atrium::ShellTaskbar();
    if (!shell) return false;
    DWORD_PTR result = 0;
    return SendMessageTimeoutW(shell, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 300, &result) != 0;
}

struct WindowIndex {
    std::unordered_map<std::wstring, std::vector<HWND>> byExe;
};

struct WindowEnumContext {
    const std::unordered_map<DWORD, std::wstring>* paths = nullptr;
    const std::unordered_set<std::wstring>* needed = nullptr;
    WindowIndex* index = nullptr;
};

BOOL CALLBACK CollectWindowProc(HWND hwnd, LPARAM param) {
    auto* ctx = reinterpret_cast<WindowEnumContext*>(param);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    const auto it = ctx->paths->find(pid);
    if (it == ctx->paths->end() || ctx->needed->count(it->second) == 0) return TRUE;
    ctx->index->byExe[it->second].push_back(hwnd);
    return TRUE;
}

WindowIndex CollectTrayWindowIndex(const std::unordered_set<std::wstring>& needed) {
    WindowIndex index;
    std::unordered_map<DWORD, std::wstring> paths;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W entry{sizeof(entry)};
        for (BOOL ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry)) {
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
            if (!process) continue;
            wchar_t path[MAX_PATH * 2]{};
            DWORD size = static_cast<DWORD>(std::size(path));
            if (QueryFullProcessImageNameW(process, 0, path, &size)) {
                paths[entry.th32ProcessID] = sm::Lower(path);
            }
            CloseHandle(process);
        }
        CloseHandle(snapshot);
    }
    WindowEnumContext ctx{&paths, &needed, &index};
    EnumWindows(CollectWindowProc, reinterpret_cast<LPARAM>(&ctx));
    // Tray icons usually live on message-only windows, which EnumWindows skips.
    for (HWND hwnd = FindWindowExW(HWND_MESSAGE, nullptr, nullptr, nullptr); hwnd;
         hwnd = FindWindowExW(HWND_MESSAGE, hwnd, nullptr, nullptr)) {
        CollectWindowProc(hwnd, reinterpret_cast<LPARAM>(&ctx));
    }
    return index;
}

bool UsableTrayRect(const RECT& rect) {
    if (rect.right - rect.left < 2 || rect.bottom - rect.top < 2) return false;
    HMONITOR monitor = MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!monitor || !GetMonitorInfoW(monitor, &info)) return false;
    return rect.left >= info.rcMonitor.left && rect.top >= info.rcMonitor.top &&
           rect.right <= info.rcMonitor.right && rect.bottom <= info.rcMonitor.bottom;
}

bool ProbeTrayIconLive(TrayIcon& icon, const WindowIndex& index) {
    if (icon.guid != GUID{}) {
        NOTIFYICONIDENTIFIER id{};
        id.cbSize = sizeof(id);
        id.guidItem = icon.guid;
        RECT rect{};
        // GetRect's coordinates can be meaningless while the taskbar is
        // hidden (Atrium's host may answer with a generic rectangle). SUCCEEDED
        // is the liveness evidence; only an on-screen rect is used for clicks.
        if (FAILED(Shell_NotifyIconGetRect(&id, &rect))) return false;
        if (UsableTrayRect(rect)) {
            icon.screenRect = rect;
            icon.hasScreenRect = true;
        }
        return true;
    }
    if (!icon.hasUid || icon.exe.empty()) return false;
    const auto windows = index.byExe.find(icon.exe);
    if (windows == index.byExe.end()) return false;
    for (HWND hwnd : windows->second) {
        NOTIFYICONIDENTIFIER id{};
        id.cbSize = sizeof(id);
        id.hWnd = hwnd;
        id.uID = icon.uid;
        RECT rect{};
        if (SUCCEEDED(Shell_NotifyIconGetRect(&id, &rect))) {
            if (UsableTrayRect(rect)) {
                icon.screenRect = rect;
                icon.hasScreenRect = true;
            }
            return true;
        }
    }
    return false;
}

size_t FilterLiveIcons(std::vector<TrayIcon>& icons, size_t& stale) {
    stale = 0;
    std::unordered_set<std::wstring> needed;
    for (const TrayIcon& icon : icons) {
        if (icon.guid == GUID{} && icon.hasUid && !icon.exe.empty()) needed.insert(icon.exe);
    }
    const WindowIndex index = CollectTrayWindowIndex(needed);
    std::vector<TrayIcon> live;
    live.reserve(icons.size());
    for (TrayIcon& icon : icons) {
        if (ProbeTrayIconLive(icon, index)) {
            live.push_back(std::move(icon));
        } else {
            ++stale;
        }
    }
    icons = std::move(live);
    return icons.size();
}

}  // namespace

bool IsTrayIconShown(const TrayIcon& icon, const Settings& settings) {
    const auto id = std::wcstoull(icon.id.c_str(), nullptr, 10);
    if (std::find(settings.trayHideIds.begin(), settings.trayHideIds.end(), id) != settings.trayHideIds.end()) {
        return false;
    }
    if (std::find(settings.trayShowIds.begin(), settings.trayShowIds.end(), id) != settings.trayShowIds.end()) {
        return true;
    }
    return icon.promoted;
}

TrayReader::TrayReader() {
    // The owning thread has already initialized COM (STA). Creating the WIC
    // factory here keeps the multi-millisecond COM/codec startup off the
    // periodic Refresh() path; if COM is not ready yet the factory is simply
    // created on the first Refresh() instead.
    EnsureWicFactory();
}

TrayReader::~TrayReader() = default;

bool TrayReader::EnsureWicFactory() {
    if (wic_) return true;
    ComPtr<IWICImagingFactory> factory;
    const HRESULT result = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                            IID_PPV_ARGS(factory.put()));
    if (FAILED(result)) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            sm::LogHr(L"TrayReader: CoCreateInstance(CLSID_WICImagingFactory)", result);
        }
        return false;
    }
    wic_ = std::move(factory);
    return true;
}

bool TrayReader::Refresh() {
    const double startedMs = sm::NowMs();
    const bool wicReady = EnsureWicFactory();
    std::vector<RegistryRecord> records = ReadRegistryRecords();

    std::vector<TrayIcon> next;
    next.reserve(records.size());
    std::unordered_set<std::wstring> liveIds;
    liveIds.reserve(records.size());
    size_t ownRecords = 0;

    for (const RegistryRecord& record : records) {
        const std::wstring exe = ResolveExecutablePath(record.executablePath, folderCache_);
        if (IsOwnExecutable(record, exe)) {
            ++ownRecords;
            continue;
        }
        liveIds.insert(record.id);

        TrayIcon icon;
        icon.id = record.id;
        icon.exe = exe;
        icon.promoted = record.promoted;
        icon.order = record.order;
        icon.label = BuildLabel(record, exe, descriptionCache_);
        icon.hasUid = record.hasUid;
        icon.uid = record.uid;
        icon.guid = record.guid;

        if (wicReady) {
            const unsigned long long snapshotHash = record.hasSnapshot ? HashBytes(record.snapshot) : 0;
            const auto cached = imageCache_.find(record.id);
            if (cached != imageCache_.end() && cached->second.snapshotHash == snapshotHash &&
                cached->second.exe == exe) {
                icon.pixels = cached->second.pixels;
                icon.imageId = cached->second.imageId;
                icon.width = static_cast<int>(kIconSize);
                icon.height = static_cast<int>(kIconSize);
            } else {
                std::vector<uint8_t> pixels;
                bool decoded = record.hasSnapshot && DecodeSnapshotPng(wic_.get(), record.snapshot, pixels);
                if (!decoded) decoded = DecodeExecutableIcon(wic_.get(), exe, pixels);
                if (decoded && pixels.size() == static_cast<size_t>(kIconSize) * kIconSize * 4) {
                    icon.width = static_cast<int>(kIconSize);
                    icon.height = static_cast<int>(kIconSize);
                    icon.pixels = std::move(pixels);
                    icon.imageId = HashPixels(icon.pixels, icon.width, icon.height);
                    imageCache_[record.id] = CachedImage{snapshotHash, exe, icon.pixels, icon.imageId};
                } else {
                    // Do not cache failures: a broken snapshot or a missing
                    // executable may be fixed while the bar is running.
                    imageCache_.erase(record.id);
                }
            }
        }
        next.push_back(std::move(icon));
    }

    // Forget records that disappeared from the registry.
    for (auto entry = imageCache_.begin(); entry != imageCache_.end();) {
        if (liveIds.count(entry->first) == 0) {
            entry = imageCache_.erase(entry);
        } else {
            ++entry;
        }
    }

    std::stable_sort(next.begin(), next.end(), SortKeyLess);

    // Collapse records that Explorer keeps twice for the same visual icon: the
    // pair is only a duplicate when both the decoded pixels and the label match,
    // so different applications sharing one icon stay separate.
    std::vector<TrayIcon> visible;
    visible.reserve(next.size());
    std::unordered_set<std::wstring> seen;
    size_t duplicateRecords = 0;
    for (TrayIcon& icon : next) {
        if (icon.imageId != 0) {
            std::wstring key = std::to_wstring(icon.imageId);
            key.push_back(static_cast<wchar_t>(0x1F));
            key += icon.label;
            if (!seen.insert(std::move(key)).second) {
                ++duplicateRecords;
                continue;
            }
        }
        visible.push_back(std::move(icon));
    }

    // Windows persists tray records for programs that are no longer running.
    // Like Atrium, prove liveness through the shell: a GUID icon must resolve,
    // and a UID icon must resolve on one of its owning process's windows.
    // Without this filter the bar mirrors dead records and looks cluttered.
    size_t staleRecords = 0;
    if (ShellTrayResponsive()) {
        FilterLiveIcons(visible, staleRecords);

        // Hidden records all resolve to the overflow chevron rectangle. Mark
        // those as proxies for opening the real hidden-icon panel, and never
        // use that shared rectangle as an icon image/click target.
        std::unordered_map<unsigned long long, int> rectCounts;
        auto rectKey = [](const RECT& r) {
            return (static_cast<unsigned long long>(static_cast<unsigned short>(r.left)) << 48) |
                   (static_cast<unsigned long long>(static_cast<unsigned short>(r.top)) << 32) |
                   (static_cast<unsigned long long>(static_cast<unsigned short>(r.right)) << 16) |
                   static_cast<unsigned long long>(static_cast<unsigned short>(r.bottom));
        };
        for (const TrayIcon& icon : visible) {
            if (icon.hasScreenRect) ++rectCounts[rectKey(icon.screenRect)];
        }
        for (TrayIcon& icon : visible) {
            if (!icon.hasScreenRect) continue;
            if (rectCounts[rectKey(icon.screenRect)] > 1) {
                icon.overflowProxy = true;
                icon.overflowRect = icon.screenRect;
                icon.hasScreenRect = false;
                continue;
            }
            // Read the current taskbar pixels for a visible promoted icon. The
            // persisted IconSnapshot can predate the app's latest state (e.g.
            // Tailscale), while these pixels are what Windows is showing now.
            std::vector<uint8_t> captured;
            // An auto-hidden taskbar keeps an on-screen window rect, but the
            // pixels at that location are wallpaper. Never harvest those.
            const bool taskbarOwns = TaskbarOwnsPoint(icon.screenRect);
            const bool capturedOk = taskbarOwns && CaptureScreenIcon(icon.screenRect, captured);
            if (capturedOk) {
                icon.pixels = std::move(captured);
                icon.width = static_cast<int>(kIconSize);
                icon.height = static_cast<int>(kIconSize);
                icon.imageId = HashPixels(icon.pixels, icon.width, icon.height);
            }
        }
    } else {
        Log(L"TrayReader: shell not responsive; keeping previous tray list");
        lastReadMs_ = sm::NowMs() - startedMs;
        return false;
    }

    if (host_) MergeHosted(visible);

    bool changed = visible.size() != icons_.size();
    if (!changed) {
        for (size_t index = 0; index < visible.size(); ++index) {
            if (!SameIcon(visible[index], icons_[index])) {
                changed = true;
                break;
            }
        }
    }
    icons_ = std::move(visible);
    lastReadMs_ = sm::NowMs() - startedMs;

    if (changed) {
        const size_t promoted =
            static_cast<size_t>(std::count_if(icons_.begin(), icons_.end(), [](const TrayIcon& icon) {
                return icon.promoted;
            }));
        const size_t withRect =
            static_cast<size_t>(std::count_if(icons_.begin(), icons_.end(), [](const TrayIcon& icon) {
                return icon.hasScreenRect;
            }));
        sm::Log(L"TrayReader: %u icons (%u promoted, %u clickable) from %u records in %.2f ms (own=%u duplicates=%u stale=%u)",
                static_cast<unsigned>(icons_.size()), static_cast<unsigned>(promoted),
                static_cast<unsigned>(withRect),
                static_cast<unsigned>(records.size()), lastReadMs_, static_cast<unsigned>(ownRecords),
                static_cast<unsigned>(duplicateRecords), static_cast<unsigned>(staleRecords));
    }
    return changed;
}

bool TrayReader::MakeHostedIcon(const atrium::HostIcon& host, TrayIcon& icon) {
    if (!host.image || host.image->bgra.empty() || host.image->width <= 0 || host.image->height <= 0) {
        return false;
    }
    const atrium::IconImage fitted = atrium::FitToSquare(*host.image, static_cast<int>(kIconSize));
    icon.pixels = fitted.bgra;
    icon.width = fitted.width;
    icon.height = fitted.height;
    icon.imageId = HashPixels(icon.pixels, icon.width, icon.height);
    return true;
}

void TrayReader::MergeHosted(std::vector<TrayIcon>& icons) {
    if (!host_ || !host_->Running() || host_->Yielded()) return;
    wchar_t ownPath[MAX_PATH * 2]{};
    GetModuleFileNameW(nullptr, ownPath, static_cast<DWORD>(std::size(ownPath)));
    std::wstring own = ownPath;
    std::transform(own.begin(), own.end(), own.begin(), std::towlower);
    const std::vector<atrium::HostIcon> hosted = host_->Icons();
    std::vector<bool> matched(icons.size(), false);
    for (const atrium::HostIcon& host : hosted) {
        if (!host.exe.empty() && host.exe == own) continue;
        int match = -1;
        GUID hostGuid{};
        bool hasGuid = false;
        if (host.key.rfind(L"guid:", 0) == 0) {
            hasGuid = CLSIDFromString(host.key.c_str() + 5, &hostGuid) == NOERROR;
        }
        for (size_t i = 0; i < icons.size(); ++i) {
            if (matched[i]) continue;
            bool same = false;
            if (hasGuid && icons[i].guid != GUID{}) {
                same = IsEqualGUID(icons[i].guid, hostGuid);
            } else if (!hasGuid && icons[i].hasUid) {
                same = icons[i].uid == host.uid && icons[i].exe == host.exe;
            }
            if (same) {
                match = static_cast<int>(i);
                break;
            }
        }
        if (match >= 0) {
            TrayIcon& icon = icons[static_cast<size_t>(match)];
            matched[static_cast<size_t>(match)] = true;
            icon.hostKey = host.key;
            icon.hosted = true;
            icon.hasScreenRect = false;
            icon.overflowProxy = false;
            if (!host.tip.empty()) {
                const size_t line = host.tip.find(L'\n');
                icon.label = host.tip.substr(0, line);
            }
            TrayIcon imageSource;
            if (MakeHostedIcon(host, imageSource)) {
                icon.pixels = std::move(imageSource.pixels);
                icon.width = imageSource.width;
                icon.height = imageSource.height;
                icon.imageId = imageSource.imageId;
            }
            continue;
        }
        TrayIcon icon;
        icon.id = host.key;
        icon.hostKey = host.key;
        icon.hosted = true;
        icon.exe = host.exe;
        icon.hasUid = host.uid != 0;
        icon.uid = host.uid;
        if (hasGuid) icon.guid = hostGuid;
        icon.promoted = !host.hidden;
        icon.order = 1000000ull + host.added;
        const std::wstring tip = host.tip.substr(0, host.tip.find(L'\n'));
        if (!tip.empty()) {
            icon.label = tip;
        } else {
            const size_t slash = host.exe.find_last_of(L"\\/");
            const std::wstring name = slash == std::wstring::npos ? host.exe : host.exe.substr(slash + 1);
            const size_t dot = name.find_last_of(L'.');
            icon.label = dot == std::wstring::npos ? name : name.substr(0, dot);
        }
        if (MakeHostedIcon(host, icon)) icons.push_back(std::move(icon));
    }
    std::sort(icons.begin(), icons.end(), SortKeyLess);
}

}  // namespace sm
