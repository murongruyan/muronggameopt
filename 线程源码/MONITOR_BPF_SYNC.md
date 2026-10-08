# monitor.bpf.o 同步说明（来源与原因）

## 为什么有这个文件

免费版 5.6 原先随包的 \`bin/monitor.bpf.o\`（827280 字节，2026-05-16 构建）只有：
sched_switch / sched_wakeup / sched_process_fork / sched_process_exec / sys_enter。

缺两个程序：
* raw_tracepoint/sched_process_exit（线程/进程退出事件）
* uprobe/queuebuffer（帧采样，FAS 帧反馈的来源）

现象：daemon 启动日志报 \`eBPF初始化失败: 程序缺失 | switch=1 wakeup=1 queuebuffer=0 fork=1 exec=1 affinity=1\`，
前台开游戏时 control_trace 的 \`frame(avg/min/max)\` / \`debt\` / \`surface\` 全是占位 0，FAS 拿不到帧数据。
换成含 queuebuffer 的 .o 后，同一台设备同一个游戏，帧时间/债务/tier/动作全部变成真实值
（详见 慕容调度-zip\runtime\free-verify\05_fas_ab_monitor_bpf.txt）。

## 本次同步内容（2026-10-05）

| 文件 | 来源 | 大小 | sha256 |
|---|---|---|---|
| 线程源码/monitor.bpf.c | 慕容调度-zip\appopt源码\monitor.bpf.c | 6352 | d2e76147cae1c631d4e195a70c8ff5cf1f4160fac8618d42cb42ce6c8023299a |
| 线程源码/monitor.bpf.o | 慕容调度-zip\bin\monitor.bpf.o | 830696 | 523186fa68e6feea7b87beea7d5cbd4bcadd5a02f1c799a8182bd5db52a68abc |
| bin/monitor.bpf.o | 同上 | 830696 | 523186fa68e6feea7b87beea7d5cbd4bcadd5a02f1c799a8182bd5db52a68abc |

源码差异是纯增量（付费侧 git diff：1 file changed, 43 insertions(+)），只新增
struct queuebuffer_frame_signal、frame_signal_rb ringbuf map、sched_process_exit 程序、queuebuffer uprobe，
没有删除或修改任何既有程序/map。

被替换掉的旧文件留档在 慕容调度-zip\runtime\free-backups\free_original_monitor_bpf\
（.o = 827280 / sha256 df83128890267cb1f788a61ecf517f098574978f4e62d9985b28a83c4974d083，
 .c = 5129 / sha256 bda70d8c7e9f27a8bff3db3349b7a297096db2ff38aaa9b6e20cf0b88e8d8466）。

## 重要：为什么仓库里预编译 .o 与 CI 重编产物不会逐字节相同

免费版 CI（.github/workflows/build-module.yml:187-191）会用上面的 monitor.bpf.c 现场
\`clang -target bpf -g -O2 -D__TARGET_ARCH_arm64\` 重新编译出 monitor.bpf.o 再拷进 bin/。
重编产物与本次同步的预编译 .o 不会逐字节相同（编译环境/版本差异，实测约差 352 字节），
但两者都包含 queuebuffer 与 sched_process_exit，功能一致。

本次发布用的是**已经过真机 A/B 验证的预编译 .o**（830696 字节）；
CI 重编输出会在下次发版时自然替换掉它。若日后发现 sha256 对不上，先看是不是这个原因，
不要当成故障。
