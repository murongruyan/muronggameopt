#ifndef FAS_CONFIG_H
#define FAS_CONFIG_H

#include "fas_types.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

// 初始化配置为默认值
void fas_config_reset_defaults(FasConfig* cfg);

// 从 JSON 字符串加载配置
bool fas_config_load(FasConfig* cfg, const char* json_str);

// 从文件加载配置
bool fas_config_load_file(FasConfig* cfg, const char* path);

// 查找 package 的 FAS 配置
const FasAppConfig* fas_config_find_app(const FasConfig* cfg, const char* package);

// 获取某个 package 的 margin_fps
float fas_config_margin_fps(const FasConfig* cfg, const char* package);
float fas_config_margin_fps_for_target(const FasConfig* cfg, const char* package, int target_fps);

// 获取某个 package 的帧债灵敏度（原 kp：值越大，同样的帧债判得越重、升频越积极）
float fas_config_debt_sensitivity(const FasConfig* cfg, const char* package);
float fas_config_debt_sensitivity_for_target(const FasConfig* cfg, const char* package, int target_fps);

#ifdef __cplusplus
}
#endif

#endif // FAS_CONFIG_H
