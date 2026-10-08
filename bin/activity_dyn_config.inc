#ifndef MURONG_DYN_CONFIG_INC
#define MURONG_DYN_CONFIG_INC

/*
 * 动态调频的独立配置：bin/cpu/dyn.json
 *
 * 按 FAS 理念重构后，dyn.json 只剩"谁管走廊 + 走廊在哪"：
 *
 * {
 *   "enabled": 1,                 // 动态调频总开关（关掉=只按档位静态写入）
 *   "corridor_owner": "dyn",      // 谁有权改 scaling_min/max：dyn | fas | mode
 *   "corridor": { "min": ..., "max": ... },
 *   "modes": { "balance": { "enabled": 1, "corridor": {...} } }
 * }
 *
 * 负载控制律（target_load / load_margin / target_loads 分段控顶）已经删除：
 * 频率由 FAS 的帧债动作档位决定，档位地板来自 cpu.json 的档位配置，
 * 甜点频率来自 fas.json 的 sweetspot_freq。
 * 日志开关不在这里：它在 config/log.conf。
 */

#define DYN_CONFIG_FILE CPU_CONFIG_DIR "dyn.json"

enum CorridorOwner {
    CORRIDOR_OWNER_DYN = 0,   /* 动态调频自己按负载改走廊（默认） */
    CORRIDOR_OWNER_FAS = 1,   /* FAS 帧紧迫度主导走廊，动态调频不抢 */
    CORRIDOR_OWNER_MODE = 2   /* 只按档位静态写入，运行时不改 */
};

static int g_corridor_owner = CORRIDOR_OWNER_DYN;
/*
 * 频率边界（每簇一份，kHz）：动态调频/执行器能碰到的范围。
 * 0 = 该簇不设边界，直接跟随档位配置（cpu.json 当前模式的 min/max）。
 * 以前是一对全局值，表达不了分簇差异，也和档位配置重复。
 */
static int g_dyn_corridor_min_khz[MAX_CLUSTERS] = {0};
static int g_dyn_corridor_max_khz[MAX_CLUSTERS] = {0};

/*
 * 动作表参数（dyn.json 的 control）：动态调频在运行时真正调的那几个旋钮。
 * 默认值 = 之前写死在 fas_engine 里的常量，配置不写就维持原行为。
 */
static const FasLadderConfig kDynControlDefaults = {30, 100, 300, 2, 250, 60};
static FasLadderConfig g_dyn_control = {30, 100, 300, 2, 250, 60};

/*
 * 日用（没有帧信号时）的动态调频：按簇负载查一张分段表，输出"地板 + 上限"这个区间。
 *
 * 为什么是区间而不是只有上限：
 *   上限决定 governor 最高能跑到哪（省电/控温），地板决定它不用从最低频慢慢爬（跟手）。
 *   低负载时上限也留余量（不是锁死），突发在区间内由 governor 自己吃，不等下一拍。
 *
 * 分段表是"负载 → 区间"的点，段间线性插值：
 *   stages: [ {load:40, floor:0, ceiling:250}, {load:70, floor:100, ceiling:500}, ... ]
 *   floor/ceiling 都是相对 [档位下限, 档位上限] 的千分比。
 *
 * 其余语义：
 *   hysteresis     负载落在段点附近这个宽度内时保持上一段，避免在边界来回跳
 *   soften_permille 上限软化系数（1000 = 不软化）
 *   load_mode      0=簇内平均 1=前二核平均 2=峰值
 *   rise/fall_hold_ms  放宽（升）立即生效，收紧（降）至少隔这么久
 */
typedef struct {
    int load_upper;        /* 这一段对应的负载（%） */
    int floor_permille;
    int ceiling_permille;
} DynDailyStage;

typedef struct {
    bool enabled;
    int hysteresis;
    int soften_permille;
    int load_mode;
    int rise_hold_ms;
    int fall_hold_ms;
    std::vector<DynDailyStage> stages;
} DynDailyParams;

static DynDailyParams make_default_daily_params() {
    DynDailyParams p = {};
    p.enabled = true;
    p.hysteresis = 5;
    p.soften_permille = 1000;
    p.load_mode = 1;      /* 前二核平均：既不掩盖单核瓶颈，也不被单核抖动带跑 */
    p.rise_hold_ms = 0;
    p.fall_hold_ms = 300;
    p.stages = {
        {40, 0, 250},
        {70, 100, 500},
        {88, 300, 800},
        {100, 550, 1000}
    };
    return p;
}

static DynDailyParams g_dyn_daily = make_default_daily_params();
static DynDailyParams g_cluster_dyn_daily[MAX_CLUSTERS];

static int dyn_json_int(cJSON* obj, const char* key, int fallback) {
    cJSON* item = cJSON_GetObjectItem(obj, key);
    if (item && cJSON_IsNumber(item)) return item->valueint;
    return fallback;
}

/* 解析 daily 块；stages 一旦给出就整表替换（不做新旧混合）。 */
static void apply_dyn_daily_json(cJSON* obj, DynDailyParams* out) {
    if (!obj || !out || !cJSON_IsObject(obj)) return;
    cJSON* daily = cJSON_GetObjectItem(obj, "daily");
    if (!daily || !cJSON_IsObject(daily)) return;

    cJSON* enabled = cJSON_GetObjectItem(daily, "enabled");
    if (enabled && (cJSON_IsBool(enabled) || cJSON_IsNumber(enabled))) {
        out->enabled = enabled->valueint != 0;
    }
    int v;
    v = dyn_json_int(daily, "hysteresis", -1);
    if (v >= 0 && v <= 50) out->hysteresis = v;
    v = dyn_json_int(daily, "soften_permille", -1);
    if (v >= 100 && v <= 1000) out->soften_permille = v;
    v = dyn_json_int(daily, "rise_hold_ms", -1);
    if (v >= 0 && v <= 2000) out->rise_hold_ms = v;
    v = dyn_json_int(daily, "fall_hold_ms", -1);
    if (v >= 0 && v <= 5000) out->fall_hold_ms = v;

    cJSON* load_mode = cJSON_GetObjectItem(daily, "load_mode");
    if (load_mode && cJSON_IsString(load_mode) && load_mode->valuestring) {
        std::string lowered = scheduler_to_lower_copy(load_mode->valuestring);
        if (lowered == "peak") out->load_mode = 2;
        else if (lowered == "avg" || lowered == "average") out->load_mode = 0;
        else if (lowered == "top2") out->load_mode = 1;
    } else if (load_mode && cJSON_IsNumber(load_mode)) {
        if (load_mode->valueint >= 0 && load_mode->valueint <= 2) out->load_mode = load_mode->valueint;
    }

    cJSON* stages = cJSON_GetObjectItem(daily, "stages");
    if (stages && cJSON_IsArray(stages)) {
        std::vector<DynDailyStage> parsed;
        cJSON* item = NULL;
        cJSON_ArrayForEach(item, stages) {
            if (!cJSON_IsObject(item)) continue;
            int load_upper = dyn_json_int(item, "load", -1);
            int floor_permille = dyn_json_int(item, "floor", -1);
            int ceiling_permille = dyn_json_int(item, "ceiling", -1);
            if (load_upper < 0 || load_upper > 100) continue;
            if (floor_permille < 0 || floor_permille > 1000) continue;
            if (ceiling_permille < 0 || ceiling_permille > 1000) continue;
            if (ceiling_permille < floor_permille) continue;
            parsed.push_back({load_upper, floor_permille, ceiling_permille});
        }
        if (!parsed.empty()) {
            std::sort(parsed.begin(), parsed.end(),
                      [](const DynDailyStage& a, const DynDailyStage& b) {
                          return a.load_upper < b.load_upper;
                      });
            out->stages = parsed;
        }
    }
}

/* control 块可以放在顶层、档位覆盖里，也接受直接平铺这些键。 */
static void apply_dyn_control_json(cJSON* obj, FasLadderConfig* out) {
    if (!obj || !out || !cJSON_IsObject(obj)) return;
    cJSON* control = cJSON_GetObjectItem(obj, "control");
    if (!control || !cJSON_IsObject(control)) control = obj;

    int v;
    v = dyn_json_int(control, "deadband_permille", -1);
    if (v >= 0 && v <= 500) out->deadband_permille = v;
    v = dyn_json_int(control, "sweetspot_permille", -1);
    if (v > 0 && v <= 2000) out->sweetspot_permille = v;
    v = dyn_json_int(control, "ceiling_permille", -1);
    if (v > 0 && v <= 5000) out->ceiling_permille = v;
    v = dyn_json_int(control, "severe_streak", -1);
    if (v >= 1 && v <= 30) out->severe_streak = v;
    v = dyn_json_int(control, "release_step_permille", -1);
    if (v >= 20 && v <= 1000) out->release_step_permille = v;
    v = dyn_json_int(control, "release_hold_ms", -1);
    if (v >= 10 && v <= 1000) out->release_hold_ms = v;
}

static const char* corridor_owner_name(int owner) {
    switch (owner) {
        case CORRIDOR_OWNER_FAS: return "fas";
        case CORRIDOR_OWNER_MODE: return "mode";
        default: return "dyn";
    }
}

/* 解析一份动态调频开关对象（dyn.json 顶层、modes.<mode> 或 clusters 里的某一簇）。 */
static void apply_dyn_params_json(DynamicTuningParams* dst, cJSON* obj) {
    if (!dst || !obj || !cJSON_IsObject(obj)) return;

    cJSON* enabled = cJSON_GetObjectItem(obj, "enabled");
    if (enabled && (cJSON_IsBool(enabled) || cJSON_IsNumber(enabled))) {
        dst->enabled = enabled->valueint != 0;
    }
}

static void reset_dyn_params_to_defaults(DynamicTuningParams* dst) {
    if (!dst) return;
    dst->enabled = true;
}

/*
 * corridor 支持两种写法，可以混用：
 *   "corridor": { "min": 787200, "max": 2899200 }           → 所有簇的默认边界
 *   "corridor": { "0": { "min": ..., "max": ... }, "1": … }  → 每簇覆盖
 * 顶层 corridor_min / corridor_max 与上面等价。
 */
static void apply_dyn_corridor_values_json(cJSON* obj, int* io_min, int* io_max) {
    if (!obj || !cJSON_IsObject(obj) || !io_min || !io_max) return;
    cJSON* min_item = cJSON_GetObjectItem(obj, "corridor_min");
    cJSON* max_item = cJSON_GetObjectItem(obj, "corridor_max");
    if (!min_item) min_item = cJSON_GetObjectItem(obj, "min");
    if (!max_item) max_item = cJSON_GetObjectItem(obj, "max");
    if (min_item && cJSON_IsNumber(min_item) && min_item->valueint > 0) {
        *io_min = min_item->valueint;
    }
    if (max_item && cJSON_IsNumber(max_item) && max_item->valueint > 0) {
        *io_max = max_item->valueint;
    }
}

static void apply_dyn_corridor_json(cJSON* obj, bool reset_first) {
    if (!obj || !cJSON_IsObject(obj)) return;
    if (reset_first) {
        for (int i = 0; i < MAX_CLUSTERS; i++) {
            g_dyn_corridor_min_khz[i] = 0;
            g_dyn_corridor_max_khz[i] = 0;
        }
    }

    /* 先当默认值（顶层 min/max 或顶层 corridor_min/corridor_max） */
    int default_min = g_dyn_corridor_min_khz[0];
    int default_max = g_dyn_corridor_max_khz[0];
    apply_dyn_corridor_values_json(obj, &default_min, &default_max);
    cJSON* corridor = cJSON_GetObjectItem(obj, "corridor");
    if (corridor && cJSON_IsObject(corridor)) {
        apply_dyn_corridor_values_json(corridor, &default_min, &default_max);
    }

    /* 再按簇覆盖（corridor["0"] / corridor["1"]） */
    bool has_per_cluster = false;
    if (corridor && cJSON_IsObject(corridor)) {
        cJSON* item = NULL;
        cJSON_ArrayForEach(item, corridor) {
            if (!item->string || !cJSON_IsObject(item)) continue;
            int idx = atoi(item->string);
            if (idx < 0 || idx >= cluster_count || idx >= MAX_CLUSTERS) continue;
            int mn = default_min, mx = default_max;
            apply_dyn_corridor_values_json(item, &mn, &mx);
            g_dyn_corridor_min_khz[idx] = mn;
            g_dyn_corridor_max_khz[idx] = mx;
            has_per_cluster = true;
        }
    }

    if (!has_per_cluster) {
        for (int i = 0; i < cluster_count && i < MAX_CLUSTERS; i++) {
            g_dyn_corridor_min_khz[i] = default_min;
            g_dyn_corridor_max_khz[i] = default_max;
        }
    }
}

/*
 * 载入 bin/cpu/dyn.json。缺文件时用默认值并把 target_loads 交给 PELT 默认，
 * 不再回落到 cpu.json（那份已经删除动态调频字段，不留兼容）。
 */
static void load_dyn_config_json(const char* mode) {
    reset_dyn_params_to_defaults(&g_dyn_params);
    g_corridor_owner = CORRIDOR_OWNER_DYN;
    g_dyn_control = kDynControlDefaults;
    g_dyn_daily = make_default_daily_params();
    for (int i = 0; i < MAX_CLUSTERS; i++) {
        g_dyn_corridor_min_khz[i] = 0;
        g_dyn_corridor_max_khz[i] = 0;
        g_cluster_dyn_daily[i] = g_dyn_daily;
    }

    FILE* fp = fopen(DYN_CONFIG_FILE, "rb");
    if (!fp) {
        log_config_message("dyn.json 不存在，动态调频使用内置默认值");
    } else {
        fseek(fp, 0, SEEK_END);
        long len = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        if (len <= 0 || len > 65536) {
            fclose(fp);
            log_config_message("dyn.json 大小异常，动态调频使用内置默认值");
        } else {
            char* data = (char*)malloc((size_t)len + 1);
            if (!data) {
                fclose(fp);
            } else {
                size_t read_size = fread(data, 1, (size_t)len, fp);
                data[read_size] = '\0';
                fclose(fp);

                cJSON* root = cJSON_Parse(data);
                free(data);
                if (!root) {
                    log_config_message("dyn.json 解析失败，动态调频使用内置默认值");
                } else {
                    apply_dyn_params_json(&g_dyn_params, root);
                    apply_dyn_corridor_json(root, false);
                    apply_dyn_control_json(root, &g_dyn_control);
                    apply_dyn_daily_json(root, &g_dyn_daily);

                    /* 档位覆盖：modes.<mode> 是同一份 dyn.json 里的按档位微调，
                       取代原来散落在 cpu.json 每个 mode 里的 dynamic_tuning。 */
                    cJSON* mode_overlay = NULL;
                    cJSON* modes = cJSON_GetObjectItem(root, "modes");
                    if (modes && cJSON_IsObject(modes) && mode && mode[0]) {
                        mode_overlay = cJSON_GetObjectItem(modes, mode);
                        if (mode_overlay && cJSON_IsObject(mode_overlay)) {
                            apply_dyn_params_json(&g_dyn_params, mode_overlay);
                            apply_dyn_control_json(mode_overlay, &g_dyn_control);
                            apply_dyn_daily_json(mode_overlay, &g_dyn_daily);
                            /* 档位可以覆盖走廊上下限（未写则沿用全局） */
                            cJSON* mode_corridor = cJSON_GetObjectItem(mode_overlay, "corridor");
                            cJSON* mode_corridor_min = cJSON_GetObjectItem(mode_overlay, "corridor_min");
                            cJSON* mode_corridor_max = cJSON_GetObjectItem(mode_overlay, "corridor_max");
                            if ((mode_corridor && cJSON_IsObject(mode_corridor)) ||
                                (mode_corridor_min && cJSON_IsNumber(mode_corridor_min)) ||
                                (mode_corridor_max && cJSON_IsNumber(mode_corridor_max))) {
                                /* 档位覆盖是该档位的完整边界（先清空再应用，避免继承别的档位） */
                                apply_dyn_corridor_json(mode_overlay, true);
                            }
                        }
                    }

                    cJSON* owner = cJSON_GetObjectItem(root, "corridor_owner");
                    if (owner && cJSON_IsString(owner) && owner->valuestring) {
                        std::string lowered = scheduler_to_lower_copy(owner->valuestring);
                        if (lowered == "fas") {
                            g_corridor_owner = CORRIDOR_OWNER_FAS;
                        } else if (lowered == "mode") {
                            g_corridor_owner = CORRIDOR_OWNER_MODE;
                        } else {
                            g_corridor_owner = CORRIDOR_OWNER_DYN;
                        }
                    }

                    /* 每簇先继承全局，再用 clusters 里的条目覆盖。 */
                    for (int i = 0; i < cluster_count && i < MAX_CLUSTERS; i++) {
                        g_cluster_dyn_params[i] = g_dyn_params;
                        g_cluster_dyn_daily[i] = g_dyn_daily;
                    }
                    cJSON* clusters = cJSON_GetObjectItem(root, "clusters");
                    if (clusters && cJSON_IsObject(clusters)) {
                        cJSON* item = NULL;
                        cJSON_ArrayForEach(item, clusters) {
                            if (!item->string) continue;
                            int idx = atoi(item->string);
                            if (idx >= 0 && idx < cluster_count && idx < MAX_CLUSTERS) {
                                apply_dyn_params_json(&g_cluster_dyn_params[idx], item);
                                apply_dyn_daily_json(item, &g_cluster_dyn_daily[idx]);
                            }
                        }
                    }
                    /* 档位内的每簇覆盖优先级最高 */
                    if (mode_overlay && cJSON_IsObject(mode_overlay)) {
                        cJSON* mode_clusters = cJSON_GetObjectItem(mode_overlay, "clusters");
                        if (mode_clusters && cJSON_IsObject(mode_clusters)) {
                            cJSON* item = NULL;
                            cJSON_ArrayForEach(item, mode_clusters) {
                                if (!item->string) continue;
                                int idx = atoi(item->string);
                                if (idx >= 0 && idx < cluster_count && idx < MAX_CLUSTERS) {
                                    apply_dyn_params_json(&g_cluster_dyn_params[idx], item);
                                    g_cluster_dyn_daily[idx] = g_dyn_daily;
                                    apply_dyn_daily_json(item, &g_cluster_dyn_daily[idx]);
                                }
                            }
                        }
                    }

                    char log_msg[256];
                    int written = safe_snprintf(
                        log_msg, sizeof(log_msg),
                        "已加载动态调频配置: %s | enabled:%d | 边界所有者:%s | 边界",
                        DYN_CONFIG_FILE,
                        g_dyn_params.enabled ? 1 : 0,
                        corridor_owner_name(g_corridor_owner));
                    for (int i = 0; i < cluster_count && i < MAX_CLUSTERS &&
                         written > 0 && (size_t)written < sizeof(log_msg); i++) {
                        int w = 0;
                        if (g_dyn_corridor_min_khz[i] <= 0 && g_dyn_corridor_max_khz[i] <= 0) {
                            w = safe_snprintf(log_msg + written, sizeof(log_msg) - (size_t)written,
                                              " | c%d:跟随档位", i);
                        } else {
                            w = safe_snprintf(log_msg + written, sizeof(log_msg) - (size_t)written,
                                              " | c%d:%d-%d", i,
                                              g_dyn_corridor_min_khz[i],
                                              g_dyn_corridor_max_khz[i]);
                        }
                        if (w <= 0) break;
                        written += w;
                    }
                    log_config_message(log_msg);

                    char control_msg[224];
                    safe_snprintf(control_msg, sizeof(control_msg),
                                  "动态调频动作表: 死区%d‰ | 甜点%d‰ | 放开上限%d‰ | 顶格连续%d帧 | 回落%d‰/%dms",
                                  g_dyn_control.deadband_permille,
                                  g_dyn_control.sweetspot_permille,
                                  g_dyn_control.ceiling_permille,
                                  g_dyn_control.severe_streak,
                                  g_dyn_control.release_step_permille,
                                  g_dyn_control.release_hold_ms);
                    log_config_message(control_msg);

                    char daily_msg[288];
                    int daily_written = safe_snprintf(
                        daily_msg, sizeof(daily_msg),
                        "日用负载表: 口径%s | 迟滞%d%% | 软化%d‰ | 放宽立即/收紧%dms | 分段",
                        g_dyn_daily.load_mode == 2 ? "峰值" :
                            (g_dyn_daily.load_mode == 1 ? "前二核" : "平均"),
                        g_dyn_daily.hysteresis,
                        g_dyn_daily.soften_permille,
                        g_dyn_daily.fall_hold_ms);
                    for (size_t si = 0; si < g_dyn_daily.stages.size() &&
                         daily_written > 0 && (size_t)daily_written < sizeof(daily_msg); si++) {
                        const DynDailyStage& st = g_dyn_daily.stages[si];
                        int w = safe_snprintf(daily_msg + daily_written,
                                              sizeof(daily_msg) - (size_t)daily_written,
                                              " | ≤%d%%:%d‰-%d‰",
                                              st.load_upper, st.floor_permille, st.ceiling_permille);
                        if (w <= 0) break;
                        daily_written += w;
                    }
                    log_config_message(daily_msg);
                    cJSON_Delete(root);
                    return;
                }
            }
        }
    }

    /* 没有配置或配置不可用：全部簇用默认值。 */
    for (int i = 0; i < cluster_count && i < MAX_CLUSTERS; i++) {
        g_cluster_dyn_params[i] = g_dyn_params;
    }
}

#endif  // MURONG_DYN_CONFIG_INC
