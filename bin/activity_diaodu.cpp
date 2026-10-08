/*
 * 纯线程版（-DMURONG_THREAD_ONLY）用 bridge_thread.h + *_thread.inc 桩，
 * 从而不编译 FAS 的 4 个实现单元与动态调频，彻底去掉这部分死代码。
 */
#ifdef MURONG_THREAD_ONLY
#include "fas/bridge_thread.h"
#else
#include "fas/bridge.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <dirent.h>
#include <sys/types.h>
#include <ctype.h>
#include <errno.h>
#include <sys/mount.h>
#include <sys/inotify.h>
#include <sys/select.h>
#include <stdarg.h>
#include <pthread.h>
#include <stdint.h>
#include <signal.h>
#include <atomic>
#include <vector>
#include <string>
#include "activity_common.inc"
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <shared_mutex>
#include <mutex>
#include <algorithm>
#include <sched.h>
#include <fnmatch.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include "cJSON.h"
#include "bpf/libbpf.h"
#include "bpf/bpf.h"

struct ring_buffer;
typedef int (*ring_buffer_sample_fn)(void* ctx, void* data, size_t size);

struct ring_buffer_opts {
    size_t sz;
};

enum probe_attach_mode {
    PROBE_ATTACH_MODE_DEFAULT = 0,
};

struct bpf_uprobe_opts {
    size_t sz;
    size_t ref_ctr_offset;
    uint64_t bpf_cookie;
    bool retprobe;
    const char* func_name;
    probe_attach_mode attach_mode;
};

extern "C" {
struct bpf_program* bpf_object__next_program(const struct bpf_object* obj, struct bpf_program* prev);
const char* bpf_program__name(const struct bpf_program* prog);
const char* bpf_program__section_name(const struct bpf_program* prog);
struct bpf_link* bpf_program__attach_uprobe_opts(const struct bpf_program* prog,
                                                 int pid,
                                                 const char* binary_path,
                                                 size_t func_offset,
                                                 const struct bpf_uprobe_opts* opts);
struct ring_buffer* ring_buffer__new(int map_fd,
                                     ring_buffer_sample_fn sample_cb,
                                     void* ctx,
                                     const struct ring_buffer_opts* opts);
int ring_buffer__poll(struct ring_buffer* rb, int timeout_ms);
void ring_buffer__free(struct ring_buffer* rb);
}

#define LOG_FILE "/data/adb/modules/muronggameopt/config/log.txt"
#define DEBUG_LOG_FILE "/data/adb/modules/muronggameopt/config/debug_log.txt"
#define FAS_LOG_FILE "/data/adb/modules/muronggameopt/config/fas_log.txt"
#define AFFINITY_LOG_FILE "/data/adb/modules/muronggameopt/config/affinity_log.txt"
#define STATS_LOG_FILE "/data/adb/modules/muronggameopt/config/stats_log.txt"
#define LOG_BACKUP_DIR "/data/adb/modules/muronggameopt/config/log_backups"
#define MAX_LOG_FILE_SIZE (200 * 1024)
#define MAX_AUX_LOG_FILE_SIZE (5 * 1024 * 1024) // 5MB for debug and stats logs
#define CONFIG_DIR "/data/adb/modules/muronggameopt/config/"
#define SCENE_CATEGORIES_FILE CONFIG_DIR "categories.json"
#define SCHEDULER_SKIP_PROCESSES_FILE CONFIG_DIR "skip_processes.txt"
/* 日志开关的独立配置文件：不再寄生在动态调频参数里 */
#define LOG_CONFIG_FILE CONFIG_DIR "log.conf"
#define MODE_FILE "/data/adb/modules/muronggameopt/config/mode.txt"
#define CPU_CONFIG_DIR "/data/adb/modules/muronggameopt/bin/cpu/"
#define BPF_OBJECT_FILE "/data/adb/modules/muronggameopt/bin/monitor.bpf.o"
#define FAS_CONFIG_FILE CPU_CONFIG_DIR "fas.json"
#define LIBGUI_SO_PATH "/system/lib64/libgui.so"
// queueBuffer 的 C++ 符号名必须使用 mangled name 才能附加 uprobe。
#define QUEUEBUFFER_SYMBOL_V1 "_ZN7android7Surface11queueBufferEP19ANativeWindowBufferi"
#define QUEUEBUFFER_SYMBOL_V2 "_ZN7android7Surface11queueBufferEP19ANativeWindowBufferiPNS_24SurfaceQueueBufferOutputE"
/*
 * Android 13+ (含本机 Android 16 / SM8850) 的 libgui 里 Surface::queueBuffer 换了签名，
 * 旧的两个 mangled name 在系统里根本不存在（实测 llvm-nm 查无此符号），
 * uprobe 就永远挂不上 → FAS 拿不到任何帧信号（frame 全 0、surface 0x0），
 * 表现就是"FAS 开着但全程不工作，频率一直由负载表在调"。
 */
#define QUEUEBUFFER_SYMBOL_V3 \
    "_ZN7android7Surface11queueBufferERKNS_2spINS_13GraphicBufferEEERKNS1_INS_5FenceEEEPNS_24SurfaceQueueBufferOutputE"
#define QUEUEBUFFER_SYMBOL_V4 \
    "_ZN7android7Surface11queueBufferERKNS_2spINS_13GraphicBufferEEERKNS_23SurfaceQueueBufferInputEPNS_24SurfaceQueueBufferOutputE"
#define MAX_POLICIES 8
#define MAX_GOV_PARAMS 20
#define MAX_SPECIAL_APPS 32
#define MAX_FAS_APPS 256
#define MAX_CLUSTERS 4
#define MAX_GOVERNORS 10
#define MAX_CPU_MODEL_LEN 32
#define MAX_LINE_LEN 512
#define MAX_PACKAGE_LEN 128
#define MAX_APP_NAME_LEN 256
#define MAX_FREQ_TABLE 128
#define MAX_EXTRA_WRITES 64

#define INOTIFY_EVENT_SIZE  (sizeof(struct inotify_event))
#define INOTIFY_BUF_LEN     (1024 * (INOTIFY_EVENT_SIZE + 16))

// 性能优化相关常量
#define APP_CHECK_INTERVAL_MS 500   // 应用检查间隔（毫秒）
#define SELECT_TIMEOUT_SEC 1        // select超时时间（秒）

typedef struct {
    int policy_num;
    int cluster_id;
    char governor[32];
    int max_freq;
    int min_freq;
    int hispeed_freq;
    int hispeed_load;
    int up_rate_limit_us;
    int down_rate_limit_us;
    int boost;
    int rtg_boost_freq;
    int target_load_shift;

    // 动态调速器参数
    int param_count;
    char param_names[MAX_GOV_PARAMS][32];
    char param_values[MAX_GOV_PARAMS][128];
    int hw_min_freq;
    /*
     * 游戏里是否把本档位的 min_freq 当作硬地板。
     *
     * 历史：默认（false）时，游戏里下限会被换成硬件最低档，因为 2026-09 实测
     * “档位 min_freq 在游戏里变硬地板”会比官方多烧约 0.6W（官方游戏配置 scaling_min 就是第 0 档）。
     * 但对高刷（165Hz）双大核场景，地板太低会让 CPU 占用率飙升、帧时间抖动变大，
     * 此时把地板抬起来反而更稳。因此做成配置项，由用户在 cpu 配置里按模式决定：
     *   "respect_min_freq_in_game": true
     */
    int respect_min_freq_in_game;
} CpuPolicy;

typedef struct {
    char background[16];
    char systembackground[16];
    char foreground[16];
    char topapp[16];
} CpusetSettings;

typedef struct {
    int min_freq;
    int max_freq;
    char min_expr[64];
    char max_expr[64];
} DdrSettings;

typedef struct {
    int min_freq;
    int max_freq;
    int high_level;
    int low_level;
} GpuSettings;

DynamicTuningParams g_dyn_params = {true};

typedef struct {
    int learning_duration_ms;
    int guard_interval_ms;
} SchedulerSessionConfig;

static SchedulerSessionConfig g_scheduler_session_config = {
    120 * 1000,
    1000
};

static SchedulerSessionConfig g_scheduler_session_config_base = {
    120 * 1000,
    1000
};

typedef struct {
    std::vector<std::string> main_thread_patterns;
    std::vector<std::string> render_thread_patterns;
    std::vector<std::string> worker_thread_patterns;
    std::vector<std::string> taskgraph_thread_patterns;
    std::vector<std::string> worker_exclude_patterns;
    std::vector<std::string> game_priority_thread_patterns;
    std::vector<std::string> single_big_core_package_patterns;
    std::vector<std::string> dual_big_core_package_patterns;
    std::vector<std::string> main_dual_big_package_patterns;
    std::vector<std::string> render_dual_big_package_patterns;
    std::vector<std::string> ue_game_package_patterns;
    float render_dual_big_min_usage_pct;
} ThreadSignatureConfig;

static ThreadSignatureConfig g_thread_signature_config = {};

enum ThreadJsonRoleKind {
    THREAD_JSON_ROLE_MAIN = 0,
    THREAD_JSON_ROLE_GFX,
    THREAD_JSON_ROLE_RENDER,
    THREAD_JSON_ROLE_WORKER,
    THREAD_JSON_ROLE_DOWNLOAD,
    THREAD_JSON_ROLE_OTHER,
    THREAD_JSON_ROLE_COUNT
};

enum ThreadJsonMatchKind {
    THREAD_JSON_MATCH_CONTAINS = 0,
    THREAD_JSON_MATCH_EXACT,
    THREAD_JSON_MATCH_PREFIX,
    THREAD_JSON_MATCH_GLOB
};

enum ThreadJsonSchedPolicyKind {
    THREAD_JSON_SCHED_INHERIT = -1,
    THREAD_JSON_SCHED_OTHER = 0,
    THREAD_JSON_SCHED_RR,
    THREAD_JSON_SCHED_FIFO
};

typedef struct {
    int match_kind;
    std::vector<std::string> patterns;
    int cpu_rank;
    int cpu_rank_start;
    int cpu_rank_end;
} ThreadJsonSelector;

typedef struct {
    bool has_cpus;
    std::vector<int> cpus;
    /*
     * 可选：显式指定登记进官方 pipeline 节点的核。
     * 不写（has_pipeline_cpu=false）时沿用原行为——取 cpus 掩码里最高的那个核。
     * 必须落在 cpus 掩码内，否则忽略并回退到最高核（见 mark_pipeline_task_for_rule_locked）。
     */
    bool has_pipeline_cpu;
    int pipeline_cpu;
    bool has_clusters;
    std::vector<std::string> clusters;
    bool has_nice;
    int nice;
    bool has_scheduler;
    int sched_policy;
    int sched_priority;
    bool fallback_to_other;
    bool reset_on_background;
} ThreadJsonAction;

typedef struct {
    bool enabled;
    std::vector<ThreadJsonSelector> selectors;
    int limit;
    bool loading_only;
    ThreadJsonAction action;
    /* Phase5: 可选稳定参数（0 = 使用全局默认） */
    int stable_hold_ms;                      /* 命中后保持多久不重选 */
    int rebind_cooldown_ms;                  /* 两次重绑最小间隔 */
    int selection_change_threshold_ms;       /* 排名变化持续多久才允许换人 */
    bool pin_once;                           /* 首次命中后固定，除非线程死亡 */
    bool prefer_existing_binding;            /* 已命中的线程优先保留，不轻易被新线程挤掉 */
} ThreadJsonRoleRule;

typedef struct {
    std::string name;
    ThreadJsonRoleRule rule;
} ThreadJsonCustomRule;

typedef struct {
    bool allow_rt;
    bool allow_fifo;
    int rt_max_threads;
    int rt_priority_min;
    int rt_priority_max;
    bool forbid_rt_roles[THREAD_JSON_ROLE_COUNT];
} ThreadJsonSafetyConfig;

typedef struct {
    std::string friendly;
    std::vector<std::string> packages;
    bool persistent_scope;
    bool has_feature_dynamic_tuning;
    bool feature_dynamic_tuning_enabled;
    bool has_feature_fas;
    bool feature_fas_enabled;
    bool has_feature_custom_thread;
    bool feature_custom_thread_enabled;
    bool has_feature_dynamic_thread_scheduler;
    bool feature_dynamic_thread_scheduler_enabled;
    bool has_session;
    SchedulerSessionConfig session;
    bool has_default_action;
    ThreadJsonAction default_action;
    ThreadJsonRoleRule roles[THREAD_JSON_ROLE_COUNT];
    std::vector<ThreadJsonCustomRule> custom_rules;
    ThreadJsonSafetyConfig safety;
} ThreadJsonAppRule;

static std::vector<ThreadJsonAppRule> g_thread_app_rules;
static int g_active_thread_app_rule_index = -1;

typedef struct {
    std::string friendly;
    std::string category;
    bool game;
    bool has_feature_dynamic_tuning;
    bool feature_dynamic_tuning_enabled;
    bool has_feature_fas;
    bool feature_fas_enabled;
    bool has_feature_custom_thread;
    bool feature_custom_thread_enabled;
    bool has_feature_dynamic_thread_scheduler;
    bool feature_dynamic_thread_scheduler_enabled;
    bool has_session;
    SchedulerSessionConfig session;
    std::vector<std::string> packages;
    std::vector<std::string> activities;
    std::vector<std::string> processes;
    bool has_default_action;
    ThreadJsonAction default_action;
} SceneCategoryRule;

static std::vector<SceneCategoryRule> g_scene_category_rules;

typedef struct {
    bool scheduler_master_enabled;
    bool dynamic_tuning_enabled;
    bool fas_enabled;
    bool custom_thread_enabled;
    bool dynamic_thread_scheduler_enabled;
    bool scene_category_enabled;
    bool base_profile_enabled;
} RuntimeFeatureSwitches;

static RuntimeFeatureSwitches g_mode_runtime_features = {true, true, true, true, true, true, true};

static FasConfig g_fas_config = {0};

// FAS output globals (written by new engine, read by dynamic_tuning/scheduler)
static std::atomic<int> g_cached_fas_guard_signal(0);
static std::atomic<int> g_cached_fas_current_fps(0);
static std::atomic<int> g_cached_fas_target_fps(0);
static std::atomic<int> g_cached_fas_valid_samples(0);
static std::atomic<int> g_cached_fas_frame_us(0);
static std::atomic<int> g_cached_fas_avg_frame_us(0);
static std::atomic<int> g_cached_fas_budget_us(0);
static std::atomic<int> g_cached_fas_debt_us(0);
static std::atomic<int> g_cached_fas_severity_us(0);
static std::atomic<int> g_cached_fas_action_level(FAS_ACTION_INVALID);
static std::atomic<int> g_cached_fas_over_budget_streak(0);
static std::atomic<int> g_cached_fas_buffer_usable(0);
static std::atomic<int> g_cached_fas_guard_active(0);
static std::atomic<int> g_cached_fas_guard_reason(0);
static long long g_fas_last_good_sample_ms = 0;

FasEngine g_fas_engine;
bool g_fas_engine_inited = false;

void reset_fas_prediction_cache() {
    g_cached_fas_current_fps.store(0); g_cached_fas_target_fps.store(0);
    g_cached_fas_valid_samples.store(0); g_cached_fas_frame_us.store(0); g_cached_fas_avg_frame_us.store(0);
    g_cached_fas_budget_us.store(0); g_cached_fas_debt_us.store(0); g_cached_fas_severity_us.store(0);
    g_cached_fas_action_level.store(FAS_ACTION_INVALID); g_cached_fas_over_budget_streak.store(0);
    g_cached_fas_buffer_usable.store(0);
    g_cached_fas_guard_active.store(0); g_cached_fas_guard_reason.store(0); g_fas_last_good_sample_ms = 0;
}
void reset_dynamic_tuning_state() {
    g_cached_fas_guard_signal.store(0); reset_fas_prediction_cache();
    extern int g_virtual_freq[]; for(int i=0;i<MAX_CLUSTERS;i++) g_virtual_freq[i]=0;
}

const char* describe_fas_buffer_state(int s) { return s ? "usable" : "unusable"; }
const char* describe_fas_guard_reason(int r) {
    switch(r) { case 1: return "mispredict"; case 2: return "tail_frame"; case 3: return "queuebuffer"; case 4: return "thread_pressure"; default: return "none"; }
}

enum ThreadClass {
    THREAD_CLASS_MAIN = 0,
    THREAD_CLASS_RENDER,
    THREAD_CLASS_BURST,
    THREAD_CLASS_BACKGROUND
};

typedef struct {
    int policy_idx;
    int cluster_id;
    int min_freq;
    int max_freq;
    float w_idle;
    float w_freq;
    float w_rq;
    float w_thermal;
    DynamicTuningParams dyn_params;
} ClusterPolicy;

typedef struct {
    int multi_window_active;
    int visible_app_count;
    int screen_off;
    int background_music;
    int camera_active;
    int scanner_active;
    int game_loading;
    int download_active;
    int mini_program_active;
    int official_conflict_active;
    long long camera_hold_until_ms;
    long long game_foreground_since_ms;
    long long game_loading_since_ms;
    long long game_loading_last_busy_ms;
    long long last_update_ms;
    char highest_mode[64];
    char foreground_process[128];
    char foreground_activity[192];
    char foreground_category[64];
} ScenePolicyState;


typedef struct {
    int top_boost;
    int sched_boost;
} SchedBoostSettings;

typedef struct {
    int downmigrate;
    int upmigrate;
    int group_downmigrate;
    int group_upmigrate;
} SchedConfigSettings;

typedef struct {
    int prefer_idle;
    int boost;
} StuneSettings;

//cpuctl相关数据结构
typedef struct {
    int uclamp_min;          // 0-100 (百分比)
    int uclamp_max;          // 0-100 (百分比)
    int latency_sensitive;   // 0或1
    int shares;              // 2-1024 (CPU份额)
} CpuCtlGroup;

typedef struct {
    CpuCtlGroup background;
    CpuCtlGroup systembackground;
    CpuCtlGroup foreground;
    CpuCtlGroup topapp;
} CpuCtlSettings;

typedef enum {
    PLATFORM_UNKNOWN = 0,
    PLATFORM_QUALCOMM,
    PLATFORM_MEDIATEK
} PlatformType;

PlatformType current_platform = PLATFORM_UNKNOWN;

typedef struct {
    int cluster_id;
    int policy_count;
    int policies[MAX_POLICIES];
    char related_cpus[64];
    int cpu_ids[32];
    int cpu_count;
} CpuCluster;

CpuPolicy cpu_policies[MAX_POLICIES];
CpuCluster cpu_clusters[MAX_CLUSTERS];
CpusetSettings cpuset;
DdrSettings ddr;
GpuSettings gpu;
SchedBoostSettings sched_boost_settings;
SchedConfigSettings sched_config_settings;
StuneSettings stune_settings;
CpuCtlSettings cpuctl; // 全局cpuctl设置
int policy_count = 0;

typedef struct {
    int count;
    int freqs[MAX_FREQ_TABLE];
} FreqTable;

typedef struct {
    uint64_t total;
    uint64_t idle;
} CpuJiffies;

static pthread_mutex_t g_app_mutex = PTHREAD_MUTEX_INITIALIZER;
static char g_current_foreground_app[MAX_APP_NAME_LEN] = "unknown";
static char g_current_mode[64] = "balance";
static char g_last_focus_app[MAX_APP_NAME_LEN] = "";
static int g_dynamic_tuning_active = 0;
static volatile sig_atomic_t g_keep_running = 1;
int g_virtual_freq[MAX_CLUSTERS] = {0};
static DynamicTuningParams g_cluster_dyn_params[MAX_CLUSTERS];
static ScenePolicyState g_scene_policy = {0};
static std::atomic<int> g_pause_game_scheduler(0);
static std::atomic<int> g_scheduler_skip_process_active(0);
static std::atomic<int> g_self_bypass_by_official_tuner(0);
static int g_official_tuner_mode = 0;
static int g_official_tuner_disabled = 0;
static int g_official_tuner_managed_by_module = 0;
static int g_official_tuner_restore_mask = 0;
static FreqTable g_freq_tables[MAX_POLICIES];

typedef struct {
    char path[256];
    char value[128];
} ExtraWrite;

static ExtraWrite g_extra_writes[MAX_EXTRA_WRITES];
static int g_extra_write_count = 0;

typedef struct {
    std::atomic<long long> loop_ticks;
    std::atomic<long long> dynamic_tuning_ticks;
    std::atomic<long long> freq_update_count;
    std::atomic<long long> affinity_apply_count;
    std::atomic<long long> affinity_fail_count;
    std::atomic<long long> guard_reapply_count;
    std::atomic<long long> boost_request_count;
    std::atomic<long long> apply_settings_count;
    std::atomic<long long> foreground_switch_count;
    /* 线程发现通道的计数：用来验收"事件驱动替代 /proc 轮询"是否真的生效。 */
    std::atomic<long long> thread_proc_scan_count;   /* /proc 全量枚举轮数 */
    std::atomic<long long> thread_event_fork_count;  /* fork/exec 事件入册次数 */
    std::atomic<long long> thread_event_exit_count;  /* 退出事件清册次数 */
    std::atomic<long long> thread_event_bind_count;  /* fork 时刻按角色落位次数 */
    std::atomic<long long> thread_event_unbind_count;/* fork 时刻解除继承掩码次数 */
    std::atomic<long long> guard_yield_count;        /* 锁定守护让位（放弃与外部互踩）次数 */
    std::atomic<long long> affinity_backoff_count;   /* 绑核无法收敛而退避的次数 */
} SchedulerRuntimeStats;

static SchedulerRuntimeStats g_runtime_stats = {};

// 函数声明
void log_message(const char* message);
void log_message_throttled(const char* key, long long interval_ms, const char* message);
static bool is_aux_log_enabled();
static void log_affinity_message(const char* message);
void log_fas_message(const char* message);
static void log_stats_message(const char* message);
bool get_foreground_app(char* out_app, size_t out_size);
void apply_settings();
void apply_cpuctl_settings(const CpuCtlSettings* settings);
void load_mode_settings_json(const char* mode);
std::string get_highest_mode_for_visible_apps();
bool is_screen_off();
bool is_background_music_playing();
bool is_camera_active();
bool is_game_loading();
static void get_visible_window_scene_state(const char* current_app, int* visible_count, int* multi_window_active);
static bool is_game_package_name(const char* package);
static bool is_launcher_package_name(const char* package);
static bool token_looks_like_package(const std::string& token);
static bool should_pause_our_game_scheduler();
static bool is_self_bypass_by_official_tuner_active();
static bool is_scheduler_skip_process_active();
static bool load_scheduler_skip_processes_config();
static bool refresh_scheduler_skip_process_state();
static void handle_scheduler_skip_process_transition();
static void update_official_game_interference_state(const char* current_app);
static void sync_official_tuner_for_game(const char* current_app);
static bool is_official_scheduler_governor_active();
static int get_policy_lowest_available_freq(int policy_idx);
static int get_policy_safe_floor_freq(int policy_idx, int khz_floor);
static void refresh_scene_policy_state(const char* current_app);
int lock_val(const char* path, const char* value);
int detect_cpu_model();
void detect_cpu_clusters();
const char* get_cpu_config_path();
const char* get_runtime_feature_config_path();
const char* get_scheduler_config_path();
const char* get_thread_signature_config_path();
static bool read_text_file(const char* path, char* buf, size_t size);
void set_governor_params(int policy_num, const char* governor);
int get_supported_governors(int policy_num, char governors[][32], int max_count);
int is_governor_supported(int policy_num, const char* governor);
void clear_log_if_needed_for(const char* path);
int remount_sysfs_rw();
void safe_strncpy(char* dest, const char* src, size_t dest_size);
int safe_snprintf(char* dest, size_t dest_size, const char* format, ...);
int safe_strcat(char* dest, size_t dest_size, const char* src);
int file_exists(const char* path);
void cleanup_resources(FILE* fp, char* buffer, cJSON* json);
int extract_chip_model(const char* input);
int is_numeric(const char* str);
void handle_inotify_events(int inotify_fd, int wd_general_config_dir, int wd_cpu_config_dir);
int initialize_application();
void run_main_loop();
char* handle_app_mode(const char* current_app);
int process_foreground_app(const char* current_app, char* last_app);
int lock_val_perm(const char* path, const char* value, mode_t pre_perm, mode_t post_perm);
void set_sched_boost(int top_boost, int sched_boost, mode_t pre_perm, mode_t post_perm);
void set_sched_config(int downmigrate, int upmigrate, int group_downmigrate, int group_upmigrate, mode_t pre_perm, mode_t post_perm);
void set_stune_topapp(int prefer_idle, int boost, mode_t pre_perm, mode_t post_perm);
void reset_dynamic_tuning_state();
void log_debug_message(const char* message);
void load_fas_config();
void load_scene_categories_config();
long long get_current_time_ms();
int is_fas_enabled_for_app(const char* package);
bool is_dynamic_tuning_enabled_for_app(const char* package);
bool is_custom_thread_enabled_for_app(const char* package);
bool is_dynamic_thread_scheduler_enabled_for_app(const char* package);
int get_app_current_fps(const char* package);
void reset_fas_prediction_cache();
static bool execute_command_trim(const char* cmd, char* out, size_t out_size);
static std::string trim_copy(const std::string& text);
static std::string scheduler_to_lower_copy(const char* text);

struct FasThreadFeatureSnapshot {
    float game_thread_usage;
    float render_thread_usage;
    float rhi_thread_usage;
    float taskgraph_thread_usage;
    float worker_top_usage;
    float worker_top_n_usage;
    float busy_thread_usage;
    float runnable_pressure;
    int worker_hot_count;
};

struct FasGpuFeatureSnapshot {
    int gpu_busy_pct;
    int gpu_freq_khz;
    int ddr_freq_khz;
    int gpu_pressure;
    int gpu_busy_valid;
    int gpu_freq_valid;
    int ddr_freq_valid;
    char gpu_busy_source[96];
    char gpu_freq_source[96];
    char ddr_freq_source[96];
};

namespace UnifiedScheduler {
    void initialize_cluster_policies();
    void reload_thread_config_now();
    void notify_foreground_app_changed(const char* package);
    void refresh_bpf_targets_now();
    void start_threads();
    void stop_threads();
    void run_scheduler_step();
    bool init_bpf();
    void cleanup_bpf();
    void request_performance_boost(int cluster_id, int duration_ms);
    float get_busy_thread_usage();
    void collect_fas_thread_features(const char* package, FasThreadFeatureSnapshot* out);
    bool has_active_download_thread();
    pid_t get_foreground_pid_snapshot();
    void describe_queuebuffer_state(const char* package, char* out, size_t out_size);
    int get_cluster_min_override(int cluster_id);
    int get_cluster_max_override(int cluster_id);
    void reset_cluster_freq_overrides();
    bool set_thread_affinity(pid_t tid, int target_cluster_id);
    bool set_thread_affinity_mask(pid_t tid, const std::vector<int>& cpus);
}

static int read_file_all(const char* path, char** out_buf, size_t* out_len) {
    if (!out_buf || !out_len) return 0;
    *out_buf = NULL;
    *out_len = 0;

    FILE* fp = fopen(path, "rb");
    if (!fp) return 0;

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return 0;
    }
    long len = ftell(fp);
    if (len <= 0) {
        fclose(fp);
        return 0;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return 0;
    }

    char* data = (char*)malloc((size_t)len + 1);
    if (!data) {
        fclose(fp);
        return 0;
    }
    size_t bytes_read = fread(data, 1, (size_t)len, fp);
    fclose(fp);
    data[bytes_read] = 0;
    *out_buf = data;
    *out_len = bytes_read;
    return 1;
}

static bool read_text_file(const char* path, char* buf, size_t size) {
    if (!path || !buf || size == 0) return false;

    FILE* fp = fopen(path, "r");
    if (!fp) return false;

    size_t bytes_read = fread(buf, 1, size - 1, fp);
    fclose(fp);

    if (bytes_read == 0) {
        buf[0] = '\0';
        return false;
    }

    buf[bytes_read] = '\0';
    return true;
}

#ifdef MURONG_THREAD_ONLY
#include "activity_fas_sampling_thread.inc"
#else
#include "activity_fas_sampling.inc"
#endif

#include "activity_command_util.inc"

static const ThreadJsonAppRule* find_thread_json_app_rule_global(const char* package) {
    if (!package || !package[0] || strcmp(package, "unknown") == 0) return NULL;
    std::string lowered_pkg = scheduler_to_lower_copy(package);
    if (lowered_pkg.empty()) return NULL;
    for (size_t i = 0; i < g_thread_app_rules.size(); i++) {
        const ThreadJsonAppRule& rule = g_thread_app_rules[i];
        for (size_t j = 0; j < rule.packages.size(); j++) {
            if (scheduler_to_lower_copy(rule.packages[j].c_str()) == lowered_pkg) {
                return &rule;
            }
        }
    }
    return NULL;
}

static const SceneCategoryRule* find_scene_category_rule_global(const char* package, const char* category) {
    if (!g_mode_runtime_features.scheduler_master_enabled ||
        !g_mode_runtime_features.scene_category_enabled) {
        return NULL;
    }
    if (category && category[0]) {
        for (size_t i = 0; i < g_scene_category_rules.size(); i++) {
            const SceneCategoryRule& rule = g_scene_category_rules[i];
            if (rule.category == category) {
                return &rule;
            }
        }
    }
    if (package && package[0] && strcmp(package, "unknown") != 0) {
        std::string lowered_pkg = scheduler_to_lower_copy(package);
        if (!lowered_pkg.empty()) {
            for (size_t i = 0; i < g_scene_category_rules.size(); i++) {
                const SceneCategoryRule& rule = g_scene_category_rules[i];
                for (size_t j = 0; j < rule.packages.size(); j++) {
                    if (scheduler_to_lower_copy(rule.packages[j].c_str()) == lowered_pkg) {
                        return &rule;
                    }
                }
            }
        }
    }
    return NULL;
}

static RuntimeFeatureSwitches normalize_runtime_feature_switches(RuntimeFeatureSwitches switches) {
    if (!switches.scheduler_master_enabled) {
        switches.dynamic_tuning_enabled = false;
        switches.fas_enabled = false;
        switches.custom_thread_enabled = false;
        switches.dynamic_thread_scheduler_enabled = false;
        switches.scene_category_enabled = false;
        switches.base_profile_enabled = false;
        return switches;
    }
    return switches;
}

static bool is_scheduler_control_panel_package(const char* package) {
    if (!package || !package[0]) return false;
    return strcmp(package, "com.murong.diaodu") == 0 ||
           strcmp(package, "com.murong.admin") == 0;
}

/*
 * 每应用的"生效开关"缓存。
 *
 * resolve_runtime_feature_switches_for_app() 会被"每条被跟踪线程 × 每拍"调用，
 * 而它内部要扫场景分类规则、线程规则（每条规则里还要把包名转小写比较），
 * 有的路径还会读一次 mode.txt —— simpleperf 上这里是 daemon 的头号热点，
 * 实测主线程常年烧 25%~33% 的核（游戏里线程更多会更高）。
 *
 * 开关只在"配置重载 / 档位切换 / 前台变化"时才会变，所以按包名缓存结果，
 * 上面这些时机清一次表即可。
 */
static std::unordered_map<std::string, RuntimeFeatureSwitches> g_feature_switch_cache;
static std::mutex g_feature_switch_cache_mutex;

static void invalidate_runtime_feature_switch_cache() {
    std::lock_guard<std::mutex> lock(g_feature_switch_cache_mutex);
    g_feature_switch_cache.clear();
}

bool is_runtime_scheduler_master_enabled() {
    return g_mode_runtime_features.scheduler_master_enabled;
}

bool is_scene_category_runtime_enabled() {
    return g_mode_runtime_features.scheduler_master_enabled &&
           g_mode_runtime_features.scene_category_enabled;
}

bool is_base_profile_runtime_enabled() {
    return g_mode_runtime_features.scheduler_master_enabled &&
           g_mode_runtime_features.base_profile_enabled;
}

static RuntimeFeatureSwitches compute_runtime_feature_switches_for_app(const char* package) {
    /*
     * 全局开关是"总闸"：每应用 / 场景分类规则只能把功能收窄，不能把用户在软件里
     * 关掉的开关重新打开。
     *
     * 之前的实现允许覆盖成 true，而 apply_builtin_thread_app_defaults() 解析
     * 6+2.json 时会把 custom_thread / dynamic_thread_scheduler 强制置为开启，
     * 结果就是：用户在界面上关掉动态线程和自定义线程，只要游戏在 6+2.json 里，
     * 就会被规则重新打开，线程永远不释放（实测 208 条线程停在 1-4）。
     */
    const RuntimeFeatureSwitches base = normalize_runtime_feature_switches(g_mode_runtime_features);
    RuntimeFeatureSwitches switches = base;

    auto clamp_to_base = [&base](RuntimeFeatureSwitches value) -> RuntimeFeatureSwitches {
        value.dynamic_tuning_enabled =
            value.dynamic_tuning_enabled && base.dynamic_tuning_enabled;
        value.fas_enabled = value.fas_enabled && base.fas_enabled;
        value.custom_thread_enabled =
            value.custom_thread_enabled && base.custom_thread_enabled;
        value.dynamic_thread_scheduler_enabled =
            value.dynamic_thread_scheduler_enabled && base.dynamic_thread_scheduler_enabled;
        value.scene_category_enabled =
            value.scene_category_enabled && base.scene_category_enabled;
        value.base_profile_enabled =
            value.base_profile_enabled && base.base_profile_enabled;
        return normalize_runtime_feature_switches(value);
    };

    if (!switches.scheduler_master_enabled) {
        return switches;
    }
    if (is_scheduler_control_panel_package(package)) {
        switches.dynamic_tuning_enabled = false;
        switches.fas_enabled = false;
        switches.custom_thread_enabled = false;
        switches.dynamic_thread_scheduler_enabled = false;
        return clamp_to_base(switches);
    }
    const SceneCategoryRule* category_rule = NULL;
    if (switches.scene_category_enabled) {
        category_rule = find_scene_category_rule_global(
            package,
            (package && g_current_foreground_app[0] && strcmp(package, g_current_foreground_app) == 0)
                ? g_scene_policy.foreground_category
                : NULL);
    }
    if (category_rule) {
        if (category_rule->has_feature_dynamic_tuning) {
            switches.dynamic_tuning_enabled = category_rule->feature_dynamic_tuning_enabled;
        }
        if (category_rule->has_feature_fas) {
            switches.fas_enabled = category_rule->feature_fas_enabled;
        }
        if (category_rule->has_feature_custom_thread) {
            switches.custom_thread_enabled = category_rule->feature_custom_thread_enabled;
        }
        if (category_rule->has_feature_dynamic_thread_scheduler) {
            switches.dynamic_thread_scheduler_enabled =
                category_rule->feature_dynamic_thread_scheduler_enabled;
        }
    }
    const ThreadJsonAppRule* app_rule = find_thread_json_app_rule_global(package);
    if (!app_rule) {
        return clamp_to_base(switches);
    }
    if (app_rule->has_feature_dynamic_tuning) {
        switches.dynamic_tuning_enabled = app_rule->feature_dynamic_tuning_enabled;
    }
    if (app_rule->has_feature_fas) {
        switches.fas_enabled = app_rule->feature_fas_enabled;
    }
    if (app_rule->has_feature_custom_thread) {
        switches.custom_thread_enabled = app_rule->feature_custom_thread_enabled;
    }
    if (app_rule->has_feature_dynamic_thread_scheduler) {
        switches.dynamic_thread_scheduler_enabled =
            app_rule->feature_dynamic_thread_scheduler_enabled;
    }
    return clamp_to_base(switches);
}

/* 带缓存的取用口：热路径（每线程每拍）走这里。 */
static RuntimeFeatureSwitches resolve_runtime_feature_switches_for_app(const char* package) {
    if (!package || !package[0]) {
        return normalize_runtime_feature_switches(g_mode_runtime_features);
    }
    std::string key(package);
    {
        std::lock_guard<std::mutex> lock(g_feature_switch_cache_mutex);
        auto it = g_feature_switch_cache.find(key);
        if (it != g_feature_switch_cache.end()) {
            return it->second;
        }
    }
    RuntimeFeatureSwitches resolved = compute_runtime_feature_switches_for_app(package);
    {
        std::lock_guard<std::mutex> lock(g_feature_switch_cache_mutex);
        if (g_feature_switch_cache.size() < 512) {
            g_feature_switch_cache[key] = resolved;
        }
    }
    return resolved;
}

bool is_dynamic_tuning_enabled_for_app(const char* package) {
    return resolve_runtime_feature_switches_for_app(package).dynamic_tuning_enabled;
}

int is_fas_enabled_for_app(const char* package) {
    if (!package || package[0] == '\0' || strcmp(package, "unknown") == 0) {
        return 0;
    }
    if (!resolve_runtime_feature_switches_for_app(package).fas_enabled) {
        return 0;
    }
    for (int i = 0; i < g_fas_config.app_count; i++) {
        if (strcmp(g_fas_config.apps[i].package, package) == 0) {
            return 1;
        }
    }
    return 0;
}

bool is_custom_thread_enabled_for_app(const char* package) {
    /* "unknown" = 真实前台是桌面/SystemUI/输入法这类不该调度的界面，
       此时必须关闭线程调度，让被管线程被释放掉。 */
    if (!package || !package[0] || strcmp(package, "unknown") == 0) return false;
    return resolve_runtime_feature_switches_for_app(package).custom_thread_enabled;
}

bool is_dynamic_thread_scheduler_enabled_for_app(const char* package) {
    if (!package || !package[0] || strcmp(package, "unknown") == 0) return false;
    return resolve_runtime_feature_switches_for_app(package).dynamic_thread_scheduler_enabled;
}

static const FasAppConfig* select_fas_profile(const char* package, int current_fps) {
    const FasAppConfig* best_match = NULL;
    const FasAppConfig* fallback_match = NULL;
    const FasAppConfig* forced_match = NULL;
    int best_diff = 2147483647;

    if (!is_fas_enabled_for_app(package)) {
        return NULL;
    }

    if (package && package[0] != '\0') {
        for (int i = 0; i < g_fas_config.app_count; i++) {
            if (strcmp(g_fas_config.apps[i].package, package) == 0) {
                const FasAppConfig* cfg = &g_fas_config.apps[i];

                if (cfg->fallback) {
                    if (!forced_match || cfg->target_fps > forced_match->target_fps) {
                        forced_match = cfg;
                    }
                    if (!fallback_match || cfg->target_fps > fallback_match->target_fps) {
                        fallback_match = cfg;
                    }
                } else {
                    if (!fallback_match || cfg->target_fps > fallback_match->target_fps) {
                        fallback_match = cfg;
                    }
                }

                if (current_fps > 0 && cfg->target_fps > 0) {
                    int diff = abs(cfg->target_fps - current_fps);
                    if (!best_match || diff < best_diff || (diff == best_diff && cfg->target_fps > best_match->target_fps)) {
                        best_match = cfg;
                        best_diff = diff;
                    }
                }
            }
        }
    }

    // 两阶段选档:
    // Phase 1 — 帧数据可信（current_fps > 0）: 用 best_match（精确匹配实际FPS）
    // Phase 2 — 帧数据不可信（预热/缺失）: 用 forced_match（最高fallback档）
    if (current_fps > 0 && best_match) {
        return best_match;
    }
    if (forced_match) {
        return forced_match;
    }
    if (fallback_match) {
        return fallback_match;
    }
    return best_match;
}



static int freq_table_load(int policy_num, FreqTable* table) {
    if (!table) return 0;
    table->count = 0;
    std::vector<int> merged_freqs;
    char path[256];
    safe_snprintf(path, sizeof(path),
        "/sys/devices/system/cpu/cpufreq/policy%d/scaling_available_frequencies",
        policy_num);
    char* data = NULL;
    size_t len = 0;
    if (!read_file_all(path, &data, &len)) {
        return 0;
    }

    int count = 0;
    char* saveptr = NULL;
    char* token = strtok_r(data, " \n\r\t", &saveptr);
    while (token) {
        int v = atoi(token);
        if (v > 0) {
            merged_freqs.push_back(v);
        }
        token = strtok_r(NULL, " \n\r\t", &saveptr);
    }
    free(data);

    safe_snprintf(path, sizeof(path),
        "/sys/devices/system/cpu/cpufreq/policy%d/scaling_boost_frequencies",
        policy_num);
    data = NULL;
    len = 0;
    if (read_file_all(path, &data, &len) && data) {
        saveptr = NULL;
        token = strtok_r(data, " \n\r\t", &saveptr);
        while (token) {
            int v = atoi(token);
            if (v > 0) {
                merged_freqs.push_back(v);
            }
            token = strtok_r(NULL, " \n\r\t", &saveptr);
        }
        free(data);
    }

    std::sort(merged_freqs.begin(), merged_freqs.end());
    merged_freqs.erase(std::unique(merged_freqs.begin(), merged_freqs.end()), merged_freqs.end());
    count = 0;
    for (size_t i = 0; i < merged_freqs.size() && count < MAX_FREQ_TABLE; i++) {
        table->freqs[count++] = merged_freqs[i];
    }
    table->count = count;
    return count > 0 ? 1 : 0;
}







int cluster_count = 0;
int ddr_max = 0;
int ddr_min = 0;
char cpu_model[MAX_CPU_MODEL_LEN] = "unknown";
char sched_path[128] = "/proc/sys/kernel";

// 锁值函数（支持权限设置）
int lock_val_perm(const char* path, const char* value, mode_t pre_perm, mode_t post_perm) {
    if (!file_exists(path)) return 0;
    (void)pre_perm;
    (void)post_perm;
    return lock_val(path, value);
}

// 比较函数：升序
int cmp_long_asc(const void* a, const void* b) {
    long arg1 = *(const long*)a;
    long arg2 = *(const long*)b;
    if (arg1 < arg2) return -1;
    if (arg1 > arg2) return 1;
    return 0;
}

// 比较函数：降序
int cmp_long_desc(const void* a, const void* b) {
    long arg1 = *(const long*)a;
    long arg2 = *(const long*)b;
    if (arg1 < arg2) return 1;
    if (arg1 > arg2) return -1;
    return 0;
}

// 初始化平台信息
void init_scheduler_info() {
    if (file_exists("/proc/sys/walt")) {
        safe_strncpy(sched_path, "/proc/sys/walt", sizeof(sched_path));
        log_message("检测到WALT调度器");
    }
}

// 调度器加速设置
void set_sched_boost(int top_boost, int sched_boost, mode_t pre_perm = 0, mode_t post_perm = 0) {
    char path[256];
    char val_str[32];

    safe_snprintf(path, sizeof(path), "%s/sched_boost_top_app", sched_path);
    safe_snprintf(val_str, sizeof(val_str), "%d", top_boost);
    lock_val_perm(path, val_str, pre_perm, post_perm);

    safe_snprintf(path, sizeof(path), "%s/sched_boost", sched_path);
    safe_snprintf(val_str, sizeof(val_str), "%d", sched_boost);
    lock_val_perm(path, val_str, pre_perm, post_perm);
}

// 调度器配置
void set_sched_config(int downmigrate, int upmigrate, int group_downmigrate, int group_upmigrate, mode_t pre_perm = 0, mode_t post_perm = 0) {
    char path[256];
    char val_str[32];

    safe_snprintf(path, sizeof(path), "%s/sched_downmigrate", sched_path);
    safe_snprintf(val_str, sizeof(val_str), "%d", downmigrate);
    lock_val_perm(path, val_str, pre_perm, post_perm);

    safe_snprintf(path, sizeof(path), "%s/sched_upmigrate", sched_path);
    safe_snprintf(val_str, sizeof(val_str), "%d", upmigrate);
    lock_val_perm(path, val_str, pre_perm, post_perm);

    safe_snprintf(path, sizeof(path), "%s/sched_group_downmigrate", sched_path);
    safe_snprintf(val_str, sizeof(val_str), "%d", group_downmigrate);
    lock_val_perm(path, val_str, pre_perm, post_perm);

    safe_snprintf(path, sizeof(path), "%s/sched_group_upmigrate", sched_path);
    safe_snprintf(val_str, sizeof(val_str), "%d", group_upmigrate);
    lock_val_perm(path, val_str, pre_perm, post_perm);
}

// Stune配置
void set_stune_topapp(int prefer_idle, int boost, mode_t pre_perm = 0, mode_t post_perm = 0) {
    char val_str[32];
    const char* base_path = NULL;

    if (file_exists("/dev/stune/top-app")) {
        base_path = "/dev/stune/top-app";
    } else if (file_exists("/dev/cpuset/top-app")) {
        base_path = "/dev/cpuset/top-app";
    }

    if (base_path) {
        char path[256];
        safe_snprintf(path, sizeof(path), "%s/schedtune.prefer_idle", base_path);
        safe_snprintf(val_str, sizeof(val_str), "%d", prefer_idle);
        lock_val_perm(path, val_str, pre_perm, post_perm);

        safe_snprintf(path, sizeof(path), "%s/schedtune.boost", base_path);
        safe_snprintf(val_str, sizeof(val_str), "%d", boost);
        lock_val_perm(path, val_str, pre_perm, post_perm);
    }
}



// 安全的字符串复制
void safe_strncpy(char* dest, const char* src, size_t dest_size) {
    if (dest_size == 0) return;
    if (src) {
        strncpy(dest, src, dest_size - 1);
        dest[dest_size - 1] = '\0';
    }
    else {
        dest[0] = '\0';
    }
}

// 安全的字符串格式化
int safe_snprintf(char* dest, size_t dest_size, const char* format, ...) {
    if (!dest || dest_size == 0) return -1;
    
    va_list args;
    va_start(args, format);
    int result = vsnprintf(dest, dest_size, format, args);
    va_end(args);
    
    // 确保字符串以null结尾
    dest[dest_size - 1] = '\0';
    
    return result;
}

// 安全的字符串连接函数
int safe_strcat(char* dest, size_t dest_size, const char* src) {
    if (!dest || !src || dest_size == 0) return -1;
    
    size_t dest_len = strlen(dest);
    size_t src_len = strlen(src);
    
    // 检查是否有足够空间（包括null终止符）
    if (dest_len + src_len >= dest_size) {
        // 只复制能放下的部分
        size_t available = dest_size - dest_len - 1;
        if (available > 0) {
            strncpy(dest + dest_len, src, available);
            dest[dest_size - 1] = '\0';
        }
        return -1; // 表示截断
    }
    
    // 安全连接
    strcpy(dest + dest_len, src);
    return 0;
}

// 检查文件是否存在
int file_exists(const char* path) {
    return access(path, F_OK) == 0;
}

// 通用资源清理函数
void cleanup_resources(FILE* fp, char* buffer, cJSON* json) {
    if (fp) {
        fclose(fp);
    }
    if (buffer) {
        free(buffer);
    }
    if (json) {
        cJSON_Delete(json);
    }
}

#include "activity_runtime_logging.inc"

#include "activity_sysfs_runtime.inc"

// 检测CPU簇
void detect_cpu_clusters() {
    policy_count = 0;
    cluster_count = 0;

    // 1. 收集所有存在的策略
    for (int i = 0; i < MAX_POLICIES; i++) {
        char path[256];
        safe_snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpufreq/policy%d", i);
        if (file_exists(path)) {
            cpu_policies[policy_count].policy_num = i;
            cpu_policies[policy_count].cluster_id = -1; // 未分配簇
            policy_count++;
        }
    }

    // 2. 为每个策略读取related_cpus并分组
    for (int i = 0; i < policy_count; i++) {
        int policy_num = cpu_policies[i].policy_num;
        char path[256];
        safe_snprintf(path, sizeof(path),
            "/sys/devices/system/cpu/cpufreq/policy%d/related_cpus",
            policy_num);

        if (!file_exists(path)) continue;

        FILE* fp = fopen(path, "r");
        if (!fp) continue;

        char line[256];
        if (fgets(line, sizeof(line), fp)) {
            // 移除换行符
            line[strcspn(line, "\n")] = '\0';

            // 尝试匹配现有簇
            int found_cluster = -1;
            for (int j = 0; j < cluster_count; j++) {
                if (strcmp(cpu_clusters[j].related_cpus, line) == 0) {
                    found_cluster = j;
                    break;
                }
            }

            if (found_cluster != -1) {
                // 添加到现有簇
                cpu_clusters[found_cluster].policies[cpu_clusters[found_cluster].policy_count++] = policy_num;
                cpu_policies[i].cluster_id = found_cluster;
            }
            else if (cluster_count < MAX_CLUSTERS) {
                // 创建新簇
                CpuCluster* cluster = &cpu_clusters[cluster_count];
                cluster->cluster_id = cluster_count;
                cluster->policy_count = 1;
                cluster->policies[0] = policy_num;
                strncpy(cluster->related_cpus, line, sizeof(cluster->related_cpus) - 1);
                cluster->related_cpus[sizeof(cluster->related_cpus) - 1] = '\0';
                
                // 预解析 related_cpus 到 cpu_ids 数组
                cluster->cpu_count = 0;
                char* p = cluster->related_cpus;
                while (*p) {
                    while (*p && !isdigit(*p)) p++;
                    if (!*p) break;
                    int start = atoi(p);
                    while (*p && isdigit(*p)) p++;
                    if (*p == '-') {
                        p++;
                        int end = atoi(p);
                        for (int k = start; k <= end && cluster->cpu_count < 32; k++) {
                            cluster->cpu_ids[cluster->cpu_count++] = k;
                        }
                        while (*p && isdigit(*p)) p++;
                    } else {
                        if (cluster->cpu_count < 32) cluster->cpu_ids[cluster->cpu_count++] = start;
                    }
                }

                cpu_policies[i].cluster_id = cluster_count;
                cluster_count++;
            }
        }
        fclose(fp);
    }

    // 3. 记录簇信息到日志
    char log_buf[1024] = "CPU集群检测结果:\n";
    for (int i = 0; i < cluster_count; i++) {
        char cluster_info[256];
        safe_snprintf(cluster_info, sizeof(cluster_info), "Cluster %d (CPUs: %s, 策略: [",
            i, cpu_clusters[i].related_cpus);

        for (int j = 0; j < cpu_clusters[i].policy_count; j++) {
            char policy_str[16];
            safe_snprintf(policy_str, sizeof(policy_str), "%d", cpu_clusters[i].policies[j]);
            safe_strcat(cluster_info, sizeof(cluster_info), policy_str);
            if (j < cpu_clusters[i].policy_count - 1) {
                safe_strcat(cluster_info, sizeof(cluster_info), ", ");
            }
        }
        safe_strcat(cluster_info, sizeof(cluster_info), "])\n");

        safe_strcat(log_buf, sizeof(log_buf), cluster_info);
    }
    log_message(log_buf);
}
#include "activity_gpu_ddr.inc"

// 获取前台应用
#include "activity_scene_foreground.inc"

#include "activity_cpu_settings.inc"

static void clear_thread_signature_config() {
    g_thread_signature_config.main_thread_patterns.clear();
    g_thread_signature_config.render_thread_patterns.clear();
    g_thread_signature_config.worker_thread_patterns.clear();
    g_thread_signature_config.taskgraph_thread_patterns.clear();
    g_thread_signature_config.worker_exclude_patterns.clear();
    g_thread_signature_config.game_priority_thread_patterns.clear();
    g_thread_signature_config.single_big_core_package_patterns.clear();
    g_thread_signature_config.dual_big_core_package_patterns.clear();
    g_thread_signature_config.main_dual_big_package_patterns.clear();
    g_thread_signature_config.render_dual_big_package_patterns.clear();
    g_thread_signature_config.ue_game_package_patterns.clear();
    g_thread_signature_config.render_dual_big_min_usage_pct = 8.0f;
    g_thread_app_rules.clear();
    g_active_thread_app_rule_index = -1;
}

static void load_string_array_config(cJSON* parent, const char* key, std::vector<std::string>& out) {
    if (!parent || !key) return;
    cJSON* arr = cJSON_GetObjectItem(parent, key);
    if (!arr || !cJSON_IsArray(arr)) return;

    out.clear();
    cJSON* item = NULL;
    cJSON_ArrayForEach(item, arr) {
        if (!cJSON_IsString(item) || !item->valuestring || !item->valuestring[0]) continue;
        std::string value = item->valuestring;
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return (char)tolower(c); });
        value = trim_copy(value);
        if (value.empty()) continue;
        out.push_back(value);
    }
}

static void load_float_config(cJSON* parent, const char* key, float* out_value, float min_value) {
    if (!parent || !key || !out_value) return;
    cJSON* item = cJSON_GetObjectItem(parent, key);
    if (!item || !cJSON_IsNumber(item)) return;
    if ((float)item->valuedouble < min_value) return;
    *out_value = (float)item->valuedouble;
}

static std::string scheduler_to_lower_copy(const char* text) {
    std::string out = text ? text : "";
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return (char)tolower(c); });
    return out;
}

#include "activity_skip_processes.inc"

#include "activity_config_loading.inc"

#ifdef MURONG_THREAD_ONLY
#include "activity_dyn_config_thread.inc"
#else
#include "activity_dyn_config.inc"
#endif

#include "activity_mode_watch.inc"

// 处理应用模式的函数
char* handle_app_mode(const char* current_app) {
    // 读取模式文件
    FILE* mode_file = fopen(MODE_FILE, "r");
    if (!mode_file) {
        log_message("未找到模式文件");
        return NULL;
    }

    char* mode = (char*)malloc(64);
    if (!mode) {
        fclose(mode_file);
        return NULL;
    }
    
    strcpy(mode, "powersave"); // 默认模式
    char line[256];

    // 读取默认模式
    if (fgets(line, sizeof(line), mode_file)) {
        line[strcspn(line, "\n")] = 0;
        safe_strncpy(mode, line, 64);
    }

    // 检查是否有应用特定的模式
    while (fgets(line, sizeof(line), mode_file)) {
        line[strcspn(line, "\n")] = 0;
        char* saveptr = NULL;
        char* package = strtok_r(line, " ", &saveptr);
        char* custom_mode = strtok_r(NULL, " ", &saveptr);

        if (package && custom_mode && strcmp(package, current_app) == 0) {
            safe_strncpy(mode, custom_mode, 64);
            break;
        }
    }
    
    fclose(mode_file);
    return mode;
}

static int get_mode_rank(const char* mode) {
    if (!mode) return 0;
    if (strcmp(mode, "powersave") == 0) return 0;
    if (strcmp(mode, "balance") == 0) return 1;
    if (strcmp(mode, "performance") == 0) return 2;
    if (strcmp(mode, "fast") == 0) return 3;
    return 1;
}

static bool is_known_mode(const char* mode) {
    return mode &&
        (strcmp(mode, "powersave") == 0 ||
         strcmp(mode, "balance") == 0 ||
         strcmp(mode, "performance") == 0 ||
         strcmp(mode, "fast") == 0);
}

static void get_mode_for_package(const char* package, char* out_mode, size_t out_size) {
    if (!out_mode || out_size == 0) return;
    safe_strncpy(out_mode, "powersave", out_size);
    FILE* mode_file = fopen(MODE_FILE, "r");
    if (!mode_file) {
        return;
    }

    char line[256];
    if (fgets(line, sizeof(line), mode_file)) {
        line[strcspn(line, "\n")] = 0;
        if (is_known_mode(line)) {
            safe_strncpy(out_mode, line, out_size);
        }
    }

    if (package && package[0]) {
        while (fgets(line, sizeof(line), mode_file)) {
            line[strcspn(line, "\n")] = 0;
            char* saveptr = NULL;
            char* pkg = strtok_r(line, " ", &saveptr);
            char* custom_mode = strtok_r(NULL, " ", &saveptr);
            if (pkg && custom_mode && strcmp(pkg, package) == 0 && is_known_mode(custom_mode)) {
                safe_strncpy(out_mode, custom_mode, out_size);
                break;
            }
        }
    }
    fclose(mode_file);
}

static bool is_launcher_package_name(const char* package) {
    if (!package || !package[0]) return false;
    return strcmp(package, "com.android.launcher") == 0 ||
           strcmp(package, "com.oplus.launcher") == 0 ||
           strcmp(package, "com.miui.home") == 0 ||
           strstr(package, "launcher") != NULL;
}

 #include "activity_scene_policy.inc"

static void run_shell_quiet(const char* cmd) {
    if (!cmd || !cmd[0]) return;
    system(cmd);
}

enum OfficialTunerMode {
    OFFICIAL_TUNER_NONE = 0,
    OFFICIAL_TUNER_XIAOMI = 1,
    OFFICIAL_TUNER_VIVO = 2,
    OFFICIAL_TUNER_OPPO = 3
};

enum OfficialTunerRestoreMask {
    OFFICIAL_TUNER_RESTORE_NONE = 0,
    OFFICIAL_TUNER_RESTORE_XIAOMI_SMARTOP = 1 << 0,
    OFFICIAL_TUNER_RESTORE_XIAOMI_JOB = 1 << 1,
    OFFICIAL_TUNER_RESTORE_VIVO_PACKAGE = 1 << 2,
    OFFICIAL_TUNER_RESTORE_OPPO_OIFACE = 1 << 3
};

static int detect_official_tuner_mode_from_device_props() {
    static int cached_mode = -1;
    if (cached_mode >= 0) {
        return cached_mode;
    }

    const char* prop_cmds[] = {
        "getprop ro.product.manufacturer",
        "getprop ro.product.brand",
        "getprop ro.product.system.brand",
        "getprop ro.vendor.product.brand"
    };

    std::string merged;
    char value[128];
    for (size_t i = 0; i < sizeof(prop_cmds) / sizeof(prop_cmds[0]); i++) {
        if (!execute_command_trim(prop_cmds[i], value, sizeof(value))) {
            continue;
        }
        std::string lowered = scheduler_to_lower_copy(value);
        lowered = trim_copy(lowered);
        if (lowered.empty()) {
            continue;
        }
        if (!merged.empty()) {
            merged += ' ';
        }
        merged += lowered;
    }

    if (merged.find("xiaomi") != std::string::npos ||
        merged.find("redmi") != std::string::npos ||
        merged.find("poco") != std::string::npos) {
        cached_mode = OFFICIAL_TUNER_XIAOMI;
    } else if (merged.find("vivo") != std::string::npos ||
               merged.find("iqoo") != std::string::npos) {
        cached_mode = OFFICIAL_TUNER_VIVO;
    } else if (merged.find("oppo") != std::string::npos ||
               merged.find("oneplus") != std::string::npos ||
               merged.find("realme") != std::string::npos ||
               merged.find("oplus") != std::string::npos) {
        cached_mode = OFFICIAL_TUNER_OPPO;
    } else {
        cached_mode = OFFICIAL_TUNER_NONE;
    }

    return cached_mode;
}

static int detect_official_tuner_mode() {
    return detect_official_tuner_mode_from_device_props();
}

#include "activity_tuner_bypass.inc"

// 处理前台应用的函数
int process_foreground_app(const char* current_app, char* last_app) {
    bool app_changed = strcmp(current_app, "unknown") != 0 && strcmp(current_app, last_app) != 0;
    char resolved_mode[64];
    get_mode_for_package(current_app, resolved_mode, sizeof(resolved_mode));
    bool mode_changed = strcmp(resolved_mode, g_current_mode) != 0;

    if (app_changed) {
        g_runtime_stats.foreground_switch_count.fetch_add(1);
        safe_strncpy(last_app, current_app, MAX_APP_NAME_LEN);
        safe_strncpy(g_last_focus_app, current_app, sizeof(g_last_focus_app));
    }

    if (app_changed || mode_changed) {
        char mode[64];
        get_mode_for_package(current_app, mode, sizeof(mode));
        safe_strncpy(g_current_mode, mode, sizeof(g_current_mode));

        load_mode_settings_json(mode);
        UnifiedScheduler::initialize_cluster_policies();
        g_dynamic_tuning_active = 0;
        reset_dynamic_tuning_state();
        apply_settings();

        g_dynamic_tuning_active = 0;
        UnifiedScheduler::notify_foreground_app_changed(current_app);
        update_official_game_interference_state(current_app);
        UnifiedScheduler::refresh_bpf_targets_now();

        if (is_aux_log_enabled()) {
            char log_msg[512];
            safe_snprintf(log_msg, sizeof(log_msg),
                "应用: %s | 模式: %s | 设置已应用",
                current_app, mode);
            log_debug_message(log_msg);
            RuntimeFeatureSwitches app_features = resolve_runtime_feature_switches_for_app(current_app);
            safe_snprintf(log_msg, sizeof(log_msg),
                          "应用功能开关 | app:%s | master:%d | dynamic_tuning:%d | fas:%d | custom_thread:%d | dynamic_thread:%d | scene:%d | base:%d",
                          current_app,
                          app_features.scheduler_master_enabled ? 1 : 0,
                          app_features.dynamic_tuning_enabled ? 1 : 0,
                          app_features.fas_enabled ? 1 : 0,
                          app_features.custom_thread_enabled ? 1 : 0,
                          app_features.dynamic_thread_scheduler_enabled ? 1 : 0,
                          app_features.scene_category_enabled ? 1 : 0,
                          app_features.base_profile_enabled ? 1 : 0);
            log_debug_message(log_msg);
        }
    }
    
    return 1; // 成功处理
}

namespace UnifiedScheduler {
#include "activity_scheduler_internal.inc"
#include "activity_scheduler_runtime.inc"
}  // namespace UnifiedScheduler

/*
 * 频率档位工具 + 场景策略刷新：两个版本共用。
 * 这些函数原先住在 activity_dynamic_tuning.inc 里，但调用者（scheduler_runtime /
 * scene_policy / main_loop）在纯线程版依然存在，故抽出来共用。
 */
#include "activity_freq_utils.inc"

/*
 * 纯线程版不编动态调频执行器：频率由 <SoC>.json 的 min/max 走廊 + apply_settings() 决定
 * （activity_scheduler_runtime.inc:688 的 should_apply_freq_update 在 dyn/FAS 双关时为 true）。
 */
#ifndef MURONG_THREAD_ONLY
#include "activity_dynamic_tuning.inc"
#endif

#include "activity_main_loop.inc"
