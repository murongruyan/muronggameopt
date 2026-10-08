#!/system/bin/sh
SKIPUNZIP=0

print_modname() {
    ui_print "*******************************"
    ui_print "        Magisk Module          "
    ui_print "Make By 慕容茹艳（酷安慕容雪绒）"
    ui_print "*******************************"
}

VOLUME_KEY_DEVICE="/dev/input/event2"

# 延迟输出函数
Outputs() {
    echo "$@"
    sleep 0.07
}

# 监听音量键
Volume_key_monitoring() {
    local choose
    while :; do
        choose="$(getevent -qlc 1 | awk '{ print $3 }')"
        case "$choose" in
            KEY_VOLUMEUP) echo "0" && break ;;
            KEY_VOLUMEDOWN) echo "1" && break ;;
        esac
    done
}

CONFIG_DIR="$MODPATH/config"
MODULE_ID="muronggameopt"
OLD_MODULE_DIR="/data/adb/modules/$MODULE_ID"

check_required_files() {
    REQUIRED_FILE_LIST="/sys/devices/system/cpu/present"
    for REQUIRED_FILE in $REQUIRED_FILE_LIST; do
        if [ ! -e $REQUIRED_FILE ]; then
            ui_print "**************************************************"
            ui_print "! $REQUIRED_FILE 文件不存在"
            ui_print "! 请联系模块作者"
            abort "**************************************************"
        fi
    done
}

remove_sys_perf_config() {
    SYSPERFCONFIG="/system/vendor/bin/msm_irqbalance"
    if [ -f "$SYSPERFCONFIG" ]; then
        local target_dir="${SYSPERFCONFIG%/*}"
        [[ ! -d $MODPATH$target_dir ]] && mkdir -p $MODPATH$target_dir
        ui_print "- 屏蔽配置文件: $SYSPERFCONFIG"
        touch $MODPATH$SYSPERFCONFIG
    fi
}

main() {
    mkdir -p "$CONFIG_DIR"
    ui_print "创建配置文件: $CONFIG_DIR/mode.txt"
    echo "powersave
com.tencent.tmgp.sgame balance
com.miHoYo.hkrpg balance
com.netease.l22 balance
com.miHoYo.Yuanshen balance
com.tencent.lolm balance
com.tencent.tmgp.cod balance
com.tencent.tmgp.pubgmhd balance
com.tencent.tmgp.codev balance
com.netease.yyslscn balance
com.tencent.tmgp.dfm balance
com.tencent.tmgp.cf balance" > "$CONFIG_DIR/mode.txt"

    # FAS 频率按本机档位生成（gen_fas.sh）：
    #   1. 目标 fas.json 已存在 -> 不覆盖（用户会在软件里手改 fas.json）
    #   2. 同名模块上一次安装留下的 fas.json -> 原样继承
    #   3. 才用 bin/cpu/fas.default.json 模板 + 本机真实档位表生成
    #      频率一律吸附到 scaling_available/boost_frequencies 的并集，
    #      读不到档位表时留空，绝不写入其它 SoC 的硬编码频率
    if [ -f "$MODPATH/bin/cpu/gen_fas.sh" ]; then
        ui_print "- 按本机 CPU 档位生成 FAS 频率配置"
        sh "$MODPATH/bin/cpu/gen_fas.sh" "$MODPATH"
    fi
}

print_modname
check_required_files
remove_sys_perf_config

main

# 修复权限设置
set_perm_recursive "$MODPATH" 0 0 0755 0644
[ -f "$MODPATH/bin/activity_diaodu" ] && set_perm "$MODPATH/bin/activity_diaodu" 0 2000 0755
set_perm "$MODPATH/service.sh" 0 0 0755
[ -f "$MODPATH/activity_diaodu.rc" ] && set_perm "$MODPATH/activity_diaodu.rc" 0 0 0755
[ -f "$MODPATH/hmbird_restore_once.sh" ] && set_perm "$MODPATH/hmbird_restore_once.sh" 0 0 0755
[ -f "$MODPATH/fengchi_cleanup.sh" ] && set_perm "$MODPATH/fengchi_cleanup.sh" 0 0 0755
set_perm "$MODPATH/vtools/init_vtools.sh" 0 0 0755
set_perm "$MODPATH/vtools/powercfg.sh" 0 0 0755
[ -f "$MODPATH/bin/sqlite3" ] && set_perm "$MODPATH/bin/sqlite3" 0 0 0755

ui_print "生成scene控制文件"
sh "$MODPATH/vtools/init_vtools.sh" "$(realpath $MODPATH/module.prop)"
