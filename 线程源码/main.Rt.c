#define _GNU_SOURCE
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
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <time.h>
#include <unistd.h>
#include <stdarg.h>
#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include "monitor.skel.h"
#include "uthash.h"

#define VERSION            "1.3.5-Ebpf-Prio"
#define BASE_CPUSET        "/dev/cpuset/system-control-apps"
#define MAX_PKG_LEN        128
#define MAX_THREAD_LEN     32
#define INITIAL_RULE_CAPACITY 8192
#define INITIAL_WILDCARD_CAPACITY 256
#define INITIAL_PRIORITY_CAPACITY 256
#define DENT_BUF_SIZE      (128 * 1024)
#define PERIODIC_FULL_SCAN_INTERVAL   60
#define DEAD_CLEANUP_INTERVAL         15
#define LOW_FREQ_THRESHOLD     25
#define HIGH_FREQ_THRESHOLD    50
#define CRITICAL_FREQ_THRESHOLD 90
#define MAX_EVENT_COUNT        200

#define LOG_I(fmt, ...) do { write_log("[I] " fmt, ##__VA_ARGS__); } while (0)
#define LOG_W(fmt, ...) do { write_log("[W] " fmt, ##__VA_ARGS__); } while (0)
#define LOG_E(fmt, ...) do { write_log("[E] " fmt, ##__VA_ARGS__); } while (0)

struct event {
    int pid;
    int tid;
    char comm[16];
    int type;
};

typedef struct {
    char pkg[MAX_PKG_LEN];
    char thread[MAX_THREAD_LEN];
    char cpuset_dir[256];
    cpu_set_t cpus;
    bool is_wildcard;
    int priority;
} AffinityRule;

typedef struct {
    char pkg[MAX_PKG_LEN];
    char thread[MAX_THREAD_LEN];
    int policy;
    int priority;
} PriorityRule;

typedef struct {
    cpu_set_t present_cpus;
    char present_str[128];
    char mems_str[32];
    bool cpuset_enabled;
    int base_cpuset_fd;
} CpuTopology;

typedef struct PackageEntry {
    char pkg[MAX_PKG_LEN];
    UT_hash_handle hh;
} PackageEntry;

typedef struct RuleNode {
    AffinityRule *rule;
    struct RuleNode *next;
} RuleNode;

typedef struct ThreadCacheEntry {
    char thread[MAX_THREAD_LEN];
    const AffinityRule *rule;
    UT_hash_handle hh;
} ThreadCacheEntry;

typedef struct {
    char pkg[MAX_PKG_LEN];
    RuleNode *rules;
    bool has_thread_rules;
    ThreadCacheEntry *thread_cache;
    UT_hash_handle hh;
} PkgRulesEntry;

typedef struct {
    atomic_int ref_count;
    AffinityRule* rules;
    size_t num_rules;
    AffinityRule** wildcard_rules;
    size_t num_wildcard_rules;
    time_t mtime;
    CpuTopology topo;
    struct PackageEntry* pkg_table;
    PkgRulesEntry *pkg_rules;
    char config_file[4096];
    char cpuset_base[256];
    PriorityRule* priority_rules;
    size_t num_priority_rules;
    char priority_config[4096];
    time_t p_mtime;
} AppConfig;

typedef struct {
    pid_t pid;
    char pkg[MAX_PKG_LEN];
    UT_hash_handle hh;
} PidCacheEntry;

typedef struct {
    pid_t tid;
    char last_comm[MAX_THREAD_LEN];
    UT_hash_handle hh;
} TidCommEntry;

typedef struct {
    pid_t pid;
    TidCommEntry *tid_comm_table;
    bool initial_scan_done;
    int event_count;
    time_t last_scan_time;
    int consecutive_changes;
    UT_hash_handle hh;
} ProcessTidCache;

typedef struct {
    time_t last_full_scan_time;
    time_t last_dead_cleanup_time;
} GlobalStatus;

struct linux_dirent64 {
    ino64_t d_ino; off64_t d_off; unsigned short d_reclen;
    unsigned char d_type; char d_name[];
};

static const struct { const char* name; int policy; } policy_map[] = {
    {"SCHED_OTHER", SCHED_OTHER}, {"SCHED_FIFO", SCHED_FIFO}, {"SCHED_RR", SCHED_RR}, {NULL, 0}
};

static _Atomic(AppConfig*) current_config = NULL;
static PidCacheEntry *pid_cache = NULL;
static ProcessTidCache *process_cache = NULL;
static GlobalStatus g_status = {0};
static bool monitor_affinity = false;

static void write_log(const char *fmt, ...) {
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char time_str[20];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);
    fprintf(stderr, "[%s] ", time_str);
    va_list args; va_start(args, fmt);
    vfprintf(stderr, fmt, args); va_end(args);
    fflush(stderr);
}

static char* strtrim(char* s) {
    char* end;
    while (isspace(*s)) s++;
    if (*s == 0) return s;
    end = s + strlen(s) - 1;
    while (end > s && isspace(*end)) end--;
    *(end + 1) = 0;
    return s;
}

static char* strtrim_line(char* s) {
    char* start = s;
    while (isspace((unsigned char)*start)) start++;
    if (!*start) return start;
    char* end = start + strlen(start) - 1;
    while (end > start && (isspace((unsigned char)*end) || *end == '#')) end--;
    end[1] = '\0';
    return start;
}

static bool read_file(int dir_fd, const char* filename, char* buf, size_t buf_size) {
    int fd = openat(dir_fd, filename, O_RDONLY | O_CLOEXEC);
    if (fd == -1) return false;
    ssize_t n = read(fd, buf, buf_size - 1);
    close(fd);
    if (n <= 0) return false;
    buf[n] = '\0'; return true;
}

static bool write_file(int dir_fd, const char* filename, const char* content, int flags) {
    int fd = openat(dir_fd, filename, flags | O_CLOEXEC, 0644);
    if (fd == -1) return false;
    ssize_t n = write(fd, content, strlen(content));
    close(fd);
    return (n == (ssize_t)strlen(content));
}

static int build_str(char *dest, size_t dest_size, ...) {
    va_list args; const char *segment; char *p = dest;
    size_t remaining = dest_size - 1;
    va_start(args, dest_size);
    while ((segment = va_arg(args, const char *)) != NULL) {
        size_t len = strlen(segment);
        if (len > remaining) { va_end(args); return 0; }
        memcpy(p, segment, len); p += len; remaining -= len;
    }
    *p = '\0'; va_end(args); return 1;
}

static bool parse_cpu_ranges(const char* spec, cpu_set_t* set, const cpu_set_t* present, char* invalid_buf) {
    if (!spec) return true;
    char* copy = strdup(spec); if (!copy) return false;
    char* s = copy; bool all_valid = true;
    while (*s) {
        char* end;
        unsigned long a = strtoul(s, &end, 0);
        if (end == s) { s++; continue; }
        unsigned long b = a;
        if (*end == '-') { s = end + 1; b = strtoul(s, &end, 10); if (end == s) b = a; }
        for (unsigned long i = a; i <= b && i < CPU_SETSIZE; i++) {
            if (present && !CPU_ISSET(i, present)) {
                if (invalid_buf) sprintf(invalid_buf, "%lu", i);
                all_valid = false; continue;
            }
            CPU_SET(i, set);
        }
        s = (*end == ',') ? end + 1 : end;
    }
    free(copy); return all_valid;
}

static char* cpu_set_to_str(const cpu_set_t *set) {
    size_t buf_size = 8 * CPU_SETSIZE; char *buf = malloc(buf_size); if (!buf) return NULL;
    int start = -1, end = -1; char *p = buf; size_t remain = buf_size - 1; bool first = true;
    for (int i = 0; i < CPU_SETSIZE; i++) {
        if (CPU_ISSET(i, set)) {
            if (start == -1) start = end = i;
            else if (i == end + 1) end = i;
            else {
                int needed = (start == end) ? snprintf(p, remain + 1, "%s%d", first ? "" : ",", start) : snprintf(p, remain + 1, "%s%d-%d", first ? "" : ",", start, end);
                p += needed; remain -= needed; start = end = i; first = false;
            }
        }
    }
    if (start != -1) {
        if (start == end) snprintf(p, remain + 1, "%s%d", first ? "" : ",", start);
        else snprintf(p, remain + 1, "%s%d-%d", first ? "" : ",", start, end);
    }
    return buf;
}

static bool create_cpuset_dir(const char *path, const char *cpus, const char *mems) {
    if (mkdir(path, 0755) != 0 && errno != EEXIST) return false;
    chmod(path, 0755); chown(path, 0, 0);
    char cpus_path[256], mems_path[256];
    build_str(cpus_path, sizeof(cpus_path), path, "/cpus", NULL);
    write_file(AT_FDCWD, cpus_path, cpus, O_WRONLY | O_CREAT | O_TRUNC);
    build_str(mems_path, sizeof(mems_path), path, "/mems", NULL);
    write_file(AT_FDCWD, mems_path, mems, O_WRONLY | O_CREAT | O_TRUNC);
    return true;
}

static CpuTopology init_cpu_topo(void) {
    CpuTopology topo = { .cpuset_enabled = false, .base_cpuset_fd = -1 };
    CPU_ZERO(&topo.present_cpus);
    if (read_file(AT_FDCWD, "/sys/devices/system/cpu/present", topo.present_str, sizeof(topo.present_str))) strtrim(topo.present_str);
    parse_cpu_ranges(topo.present_str, &topo.present_cpus, NULL, NULL);
    if (access("/dev/cpuset", F_OK) == 0) {
        if (create_cpuset_dir(BASE_CPUSET, topo.present_str, "0")) {
            topo.base_cpuset_fd = open(BASE_CPUSET, O_RDONLY | O_DIRECTORY);
            if (topo.base_cpuset_fd != -1) topo.cpuset_enabled = true;
        }
        char mem_p[256]; build_str(mem_p, 256, BASE_CPUSET, "/mems", NULL);
        if (!read_file(AT_FDCWD, mem_p, topo.mems_str, 32)) strcpy(topo.mems_str, "0"); else strtrim(topo.mems_str);
    }
    return topo;
}

static int calculate_rule_priority(const char* thread_pattern) {
    if (!thread_pattern || !*thread_pattern) return 200;
    size_t len = strlen(thread_pattern);
    const char* p = thread_pattern;
    if (strchr(p, '*') == NULL && strchr(p, '?') == NULL && strchr(p, '[') == NULL) {
        return 1000 + (int)len;
    }
    int non_wildcard_chars = 0;
    bool has_range = false, has_single_wildcard = false, has_star = false;
    while (*p) {
        if (*p == '[') has_range = true;
        else if (*p == '?') has_single_wildcard = true;
        else if (*p == '*') has_star = true;
        else non_wildcard_chars++;
        p++;
    }
    if (has_range) return 500 + non_wildcard_chars;
    else if (has_single_wildcard) return 300 + non_wildcard_chars;
    else if (has_star) return 100 + non_wildcard_chars;
    return 200;
}

static AppConfig* get_config(void) {
    AppConfig* cfg = atomic_load_explicit(&current_config, memory_order_acquire);
    if (cfg) atomic_fetch_add(&cfg->ref_count, 1);
    return cfg;
}

static void build_thread_cache(AppConfig *cfg) {
    PkgRulesEntry *entry, *tmp;
    HASH_ITER(hh, cfg->pkg_rules, entry, tmp) {
        RuleNode *curr = entry->rules;
        while (curr) {
            AffinityRule *r = curr->rule;
            if (r->thread[0] && !strchr(r->thread, '*') && !strchr(r->thread, '?') && !strchr(r->thread, '[')) {
                ThreadCacheEntry *t_entry;
                HASH_FIND_STR(entry->thread_cache, r->thread, t_entry);
                if (!t_entry) {
                    t_entry = calloc(1, sizeof(ThreadCacheEntry));
                    strncpy(t_entry->thread, r->thread, MAX_THREAD_LEN - 1);
                    t_entry->rule = r;
                    HASH_ADD_STR(entry->thread_cache, thread, t_entry);
                }
            }
            curr = curr->next;
        }
    }
}

static void free_pkg_rules(PkgRulesEntry **pkg_rules) {
    PkgRulesEntry *curr, *tmp;
    HASH_ITER(hh, *pkg_rules, curr, tmp) {
        HASH_DEL(*pkg_rules, curr);
        RuleNode *node = curr->rules;
        while (node) {
            RuleNode *next_node = node->next;
            free(node);
            node = next_node;
        }
        ThreadCacheEntry *t_curr, *t_tmp;
        HASH_ITER(hh, curr->thread_cache, t_curr, t_tmp) {
            HASH_DEL(curr->thread_cache, t_curr);
            free(t_curr);
        }
        free(curr);
    }
    *pkg_rules = NULL;
}

static void config_release(AppConfig* cfg) {
    if (cfg && atomic_fetch_sub(&cfg->ref_count, 1) == 1) {
        free(cfg->rules);
        free(cfg->wildcard_rules);
        free(cfg->priority_rules);
        PackageEntry *e, *tmp;
        HASH_ITER(hh, cfg->pkg_table, e, tmp) {
            HASH_DEL(cfg->pkg_table, e);
            free(e);
        }
        free_pkg_rules(&cfg->pkg_rules);
        if (cfg->topo.base_cpuset_fd != -1) close(cfg->topo.base_cpuset_fd);
        free(cfg);
    }
}

static void load_priority_config(AppConfig* cfg) {
    if (cfg->priority_config[0] == '\0') return;
    struct stat st;
    if (stat(cfg->priority_config, &st) != 0) {
        const char* initial_content = "# 格式：进程名{线程模式}=策略 优先级\n#| 策略名称 | 说明 | 有效优先级范围 |\n#|---------------|-------------------------------|----------------|\n#| SCHED_OTHER | 标准分时策略 (默认) | 0 |\n#| SCHED_FIFO | 实时先进先出策略 | 1-99 |\n#| SCHED_RR | 实时轮转策略 | 1-99 |\n";
        if (write_file(AT_FDCWD, cfg->priority_config, initial_content, O_WRONLY | O_CREAT | O_TRUNC))
            LOG_W("优先级配置文件不存在，创建示例文件: %s\n", cfg->priority_config);
        cfg->priority_rules = NULL;
        cfg->num_priority_rules = 0;
        return;
    }
    FILE* fp = fopen(cfg->priority_config, "r");
    if (!fp) {
        LOG_E("无法打开优先级配置文件 %s: %s\n", cfg->priority_config, strerror(errno));
        return;
    }
    size_t cap = INITIAL_PRIORITY_CAPACITY;
    PriorityRule* new_rules = malloc(cap * sizeof(PriorityRule));
    size_t rules_cnt = 0;
    char line[256];
    size_t ln = 0;

    if (!new_rules) { fclose(fp); return; }
    while (fgets(line, sizeof(line), fp)) {
        ln++;
        char* p = strtrim_line(line);
        if (*p == '#' || !*p) continue;
        char* eq = strchr(p, '=');
        if (!eq) { LOG_W("优先级配置第 %zu 行无效：缺少 '=': %s\n", ln, p); continue; }
        *eq++ = '\0';
        char *key = strtrim(p), *val = strtrim(eq);
        char *comment = strchr(val, '#'); if (comment) *comment = '\0';
        val = strtrim(val);
        if (!*key || !*val) continue;

        char *br = strchr(key, '{'), *thread = "";
        char *pkg_raw = key;
        if (br) {
            *br = '\0';
            pkg_raw = key;
            char* eb = strchr(br + 1, '}');
            if (!eb) { LOG_W("优先级配置第 %zu 行缺少闭合 '}': %s\n", ln, p); continue; }
            *eb = '\0';
            thread = strtrim(br + 1);
        }
        char *pkg = strtrim(pkg_raw), *policy_part = strtrim(val);

        if (strlen(pkg) >= MAX_PKG_LEN || strlen(thread) >= MAX_THREAD_LEN) {
            LOG_W("优先级配置第 %zu 行包名或线程名过长\n", ln); continue;
        }
        char *space = strchr(policy_part, ' ');
        if (!space) { LOG_W("优先级配置第 %zu 行缺少优先级数值\n", ln); continue; }
        *space++ = '\0';
        int policy = -1;
        for (int i = 0; policy_map[i].name; i++)
            if (strcmp(policy_map[i].name, policy_part) == 0) { policy = policy_map[i].policy; break; }
        if (policy == -1) { LOG_W("优先级配置第 %zu 行未知策略: %s\n", ln, policy_part); continue; }
        char *end;
        long prio = strtol(strtrim(space), &end, 10);
        if (*end != '\0') { LOG_W("优先级配置第 %zu 行优先级数值非法: %s\n", ln, space); continue; }

        if ((policy == SCHED_FIFO || policy == SCHED_RR) && (prio < 1 || prio > 99)) {
            LOG_W("优先级配置第 %zu 行实时策略优先级需在 1-99 之间\n", ln); continue;
        }
        if (policy == SCHED_OTHER && prio != 0) {
            LOG_W("优先级配置第 %zu 行 SCHED_OTHER 优先级必须为 0\n", ln); continue;
        }
        bool is_dup = false;
        for (size_t i = 0; i < rules_cnt; i++) {
            if (strcmp(new_rules[i].pkg, pkg) == 0 && strcmp(new_rules[i].thread, thread) == 0) {
                is_dup = true; break;
            }
        }
        if (is_dup) { LOG_W("优先级配置第 %zu 行重复规则，已跳过\n", ln); continue; }
        if (rules_cnt >= cap) {
            cap *= 2;
            PriorityRule* tmp = realloc(new_rules, cap * sizeof(PriorityRule));
            if (!tmp) { free(new_rules); fclose(fp); return; }
            new_rules = tmp;
        }
        PriorityRule* r = &new_rules[rules_cnt++];
        memset(r, 0, sizeof(PriorityRule));
        strncpy(r->pkg, pkg, MAX_PKG_LEN - 1);
        strncpy(r->thread, thread, MAX_THREAD_LEN - 1);
        r->policy = policy;
        r->priority = (int)prio;
    }
    fclose(fp);
    free(cfg->priority_rules);
    cfg->priority_rules = new_rules;
    cfg->num_priority_rules = rules_cnt;
    cfg->p_mtime = st.st_mtime;
    if (rules_cnt > 0) LOG_I("优先级配置文件解析完成，共加载 %zu 条规则\n", rules_cnt);
}

static AppConfig* load_config(const char* config_file, const char* priority_file, const CpuTopology* topo, time_t* last_mtime) {
    struct stat st;
    if (stat(config_file, &st) != 0) {
        const char* init =
            "# 规则编写与使用说明请参考仓库内的 调度整体使用说明.md 与 README.md\n\n";
        if (write_file(AT_FDCWD, config_file, init, O_WRONLY | O_CREAT | O_TRUNC))
            LOG_W("配置文件不存在，创建空的配置文件: %s\n", config_file);
        return NULL;
    }
    if (last_mtime && *last_mtime == st.st_mtime) return NULL;

    FILE *fp = fopen(config_file, "r");
    if (!fp) { LOG_E("无法打开配置文件 %s: %s\n", config_file, strerror(errno)); return NULL; }

    AppConfig* cfg = calloc(1, sizeof(AppConfig));
    if (!cfg) { fclose(fp); return NULL; }
    cfg->ref_count = 1;
    cfg->topo = *topo;
    if (topo->base_cpuset_fd != -1) cfg->topo.base_cpuset_fd = dup(topo->base_cpuset_fd);
    cfg->num_rules = 0;
    cfg->num_wildcard_rules = 0;
    cfg->pkg_rules = NULL;
    build_str(cfg->config_file, sizeof(cfg->config_file), config_file, NULL);
    build_str(cfg->priority_config, sizeof(cfg->priority_config), priority_file, NULL); 
    build_str(cfg->cpuset_base, sizeof(cfg->cpuset_base), BASE_CPUSET, NULL);

    size_t r_cap = INITIAL_RULE_CAPACITY, w_cap = INITIAL_WILDCARD_CAPACITY;
    cfg->rules = malloc(r_cap * sizeof(AffinityRule));
    cfg->wildcard_rules = malloc(w_cap * sizeof(AffinityRule*));
    if (!cfg->rules || !cfg->wildcard_rules) {
        fclose(fp); if (cfg->topo.base_cpuset_fd != -1) close(cfg->topo.base_cpuset_fd);
        free(cfg->rules); free(cfg->wildcard_rules); free(cfg); return NULL;
    }

    char line[512];
    size_t ln = 0;
    while (fgets(line, sizeof(line), fp)) {
        ln++;
        char* p = strtrim_line(line);
        if (!*p || *p == '#') continue;

        char* eq = strchr(p, '=');
        if (!eq) { LOG_W("第 %zu 行无效规则：缺少 '=': %s\n", ln, p); continue; }
        *eq++ = '\0';
        char *key = strtrim(p), *val = strtrim(eq);
        char* comment = strchr(val, '#'); if (comment) *comment = '\0';
        val = strtrim(val);
        if (!*key || !*val) continue;

        char* br = strchr(key, '{'), *thread = "";
        if (br) {
            *br++ = '\0';
            char* eb = strchr(br, '}');
            if (!eb) { LOG_W("第 %zu 行缺少闭合 '}': %s\n", ln, p); continue; }
            *eb = '\0';
            thread = strtrim(br);
        }

        char* pkg = strtrim(key);
        if (strlen(pkg) >= MAX_PKG_LEN || strlen(thread) >= MAX_THREAD_LEN) continue;

        if (cfg->num_rules >= r_cap) {
            r_cap *= 2;
            AffinityRule* tmp_r = realloc(cfg->rules, r_cap * sizeof(AffinityRule));
            if (!tmp_r) goto load_fail;
            cfg->rules = tmp_r;
        }

        AffinityRule* r = &cfg->rules[cfg->num_rules++];
        strncpy(r->pkg, pkg, MAX_PKG_LEN - 1);
        strncpy(r->thread, thread, MAX_THREAD_LEN - 1);
        CPU_ZERO(&r->cpus);

        char inv[64] = {0};
        if (!parse_cpu_ranges(val, &r->cpus, &cfg->topo.present_cpus, inv)) {
            LOG_W("第 %zu 行无效 CPU 范围：%s 规则 %s{%s}=%s\n", ln, inv, pkg, thread, val);
            cfg->num_rules--; continue;
        }
        if (CPU_COUNT(&r->cpus) == 0) { cfg->num_rules--; continue; }

        char* dn = cpu_set_to_str(&r->cpus);
        if (dn) {
            char path[512];
            build_str(path, 512, cfg->cpuset_base, "/", dn, NULL);
            if (!create_cpuset_dir(path, dn, cfg->topo.mems_str)) {
                LOG_W("第 %zu 行无法创建 cpuset %s\n", ln, path);
                free(dn); cfg->num_rules--; continue;
            }
            strncpy(r->cpuset_dir, dn, 255);
            free(dn);
        }

        r->is_wildcard = (strchr(pkg, '*') || strchr(pkg, '?') || strchr(pkg, '['));
        r->priority = calculate_rule_priority(thread[0] ? thread : pkg);

        if (!r->is_wildcard) {
            PkgRulesEntry *entry;
            HASH_FIND_STR(cfg->pkg_rules, pkg, entry);
            if (!entry) {
                entry = calloc(1, sizeof(PkgRulesEntry));
                strncpy(entry->pkg, pkg, MAX_PKG_LEN - 1);
                HASH_ADD_STR(cfg->pkg_rules, pkg, entry);
            }
            if (thread[0] != '\0') {
                entry->has_thread_rules = true;
            }
            RuleNode *node = malloc(sizeof(RuleNode));
            if (node) { node->rule = r; node->next = entry->rules; entry->rules = node; }
        } else {
            if (cfg->num_wildcard_rules >= w_cap) {
                w_cap *= 2;
                AffinityRule** tmp_w = realloc(cfg->wildcard_rules, w_cap * sizeof(AffinityRule*));
                if (!tmp_w) goto load_fail;
                cfg->wildcard_rules = tmp_w;
            }
            cfg->wildcard_rules[cfg->num_wildcard_rules++] = r;
        }

        if (!r->is_wildcard) {
            PackageEntry* pe;
            HASH_FIND_STR(cfg->pkg_table, pkg, pe);
            if (!pe) {
                pe = calloc(1, sizeof(PackageEntry));
                strncpy(pe->pkg, pkg, MAX_PKG_LEN - 1);
                HASH_ADD_STR(cfg->pkg_table, pkg, pe);
            }
        }
    }
    fclose(fp);

    if (cfg->num_rules == 0) {
        LOG_W("从 %s 未加载任何有效规则\n", config_file);
        config_release(cfg);
        return NULL;
    }
    
    load_priority_config(cfg);
    cfg->mtime = st.st_mtime;
    if (last_mtime) *last_mtime = st.st_mtime;
    build_thread_cache(cfg);

    LOG_I("配置文件解析完成，共加载 %zu 条规则，%zu 个精确包名，%zu 条通配符规则\n",
          cfg->num_rules, (size_t)HASH_COUNT(cfg->pkg_table), cfg->num_wildcard_rules);
    return cfg;

load_fail:
    fclose(fp);
    config_release(cfg);
    return NULL;
}

static bool get_process_pkgname(int dir_fd, pid_t pid, char *out, size_t outsz) {
    char path[64], buf[512] = {0};
    int fd;
    if (dir_fd != -1 && dir_fd != AT_FDCWD) {
        snprintf(path, sizeof(path), "%d/cmdline", pid);
        fd = openat(dir_fd, path, O_RDONLY | O_CLOEXEC);
    } else {
        snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
        fd = open(path, O_RDONLY | O_CLOEXEC);
    }
    if (fd == -1) return false;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) {
        if (dir_fd != -1 && dir_fd != AT_FDCWD) {
            snprintf(path, sizeof(path), "%d/comm", pid);
            fd = openat(dir_fd, path, O_RDONLY | O_CLOEXEC);
        } else {
            snprintf(path, sizeof(path), "/proc/%d/comm", pid);
            fd = open(path, O_RDONLY | O_CLOEXEC);
        }
        if (fd == -1) return false;
        n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0) return false;
    }
    buf[n] = '\0';
    char *p = buf;
    char *slash = strrchr(p, '/');
    const char *name = slash ? slash + 1 : p;
    strncpy(out, name, outsz - 1);
    out[outsz - 1] = '\0';
    strtrim(out);
    return out[0] != '\0';
}

static bool get_thread_name(pid_t tid, char *out, size_t outsz) {
    char path[64], buf[MAX_THREAD_LEN] = {0};
    snprintf(path, sizeof(path), "/proc/%d/comm", tid);
    int fd = open(path, O_RDONLY);
    if (fd == -1) return false;
    ssize_t n = read(fd, buf, sizeof(buf)-1);
    close(fd);
    if (n <= 0) return false;
    strncpy(out, buf, outsz - 1);
    strtrim(out);
    return out[0] != '\0';
}

static void update_pid_cache(pid_t pid, const char *pkg) {
    PidCacheEntry *e;
    HASH_FIND_INT(pid_cache, &pid, e);
    if (!e) {
        e = malloc(sizeof(PidCacheEntry));
        e->pid = pid;
        HASH_ADD_INT(pid_cache, pid, e);
    }
    strncpy(e->pkg, pkg, MAX_PKG_LEN - 1);
}

static bool get_from_cache(pid_t pid, char *out) {
    PidCacheEntry *e;
    HASH_FIND_INT(pid_cache, &pid, e);
    if (e) {
        strcpy(out, e->pkg);
        return true;
    }
    return false;
}

static void apply_to_tid(pid_t tid, const AffinityRule *rule, AppConfig *cfg, const char* pkg, const char* thread) {
    if (rule) {
        cpu_set_t curr;
        if (sched_getaffinity(tid, sizeof(curr), &curr) != 0 || !CPU_EQUAL(&rule->cpus, &curr)) {
            sched_setaffinity(tid, sizeof(cpu_set_t), &rule->cpus);
            if (cfg->topo.cpuset_enabled && rule->cpuset_dir[0]) {
                int fd = openat(cfg->topo.base_cpuset_fd, rule->cpuset_dir, O_RDONLY | O_DIRECTORY);
                if (fd != -1) {
                    char tid_str[32];
                    snprintf(tid_str, sizeof(tid_str), "%d\n", tid);
                    write_file(fd, "tasks", tid_str, O_WRONLY | O_APPEND);
                    close(fd);
                }
            }
        }
    }
    if (cfg->num_priority_rules > 0) {
        for (size_t i = 0; i < cfg->num_priority_rules; i++) {
            PriorityRule *pr = &cfg->priority_rules[i];
            if (fnmatch(pr->pkg, pkg, 0) == 0) {
                if (!pr->thread[0] || fnmatch(pr->thread, thread, 0) == 0) {
                    int cur_policy = sched_getscheduler(tid);
                    struct sched_param cur_param;
                    sched_getparam(tid, &cur_param);
                    int target_policy = (pr->policy == SCHED_FIFO || pr->policy == SCHED_RR) ? (pr->policy | SCHED_RESET_ON_FORK) : pr->policy;
                    if (cur_policy != target_policy || cur_param.sched_priority != pr->priority) {
                        struct sched_param param = { .sched_priority = pr->priority };
                        sched_setscheduler(tid, target_policy, &param);
                    }
                    break;
                }
            }
        }
    }
}

static const AffinityRule* find_best_rule(const char *pkg, const char *thread, AppConfig *cfg) {
    PkgRulesEntry *entry;
    HASH_FIND_STR(cfg->pkg_rules, pkg, entry);
    const AffinityRule *best_match = NULL;
    int max_prio = -1;
    const AffinityRule *pkg_fallback = NULL;
    bool skip_thread_check = (thread == NULL || thread[0] == '\0');
    if (entry) {
        if (!skip_thread_check) {
            ThreadCacheEntry *t_cache;
            HASH_FIND_STR(entry->thread_cache, thread, t_cache);
            if (t_cache) return t_cache->rule;
        }
        RuleNode *curr = entry->rules;
        while (curr) {
            AffinityRule *r = curr->rule;
            if (r->thread[0]) {
                if (!skip_thread_check) {
                    if (fnmatch(r->thread, thread, 0) == 0) {
                        if (r->priority > max_prio) {
                            max_prio = r->priority;
                            best_match = r;
                        }
                    }
                }
            } else {
                if (!pkg_fallback || r->priority > pkg_fallback->priority) pkg_fallback = r;
            }
            curr = curr->next;
        }
    }
    for (size_t i = 0; i < cfg->num_wildcard_rules; i++) {
        AffinityRule *wr = cfg->wildcard_rules[i];
        if (fnmatch(wr->pkg, pkg, 0) == 0) {
            if (wr->thread[0]) {
                if (!skip_thread_check) {
                    if (fnmatch(wr->thread, thread, 0) == 0) {
                        if (wr->priority > max_prio) {
                            max_prio = wr->priority;
                            best_match = wr;
                        }
                    }
                }
            } else {
                if (!pkg_fallback || wr->priority > pkg_fallback->priority) pkg_fallback = wr;
            }
        }
    }
    return best_match ? best_match : pkg_fallback;
}

static bool is_interested_package(const char *pkg, AppConfig *cfg) {
    PackageEntry *pe;
    HASH_FIND_STR(cfg->pkg_table, pkg, pe);
    if (pe) return true;
    for (size_t i = 0; i < cfg->num_wildcard_rules; i++) {
        if (fnmatch(cfg->wildcard_rules[i]->pkg, pkg, 0) == 0) return true;
    }
    return false;
}

static void refresh_process_rules(pid_t pid, const char *pkg_name, AppConfig *cfg) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/task", pid);
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd == -1) return;
    PkgRulesEntry *entry = NULL;
    HASH_FIND_STR(cfg->pkg_rules, pkg_name, entry);
    bool has_specific_thread_rules = (entry && entry->has_thread_rules);
    bool has_wildcards = (cfg->num_wildcard_rules > 0);
    char buf[DENT_BUF_SIZE];
    long nread;
    while ((nread = syscall(SYS_getdents64, fd, buf, DENT_BUF_SIZE)) > 0) {
        for (long bpos = 0; bpos < nread; ) {
            struct linux_dirent64 *d = (struct linux_dirent64 *)(buf + bpos);
            const char *d_name = d->d_name;
            if (d_name[0] != '.') {
                pid_t tid = (pid_t)atoi(d_name);
                char t_comm[MAX_THREAD_LEN] = {0};
                const AffinityRule *rule = NULL;
                if (has_specific_thread_rules || has_wildcards) {
                    int c_fd = openat(fd, d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
                    if (c_fd != -1) {
                        int comm_fd = openat(c_fd, "comm", O_RDONLY | O_CLOEXEC);
                        if (comm_fd != -1) {
                            ssize_t n = read(comm_fd, t_comm, sizeof(t_comm) - 1);
                            close(comm_fd);
                            if (n > 0) {
                                t_comm[n] = '\0';
                                if (t_comm[n-1] == '\n') t_comm[n-1] = '\0';
                                rule = find_best_rule(pkg_name, t_comm, cfg);
                            }
                        }
                        close(c_fd);
                    }
                } else {
                    rule = find_best_rule(pkg_name, "", cfg);
                }
                apply_to_tid(tid, rule, cfg, pkg_name, t_comm);
            }
            bpos += d->d_reclen;
        }
    }
    close(fd);
}

static void handle_ebpf_event(void *ctx, int cpu, void *data, unsigned int sz) {
    struct event *ev = data;
    static int last_limit_pid = 0;
    static time_t last_limit_time = 0;
    static int limit_count = 0;
    if (ev->type == 1) {
        time_t now_limit = time(NULL);
        if (ev->pid == last_limit_pid && now_limit == last_limit_time) {
            limit_count++;
            if (limit_count > 5) return;
        } else {
            last_limit_pid = ev->pid;
            last_limit_time = now_limit;
            limit_count = 1;
        }
    }
    AppConfig *cfg = get_config();
    if (!cfg) return;
    char pkg[MAX_PKG_LEN] = {0};
    char t_name[MAX_THREAD_LEN] = {0};
    if (ev->type == 1 || ev->type == 2) {
        int retry = 5;
        while (retry--) {
            if (get_process_pkgname(AT_FDCWD, ev->pid, pkg, sizeof(pkg))) {
                update_pid_cache(ev->pid, pkg);
                if (ev->type == 2) {
                    ProcessTidCache *pc;
                    HASH_FIND_INT(process_cache, &ev->pid, pc);
                    if (pc) pc->initial_scan_done = false;
                }
                break;
            }
            usleep(2000);
        }
    } else {
        if (!get_from_cache(ev->pid, pkg)) get_process_pkgname(AT_FDCWD, ev->pid, pkg, sizeof(pkg));
    }
    if (!pkg[0]) { config_release(cfg); return; }
    if (monitor_affinity && ev->type == 3) {
        refresh_process_rules(ev->pid, pkg, cfg);
        config_release(cfg);
        return;
    }
    PkgRulesEntry *entry = NULL;
    HASH_FIND_STR(cfg->pkg_rules, pkg, entry);
    bool has_thread_rules = (entry && entry->has_thread_rules) || (cfg->num_wildcard_rules > 0);
    if (has_thread_rules) {
        int t_retry = 3;
        while (t_retry--) {
            if (get_thread_name(ev->tid, t_name, sizeof(t_name))) break;
            usleep(1000);
        }
    }
    ProcessTidCache *proc;
    HASH_FIND_INT(process_cache, &ev->pid, proc);
    if (!proc) {
        proc = calloc(1, sizeof(ProcessTidCache));
        proc->pid = ev->pid;
        proc->last_scan_time = time(NULL);
        HASH_ADD_INT(process_cache, pid, proc);
    }
    time_t now = time(NULL);
    int interval = (int)(now - proc->last_scan_time);
    bool should_scan = false;
    if (!proc->initial_scan_done) should_scan = true;
    else if (interval >= 1) {
        int freq = proc->event_count / (interval > 0 ? interval : 1);
        if (freq < CRITICAL_FREQ_THRESHOLD) {
            if (freq > HIGH_FREQ_THRESHOLD) {
                proc->consecutive_changes++;
                if (proc->consecutive_changes > 3 && proc->event_count > MAX_EVENT_COUNT) should_scan = true;
                else should_scan = true;
            } else if (freq > LOW_FREQ_THRESHOLD || proc->event_count > 10) should_scan = true;
        }
    }
    if (should_scan) {
        refresh_process_rules(ev->pid, pkg, cfg);
        proc->initial_scan_done = true;
        proc->last_scan_time = now;
        proc->event_count = 0;
        proc->consecutive_changes = 0;
    }
    proc->event_count++;
    TidCommEntry *t_entry;
    HASH_FIND_INT(proc->tid_comm_table, &ev->tid, t_entry);
    bool need_apply = false;
    if (!t_entry) {
        need_apply = true;
        t_entry = calloc(1, sizeof(TidCommEntry));
        t_entry->tid = ev->tid;
        strncpy(t_entry->last_comm, t_name, MAX_THREAD_LEN - 1);
        HASH_ADD_INT(proc->tid_comm_table, tid, t_entry);
    } else if (has_thread_rules && strcmp(t_entry->last_comm, t_name) != 0) {
        need_apply = true;
        strncpy(t_entry->last_comm, t_name, MAX_THREAD_LEN - 1);
    }
    if (need_apply) {
        const AffinityRule *rule = find_best_rule(pkg, has_thread_rules ? t_name : "", cfg);
        apply_to_tid(ev->tid, rule, cfg, pkg, t_name);
    }
    if (proc->event_count > MAX_EVENT_COUNT * 2) proc->event_count = MAX_EVENT_COUNT;
    config_release(cfg);
}

static void cleanup_dead_processes(void) {
    ProcessTidCache *proc, *p_tmp;
    HASH_ITER(hh, process_cache, proc, p_tmp) {
        if (kill(proc->pid, 0) != 0) {
            pid_t dead_pid = proc->pid;
            TidCommEntry *t, *ttmp;
            HASH_ITER(hh, proc->tid_comm_table, t, ttmp) {
                HASH_DEL(proc->tid_comm_table, t);
                free(t);
            }
            HASH_DEL(process_cache, proc);
            free(proc);
            PidCacheEntry *pc;
            HASH_FIND_INT(pid_cache, &dead_pid, pc);
            if (pc) {
                HASH_DEL(pid_cache, pc);
                free(pc);
            }
        }
    }
    g_status.last_dead_cleanup_time = time(NULL);
}

static void periodic_full_scan(AppConfig *cfg) {
    if (!cfg) return;
    int fd = open("/proc", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd == -1) return;
    char buf[DENT_BUF_SIZE];
    long nread;
    while ((nread = syscall(SYS_getdents64, fd, buf, DENT_BUF_SIZE)) > 0) {
        for (long bpos = 0; bpos < nread; ) {
            struct linux_dirent64 *d = (struct linux_dirent64 *)(buf + bpos);
            if (isdigit(d->d_name[0])) {
                pid_t pid = (pid_t)atoi(d->d_name);
                char pkg[MAX_PKG_LEN] = {0};
                if (get_process_pkgname(fd, pid, pkg, sizeof(pkg)) && is_interested_package(pkg, cfg)) {
                    update_pid_cache(pid, pkg);
                    ProcessTidCache *pc;
                    HASH_FIND_INT(process_cache, &pid, pc);
                    if (!pc) {
                        pc = calloc(1, sizeof(ProcessTidCache));
                        pc->pid = pid;
                        pc->last_scan_time = time(NULL);
                        HASH_ADD_INT(process_cache, pid, pc);
                    }
                    refresh_process_rules(pid, pkg, cfg);
                    pc->initial_scan_done = true;
                }
            }
            bpos += d->d_reclen;
        }
    }
    close(fd);
    g_status.last_full_scan_time = time(NULL);
}

// 传递给BPF要过滤的EVENT_EXEC的程序
static void setup_filter_map(struct monitor_bpf *skel) {
    int fd = bpf_map__fd(skel->maps.filter_map);
    const char *blacklist[] = {
        /* 常用输出命令 */
        "sh", "ash", "bash", "echo", "printf", "sleep", "usleep",
        /* 安卓高频调用命令 */
        "getprop", "setprop", "dumpsys", "top", "ps", "pgrep", "cmd", "logcat",
        /* 系统或者用户工具 */
        "toybox", "busybox",
        /* 文本(件)处理常用命令 */
        "grep", "sed", "awk", "tr", "cut", "wc", "seq",
        "ls", "cat", "rm", "rmdir", "mkdir", "chmod",
        "basename", "dirname",
        /* affinity模式下过滤掉自身 */
        "AppOpt"
    };

    int val = 1;
    char log_buf[256] = {0};
    size_t offset = 0;
    for (int i = 0; i < sizeof(blacklist) / sizeof(char *); i++) {
        char key[16] = {0};
        strncpy(key, blacklist[i], 15);
        bpf_map_update_elem(fd, key, &val, BPF_ANY);
        int written = snprintf(log_buf + offset, sizeof(log_buf) - offset, "%s%s", (i == 0 ? "" : ", "), blacklist[i]);
        if (written > 0) offset += written;
    }
    LOG_I("内核态过滤启动程序黑名单已配置，过滤以下命令事件，用于降低功耗: [%s]\n", log_buf);
}

static void* config_loader_thread(void* arg) {
    AppConfig *init_cfg = (AppConfig*)arg;
    char config_file[4096], priority_file[4096];
    strncpy(config_file, init_cfg->config_file, 4095);
    strncpy(priority_file, init_cfg->priority_config, 4095);
    pthread_setname_np(pthread_self(), "AppOpt_Loader");

    time_t last_mtime = init_cfg->mtime;
    time_t last_p_mtime = init_cfg->p_mtime;

    while (1) {
        sleep(5);
        struct stat st_a, st_p;
        bool changed = false;
        if (stat(config_file, &st_a) == 0 && st_a.st_mtime > last_mtime) {
            changed = true;
        }
        if (!changed && priority_file[0] != '\0' && stat(priority_file, &st_p) == 0) {
            if (st_p.st_mtime > last_p_mtime) changed = true;
        }
        if (changed) {
            AppConfig *current = get_config();
            if (current) {
                AppConfig *new_cfg = load_config(config_file, priority_file, &current->topo, NULL);
                if (new_cfg) {
                    AppConfig *old = atomic_exchange(&current_config, new_cfg);
                    last_mtime = new_cfg->mtime;
                    last_p_mtime = new_cfg->p_mtime;
                    LOG_I("配置已重载: [Affinity:%zu] [Priority:%zu]\n", new_cfg->num_rules, new_cfg->num_priority_rules);
                    build_thread_cache(new_cfg);
                    periodic_full_scan(new_cfg);
                    config_release(old);
                }
                config_release(current);
            }
        }
    }
    return NULL;
}

static void* cache_cleaner_thread(void* arg) {
    pthread_setname_np(pthread_self(), "AppOpt_Cleaner");
    while (1) {
        sleep(DEAD_CLEANUP_INTERVAL);
        cleanup_dead_processes();
    }
    return NULL;
}

static int libbpf_print_fn(enum libbpf_print_level level, const char *format, va_list args) {
    char buf[1024];
    vsnprintf(buf, sizeof(buf), format, args);
    if (level == LIBBPF_DEBUG || level == LIBBPF_INFO) return 0;
    if (level == LIBBPF_WARN) {
        if (strstr(buf, "rt_fork") || strstr(buf, "tp_fork") || 
            strstr(buf, "rt_sys_enter") || strstr(buf, "sched_setaffinity") || 
            strstr(buf, "sys_setaffinity") || strstr(buf, "rt_exec")) {
            return 0; 
        }
        LOG_W("%s", buf);
    } else LOG_E("%s", buf);
    return 0;
}

static struct monitor_bpf* setup_ebpf(struct perf_buffer **pb, bool *use_raw) {
    struct monitor_bpf *skel = monitor_bpf__open();
    if (!skel) return NULL;
    bool has_rt_fork = true, has_rt_exec = true, has_rt_sys = true;
    if (monitor_bpf__load(skel)) {
        monitor_bpf__destroy(skel);
        skel = monitor_bpf__open();
        bpf_program__set_autoload(skel->progs.rt_fork, false);
        bpf_program__set_autoload(skel->progs.rt_exec, false);
        bpf_program__set_autoload(skel->progs.rt_sys_enter, false);
        if (monitor_bpf__load(skel)) { monitor_bpf__destroy(skel); return NULL; }
        has_rt_fork = has_rt_exec = has_rt_sys = false;
    }
    if (has_rt_fork && !bpf_program__attach(skel->progs.rt_fork)) {
        bpf_program__set_autoload(skel->progs.tp_fork, true);
        bpf_program__attach(skel->progs.tp_fork);
        has_rt_fork = false;
    } else if (!has_rt_fork) {
        bpf_program__attach(skel->progs.tp_fork);
    }
    if (has_rt_exec && !bpf_program__attach(skel->progs.rt_exec)) {
        bpf_program__set_autoload(skel->progs.tp_exec, true);
        bpf_program__attach(skel->progs.tp_exec);
        has_rt_exec = false;
    } else if (!has_rt_exec) {
        bpf_program__attach(skel->progs.tp_exec);
    }
    if (monitor_affinity) {
        bool sys_hooked = false;
        if (has_rt_sys && bpf_program__attach(skel->progs.rt_sys_enter)) {
            sys_hooked = true;
        } else {
            bpf_program__set_autoload(skel->progs.tp_sys_setaffinity, true);
            if (bpf_program__attach(skel->progs.tp_sys_setaffinity)) sys_hooked = true;
        }
        if (!sys_hooked) LOG_I("内核不支持任何形式的 sched_setaffinity 监控，已自动跳过。\n");
        else LOG_I("已经开启监控set_affinity模式\n");
    }
    *use_raw = (has_rt_fork || has_rt_exec );
    *pb = perf_buffer__new(bpf_map__fd(skel->maps.events), 8, handle_ebpf_event, NULL, NULL, NULL);
    return skel;
}

int main(int argc, char **argv) {
    libbpf_set_print(libbpf_print_fn);
    CpuTopology topo = init_cpu_topo();
    char config_file[4096] = "./applist.conf";
    char priority_file[4096] = "\0";
    int opt;
    while ((opt = getopt(argc, argv, "c:p:mvh")) != -1) {
        switch (opt) {
            case 'c': strncpy(config_file, optarg, 4095); break;
            case 'p': strncpy(priority_file, optarg, 4095); break;
            case 'm': monitor_affinity = true; break;
            case 'v': printf("AppOpt %s\n", VERSION); return 0;
            case 'h': printf("用法: %s [-c 亲和性配置] [-p 优先级配置] [-m 监控affinity] [-v]\n", argv[0]); return 0;
        }
    }

    time_t last_mtime = 0;
    AppConfig *cfg = load_config(config_file, priority_file, &topo, &last_mtime);
    if (!cfg) return 1;
    atomic_store(&current_config, cfg);
    periodic_full_scan(cfg);

    pthread_t loader_tid, cleaner_tid;
    pthread_create(&loader_tid, NULL, config_loader_thread, cfg);
    pthread_detach(loader_tid);
    pthread_create(&cleaner_tid, NULL, cache_cleaner_thread, NULL);
    pthread_detach(cleaner_tid);

    struct perf_buffer *pb = NULL;
    bool use_raw;
    struct monitor_bpf *skel = setup_ebpf(&pb, &use_raw);
    if (!skel) return 1;
    const char* prio_status = (cfg->num_priority_rules > 0) ? "ON" : "OFF";
    LOG_I("eBPF Hook方式: %s\n", use_raw ? "raw_tracepoint" : "tracepoint");
    LOG_I("启动AppOpt服务 v%s [eBPF:ON] [Priority:%s] [PID:%d]\n", VERSION, prio_status, getpid());
    setup_filter_map(skel);

    while (1) {
        perf_buffer__poll(pb, 100);
        time_t now = time(NULL);
        if (now - g_status.last_full_scan_time >= PERIODIC_FULL_SCAN_INTERVAL) {
            AppConfig *c = get_config();
            if (c) {
                periodic_full_scan(c);
                config_release(c);
            }
        }
    }

    perf_buffer__free(pb);
    monitor_bpf__destroy(skel);
    AppConfig *final_cfg = atomic_load(&current_config);
    config_release(final_cfg);
    if (topo.base_cpuset_fd != -1) close(topo.base_cpuset_fd);
    return 0;
}
