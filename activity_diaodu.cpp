#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include <dirent.h>
#include <sys/types.h>
#include <regex.h>
#include <ctype.h>
#include <errno.h>
#include <sys/mount.h>
#include "cJSON.h"

#define LOG_FILE "/data/adb/modules/muronggameopt/config/log.txt"
#define MODE_FILE "/data/adb/modules/muronggameopt/config/mode.txt"
#define CPU_CONFIG_DIR "/data/adb/modules/muronggameopt/bin/cpu/"
#define MAX_POLICIES 8
#define MAX_GOV_PARAMS 20
#define MAX_SPECIAL_APPS 32
#define MAX_CLUSTERS 4
#define MAX_GOVERNORS 10
#define MAX_CPU_MODEL_LEN 32
#define MAX_LINE_LEN 512

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

typedef struct {
    char package[128];
    int min_freq[MAX_POLICIES];
    int min_freq_count;

    // 动态参数支持
    int param_count;
    char param_names[MAX_GOV_PARAMS][32];
    char param_values[MAX_GOV_PARAMS][128];
} SpecialAppSetting;

typedef struct {
    int cluster_id;
    int policy_count;
    int policies[MAX_POLICIES];
} CpuCluster;

CpuPolicy cpu_policies[MAX_POLICIES];
CpuCluster cpu_clusters[MAX_CLUSTERS];
CpusetSettings cpuset;
DdrSettings ddr;
SpecialAppSetting special_apps_cache[MAX_SPECIAL_APPS];
int policy_count = 0;
int cluster_count = 0;
int ddr_max = 0;
int ddr_min = 0;
int special_apps_count = 0;
time_t special_apps_json_mtime = 0;
time_t mode_txt_mtime = 0;
char cpu_model[MAX_CPU_MODEL_LEN] = "unknown";

void log_message(const char* message);
char* get_foreground_app();
void apply_settings();
void apply_ddr_settings();
void load_mode_settings_json(const char* mode);
void lock_val(const char* path, const char* value);
void set_special_app_settings(const char* package);
void load_special_apps_cache();
void reload_special_apps_if_needed();
int mode_file_changed();
int detect_cpu_model();
void detect_cpu_clusters();
char* get_cpu_config_path();
char* get_special_apps_config_path();
void set_governor_params(int policy_num, const char* governor);
int get_supported_governors(int policy_num, char governors[][32], int max_count);
int is_governor_supported(int policy_num, const char* governor);
void clear_log_if_needed();
void set_policy_file_permissions();

// 日志记录函数
void log_message(const char* message) {
    clear_log_if_needed();

    FILE* log = fopen(LOG_FILE, "a");
    if (log) {
        time_t now;
        time(&now);
        struct tm* tm_info = localtime(&now);
        char timestamp[32];
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);

        fprintf(log, "%s - %s\n", timestamp, message);
        fclose(log);
    }
}

// 清理过大的日志文件
void clear_log_if_needed() {
    struct stat st;
    if (stat(LOG_FILE, &st) == 0) {
        if (st.st_size > 500 * 1024) { // 500KB
            remove(LOG_FILE);
        }
    }
}

// 重新挂载sysfs为可读写
int remount_sysfs_rw() {
    if (mount("none", "/sys", NULL, MS_REMOUNT | MS_BIND, NULL) == 0) {
        log_message("成功重新挂载/sys为可读写");
        return 1;
    }
    else {
        char log_msg[128];
        snprintf(log_msg, sizeof(log_msg), "重新挂载/sys失败: %s", strerror(errno));
        log_message(log_msg);
        return 0;
    }
}

// 设置策略文件权限
void set_policy_file_permissions() {
    for (int i = 0; i < policy_count; i++) {
        char path[256];
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpufreq/policy%d", cpu_policies[i].policy_num);

        // 修改目录权限
        if (chmod(path, 0777) != 0) {
            char log_msg[256];
            snprintf(log_msg, sizeof(log_msg), "修改目录权限失败: %s (错误: %s)", path, strerror(errno));
            log_message(log_msg);
        }

        // 修改具体文件权限
        const char* files[] = {
            "scaling_governor",
            "scaling_max_freq",
            "scaling_min_freq",
            "scaling_available_governors",
            NULL
        };

        for (int j = 0; files[j]; j++) {
            char file_path[256];
            snprintf(file_path, sizeof(file_path), "%s/%s", path, files[j]);

            if (chmod(file_path, 0666) != 0) {
                char log_msg[256];
                snprintf(log_msg, sizeof(log_msg), "修改文件权限失败: %s (错误: %s)", file_path, strerror(errno));
                log_message(log_msg);
            }
        }
    }
}

// 检测CPU型号
int detect_cpu_model() {
    FILE* cpuinfo = fopen("/proc/cpuinfo", "r");
    if (cpuinfo) {
        char line[256];
        while (fgets(line, sizeof(line), cpuinfo)) {
            if (strstr(line, "Hardware") || strstr(line, "model name")) {
                char* colon = strchr(line, ':');
                if (colon) {
                    colon += 2; // 跳过冒号和空格
                    char* end = strchr(colon, '\n');
                    if (end) *end = '\0';

                    // 检测高通芯片
                    if (strstr(colon, "SM")) {
                        // 提取芯片型号 (如 SM8650, SM8750)
                        char* start = strstr(colon, "SM");
                        if (start) {
                            // 复制直到遇到空格或结束
                            int i = 0;
                            while (*start && !isspace(*start) && i < MAX_CPU_MODEL_LEN - 1) {
                                cpu_model[i++] = *start++;
                            }
                            cpu_model[i] = '\0';
                            fclose(cpuinfo);
                            return 1;
                        }
                    }
                }
            }
        }
        fclose(cpuinfo);
    }

    // 检查构建属性
    const char* build_prop_paths[] = {
        "/odm/build.prop",
        "/system/build.prop",
        "/vendor/build.prop",
        NULL
    };

    for (int i = 0; build_prop_paths[i]; i++) {
        FILE* buildprop = fopen(build_prop_paths[i], "r");
        if (buildprop) {
            char line[256];
            while (fgets(line, sizeof(line), buildprop)) {
                if (strstr(line, "ro.board.platform") ||
                    strstr(line, "ro.chipname") ||
                    strstr(line, "ro.product.oplus.cpuinfo")) {
                    char* equals = strchr(line, '=');
                    if (equals) {
                        equals++;
                        char* end = strchr(equals, '\n');
                        if (end) *end = '\0';

                        if (strstr(equals, "SM8")) {
                            strncpy(cpu_model, equals, MAX_CPU_MODEL_LEN - 1);
                            cpu_model[MAX_CPU_MODEL_LEN - 1] = '\0';
                            fclose(buildprop);
                            return 1;
                        }
                    }
                }
            }
            fclose(buildprop);
        }
    }

    return 0;
}

// 检测CPU簇
void detect_cpu_clusters() {
    int policy_freqs[MAX_POLICIES] = { 0 };
    policy_count = 0;
    cluster_count = 0;

    // 首先收集所有policy的最大频率
    for (int i = 0; i < MAX_POLICIES; i++) {
        char path[128];
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpufreq/policy%d", i);
        if (access(path, F_OK) == 0) {
            char max_freq_path[256];
            snprintf(max_freq_path, sizeof(max_freq_path), "%s/cpuinfo_max_freq", path);
            FILE* fp = fopen(max_freq_path, "r");
            if (fp) {
                if (fscanf(fp, "%d", &policy_freqs[i]) != 1) {
                    policy_freqs[i] = 0;
                }
                fclose(fp);
            }

            // 初始化CPU策略
            cpu_policies[policy_count].policy_num = i;
            cpu_policies[policy_count].cluster_id = -1;
            policy_count++;
        }
    }

    // 根据频率分组簇
    for (int i = 0; i < policy_count; i++) {
        int policy_num = cpu_policies[i].policy_num;
        int freq = policy_freqs[policy_num];

        if (freq == 0) continue;

        int found = 0;
        for (int j = 0; j < cluster_count; j++) {
            // 频率在100MHz内视为同一簇
            if (abs(freq - policy_freqs[cpu_clusters[j].policies[0]]) < 100000) {
                cpu_clusters[j].policies[cpu_clusters[j].policy_count++] = policy_num;
                cpu_policies[i].cluster_id = j;
                found = 1;
                break;
            }
        }

        if (!found && cluster_count < MAX_CLUSTERS) {
            cpu_clusters[cluster_count].cluster_id = cluster_count;
            cpu_clusters[cluster_count].policy_count = 1;
            cpu_clusters[cluster_count].policies[0] = policy_num;
            cpu_policies[i].cluster_id = cluster_count;
            cluster_count++;
        }
    }

    // 记录簇信息
    char log_buf[512] = "CPU集群检测结果:\n";
    for (int i = 0; i < cluster_count; i++) {
        char cluster_info[128];
        snprintf(cluster_info, sizeof(cluster_info), "Cluster %d (策略: ", i);

        for (int j = 0; j < cpu_clusters[i].policy_count; j++) {
            char policy_str[16];
            snprintf(policy_str, sizeof(policy_str), "%d", cpu_clusters[i].policies[j]);
            strcat(cluster_info, policy_str);
            if (j < cpu_clusters[i].policy_count - 1) {
                strcat(cluster_info, ",");
            }
        }
        strcat(cluster_info, ")\n");

        strcat(log_buf, cluster_info);
    }

    log_message(log_buf);
}

// 获取支持的调速器
int get_supported_governors(int policy_num, char governors[][32], int max_count) {
    char path[128];
    snprintf(path, sizeof(path),
        "/sys/devices/system/cpu/cpufreq/policy%d/scaling_available_governors",
        policy_num);

    FILE* fp = fopen(path, "r");
    if (!fp) {
        char log_msg[128];
        snprintf(log_msg, sizeof(log_msg), "无法读取调速器列表: %s", path);
        log_message(log_msg);
        return 0;
    }

    char line[256];
    if (!fgets(line, sizeof(line), fp)) {
        fclose(fp);
        return 0;
    }

    fclose(fp);

    int count = 0;
    char* token = strtok(line, " ");
    while (token && count < max_count) {
        // 移除换行符和空格
        size_t len = strlen(token);
        while (len > 0 && (token[len - 1] == '\n' || token[len - 1] == '\r' || token[len - 1] == ' ')) {
            token[--len] = '\0';
        }

        if (len > 0) {
            strncpy(governors[count], token, 32);
            governors[count][31] = '\0';
            count++;
        }
        token = strtok(NULL, " ");
    }

    return count;
}

// 检查调速器是否支持
int is_governor_supported(int policy_num, const char* governor) {
    char supported_govs[MAX_GOVERNORS][32];
    int count = get_supported_governors(policy_num, supported_govs, MAX_GOVERNORS);

    for (int i = 0; i < count; i++) {
        if (strcmp(supported_govs[i], governor) == 0) {
            return 1;
        }
    }

    return 0;
}

// 设置调速器参数
void set_governor_params(int policy_num, const char* governor) {
    // 查找当前policy
    CpuPolicy* policy_ptr = NULL;
    for (int i = 0; i < policy_count; i++) {
        if (cpu_policies[i].policy_num == policy_num) {
            policy_ptr = &cpu_policies[i];
            break;
        }
    }

    if (!policy_ptr || policy_ptr->param_count == 0) return;

    // 检查调速器特定目录是否存在
    char gov_path[256];
    snprintf(gov_path, sizeof(gov_path),
        "/sys/devices/system/cpu/cpufreq/policy%d/%s",
        policy_num, governor);

    int has_gov_dir = (access(gov_path, F_OK) == 0);

    // 应用所有参数
    for (int i = 0; i < policy_ptr->param_count; i++) {
        char param_path[256];

        // 尝试调速器特定目录
        if (has_gov_dir) {
            snprintf(param_path, sizeof(param_path),
                "%s/%s", gov_path, policy_ptr->param_names[i]);
        }
        // 尝试主目录
        else {
            snprintf(param_path, sizeof(param_path),
                "/sys/devices/system/cpu/cpufreq/policy%d/%s",
                policy_num, policy_ptr->param_names[i]);
        }

        // 检查文件是否存在
        if (access(param_path, F_OK) == 0) {
            lock_val(param_path, policy_ptr->param_values[i]);
        }
        else {
            char log_msg[256];
            snprintf(log_msg, sizeof(log_msg),
                "参数文件不存在: %s", param_path);
            log_message(log_msg);
        }
    }
}

// 获取CPU配置文件路径
char* get_cpu_config_path() {
    static char path[256];
    snprintf(path, sizeof(path), "%s%s.json", CPU_CONFIG_DIR, cpu_model);
    return path;
}

// 获取特殊应用配置文件路径
char* get_special_apps_config_path() {
    static char path[256];
    snprintf(path, sizeof(path), "%s%s_apps.json", CPU_CONFIG_DIR, cpu_model);
    return path;
}

// 写入值到文件（增强权限处理）
void lock_val(const char* path, const char* value) {
    // 第一次尝试写入
    FILE* fp = fopen(path, "w");
    if (fp) {
        if (fprintf(fp, "%s", value) < 0) {
            char log_msg[256];
            snprintf(log_msg, sizeof(log_msg), "写入失败: %s -> %s (错误: %s)",
                path, value, strerror(errno));
            log_message(log_msg);
        }
        fclose(fp);
        return;
    }

    // 如果第一次失败，尝试修改权限
    if (chmod(path, 0777) != 0) {
        char log_msg[256];
        snprintf(log_msg, sizeof(log_msg), "修改权限失败: %s (错误: %s)",
            path, strerror(errno));
        log_message(log_msg);
    }
    else {
        // 第二次尝试写入
        fp = fopen(path, "w");
        if (fp) {
            if (fprintf(fp, "%s", value) < 0) {
                char log_msg[256];
                snprintf(log_msg, sizeof(log_msg), "再次写入失败: %s -> %s (错误: %s)",
                    path, value, strerror(errno));
                log_message(log_msg);
            }
            fclose(fp);
        }
        else {
            char log_msg[256];
            snprintf(log_msg, sizeof(log_msg), "再次打开失败: %s (错误: %s)",
                path, strerror(errno));
            log_message(log_msg);
        }
    }
}

// 获取前台应用
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char* get_foreground_app() {
    FILE* fp = popen("dumpsys window | grep mCurrentFocus", "r");
    if (!fp) return strdup("unknown");

    char line[512];
    char* last_valid = NULL;

    while (fgets(line, sizeof(line), fp)) {
        char* start = strchr(line, '{');
        char* end = strrchr(line, '}');  // 使用最后一个 } 作为结束点

        if (start && end && end > start) {
            // 提取 {} 之间的内容
            size_t len = end - start - 1;
            char inner[256];
            if (len > 0 && len < sizeof(inner)) {
                strncpy(inner, start + 1, len);
                inner[len] = '\0';

                // 提取最后一个空格后的内容
                char* last_space = strrchr(inner, ' ');
                char* candidate = last_space ? last_space + 1 : inner;

                // 处理 PopupWindow: 前缀
                char* popup_prefix = strstr(candidate, "PopupWindow:");
                if (popup_prefix) {
                    candidate = popup_prefix + 12;  // 跳过 "PopupWindow:"
                }

                // 处理斜杠后的 activity 名
                char* slash = strchr(candidate, '/');
                if (slash) *slash = '\0';

                // 保存最后一个有效包名
                if (last_valid) free(last_valid);
                last_valid = strdup(candidate);
            }
        }
    }

    pclose(fp);

    // 返回最后一个有效包名或 unknown
    if (last_valid) {
        return last_valid;
    }
    else {
        return strdup("unknown");
    }
}

// 应用设置
void apply_settings() {
    // 应用cpuset设置
    lock_val("/dev/cpuset/background/cpus", cpuset.background);
    lock_val("/dev/cpuset/system-background/cpus", cpuset.systembackground);
    lock_val("/dev/cpuset/foreground/cpus", cpuset.foreground);
    lock_val("/dev/cpuset/top-app/cpus", cpuset.topapp);

    // 应用CPU策略
    for (int i = 0; i < policy_count; i++) {
        CpuPolicy* policy = &cpu_policies[i];
        char path[256];

        // 设置最大/最小频率
        snprintf(path, sizeof(path),
            "/sys/devices/system/cpu/cpufreq/policy%d/scaling_max_freq",
            policy->policy_num);
        char max_freq_str[32];
        snprintf(max_freq_str, sizeof(max_freq_str), "%d", policy->max_freq);
        lock_val(path, max_freq_str);

        snprintf(path, sizeof(path),
            "/sys/devices/system/cpu/cpufreq/policy%d/scaling_min_freq",
            policy->policy_num);
        char min_freq_str[32];
        snprintf(min_freq_str, sizeof(min_freq_str), "%d", policy->min_freq);
        lock_val(path, min_freq_str);

        // 检查调速器是否支持
        if (!is_governor_supported(policy->policy_num, policy->governor)) {
            char log_msg[128];
            snprintf(log_msg, sizeof(log_msg),
                "调速器 %s 在policy%d上不支持，跳过设置",
                policy->governor, policy->policy_num);
            log_message(log_msg);
            continue;
        }

        // 设置调速器
        snprintf(path, sizeof(path),
            "/sys/devices/system/cpu/cpufreq/policy%d/scaling_governor",
            policy->policy_num);
        lock_val(path, policy->governor);

        // 设置调速器参数
        set_governor_params(policy->policy_num, policy->governor);
    }
}

// 应用DDR设置
void apply_ddr_settings() {
    DIR* dir = opendir("/sys/devices/system/cpu/bus_dcvs/DDR");
    if (!dir) {
        log_message("无法打开DDR目录");
        return;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_DIR && strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
            char path[256];

            // 设置最小频率
            snprintf(path, sizeof(path),
                "/sys/devices/system/cpu/bus_dcvs/DDR/%s/min_freq",
                entry->d_name);
            if (access(path, F_OK) == 0) {
                FILE* fp = fopen(path, "w");
                if (fp) {
                    fprintf(fp, "%d", ddr.min_freq);
                    fclose(fp);
                }
            }

            // 设置最大频率
            snprintf(path, sizeof(path),
                "/sys/devices/system/cpu/bus_dcvs/DDR/%s/max_freq",
                entry->d_name);
            if (access(path, F_OK) == 0) {
                FILE* fp = fopen(path, "w");
                if (fp) {
                    fprintf(fp, "%d", ddr.max_freq);
                    fclose(fp);
                }
            }
        }
    }
    closedir(dir);
}

// 加载模式设置JSON
void load_mode_settings_json(const char* mode) {
    char* config_path = get_cpu_config_path();
    FILE* fp = fopen(config_path, "rb");
    if (!fp) {
        char error_msg[256];
        snprintf(error_msg, sizeof(error_msg), "未找到CPU配置文件: %s (错误: %s)",
            config_path, strerror(errno));
        log_message(error_msg);
        return;
    }

    // 检查文件修改时间
    struct stat st;
    if (fstat(fileno(fp), &st) == 0) {
        static time_t last_mtime = 0;
        if (last_mtime != st.st_mtime) {
            last_mtime = st.st_mtime;
            special_apps_json_mtime = st.st_mtime; // 更新特殊应用配置时间戳
        }
    }

    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char* data = (char*)malloc(len + 1);
    if (!data) {
        log_message("内存分配失败");
        fclose(fp);
        return;
    }
    fread(data, 1, len, fp);
    data[len] = 0;
    fclose(fp);

    cJSON* root = cJSON_Parse(data);
    if (!root) {
        const char* error_ptr = cJSON_GetErrorPtr();
        if (error_ptr) {
            char error_msg[256];
            snprintf(error_msg, sizeof(error_msg), "JSON解析失败: %s", error_ptr);
            log_message(error_msg);
        }
        free(data);
        return;
    }

    // 1. 解析模式配置
    cJSON* mode_obj = cJSON_GetObjectItem(root, mode);
    if (!mode_obj) {
        char error_msg[256];
        snprintf(error_msg, sizeof(error_msg), "在JSON中未找到模式: %s", mode);
        log_message(error_msg);
        log_message("回退到均衡模式");
        mode_obj = cJSON_GetObjectItem(root, "balance");
        if (!mode_obj) {
            cJSON_Delete(root);
            free(data);
            log_message("均衡模式也不存在");
            return;
        }
    }

    // 解析CPU策略
    cJSON* policies = cJSON_GetObjectItem(mode_obj, "cpu_policies");
    if (policies && cJSON_IsArray(policies)) {
        int policy_index = 0;
        cJSON* policy;
        cJSON_ArrayForEach(policy, policies) {
            if (policy_index >= policy_count) break;

            // 获取集群ID
            cJSON* cluster_id = cJSON_GetObjectItem(policy, "cluster");
            if (cluster_id && cJSON_IsNumber(cluster_id)) {
                int cluster = cluster_id->valueint;
                if (cluster >= 0 && cluster < cluster_count) {
                    // 应用集群设置到所有策略
                    for (int j = 0; j < cpu_clusters[cluster].policy_count; j++) {
                        int policy_num = cpu_clusters[cluster].policies[j];

                        // 找到对应的策略结构
                        for (int k = 0; k < policy_count; k++) {
                            if (cpu_policies[k].policy_num == policy_num) {
                                CpuPolicy* p = &cpu_policies[k];

                                // 设置调速器
                                cJSON* governor = cJSON_GetObjectItem(policy, "governor");
                                if (governor && cJSON_IsString(governor)) {
                                    strncpy(p->governor, governor->valuestring,
                                        sizeof(p->governor) - 1);
                                }

                                // 设置频率
                                cJSON* max_freq = cJSON_GetObjectItem(policy, "max_freq");
                                if (max_freq && cJSON_IsNumber(max_freq)) {
                                    p->max_freq = max_freq->valueint;
                                }

                                cJSON* min_freq = cJSON_GetObjectItem(policy, "min_freq");
                                if (min_freq && cJSON_IsNumber(min_freq)) {
                                    p->min_freq = min_freq->valueint;
                                }

                                // 设置调速器参数
                                cJSON* params = cJSON_GetObjectItem(policy, "params");
                                if (params && cJSON_IsObject(params)) {
                                    p->param_count = 0;
                                    cJSON* param = NULL;
                                    cJSON_ArrayForEach(param, params) {
                                        if (p->param_count >= MAX_GOV_PARAMS) break;

                                        if (cJSON_IsString(param)) {
                                            strncpy(p->param_names[p->param_count], param->string, 32);
                                            strncpy(p->param_values[p->param_count], param->valuestring, 128);
                                            p->param_count++;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            policy_index++;
        }
    }

    // 解析cpuset设置
    cJSON* cpuset_obj = cJSON_GetObjectItem(mode_obj, "cpuset");
    if (cpuset_obj) {
        cJSON* background = cJSON_GetObjectItem(cpuset_obj, "background");
        if (background && cJSON_IsString(background)) {
            strncpy(cpuset.background, background->valuestring, sizeof(cpuset.background) - 1);
        }

        cJSON* systembackground = cJSON_GetObjectItem(cpuset_obj, "systembackground");
        if (systembackground && cJSON_IsString(systembackground)) {
            strncpy(cpuset.systembackground, systembackground->valuestring,
                sizeof(cpuset.systembackground) - 1);
        }

        cJSON* foreground = cJSON_GetObjectItem(cpuset_obj, "foreground");
        if (foreground && cJSON_IsString(foreground)) {
            strncpy(cpuset.foreground, foreground->valuestring, sizeof(cpuset.foreground) - 1);
        }

        cJSON* topapp = cJSON_GetObjectItem(cpuset_obj, "topapp");
        if (topapp && cJSON_IsString(topapp)) {
            strncpy(cpuset.topapp, topapp->valuestring, sizeof(cpuset.topapp) - 1);
        }
    }

    // 解析DDR设置
    cJSON* ddr_obj = cJSON_GetObjectItem(mode_obj, "ddr");
    if (ddr_obj) {
        cJSON* min_freq = cJSON_GetObjectItem(ddr_obj, "min_freq");
        if (min_freq) {
            if (cJSON_IsString(min_freq)) {
                const char* min_freq_str = min_freq->valuestring;
                ddr.min_freq = (strcmp(min_freq_str, "ddr_min") == 0) ? ddr_min : atoi(min_freq_str);
            }
            else if (cJSON_IsNumber(min_freq)) {
                ddr.min_freq = min_freq->valueint;
            }
        }

        cJSON* max_freq = cJSON_GetObjectItem(ddr_obj, "max_freq");
        if (max_freq) {
            if (cJSON_IsString(max_freq)) {
                const char* max_freq_str = max_freq->valuestring;
                if (strstr(max_freq_str, "ddr_min*")) {
                    double factor = atof(max_freq_str + strlen("ddr_min*"));
                    ddr.max_freq = (int)(ddr_min * factor);
                }
                else if (strcmp(max_freq_str, "ddr_max") == 0) {
                    ddr.max_freq = ddr_max;
                }
                else {
                    ddr.max_freq = atoi(max_freq_str);
                }
            }
            else if (cJSON_IsNumber(max_freq)) {
                ddr.max_freq = max_freq->valueint;
            }
        }
    }

    cJSON_Delete(root);
}

// 设置特殊应用设置
void set_special_app_settings(const char* package) {
    for (int i = 0; i < special_apps_count; i++) {
        if (strcmp(special_apps_cache[i].package, package) == 0) {
            char log_header[512];
            snprintf(log_header, sizeof(log_header),
                "===== 应用特殊设置: %s =====", package);
            log_message(log_header);

            // 记录特殊应用设置摘要
            char summary[1024] = "设置内容: ";
            if (special_apps_cache[i].min_freq_count > 0) {
                strcat(summary, "min_freq[");
                for (int j = 0; j < special_apps_cache[i].min_freq_count; j++) {
                    char freq_str[32];
                    snprintf(freq_str, sizeof(freq_str), "%d", special_apps_cache[i].min_freq[j]);
                    strcat(summary, freq_str);
                    if (j < special_apps_cache[i].min_freq_count - 1) strcat(summary, ",");
                }
                strcat(summary, "] ");
            }

            if (special_apps_cache[i].param_count > 0) {
                strcat(summary, "params[");
                for (int j = 0; j < special_apps_cache[i].param_count; j++) {
                    strcat(summary, special_apps_cache[i].param_names[j]);
                    strcat(summary, "=");
                    strcat(summary, special_apps_cache[i].param_values[j]);
                    if (j < special_apps_cache[i].param_count - 1) strcat(summary, ",");
                }
                strcat(summary, "]");
            }
            log_message(summary);

            // 应用设置并记录详细日志
            char detailed_log[4096] = "详细设置:\n";
            for (int j = 0; j < policy_count; j++) {
                int policy_num = cpu_policies[j].policy_num;
                char policy_log[512] = { 0 };

                // 应用最小频率设置
                if (j < special_apps_cache[i].min_freq_count &&
                    special_apps_cache[i].min_freq[j] > 0) {

                    char path[256];
                    snprintf(path, sizeof(path),
                        "/sys/devices/system/cpu/cpufreq/policy%d/scaling_min_freq",
                        policy_num);

                    char val_str[32];
                    snprintf(val_str, sizeof(val_str), "%d", special_apps_cache[i].min_freq[j]);
                    lock_val(path, val_str);

                    snprintf(policy_log + strlen(policy_log), sizeof(policy_log) - strlen(policy_log),
                        "策略 %d: min_freq=%d kHz", policy_num, special_apps_cache[i].min_freq[j]);
                }

                // 应用调速器参数
                char gov_path[256];
                snprintf(gov_path, sizeof(gov_path),
                    "/sys/devices/system/cpu/cpufreq/policy%d/%s",
                    policy_num, cpu_policies[j].governor);

                if (access(gov_path, F_OK) == 0) {
                    // 应用所有参数
                    for (int k = 0; k < special_apps_cache[i].param_count; k++) {
                        char param_path[256];
                        snprintf(param_path, sizeof(param_path),
                            "%s/%s", gov_path, special_apps_cache[i].param_names[k]);

                        if (access(param_path, F_OK) == 0) {
                            lock_val(param_path, special_apps_cache[i].param_values[k]);

                            // 添加到策略日志
                            if (strlen(policy_log) > 0) strcat(policy_log, ", ");
                            snprintf(policy_log + strlen(policy_log), sizeof(policy_log) - strlen(policy_log),
                                "%s=%s", special_apps_cache[i].param_names[k],
                                special_apps_cache[i].param_values[k]);
                        }
                    }
                }

                // 如果该策略有设置，添加到详细日志
                if (strlen(policy_log) > 0) {
                    strcat(policy_log, "\n");
                    strcat(detailed_log, policy_log);
                }
            }

            // 记录详细日志
            log_message(detailed_log);

            // 结束标记
            log_message("===== 特殊设置应用完成 =====");
            break;
        }
    }
}

// 加载特殊应用缓存
// 加载特殊应用缓存
void load_special_apps_cache() {
    special_apps_count = 0;
    char* config_path = get_cpu_config_path(); // 使用相同的配置文件

    // 获取文件修改时间
    struct stat st;
    if (stat(config_path, &st) != 0) {
        char log_msg[256];
        snprintf(log_msg, sizeof(log_msg), "无法访问配置文件: %s (错误: %s)",
            config_path, strerror(errno));
        log_message(log_msg);
        return;
    }
    special_apps_json_mtime = st.st_mtime;

    FILE* fp = fopen(config_path, "rb");
    if (!fp) {
        char log_msg[256];
        snprintf(log_msg, sizeof(log_msg), "未找到特殊应用配置文件: %s (错误: %s)",
            config_path, strerror(errno));
        log_message(log_msg);
        return;
    }

    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char* data = (char*)malloc(len + 1);
    if (!data) {
        log_message("内存分配失败");
        fclose(fp);
        return;
    }
    fread(data, 1, len, fp);
    data[len] = 0;
    fclose(fp);

    cJSON* root = cJSON_Parse(data);
    if (!root) {
        const char* error_ptr = cJSON_GetErrorPtr();
        if (error_ptr) {
            char error_msg[256];
            snprintf(error_msg, sizeof(error_msg), "解析特殊应用JSON失败: %s", error_ptr);
            log_message(error_msg);
        }
        else {
            log_message("解析特殊应用JSON失败");
        }
        free(data);
        return;
    }

    cJSON* special_apps = cJSON_GetObjectItem(root, "special_apps");
    if (!special_apps || !cJSON_IsArray(special_apps)) {
        log_message("特殊应用JSON格式无效或不存在");
        cJSON_Delete(root);
        free(data);
        return;
    }

    cJSON* app;
    int i = 0;
    cJSON_ArrayForEach(app, special_apps) {
        if (i >= MAX_SPECIAL_APPS) break;

        // 解析包名列表 (逗号分隔)
        cJSON* package_item = cJSON_GetObjectItem(app, "package");
        if (package_item && cJSON_IsString(package_item)) {
            char* packages = strdup(package_item->valuestring);
            char* package = strtok(packages, ",");

            while (package && i < MAX_SPECIAL_APPS) {
                strncpy(special_apps_cache[i].package, package, sizeof(special_apps_cache[i].package) - 1);
                special_apps_cache[i].package[sizeof(special_apps_cache[i].package) - 1] = '\0';

                // 解析min_freq数组
                cJSON* min_freq = cJSON_GetObjectItem(app, "min_freq");
                if (min_freq && cJSON_IsArray(min_freq)) {
                    int j = 0;
                    cJSON* freq;
                    cJSON_ArrayForEach(freq, min_freq) {
                        if (j >= MAX_POLICIES) break;
                        if (cJSON_IsNumber(freq)) {
                            special_apps_cache[i].min_freq[j] = freq->valueint;
                        }
                        j++;
                    }
                    special_apps_cache[i].min_freq_count = j;
                }
                else {
                    special_apps_cache[i].min_freq_count = 0;
                }

                // 解析params
                cJSON* params = cJSON_GetObjectItem(app, "params");
                if (params && cJSON_IsObject(params)) {
                    special_apps_cache[i].param_count = 0;
                    cJSON* param = NULL;
                    cJSON_ArrayForEach(param, params) {
                        if (special_apps_cache[i].param_count >= MAX_GOV_PARAMS) break;

                        if (cJSON_IsString(param)) {
                            strncpy(special_apps_cache[i].param_names[special_apps_cache[i].param_count],
                                param->string, 32);

                            strncpy(special_apps_cache[i].param_values[special_apps_cache[i].param_count],
                                param->valuestring, 128);

                            special_apps_cache[i].param_count++;
                        }
                    }
                }

                i++;
                special_apps_count++;
                package = strtok(NULL, ",");
            }
            free(packages); // 修正变量名拼写错误
        }
    }

    cJSON_Delete(root);
    free(data);

    char log_msg[128];
    snprintf(log_msg, sizeof(log_msg), "加载了 %d 个特殊应用设置", special_apps_count);
    log_message(log_msg);
}

// 如果需要重新加载特殊应用
void reload_special_apps_if_needed() {
    char* config_path = get_cpu_config_path(); // 使用相同的配置文件
    struct stat st;
    if (stat(config_path, &st) != 0) {
        return;
    }
    if (special_apps_json_mtime == st.st_mtime) {
        return;
    }
    special_apps_json_mtime = st.st_mtime;
    load_special_apps_cache();
}

// 检查模式文件是否改变
int mode_file_changed() {
    struct stat st;
    if (stat(MODE_FILE, &st) != 0) {
        return 0;
    }
    if (mode_txt_mtime != st.st_mtime) {
        mode_txt_mtime = st.st_mtime;
        return 1;
    }
    return 0;
}

int main() {
    log_message("启动调度服务");

    // 确保以root权限运行
    if (getuid() != 0) {
        log_message("错误：必须以root权限运行");
        return 1;
    }

    if (!detect_cpu_model()) {
        log_message("无法检测CPU型号，使用默认配置");
        strcpy(cpu_model, "default");
    }

    char log_msg[128];
    snprintf(log_msg, sizeof(log_msg), "检测到CPU型号: %s", cpu_model);
    log_message(log_msg);

    // 重新挂载/sys为可读写
    if (!remount_sysfs_rw()) {
        log_message("警告：无法重新挂载/sys，部分功能可能受限");
    }

    // 读取DDR频率范围
    FILE* fp = fopen("/sys/devices/system/cpu/bus_dcvs/DDR/hw_max_freq", "r");
    if (fp) {
        if (fscanf(fp, "%d", &ddr_max) != 1) {
            ddr_max = 0;
        }
        fclose(fp);
    }
    fp = fopen("/sys/devices/system/cpu/bus_dcvs/DDR/hw_min_freq", "r");
    if (fp) {
        if (fscanf(fp, "%d", &ddr_min) != 1) {
            ddr_min = 0;
        }
        fclose(fp);
    }

    // 检测CPU簇
    detect_cpu_clusters();

    // 设置策略文件权限
    set_policy_file_permissions();

    // 加载特殊应用设置
    load_special_apps_cache();

    char last_app[256] = "";

    while (1) {
        char* current_app = get_foreground_app();

        if (strcmp(current_app, "unknown") == 0) {
            free(current_app);
            sleep(1);
            continue;
        }

        if (strcmp(current_app, last_app) != 0 || mode_file_changed()) {
            strncpy(last_app, current_app, sizeof(last_app) - 1);
            last_app[sizeof(last_app) - 1] = '\0';

            // 忽略某些应用
            if (strcmp(current_app, "com.omarea.vtools") == 0 ||
                strcmp(current_app, "NotificationShade") == 0) {
                free(current_app);
                sleep(1);
                continue;
            }

            // 读取模式文件
            FILE* mode_file = fopen(MODE_FILE, "r");
            if (!mode_file) {
                log_message("未找到模式文件");
                free(current_app);
                sleep(5);
                continue;
            }

            char mode[64] = "balance";
            char line[256];

            if (fgets(line, sizeof(line), mode_file)) {
                line[strcspn(line, "\n")] = 0;
                strncpy(mode, line, sizeof(mode) - 1);
            }

            // 检查是否有应用特定的模式
            while (fgets(line, sizeof(line), mode_file)) {
                line[strcspn(line, "\n")] = 0;
                char* package = strtok(line, " ");
                char* custom_mode = strtok(NULL, " ");

                if (package && custom_mode && strcmp(package, current_app) == 0) {
                    strncpy(mode, custom_mode, sizeof(mode) - 1);
                    mode[sizeof(mode) - 1] = '\0';
                    break;
                }
            }
            fclose(mode_file);

            // 加载和应用设置
            load_mode_settings_json(mode);

            // 2. 重新加载特殊应用配置
            reload_special_apps_if_needed();

            // 3. 应用模式设置（基础配置）
            apply_settings();
            apply_ddr_settings();

            // 4. 应用特殊应用设置（覆盖部分参数）
            set_special_app_settings(current_app);

            char log_msg[512];
            snprintf(log_msg, sizeof(log_msg),
                "应用: %s | 模式: %s | 设置已应用",
                current_app, mode);
            log_message(log_msg);

            free(current_app);
        }
        else {
            free(current_app);
        }

        sleep(1);
    }

    return 0;
}