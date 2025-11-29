#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdarg.h>
#include <time.h>

// 日志级别定义
#define LOG_LEVEL_NONE     0    // 不输出任何日志
#define LOG_LEVEL_ERROR    1    // 只输出错误日志
#define LOG_LEVEL_INFO     2    // 输出信息和错误日志
#define LOG_LEVEL_DEBUG    3    // 输出所有日志

// 默认日志级别设置为NONE，可通过环境变量APPOPT_LOG_LEVEL控制
// 设置为0表示完全禁用日志写入
#ifndef APPOPT_LOG_LEVEL
#define APPOPT_LOG_LEVEL   LOG_LEVEL_NONE
#endif

#define VERSION            "2.1.0"
#define BASE_CPUSET        "/dev/cpuset/AppOpt"
#define MAX_PKG_LEN        128
#define MAX_THREAD_LEN     32
#define MIN_THREAD_LIFETIME 5   // 减少到5秒，避免过长的降级时间
#define MIN_STRATEGY_CHANGE_INTERVAL 3  // 最小策略切换间隔(秒)，防止频繁切换

// 初始容量定义内存和缓存相关常量
#define INITIAL_PROCS_CAP       128
#define INITIAL_TRACKED_PIDS_CAP 64
#define INITIAL_THREAD_CAP      32
#define INITIAL_RULES_CAP       16
#define INITIAL_TRACKERS_CAP    16
#define CONFIG_LINE_MAX         512
#define LOG_MSG_MAX             256
#define PATH_MAX_LEN            256
#define APPLY_THROTTLE_NS       100000000  // 100ms，提高响应性
#define MEMORY_GROWTH_FACTOR    1.5        // 内存增长因子
#define PROC_CACHE_TIMEOUT      5          // 进程状态缓存超时时间(秒)
#define FULL_SCAN_INTERVAL      300        // 全量扫描间隔(秒)
#define PROC_COUNT_THRESHOLD    20         // 进程数量变化阈值
#define PROC_MTIME_THRESHOLD    60         // 进程修改时间阈值(秒)

// 调度策略类型枚举
typedef enum {
    SCHED_POLICY_DEFAULT = 0, // 默认策略（SCHED_OTHER）
    SCHED_POLICY_FIFO,        // SCHED_FIFO
    SCHED_POLICY_RR,          // SCHED_RR
    SCHED_POLICY_UCLAMP,      // UClamp设置
    SCHED_POLICY_NICE         // Nice值设置
} SchedPolicyType;

// 线程跟踪器结构体
typedef struct {
    char name[MAX_THREAD_LEN];
    time_t earliest_time;
    pid_t tid;
} ThreadTracker;

typedef struct {
    char pkg[MAX_PKG_LEN];
    char thread[MAX_THREAD_LEN];
    char cpuset_dir[256];
    cpu_set_t cpus;

    // 新增的调度策略字段
    SchedPolicyType sched_policy;
    int priority;           // 用于SCHED_FIFO/SCHED_RR的优先级
    int uclamp_min;         // UClamp最小值
    int uclamp_max;         // UClamp最大值
    int nice_value;         // Nice值
} AffinityRule;

typedef struct {
    pid_t tid;
    char name[MAX_THREAD_LEN];
    char cpuset_dir[256];
    cpu_set_t cpus;

    // 新增的调度策略字段
    SchedPolicyType sched_policy;
    int priority;
    int uclamp_min;
    int uclamp_max;
    int nice_value;

    // 新增：线程首次被发现的时间戳
    time_t first_seen_time;
    // 新增：线程是否为主要线程（最早出现的同名线程）
    bool is_primary_thread;
    // 新增：线程是否已经被降级处理
    bool is_demoted;
    // 新增：上次策略切换的时间戳（防抖机制）
    time_t last_strategy_change_time;
} ThreadInfo;

typedef struct {
    pid_t pid;
    char pkg[MAX_PKG_LEN];
    char base_cpuset[128];
    cpu_set_t base_cpus;
    ThreadInfo* threads;
    size_t num_threads;
    size_t threads_cap;
    AffinityRule** thread_rules;
    size_t num_thread_rules;
    size_t thread_rules_cap;

    // 包级调度策略
    SchedPolicyType base_sched_policy;
    int base_priority;
    int base_uclamp_min;
    int base_uclamp_max;
    int base_nice_value;

    // 线程名到最早出现时间的映射
    ThreadTracker* thread_trackers;
    size_t num_trackers;
    size_t trackers_cap;
} ProcessInfo;

typedef struct {
    cpu_set_t present_cpus;
    char present_str[128];
    char mems_str[32];
    bool cpuset_enabled;
    int base_cpuset_fd;
} CpuTopology;

typedef struct {
    atomic_int ref_count;
    AffinityRule* rules;
    size_t num_rules;
    time_t mtime;
    CpuTopology topo;
    char** pkgs;
    size_t num_pkgs;
    char config_file[4096];
} AppConfig;

typedef struct {
    ProcessInfo* procs;
    size_t num_procs;
    size_t procs_cap;
    int last_proc_count;
    bool scan_all_proc;
    pid_t* tracked_pids;
    size_t num_tracked_pids;
    size_t tracked_pids_cap;
    int last_proc_total;
    time_t last_full_scan;
    
    // 全局线程跟踪器存储，用于保持线程跟踪器的持久性
    ThreadTracker* global_trackers;
    size_t num_global_trackers;
    size_t global_trackers_cap;
} ProcCache;

static atomic_int config_updated = ATOMIC_VAR_INIT(0);
static int inotify_fd = -1;
static int inotify_wd = -1;
static int inotify_supported = 0;
static _Atomic(AppConfig*)current_config = NULL;
static ProcCache global_cache = {};

// 进程状态缓存结构
typedef struct {
    pid_t pid;
    time_t last_check;
    bool exists;
} ProcessStatusEntry;

typedef struct {
    ProcessStatusEntry* entries;
    size_t num_entries;
    size_t capacity;
    time_t cache_timeout;
} ProcessStatusCache;

// 全局进程状态缓存
static ProcessStatusCache proc_status_cache = {0};

// 异步处理相关结构体和变量
typedef struct {
    ProcCache* cache;
    CpuTopology* topo;
    bool should_exit;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    bool has_work;
} AsyncAffinityContext;

static AsyncAffinityContext async_context = {0};
static pthread_t affinity_thread;
static bool async_processing_enabled = true;

// 日志级别控制
static int current_log_level = APPOPT_LOG_LEVEL;

// 初始化日志级别（从环境变量读取）
static void init_log_level(void) {
    const char* env_log_level = getenv("APPOPT_LOG_LEVEL");
    if (env_log_level) {
        int level = atoi(env_log_level);
        if (level >= LOG_LEVEL_NONE && level <= LOG_LEVEL_DEBUG) {
            current_log_level = level;
        }
    }
}

// 内部日志写入函数
static void write_log(int level, const char* level_str, const char* message) {
    // 如果当前日志级别为NONE或者消息级别高于当前设置级别，则不输出
    if (current_log_level == LOG_LEVEL_NONE || level > current_log_level) {
        return;
    }
    
    FILE* log_file = fopen("/data/local/tmp/appopt.log", "a");
    if (log_file) {
        time_t now = time(NULL);
        struct tm* tm_info = localtime(&now);
        char timestamp[32];
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);

        fprintf(log_file, "%s [%s] - %s\n", timestamp, level_str, message);
        fclose(log_file);
    }
}

// 不同级别的日志函数
static void log_error(const char* message) {
    write_log(LOG_LEVEL_ERROR, "ERROR", message);
}

static void log_info(const char* message) {
    write_log(LOG_LEVEL_INFO, "INFO", message);
}

static void __attribute__((unused)) log_debug(const char* message) {
    write_log(LOG_LEVEL_DEBUG, "DEBUG", message);
}

// 保持原有的log_message函数兼容性，默认为INFO级别
static void log_message(const char* message) {
    log_info(message);
}



// 安全的内存分配辅助函数
static void* safe_calloc(size_t nmemb, size_t size) {
    if (nmemb == 0 || size == 0) return NULL;
    // 检查溢出
    if (nmemb > SIZE_MAX / size) {
        log_error("内存分配大小溢出");
        return NULL;
    }
    void* ptr = calloc(nmemb, size);
    if (!ptr) {
        log_error("内存分配失败");
        return NULL;
    }
    return ptr;
}

static void* safe_realloc(void* ptr, size_t new_size) {
    if (new_size == 0) {
        free(ptr);
        return NULL;
    }
    void* new_ptr = realloc(ptr, new_size);
    if (!new_ptr) {
        log_error("内存重新分配失败");
        return NULL;
    }
    return new_ptr;
}

// 动态数组扩容函数
static bool expand_array(void** array, size_t* capacity, size_t element_size, size_t min_capacity) {
    if (*capacity >= min_capacity) return true;
    
    size_t new_capacity = *capacity == 0 ? 
        (min_capacity > 16 ? min_capacity : 16) : 
        (size_t)(*capacity * MEMORY_GROWTH_FACTOR);
    
    if (new_capacity < min_capacity) {
        new_capacity = min_capacity;
    }
    
    void* new_array = safe_realloc(*array, new_capacity * element_size);
    if (!new_array) return false;
    
    *array = new_array;
    *capacity = new_capacity;
    return true;
}

// 初始化进程状态缓存
static void init_process_cache(void) {
    if (!proc_status_cache.entries) {
        proc_status_cache.capacity = INITIAL_PROCS_CAP;
        proc_status_cache.entries = (ProcessStatusEntry*)safe_calloc(proc_status_cache.capacity, sizeof(ProcessStatusEntry));
        proc_status_cache.cache_timeout = PROC_CACHE_TIMEOUT;
    }
    
    // 初始化全局线程跟踪器
    if (!global_cache.global_trackers) {
        global_cache.global_trackers_cap = INITIAL_TRACKERS_CAP;
        global_cache.global_trackers = (ThreadTracker*)safe_calloc(global_cache.global_trackers_cap, sizeof(ThreadTracker));
        global_cache.num_global_trackers = 0;
    }
}

// 清理进程状态缓存
static void cleanup_process_cache(void) {
    if (proc_status_cache.entries) {
        free(proc_status_cache.entries);
        proc_status_cache.entries = NULL;
        proc_status_cache.num_entries = 0;
        proc_status_cache.capacity = 0;
    }
}

// 检查进程是否存在（带缓存）
static bool is_process_alive_cached(pid_t pid) {
    time_t now = time(NULL);
    
    // 查找缓存条目
    for (size_t i = 0; i < proc_status_cache.num_entries; i++) {
        ProcessStatusEntry* entry = &proc_status_cache.entries[i];
        if (entry->pid == pid) {
            // 检查缓存是否过期
            if (now - entry->last_check < proc_status_cache.cache_timeout) {
                return entry->exists;
            }
            // 缓存过期，更新状态
            entry->exists = (kill(pid, 0) == 0);
            entry->last_check = now;
            return entry->exists;
        }
    }
    
    // 缓存中没有找到，添加新条目
    if (proc_status_cache.num_entries >= proc_status_cache.capacity) {
        if (!expand_array((void**)&proc_status_cache.entries, &proc_status_cache.capacity, 
                         sizeof(ProcessStatusEntry), proc_status_cache.num_entries + 1)) {
            // 扩容失败，直接检查
            return (kill(pid, 0) == 0);
        }
    }
    
    ProcessStatusEntry* new_entry = &proc_status_cache.entries[proc_status_cache.num_entries++];
    new_entry->pid = pid;
    new_entry->exists = (kill(pid, 0) == 0);
    new_entry->last_check = now;
    
    return new_entry->exists;
}

static char* strtrim(char* s) {
    if (!s) return s;
    char* end;
    while (isspace((unsigned char)*s)) s++;
    if (*s == 0) return s;
    end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) end--;
    *(end + 1) = 0;
    return s;
}

static bool read_file(int dir_fd, const char* filename, char* buf, size_t buf_size) {
    if (!filename || !buf || buf_size == 0) return false;

    int fd = openat(dir_fd, filename, O_RDONLY | O_CLOEXEC);
    if (fd == -1) return false;

    // 确保至少留一个字节给null终止符
    if (buf_size < 2) {
        close(fd);
        return false;
    }

    ssize_t n = read(fd, buf, buf_size - 1);
    close(fd);
    if (n <= 0) return false;

    // 确保字符串正确终止
    buf[n] = '\0';
    
    // 移除末尾的换行符
    if (n > 0 && buf[n-1] == '\n') {
        buf[n-1] = '\0';
    }
    
    return true;
}

static bool write_file(int dir_fd, const char* filename, const char* content, int flags) {
    int fd = openat(dir_fd, filename, flags | O_CLOEXEC, 0644);
    if (fd == -1) return false;

    size_t total = strlen(content);
    ssize_t written = 0;

    while (written < (ssize_t)total) {
        ssize_t n = write(fd, content + written, total - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return false;
        }
        written += n;
    }

    close(fd);
    return true;
}

static int build_str(char* dest, size_t dest_size, ...) {
    if (!dest || dest_size == 0) return 0;

    va_list args;
    const char* segment;
    char* p = dest;
    size_t remaining = dest_size - 1;
    va_start(args, dest_size);

    while ((segment = va_arg(args, const char*)) != NULL) {
        size_t len = strlen(segment);
        if (len > remaining) {
            va_end(args);
            dest[0] = '\0';
            return 0;
        }
        memcpy(p, segment, len);
        p += len;
        remaining -= len;
    }

    *p = '\0';
    va_end(args);
    return 1;
}

static void parse_cpu_ranges(const char* spec, cpu_set_t* set, const cpu_set_t* present) {
    if (!spec || !set) return;

    // 检查输入字符串长度，防止过长的输入
    size_t spec_len = strlen(spec);
    if (spec_len == 0 || spec_len > 1024) return;

    char* copy = strdup(spec);
    if (!copy) return;

    char* s = copy;
    CPU_ZERO(set);

    while (*s) {
        // 跳过空白字符
        while (isspace(*s)) s++;
        if (!*s) break;

        char* end;
        unsigned long a = strtoul(s, &end, 10);
        if (end == s) {
            s++;
            continue;
        }

        // 检查CPU编号是否在合理范围内
        if (a >= CPU_SETSIZE) {
            s = (*end == ',') ? end + 1 : end;
            continue;
        }

        unsigned long b = a;
        if (*end == '-') {
            s = end + 1;
            b = strtoul(s, &end, 10);
            if (end == s) b = a;
            
            // 检查范围结束值是否在合理范围内
            if (b >= CPU_SETSIZE) b = CPU_SETSIZE - 1;
        }

        if (a > b) {
            unsigned long t = a;
            a = b;
            b = t;
        }

        for (unsigned long i = a; i <= b && i < CPU_SETSIZE; i++) {
            if (present && !CPU_ISSET(i, present)) continue;
            CPU_SET(i, set);
        }

        s = (*end == ',') ? end + 1 : end;
    }

    free(copy);
}

static char* cpu_set_to_str(const cpu_set_t* set) {
    if (!set) return NULL;

    char* buf = (char*)malloc(8 * CPU_SETSIZE);
    if (!buf) return NULL;

    int start = -1, end = -1;
    char* p = buf;
    size_t remain = 8 * CPU_SETSIZE - 1;
    bool first = true;

    for (int i = 0; i < CPU_SETSIZE; i++) {
        if (CPU_ISSET(i, set)) {
            if (start == -1) {
                start = end = i;
            }
            else if (i == end + 1) {
                end = i;
            }
            else {
                int needed;
                if (start == end) {
                    needed = snprintf(p, remain + 1, "%s%d", first ? "" : ",", start);
                }
                else {
                    needed = snprintf(p, remain + 1, "%s%d-%d", first ? "" : ",", start, end);
                }
                if (needed < 0 || (size_t)needed > remain) {
                    free(buf);
                    return NULL;
                }
                p += needed;
                remain -= needed;
                start = end = i;
                first = false;
            }
        }
    }

    if (start != -1) {
        int needed;
        if (start == end) {
            needed = snprintf(p, remain + 1, "%s%d", first ? "" : ",", start);
        }
        else {
            needed = snprintf(p, remain + 1, "%s%d-%d", first ? "" : ",", start, end);
        }
        if (needed < 0 || (size_t)needed > remain) {
            free(buf);
            return NULL;
        }
        p += needed;
    }

    // 修复：使用比较运算符而不是赋值运算符
    if (buf[0] == '\0') {
        strcpy(buf, "0");
    }

    return buf;
}

static bool create_cpuset_dir(const char* path, const char* cpus, const char* mems) {
    if (mkdir(path, 0755) != 0 && errno != EEXIST) return false;
    chmod(path, 0755);
    chown(path, 0, 0);

    char cpus_path[256];
    if (!build_str(cpus_path, sizeof(cpus_path), path, "/cpus", NULL) ||
        !write_file(AT_FDCWD, cpus_path, cpus, O_WRONLY | O_CREAT | O_TRUNC)) {
        return false;
    }

    char mems_path[256];
    if (!build_str(mems_path, sizeof(mems_path), path, "/mems", NULL) ||
        !write_file(AT_FDCWD, mems_path, mems, O_WRONLY | O_CREAT | O_TRUNC)) {
        return false;
    }

    return true;
}

static CpuTopology init_cpu_topo(void) {
    CpuTopology topo = {
        .present_cpus = {},
        .present_str = {},
        .mems_str = {},
        .cpuset_enabled = false,
        .base_cpuset_fd = -1
    };
    CPU_ZERO(&topo.present_cpus);

    if (read_file(AT_FDCWD, "/sys/devices/system/cpu/present", topo.present_str, sizeof(topo.present_str))) {
        strtrim(topo.present_str);
    }
    else {
        snprintf(topo.present_str, sizeof(topo.present_str), "0-%d", CPU_SETSIZE - 1);
    }

    parse_cpu_ranges(topo.present_str, &topo.present_cpus, NULL);

    if (access("/dev/cpuset", F_OK) != 0) return topo;

    if (create_cpuset_dir(BASE_CPUSET, topo.present_str, "0")) {
        topo.base_cpuset_fd = open(BASE_CPUSET, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (topo.base_cpuset_fd != -1) topo.cpuset_enabled = true;
    }

    char mems_path[256];
    if (!build_str(mems_path, sizeof(mems_path), BASE_CPUSET, "/mems", NULL) ||
        !read_file(AT_FDCWD, mems_path, topo.mems_str, sizeof(topo.mems_str))) {
        snprintf(topo.mems_str, sizeof(topo.mems_str), "0");
    }
    else {
        strtrim(topo.mems_str);
    }

    return topo;
}

static AppConfig* load_config(const char* config_file, const CpuTopology* topo, time_t* last_mtime) {
    if (!config_file || !topo) return NULL;

    struct stat st;
    if (stat(config_file, &st)) return NULL;

    AppConfig* cfg = (AppConfig*)safe_calloc(1, sizeof(AppConfig));
    if (!cfg) return NULL;

    cfg->ref_count = 1;
    cfg->topo = *topo;
    strncpy(cfg->config_file, config_file, sizeof(cfg->config_file) - 1);
    cfg->config_file[sizeof(cfg->config_file) - 1] = '\0';

    if (last_mtime && *last_mtime == st.st_mtime && *last_mtime != -1) {
        free(cfg);
        return NULL;
    }

    FILE* fp = fopen(config_file, "r");
    if (!fp) {
        free(cfg);
        return NULL;
    }

    AffinityRule* new_rules = NULL;
    char** new_pkgs = NULL;
    size_t rules_cnt = 0, pkgs_cnt = 0;
    size_t rules_cap = 0, pkgs_cap = 0;
    char line[CONFIG_LINE_MAX];

    while (fgets(line, sizeof(line), fp)) {
        char* p = strtrim(line);
        if (*p == '#' || !*p) continue;

        char* eq = strchr(p, '=');
        if (!eq) continue;
        *eq++ = 0;

        char* br = strchr(p, '{');
        const char* thread = "";
        if (br) {
            *br++ = 0;
            char* eb = strchr(br, '}');
            if (!eb) continue;
            *eb = 0;
            thread = strtrim(br);
        }

        char* pkg = strtrim(p);
        char* cpus = strtrim(eq);
        if (strlen(pkg) >= MAX_PKG_LEN || strlen(thread) >= MAX_THREAD_LEN) continue;

        // 解析调度策略部分（如果有）
        char* sched_part = strchr(cpus, ':');
        if (sched_part) {
            *sched_part++ = '\0'; // 分割字符串
            cpus = strtrim(cpus); // 重新修剪CPU部分
        }

        cpu_set_t set;
        CPU_ZERO(&set);
        parse_cpu_ranges(cpus, &set, &cfg->topo.present_cpus);
        if (CPU_COUNT(&set) == 0) continue;

        char* dir_name = cpu_set_to_str(&set);
        if (!dir_name) continue;

        char path[PATH_MAX_LEN];
        if (!build_str(path, sizeof(path), BASE_CPUSET, "/", dir_name, NULL)) {
            free(dir_name);
            continue;
        }

        if (!create_cpuset_dir(path, dir_name, cfg->topo.mems_str)) {
            free(dir_name);
            continue;
        }

        AffinityRule rule = {};
        strncpy(rule.pkg, pkg, MAX_PKG_LEN - 1);
        strncpy(rule.thread, thread, MAX_THREAD_LEN - 1);
        strncpy(rule.cpuset_dir, dir_name, sizeof(rule.cpuset_dir) - 1);
        rule.cpus = set;

        // 默认值
        rule.sched_policy = SCHED_POLICY_DEFAULT;
        rule.priority = 0;
        rule.uclamp_min = 0;
        rule.uclamp_max = 100;
        rule.nice_value = 0;

        // 解析调度策略
        if (sched_part) {
            char* sched_type = strtrim(strtok(sched_part, " "));

            if (sched_type) {
                if (strcasecmp(sched_type, "SCHED_FIFO") == 0) {
                    rule.sched_policy = SCHED_POLICY_FIFO;
                    char* prio_str = strtrim(strtok(NULL, " "));
                    if (prio_str) {
                        rule.priority = atoi(prio_str);
                        if (rule.priority < 1) rule.priority = 1;
                        if (rule.priority > 99) rule.priority = 99;
                    }
                    else {
                        rule.priority = 50; // 默认优先级
                    }
                }
                else if (strcasecmp(sched_type, "SCHED_RR") == 0) {
                    rule.sched_policy = SCHED_POLICY_RR;
                    char* prio_str = strtrim(strtok(NULL, " "));
                    if (prio_str) {
                        rule.priority = atoi(prio_str);
                        if (rule.priority < 1) rule.priority = 1;
                        if (rule.priority > 99) rule.priority = 99;
                    }
                    else {
                        rule.priority = 50; // 默认优先级
                    }
                }
                else if (strcasecmp(sched_type, "UCLAMP") == 0) {
                    rule.sched_policy = SCHED_POLICY_UCLAMP;
                    char* min_str = strtrim(strtok(NULL, " "));
                    char* max_str = strtrim(strtok(NULL, " "));
                    if (min_str) {
                        rule.uclamp_min = atoi(min_str);
                        if (rule.uclamp_min < 0) rule.uclamp_min = 0;
                        if (rule.uclamp_min > 100) rule.uclamp_min = 100;
                    }
                    if (max_str) {
                        rule.uclamp_max = atoi(max_str);
                        if (rule.uclamp_max < 0) rule.uclamp_max = 0;
                        if (rule.uclamp_max > 100) rule.uclamp_max = 100;
                    }
                    // 确保最小值不大于最大值
                    if (rule.uclamp_min > rule.uclamp_max) {
                        int temp = rule.uclamp_min;
                        rule.uclamp_min = rule.uclamp_max;
                        rule.uclamp_max = temp;
                    }
                }
                else if (strcasecmp(sched_type, "NICE") == 0) {
                    rule.sched_policy = SCHED_POLICY_NICE;
                    char* nice_str = strtrim(strtok(NULL, " "));
                    if (nice_str) {
                        rule.nice_value = atoi(nice_str);
                        if (rule.nice_value < -20) rule.nice_value = -20;
                        if (rule.nice_value > 19) rule.nice_value = 19;
                    }
                }
            }
        }

        free(dir_name);

        // 使用安全的数组扩容函数
        if (!expand_array((void**)&new_rules, &rules_cap, sizeof(AffinityRule), rules_cnt + 1)) {
            goto error;
        }
        memcpy(&new_rules[rules_cnt], &rule, sizeof(AffinityRule));
        rules_cnt++;

        bool exists = false;
        for (size_t i = 0; i < pkgs_cnt; i++) {
            if (strcmp(new_pkgs[i], pkg) == 0) {
                exists = true;
                break;
            }
        }

        if (!exists) {
            if (!expand_array((void**)&new_pkgs, &pkgs_cap, sizeof(char*), pkgs_cnt + 1)) {
                goto error;
            }
            new_pkgs[pkgs_cnt] = strdup(pkg);
            if (!new_pkgs[pkgs_cnt]) goto error;
            pkgs_cnt++;
        }
    }

    if (cfg->rules) free(cfg->rules);
    if (cfg->pkgs) {
        for (size_t i = 0; i < cfg->num_pkgs; i++) free(cfg->pkgs[i]);
        free(cfg->pkgs);
    }

    if (last_mtime) *last_mtime = st.st_mtime;
    cfg->rules = new_rules;
    cfg->num_rules = rules_cnt;
    cfg->pkgs = new_pkgs;
    cfg->num_pkgs = pkgs_cnt;
    cfg->mtime = st.st_mtime;

    fclose(fp);
    char log_msg[LOG_MSG_MAX];
    snprintf(log_msg, sizeof(log_msg), "配置文件解析完成，共加载 %zu 条规则", rules_cnt);
    log_message(log_msg);
    return cfg;

error:
    if (new_rules) free(new_rules);
    if (new_pkgs) {
        for (size_t i = 0; i < pkgs_cnt; i++) {
            if (new_pkgs[i]) free(new_pkgs[i]);
        }
        free(new_pkgs);
    }
    fclose(fp);
    free(cfg);
    return NULL;
}

// 检查进程是否在跟踪列表中
static bool is_process_tracked(pid_t pid, const ProcCache* cache) {
    for (size_t i = 0; i < cache->num_tracked_pids; i++) {
        if (cache->tracked_pids[i] == pid) {
            return true;
        }
    }
    return false;
}

// 检查进程是否应该被跳过（基于修改时间）
static bool should_skip_process(int proc_fd, const char* pid_name, time_t current_time) {
    struct stat statbuf;
    if (fstatat(proc_fd, pid_name, &statbuf, AT_SYMLINK_NOFOLLOW) != 0) {
        return true;
    }
    return (current_time - statbuf.st_mtime > PROC_MTIME_THRESHOLD);
}

// 检查进程名是否在配置的包列表中
static bool is_process_in_config(const char* process_name, const AppConfig* cfg) {
    for (size_t j = 0; j < cfg->num_pkgs; j++) {
        if (strcmp(process_name, cfg->pkgs[j]) == 0) {
            return true;
        }
    }
    return false;
}

static void free_process_info(ProcessInfo* proc) {
    if (!proc) return;
    free(proc->threads);
    free(proc->thread_rules);
    free(proc->thread_trackers);
    proc->threads = NULL;
    proc->thread_rules = NULL;
    proc->thread_trackers = NULL;
    proc->num_threads = 0;
    proc->num_thread_rules = 0;
    proc->num_trackers = 0;
    proc->threads_cap = 0;
    proc->thread_rules_cap = 0;
    proc->trackers_cap = 0;
}

// 全局线程跟踪器管理函数
static ThreadTracker* find_global_tracker(ProcCache* cache, const char* thread_name) {
    for (size_t i = 0; i < cache->num_global_trackers; i++) {
        if (strcmp(cache->global_trackers[i].name, thread_name) == 0) {
            return &cache->global_trackers[i];
        }
    }
    return NULL;
}

static ThreadTracker* add_global_tracker(ProcCache* cache, const char* thread_name, time_t earliest_time, pid_t tid) {
    if (cache->num_global_trackers >= cache->global_trackers_cap) {
        if (!expand_array((void**)&cache->global_trackers, &cache->global_trackers_cap, 
                         sizeof(ThreadTracker), cache->num_global_trackers + 1)) {
            return NULL;
        }
    }
    
    ThreadTracker* tracker = &cache->global_trackers[cache->num_global_trackers++];
    strncpy(tracker->name, thread_name, MAX_THREAD_LEN - 1);
    tracker->name[MAX_THREAD_LEN - 1] = '\0';
    tracker->earliest_time = earliest_time;
    tracker->tid = tid;
    return tracker;
}

static void update_global_tracker(ThreadTracker* tracker, time_t earliest_time, pid_t tid) {
    if (earliest_time < tracker->earliest_time) {
        tracker->earliest_time = earliest_time;
        tracker->tid = tid;
    }
}

static void proc_collect(const AppConfig* cfg, ProcCache* cache, size_t* count, bool full_scan) {
    DIR* proc_dir = opendir("/proc");
    if (!proc_dir) return;

    int proc_fd = dirfd(proc_dir);
    *count = 0;
    time_t current_time = time(NULL);

    for (size_t i = 0; i < cache->num_procs; i++) {
        free_process_info(&cache->procs[i]);
    }
    cache->num_procs = 0;

    if (cache->procs == NULL) {
        cache->procs_cap = INITIAL_PROCS_CAP;
        cache->procs = (ProcessInfo*)safe_calloc(cache->procs_cap, sizeof(ProcessInfo));
        if (!cache->procs) {
            closedir(proc_dir);
            return;
        }
    }

    struct dirent* ent;
    int current_proc_total = 0;
    while ((ent = readdir(proc_dir))) {
        char* end;
        long pid = strtol(ent->d_name, &end, 10);
        if (*end != '\0')  continue;
        current_proc_total++;

        if (!full_scan) {
            if (!is_process_tracked(pid, cache)) {
                if (should_skip_process(proc_fd, ent->d_name, current_time)) {
                    continue;
                }
            }
        }

        int pid_fd = openat(proc_fd, ent->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (pid_fd == -1) continue;

        char cmd[MAX_PKG_LEN] = { 0 };
        if (!read_file(pid_fd, "cmdline", cmd, sizeof(cmd))) {
            close(pid_fd);
            continue;
        }
        char* name = strrchr(cmd, '/');
        name = name ? name + 1 : cmd;

        if (!is_process_in_config(name, cfg)) {
            close(pid_fd);
            continue;
        }

        if (*count >= cache->procs_cap) {
            if (!expand_array((void**)&cache->procs, &cache->procs_cap, sizeof(ProcessInfo), *count + 1)) {
                close(pid_fd);
                continue;
            }
        }

        ProcessInfo* proc = &cache->procs[*count];
        memset(proc, 0, sizeof(ProcessInfo));

        proc->pid = pid;
        strncpy(proc->pkg, name, MAX_PKG_LEN - 1);
        CPU_ZERO(&proc->base_cpus);
        proc->base_cpuset[0] = '\0';
        proc->num_threads = 0;
        proc->num_thread_rules = 0;
        proc->num_trackers = 0;

        // 初始化包级调度策略
        proc->base_sched_policy = SCHED_POLICY_DEFAULT;
        proc->base_priority = 0;
        proc->base_uclamp_min = 0;
        proc->base_uclamp_max = 100;
        proc->base_nice_value = 0;

        if (!proc->thread_rules || proc->thread_rules_cap < INITIAL_RULES_CAP) {
            if (!expand_array((void**)&proc->thread_rules, &proc->thread_rules_cap, sizeof(AffinityRule*), INITIAL_RULES_CAP)) {
                close(pid_fd);
                continue;
            }
        }

        for (size_t i = 0; i < cfg->num_rules; i++) {
            const AffinityRule* rule = &cfg->rules[i];
            if (strcmp(rule->pkg, proc->pkg) != 0) continue;

            if (rule->thread[0]) {
                if (proc->num_thread_rules >= proc->thread_rules_cap) {
                    if (!expand_array((void**)&proc->thread_rules, &proc->thread_rules_cap, sizeof(AffinityRule*), proc->num_thread_rules + 1)) {
                        break;
                    }
                }
                proc->thread_rules[proc->num_thread_rules++] = (AffinityRule*)rule;
            }
            else {
                CPU_OR(&proc->base_cpus, &proc->base_cpus, &rule->cpus);
                if (proc->base_cpuset[0] == '\0') {
                    strncpy(proc->base_cpuset, rule->cpuset_dir, sizeof(proc->base_cpuset) - 1);
                }

                // 保存包级调度策略（使用最后一个包级规则的策略）
                proc->base_sched_policy = rule->sched_policy;
                proc->base_priority = rule->priority;
                proc->base_uclamp_min = rule->uclamp_min;
                proc->base_uclamp_max = rule->uclamp_max;
                proc->base_nice_value = rule->nice_value;
            }
        }

        if (CPU_COUNT(&proc->base_cpus) == 0 && proc->num_thread_rules == 0) {
            close(pid_fd);
            continue;
        }

        int task_fd = openat(pid_fd, "task", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        close(pid_fd);
        if (task_fd == -1) continue;

        DIR* task_dir = fdopendir(task_fd);
        if (!task_dir) {
            close(task_fd);
            continue;
        }

        if (!proc->threads || proc->threads_cap < INITIAL_THREAD_CAP) {
            if (!expand_array((void**)&proc->threads, &proc->threads_cap, sizeof(ThreadInfo), INITIAL_THREAD_CAP)) {
                closedir(task_dir);
                continue;
            }
        }

        struct dirent* tent;
        while ((tent = readdir(task_dir))) {
            char* end2;
            long tid = strtol(tent->d_name, &end2, 10);
            if (*end2 != '\0')  continue;
            char tname[MAX_THREAD_LEN] = { 0 };

            int tid_fd = openat(task_fd, tent->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (tid_fd == -1) continue;

            if (!read_file(tid_fd, "comm", tname, sizeof(tname))) {
                close(tid_fd);
                continue;
            }
            close(tid_fd);

            strtrim(tname);

            if (proc->num_threads >= proc->threads_cap) {
                if (!expand_array((void**)&proc->threads, &proc->threads_cap, sizeof(ThreadInfo), proc->num_threads + 1)) {
                    continue;
                }
            }

            ThreadInfo* ti = &proc->threads[proc->num_threads];
            ti->tid = tid;
            strncpy(ti->name, tname, MAX_THREAD_LEN - 1);
            CPU_ZERO(&ti->cpus);
            const char* matched = NULL;

            // 初始化线程调度策略为包级默认值
            ti->sched_policy = proc->base_sched_policy;
            ti->priority = proc->base_priority;
            ti->uclamp_min = proc->base_uclamp_min;
            ti->uclamp_max = proc->base_uclamp_max;
            ti->nice_value = proc->base_nice_value;

            // 关键修复：先检查全局ThreadTracker记录，避免重置first_seen_time
            ti->is_primary_thread = false;
            ti->is_demoted = false;
            ti->last_strategy_change_time = 0; // 初始化为0，表示从未切换过策略

            // 检查全局跟踪器中是否已经有这个线程的记录
            ThreadTracker* global_tracker = find_global_tracker(cache, tname);
            if (global_tracker) {
                // 使用全局跟踪器中记录的时间戳，保持线程的真实首次出现时间
                ti->first_seen_time = global_tracker->earliest_time;
                // 更新全局跟踪器的TID（如果当前线程更早出现）
                update_global_tracker(global_tracker, ti->first_seen_time, tid);
            } else {
                // 没有找到全局跟踪记录，设置为当前时间并添加到全局跟踪器
                ti->first_seen_time = current_time;
                add_global_tracker(cache, tname, current_time, tid);
            }

            // 匹配线程规则（线程名不为空的规则）
            for (size_t i = 0; i < proc->num_thread_rules; i++) {
                const AffinityRule* rule = proc->thread_rules[i];
                if (fnmatch(rule->thread, ti->name, FNM_NOESCAPE) == 0) {
                    CPU_OR(&ti->cpus, &ti->cpus, &rule->cpus);
                    matched = rule->cpuset_dir;

                    // 应用线程特定的调度策略
                    ti->sched_policy = rule->sched_policy;
                    ti->priority = rule->priority;
                    ti->uclamp_min = rule->uclamp_min;
                    ti->uclamp_max = rule->uclamp_max;
                    ti->nice_value = rule->nice_value;
                }
            }

            // 如果没有匹配到线程规则，则应用包级规则
            if (!matched && CPU_COUNT(&proc->base_cpus) > 0) {
                ti->cpus = proc->base_cpus;
                matched = proc->base_cpuset;
            }

            if (matched) {
                strncpy(ti->cpuset_dir, matched, sizeof(ti->cpuset_dir) - 1);
            }

            proc->num_threads++;
        }

        closedir(task_dir);

        // 标记主要线程 - 基于全局ThreadTracker中记录的最早线程
        for (size_t j = 0; j < proc->num_threads; j++) {
            ThreadInfo* ti = &proc->threads[j];

            // 查找对应的全局线程跟踪器
            ThreadTracker* global_tracker = find_global_tracker(cache, ti->name);
            if (global_tracker) {
                // 如果当前线程的TID与全局跟踪器中记录的最早线程TID匹配，标记为主要线程
                if (ti->tid == global_tracker->tid) {
                    ti->is_primary_thread = true;
                }
            }
        }

        (*count)++;
    }

    closedir(proc_dir);
    cache->last_proc_total = current_proc_total;
    cache->last_full_scan = full_scan ? time(NULL) : cache->last_full_scan;
}

static void update_cache(ProcCache* cache, const AppConfig* cfg, int* affinity_counter) {
    bool need_reload = false;
    struct sysinfo info;
    if (sysinfo(&info) != 0) {
        need_reload = true;
    }
    else {
        int current_proc_count = info.procs;
        if (current_proc_count > cache->last_proc_count + PROC_COUNT_THRESHOLD) {
            need_reload = true;
        }
        else if (current_proc_count > cache->last_proc_count) {
            *affinity_counter = 0;
        }
        cache->last_proc_count = current_proc_count;
    }

    for (size_t i = 0; i < cache->num_tracked_pids; i++) {
        if (!is_process_alive_cached(cache->tracked_pids[i])) {
            need_reload = true;
            break;
        }
    }

    time_t now = time(NULL);
    if (now - cache->last_full_scan > FULL_SCAN_INTERVAL) {
        need_reload = true;
    }

    if (need_reload) {
        size_t new_count = 0;
        bool full_scan = (now - cache->last_full_scan > FULL_SCAN_INTERVAL) ||
            (cache->num_procs == 0) ||
            (cache->last_proc_total > cache->last_proc_count + PROC_COUNT_THRESHOLD);

        proc_collect(cfg, cache, &new_count, full_scan);

        if (new_count > cache->tracked_pids_cap) {
            size_t new_cap = cache->tracked_pids_cap ? cache->tracked_pids_cap * 2 : new_count;
            pid_t* new_pids = (pid_t*)realloc(cache->tracked_pids, new_cap * sizeof(pid_t));
            if (new_pids) {
                cache->tracked_pids = new_pids;
                cache->tracked_pids_cap = new_cap;
            }
        }

        if (cache->tracked_pids) {
            cache->num_tracked_pids = 0;
            for (size_t i = 0; i < new_count; i++) {
                if (cache->num_tracked_pids < cache->tracked_pids_cap) {
                    cache->tracked_pids[cache->num_tracked_pids++] = cache->procs[i].pid;
                }
            }
        }

        cache->num_procs = new_count;
        *affinity_counter = 0;
    }
}

static void apply_scheduling_policy(const ThreadInfo* ti) {
    if (ti->sched_policy == SCHED_POLICY_DEFAULT) {
        return; // 无需处理
    }

    // 直接使用原始线程信息，不进行负载调整
    const ThreadInfo* effective_ti = ti;

    // 如果线程被降级，应用保守策略
    if (effective_ti->is_demoted) {
        char log_msg[LOG_MSG_MAX];
        snprintf(log_msg, sizeof(log_msg),
            "应用保守策略于线程 %s (TID: %d)，已被降级",
            effective_ti->name, effective_ti->tid);
        log_message(log_msg);

        // 对于已降级的线程，统一使用NICE策略，避免实时调度
        setpriority(PRIO_PROCESS, effective_ti->tid, 5); // 使用中等优先级
        return;
    }

    // 如果不是主要线程，应用温和策略
    if (!effective_ti->is_primary_thread) {
        char log_msg[LOG_MSG_MAX];
        snprintf(log_msg, sizeof(log_msg),
            "应用温和策略于线程 %s (TID: %d)，非主要线程",
            effective_ti->name, effective_ti->tid);
        log_message(log_msg);

        // 对于非主要线程，统一使用较低的NICE优先级，避免与主要线程竞争
        if (effective_ti->sched_policy == SCHED_POLICY_FIFO || effective_ti->sched_policy == SCHED_POLICY_RR) {
            // 关键修复：先将调度策略从实时策略改为SCHED_OTHER，然后设置NICE值
            struct sched_param param;
            param.sched_priority = 0; // SCHED_OTHER策略的优先级必须为0
            
            if (sched_setscheduler(effective_ti->tid, SCHED_OTHER, &param) == 0) {
                // 成功改为SCHED_OTHER后，设置NICE值
                setpriority(PRIO_PROCESS, effective_ti->tid, 10); // 较低优先级
                
                char priority_msg[LOG_MSG_MAX];
                snprintf(priority_msg, sizeof(priority_msg),
                    "非主要线程 %s (TID: %d) 从实时策略改为SCHED_OTHER+NICE=10，避免竞争",
                    effective_ti->name, effective_ti->tid);
                log_message(priority_msg);
            } else {
                char error_msg[LOG_MSG_MAX];
                snprintf(error_msg, sizeof(error_msg),
                    "警告：无法将非主要线程 %s (TID: %d) 改为SCHED_OTHER策略: %s",
                    effective_ti->name, effective_ti->tid, strerror(errno));
                log_message(error_msg);
            }
        }
        else if (effective_ti->sched_policy == SCHED_POLICY_UCLAMP) {
            // 使用较低的UCLAMP值，避免与主要线程竞争
            char path[128];
            char value[16];

            int low_min = effective_ti->uclamp_min / 3;  // 大幅降低
            int low_max = effective_ti->uclamp_max / 2;  // 降低一半

            snprintf(path, sizeof(path), "/proc/%d/task/%d/uclamp.min", getpid(), effective_ti->tid);
            snprintf(value, sizeof(value), "%d", low_min);
            write_file(AT_FDCWD, path, value, O_WRONLY);

            snprintf(path, sizeof(path), "/proc/%d/task/%d/uclamp.max", getpid(), effective_ti->tid);
            snprintf(value, sizeof(value), "%d", low_max);
            write_file(AT_FDCWD, path, value, O_WRONLY);
            
            char uclamp_msg[LOG_MSG_MAX];
            snprintf(uclamp_msg, sizeof(uclamp_msg),
                "非主要线程 %s (TID: %d) 设置UCLAMP min=%d max=%d，避免竞争",
                effective_ti->name, effective_ti->tid, low_min, low_max);
            log_message(uclamp_msg);
        }
        else if (effective_ti->sched_policy == SCHED_POLICY_NICE) {
            // 增加NICE值，降低优先级
            int low_nice = effective_ti->nice_value + 10;
            if (low_nice > 19) low_nice = 19; // 限制在最大值
            setpriority(PRIO_PROCESS, effective_ti->tid, low_nice);
            
            char nice_msg[LOG_MSG_MAX];
            snprintf(nice_msg, sizeof(nice_msg),
                "非主要线程 %s (TID: %d) 设置NICE=%d，避免竞争",
                effective_ti->name, effective_ti->tid, low_nice);
            log_message(nice_msg);
        }
        return;
    }

    // 主要线程策略：直接应用配置文件中指定的策略和优先级
    char log_msg[LOG_MSG_MAX];
    snprintf(log_msg, sizeof(log_msg),
        "应用配置策略于线程 %s (TID: %d)，主要线程，策略=%d，优先级=%d",
        effective_ti->name, effective_ti->tid, effective_ti->sched_policy, effective_ti->priority);
    log_message(log_msg);

    switch (effective_ti->sched_policy) {
    case SCHED_POLICY_FIFO:
    case SCHED_POLICY_RR: {
        // 直接使用配置文件中指定的优先级，不进行任何调整
        struct sched_param param;
        param.sched_priority = effective_ti->priority;

        int policy = (effective_ti->sched_policy == SCHED_POLICY_FIFO) ?
            SCHED_FIFO : SCHED_RR;

        if (sched_setscheduler(effective_ti->tid, policy, &param) == -1) {
            if (errno != ESRCH) {
                char err_msg[LOG_MSG_MAX];
                snprintf(err_msg, sizeof(err_msg),
                    "设置调度策略失败 tid %d, policy %d, priority %d: %s",
                    effective_ti->tid, policy, effective_ti->priority, strerror(errno));
                log_message(err_msg);
            }
        } else {
            char success_msg[LOG_MSG_MAX];
            snprintf(success_msg, sizeof(success_msg),
                "成功设置线程 %s (TID: %d) 调度策略=%s，优先级=%d",
                effective_ti->name, effective_ti->tid, 
                (policy == SCHED_FIFO) ? "SCHED_FIFO" : "SCHED_RR",
                effective_ti->priority);
            log_message(success_msg);
        }
        break;
    }

    case SCHED_POLICY_UCLAMP: {
        char path[128];
        char value[16];

        // 直接使用配置文件中指定的UCLAMP值
        snprintf(path, sizeof(path), "/proc/%d/task/%d/uclamp.min", getpid(), effective_ti->tid);
        snprintf(value, sizeof(value), "%d", effective_ti->uclamp_min);
        if (!write_file(AT_FDCWD, path, value, O_WRONLY) && errno != ENOENT) {
            if (errno != ESRCH) {
                char err_msg[LOG_MSG_MAX];
                snprintf(err_msg, sizeof(err_msg),
                    "设置uclamp.min失败 tid %d, value %d: %s",
                    effective_ti->tid, effective_ti->uclamp_min, strerror(errno));
                log_message(err_msg);
            }
        }

        snprintf(path, sizeof(path), "/proc/%d/task/%d/uclamp.max", getpid(), effective_ti->tid);
        snprintf(value, sizeof(value), "%d", effective_ti->uclamp_max);
        if (!write_file(AT_FDCWD, path, value, O_WRONLY) && errno != ENOENT) {
            if (errno != ESRCH) {
                char err_msg[LOG_MSG_MAX];
                snprintf(err_msg, sizeof(err_msg),
                    "设置uclamp.max失败 tid %d, value %d: %s",
                    effective_ti->tid, effective_ti->uclamp_max, strerror(errno));
                log_message(err_msg);
            }
        } else {
            char success_msg[LOG_MSG_MAX];
            snprintf(success_msg, sizeof(success_msg),
                "成功设置线程 %s (TID: %d) UCLAMP min=%d, max=%d",
                effective_ti->name, effective_ti->tid, 
                effective_ti->uclamp_min, effective_ti->uclamp_max);
            log_message(success_msg);
        }
        break;
    }

    case SCHED_POLICY_NICE: {
        // 直接使用配置文件中指定的NICE值
        if (setpriority(PRIO_PROCESS, effective_ti->tid, effective_ti->nice_value) == -1) {
            if (errno != ESRCH) {
                char err_msg[LOG_MSG_MAX];
                snprintf(err_msg, sizeof(err_msg),
                    "设置NICE失败 tid %d, value %d: %s",
                    effective_ti->tid, effective_ti->nice_value, strerror(errno));
                log_message(err_msg);
            }
        } else {
            char success_msg[LOG_MSG_MAX];
            snprintf(success_msg, sizeof(success_msg),
                "成功设置线程 %s (TID: %d) NICE=%d",
                effective_ti->name, effective_ti->tid, effective_ti->nice_value);
            log_message(success_msg);
        }
        break;
    }

    default:
        break;
    }
}

// 前向声明
static void apply_affinity_internal(ProcCache* cache, const CpuTopology* topo);

// 异步CPU亲和性处理线程
static void* async_affinity_thread(void* arg) {
    AsyncAffinityContext* ctx = (AsyncAffinityContext*)arg;
    
    while (true) {
        pthread_mutex_lock(&ctx->mutex);
        
        // 等待工作或退出信号
        while (!ctx->has_work && !ctx->should_exit) {
            pthread_cond_wait(&ctx->cond, &ctx->mutex);
        }
        
        if (ctx->should_exit) {
            pthread_mutex_unlock(&ctx->mutex);
            break;
        }
        
        // 复制数据以避免长时间持有锁
        ProcCache cache_copy = *ctx->cache;
        CpuTopology topo_copy = *ctx->topo;
        ctx->has_work = false;
        
        pthread_mutex_unlock(&ctx->mutex);
        
        // 执行实际的亲和性设置（不持有锁）
        apply_affinity_internal(&cache_copy, &topo_copy);
    }
    
    return NULL;
}

// 初始化异步处理
static bool init_async_affinity() {
    if (pthread_mutex_init(&async_context.mutex, NULL) != 0) {
        log_error("初始化异步处理互斥锁失败");
        return false;
    }
    
    if (pthread_cond_init(&async_context.cond, NULL) != 0) {
        pthread_mutex_destroy(&async_context.mutex);
        log_error("初始化异步处理条件变量失败");
        return false;
    }
    
    async_context.should_exit = false;
    async_context.has_work = false;
    
    if (pthread_create(&affinity_thread, NULL, async_affinity_thread, &async_context) != 0) {
        pthread_cond_destroy(&async_context.cond);
        pthread_mutex_destroy(&async_context.mutex);
        log_error("创建异步处理线程失败");
        return false;
    }
    
    log_info("异步亲和性处理线程已启动");
    return true;
}

// 清理异步处理
static void cleanup_async_affinity() {
    if (!async_processing_enabled) return;
    
    pthread_mutex_lock(&async_context.mutex);
    async_context.should_exit = true;
    pthread_cond_signal(&async_context.cond);
    pthread_mutex_unlock(&async_context.mutex);
    
    pthread_join(affinity_thread, NULL);
    pthread_cond_destroy(&async_context.cond);
    pthread_mutex_destroy(&async_context.mutex);
    
    log_info("异步亲和性处理线程已停止");
}

// 触发异步亲和性处理
static void trigger_async_affinity(ProcCache* cache, const CpuTopology* topo) {
    if (!async_processing_enabled) {
        apply_affinity_internal(cache, topo);
        return;
    }
    
    pthread_mutex_lock(&async_context.mutex);
    
    // 如果已经有待处理的工作，跳过（避免积压）
    if (async_context.has_work) {
        pthread_mutex_unlock(&async_context.mutex);
        return;
    }
    
    async_context.cache = cache;
    async_context.topo = (CpuTopology*)topo;
    async_context.has_work = true;
    
    pthread_cond_signal(&async_context.cond);
    pthread_mutex_unlock(&async_context.mutex);
}

// 重命名原来的apply_affinity为apply_affinity_internal
static void apply_affinity_internal(ProcCache* cache, const CpuTopology* topo) {
    static struct timespec last_apply = { 0, 0 };
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    // 使用常量进行节流控制
    long long elapsed_ns = (now.tv_sec - last_apply.tv_sec) * 1000000000LL + (now.tv_nsec - last_apply.tv_nsec);
    if (elapsed_ns < APPLY_THROTTLE_NS) {
        return;
    }
    last_apply = now;

    for (size_t i = 0; i < cache->num_procs; i++) {
        ProcessInfo* proc = &cache->procs[i];

        // 注意：移除了check_thread_lifetime调用，不再进行临时降级

        for (size_t j = 0; j < proc->num_threads; j++) {
            const ThreadInfo* ti = &proc->threads[j];
            if (topo->cpuset_enabled && topo->base_cpuset_fd != -1) {
                char tid_str[32];
                snprintf(tid_str, sizeof(tid_str), "%d\n", ti->tid);
                if (CPU_COUNT(&ti->cpus) == 0) {
                    cpu_set_t curr;
                    if (sched_getaffinity(ti->tid, sizeof(curr), &curr) == -1) continue;
                    if (CPU_EQUAL(&topo->present_cpus, &curr)) continue;
                    write_file(topo->base_cpuset_fd, "tasks", tid_str, O_WRONLY | O_APPEND);
                }
                else {
                    cpu_set_t curr;
                    if (sched_getaffinity(ti->tid, sizeof(curr), &curr) == -1) continue;
                    if (CPU_EQUAL(&ti->cpus, &curr)) continue;
                    if (ti->cpuset_dir[0]) {
                        int fd = openat(topo->base_cpuset_fd, ti->cpuset_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
                        if (fd != -1) {
                            write_file(fd, "tasks", tid_str, O_WRONLY | O_APPEND);
                            close(fd);
                        }
                    }
                }
            }
            if (CPU_COUNT(&ti->cpus) == 0) continue;
            if (sched_setaffinity(ti->tid, sizeof(ti->cpus), &ti->cpus) == -1 && errno == ESRCH) {
                cache->last_proc_count = 0;
            }

            // 应用调度策略
            apply_scheduling_policy(ti);
        }
    }
}

// 新的apply_affinity函数，使用异步处理
static void apply_affinity(ProcCache* cache, const CpuTopology* topo) {
    trigger_async_affinity(cache, topo);
}

static void config_release(AppConfig* cfg) {
    if (!cfg) return;
    if (atomic_fetch_sub(&cfg->ref_count, 1) == 1) {
        if (cfg->rules) free(cfg->rules);
        if (cfg->pkgs) {
            // 修复：将 i 改为 i++
            for (size_t i = 0; i < cfg->num_pkgs; i++) free(cfg->pkgs[i]);
            free(cfg->pkgs);
        }
        if (cfg->topo.base_cpuset_fd != -1) close(cfg->topo.base_cpuset_fd);
        free(cfg);
    }
}

static AppConfig* get_config() {
    AppConfig* cfg = atomic_load_explicit(&current_config, memory_order_acquire);
    if (!cfg) return NULL;
    int old_ref = atomic_fetch_add_explicit(&cfg->ref_count, 1, memory_order_acq_rel);
    if (old_ref <= 0) {
        atomic_fetch_sub_explicit(&cfg->ref_count, 1, memory_order_release);
        return NULL;
    }
    if (atomic_load_explicit(&current_config, memory_order_acquire) != cfg) {
        atomic_fetch_sub_explicit(&cfg->ref_count, 1, memory_order_release);
        return NULL;
    }
    return cfg;
}

static void* config_loader_thread(void* arg) {
    int interval = *(int*)arg;
    free(arg);
    pthread_setname_np(pthread_self(), "ConfigLoader");

    time_t last_mtime = -1;
    while (1) {
        if (inotify_supported) {
            fd_set rfds;
            struct timeval tv;
            FD_ZERO(&rfds);
            FD_SET(inotify_fd, &rfds);
            tv.tv_sec = interval;
            tv.tv_usec = 0;

            int ret = select(inotify_fd + 1, &rfds, NULL, NULL, &tv);
            if (ret < 0) {
                if (errno == EINTR) continue;
                inotify_supported = 0;
                close(inotify_fd);
                inotify_fd = -1;
                continue;
            }
            else if (ret == 0) {
                continue;
            }

            char buf[4096] __attribute__((aligned(8)));
            ssize_t len = read(inotify_fd, buf, sizeof(buf));
            if (len <= 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    inotify_supported = 0;
                    close(inotify_fd);
                    inotify_fd = -1;
                }
                continue;
            }

            bool reload_needed = false;
            for (char* p = buf; p < buf + len;) {
                struct inotify_event* event = (struct inotify_event*)p;
                if (event->mask & (IN_CLOSE_WRITE | IN_DELETE_SELF | IN_MOVE_SELF)) {
                    reload_needed = true;

                    if (event->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) {
                        usleep(500000);
                        AppConfig* cfg = get_config();
                        if (cfg) {
                            inotify_rm_watch(inotify_fd, inotify_wd);
                            for (int retry = 0; retry < 3; retry++) {
                                inotify_wd = inotify_add_watch(inotify_fd, cfg->config_file,
                                    IN_CLOSE_WRITE | IN_DELETE_SELF | IN_MOVE_SELF);
                                if (inotify_wd >= 0) break;
                                sleep(1);
                            }
                            last_mtime = -1;
                            config_release(cfg);
                        }
                        if (inotify_wd < 0) {
                            inotify_supported = 0;
                            close(inotify_fd);
                            inotify_fd = -1;
                            break;
                        }
                    }
                }
                p += sizeof(struct inotify_event) + event->len;
            }

            if (reload_needed) {
                last_mtime = -1;
                AppConfig* cfg = get_config();
                if (cfg) {
                    AppConfig* new_config = load_config(cfg->config_file, &cfg->topo, &last_mtime);
                    if (new_config) {
                        AppConfig* old_config = atomic_exchange(&current_config, new_config);
                        atomic_store(&config_updated, 1);
                        if (old_config) config_release(old_config);
                    }
                    config_release(cfg);
                }
            }
        }
        else {
            AppConfig* cfg = get_config();
            if (cfg) {
                AppConfig* new_config = load_config(cfg->config_file, &cfg->topo, &last_mtime);
                if (new_config) {
                    AppConfig* old_config = atomic_exchange(&current_config, new_config);
                    atomic_store(&config_updated, 1);
                    if (old_config) config_release(old_config);
                }
                config_release(cfg);
            }
            sleep(interval);
        }
    }
    return NULL;
}

static void cleanup(void) {
    AppConfig* cfg = atomic_exchange(&current_config, NULL);
    if (cfg) config_release(cfg);

    if (inotify_supported) {
        if (inotify_wd >= 0) inotify_rm_watch(inotify_fd, inotify_wd);
        close(inotify_fd);
    }

    if (global_cache.procs) {
        for (size_t i = 0; i < global_cache.num_procs; i++) {
            free_process_info(&global_cache.procs[i]);
        }
        free(global_cache.procs);
    }

    if (global_cache.tracked_pids) {
        free(global_cache.tracked_pids);
    }

    global_cache.procs = NULL;
    global_cache.tracked_pids = NULL;
    global_cache.num_procs = 0;
    global_cache.num_tracked_pids = 0;
    
    // 清理全局线程跟踪器
    if (global_cache.global_trackers) {
        free(global_cache.global_trackers);
        global_cache.global_trackers = NULL;
    }
    global_cache.num_global_trackers = 0;
    global_cache.global_trackers_cap = 0;
    
    // 清理异步CPU亲和性处理
    cleanup_async_affinity();
    
    // 清理进程状态缓存
    cleanup_process_cache();
}

static void print_help(const char* prog_name) {
    printf("AppOpt - 应用程序CPU优化工具 v%s\n", VERSION);
    printf("用法: %s [选项]\n", prog_name);
    printf("选项:\n");
    printf("  -c <配置文件>   指定配置文件路径 (默认: ./applist.conf)\n");
    printf("  -s <间隔>       设置检查间隔(秒) (必须>=1, 默认: 2)\n");
    printf("  -v              显示程序版本\n");
    printf("  -h              显示帮助信息\n");
    printf("\n日志控制:\n");
    printf("  环境变量 APPOPT_LOG_LEVEL 控制日志输出级别:\n");
    printf("    0 = 无日志输出 (默认)\n");
    printf("    1 = 仅错误日志\n");
    printf("    2 = 信息和错误日志\n");
    printf("    3 = 所有日志(调试)\n");
    printf("  示例: export APPOPT_LOG_LEVEL=1\n");
    printf("\n配置文件格式:\n");
    printf("  包名[.{线程名模式}] = CPU列表 [: 调度策略 参数]\n");
    printf("  调度策略:\n");
    printf("    SCHED_FIFO <优先级>  实时先进先出策略 (1-99)\n");
    printf("    SCHED_RR <优先级>    实时轮转策略 (1-99)\n");
    printf("    UCLAMP <最小> <最大> 利用率钳制 (0-100)\n");
    printf("    NICE <值>            Nice值 (-20到19)\n");
    printf("\n示例:\n");
    printf("  com.game.app{render*} = 4-7 : SCHED_RR 80\n");
    printf("  com.game.app{audio*} = 6-7 : UCLAMP 50 80\n");
    printf("  com.game.app = 0-7 : NICE -5\n");
}

int main(int argc, char** argv) {
    atexit(cleanup);
    
    // 初始化日志级别
    init_log_level();
    
    // 初始化进程状态缓存
    init_process_cache();

    // 初始化异步CPU亲和性处理
    if (!init_async_affinity()) {
        fprintf(stderr, "异步处理初始化失败\n");
        exit(EXIT_FAILURE);
    }

    CpuTopology topo = init_cpu_topo();
    char config_file[4096] = "./applist.conf";
    int sleep_interval = 2;
    int opt;

    while ((opt = getopt(argc, argv, "c:s:hv")) != -1) {
        switch (opt) {
        case 'c':
            strncpy(config_file, optarg, sizeof(config_file) - 1);
            printf("使用配置文件: %s\n", config_file);
            break;
        case 's':
        {
            char* endptr;
            long val = strtol(optarg, &endptr, 10);
            if (endptr == optarg || *endptr != '\0' || val < 1) {
                fprintf(stderr, "无效的时间间隔: %s\n", optarg);
                fprintf(stderr, "间隔必须是 >=1 的整数\n");
                exit(EXIT_FAILURE);
            }
            sleep_interval = (int)val;
            printf("检查间隔: %d 秒\n", sleep_interval);
            break;
        }
        case 'v':
            printf("AppOpt 版本 %s\n", VERSION);
            exit(EXIT_SUCCESS);
        case 'h':
            print_help(argv[0]);
            exit(EXIT_SUCCESS);
        default:
            print_help(argv[0]);
            exit(EXIT_FAILURE);
        }
    }

    struct stat st;
    if (stat(config_file, &st) != 0) {
        const char* initial_content =
            "# AppOpt 配置文件\n"
            "# 格式: 包名[.{线程名模式}] = CPU列表 [: 调度策略 参数]\n"
            "# 调度策略:\n"
            "#   SCHED_FIFO <优先级>  实时先进先出策略 (1-99)\n"
            "#   SCHED_RR <优先级>    实时轮转策略 (1-99)\n"
            "#   UCLAMP <最小> <最大> 利用率钳制 (0-100)\n"
            "#   NICE <值>            Nice值 (-20到19)\n"
            "#\n"
            "# 示例:\n"
            "com.tencent.tmgp.sgame{UnityMain*}=7 : SCHED_RR 90\n"
            "com.tencent.tmgp.sgame{UnityGfx*}=2-4 : SCHED_RR 80\n"
            "com.tencent.tmgp.sgame{Thread*}=2-6 : UCLAMP 30 70\n"
            "com.tencent.tmgp.sgame{Audio*}=2-4 : UCLAMP 50 80\n"
            "com.tencent.tmgp.sgame=2-4 : UCLAMP 20 60\n";

        if (write_file(AT_FDCWD, config_file, initial_content, O_WRONLY | O_CREAT | O_TRUNC)) {
            printf("创建新的配置文件: %s\n", config_file);
        }
        else {
            fprintf(stderr, "警告: 无法创建配置文件 %s，尝试使用现有配置\n", config_file);
            // 不退出，尝试加载现有配置文件
        }
    }

    AppConfig* initial_config = load_config(config_file, &topo, NULL);
    if (!initial_config) {
        fprintf(stderr, "初始配置加载失败\n");
        exit(EXIT_FAILURE);
    }
    atomic_store(&current_config, initial_config);
    atomic_store(&config_updated, 1);

    inotify_fd = inotify_init1(IN_CLOEXEC);
    if (inotify_fd >= 0) {
        int flags = fcntl(inotify_fd, F_GETFL);
        if (flags >= 0) fcntl(inotify_fd, F_SETFL, flags | O_NONBLOCK);
        inotify_wd = inotify_add_watch(inotify_fd, config_file, IN_CLOSE_WRITE | IN_DELETE_SELF | IN_MOVE_SELF);
        if (inotify_wd >= 0) {
            inotify_supported = 1;
            printf("启用配置文件监控\n");
        }
        else {
            close(inotify_fd);
            inotify_fd = -1;
        }
    }

    pthread_t loader_thread;
    int* interval_ptr = (int*)malloc(sizeof(int));
    if (!interval_ptr) {
        perror("内存分配失败");
        cleanup();
        exit(EXIT_FAILURE);
    }
    *interval_ptr = sleep_interval;

    if (pthread_create(&loader_thread, NULL, config_loader_thread, interval_ptr) != 0) {
        perror("配置加载器线程创建失败");
        free(interval_ptr);
        cleanup();
        exit(EXIT_FAILURE);
    }
    pthread_detach(loader_thread);

    int affinity_counter = 0;
    printf("启动AppOpt服务 v%s\n", VERSION);
    log_message("AppOpt服务启动");

    for (;;) {
        if (atomic_exchange(&config_updated, 0)) {
            global_cache.scan_all_proc = true;
            global_cache.last_proc_count = 0;
        }

        AppConfig* cfg = get_config();
        if (cfg) {
            update_cache(&global_cache, cfg, &affinity_counter);
            affinity_counter--;
            if (affinity_counter < 1) {
                apply_affinity(&global_cache, &cfg->topo);
                affinity_counter = 5;
            }
            config_release(cfg);
        }
        sleep(sleep_interval);
    }
}