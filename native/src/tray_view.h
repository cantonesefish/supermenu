#pragma once

// Display helpers for the notification-area icons.
//
// The list itself belongs to atrium::TrayModel (native/src/atrium/shell/tray_model.h);
// these are small pure functions over one atrium::TrayIcon so the bar and the tray
// popup agree on which icons are visible, what they are called and when two model
// snapshots describe the same icon. Nothing here reads Windows or the model state.

#include "atrium/shell/tray_model.h"
#include "settings.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace sm {

// The numeric NotifyIconSettings record key of `icon` ("1234"), when the model
// knows one. Run-only hosted icons have no record: the settings dialog edits
// registry records by this number, so those icons cannot be overridden.
inline std::optional<unsigned long long> TrayIconRecordNumber(const atrium::TrayIcon& icon) {
    if (icon.recordKey.empty()) return std::nullopt;
    unsigned long long value = 0;
    for (const wchar_t ch : icon.recordKey) {
        if (ch < L'0' || ch > L'9') return std::nullopt;
        const unsigned long long digit = static_cast<unsigned long long>(ch - L'0');
        if (value > (std::numeric_limits<unsigned long long>::max() - digit) / 10) return std::nullopt;
        value = value * 10 + digit;
    }
    return value;
}

// Effective placement after the user's per-record overrides: trayHide wins, then
// trayShow, otherwise Explorer's own promoted/hidden state. With no override the
// bar mirrors the Windows tray exactly.
inline bool TrayIconShown(const atrium::TrayIcon& icon, const Settings& settings) {
    const auto number = TrayIconRecordNumber(icon);
    if (!number) return icon.promoted;
    auto contains = [&](const std::vector<unsigned long long>& ids) {
        return std::find(ids.begin(), ids.end(), *number) != ids.end();
    };
    if (contains(settings.trayHideIds)) return false;
    if (contains(settings.trayShowIds)) return true;
    return icon.promoted;
}

// The icon's display name: the first line of its tooltip, else its program's file
// name without ".exe" (the model hands over lower-case full paths), else empty.
inline std::wstring TrayIconLabel(const atrium::TrayIcon& icon) {
    std::wstring title = atrium::TrayModel::Title(icon.tip);
    if (!title.empty()) return title;
    if (icon.exe.empty()) return {};
    const size_t slash = icon.exe.find_last_of(L"\\/");
    std::wstring name = slash == std::wstring::npos ? icon.exe : icon.exe.substr(slash + 1);
    if (name.size() > 4 && EndsWith(name, L".exe")) name.resize(name.size() - 4);
    return name;
}

// A key that stays equal for the same icon across model refreshes: the model's
// stable identity (record id, then tray-host key, then program), never position
// or displayed label. The tooltip is the last resort so two unknown icons are
// not treated as the same one; the empty string means "no identity at all".
inline std::wstring TrayIconIdentity(const atrium::TrayIcon& icon) {
    if (!icon.recordId.empty()) return icon.recordId;
    if (!icon.hostKey.empty()) return icon.hostKey;
    if (!icon.exe.empty()) return L"exe:" + icon.exe;
    if (!icon.tip.empty()) return L"tip:" + icon.tip;
    return {};
}

}  // namespace sm
