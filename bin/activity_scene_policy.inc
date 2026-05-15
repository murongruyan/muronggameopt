static bool token_looks_like_package(const std::string& token) {
    if (token.size() < 5 || token.find('.') == std::string::npos) return false;
    if (!isalpha((unsigned char)token[0])) return false;
    if (token.front() == '.' || token.back() == '.') return false;
    int dot_count = 0;
    for (size_t i = 0; i < token.size(); i++) {
        char c = token[i];
        if (!(isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-')) {
            return false;
        }
        if (c == '.') dot_count++;
    }
    return dot_count >= 2;
}

static bool string_equals_any(const char* value, const char* const* list, size_t count) {
    if (!value || !value[0] || !list) return false;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(value, list[i]) == 0) return true;
    }
    return false;
}

static bool string_equals_any_vector(const char* value, const std::vector<std::string>& list) {
    if (!value || !value[0]) return false;
    for (size_t i = 0; i < list.size(); i++) {
        if (list[i] == value) return true;
    }
    return false;
}

static const SceneCategoryRule* find_scene_category_rule_by_package(const char* package) {
    if (!is_scene_category_runtime_enabled()) return NULL;
    if (!package || !package[0]) return NULL;
    for (size_t i = 0; i < g_scene_category_rules.size(); i++) {
        const SceneCategoryRule& rule = g_scene_category_rules[i];
        if (string_equals_any_vector(package, rule.packages)) {
            return &rule;
        }
    }
    return NULL;
}

static const char* classify_scene_category_from_config(const char* package,
                                                       const char* process_name,
                                                       const char* activity_name) {
    if (!is_scene_category_runtime_enabled()) return "";
    for (size_t i = 0; i < g_scene_category_rules.size(); i++) {
        const SceneCategoryRule& rule = g_scene_category_rules[i];
        if (process_name && process_name[0] &&
            string_equals_any_vector(process_name, rule.processes)) {
            return rule.category.c_str();
        }
    }
    for (size_t i = 0; i < g_scene_category_rules.size(); i++) {
        const SceneCategoryRule& rule = g_scene_category_rules[i];
        if (activity_name && activity_name[0] &&
            string_equals_any_vector(activity_name, rule.activities)) {
            return rule.category.c_str();
        }
    }
    for (size_t i = 0; i < g_scene_category_rules.size(); i++) {
        const SceneCategoryRule& rule = g_scene_category_rules[i];
        if (package && package[0] &&
            string_equals_any_vector(package, rule.packages)) {
            return rule.category.c_str();
        }
    }
    return "";
}

static const char* classify_scene_category(const char* package,
                                           const char* process_name,
                                           const char* activity_name) {
    const char* configured_category = classify_scene_category_from_config(
        package, process_name, activity_name);
    if (configured_category && configured_category[0]) {
        return configured_category;
    }

    static const char* kLauncherPackages[] = {
        "com.miui.home",
        "com.meizu.flyme.launcher",
        "net.oneplus.launcher",
        "com.oppo.launcher",
        "com.mi.android.globallauncher",
        "com.teslacoilsw.launcher",
        "com.android.quicksearchbox",
        "net.oneplus.h2launcher",
        "com.oneplus.hydrogen.launcher",
        "com.microsoft.launcher",
        "org.lineageos.trebuchet",
        "org.mokee.lawnchair",
        "com.google.android.apps.nexuslauncher",
        "com.android.launcher",
        "com.bbk.launcher2",
        "com.zte.mifavor.launcher"
    };
    static const char* kLauncherActivities[] = {
        "com.tencent.mm.plugin.appbrand.ui.AppBrandLauncherUI",
        "com.tencent.mm.plugin.appbrand.launching.AppBrandLaunchProxyUI"
    };
    static const char* kScannerActivities[] = {
        "com.tencent.mm.plugin.scanner.ui.BaseScanUI",
        "com.tencent.mobileqq.olympic.activity.ScanTorchActivity",
        "com.alipay.mobile.scan.as.main.MainCaptureActivity",
        "com.jd.lib.scan.lib.zxing.client.android.CaptureActivity",
        "com.jd.lib.scan.lib.zxing.client.android.NewCaptureActivity",
        "com.etao.feimagesearch.capture.CaptureActivity",
        "com.bilibili.app.qrcode.QRcodeCaptureActivity",
        "com.oplus.scanner.ui.main.CameraActivity",
        "com.xiaomi.scanner.app.ScanActivity",
        "com.flyme.scanner.CaptureActivity",
        "com.tencent.wework.login.controller.LoginScannerActivity",
        "kt.com.fcbox.hiveconsumer.app.business.scan.ScanActivity"
    };
    static const char* kCameraPackages[] = {
        "com.android.camera",
        "com.xiaomi.scanner",
        "com.oneplus.camera",
        "com.google.android.GoogleCamera",
        "com.oplus.camera",
        "org.codeaurora.snapcam",
        "com.meizu.media.camera"
    };
    static const char* kCameraActivities[] = {
        "com.tencent.mm.plugin.recordvideo.activity.MMRecordUI",
        "com.tencent.aelight.camera.aebase.QIMCameraCaptureActivity",
        "dov.com.qq.im.QIMCameraCaptureActivity"
    };
    static const char* kMiniProgramProcesses[] = {
        "com.tencent.mm:appbrand",
        "com.tencent.mm:appbrand1",
        "com.tencent.mm:toolsmp",
        "com.tencent.mobileqq:mini",
        "com.tencent.mobileqq:mini1"
    };
    static const char* kScrollOptPackages[] = {
        "com.taobao.idlefish",
        "com.taobao.taobao",
        "com.android.browser",
        "com.baidu.tieba_mini",
        "com.baidu.tieba",
        "com.jingdong.app.mall",
        "com.tmall.wireless",
        "com.sankuai.meituan",
        "com.pupumall.customer",
        "com.eg.android.AlipayGphone",
        "com.android.vending",
        "cn.xiaochuankeji.tieba",
        "com.mfw.roadbook",
        "com.heytap.browser",
        "com.tencent.news"
    };
    static const char* kWhiteListPackages[] = {
        "com.miui.huanji",
        "com.miui.backup",
        "com.meizu.datamigration",
        "com.android.providers.downloads.ui",
        "android.process.mediaui",
        "com.coloros.backuprestore",
        "com.rarlab.rar",
        "org.swiftapps.swiftbackup",
        "xzr.konabess",
        "com.oplus.ota",
        "com.android.updater",
        "com.termux",
        "com.termux.x11",
        "vegabobo.dsusideloader"
    };

    if (process_name && process_name[0]) {
        if (strcmp(process_name, "com.tencent.mobileqq:tool") == 0) {
            return "Scanner";
        }
        if (strcmp(process_name, "com.tencent.mobileqq:peak") == 0) {
            return "Camera";
        }
        if (string_equals_any(process_name, kMiniProgramProcesses,
                              sizeof(kMiniProgramProcesses) / sizeof(kMiniProgramProcesses[0]))) {
            return "MiniProgram";
        }
    }
    if (activity_name && activity_name[0]) {
        if (string_equals_any(activity_name, kScannerActivities,
                              sizeof(kScannerActivities) / sizeof(kScannerActivities[0]))) {
            return "Scanner";
        }
        if (string_equals_any(activity_name, kCameraActivities,
                              sizeof(kCameraActivities) / sizeof(kCameraActivities[0]))) {
            return "Camera";
        }
        if (string_equals_any(activity_name, kLauncherActivities,
                              sizeof(kLauncherActivities) / sizeof(kLauncherActivities[0]))) {
            return "Launcher";
        }
    }
    if (package && package[0]) {
        if (string_equals_any(package, kCameraPackages,
                              sizeof(kCameraPackages) / sizeof(kCameraPackages[0]))) {
            return "Camera";
        }
        if (string_equals_any(package, kScrollOptPackages,
                              sizeof(kScrollOptPackages) / sizeof(kScrollOptPackages[0]))) {
            return "ScrollOpt";
        }
        if (string_equals_any(package, kWhiteListPackages,
                              sizeof(kWhiteListPackages) / sizeof(kWhiteListPackages[0]))) {
            return "WhiteList";
        }
        if (string_equals_any(package, kLauncherPackages,
                              sizeof(kLauncherPackages) / sizeof(kLauncherPackages[0])) ||
            strstr(package, "launcher") != NULL) {
            return "Launcher";
        }
    }
    return "";
}

static bool scene_category_equals(const char* category, const char* expected) {
    if (!category || !expected) return false;
    return strcmp(category, expected) == 0;
}

static bool scene_category_is_game_family(const char* category) {
    return scene_category_equals(category, "Game") ||
           scene_category_equals(category, "HonorOfKings") ||
           scene_category_equals(category, "GenshinImpact");
}

static bool scene_category_is_hok(const char* category) {
    return scene_category_equals(category, "HonorOfKings");
}

static bool scene_category_is_genshin(const char* category) {
    return scene_category_equals(category, "GenshinImpact");
}

static bool scene_category_is_generic_game(const char* category) {
    return scene_category_equals(category, "Game");
}

static int get_scene_floor_freq_for_cluster(int policy_idx, int cluster_id, const char* current_app) {
    if (policy_idx < 0 || cluster_id < 0 || cluster_id >= cluster_count) return 0;
    if (g_scene_policy.screen_off) return 0;

    bool hok_scene = scene_category_is_hok(g_scene_policy.foreground_category);
    bool genshin_scene = scene_category_is_genshin(g_scene_policy.foreground_category);
    bool generic_game_scene = scene_category_is_generic_game(g_scene_policy.foreground_category);
    bool scanner_scene = g_scene_policy.scanner_active ||
                         scene_category_equals(g_scene_policy.foreground_category, "Scanner");
    bool camera_scene = g_scene_policy.camera_active ||
                        scene_category_equals(g_scene_policy.foreground_category, "Camera");
    bool launcher_scene = scene_category_equals(g_scene_policy.foreground_category, "Launcher") ||
                          is_launcher_package_name(current_app);
    bool mini_program_scene = g_scene_policy.mini_program_active ||
                              scene_category_equals(g_scene_policy.foreground_category, "MiniProgram");
    bool scroll_opt_scene = scene_category_equals(g_scene_policy.foreground_category, "ScrollOpt");
    bool white_list_scene = scene_category_equals(g_scene_policy.foreground_category, "WhiteList");

    if (camera_scene || scanner_scene) {
        int khz_floor = 998400;
        if (cluster_id == cluster_count - 1) {
            khz_floor = 1766400;
        } else if (cluster_count > 1 && cluster_id == cluster_count - 2) {
            khz_floor = 1324800;
        }
        return get_policy_safe_floor_freq(policy_idx, khz_floor);
    }
    if (hok_scene) {
        int khz_floor = 998400;
        if (cluster_id == cluster_count - 1) {
            khz_floor = 1958400;
        } else if (cluster_count > 1 && cluster_id == cluster_count - 2) {
            khz_floor = 1478400;
        }
        return get_policy_safe_floor_freq(policy_idx, khz_floor);
    }
    if (genshin_scene) {
        int khz_floor = 998400;
        if (cluster_id == cluster_count - 1) {
            khz_floor = 1766400;
        } else if (cluster_count > 1 && cluster_id == cluster_count - 2) {
            khz_floor = 1363200;
        }
        return get_policy_safe_floor_freq(policy_idx, khz_floor);
    }
    if (generic_game_scene) {
        int khz_floor = 844800;
        if (cluster_id == cluster_count - 1) {
            khz_floor = 1497600;
        } else if (cluster_count > 1 && cluster_id == cluster_count - 2) {
            khz_floor = 1171200;
        }
        return get_policy_safe_floor_freq(policy_idx, khz_floor);
    }
    if (launcher_scene) {
        return get_policy_safe_floor_freq(policy_idx, cluster_id == cluster_count - 1 ? 1017600 : 883200);
    }
    if (mini_program_scene) {
        int khz_floor = cluster_id == cluster_count - 1 ? 1324800 : 998400;
        return get_policy_safe_floor_freq(policy_idx, khz_floor);
    }
    if (scroll_opt_scene) {
        int khz_floor = 844800;
        if (cluster_id == cluster_count - 1) {
            khz_floor = 1324800;
        } else if (cluster_count > 1 && cluster_id == cluster_count - 2) {
            khz_floor = 1075200;
        }
        return get_policy_safe_floor_freq(policy_idx, khz_floor);
    }
    if (white_list_scene) {
        int khz_floor = cluster_id == cluster_count - 1 ? 1017600 : 844800;
        return get_policy_safe_floor_freq(policy_idx, khz_floor);
    }
    return 0;
}

std::string get_highest_mode_for_visible_apps() {
    char mode[64];
    get_mode_for_package(g_current_foreground_app, mode, sizeof(mode));
    get_visible_window_scene_state(g_current_foreground_app,
                                   &g_scene_policy.visible_app_count,
                                   &g_scene_policy.multi_window_active);
    safe_strncpy(g_scene_policy.highest_mode, mode, sizeof(g_scene_policy.highest_mode));
    return std::string(mode);
}

bool is_screen_off() {
    static long long last_check_ms = 0;
    static int cached_screen_off = 0;
    long long now_ms = get_current_time_ms();
    if (now_ms - last_check_ms < 2000) {
        return cached_screen_off != 0;
    }
    last_check_ms = now_ms;
    cached_screen_off = 0;

    FILE* fp = popen("dumpsys power 2>/dev/null", "r");
    if (fp) {
        char line[512];
        while (fgets(line, sizeof(line), fp)) {
            if (strstr(line, "mScreenOn=")) {
                cached_screen_off = strstr(line, "false") != NULL ? 1 : 0;
                break;
            }
            if (strstr(line, "Display Power: state=")) {
                cached_screen_off = strstr(line, "OFF") != NULL ? 1 : 0;
            }
        }
        pclose(fp);
    }
    return cached_screen_off != 0;
}

static void add_unique_scene_package(std::vector<std::string>& packages, const std::string& pkg) {
    std::string value = trim_copy(pkg);
    if (value.empty()) return;
    trim_process_suffix(&value[0]);
    if (value.empty() || !token_looks_like_package(value) || should_ignore_foreground_package(value.c_str())) {
        return;
    }
    for (size_t i = 0; i < packages.size(); i++) {
        if (packages[i] == value) return;
    }
    packages.push_back(value);
}

static void extract_scene_packages_from_line(const std::string& line, std::vector<std::string>& packages) {
    std::string token;
    auto flush_token = [&]() {
        if (token.empty()) return;
        std::string candidate = token;
        token.clear();

        while (!candidate.empty() && (candidate.back() == ',' || candidate.back() == ';' ||
               candidate.back() == '}' || candidate.back() == ']' || candidate.back() == ')')) {
            candidate.pop_back();
        }
        while (!candidate.empty() && (candidate.front() == '{' || candidate.front() == '[' ||
               candidate.front() == '(' || candidate.front() == ',')) {
            candidate.erase(candidate.begin());
        }

        size_t eq = candidate.find('=');
        if (eq != std::string::npos) {
            candidate = candidate.substr(eq + 1);
        }
        size_t slash = candidate.find('/');
        if (slash != std::string::npos) {
            candidate = candidate.substr(0, slash);
        }
        add_unique_scene_package(packages, candidate);
    };

    for (size_t i = 0; i < line.size(); i++) {
        char c = line[i];
        bool keep = isalnum((unsigned char)c) || c == '.' || c == '_' || c == ':' || c == '/' || c == '=';
        if (keep) {
            token.push_back(c);
        } else {
            flush_token();
        }
    }
    flush_token();
}

static void get_visible_window_scene_state(const char* current_app, int* visible_count, int* multi_window_active) {
    static long long last_check_ms = 0;
    static int cached_visible_count = 1;
    static int cached_multi_window = 0;
    long long now_ms = get_current_time_ms();
    if (now_ms - last_check_ms < 2000) {
        if (visible_count) *visible_count = cached_visible_count;
        if (multi_window_active) *multi_window_active = cached_multi_window;
        return;
    }
    last_check_ms = now_ms;

    std::vector<std::string> packages;
    if (current_app && current_app[0] && strcmp(current_app, "unknown") != 0) {
        add_unique_scene_package(packages, current_app);
    }

    std::string out;
    if (execute_command_all("dumpsys activity activities 2>/dev/null", out)) {
        bool split_mode_hint = false;
        size_t start = 0;
        while (start < out.size()) {
            size_t end = out.find('\n', start);
            if (end == std::string::npos) end = out.size();
            std::string line = out.substr(start, end - start);
            std::string lower = line;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)tolower(c); });
            if (lower.find("split-screen") != std::string::npos ||
                lower.find("windowingmode=split") != std::string::npos ||
                lower.find("windowingmode=multi-window") != std::string::npos) {
                split_mode_hint = true;
            }
            if (lower.find("topresumedactivity") != std::string::npos ||
                lower.find("resumedactivity") != std::string::npos ||
                lower.find("mvisiblerequested=true") != std::string::npos ||
                lower.find("visible=true") != std::string::npos) {
                extract_scene_packages_from_line(line, packages);
            }
            start = end + 1;
        }
        cached_visible_count = packages.empty() ? ((current_app && current_app[0] &&
            strcmp(current_app, "unknown") != 0) ? 1 : 0) : (int)packages.size();
        cached_multi_window = (cached_visible_count > 1 || split_mode_hint) ? 1 : 0;
    } else {
        cached_visible_count = (current_app && current_app[0] && strcmp(current_app, "unknown") != 0) ? 1 : 0;
        cached_multi_window = 0;
    }

    if (visible_count) *visible_count = cached_visible_count;
    if (multi_window_active) *multi_window_active = cached_multi_window;
}

static bool is_game_package_name(const char* package) {
    if (!package || !package[0]) return false;
    const SceneCategoryRule* scene_rule = find_scene_category_rule_by_package(package);
    if (scene_rule && scene_rule->game) return true;
    if (strcmp(package, g_current_foreground_app) == 0 && g_scene_policy.foreground_category[0]) {
        if (scene_category_is_game_family(g_scene_policy.foreground_category)) {
            return true;
        }
    }
    char mode[64];
    get_mode_for_package(package, mode, sizeof(mode));
    return get_mode_rank(mode) >= get_mode_rank("performance");
}

bool is_background_music_playing() {
    static long long last_check_ms = 0;
    static int cached_playing = 0;
    long long now_ms = get_current_time_ms();
    if (now_ms - last_check_ms < 3000) {
        return cached_playing != 0;
    }
    last_check_ms = now_ms;
    cached_playing = 0;

    std::string out;
    if (!execute_command_all("dumpsys media_session 2>/dev/null", out)) {
        return false;
    }

    std::string lower = out;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)tolower(c); });
    bool has_playing_session = lower.find("playbackstate {state=3") != std::string::npos ||
                               lower.find("state=playing") != std::string::npos ||
                               lower.find(" state=3,") != std::string::npos;
    if (!has_playing_session) {
        return false;
    }

    if (g_current_foreground_app[0] && strcmp(g_current_foreground_app, "unknown") != 0) {
        std::string current_pkg = g_current_foreground_app;
        std::transform(current_pkg.begin(), current_pkg.end(), current_pkg.begin(),
                       [](unsigned char c) { return (char)tolower(c); });
        size_t pkg_hits = 0;
        size_t pos = 0;
        while ((pos = lower.find(current_pkg, pos)) != std::string::npos) {
            pkg_hits++;
            pos += current_pkg.size();
        }
        if (pkg_hits > 0 && lower.find("com.spotify") == std::string::npos &&
            lower.find("netease.cloudmusic") == std::string::npos &&
            lower.find("qqmusic") == std::string::npos &&
            lower.find("music") == std::string::npos) {
            return false;
        }
    }

    cached_playing = 1;
    return true;
}

bool is_camera_active() {
    // 废弃 dumpsys media.camera 检查，避免输出包含 active clients 导致的严重误判
    // 同时也节省 dumpsys 带来的系统开销
    return false;
}

bool is_game_loading() {
    return false;
}
