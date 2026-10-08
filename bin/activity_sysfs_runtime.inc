int remount_sysfs_rw() {
    if (mount("none", "/sys", NULL, MS_REMOUNT | MS_BIND, NULL) == 0) {
        log_message("成功重新挂载/sys为可读写");
        return 1;
    } else {
        char log_msg[128];
        safe_snprintf(log_msg, sizeof(log_msg), "重新挂载/sys失败: %s", strerror(errno));
        log_message(log_msg);
        return 0;
    }
}

int detect_cpu_model() {
    FILE* getprop = popen("getprop ro.soc.model", "r");
    if (getprop) {
        char line[64];
        if (fgets(line, sizeof(line), getprop)) {
            line[strcspn(line, "\n")] = '\0';
            if (extract_chip_model(line)) {
                pclose(getprop);
                return 1;
            }
        }
        pclose(getprop);
    }

    const char* build_prop_paths[] = {
        "/odm/build.prop",
        "/vendor/odm/etc/build.prop",
        "/system/vendor/build.prop",
        "/vendor/build.prop",
        "/system/build.prop",
        NULL
    };

    const char* prop_keys[] = {
        "ro.soc.model",
        "ro.product.oplus.cpuinfo",
        "ro.build.device_family",
        "ro.board.platform",
        "ro.chipname",
        "ro.vendor.product.cpuinfo",
        "ro.hardware.chipname",
        "ro.mediatek.platform",
        "ro.vendor.mediatek.platform",
        "ro.boot.platform",
        "ro.boot.chipname",
        "ro.boot.hardware.sku",
        "ro.hardware",
        "ro.product.board",
        "ro.mediatek.hardware",
        NULL
    };

    for (int i = 0; build_prop_paths[i]; i++) {
        if (!file_exists(build_prop_paths[i])) continue;

        FILE* buildprop = fopen(build_prop_paths[i], "r");
        if (!buildprop) continue;

        char line[256];
        while (fgets(line, sizeof(line), buildprop)) {
            for (int k = 0; prop_keys[k]; k++) {
                char key_pattern[64];
                safe_snprintf(key_pattern, sizeof(key_pattern), "%s=", prop_keys[k]);

                if (strstr(line, key_pattern)) {
                    char* value = strchr(line, '=') + 1;
                    char* end = strchr(value, '\n');
                    if (end) *end = '\0';

                    if (value[0] == '"' || value[0] == '\'') {
                        value++;
                        size_t len = strlen(value);
                        if (len > 0 && (value[len - 1] == '"' || value[len - 1] == '\'')) {
                            value[len - 1] = '\0';
                        }
                    }

                    if (strcmp(prop_keys[k], "ro.build.device_family") == 0) {
                        if (strlen(value) > 2 &&
                            (strncmp(value, "OP", 2) == 0 ||
                             strncmp(value, "CPH", 3) == 0)) {
                            char* model_ptr = strstr(value, "SM");
                            if (model_ptr) {
                                safe_strncpy(cpu_model, model_ptr, MAX_CPU_MODEL_LEN);
                                fclose(buildprop);
                                return 1;
                            }
                        }
                    }

                    if (strstr(prop_keys[k], "mediatek") ||
                        strstr(prop_keys[k], "mtk") ||
                        strstr(prop_keys[k], "board.platform")) {
                        if (extract_chip_model(value)) {
                            fclose(buildprop);
                            return 1;
                        }

                        if (is_numeric(value) && strlen(value) == 4) {
                            safe_snprintf(cpu_model, MAX_CPU_MODEL_LEN, "MT%s", value);
                            fclose(buildprop);
                            return 1;
                        }
                    }

                    if (extract_chip_model(value)) {
                        fclose(buildprop);
                        return 1;
                    }
                }
            }
        }
        fclose(buildprop);
    }

    const char* soc_files[] = {
        "/sys/devices/soc0/soc_id",
        "/sys/devices/soc0/machine",
        "/sys/devices/soc0/family",
        "/sys/class/soc/soc0/soc_id",
        "/sys/devices/soc0/soc_name",
        "/sys/devices/soc0/chip_name",
        NULL
    };

    for (int i = 0; soc_files[i]; i++) {
        if (!file_exists(soc_files[i])) continue;

        FILE* fp = fopen(soc_files[i], "r");
        if (!fp) continue;

        char content[32] = {0};
        if (fgets(content, sizeof(content) - 1, fp)) {
            content[strcspn(content, "\n")] = '\0';

            if (strstr(soc_files[i], "soc_id") && is_numeric(content)) {
                safe_snprintf(cpu_model, MAX_CPU_MODEL_LEN, "SM%s", content);
                fclose(fp);
                return 1;
            } else if (strstr(soc_files[i], "soc_name") ||
                       strstr(soc_files[i], "chip_name")) {
                if (extract_chip_model(content)) {
                    fclose(fp);
                    return 1;
                }

                if (strstr(content, "mt")) {
                    safe_strncpy(cpu_model, content, MAX_CPU_MODEL_LEN);
                    for (char* p = cpu_model; *p; p++) {
                        *p = toupper((unsigned char)*p);
                    }
                    fclose(fp);
                    return 1;
                }
            } else if (extract_chip_model(content)) {
                fclose(fp);
                return 1;
            }
        }
        fclose(fp);
    }

    FILE* cpuinfo = fopen("/proc/cpuinfo", "r");
    if (cpuinfo) {
        char line[256];
        while (fgets(line, sizeof(line), cpuinfo)) {
            if (strstr(line, "Hardware") ||
                strstr(line, "hardware") ||
                strstr(line, "Processor") ||
                strstr(line, "processor")) {
                char* colon = strchr(line, ':');
                if (colon) {
                    char* value = colon + 1;
                    while (isspace((unsigned char)*value)) value++;
                    char* end = strchr(value, '\n');
                    if (end) *end = '\0';
                    if (extract_chip_model(value)) {
                        fclose(cpuinfo);
                        return 1;
                    }
                }
            }
        }
        fclose(cpuinfo);
    }

    FILE* dmesg = popen("su -c 'dmesg | grep -iE \"soc|chip|qcom|msm|mtk|mediatek|sm[0-9]+|mt[0-9]+\"' 2>/dev/null", "r");
    if (dmesg) {
        char line[512];
        while (fgets(line, sizeof(line), dmesg)) {
            if (extract_chip_model(line)) {
                pclose(dmesg);
                return 1;
            }
        }
        pclose(dmesg);
    }

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

    if (file_exists("/proc/device-tree/compatible")) {
        FILE* fp = fopen("/proc/device-tree/compatible", "r");
        if (fp) {
            char content[256];
            size_t len = fread(content, 1, sizeof(content) - 1, fp);
            content[len] = '\0';

            char* ptr = content;
            while ((ptr = strstr(ptr, "mt"))) {
                if (isdigit((unsigned char)ptr[2]) &&
                    isdigit((unsigned char)ptr[3]) &&
                    isdigit((unsigned char)ptr[4]) &&
                    isdigit((unsigned char)ptr[5])) {
                    safe_snprintf(cpu_model, MAX_CPU_MODEL_LEN, "MT%.4s", ptr + 2);
                    fclose(fp);
                    return 1;
                }
                ptr++;
            }
            fclose(fp);
        }
    }

    return 0;
}

int extract_chip_model(const char* input) {
    if (strstr(input, "OPSM") || strstr(input, "CPHSM")) {
        const char* sm_ptr = strstr(input, "SM");
        if (sm_ptr) {
            int idx = 0;
            while (*sm_ptr && (isalnum((unsigned char)*sm_ptr) || *sm_ptr == '-') &&
                   idx < MAX_CPU_MODEL_LEN - 1) {
                cpu_model[idx++] = *sm_ptr++;
            }
            cpu_model[idx] = '\0';
            return 1;
        }
    }

    const char* qualcomm_patterns[] = {"SM", "QCOM", "Snapdragon", "sdm", "msm", NULL};
    for (int i = 0; qualcomm_patterns[i]; i++) {
        const char* ptr = strstr(input, qualcomm_patterns[i]);
        if (ptr) {
            if (is_numeric(ptr) && strlen(ptr) <= 5) {
                safe_snprintf(cpu_model, MAX_CPU_MODEL_LEN, "SM%s", ptr);
                return 1;
            }

            int idx = 0;
            while (*ptr && (isalnum((unsigned char)*ptr) || *ptr == '-' || *ptr == '_') &&
                   idx < MAX_CPU_MODEL_LEN - 1) {
                cpu_model[idx++] = *ptr++;
            }
            cpu_model[idx] = '\0';

            if (strncmp(cpu_model, "sm", 2) == 0) {
                cpu_model[0] = 'S';
                cpu_model[1] = 'M';
            }

            for (char* p = cpu_model; *p; p++) {
                if (*p == '_') *p = '-';
            }

            return 1;
        }
    }

    const char* mediatek_patterns[] = {"MT", "mt", "Dimensity", "dimensity", "MTK", "mtk", NULL};
    for (int i = 0; mediatek_patterns[i]; i++) {
        const char* ptr = strstr(input, mediatek_patterns[i]);
        if (ptr) {
            if (is_numeric(ptr) && strlen(ptr) == 4) {
                safe_snprintf(cpu_model, MAX_CPU_MODEL_LEN, "MT%s", ptr);
                return 1;
            }

            int idx = 0;
            if (strncasecmp(ptr, "dimensity", 9) == 0) {
                ptr += 9;
                while (*ptr && !isdigit((unsigned char)*ptr)) ptr++;
            } else if (strncasecmp(ptr, "mt", 2) == 0 && !isdigit((unsigned char)ptr[2])) {
                ptr += 2;
            }

            while (*ptr && (isalnum((unsigned char)*ptr) || *ptr == '-' || *ptr == '_') &&
                   idx < MAX_CPU_MODEL_LEN - 1) {
                if (idx > 0 && islower((unsigned char)*ptr) && !isdigit((unsigned char)*ptr)) {
                    break;
                }
                cpu_model[idx++] = *ptr++;
            }
            cpu_model[idx] = '\0';

            for (char* p = cpu_model; *p; p++) {
                *p = toupper((unsigned char)*p);
            }

            if (strncmp(cpu_model, "DIMENSITY", 9) == 0) {
                const char* num_ptr = cpu_model + 9;
                while (*num_ptr && !isdigit((unsigned char)*num_ptr)) num_ptr++;

                if (isdigit((unsigned char)*num_ptr)) {
                    char tmp[32];
                    safe_snprintf(tmp, sizeof(tmp), "MT%s", num_ptr);
                    strcpy(cpu_model, tmp);
                }
            } else if (is_numeric(cpu_model) && strlen(cpu_model) == 4) {
                char tmp[32];
                safe_snprintf(tmp, sizeof(tmp), "MT%s", cpu_model);
                strcpy(cpu_model, tmp);
            } else if (strncmp(cpu_model, "MT", 2) != 0 && strlen(cpu_model) >= 4) {
                char tmp[32];
                safe_snprintf(tmp, sizeof(tmp), "MT%s", cpu_model);
                strcpy(cpu_model, tmp);
            }

            return 1;
        }
    }

    if (strstr(input, "Exynos") || strstr(input, "exynos")) {
        const char* ptr = strstr(input, "Exynos");
        if (!ptr) ptr = strstr(input, "exynos");

        if (ptr) {
            ptr += (ptr[0] == 'E' || ptr[0] == 'e') ? 6 : 0;

            int idx = 0;
            while (*ptr && (isalnum((unsigned char)*ptr) || *ptr == '-') &&
                   idx < MAX_CPU_MODEL_LEN - 1) {
                cpu_model[idx++] = *ptr++;
            }
            cpu_model[idx] = '\0';

            if (cpu_model[0]) cpu_model[0] = (char)toupper((unsigned char)cpu_model[0]);
            return 1;
        }
    }

    if (is_numeric(input) && strlen(input) == 4) {
        safe_snprintf(cpu_model, MAX_CPU_MODEL_LEN, "SM%s", input);
        return 1;
    }

    if (strstr(input, "mt") &&
        isdigit((unsigned char)input[2]) &&
        isdigit((unsigned char)input[3]) &&
        isdigit((unsigned char)input[4]) &&
        isdigit((unsigned char)input[5])) {
        safe_snprintf(cpu_model, MAX_CPU_MODEL_LEN, "MT%.4s", input + 2);
        return 1;
    }

    return 0;
}

int is_numeric(const char* str) {
    for (const char* p = str; *p; p++) {
        if (!isdigit((unsigned char)*p)) {
            return 0;
        }
    }
    return 1;
}

int get_supported_governors(int policy_num, char governors[][32], int max_count) {
    char path[128];
    safe_snprintf(path, sizeof(path),
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
    char* saveptr = NULL;
    char* token = strtok_r(line, " ", &saveptr);
    while (token && count < max_count) {
        size_t len = strlen(token);
        while (len > 0 &&
               (token[len - 1] == '\n' || token[len - 1] == '\r' || token[len - 1] == ' ')) {
            token[--len] = '\0';
        }

        if (len > 0) {
            safe_strncpy(governors[count], token, 32);
            count++;
        }
        token = strtok_r(NULL, " ", &saveptr);
    }

    return count;
}

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

static int read_current_governor(int policy_num, char* out, size_t out_sz) {
    if (!out || out_sz == 0) return 0;
    out[0] = 0;

    char path[256];
    safe_snprintf(path, sizeof(path),
        "/sys/devices/system/cpu/cpufreq/policy%d/scaling_governor",
        policy_num);

    FILE* fp = fopen(path, "r");
    if (!fp) return 0;

    char buf[64];
    if (!fgets(buf, sizeof(buf), fp)) {
        fclose(fp);
        return 0;
    }
    fclose(fp);

    buf[strcspn(buf, "\r\n")] = 0;
    safe_strncpy(out, buf, out_sz);
    return out[0] ? 1 : 0;
}

void set_governor_params(int policy_num, const char* governor) {
    CpuPolicy* policy_ptr = NULL;
    for (int i = 0; i < policy_count; i++) {
        if (cpu_policies[i].policy_num == policy_num) {
            policy_ptr = &cpu_policies[i];
            break;
        }
    }

    if (!policy_ptr || policy_ptr->param_count == 0) return;

    char gov_path[256];
    safe_snprintf(gov_path, sizeof(gov_path),
        "/sys/devices/system/cpu/cpufreq/policy%d/%s",
        policy_num, governor);

    int has_gov_dir = file_exists(gov_path);

    for (int i = 0; i < policy_ptr->param_count; i++) {
        char param_path[256];
        if (has_gov_dir) {
            safe_snprintf(param_path, sizeof(param_path), "%s/%s", gov_path, policy_ptr->param_names[i]);
        } else {
            safe_snprintf(param_path, sizeof(param_path),
                "/sys/devices/system/cpu/cpufreq/policy%d/%s",
                policy_num, policy_ptr->param_names[i]);
        }

        if (file_exists(param_path)) {
            lock_val(param_path, policy_ptr->param_values[i]);
        }
    }
}

const char* get_cpu_config_path() {
    static char path[256];
    safe_snprintf(path, sizeof(path), "%s%s.json", CPU_CONFIG_DIR, cpu_model);
    return path;
}

const char* get_runtime_feature_config_path() {
    return CONFIG_DIR "sched.txt";
}

const char* get_scheduler_config_path() {
    static char path[256];
    safe_snprintf(path, sizeof(path), "%s%s.scheduler.json", CPU_CONFIG_DIR, cpu_model);
    return path;
}

static void build_cluster_layout_name(char* out, size_t out_size) {
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (cluster_count <= 0) return;
    size_t used = 0;
    for (int i = 0; i < cluster_count; i++) {
        int written = safe_snprintf(out + used, out_size - used, "%s%d", i == 0 ? "" : "+", cpu_clusters[i].cpu_count);
        if (written <= 0 || (size_t)written >= out_size - used) {
            break;
        }
        used += (size_t)written;
    }
}

const char* get_thread_signature_config_path() {
    static char path[256];
    safe_snprintf(path, sizeof(path), "%s%s.threads.json", CPU_CONFIG_DIR, cpu_model);
    if (file_exists(path)) {
        return path;
    }

    char layout_name[64];
    build_cluster_layout_name(layout_name, sizeof(layout_name));
    if (layout_name[0]) {
        static char layout_threads_path[256];
        safe_snprintf(layout_threads_path, sizeof(layout_threads_path), "%s%s.threads.json", CPU_CONFIG_DIR, layout_name);
        if (file_exists(layout_threads_path)) {
            return layout_threads_path;
        }

        static char layout_plain_path[256];
        safe_snprintf(layout_plain_path, sizeof(layout_plain_path), "%s%s.json", CPU_CONFIG_DIR, layout_name);
        if (file_exists(layout_plain_path)) {
            return layout_plain_path;
        }
    }
    return path;
}

/*
 * 内核节点写入路径。
 *
 * 现场暴露过两个缺陷：
 *
 *  1. 值被原样写入。部分 vendor 节点只接受一种 token 布局，例如 walt 的
 *     zone_max_util_pct（"<频率> <占用>"）在值尾带空格时直接返回 EINVAL；
 *     而任何回读节点再写回的值都会带尾空格（节点 show 出来就带）。
 *     写入前归一化，整类消除这种"手动 echo 成功、程序里失败"。
 *  2. 某路径因 EACCES/EPERM/EROFS/ENOENT 失败过一次就被永久拉黑，后续静默
 *     返回 0。开机早期节点尚未创建、/sys 仍只读等瞬态问题会让该参数一直
 *     不生效直到重启。现在改为永不拉黑、每次重试，失败日志按路径限流，
 *     并在写入后回读校验。
 */

#define MAX_WRITE_FAILURE_TRACKED 128
#define WRITE_FAILURE_LOG_INTERVAL_MS 30000LL
#define WRITE_VERIFY_LOG_INTERVAL_MS 120000LL

struct WriteFailureRecord {
    char path[256];
    long long last_fail_log_ms;
    long long last_verify_log_ms;
    unsigned long long fail_count;
    unsigned long long verify_mismatch_count;
};

static std::vector<WriteFailureRecord> g_write_failure_records;

static WriteFailureRecord* get_write_failure_record(const char* path) {
    if (!path || !path[0]) return NULL;
    for (size_t i = 0; i < g_write_failure_records.size(); i++) {
        if (strcmp(g_write_failure_records[i].path, path) == 0) {
            return &g_write_failure_records[i];
        }
    }
    if (g_write_failure_records.size() >= MAX_WRITE_FAILURE_TRACKED) {
        return NULL;
    }
    WriteFailureRecord record = {};
    safe_strncpy(record.path, path, sizeof(record.path));
    g_write_failure_records.push_back(record);
    return &g_write_failure_records.back();
}

/* 去掉首尾空白，并把内部连续空白折叠成单个空格。 */
static void normalize_config_value(const char* raw, char* out, size_t out_size) {
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!raw) return;

    size_t used = 0;
    bool pending_space = false;
    for (const char* p = raw; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (used > 0) pending_space = true;
            continue;
        }
        if (pending_space) {
            if (used + 2 >= out_size) break;
            out[used++] = ' ';
            pending_space = false;
        }
        if (used + 1 >= out_size) break;
        out[used++] = (char)c;
    }
    out[used] = '\0';
}

static void log_write_failure_throttled(WriteFailureRecord* record,
                                        const char* path,
                                        const char* value,
                                        const char* stage,
                                        int err) {
    long long now_ms = get_current_time_ms();
    if (record) {
        record->fail_count++;
        if (record->last_fail_log_ms != 0 &&
            now_ms - record->last_fail_log_ms < WRITE_FAILURE_LOG_INTERVAL_MS) {
            return;
        }
        record->last_fail_log_ms = now_ms;
    }

    char log_msg[384];
    safe_snprintf(log_msg, sizeof(log_msg),
        "写入失败(%s): %s -> %s (错误: %s, 累计 %llu 次)",
        stage ? stage : "write",
        path,
        value && value[0] ? value : "<empty>",
        strerror(err > 0 ? err : EIO),
        record ? record->fail_count : 0ULL);
    log_message(log_msg);
}

static bool parse_leading_double(const char* text, double* out) {
    if (!text || !text[0] || !out) return false;
    char* end = NULL;
    double value = strtod(text, &end);
    if (end == text) return false;
    *out = value;
    return true;
}

/*
 * 写入后回读校验。内核会自己归一化（例如 cpuctl 的 "0" 读回是 "0.00"）
 * 或按可用 OPP 夹取，所以先比字符串再比数值，两者都不等才报告。
 * 只记录不回滚，避免把内核的正常改写当成失败处理。
 */
static void verify_written_value(const char* path, const char* expected) {
    if (!path || !expected || !expected[0]) return;

    FILE* fp = fopen(path, "r");
    if (!fp) return;

    char raw[256] = {0};
    if (!fgets(raw, sizeof(raw), fp)) {
        fclose(fp);
        return;
    }
    fclose(fp);

    char actual[256];
    normalize_config_value(raw, actual, sizeof(actual));
    if (strcmp(actual, expected) == 0) return;

    double expected_number = 0.0;
    double actual_number = 0.0;
    if (parse_leading_double(expected, &expected_number) &&
        parse_leading_double(actual, &actual_number)) {
        double diff = expected_number - actual_number;
        if (diff < 0) diff = -diff;
        double scale = expected_number < 0 ? -expected_number : expected_number;
        if (diff <= (scale > 1.0 ? scale * 0.000001 : 0.000001)) {
            return;
        }
    }

    WriteFailureRecord* record = get_write_failure_record(path);
    long long now_ms = get_current_time_ms();
    if (record) {
        record->verify_mismatch_count++;
        if (record->last_verify_log_ms != 0 &&
            now_ms - record->last_verify_log_ms < WRITE_VERIFY_LOG_INTERVAL_MS) {
            return;
        }
        record->last_verify_log_ms = now_ms;
    }

    char log_msg[384];
    safe_snprintf(log_msg, sizeof(log_msg),
        "写入未生效(被内核改写): %s -> 期望 [%s] 实际 [%s]",
        path,
        expected,
        actual[0] ? actual : "<empty>");
    /*
     * 这是「平台和我们抢写同一个频率节点」的诊断，属于调频细节，
     * 走统计日志（stats_log.txt），不要污染主日志。
     */
    log_stats_message(log_msg);
}

int lock_val(const char* path, const char* value) {
    if (!path || !path[0]) return 0;

    char normalized[256];
    normalize_config_value(value, normalized, sizeof(normalized));
    if (normalized[0] == '\0') {
        /*
         * 配置里写空值表示"这一项不动"（例如某些档位下的 pl）。
         * 继续写会变成 0 字节写入，部分节点会回 EINVAL，因此直接按成功跳过。
         */
        return 1;
    }

    const bool real_file =
        (strncmp(path, "/sys/", 5) != 0 && strncmp(path, "/proc/", 6) != 0);

    FILE* fp = fopen(path, "w");

    mode_t old_mode = 0;
    bool permission_changed = false;
    if (!fp && real_file) {
        /* 只对真实文件尝试临时授权；对 sysfs 调 chmod() 永远失败，只会刷日志。 */
        struct stat st;
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
            old_mode = st.st_mode & 07777;
            if ((old_mode & S_IWUSR) == 0 && chmod(path, old_mode | S_IWUSR) == 0) {
                permission_changed = true;
                fp = fopen(path, "w");
            }
        }
    }

    if (!fp) {
        int err = errno;
        log_write_failure_throttled(get_write_failure_record(path), path, normalized, "open", err);
        return 0;
    }

    int write_error = 0;
    /* 带换行，和 shell 的 echo 语义保持一致。 */
    if (fprintf(fp, "%s\n", normalized) < 0) {
        write_error = errno;
    }
    if (fclose(fp) != 0 && write_error == 0) {
        write_error = errno;
    }

    if (permission_changed) {
        chmod(path, old_mode);
    }

    if (write_error != 0) {
        log_write_failure_throttled(get_write_failure_record(path), path, normalized, "write", write_error);
        return 0;
    }

    verify_written_value(path, normalized);
    return 1;
}
