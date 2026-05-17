# 慕容调度 5.6 免费版 — 开源模块

这是 `慕容调度` 的 **免费开源版** 模块仓库，面向 Magisk / KernelSU / APatch 模块环境。

它不是 APK 工程，而是设备侧真实运行的调度模块目录，负责模块刷入后在手机上实际生效的调度行为。

---

## 开源说明

本仓库为独立维护的开源实现，与付费版属于**两个不同的仓库**，没有代码共享关系。

- **本仓库**：免费开源版。代码独立编写，独立维护，可公开审查。
- **付费版**：闭源。包含额外的实验性/高性能特性，不在本仓库中。

两套版本都遵守相同的模块接口和 JSON 配置文件格式，但内部实现互不依赖。

---

## 功能范围

本仓库包含以下能力，全部对用户透明并可审查源码：

### ✅ 包含的功能

| 类别 | 具体能力 |
|---|---|
| **CPU 调频** | 调速器切换、最大/最小频率设置、governor 参数调整 |
| **cpuset 管控** | top-app / foreground / background 等 cgroup 策略 |
| **cpuctl 管控** | cpu.shares / cpu.max 等参数写入 |
| **场景策略** | 前台应用识别、场景分类、动态提频 |
| **自定义线程** | 基于 JSON 配置的线程匹配与绑核（`main/gfx/render/worker/other/extra`） |
| **线程级策略** | 绑核 (cpus/clusters)、实时优先级 (SCHED_RR/FIFO)、nice |
| **模式配置** | powersave / balance / performance / fast 多档位 |

### ❌ 不包含的功能

| 类别 | 说明 |
|---|---|
| **FAS 帧感知** | 付费版独占的帧率采样与 queueBuffer 监控链路 |
| **动态调频引擎** | 付费版独占的预测性频率策略链 |
| **动态线程学习** | 付费版独占的线程自动分类 / 学习 / 锁定 / guard 机制 |
| **授权验证** | 本仓库无卡密 / 设备绑定 / 在线校验逻辑 |

---

## 与本仓库不重复的开源内容

如果你希望更全面地理解这套调度的完整体系，以下文件与本仓库同时维护但不直接包含在本仓库源码里：

- `配置字段说明`：官网文档站点的免费版专题
- `场景分类指南`：`categories.json` 的字段说明
- `BPF自定义线程写法`：拓扑 JSON 的线程规则写法
- `调度整体说明`：安装链、启动链、配置主线和维护口径

## 5.6 相比旧开源版的变化

如果和旧公开版 `5.1 / 5.2` 这条线相比，`5.6` 这次不是单纯补几个功能点，而是把公开版重新收敛成一套更适合长期维护、直接公开发布、也更接近真实运行时的稳定主线。

### 版本对比

| 版本 | 公开版主线 | 版本定位 |
| --- | --- | --- |
| `5.1` | 智能配置生成、基础公开版 | 以配置生成和早期开源为主 |
| `5.2` | 旧公开仓库展示版 | 更偏功能展示和历史说明 |
| `5.6` | CPU 模式 + 场景提频 + 线程专项 | 面向当前维护和 GitHub 发布的免费版主线 |

### 主要提升

- 线程调度链更完整：不再只是按包名切模式，而是保留了线程专项调度主线，支持 `main / gfx / render / worker / other / custom` 这类线程角色规则。
- 线程控制粒度更细：公开版现在保留绑核、`nice`、`RR/FIFO` 等线程级策略，不再只是简单调频参数集合。
- 配置结构更清晰：`bin/cpu/<SoC>.json` 负责模式参数，`bin/cpu/<拓扑>.json` 负责线程专项，`config/categories.json` 负责场景分类，维护边界更明确。
- 运行时实现更完整：当前公开版主逻辑已经收敛到原生二进制、配置加载、前台识别、线程运行时这一整套链路，而不是早期偏轻量脚本式实现。
- 构建链更完整：`monitor.skel.h`、`monitor.bpf.o`、`activity_diaodu` 已重新生成，公开目录也补齐了仓库说明和发布资料。

### 新增或整理

- 新增免费版专用 [调度整体使用说明.md](file:///c:/android-ndk-r27d-windows/diaodu/apk/免费版模块/5.6/调度整体使用说明.md)，单独说明免费版安装链、启动链、配置主线和维护口径。
- 新增适合公开仓库使用的 [.gitignore](file:///c:/android-ndk-r27d-windows/diaodu/apk/免费版模块/5.6/.gitignore)，避免把本地缓存、备份和临时文件带进仓库。
- 补齐 [LICENSE](file:///c:/android-ndk-r27d-windows/diaodu/apk/免费版模块/5.6/LICENSE) 与 [CHANGELOG.md](file:///c:/android-ndk-r27d-windows/diaodu/apk/免费版模块/5.6/CHANGELOG.md)，把公开仓库需要的基础元数据补完整。
- 补充 [THIRD_PARTY_NOTICES.md](file:///c:/android-ndk-r27d-windows/diaodu/apk/免费版模块/5.6/THIRD_PARTY_NOTICES.md)，说明仓库中保留的第三方组件与生成文件来源。
- 重新整理公开版目录，只保留当前免费版真实会维护、会发布、会运行的内容。

### 本次收敛项

- 移除了设备授权相关残留，不再保留公开仓库不需要的校验字段和安装期痕迹。
- 移除了当前仓库不再维护的实验性频率策略实现与历史命名。
- 移除了不再对外维护的动态调节链路，避免公开版继续混入无效策略实现。
- 清理了备份文件、内部草稿和历史命名，让公开仓库只保留当前主线所需内容。

### 版本定位

- 这版不是历史目录的简单拼接，而是围绕 CPU 模式、场景提频、线程专项三条主线重新整理出来的公开版主仓库。
- 这版的目标不是继续堆叠杂项能力，而是把公开版收敛成能维护、能解释、能构建、能直接发 GitHub 的稳定版本。

## 仓库定位

这套免费版仓库主要维护设备侧运行内容，包括：

- 调度主二进制
- CPU / 线程 / 场景分类 JSON
- eBPF 相关源码与产物
- 安装脚本与开机服务脚本

如果你要改的是下面这些方向，主要就是改这个目录：

- CPU 模式配置
- 线程分配与绑核
- 场景分类
- 模块启动与安装逻辑

## 目录结构

- `bin/`
  - 调度主程序、核心源码、CPU 配置、构建脚本
- `bin/cpu/`
  - SoC 模式配置与线程拓扑配置
- `config/`
  - 场景分类配置
- `线程源码/`
  - eBPF 源码、`monitor.bpf.o`、`monitor.skel.h`
- `vtools/`
  - 初始化与辅助脚本
- `customize.sh`
  - 刷入安装期脚本
- `service.sh`
  - 开机服务入口
- `module.prop`
  - 模块元信息

## 当前版本信息

当前 `module.prop` 中的版本信息为：

```properties
id=muronggameopt
name=慕容调度5.6
version=5.6
versionCode=56
author=慕容茹艳（酷安慕容雪绒）
updateJson=https://raw.githubusercontent.com/murongruyan/muronggameopt/main/update3.json
```

## 构建方式

### 1. 生成 eBPF 产物

在 `线程源码/` 目录执行：

```bat
编译monitor.skel.h.bat
```

该脚本会尝试通过 WSL 调用 `clang` 和 `bpftool`，生成：

- `线程源码/monitor.bpf.o`
- `线程源码/monitor.skel.h`

### 2. 构建主程序

在 `bin/` 目录执行：

```bat
build.bat
```

该脚本会使用 Android NDK 交叉编译，输出：

- `bin/activity_diaodu`
- `bin/monitor.bpf.o`
- `bin/libc++_shared.so`

## 环境要求

- Windows
- Android NDK r27d 或兼容版本
- WSL
- WSL 中可用的 `clang`
- WSL 中可用的 `bpftool`

`build.bat` 会优先检测这些 NDK 路径：

- `%ANDROID_NDK_ROOT%`
- `%NDK_ROOT%`
- `C:\android-ndk-r27d-windows\android-ndk-r27d`
- `C:\android-ndk-r27d-windows`

## 配置主线

当前公开版最重要的配置文件是：

- `bin/cpu/<SoC>.json`
  - 模式级 CPU 参数、频率、governor、`cpuset`、`cpuctl`
- `bin/cpu/<拓扑>.json`
  - 线程专项主配置
- `config/categories.json`
  - 场景分类与场景默认策略
- `config/mode.txt`
  - 当前模式与应用级模式覆盖

如果只想记住最短对应关系：

- 改模式和频率：`bin/cpu/<SoC>.json`
- 改线程专项：`bin/cpu/<拓扑>.json`
- 改场景分类：`config/categories.json`
- 改模式切换：`config/mode.txt`

## 安装与发布

- 安装前确认 `module.prop` 中版本号、作者、更新地址正确
- 确认 `bin/activity_diaodu` 已重新编译
- 确认 `bin/monitor.bpf.o` 与 `线程源码/monitor.bpf.o` 为当前构建产物
- 打包前检查 `bin/`、`线程源码/` 中没有备份文件和临时文件
- 发布前建议同步查看 `CHANGELOG.md`、`THIRD_PARTY_NOTICES.md` 与 `调度整体使用说明.md`

## 变更记录与许可证

- 更新记录见 `CHANGELOG.md`
- 开源许可证见 `LICENSE`
- 第三方组件说明见 `THIRD_PARTY_NOTICES.md`

## 说明

- 本仓库是可公开维护的免费版实现。
- 若修改了 `线程源码/monitor.bpf.c`，应先重新生成 `monitor.skel.h` 和 `monitor.bpf.o`，再执行 `bin/build.bat`。
- 若你需要更完整的字段解释与维护口径，优先查看 `调度整体使用说明.md`。
