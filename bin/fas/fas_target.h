#ifndef FAS_TARGET_H
#define FAS_TARGET_H

#include "fas_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 解析稳定目标帧率
 *
 * 对应原版 resolve_stable_target_fps_locked()
 *
 * 返回 (stable_target_fps, changed)
 *   stable_target_fps: 当前稳定的目标帧率，0=未就绪
 *   changed: 是否发生了升降档切换
 */
void fas_resolve_target_fps(
    FasBufferStateInfo* state,
    const FasConfig* config,
    const char* package,
    int current_fps_long,
    int current_fps_short,
    long long now_ms,
    bool suppress_downshift,
    int* out_target_fps,
    bool* out_changed);

#ifdef __cplusplus
}
#endif

#endif // FAS_TARGET_H
