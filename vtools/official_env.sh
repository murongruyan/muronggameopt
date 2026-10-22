#!/system/bin/sh
# 6.11 官方环境复刻
#
# 目的：把开发者验证环境里"官方能力被禁用"的状态，按配置复刻到用户设备，
# 让用户跑在与开发者一致的环境里（不是只检测/报告）。
# 只做下面几类可逆、可配置的官方增强项，不碰系统核心服务：
#   1) 禁用 OPLUS 官方增强/定制类包（默认 com.oplus.ohome、com.oplus.customize.coreapp）
#   2) OIFace：persist.sys.oiface.enable/control 置 0，并 stop oiface
#   3) game_opt：disable_cpufreq_limit=1（解除官方游戏调频限制）
#
# 配置：config/official_env.conf（enabled=0 可整体关闭）
# 日志：config/official_env.log

MODDIR=${0%/*}
CONF="$MODDIR/../config/official_env.conf"
LOG="$MODDIR/../config/official_env.log"
[ -f "$CONF" ] && . "$CONF" 2>/dev/null

mkdir -p "$(dirname "$LOG")" 2>/dev/null

log_line() {
    echo "$(date '+%Y-%m-%d %H:%M:%S') $*" >> "$LOG" 2>/dev/null
}

[ "${enabled:-1}" = "1" ] || { log_line "enabled=0，跳过官方环境复刻"; exit 0; }

log_line "开始官方环境复刻"

# ── 1) 禁用官方增强/定制包 ──────────────────────────────────
if [ -n "${disable_packages:-}" ]; then
    OLD_IFS=$IFS
    IFS=','
    for pkg in $disable_packages; do
        pkg=$(echo "$pkg" | tr -d ' ')
        [ -n "$pkg" ] || continue
        if ! pm list packages 2>/dev/null | grep -q "^package:${pkg}$"; then
            log_line "${pkg} 未安装，跳过"
            continue
        fi
        if pm list packages -d 2>/dev/null | grep -q "^package:${pkg}$"; then
            log_line "${pkg} 已处于禁用状态"
        else
            if pm disable-user --user 0 "$pkg" >/dev/null 2>&1; then
                log_line "${pkg} 已禁用"
            else
                log_line "${pkg} 禁用失败"
            fi
        fi
    done
    IFS=$OLD_IFS
fi

# ── 2) OIFace ──────────────────────────────────────────────
if [ "${oiface_disable:-1}" = "1" ]; then
    setprop persist.sys.oiface.enable 0 2>/dev/null
    setprop persist.sys.oiface.control 0 2>/dev/null
    stop oiface >/dev/null 2>&1
    log_line "OIFace: enable=0 control=0 svc=stop"
fi

# ── 3) game_opt：解除官方游戏调频限制 ───────────────────────
if [ "${game_opt_disable_limit:-1}" = "1" ] && [ -e /proc/game_opt/disable_cpufreq_limit ]; then
    if echo 1 > /proc/game_opt/disable_cpufreq_limit 2>/dev/null; then
        log_line "game_opt: disable_cpufreq_limit=1"
    else
        log_line "game_opt: disable_cpufreq_limit 写入失败"
    fi
fi
if [ -n "${game_opt_debug:-}" ] && [ -e /proc/game_opt/debug_enable ]; then
    echo "${game_opt_debug}" > /proc/game_opt/debug_enable 2>/dev/null \
        && log_line "game_opt: debug_enable=${game_opt_debug}"
fi

log_line "官方环境复刻完成"
