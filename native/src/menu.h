#pragma once

#include "common.h"
#include "settings.h"
#include "system_status.h"

#include <string>
#include <vector>

namespace sm {

enum class MenuId {
    None,
    Apple,
    AppMenu,
    File,
    Edit,
    View,
    Window,
    Help,
    Wifi,
    Battery,
    Volume,
    ControlCenter,
    Clock,
    Search,
    Notifications,
    WindowsUpdate,
    InputMethod,
    Privacy,
    Context,
};

enum Command : int {
    CmdNone = 0,
    CmdAboutPC = 100,
    CmdSystemSettings,
    CmdAppStore,
    CmdForceQuit,
    CmdSleep,
    CmdRestart,
    CmdShutDown,
    CmdLock,
    CmdLogOut,
    CmdSuperMenuSettings,
    CmdSuperMenuFolder,
    CmdHelp,
    CmdAboutSuperMenu,
    CmdQuitSuperMenu,
    CmdReloadBar,

    CmdAboutApp = 200,
    CmdAppSettings,
    CmdHideApp,
    CmdHideOthers,
    CmdShowAll,
    CmdQuitApp,

    CmdCloseWindow = 300,
    CmdMinimizeWindow,
    CmdZoomWindow,
    CmdWindowLeft,
    CmdWindowRight,
    CmdFullScreen,
    CmdCenterWindow,

    CmdUndo = 400,
    CmdRedo,
    CmdCut,
    CmdCopy,
    CmdPaste,
    CmdSelectAll,

    CmdWifiSettings = 500,
    CmdBluetoothSettings,
    CmdBatterySettings,
    CmdSoundSettings,
    CmdNetworkSettings,
    CmdToggleTheme,
    CmdMediaPlayPause,
    CmdMediaNext,
    CmdMediaPrev,
    CmdToggleReserveWorkArea,
    CmdOpenNotificationSettings,
    CmdOpenDisplaySettings,
    CmdToggleWifi,
    CmdToggleBluetooth,
    CmdToggleFocus,
    CmdOpenSystemCalendar,
};

struct MenuItem {
    Command command = CmdNone;
    std::wstring label;
    std::wstring shortcut;
    bool separator = false;
    bool enabled = true;
    bool checked = false;
};

struct MenuModel {
    MenuId id = MenuId::None;
    std::wstring title;
    std::vector<MenuItem> items;
};

struct MenuContext {
    Settings settings;
    SystemStatus status;
    std::wstring appName;
};

std::wstring Tr(const Settings& s, const wchar_t* zh, const wchar_t* en);
std::wstring LocalizedAppName(const MenuContext& ctx);

MenuModel BuildMenu(MenuId id, const MenuContext& ctx, bool reserveWorkArea);
MenuModel BuildBarContextMenu(const MenuContext& ctx);
std::vector<MenuModel> BuildTopMenus(const MenuContext& ctx, bool reserveWorkArea);

}  // namespace sm
