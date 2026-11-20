# HarmonyOS Sans SC

`Assets/Fonts` 包含设置中心私有加载的四个 HarmonyOS Sans SC 字重（300/400/500/700），
文件来自系统安装字体，未修改。`AppAssets` 使用 WPF `Fonts.GetFontFamilies` 从本目录
建立 `Font.Body` / `Font.Demi` / `Font.Semi` / `Font.Light` 资源；字重由 XAML 样式中的
`FontWeight` 选择（Body=Regular、Demi=Medium、Semi=Bold、Light=Light）。

- 版权：Copyright 2021 Huawei Device Co., Ltd. & Hanyi Fonts。
- 许可：HarmonyOS Sans Fonts License Agreement，见 `LICENSE-HarmonyOS-Sans.txt`。
- 图标仍使用 `SegoeIcons.ttf`（Segoe Fluent Icons）。
