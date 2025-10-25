#!/system/bin/sh

# 根据C代码的extract_chip_model函数逻辑提取芯片型号
extract_chip_model_like_c() {
    local input="$1"
    [ -z "$input" ] && return 1
    
    # 转换为大写进行匹配（但保留原始大小写用于某些模式）
    local upper_input=$(echo "$input" | tr '[:lower:]' '[:upper:]')
    local original_input="$input"
    
    # 1. OnePlus特殊处理：优先检查OPSM或CPHSM模式
    if echo "$upper_input" | grep -qE 'OPSM|CPHSM'; then
        local sm_match=$(echo "$upper_input" | grep -oE 'SM[0-9]{4}' | head -1)
        [ -n "$sm_match" ] && {
            echo "$sm_match"
            return 0
        }
    fi
    
    # 2. Qualcomm检测：查找SM、SDM、MSM模式
    if echo "$upper_input" | grep -qE '^SM[0-9]|^SDM[0-9]|^MSM[0-9]'; then
        # 直接提取SM/SDM/MSM开头的型号
        local qualcomm_match=$(echo "$upper_input" | grep -oE '^(SM|SDM|MSM)[0-9]+' | head -1)
        [ -n "$qualcomm_match" ] && {
            echo "$qualcomm_match"
            return 0
        }
    fi
    
    # 3. MediaTek检测
    if echo "$upper_input" | grep -qE 'MT|DIMENSITY'; then
        # 处理Dimensity系列
        if echo "$upper_input" | grep -qi 'DIMENSITY'; then
            local dimensity_match=$(echo "$upper_input" | grep -oi 'DIMENSITY[[:space:]]*[0-9]+' | head -1)
            [ -n "$dimensity_match" ] && {
                local number=$(echo "$dimensity_match" | grep -oE '[0-9]+')
                echo "MT$number"
                return 0
            }
        fi
        
        # 处理MT开头的型号（包括大小写）
        if echo "$original_input" | grep -qiE '^mt[0-9]'; then
            # 提取mt6895z_a格式
            local mt_lower_match=$(echo "$original_input" | grep -oiE 'mt[0-9]+[a-z]*_?[a-z]?' | head -1)
            if [ -n "$mt_lower_match" ]; then
                # 转换为大写
                echo "$mt_lower_match" | tr '[:lower:]' '[:upper:]'
                return 0
            fi
        fi
        
        # 处理MT6895Z_A_TCZA -> MT6895Z_A
        if echo "$upper_input" | grep -qE 'MT[0-9]+[A-Z]+_[A-Z]_[A-Z]+'; then
            local mt_complex_match=$(echo "$upper_input" | grep -oE 'MT[0-9]+[A-Z]+_[A-Z]' | head -1)
            [ -n "$mt_complex_match" ] && {
                echo "$mt_complex_match"
                return 0
            }
        fi
        
        # 通用MT模式提取
        local mt_simple_match=$(echo "$upper_input" | grep -oE 'MT[0-9]+[A-Z]*' | head -1)
        [ -n "$mt_simple_match" ] && {
            echo "$mt_simple_match"
            return 0
        }
    fi
    
    # 4. Exynos检测
    if echo "$original_input" | grep -qi 'exynos'; then
        local exynos_match=$(echo "$original_input" | grep -oi 'exynos[0-9]+' | head -1)
        [ -n "$exynos_match" ] && {
            # 首字母大写
            echo "$exynos_match" | sed 's/^\(.\)/\U\1/'
            return 0
        }
    fi
    
    # 5. 通用数字型号检测（假设为Qualcomm）
    local generic_number=$(echo "$input" | grep -oE '\b[0-9]{4}\b' | head -1)
    [ -n "$generic_number" ] && {
        echo "SM$generic_number"
        return 0
    }
    
    # 如果都没匹配到，返回清理后的输入
    echo "$input"
    return 1
}

# 专门处理ro.soc.model属性的函数
extract_soc_model() {
    local input="$1"
    
    # 对于ro.soc.model，特别处理MediaTek的复杂格式
    if echo "$input" | grep -qiE 'mt[0-9]'; then
        # 转换大写用于匹配
        local upper_input=$(echo "$input" | tr '[:lower:]' '[:upper:]')
        
        # 精确匹配MT6895Z_A_TCZA -> MT6895Z_A
        if echo "$upper_input" | grep -qE 'MT[0-9]+[A-Z]+_[A-Z]_[A-Z]+'; then
            local extracted=$(echo "$upper_input" | grep -oE 'MT[0-9]+[A-Z]+_[A-Z]' | head -1)
            if [ -n "$extracted" ]; then
                echo "$extracted"
                return 0
            fi
        fi
    fi
    
    # 如果不是特殊格式或提取失败，使用通用逻辑
    extract_chip_model_like_c "$input"
}

detect_cpu_model() {
    # 1. 优先使用 getprop 直接获取属性
    prop_keys=(
        "ro.soc.model"                # 最高优先级（用户反馈）
        "ro.product.oplus.cpuinfo"    # 一加设备
        "ro.boot.chipname"             # 通用芯片名属性
        "ro.hardware.chipname"         # 硬件芯片名
        "ro.board.platform"            # 主板平台
        "ro.mediatek.platform"         # 联发科平台
        "ro.vendor.mediatek.platform"  # 厂商联发科平台
        "ro.boot.platform"             # 启动平台
        "ro.product.board"             # 产品主板
        "ro.mediatek.hardware"         # 联发科硬件
        "ro.build.device_family"       # 一加特有属性
    )
    
    # 尝试通过 getprop 获取属性值
    for key in "${prop_keys[@]}"; do
        value=$(getprop "$key" 2>/dev/null)
        [ -n "$value" ] || continue
        
        # 清理值（移除引号和空白字符）
        value=$(echo "$value" | tr -d '"' | tr -d "'" | tr -d ' \t\n\r')
        
        # 特别处理ro.soc.model属性
        if [ "$key" = "ro.soc.model" ]; then
            extracted_model=$(extract_soc_model "$value")
        else
            # 其他属性使用通用逻辑
            extracted_model=$(extract_chip_model_like_c "$value")
        fi
        
        [ -n "$extracted_model" ] && {
            echo "当前处理器型号:$extracted_model"
            return 0
        }
    done

    # 2. 检查 build.prop 文件（当 getprop 失败时）
    prop_files=(
        "/odm/build.prop"
        "/vendor/build.prop"
        "/system/build.prop"
        "/system/vendor/build.prop"
        "/vendor/odm/etc/build.prop"
    )
    
    for prop_file in "${prop_files[@]}"; do
        [ -f "$prop_file" ] || continue
        
        for key in "${prop_keys[@]}"; do
            value=$(grep -m1 "^$key=" "$prop_file" 2>/dev/null | cut -d= -f2-)
            [ -n "$value" ] || continue
            
            # 清理值（移除引号和空白字符）
            value=$(echo "$value" | tr -d '"' | tr -d "'" | tr -d ' \t\n\r')
            
            # 特别处理ro.soc.model属性
            if [ "$key" = "ro.soc.model" ]; then
                extracted_model=$(extract_soc_model "$value")
            else
                # 其他属性使用通用逻辑
                extracted_model=$(extract_chip_model_like_c "$value")
            fi
            
            [ -n "$extracted_model" ] && {
                echo "当前处理器型号:$extracted_model"
                return 0
            }
        done
    done

    # 3. 检查 /proc/cpuinfo
    if [ -f "/proc/cpuinfo" ]; then
        hardware_info=$(grep -m1 -E 'Hardware|hardware|Processor|processor' /proc/cpuinfo 2>/dev/null | cut -d: -f2 | sed 's/^[ \t]*//;s/[ \t]*$//')
        [ -n "$hardware_info" ] && {
            # 使用与C代码一致的芯片型号提取逻辑
            extracted_model=$(extract_chip_model_like_c "$hardware_info")
            [ -n "$extracted_model" ] && {
                echo "当前处理器型号:$extracted_model"
                return 0
            }
        }
    fi

    # 4. 检查 SoC 信息文件
    soc_files=(
        "/sys/devices/soc0/soc_id"
        "/sys/devices/soc0/machine"
        "/sys/devices/soc0/family"
        "/sys/class/soc/soc0/soc_id"
        "/sys/devices/soc0/soc_name"
        "/sys/devices/soc0/chip_name"
    )
    
    for soc_file in "${soc_files[@]}"; do
        [ -f "$soc_file" ] || continue
        
        soc_value=$(cat "$soc_file" 2>/dev/null | tr -d '\n')
        [ -z "$soc_value" ] && continue
        
        # 特殊处理 soc_id（纯数字则添加 SM 前缀）
        if [ "$(basename "$soc_file")" = "soc_id" ] && echo "$soc_value" | grep -qE '^[0-9]+$'; then
            echo "当前处理器型号:SM$soc_value"
            return 0
        fi
        
        # 使用与C代码一致的芯片型号提取逻辑
        extracted_model=$(extract_chip_model_like_c "$soc_value")
        [ -n "$extracted_model" ] && {
            echo "当前处理器型号:$extracted_model"
            return 0
        }
    done

    # 5. 检查设备树模型
    if [ -f "/sys/firmware/devicetree/base/model" ]; then
        model_value=$(cat "/sys/firmware/devicetree/base/model" 2>/dev/null)
        extracted_model=$(extract_chip_model_like_c "$model_value")
        [ -n "$extracted_model" ] && {
            echo "当前处理器型号:$extracted_model"
            return 0
        }
    fi

    # 6. 尝试 dmesg 日志（需要 root）
    if [ "$(id -u)" -eq 0 ]; then
        dmesg_output=$(dmesg 2>/dev/null | grep -iE 'soc|chip|qcom|msm|mtk|mediatek|sm[0-9]+|mt[0-9]+|exynos|snapdragon' | head -5)
        extracted_model=$(extract_chip_model_like_c "$dmesg_output")
        [ -n "$extracted_model" ] && {
            echo "当前处理器型号:$extracted_model"
            return 0
        }
    fi

    # 7. 尝试 /proc/device-tree/compatible
    if [ -f "/proc/device-tree/compatible" ]; then
        compatible=$(cat "/proc/device-tree/compatible" 2>/dev/null | tr '\0' ' ' | head -c 100)
        extracted_model=$(extract_chip_model_like_c "$compatible")
        [ -n "$extracted_model" ] && {
            echo "当前处理器型号:$extracted_model"
            return 0
        }
    fi

    # 所有检测方法失败
    echo "当前处理器型号:default"
    return 1
}

# 执行检测并输出结果
detect_cpu_model