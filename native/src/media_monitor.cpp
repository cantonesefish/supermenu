#include "media_monitor.h"

#include <d2d1.h>

#include <algorithm>
#include <cstdlib>
#include <sstream>

namespace sm {
namespace {

constexpr UINT kCoverSize = 96;

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), length);
    return result;
}

std::string TrimAscii(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::wstring Join(const std::wstring& left, const std::wstring& right) {
    if (left.empty()) return right;
    if (left.back() == L'\\' || left.back() == L'/') return left + right;
    return left + L"\\" + right;
}

bool FileExists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

}  // namespace

MediaMonitor::~MediaMonitor() { Stop(); }

void MediaMonitor::Start() {
    lastStartMs_ = NowMs();
    const std::wstring local = LocalAppData();
    if (local.empty()) return;
    directory_ = Join(Join(local, L"SuperMenu"), L"media");
    CreateDirectoryW(Join(local, L"SuperMenu").c_str(), nullptr);
    CreateDirectoryW(directory_.c_str(), nullptr);
    statePath_ = Join(directory_, L"state.txt");
    coverPath_ = Join(directory_, L"cover.bin");
    appIconPath_ = Join(directory_, L"appicon.png");

    std::vector<std::wstring> candidates = {
        Join(ExeDirectory(), L"Media\\SuperMenu.Media.exe"),
        Join(ExeDirectory(), L"..\\..\\..\\media\\SuperMenu.Media\\bin\\Release\\net10.0-windows10.0.19041.0\\SuperMenu.Media.exe"),
    };
    std::wstring helper;
    for (const auto& candidate : candidates) {
        if (FileExists(candidate)) {
            helper = candidate;
            break;
        }
    }
    if (helper.empty()) {
        Log(L"media: SuperMenu.Media.exe not found; metadata disabled");
        return;
    }

    std::wstring command = L"\"" + helper + L"\" --parent " + std::to_wstring(GetCurrentProcessId()) + L" --dir \"" +
                           directory_ + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                       &process)) {
        process_ = process.hProcess;
        processId_ = process.dwProcessId;
        CloseHandle(process.hThread);
        Log(L"media: helper started pid=%lu", processId_);
    } else {
        Log(L"media: helper launch failed (%lu)", GetLastError());
    }
}

void MediaMonitor::Stop() {
    if (process_) {
        TerminateProcess(process_, 0);
        WaitForSingleObject(process_, 500);
        CloseHandle(process_);
        process_ = nullptr;
        processId_ = 0;
    }
}

bool MediaMonitor::EnsureWic() {
    if (wic_) return true;
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory),
                                reinterpret_cast<void**>(factory.put())))) {
        return false;
    }
    wic_ = std::move(factory);
    return true;
}

std::string MediaMonitor::ReadFileBytes(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 16 * 1024 * 1024) {
        CloseHandle(file);
        return {};
    }
    std::string result(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    if (!ReadFile(file, result.data(), static_cast<DWORD>(result.size()), &read, nullptr)) {
        CloseHandle(file);
        return {};
    }
    CloseHandle(file);
    result.resize(read);
    return result;
}

void MediaMonitor::ParseState(const std::string& text, MediaInfo& out) const {
    out = MediaInfo{};
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t equals = line.find('=');
        if (equals == std::string::npos) continue;
        const std::string key = line.substr(0, equals);
        const std::string value = line.substr(equals + 1);
        if (key == "active") {
            out.active = value == "1";
        } else if (key == "title") {
            out.title = Utf8ToWide(value);
        } else if (key == "artist") {
            out.artist = Utf8ToWide(value);
        } else if (key == "album") {
            out.album = Utf8ToWide(value);
        } else if (key == "playing") {
            out.playing = value == "1";
        } else if (key == "position") {
            try {
                out.positionMs = std::clamp(std::stoi(value), 0, 86400000);
            } catch (...) {
            }
        } else if (key == "duration") {
            try {
                out.durationMs = std::clamp(std::stoi(value), 0, 86400000);
            } catch (...) {
            }
        } else if (key == "cover_rev") {
            try {
                out.coverRevision = std::stoull(value);
            } catch (...) {
            }
        } else if (key == "app_icon_rev") {
            try {
                out.appIconRevision = std::stoull(value);
            } catch (...) {
            }
        }
    }
    if (!out.active) {
        out.title.clear();
        out.artist.clear();
        out.album.clear();
        out.playing = false;
        out.positionMs = 0;
        out.durationMs = 0;
        out.coverRevision = 0;
        out.appIconRevision = 0;
    }
}

bool MediaMonitor::DecodePngFile(const std::wstring& path, std::vector<uint8_t>& pixels) {
    if (!EnsureWic()) return false;
    const std::string bytes = ReadFileBytes(path);
    if (bytes.empty() || bytes.size() > 16 * 1024 * 1024) return false;

    ComPtr<IWICStream> stream;
    if (FAILED(wic_->CreateStream(stream.put()))) return false;
    if (FAILED(stream->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<char*>(bytes.data())),
                                            static_cast<DWORD>(bytes.size())))) {
        return false;
    }
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(wic_->CreateDecoderFromStream(stream.get(), nullptr, WICDecodeMetadataCacheOnLoad, decoder.put()))) {
        return false;
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, frame.put()))) return false;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(wic_->CreateFormatConverter(converter.put()))) return false;
    if (FAILED(converter->Initialize(frame.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom))) {
        return false;
    }
    ComPtr<IWICBitmapScaler> scaler;
    if (FAILED(wic_->CreateBitmapScaler(scaler.put()))) return false;
    if (FAILED(scaler->Initialize(converter.get(), kCoverSize, kCoverSize, WICBitmapInterpolationModeFant))) return false;
    const UINT stride = kCoverSize * 4;
    pixels.assign(static_cast<size_t>(stride) * kCoverSize, 0);
    return SUCCEEDED(scaler->CopyPixels(nullptr, stride, static_cast<UINT>(pixels.size()), pixels.data()));
}

bool MediaMonitor::LoadCover(unsigned long long revision) {
    if (revision == 0 || revision == lastCoverRevision_) return false;
    lastCoverRevision_ = revision;
    std::vector<uint8_t> pixels;
    if (!DecodePngFile(coverPath_, pixels)) return false;
    info_.coverPixels = std::move(pixels);
    info_.coverWidth = static_cast<int>(kCoverSize);
    info_.coverHeight = static_cast<int>(kCoverSize);
    info_.coverRevision = revision;
    return true;
}

bool MediaMonitor::LoadAppIcon(unsigned long long revision) {
    if (revision == 0 || revision == lastAppIconRevision_) return false;
    lastAppIconRevision_ = revision;
    std::vector<uint8_t> pixels;
    if (!DecodePngFile(appIconPath_, pixels)) return false;
    info_.appIconPixels = std::move(pixels);
    info_.appIconWidth = static_cast<int>(kCoverSize);
    info_.appIconHeight = static_cast<int>(kCoverSize);
    info_.appIconRevision = revision;
    return true;
}

bool MediaMonitor::Poll() {
    const double now = NowMs();
    if (now - lastPollMs_ < 250.0) return false;
    lastPollMs_ = now;

    if (process_) {
        const DWORD wait = WaitForSingleObject(process_, 0);
        if (wait == WAIT_OBJECT_0) {
            CloseHandle(process_);
            process_ = nullptr;
            processId_ = 0;
            Log(L"media: helper exited");
        }
    }
    if (!process_ && now - lastStartMs_ > 5000.0) Start();

    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (!GetFileAttributesExW(statePath_.c_str(), GetFileExInfoStandard, &attributes)) {
        if (info_.active) {
            info_ = MediaInfo{};
            lastCoverRevision_ = 0;
            return true;
        }
        return false;
    }
    const std::string text = ReadFileBytes(statePath_);
    MediaInfo next;
    ParseState(text, next);
    if (next.coverRevision == 0 || !next.active) lastCoverRevision_ = 0;
    if (next.coverRevision > 0 && next.coverRevision != lastCoverRevision_) {
        next.coverPixels = info_.coverPixels;
        next.coverWidth = info_.coverWidth;
        next.coverHeight = info_.coverHeight;
        const bool loaded = LoadCover(next.coverRevision);
        Log(L"media: cover revision=%llu loaded=%d bytes=%zu", next.coverRevision, loaded ? 1 : 0,
            info_.coverPixels.size());
        if (info_.coverRevision == next.coverRevision) {
            next.coverPixels = info_.coverPixels;
            next.coverWidth = info_.coverWidth;
            next.coverHeight = info_.coverHeight;
        }
    } else if (next.coverRevision == info_.coverRevision) {
        next.coverPixels = info_.coverPixels;
        next.coverWidth = info_.coverWidth;
        next.coverHeight = info_.coverHeight;
    }

    if (next.appIconRevision == 0 || !next.active) lastAppIconRevision_ = 0;
    if (next.appIconRevision > 0 && next.appIconRevision != lastAppIconRevision_) {
        next.appIconPixels = info_.appIconPixels;
        next.appIconWidth = info_.appIconWidth;
        next.appIconHeight = info_.appIconHeight;
        const bool loaded = LoadAppIcon(next.appIconRevision);
        Log(L"media: app icon revision=%llu loaded=%d bytes=%zu", next.appIconRevision, loaded ? 1 : 0,
            info_.appIconPixels.size());
        if (info_.appIconRevision == next.appIconRevision) {
            next.appIconPixels = info_.appIconPixels;
            next.appIconWidth = info_.appIconWidth;
            next.appIconHeight = info_.appIconHeight;
        }
    } else if (next.appIconRevision == info_.appIconRevision) {
        next.appIconPixels = info_.appIconPixels;
        next.appIconWidth = info_.appIconWidth;
        next.appIconHeight = info_.appIconHeight;
    }

    const bool changed = next.active != info_.active || next.title != info_.title || next.artist != info_.artist ||
                         next.album != info_.album || next.playing != info_.playing ||
                         next.positionMs != info_.positionMs || next.durationMs != info_.durationMs ||
                         next.coverRevision != info_.coverRevision ||
                         next.appIconRevision != info_.appIconRevision;
    if (changed) info_ = std::move(next);
    return changed;
}

}  // namespace sm
