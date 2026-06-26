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

void set_policy_file_permissions() {
    for (int i = 0; i < policy_count; i++) {
        char path[256];
        safe_snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpufreq/policy%d", cpu_policies[i].policy_num);

        if (chmod(path, 0777) != 0) {
            char log_msg[256];
            safe_snprintf(log_msg, sizeof(log_msg), "修改目录权限失败: %s (错误: %s)", path, strerror(errno));
            log_message(log_msg);
        }

        const char* files[] = {
            "scaling_governor",
            "scaling_max_freq",
            "scaling_min_freq",
            "scaling_available_governors",
            NULL
        };

        for (int j = 0; files[j]; j++) {
            char file_path[256];
            safe_snprintf(file_path, sizeof(file_path), "%s/%s", path, files[j]);

            if (chmod(file_path, 0666) != 0) {
                char log_msg[256];
                safe_snprintf(log_msg, sizeof(log_msg), "修改文件权限失败: %s (错误: %s)", file_path, strerror(errno));
                log_message(log_msg);
            }
        }
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

#define MAX_BLACKLIST 128
static char g_write_blacklist[MAX_BLACKLIST][256];
static int g_blacklist_count = 0;

int lock_val(const char* path, const char* value) {
    for (int i = 0; i < g_blacklist_count; i++) {
        if (strcmp(g_write_blacklist[i], path) == 0) {
            return 0;
        }
    }

    FILE* fp = fopen(path, "w");
    if (fp) {
        int result = fprintf(fp, "%s", value);
        int close_ret = fclose(fp);

        if (result < 0 || close_ret != 0) {
            int err = errno;
            char log_msg[256];
            safe_snprintf(log_msg, sizeof(log_msg), "写入失败或关闭失败: %s -> %s (错误: %s)",
                path, value, strerror(err));
            log_message(log_msg);
            if (err == EACCES || err == EPERM || err == EROFS || err == ENOENT) {
                if (g_blacklist_count < MAX_BLACKLIST) {
                    safe_strncpy(g_write_blacklist[g_blacklist_count], path, 256);
                    g_blacklist_count++;
                    safe_snprintf(log_msg, sizeof(log_msg), "文件写入失败(权限/不存在)，已加入黑名单: %s", path);
                    log_message(log_msg);
                }
            }
            return 0;
        }
        return 1;
    }

    struct stat st;
    bool chmod_needed = false;
    mode_t old_mode = 0;

    if (stat(path, &st) == 0) {
        if ((st.st_mode & S_IWUSR) == 0) {
            old_mode = st.st_mode;
            if (chmod(path, st.st_mode | S_IWUSR) == 0) {
                chmod_needed = true;
            } else if (chmod(path, 0644) == 0) {
                chmod_needed = true;
                old_mode = st.st_mode;
            }
        }
    } else {
        if (chmod(path, 0644) == 0) {
            chmod_needed = true;
            old_mode = 0444;
        }
    }

    if (chmod_needed) {
        fp = fopen(path, "w");
        if (fp) {
            int write_res = fprintf(fp, "%s", value);
            int close_res = fclose(fp);
            if (write_res < 0 || close_res != 0) {
                int err = errno;
                char log_msg[256];
                safe_snprintf(log_msg, sizeof(log_msg), "再次写入或关闭失败: %s -> %s (错误: %s)",
                    path, value, strerror(err));
                log_message(log_msg);
                if (err == EACCES || err == EPERM || err == EROFS || err == ENOENT) {
                    if (g_blacklist_count < MAX_BLACKLIST) {
                        safe_strncpy(g_write_blacklist[g_blacklist_count], path, 256);
                        g_blacklist_count++;
                        safe_snprintf(log_msg, sizeof(log_msg), "文件写入失败(权限/不存在)，已加入黑名单: %s", path);
                        log_message(log_msg);
                    }
                }
            } else {
                chmod(path, old_mode);
                return 1;
            }
        } else {
            char log_msg[256];
            safe_snprintf(log_msg, sizeof(log_msg), "再次打开失败: %s (错误: %s)",
                path, strerror(errno));
            log_message(log_msg);
        }
        chmod(path, old_mode);
    } else {
        char log_msg[256];
        safe_snprintf(log_msg, sizeof(log_msg), "修改权限失败或文件不可访问: %s (错误: %s)",
            path, strerror(errno));
        log_message(log_msg);
    }

    return 0;
}
