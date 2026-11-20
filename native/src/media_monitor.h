#pragma once

#include "common.h"

#include <wincodec.h>

#include <string>
#include <vector>

namespace sm {

// Snapshot produced by the small .NET/WinRT helper. Keeping the WinRT
// projection out of the MinGW bar preserves the single native process while
// still giving the Control Center real SystemMediaTransportControls data.
struct MediaInfo {
    bool active = false;
    std::wstring title;
    std::wstring artist;
    std::wstring album;
    bool playing = false;
    int positionMs = 0;
    int durationMs = 0;
    unsigned long long coverRevision = 0;
    int coverWidth = 0;
    int coverHeight = 0;
    std::vector<uint8_t> coverPixels;  // 96x96 premultiplied BGRA
    unsigned long long appIconRevision = 0;
    int appIconWidth = 0;
    int appIconHeight = 0;
    std::vector<uint8_t> appIconPixels;  // 96x96 premultiplied BGRA
};

class MediaMonitor {
public:
    MediaMonitor() = default;
    MediaMonitor(const MediaMonitor&) = delete;
    MediaMonitor& operator=(const MediaMonitor&) = delete;
    ~MediaMonitor();

    void Start();
    void Stop();
    // Returns true when the visible snapshot changed (metadata, progress or cover).
    bool Poll();
    const MediaInfo& info() const { return info_; }

private:
    bool EnsureWic();
    bool DecodePngFile(const std::wstring& path, std::vector<uint8_t>& pixels);
    bool LoadCover(unsigned long long revision);
    bool LoadAppIcon(unsigned long long revision);
    void ParseState(const std::string& text, MediaInfo& out) const;
    static std::string ReadFileBytes(const std::wstring& path);

    HANDLE process_ = nullptr;
    DWORD processId_ = 0;
    double lastStartMs_ = -1.0;
    double lastPollMs_ = -1.0;
    std::wstring directory_;
    std::wstring statePath_;
    std::wstring coverPath_;
    std::wstring appIconPath_;
    MediaInfo info_;
    unsigned long long lastCoverRevision_ = 0;
    unsigned long long lastAppIconRevision_ = 0;
    ComPtr<IWICImagingFactory> wic_;
};

}  // namespace sm
