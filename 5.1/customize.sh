#!/system/bin/sh
SKIPUNZIP=0

print_modname() {
    ui_print "*******************************"
    ui_print "        Magisk Module          "
    ui_print "Make By 慕容茹艳（酷安慕容雪绒）"
    ui_print "*******************************"
}

CONFIG_DIR="$MODPATH/config"

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

check_cosa() {
    if pm list packages | grep -q 'com.oplus.cosa'; then
        ui_print "正在优化应用增强和电池服务"
        
        am force-stop com.oplus.cosa >/dev/null 2>&1
        pm clear com.oplus.battery >/dev/null 2>&1
        pm clear com.oplus.cosa >/dev/null 2>&1
        
        # 启用组件
        pm enable com.oplus.cosa/com.oplus.cosa.gamemanagersdk.CosaGameSdkService >/dev/null 2>&1
        pm enable com.oplus.cosa/com.oplus.cosa.service.COSAService >/dev/null 2>&1
        pm enable com.oplus.cosa/com.oplus.cosa.service.GameDaemonService >/dev/null 2>&1
        pm enable com.oplus.cosa/com.oplus.cosa.feature.ScreenPerceptionService >/dev/null 2>&1
        pm enable com.oplus.cosa/com.oplus.cosa.gpalibrary.service.GPAService >/dev/null 2>&1
        pm enable com.oplus.cosa/androidx.room.MultiInstanceInvalidationService >/dev/null 2>&1
        pm enable com.oplus.cosa/androidx.work.impl.foreground.SystemForegroundService >/dev/null 2>&1
        pm enable com.oplus.cosa/androidx.work.impl.background.systemalarm.SystemAlarmService >/dev/null 2>&1
        pm enable com.oplus.cosa/com.oplus.cosa.testlibrary.service.COSATesterService >/dev/null 2>&1
        pm enable com.oplus.cosa/androidx.work.impl.background.systemjob.SystemJobService >/dev/null 2>&1
        # 禁用以下组件
        pm disable com.oplus.cosa/com.oplus.cosa.gamemanagersdk.CosaAMTService >/dev/null 2>&1
        pm disable com.oplus.cosa/com.oplus.cosa.gamemanagersdk.HyperBoostService >/dev/null 2>&1
        pm disable com.oplus.cosa/com.oplus.cosa.gamemanagersdk.CosaHyperBoostService >/dev/null 2>&1
        pm disable com.oplus.cosa/com.oplus.cosa.service.GameEventService >/dev/null 2>&1
         
        ui_print "正在启用oiface服务"
        setprop persist.sys.oiface.enable 2
        start oiface >/dev/null 2>&1
    else
        ui_print "- 未检测到com.oplus.cosa，如是绿厂机型，请恢复应用增强服务软件，卸载该模块并重新刷入"
    fi
}

check_miui() {
    if pm list packages | grep -q 'com.xiaomi.joyose'; then
        ui_print "检测到hyperOS/Miui，禁用joyose组件"
        am force-stop com.xiaomi.joyose >/dev/null 2>&1
        pm disable com.xiaomi.joyose/com.xiaomi.joyose.smartop.SmartOpService >/dev/null 2>&1
        pm disable com.xiaomi.joyose/com.xiaomi.joyose.JoyoseJobScheduleService >/dev/null 2>&1
        pm clear com.xiaomi.joyose >/dev/null 2>&1
    else
        ui_print "- 未检测到com.xiaomi.joyose，如是米系手机，请恢复joyose软件，卸载该模块并重新刷入"
    fi
}

check_vivo() {
    if pm list packages | grep -q 'com.vivo.gamewatch'; then
        ui_print "检测到OriginOS/FuntouchOS，禁用gamewatch"
        am force-stop com.vivo.gamewatch >/dev/null 2>&1
        pm disable com.vivo.gamewatch >/dev/null 2>&1
        pm clear com.vivo.gamewatch >/dev/null 2>&1
    else
        ui_print "- 未检测到com.vivo.gamewatch，如是蓝厂手机，请恢复gamewatch软件，卸载该模块重新刷入"
    fi
}

detect_and_generate_cpu_config() {
    ui_print "正在检测CPU型号..."
    
    # 执行CPU检测脚本
    CPU_RESULT=$(/system/bin/sh "$MODPATH/JSON/cpu.sh" 2>/dev/null)
    
    if [ -z "$CPU_RESULT" ]; then
        ui_print "! CPU检测失败，使用默认配置"
        return 1
    fi
    
    # 提取CPU型号（去掉"当前处理器型号:"前缀）
    CPU_MODEL=$(echo "$CPU_RESULT" | sed 's/当前处理器型号://')
    ui_print "检测到CPU型号: $CPU_MODEL"
    
    # 如果不是SM8650，则需要生成配置
    if [ "$CPU_MODEL" != "SM8650" ]; then
        ui_print "CPU型号不是SM8650，正在生成适配配置..."
        
        # 创建临时目录结构
        TEMP_MODULE_DIR="/data/adb/modules/muronggameopt"
        TEMP_CPU_DIR="$TEMP_MODULE_DIR/bin/cpu"
        
        # 确保目录存在
        mkdir -p "$TEMP_CPU_DIR" >/dev/null 2>&1
        
        # 复制SM8650.json作为参考配置到临时目录
        if [ -f "$MODPATH/bin/cpu/SM8650.json" ]; then
            cp "$MODPATH/bin/cpu/SM8650.json" "$TEMP_CPU_DIR/" >/dev/null 2>&1
            ui_print "已复制参考配置文件到临时目录"
        else
            ui_print "! 参考配置文件不存在: $MODPATH/bin/cpu/SM8650.json"
            return 1
        fi
        
        # 设置setup程序权限并执行
        if [ -f "$MODPATH/JSON/setup" ]; then
            chmod 755 "$MODPATH/JSON/setup" >/dev/null 2>&1
            
            ui_print "正在执行setup程序生成配置..."
            cd "$TEMP_MODULE_DIR/bin/cpu" || return 1
            "$MODPATH/JSON/setup" >/dev/null 2>&1
            
            # 检查是否生成了新的配置文件
            GENERATED_CONFIG="$TEMP_CPU_DIR/${CPU_MODEL}.json"
            if [ -f "$GENERATED_CONFIG" ]; then
                ui_print "配置生成成功: ${CPU_MODEL}.json"
                
                # 确保模块目录存在
                mkdir -p "$MODPATH/bin/cpu" >/dev/null 2>&1
                
                # 复制生成的配置回模块目录
                cp "$GENERATED_CONFIG" "$MODPATH/bin/cpu/" >/dev/null 2>&1
                ui_print "已复制配置文件到模块目录"
                
                # 清理临时目录
                rm -rf "$TEMP_MODULE_DIR" >/dev/null 2>&1
                ui_print "CPU配置生成完成"
            else
                ui_print "! 配置生成失败，将使用默认配置"
                # 清理临时目录
                rm -rf "$TEMP_MODULE_DIR" >/dev/null 2>&1
                return 1
            fi
        else
            ui_print "! setup程序不存在: $MODPATH/JSON/setup"
            # 清理临时目录
            rm -rf "$TEMP_MODULE_DIR" >/dev/null 2>&1
            return 1
        fi
    else
        ui_print "CPU型号为SM8650，使用内置配置"
    fi
    
    return 0
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
com.tencent.tmgp.cod performance
com.tencent.tmgp.pubgmhd performance
com.tencent.tmgp.codev performance
com.netease.yyslscn fast
com.tencent.tmgp.dfm fast
com.tencent.tmgp.cf fast" > "$CONFIG_DIR/mode.txt"
}

print_modname
check_required_files
remove_sys_perf_config

# 修复权限设置
set_perm_recursive "$MODPATH" 0 0 0755 0644
set_perm "$MODPATH/AppOpt" 0 2000 0755
set_perm "$MODPATH/bin/activity_diaodu" 0 2000 0755

ui_print "生成scene控制文件"
sh "$MODPATH/vtools/init_vtools.sh" "$(realpath $MODPATH/module.prop)"

ui_print "生成线程配置conf文件"
/system/bin/sh "$MODPATH/CPUcluster.sh"

# CPU检测和配置生成
detect_and_generate_cpu_config

main

ui_print "正在优化系统服务..."
check_cosa
check_miui
check_vivo

ui_print "**************************************************"
ui_print "- 内置线程改为SutoLiu的线程优化模块"
ui_print "- 感谢SutoLiu的开源"
ui_print "- 本模块自动禁用官方线程，无需额外模块和操作"
ui_print "**************************************************"
ui_print "安装完成！请重启设备使调度生效"
ui_print "**************************************************"