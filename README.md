# 慕容调度模块

> 下一代智能调度引擎 · 全平台自适应优化 · 性能与能效的完美平衡  
> 从 Shell 到 C 的架构革新，内存占用减少 80%，响应速度提升 300%

## 🎯 最新版本 5.0 - 企业级升级

### 🚀 重大突破
- **CPU控制组(cpuctl)支持**  
  ⚡ 新增精细化CPU利用率控制 (uclamp_min/max 0-100%)  
  🎛️ 延迟敏感任务优先级控制 (latency_sensitive)  
  ⚖️ CPU时间片分配权重调节 (cpu.shares 2-262144)  
  🎯 四层级控制组：background/system-background/foreground/top-app

- **企业级日志管理**  
  📁 自动日志备份和轮转系统  
  🧹 智能清理旧日志，防止存储空间占用  
  📊 详细的系统运行状态记录

- **配置版本管理**  
  🔄 配置文件版本控制和兼容性检查  
  📋 支持配置迁移和向后兼容

### ⚡ 性能优化 (版本 4.9)
- **事件驱动架构**  
  🔍 inotify实时文件监控，告别轮询模式  
  ⚡ 配置文件变化即时响应，延迟 <10ms  
  🎯 智能前台应用过滤，提升检测准确率

- **系统资源优化**  
  💾 事件驱动模式减少CPU占用 60%  
  🛡️ 新增缓冲区溢出保护机制  
  🔧 增强错误处理和系统稳定性

## 🚀 架构演进历程

### 版本对比
| 版本 | 核心特性 | 性能提升 |
|------|----------|----------|
| **5.0** | CPU控制组 + 企业级日志 | 精细化控制 |
| **4.9** | 事件驱动 + 实时监控 | CPU占用 ↓60% |
| **4.8** | 基础调度 + 轮询检测 | 稳定运行 |

### 技术架构升级
```
4.8: 基础调度功能 → 轮询检测模式
4.9: 事件驱动架构 → 实时响应机制  
5.0: 企业级特性 → 精细化控制系统
```

## 🚀 全新架构升级 (慕容调度4.7)

### 核心突破
- **动态拓扑识别**  
  ⚡ 智能识别多级CPU集群架构 (3X Cortex-X4 + 5X A720 + 2X A520)
- **通用调速框架**  
  🔧 支持 schedutil/walt/cpufreq 等多种调速器 + 动态参数注入
- **跨平台适配**  
  🌐 高通 SM8550/SM8650/SM8750 全系支持 + 自动回退机制
- **智能频率引擎**  
  📊 支持 DDR 动态表达式：`"ddr_min*1.2"`，`"ddr_max-200000"`

### 性能飞跃
| 指标         | V4.5   | V4.7     | 提升    |
|--------------|--------|----------|---------|
| 内存占用     | 20MB   | **4.4MB** | ↓78%   |
| 响应延迟     | 150ms  | **<50ms** | 3X     |
| 配置加载速度 | 300ms  | **80ms**  | 275%   |
| 策略应用速度 | 120ms  | **40ms**  | 200%   |

## ✨ 核心特性

### 智能调度系统
- **三阶策略引擎**  
  `基础模式 → 应用优化 → 实时动态调整`
- **热更新配置系统**  
  修改JSON即时生效，无需重启进程
- **自适应降级机制**  
  错误配置自动回退均衡模式 + 详细错误日志
- **精准应用识别**  
  窗口焦点追踪 + 包名过滤双重机制

### 资源优化
- **动态频率控制**  
  CPU/DDR 频率智能调节 (支持绝对值和相对值)
- **cpuset智能分配**  
  四层级核心隔离：`top-app` `foreground` `background` `system-background`
- **特殊应用加速**  
  相机/游戏/启动器等，最高支持32个应用专属优化策略

## 📦 安装指南

### 刷入步骤
```bash
# 下载最新模块

# 刷入模块

# 选择优化模式 (刷机时音量键选择)
[↑] 保留插帧（不推荐）
[↓] 完全优化 （推荐）
```

> 自动禁用冲突服务：`oiface` `joyose` `gamewatch`

## ⚙️ 配置系统

### 目录结构
```
/data/adb/modules/muronggameopt/
├── bin/
│   └── cpu/
│       ├── SM8550.json        # 骁龙8 Gen2配置（演示结构，并无8 Gen2配置）
│       ├── SM8650.json        # 骁龙8 Gen3配置
└── config/
    ├── mode.txt               # 应用模式映射
    └── log.txt                # 运行日志
```

### 模式配置 (`config/mode.txt`)
```txt
# 格式: [包名] [模式]
performance    # 全局默认模式

com.tencent.mm balance       # 微信使用均衡模式
com.mi.horizon performance   # 地平线5使用性能模式
```

### CPU 配置文件示例 (`bin/cpu/SM8750.json`)
```json
{
  "powersave": { //省电模式配置
    "cpu_policies": [ //CPU参数设置
      {
        "cluster": 0, //第一个CPU簇（此配置为骁龙8E的，所以仅有两个，0代表第一个CPU簇对应policy0）
        "governor": "walt", //调速器设置为walt
        "max_freq": 2227200, //最小频率设置为2227200 KHz (2227.2 MHz)
        "min_freq": 384000, //最小频率设置为384000 KHz (384 MHz)
        "params": {
          "adaptive_level_1": "90", //负载阈值设置为90%。当CPU负载超过当前频率对应的阈值时，调度器会认为该频率不足以处理负载，从而尝试升频。值越低越积极升频，值越高越保守。
          "hispeed_cond_freq":"960000", //高负载时最低频率阈值设为960mhz
          //上面两个选项仅8e有，8GEN3为target_loads，及负载阈值触发升频，假如我这样写："target_loads": "80",代表每个频率占用率为80%，"80 1574400:85",代表频率1574.4mhz前占用率都为80%，达到1574.4mhz占用率为85%，"target_loads": "80 1574400:85 2035200:90"` 的解释是准确的：代表在频率达到1574.4 MHz之前，负载阈值是80%；在1574.4 MHz到2035.2 MHz之间，负载阈值是85%；在2035.2 MHz及以上，负载阈值是90%。
          "hispeed_freq": "960000", //高负载立即提升到960mhz频率
          "hispeed_load": "90", //达到90%占用立即升到hispeed_freq所设置的频率
          "up_rate_limit_us": "8000", //升频操作之间的最小时间间隔(8000微秒 / 8毫秒)。值越大升频越缓慢平滑。
          "down_rate_limit_us": "2000", //降频操作之间的最小时间间隔(2000微秒 / 2毫秒)。值越大降频越缓慢平滑
          "boost": "-30", //对调度器计算出的"需求频率"进行百分比偏移。负值(如-30)降低需求频率(更倾向于降频/低频运行)，正值(如10)提高需求频率(更倾向于升频/高频运行)，0表示不偏移。偏移后的频率仍受min_freq和max_freq限制。负值有助于更快降到rtg_boost_freq附近。
          "rtg_boost_freq": "748800", //实时任务组(RTG)提升频率。当有实时任务(如UI线程)被调度时，调度器会努力让CPU至少运行在此频率(748800 KHz / 748.8 MHz)上以保证响应性。
          "target_load_shift": "2" //检测负载精度设置为2
        }
      },
      {
        "cluster": 1, //代表第二个簇，对应policy6，后面都一样，不过多解释了
        "governor": "walt",
        "max_freq": 2246400,
        "min_freq": 1017600,
        "params": {
          "adaptive_level_1": "90",
          "hispeed_cond_freq":"1017600",
          "hispeed_freq": "1017600",
          "hispeed_load": "90",
          "up_rate_limit_us": "8000",
          "down_rate_limit_us": "2000",
          "boost": "-35",
          "rtg_boost_freq": "1017600",
          "target_load_shift": "2"
        }
      }
    ],
    "cpuset": { //线程隔离
      "background": "0-4", //用户后台进程设置为0到4核心，0，1，2，3，4。
      "systembackground": "0-4", //系统后台进程设置为0到4核心
      "foreground": "0-6", //前台进程设置为0-6核心
      "topapp": "0-7" //顶层进程设置为0-7核心
    },
    "ddr": {
      "min_freq": "ddr_min", //最小ddr频率不变
      "max_freq": "ddr_min*3.824499" //最大ddr频率设置为最小ddr频率的3.8倍。
    },
    "cpuctl": { //CPU控制组设置 (5.0版本新增)
      "background": {
        "uclamp_min": 0,        //后台进程CPU利用率下限 0%
        "uclamp_max": 30,       //后台进程CPU利用率上限 30%
        "latency_sensitive": 0, //后台进程不需要低延迟
        "shares": 52            //后台进程CPU时间片权重较低
      },
      "systembackground": {
        "uclamp_min": 0,        //系统后台进程CPU利用率下限 0%
        "uclamp_max": 50,       //系统后台进程CPU利用率上限 50%
        "latency_sensitive": 0, //系统后台进程不需要低延迟
        "shares": 256           //系统后台进程CPU时间片权重中等
      },
      "foreground": {
        "uclamp_min": 10,       //前台进程CPU利用率下限 10%
        "uclamp_max": 80,       //前台进程CPU利用率上限 80%
        "latency_sensitive": 1, //前台进程需要低延迟响应
        "shares": 1024          //前台进程CPU时间片权重较高
      },
      "topapp": {
        "uclamp_min": 20,       //顶层应用CPU利用率下限 20%
        "uclamp_max": 100,      //顶层应用CPU利用率上限 100%
        "latency_sensitive": 1, //顶层应用需要最低延迟
        "shares": 1024          //顶层应用CPU时间片权重最高
      }
    }
  },
  "balance": { //均衡模式，后面都一样了，不多解释了。

      省略......
    },

  "performance":{

    },

  "fast":{

    },
  "special_apps": [ //单应用模式
  {
    "package": "com.oplus.engineercamera,com.oplus.camera,com.android.launcher", //相机，工程相机，桌面的包名
    "min_freq": [1552000, 1958400], //覆盖上面普通模式的最小频率
    "params": {
      "up_rate_limit_us": "0", //覆盖升频延迟为0ms
      "down_rate_limit_us": "0", //覆盖降频延迟为0ms
      "boost": "0" //覆盖boost参数为0
    }
  }
]
}```

## 🔍 问题诊断

### 查看实时日志
```bash
# 查看当前日志
adb shell "tail -f /data/adb/modules/muronggameopt/config/log.txt"

# 查看日志备份 (5.0版本新增)
adb shell "ls -la /data/adb/modules/muronggameopt/config/log_backups/"
adb shell "cat /data/adb/modules/muronggameopt/config/log_backups/log_backup_20250117_191503.txt"

# 示例输出
2025-07-17 19:15:03 - 启动调度服务
2025-07-17 19:15:03 - 检测到CPU型号: SM8650
2025-07-17 19:15:03 - 成功重新挂载/sys为可读写
2025-07-17 19:15:03 - CPU集群检测结果:
Cluster 0 (策略: 0)
Cluster 1 (策略: 2)
Cluster 2 (策略: 5)
Cluster 3 (策略: 7)

2025-07-17 19:15:03 - 加载了 3 个特殊应用设置2025-07-17 19:21:24 - 应用: com.oplus.screenrecorder | 模式: powersave | 设置已应用
2025-07-17 19:21:26 - 应用: bin.mt.plus | 模式: powersave | 设置已应用
2025-07-17 19:24:07 - ===== 应用特殊设置: com.android.launcher =====
2025-07-17 19:24:07 - 设置内容: min_freq[1344000,1920000,1920000,1939200] params[up_rate_limit_us=0,down_rate_limit_us=0,boost=0]
2025-07-17 19:24:07 - 详细设置:
策略 0: min_freq=1344000 kHz, up_rate_limit_us=0, down_rate_limit_us=0, boost=0
策略 2: min_freq=1920000 kHz, up_rate_limit_us=0, down_rate_limit_us=0, boost=0
策略 5: min_freq=1920000 kHz, up_rate_limit_us=0, down_rate_limit_us=0, boost=0
策略 7: min_freq=1939200 kHz, up_rate_limit_us=0, down_rate_limit_us=0, boost=0

2025-07-17 19:24:07 - ===== 特殊设置应用完成 =====

> **💡 技术提示**：模块采用`双层缓存机制`，配置加载速度比传统方案快3倍  
> **📬 反馈交友**： QQ群: 974835379  
> **🌎 适配机型**：一加12/小米14/真我GT5 Pro 等骁龙8 Gen3机型，其他处理器请自行二改

---

## 📄 开源许可证

本项目采用 **GNU General Public License v3.0** 开源许可证。

### 🔓 您的权利
- ✅ **自由使用** - 可以自由运行本软件
- ✅ **自由研究** - 可以研究软件工作原理
- ✅ **自由分享** - 可以重新分发软件副本
- ✅ **自由修改** - 可以修改软件并分发修改版本
- ✅ **商业使用** - 可以用于商业目的

### 📋 您的义务
- 📝 **保留版权** - 必须保留原始版权声明
- 🔄 **相同许可** - 修改版本必须使用相同的GPLv3许可证
- 📖 **提供源码** - 分发时必须提供源代码或提供获取源代码的方式
- 🏷️ **标明修改** - 如果修改了代码，必须明确标明修改内容

### ⚖️ 法律声明
```
慕容调度模块 (Murong Scheduling Module)
Copyright (C) 2025 慕容如燕 (murongruyan)

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
```

### 🔗 相关链接
- 📜 [完整许可证文本](LICENSE)
- 🌐 [GPLv3官方说明](https://www.gnu.org/licenses/gpl-3.0.html)
- 📚 [开源许可证指南](https://choosealicense.com/licenses/gpl-3.0/)

---

**💡 选择GPLv3的原因：**
- 🛡️ **强保护性** - 确保代码永远保持开源
- 🤝 **社区友好** - 鼓励开源社区贡献和改进
- ⚖️ **防止闭源** - 防止他人将开源代码闭源商业化
- 🔄 **持续开放** - 保证所有衍生作品都保持开源
