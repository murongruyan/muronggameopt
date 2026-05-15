#include "vmlinux.h"
#include <asm/unistd.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>

struct process_event_signal {
    __s32 pid;
    __s32 tid;
    char comm[16];
    __s32 type;
};

struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, __u32);
} target_tgid_map SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, __u32);
} urgent_flag_map SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 1024);
    __type(key, __u32);
    __type(value, __u64);
} thread_runtime_map SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 128);
    __type(key, __u32);
    __type(value, __u32);
} target_tid_map SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, __u64);
} cpu_last_ts_map SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 1 << 15);
} process_event_rb SEC(".maps");

static __always_inline void emit_process_event(__u32 pid, __u32 tid, const char* comm, __s32 type) {
    struct process_event_signal* signal;

    signal = bpf_ringbuf_reserve(&process_event_rb, sizeof(*signal), 0);
    if (!signal) {
        return;
    }

    signal->pid = (__s32)pid;
    signal->tid = (__s32)tid;
    signal->type = type;
    __builtin_memset(signal->comm, 0, sizeof(signal->comm));
    if (comm) {
        __builtin_memcpy(signal->comm, comm, sizeof(signal->comm));
    }
    bpf_ringbuf_submit(signal, 0);
}

SEC("raw_tracepoint/sched_switch")
int handle_sched_switch(struct bpf_raw_tracepoint_args *ctx) {
    struct task_struct *prev = (struct task_struct *)ctx->args[1];
    struct task_struct *next = (struct task_struct *)ctx->args[2];
    __u32 prev_tid = BPF_CORE_READ(prev, pid);
    __u32 prev_tgid = BPF_CORE_READ(prev, tgid);
    __u64 now = bpf_ktime_get_ns();
    __u32 zero = 0;

    __u32* target_tgid_ptr = bpf_map_lookup_elem(&target_tgid_map, &zero);
    if (!target_tgid_ptr || *target_tgid_ptr == 0) {
        return 0;
    }

    if (prev_tgid == *target_tgid_ptr) {
        __u64* last_ts_ptr = bpf_map_lookup_elem(&cpu_last_ts_map, &zero);
        if (last_ts_ptr && *last_ts_ptr > 0 && now > *last_ts_ptr) {
            __u64 runtime = now - *last_ts_ptr;
            __u64* total_runtime = bpf_map_lookup_elem(&thread_runtime_map, &prev_tid);
            if (total_runtime) {
                *total_runtime += runtime;
            } else {
                bpf_map_update_elem(&thread_runtime_map, &prev_tid, &runtime, BPF_ANY);
            }
        }
    }

    bpf_map_update_elem(&cpu_last_ts_map, &zero, &now, BPF_ANY);
    return 0;
}

SEC("raw_tracepoint/sched_wakeup")
int handle_sched_wakeup(struct bpf_raw_tracepoint_args *ctx) {
    struct task_struct *task = (struct task_struct *)ctx->args[0];
    __u32 tid = BPF_CORE_READ(task, pid);
    __u32 *enabled = bpf_map_lookup_elem(&target_tid_map, &tid);
    if (!enabled || *enabled == 0) {
        return 0;
    }

    __u32 zero = 0;
    __u32 urgent = 1;
    bpf_map_update_elem(&urgent_flag_map, &zero, &urgent, BPF_ANY);
    return 0;
}

SEC("raw_tracepoint/sched_process_fork")
int handle_process_fork(struct bpf_raw_tracepoint_args *ctx) {
    struct task_struct *child = (struct task_struct *)ctx->args[1];
    __u32 pid = BPF_CORE_READ(child, tgid);
    __u32 tid = BPF_CORE_READ(child, pid);
    char comm[16];

    if (bpf_core_read_str(comm, sizeof(comm), &child->comm) <= 0) {
        __builtin_memset(comm, 0, sizeof(comm));
    }
    emit_process_event(pid, tid, comm, 1);
    return 0;
}

SEC("raw_tracepoint/sched_process_exec")
int handle_process_exec(struct bpf_raw_tracepoint_args *ctx) {
    struct task_struct *task = (struct task_struct *)ctx->args[0];
    __u32 pid = BPF_CORE_READ(task, tgid);
    __u32 tid = BPF_CORE_READ(task, pid);
    char comm[16];

    if (bpf_core_read_str(comm, sizeof(comm), &task->comm) <= 0) {
        __builtin_memset(comm, 0, sizeof(comm));
    }
    emit_process_event(pid, tid, comm, 2);
    return 0;
}

SEC("raw_tracepoint/sys_enter")
int handle_sched_setaffinity(struct bpf_raw_tracepoint_args *ctx) {
    __u64 syscall_nr = ctx->args[0];
    if (syscall_nr != __NR_sched_setaffinity) {
        return 0;
    }

    __u64 id = bpf_get_current_pid_tgid();
    __u32 pid = (__u32)(id >> 32);
    __u32 tid = (__u32)id;
    char comm[16];
    if (bpf_get_current_comm(comm, sizeof(comm)) != 0) {
        __builtin_memset(comm, 0, sizeof(comm));
    }
    emit_process_event(pid, tid, comm, 3);
    return 0;
}

char LICENSE[] SEC("license") = "GPL";
