#!/system/bin/sh
TeamS=${0%/*}

# 获取CPU拓扑信息
present=$(cat /sys/devices/system/cpu/present 2>/dev/null)
total_cpus=$(echo "$present" | awk -F'-' '{print $2 + 1}')

# 自动检测所有存在的CPU策略
policy_numbers=()
for policy in /sys/devices/system/cpu/cpufreq/policy*; do
    [ -d "$policy" ] && policy_numbers+=(${policy##*policy})
done

# 按数值升序排列策略
policy_numbers=($(printf "%s\n" "${policy_numbers[@]}" | sort -n))

# 计算CPU集群配置
clusters=()
num_policies=${#policy_numbers[@]}

# 修改为POSIX兼容的循环
i=0
while [ $i -lt $((num_policies - 1)) ]; do
    next=$((i + 1))
    diff=$(( ${policy_numbers[next]} - ${policy_numbers[i]} ))
    clusters+=($diff)
    i=$((i + 1))
done

# 添加最后一个集群的核心数
if [ $num_policies -gt 0 ]; then
    last_cluster=$((total_cpus - policy_numbers[num_policies-1]))
    clusters+=($last_cluster)
fi

# 生成CPU架构字符串
cpu_arch=$(IFS='+'; echo "${clusters[*]}")

# 执行对应的优化脚本
script_path="$TeamS/tool/CPU${cpu_arch}.sh"
if [ -f "$script_path" ]; then
    /system/bin/sh "$script_path"
else
    echo "未找到适配的CPU优化脚本：$script_path" >&2
    echo "检测到的CPU架构：${cpu_arch}" >&2
fi