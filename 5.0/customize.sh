#!/bin/bash
# 注意：这不是占位符！此代码将模块文件安装到系统并设置权限
SKIPUNZIP=0

print_modname() {
    ui_print "*******************************"
    ui_print "        Magisk Module          "
    ui_print "Make By 慕容茹艳（酷安慕容雪绒）"
    ui_print "*******************************"
}

# 目标目录
DIR="/data/adb/modules_update/muronggameopt/config"
DIY="/data/adb/modules/muronggameopt/config"
VOLUME_KEY_DEVICE="/dev/input/event2"
your_module_id="muronggameopt"

# 延迟输出函数
Outputs() {
    echo "$@"
    sleep 0.07
}

# 获取应用名称
get_app_name() {
    local package_name="$1"
    local app_name=$(dumpsys package "$package_name" | grep -i 'application-label:' | cut -d':' -f2 | tr -d '[:space:]')
    if [ -z "$app_name" ]; then
        app_name="$package_name"
    fi
    echo "$app_name"
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

# 安装APK
install_apk() {
    apk_file="$1"
    if [ -f "$apk_file" ]; then
        pm install -r "$apk_file"
        echo "软件安装成功."
    else
        echo "Error: $apk_file 未找到!"
        exit 1
    fi
}

# 检查必要文件
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

# 移除系统性能配置
remove_sys_perf_config() {
    SYSPERFCONFIG="/system/vendor/bin/msm_irqbalance"
    if [ -f "$SYSPERFCONFIG" ]; then
        [[ ! -d $MODPATH${SYSPERFCONFIG%/*} ]] && mkdir -p $MODPATH${SYSPERFCONFIG%/*}
        ui_print "- 配置文件: $SYSPERFCONFIG"
        touch $MODPATH$SYSPERFCONFIG
        ui_print "**************************************************"
    fi
}

# 检测ColorOS/RealmeUI的oiface服务
check_oiface() {
    if pm list packages | grep -q 'com.oplus.cosa'; then
        ui_print "检测到ColorOS/RealmeUI系统，启用oiface"
        if [ -n "$(getprop persist.sys.oiface.enable)" ]; then
            setprop persist.sys.oiface.enable 2
            stop oiface >/dev/null 2>&1
        fi
    fi
}

# 检测ColorOS的应用增强服务包是否存在
check_cosa() {
    if pm list packages | grep -q 'com.oplus.cosa'; then
            # 禁用不必要的服务
            pm clear com.oplus.battery >/dev/null 2>&1
            pm clear com.oplus.cosa >/dev/null 2>&1
            pm disable com.oplus.cosa/com.oplus.cosa.gamemanagersdk.CosaAMTService > /dev/null 2>&1
            pm disable com.oplus.cosa/com.oplus.cosa.gamemanagersdk.HyperBoostService > /dev/null 2>&1
            pm enable com.oplus.cosa/com.oplus.cosa.gamemanagersdk.CosaGameSdkService > /dev/null 2>&1
            pm enable com.oplus.cosa/com.oplus.cosa.service.COSAService > /dev/null 2>&1
            pm disable com.oplus.cosa/com.oplus.cosa.service.GameEventService > /dev/null 2>&1
            pm enable com.oplus.cosa/com.oplus.cosa.service.GameDaemonService > /dev/null 2>&1
            pm disable com.oplus.cosa/com.oplus.cosa.gamemanagersdk.CosaHyperBoostService > /dev/null 2>&1
            pm enable com.oplus.cosa/com.oplus.cosa.testlibrary.service.COSATesterService > /dev/null 2>&1
            pm disable com.oplus.cosa/androidx.work.impl.background.systemjob.SystemJobService > /dev/null 2>&1
            pm enable com.oplus.cosa/com.oplus.cosa.feature.ScreenPerceptionService > /dev/null 2>&1
            pm enable com.oplus.cosa/com.oplus.cosa.gpalibrary.service.GPAService > /dev/null 2>&1
            pm enable com.oplus.cosa/androidx.work.impl.background.systemalarm.SystemAlarmService > /dev/null 2>&1
            pm enable com.oplus.cosa/androidx.work.impl.foreground.SystemForegroundService > /dev/null 2>&1
            pm enable com.oplus.cosa/androidx.room.MultiInstanceInvalidationService > /dev/null 2>&1
    fi
}

# 检测MIUI的Joyose服务包是否存在
check_miui() {
    if pm list packages | grep -q 'com.xiaomi.joyose'; then
        ui_print "检测到hyperOS/Miui，禁用joyose"
        pm disable-user com.xiaomi.joyose >/dev/null 2>&1
        pm clear com.xiaomi.joyose >/dev/null 2>&1
    fi
}

# 检测vivo游戏盒子进程
check_vivo() {
    if pgrep -f 'com.vivo.gamewatch' >/dev/null; then
        ui_print "检测到OriginOS/FuntouchOS，禁用gamewatch"
        pm disable-user com.vivo.gamewatch >/dev/null 2>&1
        pm clear com.vivo.gamewatch >/dev/null 2>&1
    fi
}

set_prop() {
    resetprop "$1" "$2"
}

Vulkan() {
# Vulkan核心
set_prop persist.graphics.vulkan.disable 0
set_prop debug.hwui.vulkan_safe_mode 1
set_prop persist.vulkan.fallback 0
set_prop ro.hwui.use_vulkan true
set_prop debug.hwui.target_cpu_time_percent 70
set_prop renderthread.skia.reduceopstasksplitting true
set_prop debug.hwui.disable_buffer_age true
set_prop ro.hwui.texture_cache_size 72
set_prop ro.hwui.layer_cache_size 36
set_prop persist.sys.ui.thread.boost true
set_prop debug.egl.force_fxaa 1

find /data/user_de -name '*shaders_cache*' -delete 2>/dev/null
find /data/vulkan -name '*.cache' -delete 2>/dev/null
rm -rf /data/vulkan_cache/* 2>/dev/null
}

# 主安装逻辑
main() {
    # 确保目标目录存在
    mkdir -p "$DIR"
    
    echo "powersave
com.tencent.tmgp.sgame balance
com.miHoYo.hkrpg balance
com.netease.l22 balance
com.miHoYo.Yuanshen balance
com.tencent.lolm balance
com.tencent.tmgp.cod performance
com.tencent.tmgp.pubgmhd performance
com.netease.yyslscn fast
com.tencent.tmgp.dfm fast
com.tencent.tmgp.cf fast" > "$DIR/mode.txt"

}

# ============= 主执行流程 =============
print_modname

# 1. 检查必要文件
check_required_files

# 3. 移除系统性能配置
remove_sys_perf_config

# 4. 设置基本权限
set_perm_recursive "$MODPATH" 0 0 0755 0644
set_perm "$MODPATH/AppOpt" 0 2000 0755
set_perm "$MODPATH/*.sh" 0 2000 0755

# 5. 初始化其他组件
sh $MODPATH/vtools/init_vtools.sh $(realpath $MODPATH/module.prop)

/system/bin/sh $MODPATH/CPUcluster.sh

# 6. 主安装逻辑
main

Vulkan
ui_print "启用Vulkan渲染"

# 7. 禁用系统服务
ui_print "正在优化系统服务..."
check_oiface
check_cosa
check_miui
check_vivo

# 8. 最终提示
ui_print "- 内置线程改为SutoLiu的线程优化模块"
ui_print "- 感谢SutoLiu的开源"
ui_print "- 为了让小白可以直接使用，后续模块将自动禁用官方线程"
ui_print "**************************************************"
ui_print "安装完成！请重启设备使调度生效"
ui_print "**************************************************"