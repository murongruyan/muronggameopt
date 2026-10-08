static void normalize_config_cpu_list(std::vector<int>& cpus) {
    cpus.erase(std::remove_if(cpus.begin(), cpus.end(), [](int cpu) {
        return cpu < 0 || cpu >= CPU_SETSIZE;
    }), cpus.end());
    std::sort(cpus.begin(), cpus.end());
    cpus.erase(std::unique(cpus.begin(), cpus.end()), cpus.end());
}

static void parse_cpu_list_string(const char* text, std::vector<int>& out) {
    out.clear();
    if (!text || !text[0]) return;
    std::string value = trim_copy(text);
    size_t start = 0;
    while (start < value.size()) {
        size_t end = value.find(',', start);
        std::string token = trim_copy(value.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (!token.empty()) {
            size_t dash = token.find('-');
            if (dash != std::string::npos) {
                int cpu_start = atoi(token.substr(0, dash).c_str());
                int cpu_end = atoi(token.substr(dash + 1).c_str());
                if (cpu_end >= cpu_start) {
                    for (int cpu = cpu_start; cpu <= cpu_end; cpu++) {
                        out.push_back(cpu);
                    }
                }
            } else {
                out.push_back(atoi(token.c_str()));
            }
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    normalize_config_cpu_list(out);
}

static void parse_cpu_mask_hex_string(const char* text, std::vector<int>& out) {
    out.clear();
    if (!text || !text[0]) return;
    char* parse_end = NULL;
    unsigned long long mask = strtoull(text, &parse_end, 0);
    if (parse_end == text) return;
    for (int cpu = 0; cpu < 64; cpu++) {
        if ((mask & (1ULL << cpu)) != 0) {
            out.push_back(cpu);
        }
    }
    normalize_config_cpu_list(out);
}

static int parse_thread_json_match_kind(const char* text) {
    std::string value = scheduler_to_lower_copy(text);
    if (value == "exact") return THREAD_JSON_MATCH_EXACT;
    if (value == "prefix") return THREAD_JSON_MATCH_PREFIX;
    if (value == "glob") return THREAD_JSON_MATCH_GLOB;
    return THREAD_JSON_MATCH_CONTAINS;
}

static int parse_thread_json_sched_policy_kind(const char* text) {
    std::string value = scheduler_to_lower_copy(text);
    if (value == "rr") return THREAD_JSON_SCHED_RR;
    if (value == "fifo") return THREAD_JSON_SCHED_FIFO;
    if (value == "other") return THREAD_JSON_SCHED_OTHER;
    return THREAD_JSON_SCHED_INHERIT;
}

static void parse_thread_json_rank_range(const char* text, int* start_rank, int* end_rank) {
    if (!start_rank || !end_rank) return;
    *start_rank = 0;
    *end_rank = 0;
    if (!text || !text[0]) return;
    std::string value = trim_copy(text);
    size_t dash = value.find('-');
    if (dash == std::string::npos) {
        int rank = atoi(value.c_str());
        if (rank > 0) {
            *start_rank = rank;
            *end_rank = rank;
        }
        return;
    }
    int start_value = atoi(value.substr(0, dash).c_str());
    int end_value = atoi(value.substr(dash + 1).c_str());
    if (start_value > 0 && end_value >= start_value) {
        *start_rank = start_value;
        *end_rank = end_value;
    }
}

static bool json_array_contains_lowered_string(cJSON* arr, const char* expected) {
    if (!arr || !cJSON_IsArray(arr) || !expected || !expected[0]) return false;
    std::string expected_value = scheduler_to_lower_copy(expected);
    cJSON* item = NULL;
    cJSON_ArrayForEach(item, arr) {
        if (!cJSON_IsString(item) || !item->valuestring) continue;
        if (scheduler_to_lower_copy(item->valuestring) == expected_value) {
            return true;
        }
    }
    return false;
}

static void load_lowered_string_array_flexible(cJSON* parent, const char* key, std::vector<std::string>& out) {
    out.clear();
    if (!parent || !key) return;
    cJSON* item = cJSON_GetObjectItem(parent, key);
    if (!item) return;
    if (cJSON_IsString(item) && item->valuestring && item->valuestring[0]) {
        std::string value = trim_copy(item->valuestring);
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return (char)tolower(c); });
        if (!value.empty()) {
            out.push_back(value);
        }
        return;
    }
    if (!cJSON_IsArray(item)) return;
    cJSON* child = NULL;
    cJSON_ArrayForEach(child, item) {
        if (!cJSON_IsString(child) || !child->valuestring || !child->valuestring[0]) continue;
        std::string value = trim_copy(child->valuestring);
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return (char)tolower(c); });
        if (!value.empty()) {
            out.push_back(value);
        }
    }
}

static void load_thread_json_action_config(cJSON* action_obj, ThreadJsonAction* out) {
    if (!out) return;
    *out = {};
    out->sched_policy = THREAD_JSON_SCHED_INHERIT;
    if (!action_obj || !cJSON_IsObject(action_obj)) return;

    cJSON* cpus = cJSON_GetObjectItem(action_obj, "cpus");
    if (cpus && cJSON_IsString(cpus) && cpus->valuestring) {
        parse_cpu_list_string(cpus->valuestring, out->cpus);
        out->has_cpus = !out->cpus.empty();
    }

    if (!out->has_cpus) {
        cJSON* mask_hex = cJSON_GetObjectItem(action_obj, "mask_hex");
        if (mask_hex && cJSON_IsString(mask_hex) && mask_hex->valuestring) {
            parse_cpu_mask_hex_string(mask_hex->valuestring, out->cpus);
            out->has_cpus = !out->cpus.empty();
        }
    }

    /*
     * 可选字段 cpu：显式指定登记进官方 pipeline 节点的核，与 cpus 解耦。
     * 支持数字 6 或字符串 "6"（软件的可视化编辑器两种都可能写出来）。
     * 不写时 has_pipeline_cpu 保持 false，沿用“取掩码最高核”的原有行为。
     */
    cJSON* pipeline_cpu = cJSON_GetObjectItem(action_obj, "cpu");
    if (pipeline_cpu) {
        int parsed_cpu = -1;
        if (cJSON_IsNumber(pipeline_cpu)) {
            parsed_cpu = pipeline_cpu->valueint;
        } else if (cJSON_IsString(pipeline_cpu) && pipeline_cpu->valuestring) {
            parsed_cpu = atoi(pipeline_cpu->valuestring);
        }
        if (parsed_cpu >= 0 && parsed_cpu < 1024) {
            out->has_pipeline_cpu = true;
            out->pipeline_cpu = parsed_cpu;
        }
    }

    load_lowered_string_array_flexible(action_obj, "clusters", out->clusters);
    out->has_clusters = !out->clusters.empty();

    cJSON* nice = cJSON_GetObjectItem(action_obj, "nice");
    if (nice && cJSON_IsNumber(nice)) {
        out->has_nice = true;
        out->nice = nice->valueint;
    }

    cJSON* scheduler = cJSON_GetObjectItem(action_obj, "scheduler");
    if (scheduler && cJSON_IsObject(scheduler)) {
        cJSON* policy = cJSON_GetObjectItem(scheduler, "policy");
        if (policy && cJSON_IsString(policy) && policy->valuestring) {
            out->sched_policy = parse_thread_json_sched_policy_kind(policy->valuestring);
        }
        cJSON* priority = cJSON_GetObjectItem(scheduler, "priority");
        if (priority && cJSON_IsNumber(priority)) {
            out->sched_priority = priority->valueint;
        }
        cJSON* fallback = cJSON_GetObjectItem(scheduler, "fallback");
        if (fallback && cJSON_IsString(fallback) && fallback->valuestring) {
            out->fallback_to_other = scheduler_to_lower_copy(fallback->valuestring) == "other";
        }
        cJSON* reset_on_background = cJSON_GetObjectItem(scheduler, "reset_on_background");
        if (reset_on_background &&
            (cJSON_IsBool(reset_on_background) || cJSON_IsNumber(reset_on_background))) {
            out->reset_on_background = reset_on_background->valueint != 0;
        }
        out->has_scheduler = out->sched_policy != THREAD_JSON_SCHED_INHERIT;
    }
}

static void load_thread_json_selector_config(cJSON* selector_obj, ThreadJsonSelector* out) {
    if (!out) return;
    *out = {};
    out->match_kind = THREAD_JSON_MATCH_CONTAINS;
    if (!selector_obj || !cJSON_IsObject(selector_obj)) return;

    cJSON* match = cJSON_GetObjectItem(selector_obj, "match");
    if (match && cJSON_IsString(match) && match->valuestring) {
        out->match_kind = parse_thread_json_match_kind(match->valuestring);
    }
    load_string_array_config(selector_obj, "patterns", out->patterns);

    cJSON* cpu_rank = cJSON_GetObjectItem(selector_obj, "cpu_rank");
    if (cpu_rank && cJSON_IsNumber(cpu_rank) && cpu_rank->valueint > 0) {
        out->cpu_rank = cpu_rank->valueint;
    }

    cJSON* cpu_rank_range = cJSON_GetObjectItem(selector_obj, "cpu_rank_range");
    if (cpu_rank_range && cJSON_IsString(cpu_rank_range) && cpu_rank_range->valuestring) {
        parse_thread_json_rank_range(cpu_rank_range->valuestring, &out->cpu_rank_start, &out->cpu_rank_end);
    }
}

static void load_thread_json_role_rule_config(cJSON* role_obj, ThreadJsonRoleRule* out) {
    if (!out) return;
    *out = {};
    if (!role_obj || !cJSON_IsObject(role_obj)) return;

    cJSON* selectors = cJSON_GetObjectItem(role_obj, "selectors");
    if (selectors && cJSON_IsArray(selectors)) {
        cJSON* item = NULL;
        cJSON_ArrayForEach(item, selectors) {
            if (!cJSON_IsObject(item)) continue;
            ThreadJsonSelector selector = {};
            load_thread_json_selector_config(item, &selector);
            if (!selector.patterns.empty()) {
                out->selectors.push_back(selector);
            }
        }
    }

    cJSON* limit = cJSON_GetObjectItem(role_obj, "limit");
    if (limit && cJSON_IsNumber(limit) && limit->valueint > 0) {
        out->limit = limit->valueint;
    }

    /* Phase5: 可选稳定参数 */
    cJSON* stable_hold = cJSON_GetObjectItem(role_obj, "stable_hold_ms");
    if (stable_hold && cJSON_IsNumber(stable_hold) && stable_hold->valueint > 0) {
        out->stable_hold_ms = stable_hold->valueint;
    }
    cJSON* rebind = cJSON_GetObjectItem(role_obj, "rebind_cooldown_ms");
    if (rebind && cJSON_IsNumber(rebind) && rebind->valueint > 0) {
        out->rebind_cooldown_ms = rebind->valueint;
    }
    cJSON* sel_thresh = cJSON_GetObjectItem(role_obj, "selection_change_threshold_ms");
    if (sel_thresh && cJSON_IsNumber(sel_thresh) && sel_thresh->valueint > 0) {
        out->selection_change_threshold_ms = sel_thresh->valueint;
    }
    cJSON* pin_once = cJSON_GetObjectItem(role_obj, "pin_once");
    if (pin_once && cJSON_IsBool(pin_once)) {
        out->pin_once = cJSON_IsTrue(pin_once);
    }
    cJSON* prefer = cJSON_GetObjectItem(role_obj, "prefer_existing_binding");
    if (prefer && cJSON_IsBool(prefer)) {
        out->prefer_existing_binding = cJSON_IsTrue(prefer);
    }

    cJSON* when = cJSON_GetObjectItem(role_obj, "when");
    out->loading_only = json_array_contains_lowered_string(when, "loading");

    cJSON* action = cJSON_GetObjectItem(role_obj, "action");
    load_thread_json_action_config(action, &out->action);
    out->enabled = !out->selectors.empty();
}

static void load_thread_json_compact_action_config(cJSON* action_obj, ThreadJsonAction* out) {
    load_thread_json_action_config(action_obj, out);
    if (!out || !action_obj || !cJSON_IsObject(action_obj)) return;
    cJSON* rr = cJSON_GetObjectItem(action_obj, "rr");
    if (rr && cJSON_IsNumber(rr)) {
        out->has_scheduler = true;
        out->sched_policy = THREAD_JSON_SCHED_RR;
        out->sched_priority = rr->valueint;
        out->fallback_to_other = true;
        out->reset_on_background = true;
    }
    cJSON* fifo = cJSON_GetObjectItem(action_obj, "fifo");
    if (fifo && cJSON_IsNumber(fifo)) {
        out->has_scheduler = true;
        out->sched_policy = THREAD_JSON_SCHED_FIFO;
        out->sched_priority = fifo->valueint;
        out->fallback_to_other = true;
        out->reset_on_background = true;
    }
}

static void thread_rule_dsl_append_selector_pattern(ThreadJsonRoleRule* out_rule,
                                                    const std::string& pattern);

static void load_thread_json_compact_match_field(cJSON* obj, ThreadJsonRoleRule* out_rule) {
    if (!obj || !out_rule) return;
    cJSON* match = cJSON_GetObjectItem(obj, "match");
    if (!match) return;
    if (cJSON_IsString(match) && match->valuestring && match->valuestring[0]) {
        thread_rule_dsl_append_selector_pattern(out_rule, trim_copy(match->valuestring));
        return;
    }
    if (!cJSON_IsArray(match)) return;
    cJSON* item = NULL;
    cJSON_ArrayForEach(item, match) {
        if (!cJSON_IsString(item) || !item->valuestring || !item->valuestring[0]) continue;
        thread_rule_dsl_append_selector_pattern(out_rule, trim_copy(item->valuestring));
    }
}

static bool load_thread_json_compact_role_rule_config(cJSON* role_obj,
                                                      int role_kind,
                                                      ThreadJsonRoleRule* out) {
    if (!out || !role_obj || !cJSON_IsObject(role_obj)) return false;
    *out = {};
    load_thread_json_compact_match_field(role_obj, out);
    cJSON* limit = cJSON_GetObjectItem(role_obj, "limit");
    if (limit && cJSON_IsNumber(limit) && limit->valueint > 0) {
        out->limit = limit->valueint;
    }
    /* Phase5: 可选稳定参数（紧凑格式） */
    cJSON* stable_hold = cJSON_GetObjectItem(role_obj, "stable_hold_ms");
    if (stable_hold && cJSON_IsNumber(stable_hold) && stable_hold->valueint > 0) {
        out->stable_hold_ms = stable_hold->valueint;
    }
    cJSON* rebind = cJSON_GetObjectItem(role_obj, "rebind_cooldown_ms");
    if (rebind && cJSON_IsNumber(rebind) && rebind->valueint > 0) {
        out->rebind_cooldown_ms = rebind->valueint;
    }
    cJSON* sel_thresh = cJSON_GetObjectItem(role_obj, "selection_change_threshold_ms");
    if (sel_thresh && cJSON_IsNumber(sel_thresh) && sel_thresh->valueint > 0) {
        out->selection_change_threshold_ms = sel_thresh->valueint;
    }
    cJSON* pin_once = cJSON_GetObjectItem(role_obj, "pin_once");
    if (pin_once && cJSON_IsBool(pin_once)) {
        out->pin_once = cJSON_IsTrue(pin_once);
    }
    cJSON* prefer = cJSON_GetObjectItem(role_obj, "prefer_existing_binding");
    if (prefer && cJSON_IsBool(prefer)) {
        out->prefer_existing_binding = cJSON_IsTrue(prefer);
    }
    load_thread_json_compact_action_config(role_obj, &out->action);
    if (out->limit <= 0 &&
        (role_kind == THREAD_JSON_ROLE_MAIN ||
         role_kind == THREAD_JSON_ROLE_GFX ||
         role_kind == THREAD_JSON_ROLE_RENDER)) {
        out->limit = 1;
    }
    out->enabled = !out->selectors.empty();
    return out->enabled || out->action.has_cpus || out->action.has_clusters ||
           out->action.has_nice || out->action.has_scheduler;
}

static bool thread_rule_json_key_is_bracket_rule(const char* key) {
    if (!key) return false;
    const char* open = strchr(key, '[');
    const char* close = strrchr(key, ']');
    return open && close && close > open + 1;
}

static void load_thread_json_compact_alias_rule(const char* key,
                                                cJSON* alias_obj,
                                                ThreadJsonAppRule* out) {
    if (!key || !alias_obj || !out || !cJSON_IsObject(alias_obj)) return;
    const char* open = strchr(key, '[');
    const char* close = strrchr(key, ']');
    if (!open || !close || close <= open + 1) return;
    std::string pattern = std::string(open + 1, (size_t)(close - open - 1));
    pattern = trim_copy(pattern);
    if (pattern.empty()) return;
    ThreadJsonCustomRule custom_rule = {};
    custom_rule.name = pattern;
    custom_rule.rule.limit = 0;
    thread_rule_dsl_append_selector_pattern(&custom_rule.rule, pattern);
    cJSON* limit = cJSON_GetObjectItem(alias_obj, "limit");
    if (limit && cJSON_IsNumber(limit) && limit->valueint > 0) {
        custom_rule.rule.limit = limit->valueint;
    }
    load_thread_json_compact_action_config(alias_obj, &custom_rule.rule.action);
    custom_rule.rule.enabled = !custom_rule.rule.selectors.empty();
    if (custom_rule.rule.enabled) {
        out->custom_rules.push_back(custom_rule);
    }
}

static void load_thread_json_safety_config(cJSON* safety_obj, ThreadJsonSafetyConfig* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->rt_max_threads = 1;
    out->rt_priority_min = 2;
    out->rt_priority_max = 12;
    out->forbid_rt_roles[THREAD_JSON_ROLE_WORKER] = true;
    out->forbid_rt_roles[THREAD_JSON_ROLE_DOWNLOAD] = true;
    out->forbid_rt_roles[THREAD_JSON_ROLE_OTHER] = true;
    if (!safety_obj || !cJSON_IsObject(safety_obj)) return;

    cJSON* allow_rt = cJSON_GetObjectItem(safety_obj, "allow_rt");
    if (allow_rt && (cJSON_IsBool(allow_rt) || cJSON_IsNumber(allow_rt))) {
        out->allow_rt = allow_rt->valueint != 0;
    }
    cJSON* allow_fifo = cJSON_GetObjectItem(safety_obj, "allow_fifo");
    if (allow_fifo && (cJSON_IsBool(allow_fifo) || cJSON_IsNumber(allow_fifo))) {
        out->allow_fifo = allow_fifo->valueint != 0;
    }
    cJSON* rt_max_threads = cJSON_GetObjectItem(safety_obj, "rt_max_threads");
    if (rt_max_threads && cJSON_IsNumber(rt_max_threads) && rt_max_threads->valueint >= 0) {
        out->rt_max_threads = rt_max_threads->valueint;
    }
    cJSON* rt_priority_min = cJSON_GetObjectItem(safety_obj, "rt_priority_min");
    if (rt_priority_min && cJSON_IsNumber(rt_priority_min) && rt_priority_min->valueint > 0) {
        out->rt_priority_min = rt_priority_min->valueint;
    }
    cJSON* rt_priority_max = cJSON_GetObjectItem(safety_obj, "rt_priority_max");
    if (rt_priority_max && cJSON_IsNumber(rt_priority_max) && rt_priority_max->valueint > 0) {
        out->rt_priority_max = rt_priority_max->valueint;
    }
    if (out->rt_priority_max < out->rt_priority_min) {
        out->rt_priority_max = out->rt_priority_min;
    }

    cJSON* forbid_roles = cJSON_GetObjectItem(safety_obj, "forbid_rt_roles");
    if (forbid_roles && cJSON_IsArray(forbid_roles)) {
        bool parsed_any = false;
        cJSON* item = NULL;
        bool role_flags[THREAD_JSON_ROLE_COUNT] = {false};
        cJSON_ArrayForEach(item, forbid_roles) {
            if (!cJSON_IsString(item) || !item->valuestring) continue;
            std::string name = scheduler_to_lower_copy(item->valuestring);
            if (name == "main") role_flags[THREAD_JSON_ROLE_MAIN] = true;
            else if (name == "gfx") role_flags[THREAD_JSON_ROLE_GFX] = true;
            else if (name == "render") role_flags[THREAD_JSON_ROLE_RENDER] = true;
            else if (name == "worker") role_flags[THREAD_JSON_ROLE_WORKER] = true;
            else if (name == "download") role_flags[THREAD_JSON_ROLE_DOWNLOAD] = true;
            else if (name == "other") role_flags[THREAD_JSON_ROLE_OTHER] = true;
            else continue;
            parsed_any = true;
        }
        if (parsed_any) {
            for (int i = 0; i < THREAD_JSON_ROLE_COUNT; i++) {
                out->forbid_rt_roles[i] = role_flags[i];
            }
        }
    }
}

static void thread_rule_dsl_skip_separators(const char** cursor) {
    if (!cursor || !*cursor) return;
    while (**cursor) {
        unsigned char ch = (unsigned char)**cursor;
        if (isspace(ch) || ch == ',' || ch == ';') {
            (*cursor)++;
            continue;
        }
        break;
    }
}

static bool thread_rule_dsl_parse_quoted_string(const char** cursor, std::string& out) {
    out.clear();
    if (!cursor || !*cursor || **cursor != '"') return false;
    (*cursor)++;
    while (**cursor) {
        char ch = **cursor;
        if (ch == '\\' && (*cursor)[1]) {
            out.push_back((*cursor)[1]);
            *cursor += 2;
            continue;
        }
        if (ch == '"') {
            (*cursor)++;
            return true;
        }
        out.push_back(ch);
        (*cursor)++;
    }
    return false;
}

static bool thread_rule_dsl_extract_balanced(const char** cursor,
                                             char open_char,
                                             char close_char,
                                             std::string& out) {
    out.clear();
    if (!cursor || !*cursor || **cursor != open_char) return false;
    const char* p = *cursor;
    int depth = 0;
    bool in_string = false;
    while (*p) {
        char ch = *p;
        if (ch == '"' && (p == *cursor || p[-1] != '\\')) {
            in_string = !in_string;
        } else if (!in_string) {
            if (ch == open_char) {
                depth++;
                if (depth > 1) out.push_back(ch);
            } else if (ch == close_char) {
                depth--;
                if (depth == 0) {
                    p++;
                    *cursor = p;
                    return true;
                }
                out.push_back(ch);
            } else {
                if (depth >= 1) out.push_back(ch);
            }
        } else if (depth >= 1) {
            out.push_back(ch);
        }
        p++;
    }
    return false;
}

static std::string thread_rule_dsl_parse_plain_token(const char** cursor) {
    std::string out;
    if (!cursor || !*cursor) return out;
    while (**cursor) {
        char ch = **cursor;
        if (ch == ',' || ch == ';' || ch == '\r' || ch == '\n' ||
            ch == '}' || ch == ']' || ch == '{') {
            break;
        }
        out.push_back(ch);
        (*cursor)++;
    }
    return trim_copy(out);
}

static std::string thread_rule_dsl_parse_key_token(const char** cursor) {
    std::string out;
    if (!cursor || !*cursor) return out;
    while (**cursor) {
        char ch = **cursor;
        if (ch == ':' || isspace((unsigned char)ch)) {
            break;
        }
        out.push_back(ch);
        (*cursor)++;
    }
    return trim_copy(out);
}

static bool thread_rule_dsl_parse_bool_token(const std::string& raw_value, bool* out_value) {
    if (!out_value) return false;
    std::string value = scheduler_to_lower_copy(trim_copy(raw_value).c_str());
    if (value == "true" || value == "1" || value == "yes" || value == "on") {
        *out_value = true;
        return true;
    }
    if (value == "false" || value == "0" || value == "no" || value == "off") {
        *out_value = false;
        return true;
    }
    return false;
}

static int thread_rule_dsl_infer_match_kind(const std::string& lowered_pattern) {
    if (lowered_pattern.find('*') != std::string::npos ||
        lowered_pattern.find('?') != std::string::npos) {
        return THREAD_JSON_MATCH_GLOB;
    }
    return THREAD_JSON_MATCH_CONTAINS;
}

static bool thread_rule_dsl_parse_rank_suffix(const std::string& raw_pattern,
                                              std::string* out_pattern,
                                              int* out_rank_start,
                                              int* out_rank_end) {
    if (out_pattern) *out_pattern = raw_pattern;
    if (out_rank_start) *out_rank_start = 0;
    if (out_rank_end) *out_rank_end = 0;
    size_t at_pos = raw_pattern.rfind('@');
    if (at_pos == std::string::npos || at_pos == 0 || at_pos + 1 >= raw_pattern.size()) {
        return false;
    }
    std::string suffix = trim_copy(raw_pattern.substr(at_pos + 1));
    if (suffix.empty()) return false;
    bool all_digits = true;
    for (size_t i = 0; i < suffix.size(); i++) {
        char ch = suffix[i];
        if (!(isdigit((unsigned char)ch) || ch == '-')) {
            all_digits = false;
            break;
        }
    }
    if (!all_digits) return false;
    int rank_start = 0;
    int rank_end = 0;
    parse_thread_json_rank_range(suffix.c_str(), &rank_start, &rank_end);
    if (rank_start <= 0) return false;
    if (rank_end < rank_start) rank_end = rank_start;
    if (out_pattern) *out_pattern = trim_copy(raw_pattern.substr(0, at_pos));
    if (out_rank_start) *out_rank_start = rank_start;
    if (out_rank_end) *out_rank_end = rank_end;
    return true;
}

static void thread_rule_dsl_append_selector_pattern(ThreadJsonRoleRule* out_rule,
                                                    const std::string& pattern) {
    if (!out_rule) return;
    std::string normalized_pattern = trim_copy(pattern);
    int rank_start = 0;
    int rank_end = 0;
    thread_rule_dsl_parse_rank_suffix(normalized_pattern, &normalized_pattern, &rank_start, &rank_end);
    bool force_exact = !normalized_pattern.empty() && normalized_pattern[0] == '=';
    if (force_exact) {
        normalized_pattern = trim_copy(normalized_pattern.substr(1));
    }
    std::string lowered_pattern = scheduler_to_lower_copy(normalized_pattern.c_str());
    if (lowered_pattern.empty()) return;
    ThreadJsonSelector selector = {};
    selector.match_kind = force_exact ? THREAD_JSON_MATCH_EXACT :
                          thread_rule_dsl_infer_match_kind(lowered_pattern);
    selector.patterns.push_back(lowered_pattern);
    if (rank_start > 0) {
        selector.cpu_rank = rank_start == rank_end ? rank_start : 0;
        selector.cpu_rank_start = rank_start;
        selector.cpu_rank_end = rank_end;
    }
    out_rule->selectors.push_back(selector);
}

static void thread_rule_dsl_parse_string_list(const std::string& body,
                                              std::vector<std::string>& out,
                                              bool lowercase) {
    out.clear();
    const char* cursor = body.c_str();
    while (*cursor) {
        thread_rule_dsl_skip_separators(&cursor);
        if (!*cursor) break;
        std::string value;
        if (*cursor == '"') {
            if (!thread_rule_dsl_parse_quoted_string(&cursor, value)) {
                break;
            }
        } else {
            value = thread_rule_dsl_parse_plain_token(&cursor);
        }
        value = trim_copy(value);
        if (!value.empty()) {
            if (lowercase) {
                value = scheduler_to_lower_copy(value.c_str());
            }
            out.push_back(value);
        }
        thread_rule_dsl_skip_separators(&cursor);
    }
}

static void thread_rule_dsl_apply_action_field(ThreadJsonAction* action,
                                               const std::string& key,
                                               const std::string& raw_value) {
    if (!action || key.empty()) return;
    std::string lowered_key = scheduler_to_lower_copy(key.c_str());
    std::string value = trim_copy(raw_value);
    if (lowered_key == "cpus") {
        parse_cpu_list_string(value.c_str(), action->cpus);
        action->has_cpus = !action->cpus.empty();
        return;
    }
    if (lowered_key == "mask_hex") {
        parse_cpu_mask_hex_string(value.c_str(), action->cpus);
        action->has_cpus = !action->cpus.empty();
        return;
    }
    /*
     * cpu：显式指定登记进官方 pipeline 节点的核（与 cpus 解耦）。
     * 位置必须在 cpus/mask_hex 之后判断，避免与 "cpus" 键混淆。
     * 不写时保持 has_pipeline_cpu=false，即沿用“取掩码最高核”的原有行为。
     */
    if (lowered_key == "cpu") {
        int parsed_cpu = atoi(value.c_str());
        if (parsed_cpu >= 0 && parsed_cpu < 1024) {
            action->has_pipeline_cpu = true;
            action->pipeline_cpu = parsed_cpu;
        }
        return;
    }
    if (lowered_key == "nice") {
        action->has_nice = true;
        action->nice = atoi(value.c_str());
        return;
    }
    if (lowered_key == "rr") {
        action->has_scheduler = true;
        action->sched_policy = THREAD_JSON_SCHED_RR;
        action->sched_priority = atoi(value.c_str());
        action->fallback_to_other = true;
        action->reset_on_background = true;
        return;
    }
    if (lowered_key == "fifo") {
        action->has_scheduler = true;
        action->sched_policy = THREAD_JSON_SCHED_FIFO;
        action->sched_priority = atoi(value.c_str());
        action->fallback_to_other = true;
        action->reset_on_background = true;
        return;
    }
}

static void thread_rule_dsl_parse_role_body(const std::string& body,
                                            int role_kind,
                                            ThreadJsonRoleRule* out_rule) {
    if (!out_rule) return;
    *out_rule = {};
    const char* cursor = body.c_str();
    while (*cursor) {
        thread_rule_dsl_skip_separators(&cursor);
        if (!*cursor) break;
        if (*cursor == '[') {
            std::string pattern_body;
            if (!thread_rule_dsl_extract_balanced(&cursor, '[', ']', pattern_body)) {
                break;
            }
            std::vector<std::string> patterns;
            thread_rule_dsl_parse_string_list(pattern_body, patterns, false);
            for (size_t i = 0; i < patterns.size(); i++) {
                thread_rule_dsl_append_selector_pattern(out_rule, patterns[i]);
            }
            continue;
        }
        std::string key = thread_rule_dsl_parse_key_token(&cursor);
        thread_rule_dsl_skip_separators(&cursor);
        if (key.empty() || *cursor != ':') {
            while (*cursor && *cursor != ',' && *cursor != '\n' && *cursor != '\r') cursor++;
            continue;
        }
        cursor++;
        thread_rule_dsl_skip_separators(&cursor);
        std::string value;
        if (*cursor == '"') {
            thread_rule_dsl_parse_quoted_string(&cursor, value);
        } else {
            value = thread_rule_dsl_parse_plain_token(&cursor);
        }
        if (scheduler_to_lower_copy(key.c_str()) == "limit") {
            out_rule->limit = std::max(0, atoi(value.c_str()));
            continue;
        }
        thread_rule_dsl_apply_action_field(&out_rule->action, key, value);
    }
    if (out_rule->limit <= 0 &&
        (role_kind == THREAD_JSON_ROLE_MAIN ||
         role_kind == THREAD_JSON_ROLE_GFX ||
         role_kind == THREAD_JSON_ROLE_RENDER)) {
        out_rule->limit = 1;
    }
    if (out_rule->limit <= 0) {
        for (size_t i = 0; i < out_rule->selectors.size(); i++) {
            const ThreadJsonSelector& selector = out_rule->selectors[i];
            if (selector.cpu_rank_start > 0 && selector.cpu_rank_end >= selector.cpu_rank_start) {
                out_rule->limit = std::max(out_rule->limit,
                                           selector.cpu_rank_end - selector.cpu_rank_start + 1);
            } else if (selector.cpu_rank > 0) {
                out_rule->limit = std::max(out_rule->limit, 1);
            }
        }
    }
    out_rule->enabled = !out_rule->selectors.empty();
}

static void thread_rule_dsl_parse_default_action_body(const std::string& body,
                                                      ThreadJsonAction* out_action,
                                                      bool* has_default_action) {
    if (!out_action || !has_default_action) return;
    *out_action = {};
    const char* cursor = body.c_str();
    while (*cursor) {
        thread_rule_dsl_skip_separators(&cursor);
        if (!*cursor) break;
        std::string key = thread_rule_dsl_parse_key_token(&cursor);
        thread_rule_dsl_skip_separators(&cursor);
        if (key.empty() || *cursor != ':') {
            while (*cursor && *cursor != ',' && *cursor != '\n' && *cursor != '\r') cursor++;
            continue;
        }
        cursor++;
        thread_rule_dsl_skip_separators(&cursor);
        std::string value;
        if (*cursor == '"') {
            thread_rule_dsl_parse_quoted_string(&cursor, value);
        } else {
            value = thread_rule_dsl_parse_plain_token(&cursor);
        }
        thread_rule_dsl_apply_action_field(out_action, key, value);
    }
    *has_default_action = out_action->has_cpus || out_action->has_clusters ||
                          out_action->has_nice || out_action->has_scheduler;
}

static void thread_rule_dsl_parse_features_body(const std::string& body,
                                                ThreadJsonAppRule* out_rule) {
    if (!out_rule) return;
    const char* cursor = body.c_str();
    while (*cursor) {
        thread_rule_dsl_skip_separators(&cursor);
        if (!*cursor) break;
        std::string key = thread_rule_dsl_parse_key_token(&cursor);
        thread_rule_dsl_skip_separators(&cursor);
        if (key.empty() || *cursor != ':') {
            while (*cursor && *cursor != ',' && *cursor != '\n' && *cursor != '\r') cursor++;
            continue;
        }
        cursor++;
        thread_rule_dsl_skip_separators(&cursor);
        std::string value;
        if (*cursor == '"') {
            thread_rule_dsl_parse_quoted_string(&cursor, value);
        } else {
            value = thread_rule_dsl_parse_plain_token(&cursor);
        }
        bool enabled = false;
        if (!thread_rule_dsl_parse_bool_token(value, &enabled)) {
            continue;
        }
        std::string lowered_key = scheduler_to_lower_copy(key.c_str());
        if (lowered_key == "dynamic_tuning") {
            out_rule->has_feature_dynamic_tuning = true;
            out_rule->feature_dynamic_tuning_enabled = enabled;
        } else if (lowered_key == "fas") {
            out_rule->has_feature_fas = true;
            out_rule->feature_fas_enabled = enabled;
        } else if (lowered_key == "custom_thread") {
            out_rule->has_feature_custom_thread = true;
            out_rule->feature_custom_thread_enabled = enabled;
        } else if (lowered_key == "dynamic_thread_scheduler") {
            out_rule->has_feature_dynamic_thread_scheduler = true;
            out_rule->feature_dynamic_thread_scheduler_enabled = enabled;
        }
    }
}

static void thread_rule_dsl_parse_session_body(const std::string& body,
                                               ThreadJsonAppRule* out_rule) {
    if (!out_rule) return;
    const char* cursor = body.c_str();
    while (*cursor) {
        thread_rule_dsl_skip_separators(&cursor);
        if (!*cursor) break;
        std::string key = thread_rule_dsl_parse_key_token(&cursor);
        thread_rule_dsl_skip_separators(&cursor);
        if (key.empty() || *cursor != ':') {
            while (*cursor && *cursor != ',' && *cursor != '\n' && *cursor != '\r') cursor++;
            continue;
        }
        cursor++;
        thread_rule_dsl_skip_separators(&cursor);
        std::string value;
        if (*cursor == '"') {
            thread_rule_dsl_parse_quoted_string(&cursor, value);
        } else {
            value = thread_rule_dsl_parse_plain_token(&cursor);
        }
        int int_value = atoi(value.c_str());
        std::string lowered_key = scheduler_to_lower_copy(key.c_str());
        if (lowered_key == "learning_duration_ms" && int_value >= 10000) {
            out_rule->session.learning_duration_ms = int_value;
            out_rule->has_session = true;
        } else if (lowered_key == "guard_interval_ms" && int_value >= 100) {
            out_rule->session.guard_interval_ms = int_value;
            out_rule->has_session = true;
        }
    }
}

static void thread_rule_dsl_parse_safety_body(const std::string& body,
                                              ThreadJsonSafetyConfig* out) {
    if (!out) return;
    const char* cursor = body.c_str();
    while (*cursor) {
        thread_rule_dsl_skip_separators(&cursor);
        if (!*cursor) break;
        std::string key = thread_rule_dsl_parse_key_token(&cursor);
        thread_rule_dsl_skip_separators(&cursor);
        if (key.empty() || *cursor != ':') {
            while (*cursor && *cursor != ',' && *cursor != '\n' && *cursor != '\r') cursor++;
            continue;
        }
        cursor++;
        thread_rule_dsl_skip_separators(&cursor);
        std::string lowered_key = scheduler_to_lower_copy(key.c_str());
        if (lowered_key == "forbid_rt_roles" && *cursor == '[') {
            std::string list_body;
            if (!thread_rule_dsl_extract_balanced(&cursor, '[', ']', list_body)) {
                break;
            }
            std::vector<std::string> roles;
            thread_rule_dsl_parse_string_list(list_body, roles, true);
            bool role_flags[THREAD_JSON_ROLE_COUNT] = {false};
            bool parsed_any = false;
            for (size_t i = 0; i < roles.size(); i++) {
                if (roles[i] == "main") role_flags[THREAD_JSON_ROLE_MAIN] = true;
                else if (roles[i] == "gfx") role_flags[THREAD_JSON_ROLE_GFX] = true;
                else if (roles[i] == "render") role_flags[THREAD_JSON_ROLE_RENDER] = true;
                else if (roles[i] == "worker") role_flags[THREAD_JSON_ROLE_WORKER] = true;
                else if (roles[i] == "download") role_flags[THREAD_JSON_ROLE_DOWNLOAD] = true;
                else if (roles[i] == "other") role_flags[THREAD_JSON_ROLE_OTHER] = true;
                else continue;
                parsed_any = true;
            }
            if (parsed_any) {
                for (int i = 0; i < THREAD_JSON_ROLE_COUNT; i++) {
                    out->forbid_rt_roles[i] = role_flags[i];
                }
            }
            continue;
        }
        std::string value;
        if (*cursor == '"') {
            thread_rule_dsl_parse_quoted_string(&cursor, value);
        } else {
            value = thread_rule_dsl_parse_plain_token(&cursor);
        }
        bool bool_value = false;
        if (lowered_key == "allow_rt" && thread_rule_dsl_parse_bool_token(value, &bool_value)) {
            out->allow_rt = bool_value;
        } else if (lowered_key == "allow_fifo" && thread_rule_dsl_parse_bool_token(value, &bool_value)) {
            out->allow_fifo = bool_value;
        } else if (lowered_key == "rt_max_threads") {
            out->rt_max_threads = atoi(value.c_str());
        } else if (lowered_key == "rt_priority_min") {
            out->rt_priority_min = atoi(value.c_str());
        } else if (lowered_key == "rt_priority_max") {
            out->rt_priority_max = atoi(value.c_str());
        }
    }
    if (out->rt_priority_max < out->rt_priority_min) {
        out->rt_priority_max = out->rt_priority_min;
    }
}

static void thread_rule_dsl_assign_signature_list(const std::string& key,
                                                  const std::vector<std::string>& values) {
    if (key == "main_thread_patterns") {
        g_thread_signature_config.main_thread_patterns = values;
    } else if (key == "render_thread_patterns") {
        g_thread_signature_config.render_thread_patterns = values;
    } else if (key == "worker_thread_patterns") {
        g_thread_signature_config.worker_thread_patterns = values;
    } else if (key == "taskgraph_thread_patterns") {
        g_thread_signature_config.taskgraph_thread_patterns = values;
    } else if (key == "worker_exclude_patterns") {
        g_thread_signature_config.worker_exclude_patterns = values;
    } else if (key == "game_priority_thread_patterns") {
        g_thread_signature_config.game_priority_thread_patterns = values;
    } else if (key == "single_big_core_package_patterns") {
        g_thread_signature_config.single_big_core_package_patterns = values;
    } else if (key == "dual_big_core_package_patterns") {
        g_thread_signature_config.dual_big_core_package_patterns = values;
    } else if (key == "main_dual_big_package_patterns") {
        g_thread_signature_config.main_dual_big_package_patterns = values;
    } else if (key == "render_dual_big_package_patterns") {
        g_thread_signature_config.render_dual_big_package_patterns = values;
    } else if (key == "ue_game_package_patterns") {
        g_thread_signature_config.ue_game_package_patterns = values;
    }
}

static bool thread_rule_config_matches_layout(const char* config_path, const char* layout_name) {
    if (!config_path || !layout_name || !layout_name[0]) return false;
    std::string lowered_path = scheduler_to_lower_copy(config_path);
    std::string lowered_layout = scheduler_to_lower_copy(layout_name);
    return lowered_path.find(lowered_layout) != std::string::npos;
}

static void thread_rule_assign_builtin_signatures_for_6p2() {
    g_thread_signature_config.main_thread_patterns = {"unitymain", "gamethread"};
    g_thread_signature_config.render_thread_patterns =
        {"unitygfx", "gfxdevice", "renderthread", "glthread", "rhithread"};
    g_thread_signature_config.worker_thread_patterns = {"job.worker", "worker", "task"};
    g_thread_signature_config.taskgraph_thread_patterns = {"taskgraphnp"};
    g_thread_signature_config.worker_exclude_patterns = {"asynctask", "hwuitask", "hwui"};
    g_thread_signature_config.game_priority_thread_patterns =
        {"unitygfx", "gfxdevice", "renderthread", "glthread", "rhithread"};
}

static void apply_builtin_thread_signature_defaults(const char* config_path) {
    if (thread_rule_config_matches_layout(config_path, "6+2")) {
        thread_rule_assign_builtin_signatures_for_6p2();
    }
}

static bool thread_rule_action_uses_rt_scheduler(const ThreadJsonAction& action) {
    return action.has_scheduler &&
           (action.sched_policy == THREAD_JSON_SCHED_RR ||
            action.sched_policy == THREAD_JSON_SCHED_FIFO);
}

static void apply_builtin_thread_app_defaults(const char* config_path, ThreadJsonAppRule* out_rule) {
    if (!out_rule) return;
    if (thread_rule_config_matches_layout(config_path, "6+2")) {
        if (!out_rule->has_feature_dynamic_tuning) {
            out_rule->has_feature_dynamic_tuning = true;
            out_rule->feature_dynamic_tuning_enabled = true;
        }
        if (!out_rule->has_feature_fas) {
            out_rule->has_feature_fas = true;
            out_rule->feature_fas_enabled = true;
        }
        if (!out_rule->has_feature_custom_thread) {
            out_rule->has_feature_custom_thread = true;
            out_rule->feature_custom_thread_enabled = true;
        }
        if (!out_rule->has_feature_dynamic_thread_scheduler) {
            out_rule->has_feature_dynamic_thread_scheduler = true;
            out_rule->feature_dynamic_thread_scheduler_enabled = true;
        }
        if (!out_rule->has_session) {
            out_rule->session.learning_duration_ms = 90000;
            out_rule->session.guard_interval_ms = 700;
            out_rule->has_session = true;
        }
        bool has_rt_rule = false;
        bool has_fifo_rule = false;
        int rt_slots = 0;
        for (int i = 0; i < THREAD_JSON_ROLE_COUNT; i++) {
            const ThreadJsonRoleRule& role_rule = out_rule->roles[i];
            if (!role_rule.enabled || !thread_rule_action_uses_rt_scheduler(role_rule.action)) continue;
            has_rt_rule = true;
            if (role_rule.action.sched_policy == THREAD_JSON_SCHED_FIFO) {
                has_fifo_rule = true;
            }
            rt_slots += std::max(1, role_rule.limit > 0 ? role_rule.limit : 1);
        }
        for (size_t i = 0; i < out_rule->custom_rules.size(); i++) {
            const ThreadJsonRoleRule& rule = out_rule->custom_rules[i].rule;
            if (!rule.enabled || !thread_rule_action_uses_rt_scheduler(rule.action)) continue;
            has_rt_rule = true;
            if (rule.action.sched_policy == THREAD_JSON_SCHED_FIFO) {
                has_fifo_rule = true;
            }
            rt_slots += std::max(1, rule.limit > 0 ? rule.limit : 1);
        }
        out_rule->safety.allow_rt = has_rt_rule;
        out_rule->safety.allow_fifo = has_fifo_rule;
        out_rule->safety.rt_priority_min = 1;
        out_rule->safety.rt_priority_max = 10;
        out_rule->safety.forbid_rt_roles[THREAD_JSON_ROLE_DOWNLOAD] = true;
        out_rule->safety.forbid_rt_roles[THREAD_JSON_ROLE_OTHER] = true;
        if (!has_rt_rule) {
            out_rule->safety.rt_max_threads = 0;
        } else {
            out_rule->safety.rt_max_threads = std::min(4, std::max(1, rt_slots));
        }
    }
}

static void thread_rule_dsl_parse_signatures_body(const std::string& body) {
    const char* cursor = body.c_str();
    while (*cursor) {
        thread_rule_dsl_skip_separators(&cursor);
        if (!*cursor) break;
        std::string key = thread_rule_dsl_parse_key_token(&cursor);
        thread_rule_dsl_skip_separators(&cursor);
        if (key.empty() || *cursor != ':') {
            while (*cursor && *cursor != ',' && *cursor != '\n' && *cursor != '\r') cursor++;
            continue;
        }
        cursor++;
        thread_rule_dsl_skip_separators(&cursor);
        if (*cursor == '[') {
            std::string list_body;
            if (!thread_rule_dsl_extract_balanced(&cursor, '[', ']', list_body)) {
                break;
            }
            std::vector<std::string> values;
            thread_rule_dsl_parse_string_list(list_body, values, false);
            thread_rule_dsl_assign_signature_list(scheduler_to_lower_copy(key.c_str()), values);
        } else {
            std::string value;
            if (*cursor == '"') {
                thread_rule_dsl_parse_quoted_string(&cursor, value);
            } else {
                value = thread_rule_dsl_parse_plain_token(&cursor);
            }
            if (scheduler_to_lower_copy(key.c_str()) == "render_dual_big_min_usage_pct") {
                g_thread_signature_config.render_dual_big_min_usage_pct = (float)atof(value.c_str());
            }
        }
    }
}

static void thread_rule_dsl_parse_alias_rule(ThreadJsonAppRule* out_rule,
                                             const std::string& pattern,
                                             const std::string& rhs) {
    if (!out_rule) return;
    ThreadJsonCustomRule custom_rule = {};
    custom_rule.name = trim_copy(pattern);
    if (custom_rule.name.size() >= 2 &&
        custom_rule.name.front() == '"' &&
        custom_rule.name.back() == '"') {
        custom_rule.name = custom_rule.name.substr(1, custom_rule.name.size() - 2);
    }
    custom_rule.rule.limit = 0;
    thread_rule_dsl_append_selector_pattern(&custom_rule.rule, custom_rule.name);
    std::string rhs_trimmed = trim_copy(rhs);
    if (rhs_trimmed.empty()) return;
    if (!rhs_trimmed.empty() && rhs_trimmed[0] == '{') {
        const char* rhs_cursor = rhs_trimmed.c_str();
        std::string body;
        if (thread_rule_dsl_extract_balanced(&rhs_cursor, '{', '}', body)) {
            const char* body_cursor = body.c_str();
            while (*body_cursor) {
                thread_rule_dsl_skip_separators(&body_cursor);
                if (!*body_cursor) break;
                std::string key = thread_rule_dsl_parse_key_token(&body_cursor);
                thread_rule_dsl_skip_separators(&body_cursor);
                if (key.empty() || *body_cursor != ':') {
                    while (*body_cursor &&
                           *body_cursor != ',' && *body_cursor != '\n' && *body_cursor != '\r') {
                        body_cursor++;
                    }
                    continue;
                }
                body_cursor++;
                thread_rule_dsl_skip_separators(&body_cursor);
                std::string value;
                if (*body_cursor == '"') {
                    thread_rule_dsl_parse_quoted_string(&body_cursor, value);
                } else {
                    value = thread_rule_dsl_parse_plain_token(&body_cursor);
                }
                if (scheduler_to_lower_copy(key.c_str()) == "limit") {
                    custom_rule.rule.limit = std::max(0, atoi(value.c_str()));
                    continue;
                }
                thread_rule_dsl_apply_action_field(&custom_rule.rule.action, key, value);
            }
        }
    } else {
        thread_rule_dsl_apply_action_field(&custom_rule.rule.action, "cpus", rhs_trimmed);
    }
    custom_rule.rule.enabled = !custom_rule.rule.selectors.empty();
    if (custom_rule.rule.enabled) {
        out_rule->custom_rules.push_back(custom_rule);
    }
}

static bool load_thread_rule_app_dsl_block(const std::string& body, ThreadJsonAppRule* out) {
    if (!out) return false;
    *out = {};
    out->safety.rt_max_threads = 1;
    out->safety.rt_priority_min = 2;
    out->safety.rt_priority_max = 12;
    out->safety.forbid_rt_roles[THREAD_JSON_ROLE_WORKER] = true;
    out->safety.forbid_rt_roles[THREAD_JSON_ROLE_DOWNLOAD] = true;
    out->safety.forbid_rt_roles[THREAD_JSON_ROLE_OTHER] = true;
    const char* cursor = body.c_str();
    while (*cursor) {
        thread_rule_dsl_skip_separators(&cursor);
        if (!*cursor) break;
        if (*cursor == '[') {
            std::string lhs_body;
            if (!thread_rule_dsl_extract_balanced(&cursor, '[', ']', lhs_body)) {
                break;
            }
            thread_rule_dsl_skip_separators(&cursor);
            if (*cursor != '=') {
                continue;
            }
            cursor++;
            thread_rule_dsl_skip_separators(&cursor);
            std::string rhs;
            if (*cursor == '{') {
                std::string rhs_body;
                if (!thread_rule_dsl_extract_balanced(&cursor, '{', '}', rhs_body)) {
                    break;
                }
                rhs = "{" + rhs_body + "}";
            } else {
                rhs = thread_rule_dsl_parse_plain_token(&cursor);
            }
            thread_rule_dsl_parse_alias_rule(out, trim_copy(lhs_body), rhs);
            continue;
        }

        std::string key = thread_rule_dsl_parse_key_token(&cursor);
        thread_rule_dsl_skip_separators(&cursor);
        if (key.empty() || *cursor != ':') {
            while (*cursor && *cursor != ',' && *cursor != '\n' && *cursor != '\r') cursor++;
            continue;
        }
        cursor++;
        thread_rule_dsl_skip_separators(&cursor);
        std::string lowered_key = scheduler_to_lower_copy(key.c_str());
        if (lowered_key == "friendly") {
            std::string value;
            if (*cursor == '"') {
                thread_rule_dsl_parse_quoted_string(&cursor, value);
            } else {
                value = thread_rule_dsl_parse_plain_token(&cursor);
            }
            out->friendly = trim_copy(value);
            continue;
        }
        if (lowered_key == "packages") {
            std::string package_body;
            if (!thread_rule_dsl_extract_balanced(&cursor, '[', ']', package_body)) {
                break;
            }
            thread_rule_dsl_parse_string_list(package_body, out->packages, false);
            continue;
        }
        if (lowered_key == "scope") {
            std::string value;
            if (*cursor == '"') {
                thread_rule_dsl_parse_quoted_string(&cursor, value);
            } else {
                value = thread_rule_dsl_parse_plain_token(&cursor);
            }
            std::string scope_value = scheduler_to_lower_copy(value.c_str());
            out->persistent_scope = (scope_value == "persistent" ||
                                     scope_value == "global" ||
                                     scope_value == "system");
            continue;
        }
        if (lowered_key == "main" ||
            lowered_key == "gfx" ||
            lowered_key == "render" ||
            lowered_key == "worker") {
            std::string role_body;
            if (!thread_rule_dsl_extract_balanced(&cursor, '{', '}', role_body)) {
                break;
            }
            int role_kind = lowered_key == "main" ? THREAD_JSON_ROLE_MAIN :
                            (lowered_key == "gfx" ? THREAD_JSON_ROLE_GFX :
                             (lowered_key == "render" ? THREAD_JSON_ROLE_RENDER :
                              THREAD_JSON_ROLE_WORKER));
            thread_rule_dsl_parse_role_body(role_body, role_kind, &out->roles[role_kind]);
            continue;
        }
        if (lowered_key == "other") {
            std::string other_body;
            if (!thread_rule_dsl_extract_balanced(&cursor, '{', '}', other_body)) {
                break;
            }
            thread_rule_dsl_parse_default_action_body(other_body, &out->default_action,
                                                      &out->has_default_action);
            continue;
        }
        if (lowered_key == "features") {
            std::string feature_body;
            if (!thread_rule_dsl_extract_balanced(&cursor, '{', '}', feature_body)) {
                break;
            }
            thread_rule_dsl_parse_features_body(feature_body, out);
            continue;
        }
        if (lowered_key == "session") {
            std::string session_body;
            if (!thread_rule_dsl_extract_balanced(&cursor, '{', '}', session_body)) {
                break;
            }
            thread_rule_dsl_parse_session_body(session_body, out);
            continue;
        }
        if (lowered_key == "safety") {
            std::string safety_body;
            if (!thread_rule_dsl_extract_balanced(&cursor, '{', '}', safety_body)) {
                break;
            }
            thread_rule_dsl_parse_safety_body(safety_body, &out->safety);
            continue;
        }
        while (*cursor && *cursor != ',' && *cursor != '\n' && *cursor != '\r') cursor++;
    }
    return !out->packages.empty();
}

static bool load_thread_rules_dsl_config(const char* data, const char* config_path) {
    if (!data) return false;
    apply_builtin_thread_signature_defaults(config_path);
    const char* signatures_keyword = strstr(data, "signatures");
    if (signatures_keyword) {
        const char* signatures_colon = strchr(signatures_keyword, ':');
        if (signatures_colon) {
            const char* signatures_object = strchr(signatures_colon, '{');
            if (signatures_object) {
                std::string signatures_body;
                const char* signatures_cursor = signatures_object;
                if (thread_rule_dsl_extract_balanced(&signatures_cursor, '{', '}', signatures_body)) {
                    thread_rule_dsl_parse_signatures_body(signatures_body);
                }
            }
        }
    }
    const char* apps_keyword = strstr(data, "apps");
    if (!apps_keyword) return false;
    const char* apps_colon = strchr(apps_keyword, ':');
    if (!apps_colon) return false;
    const char* apps_array = strchr(apps_colon, '[');
    if (!apps_array) return false;
    std::string apps_body;
    const char* cursor = apps_array;
    if (!thread_rule_dsl_extract_balanced(&cursor, '[', ']', apps_body)) {
        return false;
    }
    const char* app_cursor = apps_body.c_str();
    int loaded_count = 0;
    while (*app_cursor) {
        thread_rule_dsl_skip_separators(&app_cursor);
        if (!*app_cursor) break;
        if (*app_cursor != '{') {
            app_cursor++;
            continue;
        }
        std::string app_body;
        if (!thread_rule_dsl_extract_balanced(&app_cursor, '{', '}', app_body)) {
            break;
        }
        ThreadJsonAppRule app_rule = {};
        if (load_thread_rule_app_dsl_block(app_body, &app_rule) && !app_rule.packages.empty()) {
            apply_builtin_thread_app_defaults(config_path, &app_rule);
            g_thread_app_rules.push_back(app_rule);
            loaded_count++;
        }
    }
    if (loaded_count <= 0) return false;
    char log_msg[256];
    safe_snprintf(log_msg, sizeof(log_msg),
                  "已加载线程规则DSL配置: %s | apps:%d",
                  config_path ? config_path : "unknown",
                  loaded_count);
    log_config_message(log_msg);
    return true;
}

static void load_thread_json_app_rule_config(cJSON* app_obj, ThreadJsonAppRule* out) {
    if (!out) return;
    *out = {};
    out->safety.rt_max_threads = 1;
    out->safety.rt_priority_min = 2;
    out->safety.rt_priority_max = 12;
    out->safety.forbid_rt_roles[THREAD_JSON_ROLE_WORKER] = true;
    out->safety.forbid_rt_roles[THREAD_JSON_ROLE_DOWNLOAD] = true;
    out->safety.forbid_rt_roles[THREAD_JSON_ROLE_OTHER] = true;
    if (!app_obj || !cJSON_IsObject(app_obj)) return;

    cJSON* friendly = cJSON_GetObjectItem(app_obj, "friendly");
    if (friendly && cJSON_IsString(friendly) && friendly->valuestring) {
        out->friendly = trim_copy(friendly->valuestring);
    }
    load_string_array_config(app_obj, "packages", out->packages);
    cJSON* scope = cJSON_GetObjectItem(app_obj, "scope");
    if (scope && cJSON_IsString(scope) && scope->valuestring) {
        std::string scope_value = scheduler_to_lower_copy(scope->valuestring);
        out->persistent_scope = (scope_value == "persistent" || scope_value == "global" ||
                                 scope_value == "system");
    }

    cJSON* features = cJSON_GetObjectItem(app_obj, "features");
    if (features && cJSON_IsObject(features)) {
        cJSON* dynamic_tuning = cJSON_GetObjectItem(features, "dynamic_tuning");
        if (dynamic_tuning && (cJSON_IsBool(dynamic_tuning) || cJSON_IsNumber(dynamic_tuning))) {
            out->has_feature_dynamic_tuning = true;
            out->feature_dynamic_tuning_enabled = dynamic_tuning->valueint != 0;
        }
        cJSON* fas = cJSON_GetObjectItem(features, "fas");
        if (fas && (cJSON_IsBool(fas) || cJSON_IsNumber(fas))) {
            out->has_feature_fas = true;
            out->feature_fas_enabled = fas->valueint != 0;
        }
        cJSON* custom_thread = cJSON_GetObjectItem(features, "custom_thread");
        if (custom_thread && (cJSON_IsBool(custom_thread) || cJSON_IsNumber(custom_thread))) {
            out->has_feature_custom_thread = true;
            out->feature_custom_thread_enabled = custom_thread->valueint != 0;
        }
        cJSON* dynamic_thread_scheduler =
            cJSON_GetObjectItem(features, "dynamic_thread_scheduler");
        if (dynamic_thread_scheduler &&
            (cJSON_IsBool(dynamic_thread_scheduler) || cJSON_IsNumber(dynamic_thread_scheduler))) {
            out->has_feature_dynamic_thread_scheduler = true;
            out->feature_dynamic_thread_scheduler_enabled =
                dynamic_thread_scheduler->valueint != 0;
        }
    }

    cJSON* default_action = cJSON_GetObjectItem(app_obj, "default_action");
    if (default_action && cJSON_IsObject(default_action)) {
        load_thread_json_action_config(default_action, &out->default_action);
        out->has_default_action = out->default_action.has_clusters ||
                                  out->default_action.has_cpus ||
                                  out->default_action.has_nice ||
                                  out->default_action.has_scheduler;
    }

    cJSON* compact_other = cJSON_GetObjectItem(app_obj, "other");
    if (!out->has_default_action && compact_other && cJSON_IsObject(compact_other)) {
        load_thread_json_compact_action_config(compact_other, &out->default_action);
        out->has_default_action = out->default_action.has_clusters ||
                                  out->default_action.has_cpus ||
                                  out->default_action.has_nice ||
                                  out->default_action.has_scheduler;
    }

    cJSON* session = cJSON_GetObjectItem(app_obj, "session");
    if (session && cJSON_IsObject(session)) {
        cJSON* learning_duration_ms = cJSON_GetObjectItem(session, "learning_duration_ms");
        cJSON* guard_interval_ms = cJSON_GetObjectItem(session, "guard_interval_ms");
        if (learning_duration_ms && cJSON_IsNumber(learning_duration_ms) &&
            learning_duration_ms->valueint >= 10000) {
            out->session.learning_duration_ms = learning_duration_ms->valueint;
            out->has_session = true;
        }
        if (guard_interval_ms && cJSON_IsNumber(guard_interval_ms) &&
            guard_interval_ms->valueint >= 100) {
            out->session.guard_interval_ms = guard_interval_ms->valueint;
            out->has_session = true;
        }
    }

    cJSON* threads = cJSON_GetObjectItem(app_obj, "threads");
    if (threads && cJSON_IsObject(threads)) {
        static const struct {
            const char* key;
            int role;
        } role_map[] = {
            {"main", THREAD_JSON_ROLE_MAIN},
            {"gfx", THREAD_JSON_ROLE_GFX},
            {"render", THREAD_JSON_ROLE_RENDER},
            {"worker", THREAD_JSON_ROLE_WORKER},
            {"download", THREAD_JSON_ROLE_DOWNLOAD},
            {"other", THREAD_JSON_ROLE_OTHER}
        };
        for (size_t i = 0; i < sizeof(role_map) / sizeof(role_map[0]); i++) {
            cJSON* role_obj = cJSON_GetObjectItem(threads, role_map[i].key);
            if (role_obj && cJSON_IsObject(role_obj)) {
                load_thread_json_role_rule_config(role_obj, &out->roles[role_map[i].role]);
            }
        }

        cJSON* custom = cJSON_GetObjectItem(threads, "custom");
        if (custom && cJSON_IsArray(custom)) {
            cJSON* item = NULL;
            cJSON_ArrayForEach(item, custom) {
                if (!cJSON_IsObject(item)) continue;
                ThreadJsonCustomRule custom_rule = {};
                cJSON* name = cJSON_GetObjectItem(item, "name");
                if (name && cJSON_IsString(name) && name->valuestring) {
                    custom_rule.name = trim_copy(name->valuestring);
                }
                load_thread_json_role_rule_config(item, &custom_rule.rule);
                if (custom_rule.rule.enabled) {
                    out->custom_rules.push_back(custom_rule);
                }
            }
        }
    }

    static const struct {
        const char* key;
        int role;
    } compact_role_map[] = {
        {"main", THREAD_JSON_ROLE_MAIN},
        {"gfx", THREAD_JSON_ROLE_GFX},
        {"render", THREAD_JSON_ROLE_RENDER},
        {"worker", THREAD_JSON_ROLE_WORKER}
    };
    for (size_t i = 0; i < sizeof(compact_role_map) / sizeof(compact_role_map[0]); i++) {
        if (out->roles[compact_role_map[i].role].enabled) continue;
        cJSON* role_obj = cJSON_GetObjectItem(app_obj, compact_role_map[i].key);
        if (role_obj && cJSON_IsObject(role_obj)) {
            load_thread_json_compact_role_rule_config(role_obj, compact_role_map[i].role,
                                                      &out->roles[compact_role_map[i].role]);
        }
    }

    cJSON* child = NULL;
    cJSON_ArrayForEach(child, app_obj) {
        if (!child->string || !thread_rule_json_key_is_bracket_rule(child->string)) continue;
        load_thread_json_compact_alias_rule(child->string, child, out);
    }

    cJSON* safety = cJSON_GetObjectItem(app_obj, "safety");
    load_thread_json_safety_config(safety, &out->safety);
}

static void load_thread_signature_config() {
    // 热更新保护: 构建期间持有 g_mutex 写锁，防止读线程遍历半构建状态
    std::lock_guard<std::shared_mutex> lock(g_mutex);
    clear_thread_signature_config();

    const char* config_path = get_thread_signature_config_path();
    if (!file_exists(config_path)) {
        return;
    }

    FILE* fp = fopen(config_path, "rb");
    if (!fp) {
        char error_msg[256];
        safe_snprintf(error_msg, sizeof(error_msg), "无法打开线程签名扩展配置: %s (错误: %s)",
                      config_path, strerror(errno));
        log_config_message(error_msg);
        return;
    }

    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    if (len <= 0) {
        log_config_message("线程签名扩展配置为空或读取失败");
        cleanup_resources(fp, NULL, NULL);
        return;
    }
    fseek(fp, 0, SEEK_SET);

    char* data = (char*)malloc(len + 1);
    if (!data) {
        log_config_message("内存分配失败");
        cleanup_resources(fp, NULL, NULL);
        return;
    }

    size_t bytes_read = fread(data, 1, len, fp);
    data[bytes_read] = 0;
    fclose(fp);
    fp = NULL;

    cJSON* root = cJSON_Parse(data);
    if (!root) {
        if (load_thread_rules_dsl_config(data, config_path)) {
            cleanup_resources(NULL, data, NULL);
            return;
        }
        const char* error_ptr = cJSON_GetErrorPtr();
        if (error_ptr) {
            char error_msg[256];
            safe_snprintf(error_msg, sizeof(error_msg), "线程签名扩展JSON/DSL解析失败: %s", error_ptr);
            log_config_message(error_msg);
        }
        cleanup_resources(NULL, data, NULL);
        return;
    }

    apply_builtin_thread_signature_defaults(config_path);

    cJSON* signature_root = cJSON_GetObjectItem(root, "signatures");
    if (!signature_root || !cJSON_IsObject(signature_root)) {
        signature_root = root;
    }

    load_string_array_config(signature_root, "main_thread_patterns", g_thread_signature_config.main_thread_patterns);
    load_string_array_config(signature_root, "render_thread_patterns", g_thread_signature_config.render_thread_patterns);
    load_string_array_config(signature_root, "worker_thread_patterns", g_thread_signature_config.worker_thread_patterns);
    load_string_array_config(signature_root, "taskgraph_thread_patterns", g_thread_signature_config.taskgraph_thread_patterns);
    load_string_array_config(signature_root, "worker_exclude_patterns", g_thread_signature_config.worker_exclude_patterns);
    load_string_array_config(signature_root, "game_priority_thread_patterns", g_thread_signature_config.game_priority_thread_patterns);
    load_string_array_config(signature_root, "single_big_core_package_patterns", g_thread_signature_config.single_big_core_package_patterns);
    load_string_array_config(signature_root, "dual_big_core_package_patterns", g_thread_signature_config.dual_big_core_package_patterns);
    load_string_array_config(signature_root, "main_dual_big_package_patterns", g_thread_signature_config.main_dual_big_package_patterns);
    load_string_array_config(signature_root, "render_dual_big_package_patterns", g_thread_signature_config.render_dual_big_package_patterns);
    load_string_array_config(signature_root, "ue_game_package_patterns", g_thread_signature_config.ue_game_package_patterns);
    load_float_config(signature_root, "render_dual_big_min_usage_pct",
                      &g_thread_signature_config.render_dual_big_min_usage_pct, 0.0f);

    cJSON* apps = cJSON_GetObjectItem(root, "apps");
    if (apps && cJSON_IsArray(apps)) {
        cJSON* item = NULL;
        cJSON_ArrayForEach(item, apps) {
            if (!cJSON_IsObject(item)) continue;
            ThreadJsonAppRule app_rule = {};
            load_thread_json_app_rule_config(item, &app_rule);
            if (!app_rule.packages.empty()) {
                apply_builtin_thread_app_defaults(config_path, &app_rule);
                g_thread_app_rules.push_back(app_rule);
            }
        }
    }

    char log_msg[320];
    safe_snprintf(log_msg, sizeof(log_msg),
                  "已加载线程签名扩展配置: %s | main:%d render:%d worker:%d taskgraph:%d | apps:%d",
                  config_path,
                  (int)g_thread_signature_config.main_thread_patterns.size(),
                  (int)g_thread_signature_config.render_thread_patterns.size(),
                  (int)g_thread_signature_config.worker_thread_patterns.size(),
                  (int)g_thread_signature_config.taskgraph_thread_patterns.size(),
                  (int)g_thread_app_rules.size());
    log_config_message(log_msg);

    // 设备级 persistent 规则覆盖: 如果存在 {cpu_model}.persistent.json，
    // 其 persistent-scope 规则会替换模板中的同名包规则（用于跨SoC分发调参）
    char persistent_override_path[256];
    safe_snprintf(persistent_override_path, sizeof(persistent_override_path),
                  "%s%s.persistent.json", CPU_CONFIG_DIR, cpu_model);
    if (file_exists(persistent_override_path)) {
        FILE* pf = fopen(persistent_override_path, "rb");
        if (pf) {
            fseek(pf, 0, SEEK_END);
            long plen = ftell(pf);
            fseek(pf, 0, SEEK_SET);
            if (plen > 0) {
                char* pdata = (char*)malloc(plen + 1);
                if (pdata) {
                    size_t pbread = fread(pdata, 1, plen, pf);
                    pdata[pbread] = 0;
                    cJSON* proot = cJSON_Parse(pdata);
                    if (proot) {
                        cJSON* persistent_root = cJSON_GetObjectItem(proot, "apps");
                        if (persistent_root && cJSON_IsArray(persistent_root)) {
                            cJSON* pitem = NULL;
                            cJSON_ArrayForEach(pitem, persistent_root) {
                                if (!cJSON_IsObject(pitem)) continue;
                                cJSON* scope = cJSON_GetObjectItem(pitem, "scope");
                                if (!scope || !cJSON_IsString(scope) || strcmp(scope->valuestring, "persistent") != 0) continue;
                                cJSON* packages = cJSON_GetObjectItem(pitem, "packages");
                                if (!packages || !cJSON_IsArray(packages) || !cJSON_GetArraySize(packages)) continue;
                                const char* first_pkg = cJSON_GetArrayItem(packages, 0)->valuestring;
                                if (!first_pkg) continue;

                                // 删除模板中的同名persistent包规则
                                for (auto it = g_thread_app_rules.begin(); it != g_thread_app_rules.end(); ) {
                                    if (!it->packages.empty() && strcmp(it->packages[0].c_str(), first_pkg) == 0) {
                                        it = g_thread_app_rules.erase(it);
                                    } else {
                                        ++it;
                                    }
                                }

                                // 加载覆盖规则
                                ThreadJsonAppRule override_rule = {};
                                load_thread_json_app_rule_config(pitem, &override_rule);
                                if (!override_rule.packages.empty()) {
                                    apply_builtin_thread_app_defaults(persistent_override_path, &override_rule);
                                    g_thread_app_rules.push_back(override_rule);
                                }
                            }
                        }
                        cJSON_Delete(proot);
                    }
                    free(pdata);
                }
            }
            fclose(pf);
            char pmsg[256];
            safe_snprintf(pmsg, sizeof(pmsg), "已加载设备持久规则覆盖: %s", persistent_override_path);
            log_config_message(pmsg);
        }
    }

    cleanup_resources(NULL, data, root);
    /* 线程规则变了：每应用的生效开关缓存作废（规则里也有按应用的开关覆写） */
    invalidate_runtime_feature_switch_cache();
}

static void apply_scheduler_session_json(cJSON* session_obj) {
    if (!session_obj || !cJSON_IsObject(session_obj)) return;

    cJSON* learning_duration_ms = cJSON_GetObjectItem(session_obj, "learning_duration_ms");
    if (learning_duration_ms && cJSON_IsNumber(learning_duration_ms) &&
        learning_duration_ms->valueint >= 10000) {
        g_scheduler_session_config.learning_duration_ms = learning_duration_ms->valueint;
    }

    cJSON* guard_interval_ms = cJSON_GetObjectItem(session_obj, "guard_interval_ms");
    if (guard_interval_ms && cJSON_IsNumber(guard_interval_ms) &&
        guard_interval_ms->valueint >= 100) {
        g_scheduler_session_config.guard_interval_ms = guard_interval_ms->valueint;
    }
}

static void load_scheduler_session_config_json(const char* mode) {
    const char* config_path = get_scheduler_config_path();
    if (!file_exists(config_path)) {
        return;
    }

    FILE* fp = fopen(config_path, "rb");
    if (!fp) {
        char error_msg[256];
        safe_snprintf(error_msg, sizeof(error_msg), "无法打开调度扩展配置: %s (错误: %s)",
                      config_path, strerror(errno));
        log_config_message(error_msg);
        return;
    }

    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    if (len <= 0) {
        log_config_message("调度扩展配置为空或读取失败");
        cleanup_resources(fp, NULL, NULL);
        return;
    }
    fseek(fp, 0, SEEK_SET);

    char* data = (char*)malloc(len + 1);
    if (!data) {
        log_config_message("内存分配失败");
        cleanup_resources(fp, NULL, NULL);
        return;
    }

    size_t bytes_read = fread(data, 1, len, fp);
    data[bytes_read] = 0;
    fclose(fp);
    fp = NULL;

    cJSON* root = cJSON_Parse(data);
    if (!root) {
        const char* error_ptr = cJSON_GetErrorPtr();
        if (error_ptr) {
            char error_msg[256];
            safe_snprintf(error_msg, sizeof(error_msg), "调度扩展JSON解析失败: %s", error_ptr);
            log_config_message(error_msg);
        }
        cleanup_resources(NULL, data, NULL);
        return;
    }

    cJSON* default_obj = cJSON_GetObjectItem(root, "default");
    if (default_obj && cJSON_IsObject(default_obj)) {
        apply_scheduler_session_json(default_obj);
    }

    cJSON* mode_obj = cJSON_GetObjectItem(root, mode);
    if (mode_obj && cJSON_IsObject(mode_obj)) {
        apply_scheduler_session_json(mode_obj);
    }

    cleanup_resources(NULL, data, root);
}

void load_scene_categories_config() {
    std::lock_guard<std::shared_mutex> lock(g_mutex);
    g_scene_category_rules.clear();
    if (!file_exists(SCENE_CATEGORIES_FILE)) {
        return;
    }

    FILE* fp = fopen(SCENE_CATEGORIES_FILE, "rb");
    if (!fp) {
        char error_msg[256];
        safe_snprintf(error_msg, sizeof(error_msg), "无法打开场景分类配置: %s (错误: %s)",
                      SCENE_CATEGORIES_FILE, strerror(errno));
        log_config_message(error_msg);
        return;
    }

    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    if (len <= 0) {
        log_config_message("场景分类配置为空或读取失败");
        cleanup_resources(fp, NULL, NULL);
        return;
    }
    fseek(fp, 0, SEEK_SET);

    char* data = (char*)malloc(len + 1);
    if (!data) {
        log_config_message("内存分配失败");
        cleanup_resources(fp, NULL, NULL);
        return;
    }

    size_t bytes_read = fread(data, 1, len, fp);
    data[bytes_read] = 0;
    fclose(fp);
    fp = NULL;

    cJSON* root = cJSON_Parse(data);
    if (!root) {
        const char* error_ptr = cJSON_GetErrorPtr();
        if (error_ptr) {
            char error_msg[256];
            safe_snprintf(error_msg, sizeof(error_msg), "场景分类JSON解析失败: %s", error_ptr);
            log_config_message(error_msg);
        }
        cleanup_resources(NULL, data, NULL);
        return;
    }

    cJSON* rules = root;
    if (cJSON_IsObject(root)) {
        cJSON* categories = cJSON_GetObjectItem(root, "categories");
        if (categories && cJSON_IsArray(categories)) {
            rules = categories;
        } else {
            cJSON* items = cJSON_GetObjectItem(root, "items");
            if (items && cJSON_IsArray(items)) {
                rules = items;
            }
        }
    }

    if (rules && cJSON_IsArray(rules)) {
        cJSON* item = NULL;
        cJSON_ArrayForEach(item, rules) {
            if (!cJSON_IsObject(item)) continue;
            SceneCategoryRule rule = {};

            cJSON* friendly = cJSON_GetObjectItem(item, "friendly");
            if (friendly && cJSON_IsString(friendly) && friendly->valuestring) {
                rule.friendly = trim_copy(friendly->valuestring);
            }

            cJSON* category = cJSON_GetObjectItem(item, "category");
            if (category && cJSON_IsString(category) && category->valuestring) {
                rule.category = trim_copy(category->valuestring);
            }
            cJSON* game = cJSON_GetObjectItem(item, "game");
            if (game && (cJSON_IsBool(game) || cJSON_IsNumber(game))) {
                rule.game = game->valueint != 0;
            } else {
                std::string lowered_category = scheduler_to_lower_copy(rule.category.c_str());
                if (lowered_category == "game" ||
                    lowered_category == "honorofkings" ||
                    lowered_category == "genshinimpact") {
                    rule.game = true;
                }
            }
            cJSON* features = cJSON_GetObjectItem(item, "features");
            if (features && cJSON_IsObject(features)) {
                cJSON* dynamic_tuning = cJSON_GetObjectItem(features, "dynamic_tuning");
                if (dynamic_tuning && (cJSON_IsBool(dynamic_tuning) || cJSON_IsNumber(dynamic_tuning))) {
                    rule.has_feature_dynamic_tuning = true;
                    rule.feature_dynamic_tuning_enabled = dynamic_tuning->valueint != 0;
                }
                cJSON* fas = cJSON_GetObjectItem(features, "fas");
                if (fas && (cJSON_IsBool(fas) || cJSON_IsNumber(fas))) {
                    rule.has_feature_fas = true;
                    rule.feature_fas_enabled = fas->valueint != 0;
                }
                cJSON* custom_thread = cJSON_GetObjectItem(features, "custom_thread");
                if (custom_thread &&
                    (cJSON_IsBool(custom_thread) || cJSON_IsNumber(custom_thread))) {
                    rule.has_feature_custom_thread = true;
                    rule.feature_custom_thread_enabled = custom_thread->valueint != 0;
                }
                cJSON* dynamic_thread_scheduler =
                    cJSON_GetObjectItem(features, "dynamic_thread_scheduler");
                if (dynamic_thread_scheduler &&
                    (cJSON_IsBool(dynamic_thread_scheduler) || cJSON_IsNumber(dynamic_thread_scheduler))) {
                    rule.has_feature_dynamic_thread_scheduler = true;
                    rule.feature_dynamic_thread_scheduler_enabled =
                        dynamic_thread_scheduler->valueint != 0;
                }
            }
            cJSON* session = cJSON_GetObjectItem(item, "session");
            if (session && cJSON_IsObject(session)) {
                cJSON* learning_duration_ms = cJSON_GetObjectItem(session, "learning_duration_ms");
                cJSON* guard_interval_ms = cJSON_GetObjectItem(session, "guard_interval_ms");
                if (learning_duration_ms && cJSON_IsNumber(learning_duration_ms) &&
                    learning_duration_ms->valueint >= 10000) {
                    rule.session.learning_duration_ms = learning_duration_ms->valueint;
                    rule.has_session = true;
                }
                if (guard_interval_ms && cJSON_IsNumber(guard_interval_ms) &&
                    guard_interval_ms->valueint >= 100) {
                    rule.session.guard_interval_ms = guard_interval_ms->valueint;
                    rule.has_session = true;
                }
            }

            load_string_array_config(item, "packages", rule.packages);
            load_string_array_config(item, "activities", rule.activities);
            load_string_array_config(item, "processes", rule.processes);
            cJSON* default_action = cJSON_GetObjectItem(item, "default_action");
            if (default_action && cJSON_IsObject(default_action)) {
                load_thread_json_action_config(default_action, &rule.default_action);
                rule.has_default_action = rule.default_action.has_clusters ||
                                          rule.default_action.has_cpus ||
                                          rule.default_action.has_nice ||
                                          rule.default_action.has_scheduler;
            }

            if (rule.category.empty()) continue;
            if (rule.packages.empty() && rule.activities.empty() && rule.processes.empty() &&
                !rule.has_default_action) continue;
            g_scene_category_rules.push_back(rule);
        }
    }

    char log_msg[256];
    safe_snprintf(log_msg, sizeof(log_msg),
                  "已加载场景分类配置: %s | rules:%d",
                  SCENE_CATEGORIES_FILE,
                  (int)g_scene_category_rules.size());
    log_config_message(log_msg);

    cleanup_resources(NULL, data, root);
    /* 场景分类规则变了：每应用的生效开关缓存作废 */
    invalidate_runtime_feature_switch_cache();
}
