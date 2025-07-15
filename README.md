# 慕容调度模块

> 专为 SM8650/8Gen3 平台打造的高性能系统调度模块  
> 从 Shell 到 C 的架构革新，内存占用减少 80%，响应速度提升 300%

## ✨ 核心特性

- **颠覆性架构**  
  Shell → C 全面重构，内存占用 **20MB → 4.4MB** (↓78%)
- **极速响应**  
  前台应用检测 <50ms，执行效率 **提升300%**
- **智能容错**  
  错误配置自动回退均衡模式 + 三重处理器校验
- **热更新系统**  
  JSON 配置修改即时生效，无需重启
- **动态频率引擎**  
  支持 DDR 频率表达式计算：`"ddr_min*3.824499"`
- **特殊应用优化**  
  相机/启动器等关键应用专属调度策略

## 📦 安装指南

### 刷入步骤
1. 下载最新模块
2. 打开你的root管理器
3. 刷入按推荐选择
4. **音量键选择**：
   - ↑ 保留插帧功耗（不推荐）
   - ↓ 完全优化（推荐）

> 刷机过程自动禁用冲突进程 oiface

## ⚙️ 配置说明

### 模式配置 (`mode.txt`)
```
powersave  # 默认模式
com.tencent.mm balance  # 微信使用均衡模式
com.miui.performance performance  # 游戏使用性能模式
```

### JSON 配置 (`activity_diaodu.json`)
```json
{
  "powersave": {
    "cpu_policies": [
      {
        "governor": "walt",
        "max_freq": 1574400,
        "min_freq": 364000,
        "target_loads": "80 1574400:85"
      }
    ],
    "ddr": {
      "min_freq": "ddr_min",
      "max_freq": "ddr_min*3.824499"
    }
  },
  "special_apps": [
    {
      "package": "com.oplus.camera",
      "min_freq": [1344000,1920000,1920000,1939200]
    }
  ]
}
```

## 📊 性能对比

| 指标         | v1.0  | v2.0     | 提升    |
|--------------|-------|----------|---------|
| 内存占用     | 20MB  | **4.4MB**| ↓78%    |
| 响应延迟     | 150ms | **35ms** | ↓76%    |
| 应用启动     | 420ms | **290ms**| +31%    |
| 游戏帧波动   | 8.2%  | **2.1%** | ↓74%    |

## ⚠️ 注意事项

1. **设备兼容**  
   仅支持 SM8650/骁龙 8Gen3 平台
   ```c
   // 三重校验逻辑
   check_processor() {
     return find_cpuinfo("SM8650") || 
            find_buildprop("/odm/build.prop") || 
            find_buildprop("/system/build.prop");
   }
   ```

2. **配置建议**  
   - 修改 JSON 后自动生效（10s 内）
   - 错误模式自动切换 `balance`
   - 特殊应用配置优先于全局模式

## ❓ 常见问题

**Q：如何确认调度生效？**  
```bash
adb shell cat /data/adb/modules/muronggameopt/config/log.txt
```

**Q：能否兼容其他处理器？**  
× 设计仅针对 SM8650/8Gen3 平台架构

**Q：配置错误怎么办？**  
✓ 自动回退到均衡模式 + 错误日志记录

**Q：为何禁用 oiface 进程？**  
⚠️ 该进程与调度策略存在资源冲突

---
 
🐛 **问题反馈，聊天交友**：QQ群聊：974835379
