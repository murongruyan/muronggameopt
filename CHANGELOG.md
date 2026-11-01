# 更新记录

## 6.14 优化批次（免费版）
- 平台限制配置支持热重载（`config/platform_limits.conf` 改完约 5s 生效，`enabled=0`/删除配置会恢复 IPC 原值）

- 修复：动态调频日用区间千分比换算 int 溢出（大核 span 大时上限被算错/钉在 floor）
- 新增：DDR 按 GPU 频率动态压制（`config/ddr_gpu_cap.conf`）：GPU≤300MHz → DDR 上限 6389MT/s（本机 3187000kHz）；GPU≥400MHz → 9569MT/s（4780000kHz）；中间线性插值。写全部 `max_freq` 路径，平台持续改写则自动停写；关闭/删配置 5s 内恢复原上限，退出时恢复
- 新增：平台限制管理（`config/platform_limits.conf`）：WALT IPC 频率档位自动钳制（默认取当前第 3 档，可配固定上限/关闭）、日常与游戏两组 WALT 频率上限（默认 INT_MAX 不改变系统行为，可下调）、`msm_performance cpu_max_freq` 基线记录（可选游戏中 raise-only）
- 修复：FAS 帧信号带 tgid/tid 并按目标游戏过滤，避免混入其它应用的 queueBuffer 帧；需 daemon 与 monitor.bpf.o 成对更新
- 修复：RT 模式 `sched_rt_*` sysctl 保存原值，在关闭 RT/切应用/游戏退出/进程退出时恢复
- 修复：`sched.txt` 缺失时 `fas.json defaults.enabled` 不同步；`off` 等值被误解析为 true
- 修复：配置热重载可靠性（加载成功后才提交 hash、失败指数退避重试、inotify 溢出/删除处理、无 inotify 降级定时轮询）
- 修复：线程模板恢复加载 `<拓扑>.json`（含 rt 块），并兼容 6+2 / 2+6 两种命名顺序（`.threads.json` 仍优先）
- 修复：`fas.json` 迁移写回原子化（tmp + fsync + rename；备份失败不覆盖原文件）
- 优化：热路径 sysfs 采样缓存（thermal 400ms、freq 300ms、写后回读节流 1s）、同一 tick affinity 去重、线程 comm 小写缓存、候选容器复用
- 配置：新增 `config/ddr_gpu_cap.conf`、`config/platform_limits.conf`；重新生成 `monitor.skel.h`、`monitor.bpf.o` 与 `activity_diaodu`
## 5.6 免费公开版

- FAS 快速降频开关收敛：`fast_down` 并入 `quick_drop`（历史别名；解析按旧语义 OR 合并并打一次弃用日志，迁移写回不再输出 `fast_down`）
- RT 语义修正：`rt` 块从 `bin/cpu/dyn.json` 迁移到线程配置文件（`bin/cpu/<cpuModel>.threads.json`，兼容 `<拓扑>.threads.json` / 旧 `<拓扑>.json`）；模块优先读新位置，旧 `dyn.json.rt` 仅回退兼容并打印一次弃用提示
- 纯线程版（不打包 `dyn.json`）现在也能从线程配置文件读取 RT 配置
- 旧版 App / 旧配置仍可把 `rt` 写在 `dyn.json`（模块回退读取并打印一次弃用提示；新版 App 应写入线程配置）
- 整理为可公开发布的免费版目录结构
- 保留 CPU 调速器、最大最小频率、调速器参数、`cpuset`、`cpuctl`、场景提频、自定义线程
- 移除设备授权相关残留、实验性频率策略链和当前未维护的动态调节实现
- 重新生成 `monitor.skel.h`、`monitor.bpf.o` 与 `activity_diaodu`
- 补齐 `README.md`、`LICENSE`、`THIRD_PARTY_NOTICES.md` 与公开说明文档

## 5.2

- 引入更细粒度的线程调度策略
- 增强平台识别与 CPU 模式适配能力
- 优化日志级别控制与运行时缓存

## 5.1

- 开源智能 CPU 配置生成系统
- 新增芯片识别与配置生成工具链
- 提升多平台兼容性与配置维护性

## 5.0

- 新增 `cpuctl` 支持
- 增强日志备份与配置版本管理
- 改进 CPU 检测与特殊应用配置结构

## 4.9

- 切换为事件驱动配置监听
- 优化前台应用检测与配置热更新
- 提升整体稳定性和资源利用效率
