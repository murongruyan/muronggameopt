/*
 * 慕容调度模块 (Murong Scheduling Module) v4.9
 * 事件驱动架构与性能优化版本
 * 
 * Copyright (c) 2025 慕容茹艳 (murongruyan)
 * GitHub: https://github.com/murongruyan/muronggameopt
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include <dirent.h>
#include <sys/types.h>
#include <ctype.h>
#include <errno.h>
#include <sys/mount.h>
#include <sys/inotify.h>
#include <sys/select.h>
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
#define MAX_PACKAGE_LEN 128
#define MAX_APP_NAME_LEN 128
#define MAX_VERSION_LEN 32

#define INOTIFY_EVENT_SIZE  (sizeof(struct inotify_event))
#define INOTIFY_BUF_LEN     (1024 * (INOTIFY_EVENT_SIZE + 16))

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

// 增强的特殊应用配置结构
typedef struct {
    char package[MAX_PACKAGE_LEN];

    // 完整的CPU策略设置
    int policy_count;
    struct {
        int min_freq;
        int max_freq;
        char governor[32];

        // 调速器参数
        int param_count;
        char param_names[MAX_GOV_PARAMS][32];
        char param_values[MAX_GOV_PARAMS][128];
    } policies[MAX_POLICIES];

    // DDR设置
    int ddr_min_freq;
    int ddr_max_freq;

    // Cpuset设置
    char cpuset_background[16];
    char cpuset_systembackground[16];
    char cpuset_foreground[16];
    char cpuset_topapp[16];
} SpecialAppSetting;

typedef struct {
    int cluster_id;
    int policy_count;
    int policies[MAX_POLICIES];
    char related_cpus[64];
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
char cpu_model[MAX_CPU_MODEL_LEN] = "unknown";
char config_version[MAX_VERSION_LEN] = "1.0"; // 默认版本号

// 函数声明
void log_message(const char* message);
char* get_foreground_app();
void apply_settings();
void apply_ddr_settings();
void load_mode_settings_json(const char* mode);
int lock_val(const char* path, const char* value);
void set_special_app_settings(const char* package);
void load_special_apps_cache();
int detect_cpu_model();
void detect_cpu_clusters();
const char* get_cpu_config_path();
const char* get_special_apps_config_path();
void set_governor_params(int policy_num, const char* governor);
int get_supported_governors(int policy_num, char governors[][32], int max_count);
int is_governor_supported(int policy_num, const char* governor);
void clear_log_if_needed();
void set_policy_file_permissions();
int remount_sysfs_rw();
void safe_strncpy(char* dest, const char* src, size_t dest_size);
int file_exists(const char* path);
void apply_special_app_cpu_settings(const SpecialAppSetting* app_setting);
void apply_special_app_ddr_settings(const SpecialAppSetting* app_setting);
void apply_special_app_cpuset_settings(const SpecialAppSetting* app_setting);
int extract_chip_model(const char* input);
int is_numeric(const char* str);
void handle_inotify_events(int inotify_fd, int wd_mode_file, int wd_config_file, int wd_config_dir);
void load_config_version(); // 新增加载配置版本函数

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

// 检查文件是否存在
int file_exists(const char* path) {
    return access(path, F_OK) == 0;
}

// 清理过大的日志文件
void clear_log_if_needed() {
    struct stat st;
    if (stat(LOG_FILE, &st) == 0 && st.st_size > 500 * 1024) {
        truncate(LOG_FILE, 0); // 清空文件而不是删除
        log_message("日志文件已清空");
    }
}

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
    // 0. 首先尝试直接执行getprop命令
    FILE* getprop = popen("getprop ro.soc.model", "r");
    if (getprop) {
        char line[64];
        if (fgets(line, sizeof(line), getprop)) {
            // 移除换行符
            line[strcspn(line, "\n")] = '\0';

            // 尝试提取芯片型号
            if (extract_chip_model(line)) {
                pclose(getprop);
                return 1;
            }
        }
        pclose(getprop);
    }

    // 检查构建属性 - 优先检测
    const char* build_prop_paths[] = {
        "/odm/build.prop",               // 一加等设备
        "/vendor/odm/etc/build.prop",    // 某些设备的ODM路径
        "/system/vendor/build.prop",     // 小米、ColorOS设备
        "/vendor/build.prop",            // 通用路径
        "/system/build.prop",            // 通用路径
        NULL
    };

    // 扩展的属性键列表
    const char* prop_keys[] = {
        "ro.soc.model",                  // 小米特有
        "ro.product.oplus.cpuinfo",      // 一加特有
        "ro.build.device_family",        // 一加特有
        "ro.board.platform",             // 通用平台标识（ColorOS天玑8100使用）
        "ro.chipname",
        "ro.vendor.product.cpuinfo",
        "ro.hardware.chipname",
        "ro.mediatek.platform",          // 天玑通用
        "ro.vendor.mediatek.platform",   // 天玑9000特有
        "ro.boot.platform",
        "ro.boot.chipname",
        "ro.boot.hardware.sku",
        "ro.hardware",                   // 通用硬件标识
        "ro.product.board",              // 主板标识
        "ro.mediatek.hardware",          // 天玑硬件标识
        NULL
    };

    // 1. 检查所有可能的build.prop文件
    for (int i = 0; build_prop_paths[i]; i++) {
        if (!file_exists(build_prop_paths[i])) continue;

        FILE* buildprop = fopen(build_prop_paths[i], "r");
        if (!buildprop) continue;

        char line[256];
        while (fgets(line, sizeof(line), buildprop)) {
            // 检查所有可能的属性键
            for (int k = 0; prop_keys[k]; k++) {
                // 精确匹配键名（包含等号）
                char key_pattern[64];
                snprintf(key_pattern, sizeof(key_pattern), "%s=", prop_keys[k]);

                if (strstr(line, key_pattern)) {
                    char* value = strchr(line, '=') + 1;

                    // 清理值
                    char* end = strchr(value, '\n');
                    if (end) *end = '\0';

                    // 移除引号
                    if (value[0] == '"' || value[0] == '\'') {
                        value++;
                        size_t len = strlen(value);
                        if (len > 0 && (value[len - 1] == '"' || value[len - 1] == '\'')) {
                            value[len - 1] = '\0';
                        }
                    }

                    // 特殊处理一加的device_family格式 (OPSM8650)
                    if (strcmp(prop_keys[k], "ro.build.device_family") == 0) {
                        if (strlen(value) > 2 &&
                            (strncmp(value, "OP", 2) == 0 ||
                                strncmp(value, "CPH", 3) == 0)) {
                            // 提取型号部分 (例如 OPSM8650 -> SM8650)
                            char* model_ptr = strstr(value, "SM");
                            if (model_ptr) {
                                safe_strncpy(cpu_model, model_ptr, MAX_CPU_MODEL_LEN);
                                fclose(buildprop);
                                return 1;
                            }
                        }
                    }

                    // 特殊处理天玑平台属性
                    if (strstr(prop_keys[k], "mediatek") ||
                        strstr(prop_keys[k], "mtk") ||
                        strstr(prop_keys[k], "board.platform")) {
                        // 直接尝试提取芯片型号（不区分大小写）
                        if (extract_chip_model(value)) {
                            fclose(buildprop);
                            return 1;
                        }

                        // 如果是纯数字，添加MT前缀
                        if (is_numeric(value) && strlen(value) == 4) {
                            snprintf(cpu_model, MAX_CPU_MODEL_LEN, "MT%s", value);
                            fclose(buildprop);
                            return 1;
                        }
                    }

                    // 尝试提取芯片型号
                    if (extract_chip_model(value)) {
                        fclose(buildprop);
                        return 1;
                    }
                }
            }
        }
        fclose(buildprop);
    }

    // 2. 检查/sys/devices/soc0 - 最可靠的高通检测方法
    const char* soc_files[] = {
        "/sys/devices/soc0/soc_id",      // 直接包含SMxxxx的数字部分
        "/sys/devices/soc0/machine",     // 可能包含完整型号
        "/sys/devices/soc0/family",      // 可能包含"Snapdragon"
        "/sys/class/soc/soc0/soc_id",    // 旧版路径
        "/sys/devices/soc0/soc_name",    // 天玑可能使用
        "/sys/devices/soc0/chip_name",   // 备用路径
        NULL
    };

    for (int i = 0; soc_files[i]; i++) {
        if (!file_exists(soc_files[i])) continue;

        FILE* fp = fopen(soc_files[i], "r");
        if (!fp) continue;

        char content[32] = { 0 };
        if (fgets(content, sizeof(content) - 1, fp)) {
            content[strcspn(content, "\n")] = '\0';

            // 特殊处理soc_id：纯数字则添加SM前缀
            if (strstr(soc_files[i], "soc_id") && is_numeric(content)) {
                snprintf(cpu_model, MAX_CPU_MODEL_LEN, "SM%s", content);
                fclose(fp);
                return 1;
            }
            // 天玑设备可能使用soc_name/chip_name
            else if (strstr(soc_files[i], "soc_name") ||
                strstr(soc_files[i], "chip_name")) {
                // 直接尝试提取型号
                if (extract_chip_model(content)) {
                    fclose(fp);
                    return 1;
                }

                // 如果是mt开头的小写字符串
                if (strstr(content, "mt")) {
                    safe_strncpy(cpu_model, content, MAX_CPU_MODEL_LEN);
                    // 转换为大写
                    for (char* p = cpu_model; *p; p++) {
                        *p = toupper((unsigned char)*p);
                    }
                    fclose(fp);
                    return 1;
                }
            }
            // 其他文件尝试直接提取型号
            else if (extract_chip_model(content)) {
                fclose(fp);
                return 1;
            }
        }
        fclose(fp);
    }

    // 3. 检查/proc/cpuinfo
    FILE* cpuinfo = fopen("/proc/cpuinfo", "r");
    if (cpuinfo) {
        char line[256];
        while (fgets(line, sizeof(line), cpuinfo)) {
            // 检测多种可能的标识字段
            if (strstr(line, "Hardware") ||
                strstr(line, "hardware") ||
                strstr(line, "Processor") ||
                strstr(line, "processor")) {

                char* colon = strchr(line, ':');
                if (colon) {
                    char* value = colon + 1;
                    // 跳过前导空格
                    while (isspace(*value)) value++;
                    // 清理值
                    char* end = strchr(value, '\n');
                    if (end) *end = '\0';
                    // 尝试提取型号
                    if (extract_chip_model(value)) {
                        fclose(cpuinfo);
                        return 1;
                    }
                }
            }
        }
        fclose(cpuinfo);
    }

    // 4. 检查dmesg输出 - 需要root权限
    FILE* dmesg = popen("su -c 'dmesg | grep -iE \"soc|chip|qcom|msm|mtk|mediatek|sm[0-9]+|mt[0-9]+\"' 2>/dev/null", "r");
    if (dmesg) {
        char line[512];
        while (fgets(line, sizeof(line), dmesg)) {
            // 尝试提取型号
            if (extract_chip_model(line)) {
                pclose(dmesg);
                return 1;
            }
        }
        pclose(dmesg);
    }

    // 5. 检查/sys/firmware/devicetree/base/model (备用方法)
    if (file_exists("/sys/firmware/devicetree/base/model")) {
        FILE* fp = fopen("/sys/firmware/devicetree/base/model", "r");
        if (fp) {
            char model[128];
            if (fgets(model, sizeof(model), fp)) {
                model[strcspn(model, "\n")] = '\0';
                if (extract_chip_model(model)) {
                    fclose(fp);
                    return 1;
                }
            }
            fclose(fp);
        }
    }

    // 6. 天玑专用检测：/proc/device-tree/compatible
    if (file_exists("/proc/device-tree/compatible")) {
        FILE* fp = fopen("/proc/device-tree/compatible", "r");
        if (fp) {
            char content[256];
            size_t len = fread(content, 1, sizeof(content) - 1, fp);
            content[len] = '\0';

            // 查找MTxxxx模式
            char* ptr = content;
            while ((ptr = strstr(ptr, "mt"))) {
                // 检查是否是有效的型号格式 (mtXXXX)
                if (isdigit(ptr[2]) && isdigit(ptr[3]) && isdigit(ptr[4]) && isdigit(ptr[5])) {
                    snprintf(cpu_model, MAX_CPU_MODEL_LEN, "MT%.4s", ptr + 2);
                    fclose(fp);
                    return 1;
                }
                ptr++;
            }
            fclose(fp);
        }
    }

    return 0; // 未检测到
}

// 辅助函数：从字符串中提取芯片型号
int extract_chip_model(const char* input) {
    // 0. 特殊处理一加的OPSM格式
    if (strstr(input, "OPSM") || strstr(input, "CPHSM")) {
        const char* sm_ptr = strstr(input, "SM");
        if (sm_ptr) {
            // 复制整个SMxxxx字符串
            int idx = 0;
            while (*sm_ptr && (isalnum(*sm_ptr) || *sm_ptr == '-') && idx < MAX_CPU_MODEL_LEN - 1) {
                cpu_model[idx++] = *sm_ptr++;
            }
            cpu_model[idx] = '\0';
            return 1;
        }
    }

    // 1. 尝试匹配高通型号 (SMxxxx, QCOM, Snapdragon)
    const char* qualcomm_patterns[] = { "SM", "QCOM", "Snapdragon", "sdm", "msm", NULL };
    for (int i = 0; qualcomm_patterns[i]; i++) {
        const char* ptr = strstr(input, qualcomm_patterns[i]);
        if (ptr) {
            // 对于数字开头的模式 (如soc_id中的纯数字)
            if (is_numeric(ptr) && strlen(ptr) <= 5) {
                snprintf(cpu_model, MAX_CPU_MODEL_LEN, "SM%s", ptr);
                return 1;
            }

            // 复制整个型号字符串
            int idx = 0;
            while (*ptr && (isalnum(*ptr) || *ptr == '-' || *ptr == '_') && idx < MAX_CPU_MODEL_LEN - 1) {
                cpu_model[idx++] = *ptr++;
            }
            cpu_model[idx] = '\0';

            // 确保SM前缀为大写
            if (strncmp(cpu_model, "sm", 2) == 0) {
                cpu_model[0] = 'S';
                cpu_model[1] = 'M';
            }

            // 清理下划线和多余字符
            for (char* p = cpu_model; *p; p++) {
                if (*p == '_') *p = '-';
            }

            return 1;
        }
    }

    // 2. 尝试匹配天玑型号 (MTxxxx, mt, Dimensity)
    const char* mediatek_patterns[] = { "MT", "mt", "Dimensity", "dimensity", "MTK", "mtk", NULL };
    for (int i = 0; mediatek_patterns[i]; i++) {
        const char* ptr = strstr(input, mediatek_patterns[i]);
        if (ptr) {
            // 对于纯数字（如soc_id中的情况）
            if (is_numeric(ptr) && strlen(ptr) == 4) {
                snprintf(cpu_model, MAX_CPU_MODEL_LEN, "MT%s", ptr);
                return 1;
            }

            // 复制整个型号字符串
            int idx = 0;

            // 如果是Dimensity，定位到数字部分
            if (strncasecmp(ptr, "dimensity", 9) == 0) {
                ptr += 9;
                // 跳过空格和符号
                while (*ptr && !isdigit(*ptr)) ptr++;
            }

            // 如果是mt开头的小写字符串
            else if (strncasecmp(ptr, "mt", 2) == 0 && !isdigit(ptr[2])) {
                // 跳过"mt"前缀
                ptr += 2;
            }

            // 复制有效的型号部分
            while (*ptr && (isalnum(*ptr) || *ptr == '-' || *ptr == '_') && idx < MAX_CPU_MODEL_LEN - 1) {
                // 遇到小写字母时停止（天玑型号后可能有小写后缀）
                if (idx > 0 && islower(*ptr) && !isdigit(*ptr)) {
                    break;
                }
                cpu_model[idx++] = *ptr++;
            }
            cpu_model[idx] = '\0';

            // 转换为大写
            for (char* p = cpu_model; *p; p++) {
                *p = toupper((unsigned char)*p);
            }

            // 处理Dimensity开头的型号
            if (strncmp(cpu_model, "DIMENSITY", 9) == 0) {
                // 提取数字部分
                const char* num_ptr = cpu_model + 9;
                while (*num_ptr && !isdigit(*num_ptr)) num_ptr++;

                if (isdigit(*num_ptr)) {
                    char tmp[32];
                    snprintf(tmp, sizeof(tmp), "MT%s", num_ptr);
                    strcpy(cpu_model, tmp);
                }
            }
            // 添加MT前缀如果是纯数字
            else if (is_numeric(cpu_model) && strlen(cpu_model) == 4) {
                char tmp[32];
                snprintf(tmp, sizeof(tmp), "MT%s", cpu_model);
                strcpy(cpu_model, tmp);
            }
            // 确保MT前缀存在
            else if (strncmp(cpu_model, "MT", 2) != 0 && strlen(cpu_model) >= 4) {
                char tmp[32];
                snprintf(tmp, sizeof(tmp), "MT%s", cpu_model);
                strcpy(cpu_model, tmp);
            }

            return 1;
        }
    }

    // 3. 尝试匹配Exynos型号
    if (strstr(input, "Exynos") || strstr(input, "exynos")) {
        const char* ptr = strstr(input, "Exynos");
        if (!ptr) ptr = strstr(input, "exynos");

        if (ptr) {
            // 跳过"Exynos"字符串
            ptr += (ptr[0] == 'E' || ptr[0] == 'e') ? 6 : 0;

            // 复制整个型号
            int idx = 0;
            while (*ptr && (isalnum(*ptr) || *ptr == '-') && idx < MAX_CPU_MODEL_LEN - 1) {
                cpu_model[idx++] = *ptr++;
            }
            cpu_model[idx] = '\0';

            // 确保首字母大写
            if (cpu_model[0]) cpu_model[0] = toupper(cpu_model[0]);
            return 1;
        }
    }

    // 4. 尝试匹配通用数字型号 (如8250, 8650)
    if (is_numeric(input) && strlen(input) == 4) {
        // 优先假设是高通
        snprintf(cpu_model, MAX_CPU_MODEL_LEN, "SM%s", input);
        return 1;
    }

    // 5. 尝试匹配小写mt开头的型号 (如mt6895)
    if (strstr(input, "mt") && isdigit(input[2]) && isdigit(input[3]) && isdigit(input[4]) && isdigit(input[5])) {
        snprintf(cpu_model, MAX_CPU_MODEL_LEN, "MT%.4s", input + 2);
        return 1;
    }

    return 0;
}

// 辅助函数：检查字符串是否为纯数字
int is_numeric(const char* str) {
    for (const char* p = str; *p; p++) {
        if (!isdigit((unsigned char)*p)) {
            return 0;
        }
    }
    return 1;
}

// 检测CPU簇
void detect_cpu_clusters() {
    policy_count = 0;
    cluster_count = 0;

    // 1. 收集所有存在的策略
    for (int i = 0; i < MAX_POLICIES; i++) {
        char path[128];
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpufreq/policy%d", i);
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
        snprintf(path, sizeof(path),
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
        snprintf(cluster_info, sizeof(cluster_info), "Cluster %d (CPUs: %s, 策略: [",
            cpu_clusters[i].cluster_id, cpu_clusters[i].related_cpus);

        for (int j = 0; j < cpu_clusters[i].policy_count; j++) {
            char policy_str[16];
            snprintf(policy_str, sizeof(policy_str), "%d", cpu_clusters[i].policies[j]);
            strcat(cluster_info, policy_str);
            if (j < cpu_clusters[i].policy_count - 1) {
                strcat(cluster_info, ", ");
            }
        }
        strcat(cluster_info, "])\n");

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

    if (!file_exists(path)) return 0;

    FILE* fp = fopen(path, "r");
    if (!fp) return 0;

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
            safe_strncpy(governors[count], token, 32);
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

    int has_gov_dir = file_exists(gov_path);

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
        if (file_exists(param_path)) {
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
const char* get_cpu_config_path() {
    static char path[256];
    snprintf(path, sizeof(path), "%s%s.json", CPU_CONFIG_DIR, cpu_model);
    return path;
}

// 写入值到文件（增强权限处理）
int lock_val(const char* path, const char* value) {
    // 第一次尝试写入
    FILE* fp = fopen(path, "w");
    if (fp) {
        int result = fprintf(fp, "%s", value);
        fclose(fp);

        if (result < 0) {
            char log_msg[256];
            snprintf(log_msg, sizeof(log_msg), "写入失败: %s -> %s (错误: %s)",
                path, value, strerror(errno));
            log_message(log_msg);
            return 0;
        }
        return 1;
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
            else {
                fclose(fp);
                return 1;
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

    // 使用echo命令作为最后手段
    char command[512];
    snprintf(command, sizeof(command), "echo '%s' > '%s' 2>/dev/null", value, path);
    int ret = system(command);
    if (ret != 0) {
        char log_msg[256];
        snprintf(log_msg, sizeof(log_msg), "无法写入 %s: echo命令失败 (错误: %d)", path, ret);
        log_message(log_msg);
        return 0;
    }

    return 1;
}

// 获取前台应用
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

                // 验证包名格式 - 必须包含点号且长度合理
                if (strlen(candidate) > 0) {
                    // 检查是否包含点号（有效包名特征）
                    int has_dot = 0;
                    int has_valid_chars = 1;

                    for (char* p = candidate; *p; p++) {
                        if (*p == '.') has_dot = 1;
                        // 检查是否只包含合法字符（字母、数字、点、下划线）
                        if (!isalnum((unsigned char)*p) && *p != '.' && *p != '_') {
                            has_valid_chars = 0;
                            break;
                        }
                    }

                    // 有效包名必须包含点号，长度至少为3，且包含合法字符
                    if (has_dot && has_valid_chars && strlen(candidate) >= 3) {
                        // 保存最后一个有效包名
                        if (last_valid) free(last_valid);
                        last_valid = strdup(candidate);
                    }
                }
            }
        }
    }

    pclose(fp);

    // 返回最后一个有效包名或 unknown
    if (last_valid) {
        return last_valid;
    }
    return strdup("unknown");
}

// 加载配置文件中的版本号
void load_config_version() {
    // 首先设置默认版本为5.0
    safe_strncpy(config_version, "5.0", sizeof(config_version));

    const char* config_path = get_cpu_config_path();

    // 添加详细的日志信息
    char log_msg[256];
    snprintf(log_msg, sizeof(log_msg), "检查配置文件: %s", config_path);
    log_message(log_msg);

    if (!file_exists(config_path)) {
        snprintf(log_msg, sizeof(log_msg), "配置文件不存在: %s", config_path);
        log_message(log_msg);
        return;
    }

    FILE* fp = fopen(config_path, "rb");
    if (!fp) {
        snprintf(log_msg, sizeof(log_msg), "无法打开配置文件: %s (错误: %s)",
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

    size_t bytes_read = fread(data, 1, len, fp);
    data[bytes_read] = 0;
    fclose(fp);

    // 解析JSON
    cJSON* root = cJSON_Parse(data);
    if (!root) {
        const char* error_ptr = cJSON_GetErrorPtr();
        if (error_ptr) {
            snprintf(log_msg, sizeof(log_msg), "JSON解析失败: %s (位置: %p)", error_ptr, error_ptr);
            log_message(log_msg);
        }
        else {
            log_message("JSON解析失败: 未知错误");
        }
        free(data);
        return;
    }

    // 解析版本号
    cJSON* version = cJSON_GetObjectItem(root, "version");
    if (version && cJSON_IsString(version)) {
        safe_strncpy(config_version, version->valuestring, sizeof(config_version));
        // 不记录版本号相关的日志
    }

    cJSON_Delete(root);
    free(data);
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
            if (file_exists(path)) {
                char min_freq_str[32];
                snprintf(min_freq_str, sizeof(min_freq_str), "%d", ddr.min_freq);
                lock_val(path, min_freq_str);
            }

            // 设置最大频率
            snprintf(path, sizeof(path),
                "/sys/devices/system/cpu/bus_dcvs/DDR/%s/max_freq",
                entry->d_name);
            if (file_exists(path)) {
                char max_freq_str[32];
                snprintf(max_freq_str, sizeof(max_freq_str), "%d", ddr.max_freq);
                lock_val(path, max_freq_str);
            }
        }
    }
    closedir(dir);
}

// 加载模式设置JSON
void load_mode_settings_json(const char* mode) {
    const char* config_path = get_cpu_config_path();
    if (!file_exists(config_path)) {
        char error_msg[256];
        snprintf(error_msg, sizeof(error_msg), "未找到CPU配置文件: %s", config_path);
        log_message(error_msg);
        return;
    }

    FILE* fp = fopen(config_path, "rb");
    if (!fp) {
        char error_msg[256];
        snprintf(error_msg, sizeof(error_msg), "无法打开配置文件: %s (错误: %s)",
            config_path, strerror(errno));
        log_message(error_msg);
        return;
    }

    // 获取文件大小
    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    // 分配内存
    char* data = (char*)malloc(len + 1);
    if (!data) {
        log_message("内存分配失败");
        fclose(fp);
        return;
    }

    // 读取数据
    size_t bytes_read = fread(data, 1, len, fp);
    data[bytes_read] = 0;
    fclose(fp);

    // 解析JSON
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
                                    safe_strncpy(p->governor, governor->valuestring, sizeof(p->governor));
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
                                            safe_strncpy(p->param_names[p->param_count], param->string, 32);
                                            safe_strncpy(p->param_values[p->param_count], param->valuestring, 128);
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
            safe_strncpy(cpuset.background, background->valuestring, sizeof(cpuset.background));
        }

        cJSON* systembackground = cJSON_GetObjectItem(cpuset_obj, "systembackground");
        if (systembackground && cJSON_IsString(systembackground)) {
            safe_strncpy(cpuset.systembackground, systembackground->valuestring,
                sizeof(cpuset.systembackground));
        }

        cJSON* foreground = cJSON_GetObjectItem(cpuset_obj, "foreground");
        if (foreground && cJSON_IsString(foreground)) {
            safe_strncpy(cpuset.foreground, foreground->valuestring, sizeof(cpuset.foreground));
        }

        cJSON* topapp = cJSON_GetObjectItem(cpuset_obj, "topapp");
        if (topapp && cJSON_IsString(topapp)) {
            safe_strncpy(cpuset.topapp, topapp->valuestring, sizeof(cpuset.topapp));
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
    free(data);
}

// 应用特殊应用的CPU设置
void apply_special_app_cpu_settings(const SpecialAppSetting* app_setting) {


    for (int i = 0; i < app_setting->policy_count && i < policy_count; i++) {
        int policy_num = cpu_policies[i].policy_num;
        char path[256];

        // 应用最大频率
        if (app_setting->policies[i].max_freq > 0) {
            snprintf(path, sizeof(path),
                "/sys/devices/system/cpu/cpufreq/policy%d/scaling_max_freq",
                policy_num);

            char max_freq_str[32];
            snprintf(max_freq_str, sizeof(max_freq_str), "%d", app_setting->policies[i].max_freq);
            lock_val(path, max_freq_str);
        }

        // 应用最小频率
        if (app_setting->policies[i].min_freq > 0) {
            snprintf(path, sizeof(path),
                "/sys/devices/system/cpu/cpufreq/policy%d/scaling_min_freq",
                policy_num);

            char min_freq_str[32];
            snprintf(min_freq_str, sizeof(min_freq_str), "%d", app_setting->policies[i].min_freq);
            lock_val(path, min_freq_str);
        }

        // 应用调速器
        if (strlen(app_setting->policies[i].governor) > 0 &&
            is_governor_supported(policy_num, app_setting->policies[i].governor)) {

            snprintf(path, sizeof(path),
                "/sys/devices/system/cpu/cpufreq/policy%d/scaling_governor",
                policy_num);
            lock_val(path, app_setting->policies[i].governor);

            // 应用调速器参数
            char gov_path[256];
            snprintf(gov_path, sizeof(gov_path),
                "/sys/devices/system/cpu/cpufreq/policy%d/%s",
                policy_num, app_setting->policies[i].governor);

            if (file_exists(gov_path)) {
                for (int j = 0; j < app_setting->policies[i].param_count; j++) {
                    char param_path[256];
                    snprintf(param_path, sizeof(param_path),
                        "%s/%s", gov_path, app_setting->policies[i].param_names[j]);

                    if (file_exists(param_path)) {
                        lock_val(param_path, app_setting->policies[i].param_values[j]);
                    }
                }
            }
        }
    }
}

// 应用特殊应用的DDR设置
void apply_special_app_ddr_settings(const SpecialAppSetting* app_setting) {


    if (app_setting->ddr_min_freq <= 0 && app_setting->ddr_max_freq <= 0) {
        return;
    }

    DIR* dir = opendir("/sys/devices/system/cpu/bus_dcvs/DDR");
    if (!dir) {
        return;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_DIR && strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
            char path[256];

            // 应用最小频率
            if (app_setting->ddr_min_freq > 0) {
                snprintf(path, sizeof(path),
                    "/sys/devices/system/cpu/bus_dcvs/DDR/%s/min_freq",
                    entry->d_name);

                if (file_exists(path)) {
                    char min_freq_str[32];
                    snprintf(min_freq_str, sizeof(min_freq_str), "%d", app_setting->ddr_min_freq);
                    lock_val(path, min_freq_str);
                }
            }

            // 应用最大频率
            if (app_setting->ddr_max_freq > 0) {
                snprintf(path, sizeof(path),
                    "/sys/devices/system/cpu/bus_dcvs/DDR/%s/max_freq",
                    entry->d_name);

                if (file_exists(path)) {
                    char max_freq_str[32];
                    snprintf(max_freq_str, sizeof(max_freq_str), "%d", app_setting->ddr_max_freq);
                    lock_val(path, max_freq_str);
                }
            }
        }
    }
    closedir(dir);
}

// 应用特殊应用的cpuset设置
void apply_special_app_cpuset_settings(const SpecialAppSetting* app_setting) {


    if (strlen(app_setting->cpuset_background) > 0) {
        lock_val("/dev/cpuset/background/cpus", app_setting->cpuset_background);
    }

    if (strlen(app_setting->cpuset_systembackground) > 0) {
        lock_val("/dev/cpuset/system-background/cpus", app_setting->cpuset_systembackground);
    }

    if (strlen(app_setting->cpuset_foreground) > 0) {
        lock_val("/dev/cpuset/foreground/cpus", app_setting->cpuset_foreground);
    }

    if (strlen(app_setting->cpuset_topapp) > 0) {
        lock_val("/dev/cpuset/top-app/cpus", app_setting->cpuset_topapp);
    }
}

// 设置特殊应用设置
void set_special_app_settings(const char* package) {


    for (int i = 0; i < special_apps_count; i++) {
        if (strcmp(special_apps_cache[i].package, package) == 0) {
            SpecialAppSetting* app_setting = &special_apps_cache[i];

            char log_header[512];
            snprintf(log_header, sizeof(log_header),
                "===== 应用特殊设置: %s =====", package);
            log_message(log_header);

            // 记录特殊应用设置摘要
            char summary[2048] = "设置内容: ";

            // CPU策略设置
            if (app_setting->policy_count > 0) {
                strcat(summary, "CPU策略[");
                for (int j = 0; j < app_setting->policy_count; j++) {
                    char policy_summary[256];
                    snprintf(policy_summary, sizeof(policy_summary),
                        "policy%d: min=%d max=%d gov=%s params=%d",
                        j,
                        app_setting->policies[j].min_freq,
                        app_setting->policies[j].max_freq,
                        app_setting->policies[j].governor,
                        app_setting->policies[j].param_count);
                    strcat(summary, policy_summary);
                    if (j < app_setting->policy_count - 1) strcat(summary, "; ");
                }
                strcat(summary, "] ");
            }

            // DDR设置
            if (app_setting->ddr_min_freq > 0 || app_setting->ddr_max_freq > 0) {
                char ddr_summary[128];
                snprintf(ddr_summary, sizeof(ddr_summary), "DDR: min=%d max=%d ",
                    app_setting->ddr_min_freq, app_setting->ddr_max_freq);
                strcat(summary, ddr_summary);
            }

            // Cpuset设置
            if (strlen(app_setting->cpuset_background) > 0 ||
                strlen(app_setting->cpuset_systembackground) > 0 ||
                strlen(app_setting->cpuset_foreground) > 0 ||
                strlen(app_setting->cpuset_topapp) > 0) {

                strcat(summary, "Cpuset: [");
                if (strlen(app_setting->cpuset_background) > 0) {
                    strcat(summary, "bg=");
                    strcat(summary, app_setting->cpuset_background);
                    strcat(summary, ",");
                }
                if (strlen(app_setting->cpuset_systembackground) > 0) {
                    strcat(summary, "sysbg=");
                    strcat(summary, app_setting->cpuset_systembackground);
                    strcat(summary, ",");
                }
                if (strlen(app_setting->cpuset_foreground) > 0) {
                    strcat(summary, "fg=");
                    strcat(summary, app_setting->cpuset_foreground);
                    strcat(summary, ",");
                }
                if (strlen(app_setting->cpuset_topapp) > 0) {
                    strcat(summary, "topapp=");
                    strcat(summary, app_setting->cpuset_topapp);
                }
                strcat(summary, "]");
            }

            log_message(summary);

            // 应用所有设置
            apply_special_app_cpu_settings(app_setting);
            apply_special_app_ddr_settings(app_setting);
            apply_special_app_cpuset_settings(app_setting);

            // 结束标记
            log_message("===== 特殊设置应用完成 =====");
            return;
        }
    }
}

// 加载特殊应用缓存
void load_special_apps_cache() {
    special_apps_count = 0;
    const char* config_path = get_cpu_config_path();

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
    size_t bytes_read = fread(data, 1, len, fp);
    data[bytes_read] = 0;
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
                SpecialAppSetting* setting = &special_apps_cache[i];
                safe_strncpy(setting->package, package, sizeof(setting->package));

                // 解析完整的CPU策略设置
                cJSON* policies = cJSON_GetObjectItem(app, "cpu_policies");
                if (policies && cJSON_IsArray(policies)) {
                    int policy_idx = 0;
                    cJSON* policy;
                    cJSON_ArrayForEach(policy, policies) {
                        if (policy_idx >= MAX_POLICIES) break;

                        // 频率设置
                        cJSON* min_freq = cJSON_GetObjectItem(policy, "min_freq");
                        if (min_freq && cJSON_IsNumber(min_freq)) {
                            setting->policies[policy_idx].min_freq = min_freq->valueint;
                        }

                        cJSON* max_freq = cJSON_GetObjectItem(policy, "max_freq");
                        if (max_freq && cJSON_IsNumber(max_freq)) {
                            setting->policies[policy_idx].max_freq = max_freq->valueint;
                        }

                        // 调速器
                        cJSON* governor = cJSON_GetObjectItem(policy, "governor");
                        if (governor && cJSON_IsString(governor)) {
                            safe_strncpy(setting->policies[policy_idx].governor,
                                governor->valuestring,
                                sizeof(setting->policies[policy_idx].governor));
                        }

                        // 调速器参数
                        cJSON* params = cJSON_GetObjectItem(policy, "params");
                        if (params && cJSON_IsObject(params)) {
                            setting->policies[policy_idx].param_count = 0;
                            cJSON* param = NULL;
                            cJSON_ArrayForEach(param, params) {
                                if (setting->policies[policy_idx].param_count >= MAX_GOV_PARAMS) break;

                                if (cJSON_IsString(param)) {
                                    safe_strncpy(setting->policies[policy_idx].param_names[setting->policies[policy_idx].param_count],
                                        param->string, 32);

                                    safe_strncpy(setting->policies[policy_idx].param_values[setting->policies[policy_idx].param_count],
                                        param->valuestring, 128);

                                    setting->policies[policy_idx].param_count++;
                                }
                            }
                        }

                        policy_idx++;
                    }
                    setting->policy_count = policy_idx;
                }
                else {
                    setting->policy_count = 0;
                }

                // 解析DDR设置
                cJSON* ddr_obj = cJSON_GetObjectItem(app, "ddr");
                if (ddr_obj) {
                    cJSON* min_freq = cJSON_GetObjectItem(ddr_obj, "min_freq");
                    if (min_freq && cJSON_IsNumber(min_freq)) {
                        setting->ddr_min_freq = min_freq->valueint;
                    }

                    cJSON* max_freq = cJSON_GetObjectItem(ddr_obj, "max_freq");
                    if (max_freq && cJSON_IsNumber(max_freq)) {
                        setting->ddr_max_freq = max_freq->valueint;
                    }
                }

                // 解析cpuset设置
                cJSON* cpuset_obj = cJSON_GetObjectItem(app, "cpuset");
                if (cpuset_obj) {
                    cJSON* background = cJSON_GetObjectItem(cpuset_obj, "background");
                    if (background && cJSON_IsString(background)) {
                        safe_strncpy(setting->cpuset_background, background->valuestring,
                            sizeof(setting->cpuset_background));
                    }

                    cJSON* systembackground = cJSON_GetObjectItem(cpuset_obj, "systembackground");
                    if (systembackground && cJSON_IsString(systembackground)) {
                        safe_strncpy(setting->cpuset_systembackground, systembackground->valuestring,
                            sizeof(setting->cpuset_systembackground));
                    }

                    cJSON* foreground = cJSON_GetObjectItem(cpuset_obj, "foreground");
                    if (foreground && cJSON_IsString(foreground)) {
                        safe_strncpy(setting->cpuset_foreground, foreground->valuestring,
                            sizeof(setting->cpuset_foreground));
                    }

                    cJSON* topapp = cJSON_GetObjectItem(cpuset_obj, "topapp");
                    if (topapp && cJSON_IsString(topapp)) {
                        safe_strncpy(setting->cpuset_topapp, topapp->valuestring,
                            sizeof(setting->cpuset_topapp));
                    }
                }

                i++;
                special_apps_count++;
                package = strtok(NULL, ",");
            }
            free(packages);
        }
    }

    cJSON_Delete(root);
    free(data);

    char log_msg[128];
    snprintf(log_msg, sizeof(log_msg), "加载了 %d 个特殊应用设置", special_apps_count);
    log_message(log_msg);
}

// 处理inotify事件
void handle_inotify_events(int inotify_fd, int wd_mode_file, int wd_config_file, int wd_config_dir) {
    char buffer[INOTIFY_BUF_LEN];
    ssize_t length = read(inotify_fd, buffer, INOTIFY_BUF_LEN);
    if (length < 0) {
        return; // 没有事件
    }

    for (char* ptr = buffer; ptr < buffer + length; ) {
        struct inotify_event* event = (struct inotify_event*)ptr;
        ptr += INOTIFY_EVENT_SIZE + event->len;

        // 忽略临时文件
        if (event->len > 0 && strstr(event->name, ".tmp") != NULL) {
            continue;
        }

        if (event->mask & (IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_TO)) {
            if (event->wd == wd_mode_file) {
                log_message("检测到模式文件更新");
                // 立即重载模式设置
                char mode[64] = "balance";
                FILE* mode_file = fopen(MODE_FILE, "r");
                if (mode_file) {
                    char line[256];
                    if (fgets(line, sizeof(line), mode_file)) {
                        line[strcspn(line, "\n")] = 0;
                        safe_strncpy(mode, line, sizeof(mode));
                    }
                    fclose(mode_file);
                }
                load_mode_settings_json(mode);
            }
            else if (event->wd == wd_config_file) {
                log_message("检测到CPU配置文件更新");
                load_special_apps_cache();
            }
            else if (event->wd == wd_config_dir) {
                // 检查是否是当前CPU型号的配置文件
                if (event->len > 0 && strstr(event->name, cpu_model) != NULL) {
                    log_message("检测到CPU配置文件更新");
                    load_special_apps_cache();
                }
            }
        }
    }
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
        safe_strncpy(cpu_model, "default", sizeof(cpu_model));
    }

    char log_msg[128];
    snprintf(log_msg, sizeof(log_msg), "检测到CPU型号: %s", cpu_model);
    log_message(log_msg);

    // 加载配置文件版本 - 必须在检测CPU型号之后调用！
    load_config_version();

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

    // 初始化inotify
    int inotify_fd = inotify_init1(IN_NONBLOCK);
    if (inotify_fd < 0) {
        log_message("无法初始化inotify");
    }

    int wd_mode_file = -1;
    int wd_config_file = -1;
    int wd_config_dir = -1;

    // 添加inotify监听
    if (inotify_fd >= 0) {
        // 监听模式文件
        wd_mode_file = inotify_add_watch(inotify_fd, MODE_FILE,
            IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_TO);

        // 监听CPU配置文件
        const char* config_path = get_cpu_config_path();
        wd_config_file = inotify_add_watch(inotify_fd, config_path,
            IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_TO);

        // 监听配置文件目录
        wd_config_dir = inotify_add_watch(inotify_fd, CPU_CONFIG_DIR,
            IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_DELETE);

        log_message("inotify监听已设置");
    }

    char last_app[MAX_APP_NAME_LEN] = "";
    struct timeval tv;

    while (1) {
        // 设置超时时间为1秒
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        fd_set fds;
        FD_ZERO(&fds);

        if (inotify_fd >= 0) {
            FD_SET(inotify_fd, &fds);
        }

        // 使用select等待事件
        int max_fd = inotify_fd;
        int ret = select(max_fd + 1, &fds, NULL, NULL, &tv);

        if (ret > 0 && inotify_fd >= 0 && FD_ISSET(inotify_fd, &fds)) {
            handle_inotify_events(inotify_fd, wd_mode_file, wd_config_file, wd_config_dir);
        }

        // 处理前台应用检测
        char* current_app = get_foreground_app();

        // 新添加：过滤无效应用名称（长度过短、包含空格等）
        if (strlen(current_app) < 3 || strchr(current_app, ' ') != NULL) {
            free(current_app);
            continue; // 跳过本次循环
        }

        // 忽略某些系统应用
        if (strcmp(current_app, "com.omarea.vtools") == 0 ||
            strcmp(current_app, "NotificationShade") == 0 ||
            strcmp(current_app, "com.android.systemui") == 0) {
            free(current_app);
            continue;
        }

        if (strcmp(current_app, "unknown") != 0 &&
            strcmp(current_app, last_app) != 0) {
            safe_strncpy(last_app, current_app, sizeof(last_app));

            // 读取模式文件
            FILE* mode_file = fopen(MODE_FILE, "r");
            if (!mode_file) {
                log_message("未找到模式文件");
                free(current_app);
                continue;
            }

            char mode[64] = "balance";
            char line[256];

            if (fgets(line, sizeof(line), mode_file)) {
                line[strcspn(line, "\n")] = 0;
                safe_strncpy(mode, line, sizeof(mode));
            }

            // 检查是否有应用特定的模式
            while (fgets(line, sizeof(line), mode_file)) {
                line[strcspn(line, "\n")] = 0;
                char* package = strtok(line, " ");
                char* custom_mode = strtok(NULL, " ");

                if (package && custom_mode && strcmp(package, current_app) == 0) {
                    safe_strncpy(mode, custom_mode, sizeof(mode));
                    break;
                }
            }
            fclose(mode_file);

            // 加载和应用设置
            load_mode_settings_json(mode);
            apply_settings();
            apply_ddr_settings();

            // 应用特殊应用设置（覆盖部分参数）
            set_special_app_settings(current_app);

            char log_msg[512];
            snprintf(log_msg, sizeof(log_msg),
                "应用: %s | 模式: %s | 设置已应用",
                current_app, mode);
            log_message(log_msg);
        }

        free(current_app);
    }

    if (inotify_fd >= 0) {
        close(inotify_fd);
    }

    return 0;
}