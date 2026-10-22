/*
 * 官方组件状态检测（6.10）
 *
 * 用途：定期检查系统里"官方能力"当前处于什么状态，写到 config/official_state.txt，
 * 并在日志里给一行摘要。重点：
 *   - pm list packages -d：被禁用的官方/OPLUS/游戏增强相关包；
 *   - OIFace（init.svc.oiface + persist.sys.oiface.*）；
 *   - game_opt 内核接口（/proc/game_opt、oplus_bsp_game_opt 等）；
 *   - com.oplus.cosa 官方调度是否可用；
 *   - Magisk/KernelSU 模块里带 disable 标记的模块。
 * 只读检测，不改任何系统状态；5 分钟一次。
 */
static long long g_official_state_last_ms = 0;

static std::string official_state_trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) {
        s.pop_back();
    }
    return s;
}

static std::string official_state_read_prop(const char* name) {
    char cmd[256];
    safe_snprintf(cmd, sizeof(cmd), "getprop %s 2>/dev/null", name);
    FILE* fp = popen(cmd, "r");
    if (!fp) return std::string();
    char line[256] = {0};
    std::string result;
    if (fgets(line, sizeof(line), fp)) result = official_state_trim(line);
    pclose(fp);
    return result;
}

static std::string official_state_read_file_line(const char* path) {
    FILE* fp = fopen(path, "r");
    if (!fp) return std::string();
    char line[256] = {0};
    std::string result;
    if (fgets(line, sizeof(line), fp)) result = official_state_trim(line);
    fclose(fp);
    return result;
}

static void refresh_official_state(bool force) {
    long long now = get_current_time_ms();
    if (!force && now - g_official_state_last_ms < 300000) return;
    g_official_state_last_ms = now;

    std::string disabled;
    int disabled_count = 0;
    FILE* fp = popen("pm list packages -d 2>/dev/null", "r");
    if (fp) {
        char line[512];
        while (fgets(line, sizeof(line), fp)) {
            if (strncmp(line, "package:", 8) != 0) continue;
            std::string pkg = official_state_trim(std::string(line + 8));
            if (pkg.empty()) continue;
            disabled_count++;
            if (pkg.find("oplus") != std::string::npos ||
                pkg.find("coloros") != std::string::npos ||
                pkg.find("game") != std::string::npos ||
                pkg.find("enhance") != std::string::npos ||
                pkg.find("oiface") != std::string::npos ||
                pkg.find("customize") != std::string::npos ||
                pkg.find("cosa") != std::string::npos) {
                if (!disabled.empty()) disabled += ",";
                disabled += pkg;
            }
        }
        pclose(fp);
    }

    std::string oiface_enable = official_state_read_prop("persist.sys.oiface.enable");
    std::string oiface_control = official_state_read_prop("persist.sys.oiface.control");
    std::string oiface_service = official_state_read_prop("init.svc.oiface");

    bool game_opt_present = (access("/proc/game_opt", F_OK) == 0);
    std::string game_opt_disable_limit = game_opt_present
        ? official_state_read_file_line("/proc/game_opt/disable_cpufreq_limit")
        : "";
    std::string game_opt_debug = game_opt_present
        ? official_state_read_file_line("/proc/game_opt/debug_enable")
        : "";

    std::string cosa_state = "unknown";
    {
        FILE* check = popen("pm list packages -e com.oplus.cosa 2>/dev/null", "r");
        if (check) {
            char line[256] = {0};
            bool found = false;
            while (fgets(line, sizeof(line), check)) {
                if (strstr(line, "com.oplus.cosa")) { found = true; break; }
            }
            pclose(check);
            cosa_state = found ? "enabled" : "disabled";
        }
    }

    std::string disabled_modules;
    int disabled_module_count = 0;
    DIR* dir = opendir("/data/adb/modules");
    if (dir) {
        struct dirent* entry = NULL;
        while ((entry = readdir(dir)) != NULL) {
            if (entry->d_name[0] == '.') continue;
            char flag_path[256];
            safe_snprintf(flag_path, sizeof(flag_path), "/data/adb/modules/%s/disable", entry->d_name);
            if (access(flag_path, F_OK) == 0) {
                disabled_module_count++;
                if (!disabled_modules.empty()) disabled_modules += ",";
                disabled_modules += entry->d_name;
            }
        }
        closedir(dir);
    }

    char path[256];
    safe_snprintf(path, sizeof(path), "%s/official_state.txt", CONFIG_DIR);
    FILE* out = fopen(path, "w");
    if (out) {
        fprintf(out, "# 官方组件状态（模块只读检测，%lld）\n", now);
        fprintf(out, "disabled_packages_count=%d\n", disabled_count);
        fprintf(out, "disabled_official_related=%s\n", disabled.empty() ? "(none)" : disabled.c_str());
        fprintf(out, "oiface_enable=%s\n", oiface_enable.empty() ? "(unknown)" : oiface_enable.c_str());
        fprintf(out, "oiface_control=%s\n", oiface_control.empty() ? "(unknown)" : oiface_control.c_str());
        fprintf(out, "oiface_service=%s\n", oiface_service.empty() ? "(unknown)" : oiface_service.c_str());
        fprintf(out, "game_opt=%s\n", game_opt_present ? "present" : "absent");
        fprintf(out, "game_opt_disable_cpufreq_limit=%s\n",
                game_opt_disable_limit.empty() ? "(unreadable)" : game_opt_disable_limit.c_str());
        fprintf(out, "game_opt_debug_enable=%s\n",
                game_opt_debug.empty() ? "(unreadable)" : game_opt_debug.c_str());
        fprintf(out, "oplus_cosa=%s\n", cosa_state.c_str());
        fprintf(out, "disabled_modules_count=%d\n", disabled_module_count);
        fprintf(out, "disabled_modules=%s\n",
                disabled_modules.empty() ? "(none)" : disabled_modules.c_str());
        fclose(out);
    }

    char msg[512];
    safe_snprintf(msg, sizeof(msg),
                  "官方状态 | 禁用包:%d(%s) | OIFace enable=%s control=%s svc=%s | game_opt=%s | cosa=%s | 禁用模块:%d(%s)",
                  disabled_count,
                  disabled.empty() ? "无相关" : disabled.c_str(),
                  oiface_enable.empty() ? "?" : oiface_enable.c_str(),
                  oiface_control.empty() ? "?" : oiface_control.c_str(),
                  oiface_service.empty() ? "?" : oiface_service.c_str(),
                  game_opt_present ? "present" : "absent",
                  cosa_state.c_str(),
                  disabled_module_count,
                  disabled_modules.empty() ? "无" : disabled_modules.c_str());
    log_config_message(msg);
}
