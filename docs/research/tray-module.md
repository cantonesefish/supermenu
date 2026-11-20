# Tray registry reader (`native/src/tray.h`, `tray.cpp`)

Status: implemented, standalone-verified, integrated by the lead. Read-only with
respect to the registry and settings; it never registers or modifies tray icons.

## What it does

`sm::TrayReader` reads `HKCU\Control Panel\NotifyIconSettings` (Explorer's
Windows 11 notification-area persistence) and exposes a render-ready list:

- Every subkey is a decimal-uint64 record id. For each record it reads
  `IconSnapshot` (PNG), `ExecutablePath`, `InitialTooltip`, `IsPromoted`.
- `UIOrderList` (root, REG_BINARY) is parsed as little-endian uint64 record ids;
  the index is stored in `TrayIcon::order`, and the result is sorted by it
  (records without an order would sort last by label then id).
- `ExecutablePath` values starting with `{FOLDERID-GUID}\...` are resolved with
  `SHGetKnownFolderPath`; `%VAR%` is expanded; the result is stored absolute and
  lower-case in `TrayIcon::exe` (empty if it cannot be made absolute).
- `label`: cleaned `InitialTooltip` first line (U+202A..202E, U+200E/200F,
  U+2066..2069 removed, CR/LF/TAB handled, trimmed), else the executable's
  version-resource `FileDescription`, else its file stem.
- `pixels`: 32x32, 32bpp **premultiplied BGRA** (`width*height*4 == 4096`).
  `IconSnapshot` PNGs are decoded with WIC and scaled with aspect preserved
  (`WICBitmapInterpolationModeFant`) onto a transparent 32x32 canvas (non-square
  and 16x16/20x20/86x86/128x128 snapshots are handled). If the snapshot is
  missing/invalid, the executable's shell small icon is used
  (`SHGetFileInfoW(SHGFI_ICON|SHGFI_SMALLICON)` -> `CreateBitmapFromHICON` ->
  PBGRA). If everything fails the record keeps empty `pixels` and `imageId == 0`.
- `imageId`: FNV-1a over dimensions+pixels, stable across instances/refreshes;
  suitable as a D2D bitmap cache key.
- Records whose executable is `SuperMenuBar.exe` or `SuperMenu.Settings.exe`
  (checked on raw and resolved file name) are excluded.
- Duplicates (same `imageId` **and** same `label`) are collapsed, keeping the
  earliest by `order`; same icon with a different label is kept separate.

## Interface

```cpp
namespace sm {
class TrayReader {
public:
    TrayReader();                      // creates the WIC factory once (COM STA thread)
    ~TrayReader();
    bool Refresh();                    // true when set/content/order/promotion/image changed
    const std::vector<TrayIcon>& icons() const;
    double lastReadMs() const;         // self-timing of the last Refresh() (diagnostics)
};
}
```

Threading: call the constructor, `Refresh()` and the destructor on the
COM-initialized UI thread (`CoInitializeEx(COINIT_APARTMENTTHREADED)` already
done by the app). No C++/WinRT, no WRL; links `windowscodecs ole32 oleaut32
uuid version shlwapi advapi32 shell32 user32`.

## Verification (standalone harness, not part of the repo build)

Compile command used (same flags as `native/CMakeLists.txt` minus `-mwindows`):

```
C:\msys64\ucrt64\bin\g++.exe -std=c++20 -O2 -Wall -Wextra
  -Wno-unused-parameter -Wno-cast-function-type -Wno-unused-function
  -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_WIN32_WINNT=0x0A00
  -finput-charset=UTF-8 -fexec-charset=UTF-8 -municode -static -I native/src
  native/src/tray.cpp tray_test.cpp -o tray_test.exe
  -lwindowscodecs -lole32 -loleaut32 -luuid -lversion -lshlwapi
  -ladvapi32 -lshell32 -luser32 -lgdi32
```

Build: 0 errors, 0 warnings. Result: **37/37 checks passed**, `RESULT: PASS`.

This machine (verified by an independent registry probe in the harness):

- raw records: **37**, `IsPromoted=1`: **9**, `UIOrderList`: 37 ids / 296 bytes
- excluded as SuperMenu's own: **2** records
  (`dist\SuperMenuBar.exe` and `native\build\dist\SuperMenuBar.exe`)
- collapsed duplicates: **2** (NVIDIA `NVIDIA 设置` pair; the two identical
  `explorer.exe` 20x20 snapshots that both fall back to "Windows 资源管理器")
- visible result: **33 icons = 9 promoted + 24 hidden**; sorted exactly in
  `UIOrderList` order (positions 0,1,26,34 are the excluded/collapsed ones)
- Intel "® Graphics Software"/"英特尔® Graphics Software" pair: same imageId,
  different labels -> correctly kept as 2 entries
- Office tooltip `‪Microsoft 365和 Office 正在后台安装。\n单击此处查看状态。‬` becomes
  `Microsoft 365和 Office 正在后台安装。` (bidi controls stripped, first line only)
- every pixel premultiplied (r,g,b <= a), no fully transparent icons, all
  records have absolute lower-case GUID-free `exe`, every label non-empty
- `SHGetFileInfoW` + `CreateBitmapFromHICON` + PBGRA fallback pipeline verified
  separately on `C:\Windows\explorer.exe` (16x16 -> premultiplied BGRA)

Timing (`TrayReader::Refresh()`, `lastReadMs()`): typical first refresh after
process start **12.4-12.9 ms** (cold WIC/PNG codecs, 37 decodes, version-resource
queries; one cold-run outlier of 15.55 ms was seen when the freshly built test
ran under load), second/third refresh **0.89-1.34 ms**. A second reader instance
in the same process (cold decode, warm codecs) takes ~6.3-7.7 ms.

## Caching / performance notes

`TrayReader` caches across `Refresh()` calls (per reader instance):

- decoded icons keyed by record id + FNV-1a fingerprint of the raw snapshot
  bytes + resolved exe path (failure case is not cached so it can recover);
- `FileDescription` strings keyed by exe path;
- known-folder GUID -> path resolutions.

So a warm `Refresh()` only re-reads the registry (~0.85 ms), re-hashes the
snapshots and rebuilds the list; it does **not** decode PNGs or query version
resources. Caches are pruned to live record ids. Note: constructing a new
`TrayReader` throws the caches away -- keep one reader alive.

The WIC factory is created in the constructor (the contract allows
constructor/Refresh creation) to keep its ~2 ms startup off the first
`Refresh()`. If the constructor runs before COM is initialized the factory
creation simply retries on the first `Refresh()`.

If the app measures ~109 ms per refresh, that time is outside this module
(its own `Refresh()` self-times at ~1 ms warm / ~12.5 ms cold). Likely
candidates in the renderer: re-creating `ID2D1Bitmap`s from `pixels` for all
icons every refresh, per-icon logging, or rebuilding the whole tray panel.
Suggested: log `reader.lastReadMs()` separately from the panel rebuild; cache
`ID2D1Bitmap`s by `imageId` (`DXGI_FORMAT_B8G8R8A8_UNORM` +
`D2D1_ALPHA_MODE_PREMULTIPLIED`, 32x32, 96 dpi) and only rebuild when
`imageId` changes; log once per content change.

## Known limitations / possible follow-ups

- Tips that are localization resource ids (e.g. Huawei Cloud's
  `IDS_MAIN_PAGE_TITLE`) are used verbatim; a follow-up could treat
  `IDS_*` like an empty tooltip and fall back to FileDescription.
- Unknown leading `{GUID}` folders: record keeps `exe == ""` and label falls
  back to the file stem; pixels still decode from the snapshot.
- Labels are cleaned with the `C` locale (`iswspace`); only ASCII whitespace,
  NBSP and U+3000 are trimmed.
