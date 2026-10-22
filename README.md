# 慕容调度 6.11 免费版 — 开源模块

这是 **慕容调度** 的免费开源版模块，运行在 Magisk / KernelSU / APatch 的模块环境里，
负责刷入之后设备上实际生效的调频、线程与场景调度行为。

它是**设备侧模块仓库**，不是 APK 工程；配套 App（`慕容调度` 2.1.10+）用于图形化配置，
模块本身可以直接用 JSON 配置文件工作。

> **在线教程/文档**：
> - 调度教程（安装、刷入、模式切换、常见问题）：<https://murongdiaodu.rl1.cc/docs.html?doc=app_tutorial.md>
> - 文档中心（快速开始 / 动态调频 / FAS / 线程 / 场景分类）：<https://murongdiaodu.rl1.cc/docs.html>
>
> 仓库内对应文档见 `调度整体使用说明.md`，更新记录见 `changelog.txt` / `CHANGELOG.md`。

---

## 一、这是什么

- **免费开源**：用户态调度实现，源码可审查、可自行编译；与闭源的付费内核版是两个独立仓库。
- **纯用户态 + eBPF**：不替换内核、不刷 boot.img，通过 eBPF 事件、`sched_setaffinity`、
  cpufreq 节点、cgroup/cpuset 与厂商接口完成调度。
- **面向游戏与日常**：FAS 帧感知调频 + 动态调频 + 线程角色绑核 / 动态线程放置 + 场景提频。
- **可解释、可关闭**：所有自动行为都有配置项和日志；默认配置在不折腾的情况下即可用。

支持环境：

- 架构：arm64（AArch64）
- Root：Magisk / KernelSU / APatch
- 系统：Android 12+ 较稳（eBPF/BTF 与 cpuset v1 可用性取决于机型）
- 推荐配套：**慕容调度 App 2.1.10+**（可选，不装也能用 JSON 配置）

---

## 二、功能范围

### ✅ 已包含

| 类别 | 能力 |
|---|---|
| **FAS 帧感知调频** | 三相位控制器（CALIBRATING / STABLE / DEGRADED）：帧债、甜点频率、深睡地板、稳定态 headroom、quick_drop、占用率升频；可调 `conservative`、`target_fps_offset_permille`、`fast_down` |
| **动态调频** | `dyn.json` 阶段表（负载 → 地板/上限千分比）、迟滞、软化、收紧间隔；`corridor_owner` 决定边界由 dyn / fas / mode 谁主导；支持每档位覆盖 |
| **限流曲线（limiter）** | 负载阶段 `idle/inactive/p1/p2/p3` → 每簇 `min/max` 钳制 + `margins` 曲线 + `excludes/prefer` + `core_ctl`，叠加在 FAS / 日用调频之上（默认关闭） |
| **自定义线程** | `bin/cpu/<拓扑>.json` 中按 `main / gfx / render / worker / other / extra` 角色匹配线程，支持 `cpus` / `clusters` / `limit` / `nice` / `rr` / `fifo` |
| **动态线程** | 学习—锁定会话：按负载给未显式规则的线程选簇，迁移节流；支持 `thread_select.strategy = hot / least_cpu` |
| **线程事件驱动** | eBPF `fork / exec / task_rename / exit`：线程改名后立即按新名字重匹配规则（6.7+） |
| **语义核名** | `clusters` 支持 `little / mid / big / prime / top / all` 与 `e-core / p-core / hp-core / all-core` |
| **cpuset + affinity 双写** | 绑核失败时按目标掩码创建 `/dev/cpuset/murong_sched/<hash>` 专用子 cpuset，再重试 `sched_setaffinity`，释放时迁回原组（6.9+） |
| **厂商接口（可选）** | FPSGO `fbt_attr_by_pid`、WALT `sched_per_task_boost`、`sched_pipeline_special`；默认关闭，只写真实存在的节点 |
| **RT 模式（可选）** | 前台游戏主线程 `SCHED_RR / SCHED_FIFO` + 可选 `sched_rt_*` sysctl；默认关闭，切后台/退出恢复 |
| **前台识别** | dumpsys 前台组件解析；可选 Binder 探测（`AIBinder_dump`，fork 子进程隔离，异常自动回退 dumpsys） |
| **场景策略** | `config/categories.json` 场景分类与场景默认策略、按应用模式覆盖（`config/mode.txt`） |
| **官方环境复刻** | 按 `config/official_env.conf` 复刻开发者验证环境：禁用官方增强/定制包、关闭 OIFace、解除 `game_opt` 官方调频限制（可配置关闭） |
| **状态自检** | 只读检测写入 `config/official_state.txt`：被禁用包、OIFace、`/proc/game_opt`、`com.oplus.cosa`、带 `disable` 的 Magisk 模块 |
| **模式系统** | `powersave / balance / performance / fast` 多档位 + 全局功能开关（`config/sched.txt`） |

### ❌ 不包含

| 类别 | 说明 |
|---|---|
| 授权 / 卡密 / 设备绑定 | 免费版无任何在线校验或授权逻辑 |
| 内核模块 / boot 镜像修改 | 免费版是纯用户态；内核级调度属于付费版独立仓库 |
| 官方调度数据库修改 | 不触碰 `com.oplus.cosa` 等官方调度数据库；需要官方调度屏蔽请另装专门模块 |
| 温度墙 / 充电 / 显示增强 | 不属于本模块职责，请用对应模块 |

---

## 三、快速开始

1. 从 [GitHub Releases](https://github.com/murongruyan/muronggameopt/releases) 下载
   `murongdiaodu-free-module-<版本>.zip`，在 Magisk / KernelSU / APatch 中刷入。
2. 重启设备。
3. （建议）安装配套 App `慕容调度 2.1.10+`，在 App 里查看/修改配置。
4. 查看运行日志：
   - `config/log.txt`：主日志
   - `config/debug_log.txt`：调试日志（`config/log.conf` 中 `enabled=1` 才写）
   - `config/fas_log.txt`、`config/stats_log.txt`、`config/affinity_log.txt`：FAS / 调频 / 绑核
   - `config/official_env.log`：官方环境复刻记录
   - `config/official_state.txt`：官方组件状态快照

全局功能开关（`config/sched.txt`）：

```properties
main=1   # 调度总控
cth=1    # 自定义线程
dth=1    # 动态线程
cat=1    # 场景分类
base=1   # 模式基础
```

关闭对应项并确认后写入该文件；模块读取的是这里的值，关闭后不会再被自动打开。

---

## 四、配置主线

| 想改什么 | 改哪里 |
|---|---|
| CPU 模式参数、频率、governor、cpuset、cpuctl | `bin/cpu/<SoC>.json` |
| 线程角色规则 / 绑核 / nice / RR / FIFO | `bin/cpu/<拓扑>.json`（如 `6+2.json`） |
| FAS 甜点/深睡地板/headroom/新参数 | `bin/cpu/fas.json`（`defaults` 与 `deep_floor_freq` / `sweetspot_freq`） |
| 动态调频阶段表、走廊、limiter、RT、厂商接口 | `bin/cpu/dyn.json` |
| 场景分类与场景默认策略 | `config/categories.json` |
| 当前模式 / 按应用模式覆盖 | `config/mode.txt` |
| 全局功能开关 | `config/sched.txt` |
| 官方环境复刻项 | `config/official_env.conf` |
| 日志开关 | `config/log.conf` |

### FAS 关键参数（`bin/cpu/fas.json → defaults`）

```json
{
  "enabled": 1,
  "quick_drop": 0,
  "fast_down": 0,
  "conservative": 1,
  "target_fps_offset_permille": 0,
  "report_thread_state": 1,
  "upscale_occupancy": 85,
  "stable_headroom_permille": 1250
}
```

- `conservative`：1=默认；2~4=连续更多采样才判掉帧（升频更保守）
- `target_fps_offset_permille`：预算目标帧率下调千分比（0~300，0=不降）
- `fast_down`：`quick_drop` 的别名，任一开即“帧率稳定立刻降到底板”
- `stable_headroom_permille`：稳定态天花板 = 目标 × 该值 / 1000

### 限流曲线（`bin/cpu/dyn.json → limiter`，默认关闭）

```json
"limiter": {
  "enabled": 0,
  "scope": "games",
  "apply_fas": 1,
  "write_walt": 0,
  "load_mode": 1,
  "stage_hysteresis": 5,
  "stage_load": { "idle": 20, "inactive": 35, "p1": 55, "p2": 75, "p3": 90 },
  "stages": {
    "p2": { "0": { "min": 960000, "max": 2400000, "margins": "200 1555200:130 1996800:90" } }
  }
}
```

- `enabled=1` 后按 cluster0 负载选阶段，对每簇 min/max 做钳制；`apply_fas=1` 表示 FAS 路径也受约束
- `write_walt=1` 时把该阶段 `margins` 写到对应 policy 的 `walt/target_loads`（节点不存在则跳过）

### RT 模式与厂商接口（默认关闭）

```json
"rt": { "enabled": 0, "policy": "rr", "priority": 1, "write_sysctls": 1 },
"vendor_hints": { "enabled": 0, "fpsgo": 1, "walt": 1, "pipeline": 0 },
"thread_select": { "strategy": "hot" }
```

> RT 模式会让游戏主线程进入 SCHED_RR/FIFO，**可能导致卡死/发热**，仅在明确知道风险时开启。

### 官方环境复刻（`config/official_env.conf`）

默认复刻开发者验证环境，可整体关闭：

```properties
enabled=1
disable_packages=com.oplus.ohome,com.oplus.customize.coreapp
oiface_disable=1
game_opt_disable_limit=1
game_opt_debug=0
```

脚本 `vtools/official_env.sh` 在每次模块启动时执行（`activity_diaodu.rc` 中调用），幂等：
已禁用的跳过、未安装的跳过；日志见 `config/official_env.log`。

---

## 五、与 Scene 的配合

本模块支持作为 **Scene 第三方性能调节 profile** 使用：

- 模块会生成 `/data/powercfg.json` 与 `/data/powercfg.sh`；
- Scene 切换到“慕容调度”后，档位切换会调用 `sh /data/powercfg.sh <scheme>`，模块写入
  `config/mode.txt`（首行全局档位 + 后续按应用覆盖行）；
- 线程/FAS/动态调频仍由模块自身配置控制，与 Scene 的档位切换互不冲突。

如果你在 Scene 里切到 Scene 官方的 LP/HP/EP 档位，Scene 会用自己的 daemon/配置，
此时本模块的部分调度可能被禁用或覆盖，属于正常现象；切回“慕容调度”即可恢复。

---

## 六、目录结构

```
.
├── activity_diaodu.rc          # 模块启动入口（先复刻官方环境，再拉起调度）
├── service.sh                  # 开机服务（boot-completed 后初始化 vtools / 启动调度）
├── customize.sh                # 刷入安装期脚本
├── module.prop                 # 模块元信息（版本/作者/更新地址）
├── bin/
│   ├── activity_diaodu         # 调度主程序（arm64 用户态二进制）
│   ├── libbpf.so               # eBPF 用户态依赖
│   ├── monitor.bpf.o           # eBPF 对象（fork/exec/rename/exit 等事件）
│   ├── fas/                    # FAS 三相位引擎与配置加载源码
│   └── cpu/
│       ├── <SoC>.json          # 模式级 CPU 参数
│       ├── <拓扑>.json         # 线程角色规则（如 6+2.json）
│       ├── dyn.json            # 动态调频 / limiter / RT / 厂商接口
│       ├── fas.json            # FAS 配置（甜点/地板/headroom/…）
│       └── gen_fas.sh          # 按机型频率表生成默认 FAS 参数
├── config/
│   ├── sched.txt               # 全局功能开关
│   ├── mode.txt                # 当前模式与按应用覆盖
│   ├── categories.json         # 场景分类
│   ├── official_env.conf       # 官方环境复刻配置
│   └── log.conf                # 调试日志开关
├── 线程源码/                    # eBPF 源码与构建脚本（monitor.bpf.c / vmlinux.h / bpf 头文件）
├── vtools/                     # Scene 集成、官方环境复刻等辅助脚本
└── 调度整体使用说明.md           # 安装链 / 启动链 / 维护口径
```

---

## 七、构建方式

### 1. 生成 eBPF 产物

在 `线程源码/` 目录执行 `编译monitor.skel.h.bat`（需要 WSL + clang + bpftool），或手动：

```bash
clang -target bpf -g -O2 -D__TARGET_ARCH_arm64 \
  -I<libbpf 头文件目录> -I. -c monitor.bpf.c -o monitor.bpf.o
```

### 2. 构建主程序

在 `bin/` 目录执行 `build.bat`（Windows + Android NDK r30），输出：

- `bin/activity_diaodu`
- `bin/monitor.bpf.o`

GitHub Actions 会使用 NDK `30.0.14904198` 重新编译并打包模块 zip。

环境要求：

- Windows
- Android NDK `30.0.14904198` 或兼容的 r30 系版本
- WSL + clang（仅 eBPF 对象需要）

---

## 八、常见问题

**Q：刷入后没有生效？**
先确认模块已启用（Magisk/KernelSU 模块列表），查看 `config/log.txt` 是否有
`模式 ... 的功能开关(生效)`、`eBPF 内核聚合 ... 已启用` 等日志；确认 `config/sched.txt`
中 `main=1`，以及 `config/mode.txt` 档位正确。

**Q：FAS 的深睡地板没有生效？**
确认 `bin/cpu/fas.json → defaults.enabled = 1`，并且当前应用没有被 App 的按应用配置
关闭 FAS；查看 `config/debug_log.txt` 里是否有 `fas:1 | fas_buf:1` 和 `c:fas` 执行日志。

**Q：线程没有按规则绑核？**
确认 `config/sched.txt` 中 `cth=1`/`dth=1`，规则包名与当前前台包名一致；查看
`config/affinity_log.txt` 的“静态规则绑核 / 动态层被显式规则压制”等记录。

**Q：怎么完全关闭官方环境复刻？**
`config/official_env.conf` 设 `enabled=0`，重启或手动执行 `vtools/official_env.sh`。

**Q：会不会影响系统 OTA / 保修？**
模块只改运行时的 sysfs/cgroup/应用启用状态，不修改 system/vendor 分区；卸载模块并恢复
官方设置即可回到原状。官方环境复刻会禁用部分 OPLUS 包，按需关闭。

**Q：App 里看不到新功能？**
需要 App **2.1.10+**；旧版 App 仍可使用模块原有功能，但看不到 limiter/RT/厂商接口配置页。

---

## 九、变更记录与许可证

- 详细更新记录：`changelog.txt`（完整历史）与 `CHANGELOG.md`
- 开源许可证：`LICENSE`
- 第三方组件说明：`THIRD_PARTY_NOTICES.md`
- 使用/维护口径：`调度整体使用说明.md`

当前版本信息（`module.prop`）：

```properties
id=muronggameopt
name=慕容调度6.11
version=6.11
versionCode=611
author=慕容茹艳（酷安慕容雪绒）
updateJson=https://raw.githubusercontent.com/murongruyan/muronggameopt/main/update3.json
```
