#!/usr/bin/env bash
# LXQt 系统监视面板插件 —— 一键安装
# 用法：./install.sh [--no-restart]
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LIB_DIR="${LXQTPANEL_PLUGIN_DIR:-$HOME/.local/lib/lxqt-panel}"
DATA_HOME="${XDG_DATA_HOME:-$HOME/.local/share}"
PLUGIN_DESKTOP_DIR="$DATA_HOME/lxqt/lxqt-panel"
CONFIG_HOME="${XDG_CONFIG_HOME:-$HOME/.config}"
SESSION_CONF="$CONFIG_HOME/lxqt/session.conf"
PANEL_CONF="$CONFIG_HOME/lxqt/panel.conf"
AUTOSTART_FILE="$CONFIG_HOME/autostart/lxqt-panel.desktop"

RESTART=1
for arg in "$@"; do
    case "$arg" in
        --no-restart) RESTART=0 ;;
        -h|--help)
            sed -n '2,4p' "$0" | sed 's/^# \{0,1\}//'
            echo
            echo "选项："
            echo "  --no-restart   不改动正在运行的面板，装完后手动重启面板"
            echo
            echo "环境变量："
            echo "  LXQTPANEL_PLUGIN_DIR   插件库目录（默认 ~/.local/lib/lxqt-panel）"
            exit 0
            ;;
        *) echo "未知参数：$arg（用 --help 查看用法）" >&2; exit 2 ;;
    esac
done

say()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
ok()   { printf '\033[1;32m  ✓\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m  !\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31m错误：\033[0m %s\n' "$*" >&2; exit 1; }

# ---------- 依赖检查 ----------
say "检查编译依赖"
QMAKE="$(command -v qmake || command -v qmake-qt5 || true)"
[ -n "$QMAKE" ] || die "找不到 qmake。Ubuntu/Debian: sudo apt install qtbase5-dev；Arch: sudo pacman -S qt5-base"
command -v make >/dev/null || die "找不到 make。Ubuntu/Debian: sudo apt install build-essential"
command -v g++  >/dev/null || die "找不到 g++。Ubuntu/Debian: sudo apt install build-essential"

HEADER=""
for h in /usr/include/lxqt/ilxqtpanelplugin.h /usr/include/lxqt-panel/ilxqtpanelplugin.h; do
    [ -f "$h" ] && { HEADER="$h"; break; }
done
[ -n "$HEADER" ] || die "找不到 ilxqtpanelplugin.h（LXQt 面板开发头文件）。
Ubuntu/Debian: sudo apt install lxqt-panel
Arch:          sudo pacman -S lxqt-panel"
ok "qmake: $QMAKE"
ok "头文件: $HEADER"

# ---------- 编译 ----------
say "编译插件"
mkdir -p "$REPO_DIR/build"
( cd "$REPO_DIR/build" && "$QMAKE" .. >/dev/null && make -j"$(nproc 2>/dev/null || echo 2)" )
[ -f "$REPO_DIR/build/libsysmon.so" ] || die "编译失败，没有生成 build/libsysmon.so"
ok "build/libsysmon.so"

# ---------- 安装文件 ----------
say "安装文件"
install -Dm755 "$REPO_DIR/build/libsysmon.so" "$LIB_DIR/libsysmon.so"
install -Dm644 "$REPO_DIR/sysmon.desktop"     "$PLUGIN_DESKTOP_DIR/sysmon.desktop"
ok "$LIB_DIR/libsysmon.so"
ok "$PLUGIN_DESKTOP_DIR/sysmon.desktop"

# ---------- 改配置 ----------
backup() {
    [ -f "$1" ] || return 0
    cp -a "$1" "$1.bak-sysmon"
    ok "备份 ${1/#$HOME/~} → ${1/#$HOME/~}.bak-sysmon"
    return 0
}

WAS_RUNNING=0
if pgrep -x lxqt-panel >/dev/null 2>&1; then
    WAS_RUNNING=1
    if [ "$RESTART" -eq 1 ]; then
        say "暂停面板（避免旧进程覆盖配置）"
        pkill -x lxqt-panel 2>/dev/null || true
        for _ in $(seq 1 30); do
            pgrep -x lxqt-panel >/dev/null 2>&1 || break
            sleep 0.1
        done
        if pgrep -x lxqt-panel >/dev/null 2>&1; then
            pkill -9 -x lxqt-panel 2>/dev/null || true
            sleep 0.3
        fi
        ok "面板已停止"
    else
        warn "面板正在运行；--no-restart 模式下没有停它，装完请手动重启面板"
    fi
fi

say "写入配置"
backup "$SESSION_CONF"
backup "$PANEL_CONF"
backup "$AUTOSTART_FILE"

SYSMON_LIB_DIR="$LIB_DIR" \
SYSMON_SESSION_CONF="$SESSION_CONF" \
SYSMON_PANEL_CONF="$PANEL_CONF" \
SYSMON_AUTOSTART="$AUTOSTART_FILE" \
python3 - <<'PY'
import glob, os, re, sys

lib       = os.environ["SYSMON_LIB_DIR"]
sess_path = os.environ["SYSMON_SESSION_CONF"]
panel_path= os.environ["SYSMON_PANEL_CONF"]
auto_path = os.environ["SYSMON_AUTOSTART"]

def read_lines(path):
    if not os.path.exists(path):
        return []
    with open(path, encoding="utf-8") as f:
        return f.read().splitlines()

def write_lines(path, lines):
    # panel.conf 可能是只读的（0444），QSettings 本身用“临时文件 + rename”写入。
    # 这里照做，并保留原文件权限。
    directory = os.path.dirname(path) or "."
    os.makedirs(directory, exist_ok=True)
    tmp = path + ".tmp-sysmon"
    with open(tmp, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    if os.path.exists(path):
        try:
            os.chmod(tmp, os.stat(path).st_mode & 0o777)
        except OSError:
            pass
    os.replace(tmp, path)

def section_bounds(lines, name):
    start = None
    for i, line in enumerate(lines):
        if line.strip() == "[%s]" % name:
            start = i
            break
    if start is None:
        return None
    end = len(lines)
    for j in range(start + 1, len(lines)):
        if lines[j].lstrip().startswith("["):
            end = j
            break
    return start, end

# QSettings 风格的字符串：含空格/特殊字符时加引号
def ini_value(text):
    if re.search(r'[\s";=#]', text):
        return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'
    return text

# ---------- 1. session.conf：让面板启动时能找到用户目录里的插件 ----------
lines = read_lines(sess_path)
entry = "LXQTPANEL_PLUGIN_PATH=" + ini_value(lib)
bounds = section_bounds(lines, "Environment")
if bounds is None:
    if lines and lines[-1].strip():
        lines.append("")
    lines += ["[Environment]", entry]
else:
    start, end = bounds
    for i in range(start + 1, end):
        if lines[i].startswith("LXQTPANEL_PLUGIN_PATH="):
            lines[i] = entry
            break
    else:
        lines.insert(start + 1, entry)
write_lines(sess_path, lines)

# ---------- 2. 自启项：给 lxqt-panel 也注入同一个环境变量 ----------
execline = 'Exec=env LXQTPANEL_PLUGIN_PATH=%s lxqt-panel' % ini_value(lib)
lines = read_lines(auto_path)
if not lines:
    # 优先复制系统自启项，保留翻译名称等字段
    for src in ("/usr/share/applications/lxqt-panel.desktop",
                "/etc/xdg/autostart/lxqt-panel.desktop"):
        if os.path.exists(src):
            lines = read_lines(src)
            break
    else:
        lines = [
            "[Desktop Entry]",
            "Type=Application",
            "TryExec=lxqt-panel",
            "OnlyShowIn=LXQt;",
            "X-LXQt-Module=true",
            "Name=LXQt Panel",
            "Name[zh_CN]=LXQt 面板",
        ]
for i, line in enumerate(lines):
    if line.startswith("Exec="):
        lines[i] = execline
        break
else:
    lines.insert(1, execline)
write_lines(auto_path, lines)

# ---------- 3. panel.conf：把 sysmon 加进插件列表 ----------
DEFAULT_PLUGINS = ("mainmenu, desktopswitch, quicklaunch, showdesktop, taskbar, "
                   "sysmon, mount, volume, tray, statusnotifier, worldclock")
RIGHT_GROUP = {"mount", "volume", "tray", "statusnotifier", "worldclock", "sysmon"}

lines = read_lines(panel_path)
panel_idx = None
panel_name = None
for i, line in enumerate(lines):
    m = re.match(r'^\[(panel\d+)\]\s*$', line)
    if m:
        panel_idx, panel_name = i, m.group(1)
        break
if panel_idx is None:
    panel_name = "panel1"
    if lines and lines[-1].strip():
        lines.append("")
    lines.append("[%s]" % panel_name)
    panel_idx = len(lines) - 1

bounds = section_bounds(lines, panel_name)
start, end = bounds
plugins_line = None
for i in range(start + 1, end):
    if lines[i].startswith("plugins="):
        plugins_line = i
        break

def system_default_plugins(name):
    for path in ["/etc/xdg/lxqt/panel.conf"] + sorted(glob.glob("/etc/xdg/*/lxqt/panel.conf")):
        if not os.path.exists(path):
            continue
        bl = read_lines(path)
        b = section_bounds(bl, name)
        if not b:
            continue
        for line in bl[b[0] + 1:b[1]]:
            if line.startswith("plugins="):
                return line[len("plugins="):]
    return DEFAULT_PLUGINS

if plugins_line is None:
    value = system_default_plugins(panel_name)
else:
    value = lines[plugins_line][len("plugins="):]

tokens = [t.strip() for t in value.split(",") if t.strip()]
if "sysmon" not in tokens:
    if "taskbar" in tokens:
        pos = tokens.index("taskbar") + 1
    else:
        pos = next((i for i, t in enumerate(tokens) if t in RIGHT_GROUP), len(tokens))
    tokens.insert(pos, "sysmon")

new_line = "plugins=" + ", ".join(tokens)
if plugins_line is None:
    lines.insert(end, new_line)
else:
    lines[plugins_line] = new_line

if section_bounds(lines, "sysmon") is None:
    if lines and lines[-1].strip():
        lines.append("")
    lines += ["[sysmon]", "alignment=Right", "type=sysmon"]

write_lines(panel_path, lines)

print("  ✓ %s：plugins 已包含 sysmon" % panel_path)
PY
ok "配置写入完成"

# ---------- 重启面板 ----------
if [ "$WAS_RUNNING" -eq 1 ] && [ "$RESTART" -eq 1 ]; then
    say "启动面板"
    if [ -n "${DISPLAY:-}" ]; then
        setsid env LXQTPANEL_PLUGIN_PATH="$LIB_DIR" lxqt-panel >/tmp/lxqt-panel.log 2>&1 &
        sleep 1
        pgrep -x lxqt-panel >/dev/null 2>&1 && ok "面板已重启" || warn "面板没能启动，查看 /tmp/lxqt-panel.log"
    else
        warn "没有 DISPLAY，跳过启动；下次登录会自动加载"
    fi
fi

cat <<EOF

安装完成。

  插件库：  $LIB_DIR/libsysmon.so
  描述文件：$PLUGIN_DESKTOP_DIR/sysmon.desktop
  加载变量：LXQTPANEL_PLUGIN_PATH=$LIB_DIR

EOF

if [ "$RESTART" -eq 0 ]; then
    cat <<EOF
面板还在运行，手动重启后生效：

  pkill -x lxqt-panel; sleep 1
  setsid env LXQTPANEL_PLUGIN_PATH="$LIB_DIR" lxqt-panel >/tmp/lxqt-panel.log 2>&1 &

EOF
fi

echo "卸载：./uninstall.sh"
