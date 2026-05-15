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
}

print_modname
check_required_files
remove_sys_perf_config

main

# 修复权限设置
set_perm_recursive "$MODPATH" 0 0 0755 0644
set_perm "$MODPATH/bin/activity_diaodu" 0 2000 0755

ui_print "生成scene控制文件"
sh "$MODPATH/vtools/init_vtools.sh" "$(realpath $MODPATH/module.prop)"
