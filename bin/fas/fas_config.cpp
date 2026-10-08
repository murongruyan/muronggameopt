#include "fas_config.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <string>
#include <vector>

extern void log_message(const char* msg);

/*
 * 帧债灵敏度（配置里的 kp）：3.0 是基准，值越大同样的帧债判得越重。
 * 不再换算成"频率比例增益"——目标频率已经不从引擎输出，频率由执行器按动作档位查表。
 */
static float normalize_sensitivity(float kp, float fallback) {
    if (kp <= 0.0f) return fallback;
    return kp;
}

void fas_config_reset_defaults(FasConfig* cfg) {
    if (!cfg) return;
    cfg->default_target_fps = 60;
    cfg->base_margin_fps = 1.0f;
    cfg->default_kp = 3.0f;
    cfg->app_count = 0;
    memset(cfg->apps, 0, sizeof(cfg->apps));
}

static float json_get_float(cJSON* obj, const char* key, float def) {
    cJSON* item = cJSON_GetObjectItem(obj, key);
    if (item && cJSON_IsNumber(item)) return (float)item->valuedouble;
    return def;
}

static int json_get_int(cJSON* obj, const char* key, int def) {
    cJSON* item = cJSON_GetObjectItem(obj, key);
    if (item && cJSON_IsNumber(item)) return item->valueint;
    return def;
}

static bool json_get_bool(cJSON* obj, const char* key, bool def) {
    cJSON* item = cJSON_GetObjectItem(obj, key);
    if (item && (cJSON_IsBool(item) || cJSON_IsNumber(item))) return item->valueint != 0;
    return def;
}

static void collect_packages(cJSON* app, std::vector<std::string>* out) {
    if (!app || !out) return;
    out->clear();

    cJSON* pkgs = cJSON_GetObjectItem(app, "packages");
    if (pkgs && cJSON_IsArray(pkgs)) {
        cJSON* item = NULL;
        cJSON_ArrayForEach(item, pkgs) {
            if (cJSON_IsString(item) && item->valuestring) {
                std::string s = item->valuestring;
                while (!s.empty() && (s[0] == ' ' || s[0] == '\t')) s.erase(0, 1);
                while (!s.empty() && (s[s.size() - 1] == ' ' || s[s.size() - 1] == '\t')) s.pop_back();
                if (!s.empty() &&
                    std::find(out->begin(), out->end(), s) == out->end()) {
                    out->push_back(s);
                }
            }
        }
    }

    cJSON* pkg = cJSON_GetObjectItem(app, "package");
    if (pkg && cJSON_IsString(pkg) && pkg->valuestring) {
        char* dup = strdup(pkg->valuestring);
        if (dup) {
            char* saveptr = NULL;
            char* token = strtok_r(dup, ",", &saveptr);
            while (token) {
                char* start = token;
                while (*start == ' ' || *start == '\t') start++;
                char* end = start + strlen(start) - 1;
                while (end > start && (*end == ' ' || *end == '\t')) end--;
                *(end + 1) = '\0';
                if (strlen(start) > 0) {
                    std::string s(start);
                    if (std::find(out->begin(), out->end(), s) == out->end()) {
                        out->push_back(s);
                    }
                }
                token = strtok_r(NULL, ",", &saveptr);
            }
            free(dup);
        }
    }
}

static void set_app_entry(FasAppConfig* ac,
    const std::string& package,
    const char* friendly,
    int target_fps,
    float margin_fps,
    float kp,
    bool fallback)
{
    if (!ac) return;
    memset(ac, 0, sizeof(*ac));
    strncpy(ac->package, package.c_str(), sizeof(ac->package) - 1);
    ac->package[sizeof(ac->package) - 1] = '\0';
    if (friendly && friendly[0]) {
        strncpy(ac->friendly, friendly, sizeof(ac->friendly) - 1);
        ac->friendly[sizeof(ac->friendly) - 1] = '\0';
    }
    ac->target_fps = target_fps;
    ac->margin_fps = margin_fps;
    ac->kp = kp;
    ac->fallback = fallback ? 1 : 0;
}

bool fas_config_load(FasConfig* cfg, const char* json_str) {
    if (!cfg || !json_str) return false;

    fas_config_reset_defaults(cfg);

    cJSON* root = cJSON_Parse(json_str);
    if (!root) {
        log_message("fas.json 解析失败");
        return false;
    }

    cfg->default_target_fps = json_get_int(root, "default_target_fps", cfg->default_target_fps);
    cfg->base_margin_fps = json_get_float(root, "base_margin_fps", cfg->base_margin_fps);
    cfg->default_kp = normalize_sensitivity(
        json_get_float(root, "default_kp", cfg->default_kp),
        cfg->default_kp);

    cJSON* defaults = cJSON_GetObjectItem(root, "defaults");
    if (defaults && cJSON_IsObject(defaults)) {
        cfg->default_target_fps =
            json_get_int(defaults, "default_target_fps", cfg->default_target_fps);
        cfg->base_margin_fps =
            json_get_float(defaults, "base_margin_fps", cfg->base_margin_fps);
        cfg->default_kp = normalize_sensitivity(
            json_get_float(defaults, "kp", cfg->default_kp),
            cfg->default_kp);

    }

    bool truncated = false;
    cJSON* apps = cJSON_GetObjectItem(root, "apps");
    if (apps && cJSON_IsArray(apps)) {
        cJSON* app = NULL;
        cJSON_ArrayForEach(app, apps) {
            if (!cJSON_IsObject(app)) continue;
            if (cfg->app_count >= FAS_MAX_FAS_APPS) {
                truncated = true;
                break;
            }

            std::vector<std::string> pkgs;
            collect_packages(app, &pkgs);
            if (pkgs.empty()) continue;

            cJSON* friendly = cJSON_GetObjectItem(app, "friendly");
            const char* friendly_str =
                (friendly && cJSON_IsString(friendly) && friendly->valuestring) ? friendly->valuestring : "";

            cJSON* fps_profiles = cJSON_GetObjectItem(app, "fps_profiles");
            if (fps_profiles && cJSON_IsArray(fps_profiles)) {
                cJSON* profile = NULL;
                cJSON_ArrayForEach(profile, fps_profiles) {
                    if (!cJSON_IsObject(profile)) continue;
                    int target_fps = json_get_int(profile, "target_fps", cfg->default_target_fps);
                    float margin_fps = json_get_float(profile, "margin_fps", -1.0f);
                    float kp = normalize_sensitivity(
                        json_get_float(profile, "kp", cfg->default_kp),
                        cfg->default_kp);
                    bool fallback = json_get_bool(profile, "fallback", false);

                    for (size_t i = 0; i < pkgs.size() && cfg->app_count < FAS_MAX_FAS_APPS; i++) {
                        set_app_entry(
                            &cfg->apps[cfg->app_count++],
                            pkgs[i],
                            friendly_str,
                            target_fps,
                            margin_fps,
                            kp,
                            fallback);
                    }
                    if (cfg->app_count >= FAS_MAX_FAS_APPS) truncated = true;
                }
                continue;
            }

            int target_fps = json_get_int(app, "target_fps", cfg->default_target_fps);
            float margin_fps = json_get_float(app, "margin_fps", -1.0f);
            float kp = normalize_sensitivity(
                json_get_float(app, "kp", cfg->default_kp),
                cfg->default_kp);
            bool fallback = json_get_bool(app, "fallback", false);

            for (size_t i = 0; i < pkgs.size() && cfg->app_count < FAS_MAX_FAS_APPS; i++) {
                set_app_entry(
                    &cfg->apps[cfg->app_count++],
                    pkgs[i],
                    friendly_str,
                    target_fps,
                    margin_fps,
                    kp,
                    fallback);
            }
            if (cfg->app_count >= FAS_MAX_FAS_APPS) truncated = true;
        }
    }

    cJSON_Delete(root);
    if (truncated) {
        log_message("fas.json 应用配置超过容量上限，后续包名已被截断");
    }
    return true;
}

bool fas_config_load_file(FasConfig* cfg, const char* path) {
    if (!cfg || !path) return false;

    FILE* fp = fopen(path, "r");
    if (!fp) {
        char msg[256];
        snprintf(msg, sizeof(msg), "fas_config: 无法打开 %s", path);
        log_message(msg);
        return false;
    }

    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (len <= 0) { fclose(fp); return false; }

    char* buf = (char*)malloc(len + 1);
    if (!buf) { fclose(fp); return false; }

    size_t n = fread(buf, 1, len, fp);
    fclose(fp);
    buf[n] = '\0';

    bool ok = fas_config_load(cfg, buf);
    free(buf);
    return ok;
}

const FasAppConfig* fas_config_find_app(const FasConfig* cfg, const char* package) {
    if (!cfg || !package) return NULL;
    for (int i = 0; i < cfg->app_count; i++) {
        if (strcmp(cfg->apps[i].package, package) == 0) {
            return &cfg->apps[i];
        }
    }
    return NULL;
}

static const FasAppConfig* find_profile(const FasConfig* cfg, const char* package, int target_fps) {
    if (!cfg || !package) return NULL;
    const FasAppConfig* fallback = NULL;
    for (int i = 0; i < cfg->app_count; i++) {
        const FasAppConfig* ac = &cfg->apps[i];
        if (strcmp(ac->package, package) != 0) continue;
        if (target_fps > 0 && ac->target_fps == target_fps) return ac;
        if (!fallback || ac->fallback) fallback = ac;
    }
    return fallback;
}

float fas_config_margin_fps(const FasConfig* cfg, const char* package) {
    return fas_config_margin_fps_for_target(cfg, package, 0);
}

float fas_config_margin_fps_for_target(const FasConfig* cfg, const char* package, int target_fps) {
    if (!cfg) return 1.0f;
    const FasAppConfig* ac = find_profile(cfg, package, target_fps);
    if (ac && ac->margin_fps > 0.0f) return ac->margin_fps;
    return cfg->base_margin_fps > 0.0f ? cfg->base_margin_fps : 1.0f;
}

float fas_config_debt_sensitivity(const FasConfig* cfg, const char* package) {
    return fas_config_debt_sensitivity_for_target(cfg, package, 0);
}

float fas_config_debt_sensitivity_for_target(const FasConfig* cfg, const char* package, int target_fps) {
    if (!cfg) return 3.0f;
    const FasAppConfig* ac = find_profile(cfg, package, target_fps);
    float kp = (ac && ac->kp > 0.0f) ? ac->kp : cfg->default_kp;
    return normalize_sensitivity(kp, 3.0f);
}
