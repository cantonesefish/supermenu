#pragma once

// Segoe Fluent Icons codepoints used by the native UI. The font itself is
// shipped in Fonts\SegoeIcons.ttf and loaded as a private DirectWrite
// collection; these are the public Unicode PUA values from the icon list:
// https://learn.microsoft.com/windows/apps/design/iconography/segoe-fluent-icons-font
namespace sm::fluent {

constexpr wchar_t Network = 0xE968;
constexpr wchar_t Wifi = 0xE701;
constexpr wchar_t Bluetooth = 0xE702;
constexpr wchar_t Brightness = 0xE706;
constexpr wchar_t Wifi1 = 0xE872;
constexpr wchar_t Wifi2 = 0xE873;
constexpr wchar_t Wifi3 = 0xE874;
constexpr wchar_t LowerBrightness = 0xEC8A;
constexpr wchar_t Focus = 0xE708;
constexpr wchar_t FocusSolid = 0xF1DB;
constexpr wchar_t ChevronDown = 0xE70D;
constexpr wchar_t Search = 0xE721;
constexpr wchar_t Volume = 0xE767;
constexpr wchar_t Play = 0xE768;
constexpr wchar_t Pause = 0xE769;
constexpr wchar_t ChevronLeft = 0xE76B;
constexpr wchar_t ChevronRight = 0xE76C;
constexpr wchar_t Display = 0xE7F4;
constexpr wchar_t Calendar = 0xE787;
constexpr wchar_t WindowsUpdate = 0xF0C5;
constexpr wchar_t KeyboardStandard = 0xE92E;
constexpr wchar_t Check = 0xE73E;
constexpr wchar_t Ethernet = 0xE839;
constexpr wchar_t Previous = 0xE892;
constexpr wchar_t Next = 0xE893;
constexpr wchar_t ControlCenter = 0xE9E9;
constexpr wchar_t Notification = 0xEA8F;
constexpr wchar_t EthernetAni = 0xEAA4;
constexpr wchar_t WifiAni1 = 0xEAA5;
constexpr wchar_t WifiAni2 = 0xEAA8;
constexpr wchar_t MusicNote = 0xEC4F;
constexpr wchar_t Mute = 0xE74F;
constexpr wchar_t Volume0 = 0xE992;
constexpr wchar_t Volume1 = 0xE993;
constexpr wchar_t Volume2 = 0xE994;
constexpr wchar_t Volume3 = 0xE995;

}  // namespace sm::fluent
