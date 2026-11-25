# 系统监视面板小组件（LXQt 原生插件）

在 LXQt 面板上用正常字体显示 CPU 使用率、内存占用、实时网速、整机功耗和 CPU 温度，
替代原来 22×22 像素的数字托盘图标（`C98 M82 22K` 那种不直观的写法）。

显示效果：

```
CPU 26% │ 内存 74% │ ↓ 11 KB/s ↑ 1.6 KB/s │ 14 W │ 56°C
```

鼠标悬停显示详情（内存已用/总量、功耗拆分、NVMe/WiFi 温度等）。数值每秒刷新。

## 文件位置

| 用途 | 路径 |
| --- | --- |
| 源码 | `~/.local/src/lxqt-sysmon/` |
| 插件库 | `~/.local/lib/lxqt-panel/libsysmon.so` |
| 插件描述 | `~/.local/share/lxqt/lxqt-panel/sysmon.desktop` |
| 面板配置 | `~/.config/lxqt/panel.conf`（`[panel1] plugins=...` 与 `[sysmon]`） |
| 加载路径 | `~/.config/lxqt/session.conf` 的 `[Environment]` 与 `~/.config/autostart/lxqt-panel.desktop` |

LXQt 面板只从 `/usr/lib/.../lxqt-panel/` 和 `$LXQTPANEL_PLUGIN_PATH` 加载插件，
没有 root 权限时用后者指向用户目录，所以需要环境变量 `LXQTPANEL_PLUGIN_PATH`。
两处都写了一份，任一生效即可。

## 重新编译安装

```bash
cd ~/.local/src/lxqt-sysmon
qmake sysmon.pro && make -j4
cp build/libsysmon.so ~/.local/lib/lxqt-panel/
pkill lxqt-panel; setsid env LXQTPANEL_PLUGIN_PATH=$HOME/.local/lib/lxqt-panel lxqt-panel >/dev/null 2>&1 &
```

## 可调参数（`sysmonplugin.cpp`）

- 阈值：CPU 70%/90%，内存 80%/92% 由普通色转为警告色、危险色。
- 网速格式：≥1 MB/s 显示 `x.x MB/s`，≥10 KB/s 显示整数 KB/s，其余保留一位小数。
- 宽度：`contentWidth()` 用最坏情况取值，保证每秒刷新时面板不抖动。

## 回滚

```bash
cp ~/.config/lxqt/panel.conf.bak-sysmon ~/.config/lxqt/panel.conf
mv ~/.config/autostart/lxqt-panel.desktop ~/.config/autostart/lxqt-panel.desktop.disabled
cp ~/.config/lxqt/session.conf.bak-sysmon ~/.config/lxqt/session.conf
mv ~/.config/autostart/power-temp-tray.desktop.disabled ~/.config/autostart/power-temp-tray.desktop
```

然后重新登录；旧的 `~/.local/bin/power-temp-tray.py` 仍保留，可随时恢复。
