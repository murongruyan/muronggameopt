# FAS 模块 — 使用说明

## 职责

FAS（Frame Aware Scheduling）只看画面：从 queueBuffer 相邻两帧的间隔得到**真实帧时间**，
和这一帧的渲染时间预算相减得到**帧债**，再把帧债换算成**动作档位**。

引擎不统计 CPU 负载、不预测频率、不为决策扫 `/proc`；频率怎么给是执行器按档位查表的事。

```
帧债 = 短窗口平均帧时间 - (1e6 / (target_fps - margin_fps))

深睡   帧债 <= 0            → 执行器贴地板（能省就省）
甜点   0 < 帧债 <= 10% 预算 → 甜点频率（帧债>0 时的最低频率）
放开   10% < 帧债 <= 30%    → 甜点做底 + 放开档位上限
顶格   帧债 > 30% 预算      → 顶 fmax（记一次顶格事件）
       或连续两帧严重超预算
```

## 文件

| 文件 | 职责 |
|------|------|
| `fas.h` / `bridge.h` | 统一头文件 |
| `fas_types.h` | 类型定义：`FasConfig` / `FasOutput` / `FasActionLevel` |
| `fas_config.h+cpp` | fas.json 加载、解析（开关、档位、margin、灵敏度） |
| `fas_sampling.h+cpp` | 帧缓冲 + 窗口统计 + 快照采集（帧时间只由真实帧构成） |
| `fas_target.h+cpp` | 目标帧率识别（升降档，降档/掉档 3s 防抖） |
| `fas_engine.h+cpp` | FasEngine 容器：`push_frame` / `tick` → 动作档位 |

## 使用方法

```c
#include "fas/fas.h"

FasEngine engine;
fas_engine_init(&engine, "/data/adb/modules/muronggameopt/bin/cpu/fas.json");

// 前台切换时
fas_engine_on_app_change(&engine, "com.example.game", pid);

// 每来一帧（queueBuffer 的相邻间隔）
fas_engine_push_frame(&engine, interval_ns);

// 每个采样周期
FasOutput out = fas_engine_tick(&engine);
if (out.action_level >= 0) {
    // 交给执行器：按档位查表写 scaling_min/max
    apply_action_level(out.action_level, out.budget_us, out.debt_us);
}

fas_engine_destroy(&engine);
```
