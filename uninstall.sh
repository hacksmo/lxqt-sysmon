#!/usr/bin/env bash
# LXQt 系统监视面板插件 —— 卸载（把 install.sh 做的改动撤销）
# 用法：./uninstall.sh [--no-restart]
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
            sed -n '2,3p' "$0" | sed 's/^# \{0,1\}//'
            echo
            echo "选项："
            echo "  --no-restart   不改动正在运行的面板"
            exit 0
            ;;
        *) echo "未知参数：$arg（用 --help 查看用法）" >&2; exit 2 ;;
    esac
done

say()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
ok()   { printf '\033[1;32m  ✓\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m  !\033[0m %s\n' "$*" >&2; }

WAS_RUNNING=0
if pgrep -x lxqt-panel >/dev/null 2>&1; then
    WAS_RUNNING=1
    if [ "$RESTART" -eq 1 ]; then
        say "暂停面板"
        pkill -x lxqt-panel 2>/dev/null || true
        for _ in $(seq 1 30); do
            pgrep -x lxqt-panel >/dev/null 2>&1 || break
            sleep 0.1
        done
        pgrep -x lxqt-panel >/dev/null 2>&1 && pkill -9 -x lxqt-panel 2>/dev/null || true
        sleep 0.3
        ok "面板已停止"
    else
        warn "面板正在运行；--no-restart 模式下没有停它"
    fi
fi

say "删除插件文件"
rm -f "$LIB_DIR/libsysmon.so" "$PLUGIN_DESKTOP_DIR/sysmon.desktop"
ok "$LIB_DIR/libsysmon.so"
ok "$PLUGIN_DESKTOP_DIR/sysmon.desktop"

backup() {
    [ -f "$1" ] || return 0
    cp -a "$1" "$1.bak-sysmon-uninstall"
    ok "备份 ${1/#$HOME/~} → ${1/#$HOME/~}.bak-sysmon-uninstall"
    return 0
}
backup "$SESSION_CONF"
backup "$PANEL_CONF"
backup "$AUTOSTART_FILE"

say "撤销配置改动"
SYSMON_LIB_DIR="$LIB_DIR" \
SYSMON_SESSION_CONF="$SESSION_CONF" \
SYSMON_PANEL_CONF="$PANEL_CONF" \
SYSMON_AUTOSTART="$AUTOSTART_FILE" \
python3 - <<'PY'
import os, re

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

def unquote(value):
    value = value.strip()
    if len(value) >= 2 and value.startswith('"') and value.endswith('"'):
        value = value[1:-1].replace('\\"', '"').replace("\\\\", "\\")
    return value

# ---------- session.conf ----------
lines = read_lines(sess_path)
bounds = section_bounds(lines, "Environment")
if bounds:
    start, end = bounds
    remove = []
    for i in range(start + 1, end):
        if lines[i].startswith("LXQTPANEL_PLUGIN_PATH="):
            parts = [unquote(p) for p in lines[i][len("LXQTPANEL_PLUGIN_PATH="):].split(":")]
            parts = [p for p in parts if p and os.path.normpath(p) != os.path.normpath(lib)]
            if parts:
                lines[i] = "LXQTPANEL_PLUGIN_PATH=" + ":".join(parts)
            else:
                remove.append(i)
    for i in reversed(remove):
        del lines[i]
    write_lines(sess_path, lines)

# ---------- 自启项 ----------
lines = read_lines(auto_path)
if lines:
    orig = list(lines)
    changed = False
    for i, line in enumerate(lines):
        if line.startswith("Exec=") and lib in line:
            lines[i] = "Exec=lxqt-panel"
            changed = True
    if changed:
        # 如果去掉前缀后和系统自带文件一致，就直接删掉覆盖文件，恢复系统默认
        system_same = False
        for src in ("/usr/share/applications/lxqt-panel.desktop",
                    "/etc/xdg/autostart/lxqt-panel.desktop"):
            if os.path.exists(src):
                with open(src, encoding="utf-8") as f:
                    if f.read().splitlines() == lines:
                        system_same = True
                        break
        if system_same:
            os.remove(auto_path)
        else:
            write_lines(auto_path, lines)

# ---------- panel.conf ----------
lines = read_lines(panel_path)
panel_idx = None
panel_name = None
for i, line in enumerate(lines):
    m = re.match(r'^\[(panel\d+)\]\s*$', line)
    if m:
        panel_idx, panel_name = i, m.group(1)
        break
if panel_idx is not None:
    bounds = section_bounds(lines, panel_name)
    start, end = bounds
    for i in range(start + 1, end):
        if lines[i].startswith("plugins="):
            tokens = [t.strip() for t in lines[i][len("plugins="):].split(",") if t.strip()]
            tokens = [t for t in tokens if t != "sysmon"]
            lines[i] = "plugins=" + ", ".join(tokens)
            break

bounds = section_bounds(lines, "sysmon")
if bounds:
    start, end = bounds
    # 连同前面的空行一起删掉
    if start > 0 and not lines[start - 1].strip():
        start -= 1
    del lines[start:end]

write_lines(panel_path, lines)
print("  ✓ 已从 panel.conf 移除 sysmon")
PY

if [ "$WAS_RUNNING" -eq 1 ] && [ "$RESTART" -eq 1 ] && [ -n "${DISPLAY:-}" ]; then
    say "启动面板"
    setsid lxqt-panel >/tmp/lxqt-panel.log 2>&1 &
    sleep 1
    pgrep -x lxqt-panel >/dev/null 2>&1 && ok "面板已重启" || warn "面板没能启动，查看 /tmp/lxqt-panel.log"
fi

cat <<EOF

卸载完成。

已删除：
  $LIB_DIR/libsysmon.so
  $PLUGIN_DESKTOP_DIR/sysmon.desktop

已撤销：
  session.conf 里的 LXQTPANEL_PLUGIN_PATH
  lxqt-panel 自启项里的 env 前缀
  panel.conf 插件列表里的 sysmon

配置备份后缀：.bak-sysmon-uninstall
EOF
