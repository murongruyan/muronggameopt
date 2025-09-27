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
    if pgrep -f 'oiface' >/dev/null; then
        ui_print "检测到ColorOS/RealmeUI系统，禁用oiface"
        if [ -n "$(getprop persist.sys.oiface.enable)" ]; then
            setprop persist.sys.oiface.enable 0
            stop oiface >/dev/null 2>&1
        fi
    fi
}

# 检测ColorOS的应用增强服务包是否存在
check_cosa() {
    if pm list packages | grep -q 'com.oplus.cosa'; then
        sleep 2
        ui_print "**************************************************"
        ui_print "是否保留ColorOS插帧功能？"
        ui_print "   - 音量上键 = 保留插帧（不推荐）"
        ui_print "   - 音量下键 = 不保留插帧（推荐，功耗更低）"
        ui_print "**************************************************"
        local choice=$(Volume_key_monitoring)
        
        if [ "$choice" == "0" ]; then
            ui_print "已选择保留插帧功能"
            # 全开所有服务
            pm clear com.oplus.battery >/dev/null 2>&1
            pm clear com.oplus.cosa >/dev/null 2>&1
            pm enable com.oplus.cosa/com.oplus.cosa.gamemanagersdk.CosaAMTService > /dev/null 2>&1
            pm enable com.oplus.cosa/com.oplus.cosa.gamemanagersdk.HyperBoostService > /dev/null 2>&1
            pm enable com.oplus.cosa/com.oplus.cosa.gamemanagersdk.CosaGameSdkService > /dev/null 2>&1
            pm enable com.oplus.cosa/com.oplus.cosa.service.COSAService > /dev/null 2>&1
            pm enable com.oplus.cosa/com.oplus.cosa.service.GameEventService > /dev/null 2>&1
            pm enable com.oplus.cosa/com.oplus.cosa.service.GameDaemonService > /dev/null 2>&1
            pm enable com.oplus.cosa/com.oplus.cosa.gamemanagersdk.CosaHyperBoostService > /dev/null 2>&1
            pm enable com.oplus.cosa/com.oplus.cosa.testlibrary.service.COSATesterService > /dev/null 2>&1
            pm enable com.oplus.cosa/androidx.work.impl.background.systemjob.SystemJobService > /dev/null 2>&1
            pm enable com.oplus.cosa/com.oplus.cosa.feature.ScreenPerceptionService > /dev/null 2>&1
            pm enable com.oplus.cosa/com.oplus.cosa.gpalibrary.service.GPAService > /dev/null 2>&1
            pm enable com.oplus.cosa/androidx.work.impl.background.systemalarm.SystemAlarmService > /dev/null 2>&1
            pm enable com.oplus.cosa/androidx.work.impl.foreground.SystemForegroundService > /dev/null 2>&1
            pm enable com.oplus.cosa/androidx.room.MultiInstanceInvalidationService > /dev/null 2>&1
            # 写入标记文件
            mkdir -p $MODPATH/tmp
            echo "1" > $MODPATH/tmp/GameEventService_state
            echo "1" > $MODPATH/tmp/CosaAMTService_state
            echo "1" > $MODPATH/tmp/CosaHyperBoostService_state
            echo "1" > $MODPATH/tmp/HyperBoostService_state
        else
            ui_print "已选择不保留插帧功能"
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
            pm enable com.oplus.cosa/androidx.work.impl.background.systemjob.SystemJobService > /dev/null 2>&1
            pm enable com.oplus.cosa/com.oplus.cosa.feature.ScreenPerceptionService > /dev/null 2>&1
            pm enable com.oplus.cosa/com.oplus.cosa.gpalibrary.service.GPAService > /dev/null 2>&1
            pm enable com.oplus.cosa/androidx.work.impl.background.systemalarm.SystemAlarmService > /dev/null 2>&1
            pm enable com.oplus.cosa/androidx.work.impl.foreground.SystemForegroundService > /dev/null 2>&1
            pm enable com.oplus.cosa/androidx.room.MultiInstanceInvalidationService > /dev/null 2>&1
        fi
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

# 判断是否需要优化应用增强和电池
need_optimize_battery() {
    local module_path="/data/adb/modules/muronggameopt"
    
    # 检查模块目录是否存在
    if [ ! -d "$module_path" ]; then
        ui_print "首次安装，需要优化应用增强和电池"
        return 0 # 需要优化
    fi
    
    # 检查module.prop文件是否存在
    if [ ! -f "$module_path/module.prop" ]; then
        ui_print "模块配置文件不存在，需要优化应用增强和电池"
        return 0 # 需要优化
    fi
    
    # 获取模块名称和版本号
    module_name=$(grep 'name=' "$module_path/module.prop" | cut -d'=' -f2)
    module_version=$(grep 'version=' "$module_path/module.prop" | cut -d'=' -f2)
    
    # 检查模块名称
    if [ "$module_name" != "慕容调度" ]; then
        ui_print "旧模块名称不同，需要优化应用增强和电池"
        return 0 # 需要优化
    fi
    
    # 检查版本号是否大于4.5
    if awk -v ver="$module_version" -v min_ver="4.5" 'BEGIN {
        split(ver, v, /\./); 
        split(min_ver, mv, /\./);
        v_major = v[1] + 0; v_minor = v[2] + 0;
        mv_major = mv[1] + 0; mv_minor = mv[2] + 0;
        if (v_major > mv_major) exit 1;
        if (v_major == mv_major && v_minor > mv_minor) exit 1;
        exit 0;
    }'; then
        ui_print "旧模块版本 <= 4.5，需要优化应用增强和电池"
        return 0 # 需要优化
    else
        ui_print "旧模块版本 > 4.5，跳过优化应用增强和电池"
        return 1 # 不需要优化
    fi
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

# 7. 禁用系统服务
ui_print "正在优化系统服务..."
check_oiface
sleep 2

# 根据条件判断是否需要优化应用增强和电池
if need_optimize_battery; then
    ui_print "执行应用增强和电池优化..."
    check_cosa
else
    ui_print "跳过应用增强和电池优化"
fi

check_miui
check_vivo

# 8. 最终提示
ui_print "- 内置线程改为SutoLiu的线程优化模块"
ui_print "- 感谢SutoLiu的开源"
ui_print "- 为了让小白可以直接使用，后续模块将自动禁用官方线程"
ui_print "**************************************************"
ui_print "安装完成！请重启设备使调度生效"
ui_print "**************************************************"