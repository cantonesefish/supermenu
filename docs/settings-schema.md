# SuperMenu settings contract

The native bar (`native/`) and the WPF settings app (`settings/SuperMenu.Settings/`) share one file:

```
%LOCALAPPDATA%\SuperMenu\settings.json
```

- UTF-8 (no BOM preferred), flat JSON object, camelCase keys. No arrays or nested objects in v1.
- Missing or malformed keys fall back to the defaults below. Unknown keys are preserved where practical but ignored by the native bar.
- Native bar watches the file and also reloads when it receives the broadcast message registered as `SuperMenu.SettingsChanged` (`RegisterWindowMessageW`). The settings app writes the file atomically (tmp + replace) and broadcasts that message after writing.
- A running native bar must never crash on a malformed/incomplete file: keep the last good settings.
- Values are DIP (device-independent pixels, 1/96 inch) unless a key says px.

## Defaults and ranges

| Key | Type | Default | Range / values | Meaning |
| --- | --- | --- | --- | --- |
| `version` | int | `1` | 1 | Schema version. |
| `language` | string | `"zh-CN"` | `"zh-CN"`, `"en-US"` | UI language of the bar and settings app. |
| `theme` | string | `"auto"` | `"auto"`, `"light"`, `"dark"` | `auto` follows Windows app theme. Controls tint/wash; ink still adapts to the real backdrop. |
| `logoStyle` | string | `"apple"` | `"apple"`, `"windows"`, `"windows-mono"` | Leftmost menu button artwork: Apple silhouette, colored Windows 11 four-square logo, or ink-colored Windows logo. |
| `barHeight` | double | `28` | 24–44 DIP | Height of the glass bar (not including the drop shadow). |
| `monitor` | int | `0` | 0–15 | Monitor index (`0` = primary). Native clamps to connected monitors. |
| `reserveWorkArea` | bool | `true` | | Register an appbar and reserve the bar strip so maximized windows sit below it. |
| `autoHide` | bool | `false` | | Slide the bar out when the pointer leaves it. |
| `hideInFullscreen` | bool | `true` | | Hide the bar while a fullscreen app/game is foreground. |
| `showSeconds` | bool | `false` | | Include seconds in the clock. |
| `use24Hour` | bool | `true` | | 24-hour clock. |
| `showDate` | bool | `true` | | Show the weekday/date next to the clock. |
| `showAppName` | bool | `true` | | Show the foreground app's display name (bold) in the menu strip. |
| `showAppleMenu` | bool | `true` | | Show the leftmost logo menu. |
| `showWifi` | bool | `true` | | Network glyph in the merged right status block: auto-switches between wired Ethernet (`E839`), Wi-Fi (`E701`) and no connection. |
| `showBattery` | bool | `true` | | Battery status icon in the right cluster. |
| `showBatteryPercent` | bool | `false` | | Show the battery percentage beside the icon. |
| `showVolume` | bool | `false` | | Volume icon in the right cluster (macOS hides it by default). |
| `showControlCenter` | bool | `true` | | Control Center icon. |
| `showSearch` | bool | `true` | | Spotlight/search icon (opens Windows search in v1). |
| `showNotifications` | bool | `false` | | Optional notification bell. |
| `rightIconSpacing` | int | `20` | 2–24 DIP | Gap between right-side items (merged status block, search, clock, tray icons). |
| `trayShow` | string | `""` | `;`-separated decimal record IDs | Force these Explorer tray records into the top bar, regardless of `IsPromoted`. |
| `trayHide` | string | `""` | `;`-separated decimal record IDs | Force these Explorer tray records into the hidden dropdown, regardless of `IsPromoted`. |
| `fontScale` | double | `1.0` | 0.85–1.25 | Scales bar text only. |
| `glassPreset` | string | `"liquid"` | `"liquid"`, `"frosted"`, `"clear"` | Preset for the glass parameters. Individual values below still apply. |
| `transparency` | double | `0.94` | 0–1 | 1 = clear backdrop, 0 = fully tinted. |
| `blur` | double | `0.52` | 0–1 | Frost amount. |
| `refraction` | double | `18` | 0–40 | Rim lens displacement in px. |
| `edge` | double | `18` | 0–32 | Width of the refracting rim in px. |
| `dispersion` | double | `0.30` | 0–1 | Chromatic spread of the rim refraction. |
| `highlight` | double | `0.80` | 0–1.5 | Specular rim/sheen strength. |
| `shadow` | double | `0.35` | 0–1 | Drop shadow strength. |
| `tint` | string | `"#FFFFFF"` | `#RRGGBB` | Glass tint colour. |
| `noise` | double | `0.015` | 0–0.1 | Optional fine grain (reserved; native may render 0 in v1). |
| `saturation` | double | `1.15` | 0.5–2 | Backdrop vibrancy/saturation boost. |
| `launchAtLogin` | bool | `false` | | Create/remove the per-user at-logon Task Scheduler task (HKCU Run fallback). |
| `menuAnimations` | bool | `true` | | Animate menu/panel opening. |

## Notes

- `trayShow`/`trayHide` are override lists over Explorer's `NotifyIconSettings`
  record IDs (the numeric subkey names, also used by `UIOrderList`). `trayHide`
  wins if an ID appears in both; otherwise the record's `IsPromoted` is used.
  An empty string means "follow Windows". The settings app writes sorted,
  semicolon-separated decimal IDs. Overrides only apply while the record passes
  the shell liveness check; the settings list itself reads the native
  `%LOCALAPPDATA%\SuperMenu\tray-live.txt` snapshot (fresh within 15 s) to show
  only currently registered icons.
- `logoStyle="windows-mono"` draws the Windows four-pane mark with the current
  backdrop ink (black on bright glass, white on dark) instead of `#0078D4`.
- `version` must remain `1` until a migration is written.
- The native app writes defaults on first run if the file does not exist; the settings app also writes defaults if missing.
- The settings app must not store UI-only state in this file.
- Do not add monitor geometry, window handles, or machine-specific state.

