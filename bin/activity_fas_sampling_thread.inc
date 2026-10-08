#ifndef ACTIVITY_FAS_SAMPLING_THREAD_INC
#define ACTIVITY_FAS_SAMPLING_THREAD_INC

/*
 * activity_fas_sampling_thread.inc —— 纯线程版（-DMURONG_THREAD_ONLY）的 FAS 桩。
 *
 * 只在纯线程版构建里 include，替代 activity_fas_sampling.inc。
 * 它不解析 fas.json、不做甜点/深睡地板查表、不处理 queueBuffer 帧信号，
 * 因此纯线程版可以不编译任何 FAS 实现单元（由 fas/fas_thread_stub.cpp 顶替）。
 *
 * 【为什么必须保留这些内部符号】
 *   activity_scheduler_internal.inc 的运行时统计日志会直接引用 FAS 内部状态
 *   （g_queuebuffer_signal_count / g_queuebuffer_surface_states / g_queuebuffer_package …），
 *   它并不知道 FAS 是否启用。所以这里保留与真实版本【同名同类型】的状态定义，
 *   只是永远不更新（恒为空/0），从而统计日志可以照常编译并显示"无帧信号"。
 *
 * 【为什么必须提供 g_fas_config】
 *   is_fas_enabled_for_app() 与 select_fas_profile() 定义在 activity_diaodu.cpp 里，
 *   直接读 g_fas_config.app_count / apps[]。这里保持 app_count = 0，
 *   于是 is_fas_enabled_for_app() 恒为 false —— FAS 在纯线程版里天然关闭。
 *
 * 【为什么必须提供 queuebuffer 的函数名】
 *   activity_scheduler_internal.inc 引用了 reset_queuebuffer_frame_cache /
 *   handle_queuebuffer_frame_signal，签名必须一致。
 */

/*
 * FAS 总开关。真实版本由 fas.json 的 defaults.enabled 驱动；
 * 纯线程版恒为 false —— activity_mode_watch.inc 会把它读进
 * g_mode_runtime_features.fas_enabled，因此这是「显式关闭」而不是「恰好没配」。
 */
static bool g_fas_enabled_config = false;

/* 注意：g_fas_config 的定义在 activity_diaodu.cpp（:361），此处不要重复定义。 */

/* ── queueBuffer 内部状态：与真实版本同名同类型，纯线程版恒为空 ── */
typedef struct {
    uint64_t ktime_ns;
    uint64_t buffer;
} QueueBufferFrameSignal;

typedef struct {
    uint64_t surface_key;
    uint64_t last_ktime_ns;
    uint64_t last_seen_ktime_ns;
    long long last_wall_ms;
    uint64_t signal_count;
    uint64_t accepted_count;
    uint64_t rejected_count;
    uint64_t last_delta_ns;
    double interval_ewma_ms;
    std::vector<double> frametimes_ms;
} QueueBufferSurfaceState;

static std::mutex g_frame_sample_mutex;
static long long g_queuebuffer_last_wall_ms = 0;
static uint64_t g_queuebuffer_signal_count = 0;
static uint64_t g_queuebuffer_accepted_count = 0;
static uint64_t g_queuebuffer_rejected_count = 0;
static uint64_t g_queuebuffer_last_delta_ns = 0;
static char g_queuebuffer_package[MAX_PACKAGE_LEN] = "";
static pid_t g_queuebuffer_pid = -1;
static std::vector<QueueBufferSurfaceState> g_queuebuffer_surface_states;
static uint64_t g_queuebuffer_primary_key = 0;

/* 纯线程版永远没有可用 surface，返回 nullptr 即可 */
static QueueBufferSurfaceState* select_primary_queuebuffer_surface_locked() {
    return nullptr;
}

static void reset_queuebuffer_frame_cache(pid_t pid, const char* package) {
    /* 保留调用语义（-1, "" 表示清空），但不维护任何缓存 */
    g_queuebuffer_pid = pid;
    g_queuebuffer_primary_key = 0;
    (void)package;
}

/* ring_buffer 回调签名：int(void*, void*, size_t)；恒返回 0 = 不消费 */
static int handle_queuebuffer_frame_signal(void* ctx, void* data, size_t size) {
    (void)ctx;
    (void)data;
    (void)size;
    return 0;
}

/* ── 频率查表（纯线程版不使用，返回 0 = 没配） ── */
static int sweetspot_freq_for_policy(int policy_idx) {
    (void)policy_idx;
    return 0;
}

static int deep_floor_freq_for_policy(int policy_idx) {
    (void)policy_idx;
    return 0;
}

/* ── 档位切换时的重载入口：空操作，且不打日志（避免刷屏） ── */
static void load_fas_sweetspot_for_mode(const char* mode) {
    (void)mode;
}

static void load_fas_deep_floor_for_mode(const char* mode) {
    (void)mode;
}

/* ── FAS 总配置加载：纯线程版不做解析，只确保 FAS 处于关闭态 ── */
void load_fas_config() {
    g_fas_enabled_config = false;
}

#endif /* ACTIVITY_FAS_SAMPLING_THREAD_INC */
