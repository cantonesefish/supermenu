# HarmonyOS Sans SC

SuperMenu 的界面字体。这里是从系统安装的 **HarmonyOS Sans SC** 复制出的四个字重，
未经修改，随应用私有加载，不安装到系统：

| 文件 | OS/2 usWeightClass | 用途 |
| --- | ---: | --- |
| `HarmonyOS_Sans_SC_Light.ttf` | 300 | 轻量展示文字 |
| `HarmonyOS_Sans_SC_Regular.ttf` | 400 | 正文、菜单、日期 |
| `HarmonyOS_Sans_SC_Medium.ttf` | 500 | 卡片标题、强调、次级标题 |
| `HarmonyOS_Sans_SC_Bold.ttf` | 700 | 页面标题、应用名、强调数字 |

- 版权：Copyright 2021 Huawei Device Co., Ltd.，字体设计：Hanyi Fonts。
- 许可：HarmonyOS Sans Fonts License Agreement；完整协议见
  `LICENSE-HarmonyOS-Sans.txt`。协议允许将**未修改**的字体嵌入/随软件分发，要求
  在软件中显著注明使用了 HarmonyOS Sans Fonts，并保留版权与协议。
- 旧的四字重 MiSans 已从应用字体加载路径移除；本目录只保留 HarmonyOS Sans。
- 图标字体仍为 `SegoeIcons.ttf`（Segoe Fluent Icons），不与正文字体混用。
- 原生 DirectWrite 使用 `IDWriteFactory5` 私有集合；设置中心使用 WPF
  `Fonts.GetFontFamilies` 从 `Assets/Fonts` 私有加载。
