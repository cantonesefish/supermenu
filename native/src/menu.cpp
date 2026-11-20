#include "menu.h"

#include <algorithm>

namespace sm {

std::wstring Tr(const Settings& s, const wchar_t* zh, const wchar_t* en) {
    return s.language == L"en-US" ? en : zh;
}

std::wstring LocalizedAppName(const MenuContext& ctx) {
    if (ctx.status.app.desktop) return Tr(ctx.settings, L"访达", L"Finder");
    return ctx.status.app.name.empty() ? Tr(ctx.settings, L"超级菜单栏", L"SuperMenu") : ctx.status.app.name;
}

namespace {
MenuItem Sep() {
    MenuItem i;
    i.separator = true;
    return i;
}
MenuItem Item(Command c, std::wstring label, std::wstring shortcut = L"", bool enabled = true, bool checked = false) {
    MenuItem i;
    i.command = c;
    i.label = std::move(label);
    i.shortcut = std::move(shortcut);
    i.enabled = enabled;
    i.checked = checked;
    return i;
}
}  // namespace

MenuModel BuildBarContextMenu(const MenuContext& ctx) {
    const Settings& s = ctx.settings;
    MenuModel m;
    m.id = MenuId::Context;
    m.items = {
        Item(CmdSuperMenuSettings, Tr(s, L"打开 SuperMenu 设置…", L"Open SuperMenu Settings…")),
        Item(CmdSuperMenuFolder, Tr(s, L"打开配置文件夹", L"Open Settings Folder")),
        Item(CmdAboutSuperMenu, Tr(s, L"关于 SuperMenu", L"About SuperMenu")),
        Sep(),
        Item(CmdReloadBar, Tr(s, L"重新载入顶栏", L"Reload Menu Bar")),
        Sep(),
        Item(CmdQuitSuperMenu, Tr(s, L"退出 SuperMenu", L"Quit SuperMenu")),
    };
    return m;
}

MenuModel BuildMenu(MenuId id, const MenuContext& ctx, bool reserveWorkArea) {
    const Settings& s = ctx.settings;
    const std::wstring app = LocalizedAppName(ctx);
    MenuModel m;
    m.id = id;
    m.title = app;
    switch (id) {
        case MenuId::Apple:
            m.title.clear();
            m.items = {
                Item(CmdAboutPC, Tr(s, L"关于本机", L"About This Mac")),
                Item(CmdSystemSettings, Tr(s, L"系统设置…", L"System Settings…")),
                Item(CmdAppStore, Tr(s, L"App Store…", L"App Store…")),
                Sep(),
                Item(CmdForceQuit, Tr(s, L"强制退出…", L"Force Quit…"), L"⌥⌘⎋"),
                Sep(),
                Item(CmdSleep, Tr(s, L"睡眠", L"Sleep")),
                Item(CmdRestart, Tr(s, L"重新启动…", L"Restart…")),
                Item(CmdShutDown, Tr(s, L"关机…", L"Shut Down…")),
                Sep(),
                Item(CmdLock, Tr(s, L"锁定屏幕", L"Lock Screen"), L"⌃⌘Q"),
                Item(CmdLogOut, Tr(s, L"退出登录…", L"Log Out…"), L"⇧⌘Q"),
                Sep(),
                Item(CmdSuperMenuSettings, Tr(s, L"SuperMenu 设置…", L"SuperMenu Settings…"), L"⌘,"),
                Item(CmdSuperMenuFolder, Tr(s, L"打开配置文件夹", L"Open Settings Folder")),
                Item(CmdHelp, Tr(s, L"超级菜单栏帮助", L"SuperMenu Help")),
                Sep(),
                Item(CmdQuitSuperMenu, Tr(s, L"退出 SuperMenu", L"Quit SuperMenu"), L"⌘Q"),
            };
            break;
        case MenuId::AppMenu:
            m.items = {
                Item(CmdAboutApp, Tr(s, L"关于 ", L"About ") + app),
                Sep(),
                Item(CmdAppSettings, Tr(s, L"设置…", L"Settings…"), L"⌘,"),
                Sep(),
                Item(CmdHideApp, Tr(s, L"隐藏 ", L"Hide ") + app, L"⌘H"),
                Item(CmdHideOthers, Tr(s, L"隐藏其他", L"Hide Others"), L"⌥⌘H"),
                Item(CmdShowAll, Tr(s, L"全部显示", L"Show All")),
                Sep(),
                Item(CmdQuitApp, Tr(s, L"退出 ", L"Quit ") + app, L"⌘Q"),
            };
            break;
        case MenuId::File:
            m.title = Tr(s, L"文件", L"File");
            m.items = {
                Item(CmdCloseWindow, Tr(s, L"关闭窗口", L"Close Window"), L"⌘W"),
                Item(CmdMinimizeWindow, Tr(s, L"最小化", L"Minimize"), L"⌘M"),
                Item(CmdZoomWindow, Tr(s, L"缩放", L"Zoom")),
                Sep(),
                Item(CmdWindowLeft, Tr(s, L"窗口移到左半屏", L"Move Window to Left")),
                Item(CmdWindowRight, Tr(s, L"窗口移到右半屏", L"Move Window to Right")),
                Sep(),
                Item(CmdSuperMenuSettings, Tr(s, L"SuperMenu 设置…", L"SuperMenu Settings…"), L"⌘,"),
            };
            break;
        case MenuId::Edit:
            m.title = Tr(s, L"编辑", L"Edit");
            m.items = {
                Item(CmdUndo, Tr(s, L"撤销", L"Undo"), L"⌘Z"),
                Item(CmdRedo, Tr(s, L"重做", L"Redo"), L"⇧⌘Z"),
                Sep(),
                Item(CmdCut, Tr(s, L"剪切", L"Cut"), L"⌘X"),
                Item(CmdCopy, Tr(s, L"拷贝", L"Copy"), L"⌘C"),
                Item(CmdPaste, Tr(s, L"粘贴", L"Paste"), L"⌘V"),
                Item(CmdSelectAll, Tr(s, L"全选", L"Select All"), L"⌘A"),
            };
            break;
        case MenuId::View:
            m.title = Tr(s, L"显示", L"View");
            m.items = {
                Item(CmdFullScreen, Tr(s, L"进入全屏幕", L"Enter Full Screen"), L"F11"),
                Sep(),
                Item(CmdCloseWindow, Tr(s, L"关闭当前窗口", L"Close Current Window")),
                Item(CmdCenterWindow, Tr(s, L"居中窗口", L"Center Window"), L"", false),
            };
            break;
        case MenuId::Window:
            m.title = Tr(s, L"窗口", L"Window");
            m.items = {
                Item(CmdMinimizeWindow, Tr(s, L"最小化", L"Minimize"), L"⌘M"),
                Item(CmdZoomWindow, Tr(s, L"缩放", L"Zoom")),
                Sep(),
                Item(CmdWindowLeft, Tr(s, L"移动到左半屏", L"Tile to Left Half")),
                Item(CmdWindowRight, Tr(s, L"移动到右半屏", L"Tile to Right Half")),
                Sep(),
                Item(CmdCloseWindow, Tr(s, L"关闭", L"Close")),
            };
            break;
        case MenuId::Help:
            m.title = Tr(s, L"帮助", L"Help");
            m.items = {
                Item(CmdHelp, Tr(s, L"SuperMenu 帮助", L"SuperMenu Help")),
                Sep(),
                Item(CmdAboutSuperMenu, Tr(s, L"关于 SuperMenu", L"About SuperMenu")),
                Item(CmdSuperMenuFolder, Tr(s, L"打开配置文件夹", L"Open Settings Folder")),
            };
            break;
        default:
            break;
    }
    (void)reserveWorkArea;
    return m;
}

std::vector<MenuModel> BuildTopMenus(const MenuContext& ctx, bool reserveWorkArea) {
    std::vector<MenuModel> menus;
    const Settings& s = ctx.settings;
    if (s.showAppleMenu) menus.push_back(BuildMenu(MenuId::Apple, ctx, reserveWorkArea));
    if (s.showAppName) menus.push_back(BuildMenu(MenuId::AppMenu, ctx, reserveWorkArea));
    menus.push_back(BuildMenu(MenuId::File, ctx, reserveWorkArea));
    menus.push_back(BuildMenu(MenuId::Edit, ctx, reserveWorkArea));
    menus.push_back(BuildMenu(MenuId::View, ctx, reserveWorkArea));
    menus.push_back(BuildMenu(MenuId::Window, ctx, reserveWorkArea));
    menus.push_back(BuildMenu(MenuId::Help, ctx, reserveWorkArea));
    return menus;
}

}  // namespace sm
