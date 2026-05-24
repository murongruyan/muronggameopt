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

static void build_persistent_thread_override_path(char* out, size_t out_size) {
    if (!out || out_size == 0) return;
    safe_snprintf(out, out_size, "%s%s.persistent.json", CPU_CONFIG_DIR, cpu_model);
}

static void load_persistent_thread_override_config_locked() {
    char persistent_override_path[256];
    build_persistent_thread_override_path(persistent_override_path, sizeof(persistent_override_path));
    if (!file_exists(persistent_override_path)) {
        return;
    }

    FILE* pf = fopen(persistent_override_path, "rb");
    if (!pf) {
        return;
    }

    fseek(pf, 0, SEEK_END);
    long plen = ftell(pf);
    fseek(pf, 0, SEEK_SET);
    if (plen <= 0) {
        fclose(pf);
        return;
    }

    char* pdata = (char*)malloc(plen + 1);
    if (!pdata) {
        fclose(pf);
        return;
    }

    size_t pbread = fread(pdata, 1, plen, pf);
    pdata[pbread] = 0;
    fclose(pf);

    cJSON* proot = cJSON_Parse(pdata);
    free(pdata);
    if (!proot) {
        return;
    }

    cJSON* persistent_root = cJSON_GetObjectItem(proot, "apps");
    if (persistent_root && cJSON_IsArray(persistent_root)) {
        cJSON* pitem = NULL;
        cJSON_ArrayForEach(pitem, persistent_root) {
            if (!cJSON_IsObject(pitem)) continue;
            cJSON* scope = cJSON_GetObjectItem(pitem, "scope");
            if (!scope || !cJSON_IsString(scope) || !scope->valuestring) continue;
            std::string scope_value = scheduler_to_lower_copy(scope->valuestring);
            if (scope_value != "persistent") continue;

            cJSON* packages = cJSON_GetObjectItem(pitem, "packages");
            if (!packages || !cJSON_IsArray(packages) || !cJSON_GetArraySize(packages)) continue;
            cJSON* first_package_item = cJSON_GetArrayItem(packages, 0);
            if (!first_package_item || !cJSON_IsString(first_package_item) || !first_package_item->valuestring) {
                continue;
            }
            std::string first_pkg = scheduler_to_lower_copy(first_package_item->valuestring);
            if (first_pkg.empty()) continue;

            for (auto it = g_thread_app_rules.begin(); it != g_thread_app_rules.end(); ) {
                if (!it->packages.empty() && scheduler_to_lower_copy(it->packages[0].c_str()) == first_pkg) {
                    it = g_thread_app_rules.erase(it);
                } else {
                    ++it;
                }
            }

            ThreadJsonAppRule override_rule = {};
            load_thread_json_app_rule_config(pitem, &override_rule);
            if (!override_rule.packages.empty()) {
                g_thread_app_rules.push_back(override_rule);
            }
        }
    }

    cJSON_Delete(proot);

    char log_msg[256];
    safe_snprintf(log_msg, sizeof(log_msg), "已加载设备持久规则覆盖: %s", persistent_override_path);
    log_message(log_msg);
}

static void load_thread_signature_config() {
    std::lock_guard<std::mutex> lock(g_thread_config_mutex);
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
        log_message(error_msg);
        return;
    }

    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    if (len <= 0) {
        log_message("线程签名扩展配置为空或读取失败");
        cleanup_resources(fp, NULL, NULL);
        return;
    }
    fseek(fp, 0, SEEK_SET);

    char* data = (char*)malloc(len + 1);
    if (!data) {
        log_message("内存分配失败");
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
            safe_snprintf(error_msg, sizeof(error_msg), "线程签名扩展JSON解析失败: %s", error_ptr);
            log_message(error_msg);
        }
        cleanup_resources(NULL, data, NULL);
        return;
    }

    cJSON* apps = cJSON_GetObjectItem(root, "apps");
    if (apps && cJSON_IsArray(apps)) {
        cJSON* item = NULL;
        cJSON_ArrayForEach(item, apps) {
            if (!cJSON_IsObject(item)) continue;
            ThreadJsonAppRule app_rule = {};
            load_thread_json_app_rule_config(item, &app_rule);
            if (!app_rule.packages.empty()) {
                g_thread_app_rules.push_back(app_rule);
            }
        }
    }

    load_persistent_thread_override_config_locked();

    char log_msg[320];
    safe_snprintf(log_msg, sizeof(log_msg),
                  "已加载线程扩展配置: %s | apps:%d",
                  config_path,
                  (int)g_thread_app_rules.size());
    log_message(log_msg);

    cleanup_resources(NULL, data, root);
}


void load_scene_categories_config() {
    g_scene_category_rules.clear();
    if (!file_exists(SCENE_CATEGORIES_FILE)) {
        return;
    }

    FILE* fp = fopen(SCENE_CATEGORIES_FILE, "rb");
    if (!fp) {
        char error_msg[256];
        safe_snprintf(error_msg, sizeof(error_msg), "无法打开场景分类配置: %s (错误: %s)",
                      SCENE_CATEGORIES_FILE, strerror(errno));
        log_message(error_msg);
        return;
    }

    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    if (len <= 0) {
        log_message("场景分类配置为空或读取失败");
        cleanup_resources(fp, NULL, NULL);
        return;
    }
    fseek(fp, 0, SEEK_SET);

    char* data = (char*)malloc(len + 1);
    if (!data) {
        log_message("内存分配失败");
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
            log_message(error_msg);
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
    log_message(log_msg);

    cleanup_resources(NULL, data, root);
}
