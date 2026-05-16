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

extern "C" {
struct bpf_program* bpf_object__next_program(const struct bpf_object* obj, struct bpf_program* prev);
const char* bpf_program__name(const struct bpf_program* prog);
const char* bpf_program__section_name(const struct bpf_program* prog);
struct ring_buffer* ring_buffer__new(int map_fd,
                                     ring_buffer_sample_fn sample_cb,
                                     void* ctx,
                                     const struct ring_buffer_opts* opts);
int ring_buffer__poll(struct ring_buffer* rb, int timeout_ms);
void ring_buffer__free(struct ring_buffer* rb);
}

#define LOG_FILE "/data/adb/modules/muronggameopt/config/log.txt"
#define DEBUG_LOG_FILE "/data/adb/modules/muronggameopt/config/debug_log.txt"
#define TRACE_LOG_FILE "/data/adb/modules/muronggameopt/config/trace_log.txt"
#define AFFINITY_LOG_FILE "/data/adb/modules/muronggameopt/config/affinity_log.txt"
#define STATS_LOG_FILE "/data/adb/modules/muronggameopt/config/stats_log.txt"
#define LOG_BACKUP_DIR "/data/adb/modules/muronggameopt/config/log_backups"
#define MAX_LOG_FILE_SIZE (200 * 1024)
#define MAX_AUX_LOG_FILE_SIZE (5 * 1024 * 1024) // 5MB for debug and stats logs
#define CONFIG_DIR "/data/adb/modules/muronggameopt/config/"
#define SCENE_CATEGORIES_FILE CONFIG_DIR "categories.json"
#define MODE_FILE "/data/adb/modules/muronggameopt/config/mode.txt"
#define CPU_CONFIG_DIR "/data/adb/modules/muronggameopt/bin/cpu/"
#define BPF_OBJECT_FILE "/data/adb/modules/muronggameopt/bin/monitor.bpf.o"
#define MAX_POLICIES 8
#define MAX_GOV_PARAMS 20
#define MAX_SPECIAL_APPS 32
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

    // 调速器参数
    int param_count;
    char param_names[MAX_GOV_PARAMS][32];
    char param_values[MAX_GOV_PARAMS][128];
    int hw_min_freq;
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
} DdrSettings;

DynamicTuningParams g_dyn_params = {true, 85, 5, false, 0, {}};

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
    bool bpf_thread_enabled;
    bool scene_category_enabled;
    bool base_profile_enabled;
} RuntimeFeatureSwitches;

static RuntimeFeatureSwitches g_mode_runtime_features = {true, true, true, true};

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
    int target_load;
    int load_margin;
    int render_min_boost_freq;
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
static volatile sig_atomic_t g_keep_running = 1;
static DynamicTuningParams g_cluster_dyn_params[MAX_CLUSTERS];
static ScenePolicyState g_scene_policy = {0};
static std::atomic<int> g_pause_game_scheduler(0);
static int g_official_tuner_mode = 0;
static int g_official_tuner_disabled = 0;
static int g_official_tuner_managed_by_module = 0;
static FreqTable g_freq_tables[MAX_POLICIES];

typedef struct {
    char path[256];
    char value[128];
} ExtraWrite;

static ExtraWrite g_extra_writes[MAX_EXTRA_WRITES];
static int g_extra_write_count = 0;

typedef struct {
    std::atomic<long long> loop_ticks;
    std::atomic<long long> policy_eval_ticks;
    std::atomic<long long> freq_update_count;
    std::atomic<long long> affinity_apply_count;
    std::atomic<long long> affinity_fail_count;
    std::atomic<long long> guard_reapply_count;
    std::atomic<long long> boost_request_count;
    std::atomic<long long> apply_settings_count;
    std::atomic<long long> foreground_switch_count;
} SchedulerRuntimeStats;

static SchedulerRuntimeStats g_runtime_stats = {};

// 函数声明
void log_message(const char* message);
static void log_affinity_message(const char* message);
static void log_stats_message(const char* message);
char* get_foreground_app();
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
static const char* classify_scene_category_from_config(const char* package,
                                                       const char* process_name,
                                                       const char* activity_name);
static const char* classify_scene_category(const char* package,
                                           const char* process_name,
                                           const char* activity_name);
static const char* get_cached_foreground_activity_name();
static bool should_pause_our_game_scheduler();
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
const char* get_thread_signature_config_path();
void set_governor_params(int policy_num, const char* governor);
int get_supported_governors(int policy_num, char governors[][32], int max_count);
int is_governor_supported(int policy_num, const char* governor);
void clear_log_if_needed_for(const char* path);
void set_policy_file_permissions();
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
void log_debug_message(const char* message);
void load_scene_categories_config();
static long long get_current_time_ms();
bool is_bpf_thread_enabled_for_app(const char* package);
static bool execute_command_all(const char* cmd, std::string& out);
static std::string trim_copy(const std::string& text);
static std::string scheduler_to_lower_copy(const char* text);
namespace UnifiedScheduler {
    void initialize_cluster_policies();
    void reload_thread_config_now();
    void notify_foreground_app_changed(const char* package);
    void refresh_bpf_targets_now();
    void start_threads();
    void stop_threads();
    void run_scheduler_step();
    void adjust_policy_by_fps();
    bool init_bpf();
    void cleanup_bpf();
    void request_performance_boost(int cluster_id, int duration_ms);
    float get_busy_thread_usage();
    bool has_active_download_thread();
    pid_t get_foreground_pid_snapshot();
    void describe_frame_probe_state(const char* package, char* out, size_t out_size);
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
        switches.bpf_thread_enabled = false;
        switches.scene_category_enabled = false;
        switches.base_profile_enabled = false;
    }
    return switches;
}

static bool is_scheduler_control_panel_package(const char* package) {
    if (!package || !package[0]) return false;
    return strcmp(package, "com.murong.diaodu") == 0 ||
           strcmp(package, "com.murong.admin") == 0;
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

static RuntimeFeatureSwitches resolve_runtime_feature_switches_for_app(const char* package) {
    RuntimeFeatureSwitches switches = normalize_runtime_feature_switches(g_mode_runtime_features);
    if (!switches.scheduler_master_enabled) {
        return switches;
    }
    if (is_scheduler_control_panel_package(package)) {
        switches.bpf_thread_enabled = false;
        return normalize_runtime_feature_switches(switches);
    }
    return normalize_runtime_feature_switches(switches);
}

bool is_bpf_thread_enabled_for_app(const char* package) {
    return resolve_runtime_feature_switches_for_app(package).bpf_thread_enabled;
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

    // 修改前权限
    if (pre_perm != 0) {
        chmod(path, pre_perm);
    }

    // 写入值
    int ret = lock_val(path, value);

    // 修改后权限
    if (post_perm != 0) {
        chmod(path, post_perm);
    }

    return ret;
}

static bool execute_command_all(const char* cmd, std::string& out) {
    out.clear();
    if (!cmd || !cmd[0]) {
        return false;
    }
    FILE* pipe = popen(cmd, "r");
    if (!pipe) {
        return false;
    }
    char buffer[512];
    while (fgets(buffer, sizeof(buffer), pipe)) {
        out.append(buffer);
    }
    int status = pclose(pipe);
    return status == 0;
}

static std::string trim_copy(const std::string& text) {
    size_t start = 0;
    while (start < text.size() && isspace((unsigned char)text[start])) {
        start++;
    }
    size_t end = text.size();
    while (end > start && isspace((unsigned char)text[end - 1])) {
        end--;
    }
    return text.substr(start, end - start);
}

static int get_policy_lowest_available_freq(int policy_idx) {
    if (policy_idx < 0 || policy_idx >= policy_count) {
        return 0;
    }
    int policy_num = cpu_policies[policy_idx].policy_num;
    if (policy_num < 0 || policy_num >= MAX_POLICIES) {
        return std::max(0, cpu_policies[policy_idx].min_freq);
    }
    FreqTable* table = &g_freq_tables[policy_num];
    if (table->count <= 0) {
        return std::max(0, cpu_policies[policy_idx].min_freq);
    }
    int lowest = table->freqs[0];
    for (int i = 1; i < table->count; i++) {
        if (table->freqs[i] > 0 && (lowest <= 0 || table->freqs[i] < lowest)) {
            lowest = table->freqs[i];
        }
    }
    return lowest > 0 ? lowest : std::max(0, cpu_policies[policy_idx].min_freq);
}

static int get_policy_safe_floor_freq(int policy_idx, int khz_floor) {
    if (policy_idx < 0 || policy_idx >= policy_count) {
        return std::max(0, khz_floor);
    }
    int policy_num = cpu_policies[policy_idx].policy_num;
    if (policy_num < 0 || policy_num >= MAX_POLICIES || g_freq_tables[policy_num].count <= 0) {
        return std::max(get_policy_lowest_available_freq(policy_idx), khz_floor);
    }
    FreqTable* table = &g_freq_tables[policy_num];
    int candidate = 0;
    for (int i = 0; i < table->count; i++) {
        int freq = table->freqs[i];
        if (freq <= 0) continue;
        if (freq >= khz_floor && (candidate == 0 || freq < candidate)) {
            candidate = freq;
        }
    }
    if (candidate > 0) {
        return candidate;
    }
    return std::max(get_policy_lowest_available_freq(policy_idx), khz_floor);
}

static void refresh_scene_policy_state(const char* current_app) {
    long long now_ms = get_current_time_ms();
    g_scene_policy.last_update_ms = now_ms;
    g_scene_policy.screen_off = is_screen_off() ? 1 : 0;
    g_scene_policy.background_music = is_background_music_playing() ? 1 : 0;
    g_scene_policy.camera_active = is_camera_active() ? 1 : 0;
    g_scene_policy.game_loading = is_game_loading() ? 1 : 0;
    g_scene_policy.download_active = UnifiedScheduler::has_active_download_thread() ? 1 : 0;
    g_scene_policy.official_conflict_active = should_pause_our_game_scheduler() ? 1 : 0;
    g_scene_policy.mini_program_active = 0;

    std::string highest_mode = get_highest_mode_for_visible_apps();
    safe_strncpy(g_scene_policy.highest_mode,
                 highest_mode.empty() ? "powersave" : highest_mode.c_str(),
                 sizeof(g_scene_policy.highest_mode));

    const char* fg_activity = get_cached_foreground_activity_name();
    safe_strncpy(g_scene_policy.foreground_activity,
                 (fg_activity && fg_activity[0]) ? fg_activity : "",
                 sizeof(g_scene_policy.foreground_activity));
    safe_strncpy(g_scene_policy.foreground_process,
                 (current_app && current_app[0]) ? current_app : "",
                 sizeof(g_scene_policy.foreground_process));

    const char* category = classify_scene_category(
        current_app,
        g_scene_policy.foreground_process,
        g_scene_policy.foreground_activity);
    safe_strncpy(g_scene_policy.foreground_category,
                 (category && category[0]) ? category : "",
                 sizeof(g_scene_policy.foreground_category));
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
void set_sched_boost(int top_boost, int sched_boost, mode_t pre_perm = 0755, mode_t post_perm = 0444) {
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
void set_sched_config(int downmigrate, int upmigrate, int group_downmigrate, int group_upmigrate, mode_t pre_perm = 0755, mode_t post_perm = 0444) {
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
void set_stune_topapp(int prefer_idle, int boost, mode_t pre_perm = 0755, mode_t post_perm = 0444) {
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


// 获取前台应用
#include "activity_scene_foreground.inc"

#include "activity_cpu_settings.inc"

// 应用DDR设置
void apply_ddr_settings() {
}

// 保留空实现，避免旧调用链继续写DDR节点
void apply_ddr_settings_qualcomm() {
}

void apply_ddr_settings_mediatek() {
}

void try_mediatek_alternative_ddr_paths() {
}

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

static std::string scheduler_to_lower_copy(const char* text) {
    std::string out = text ? text : "";
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return (char)tolower(c); });
    return out;
}

#include "activity_config_loading.inc"

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
        char* package = strtok(line, " ");
        char* custom_mode = strtok(NULL, " ");

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
            char* pkg = strtok(line, " ");
            char* custom_mode = strtok(NULL, " ");
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

static bool process_exists_by_package(const char* package) {
    if (!package || !package[0]) return false;
    char cmd[256];
    safe_snprintf(cmd, sizeof(cmd), "pidof %s 2>/dev/null", package);
    FILE* fp = popen(cmd, "r");
    if (!fp) return false;
    char buf[64];
    bool exists = fgets(buf, sizeof(buf), fp) != NULL;
    pclose(fp);
    return exists;
}

static bool process_exists_by_name(const char* name) {
    return process_exists_by_package(name);
}

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

static int detect_official_tuner_mode() {
    if (process_exists_by_name("gameopt_hal_service-1-0") || process_exists_by_package("com.oplus.cosa")) {
        return OFFICIAL_TUNER_OPPO;
    }
    if (process_exists_by_package("com.xiaomi.joyose")) {
        return OFFICIAL_TUNER_XIAOMI;
    }
    if (process_exists_by_package("com.vivo.gamewatch")) {
        return OFFICIAL_TUNER_VIVO;
    }
    return OFFICIAL_TUNER_NONE;
}

static void disable_official_tuner_for_game(int mode) {
    switch (mode) {
    case OFFICIAL_TUNER_XIAOMI:
        run_shell_quiet("am force-stop com.xiaomi.joyose >/dev/null 2>&1");
        run_shell_quiet("pm disable com.xiaomi.joyose/com.xiaomi.joyose.smartop.SmartOpService >/dev/null 2>&1");
        run_shell_quiet("pm disable com.xiaomi.joyose/com.xiaomi.joyose.JoyoseJobScheduleService >/dev/null 2>&1");
        break;
    case OFFICIAL_TUNER_VIVO:
        run_shell_quiet("am force-stop com.vivo.gamewatch >/dev/null 2>&1");
        run_shell_quiet("pm disable com.vivo.gamewatch >/dev/null 2>&1");
        break;
    case OFFICIAL_TUNER_OPPO:
        run_shell_quiet("stop gameopt_hal_service-1-0 >/dev/null 2>&1");
        break;
    default:
        break;
    }
}

static void restore_official_tuner_after_game(int mode) {
    switch (mode) {
    case OFFICIAL_TUNER_XIAOMI:
        run_shell_quiet("pm enable com.xiaomi.joyose/com.xiaomi.joyose.smartop.SmartOpService >/dev/null 2>&1");
        run_shell_quiet("pm enable com.xiaomi.joyose/com.xiaomi.joyose.JoyoseJobScheduleService >/dev/null 2>&1");
        break;
    case OFFICIAL_TUNER_VIVO:
        run_shell_quiet("pm enable com.vivo.gamewatch >/dev/null 2>&1");
        break;
    case OFFICIAL_TUNER_OPPO:
        run_shell_quiet("start gameopt_hal_service-1-0 >/dev/null 2>&1");
        break;
    default:
        break;
    }
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

        update_official_game_interference_state(current_app);
        load_mode_settings_json(mode);
        apply_settings();

        UnifiedScheduler::notify_foreground_app_changed(current_app);
        UnifiedScheduler::refresh_bpf_targets_now();
        if (g_dyn_params.debug_log) {
            char log_msg[512];
            safe_snprintf(log_msg, sizeof(log_msg),
                "应用: %s | 模式: %s | 设置已应用",
                current_app, mode);
            log_debug_message(log_msg);
            RuntimeFeatureSwitches app_features = resolve_runtime_feature_switches_for_app(current_app);
            safe_snprintf(log_msg, sizeof(log_msg),
                          "应用功能开关 | app:%s | master:%d | bpf_thread:%d | scene:%d | base:%d",
                          current_app,
                          app_features.scheduler_master_enabled ? 1 : 0,
                          app_features.bpf_thread_enabled ? 1 : 0,
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

#include "activity_main_loop.inc"
