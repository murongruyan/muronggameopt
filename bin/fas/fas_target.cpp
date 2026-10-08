#include "fas_target.h"
#include <cstring>
#include <cstdlib>
#include <algorithm>

/*
 * 档位切换的保持时间。
 *
 * 目标帧率来自"游戏当前跑在哪一档"，但游戏掉帧和游戏改档在短窗口里看起来是一样的。
 * 以前降档只要 900ms、掉档只要 1200ms，于是加载/重场景的瞬时掉帧会把目标从 144
 * 一路判成 60；目标一变引擎就清空帧缓冲并重新暖机，FAS 反复进入"不可用"，
 * 动态调频趁机接管，形成来回抖动。
 *
 * 现在只有游戏真的持续跑在另一档（改设置、切模式）才换档，瞬时掉帧保持原目标，
 * 由帧债机制去处理。
 */
static const long long kTargetAcquireHoldMs = 400;
static const long long kTargetUpshiftHoldMs = 700;
static const long long kTargetDownshiftHoldMs = 3000;
static const long long kTargetDropHoldMs = 3000;

/* 保持当前档位的容忍度：目标帧率的 1/12（144→12fps，60→5fps） */
static int keep_current_floor_fps(int stable_target_fps) {
    if (stable_target_fps <= 0) return 0;
    int margin = stable_target_fps / 12;
    if (margin <= 0) margin = 1;
    return std::max(stable_target_fps - margin, 10);
}

// ── 辅助: 获取某 package 的目标帧率列表（升序）──

static int get_targets(const FasConfig* config, const char* package,
                       int* out, int max_out) {
    if (!config || !package || !out) return 0;
    int count = 0;
    for (int i = 0; i < config->app_count && count < max_out; i++) {
        if (strcmp(config->apps[i].package, package) == 0 && config->apps[i].target_fps > 0) {
            out[count++] = config->apps[i].target_fps;
        }
    }
    std::sort(out, out + count);
    return count;
}

static int dedup_sorted_targets(int* targets, int count) {
    if (!targets || count <= 0) return 0;
    int write = 1;
    for (int i = 1; i < count; i++) {
        if (targets[i] != targets[write - 1]) {
            targets[write++] = targets[i];
        }
    }
    return write;
}

static int resolve_instant_target_fps(const int* targets, int target_count, int current_fps_long) {
    if (!targets || target_count <= 0) return 0;

    int resolved_target_fps = 0;
    int min_required_fps = std::max(targets[0] - 10, 10);
    if (current_fps_long >= min_required_fps) {
        resolved_target_fps = targets[target_count - 1];
        for (int i = 0; i < target_count; i++) {
            if (current_fps_long <= targets[i] + 3) {
                resolved_target_fps = targets[i];
                break;
            }
        }
    }
    return resolved_target_fps;
}

static long long target_change_hold_ms(int stable_target_fps, int candidate_target_fps) {
    if (candidate_target_fps <= 0) return kTargetDropHoldMs;
    if (stable_target_fps <= 0) return kTargetAcquireHoldMs;
    if (candidate_target_fps > stable_target_fps) return kTargetUpshiftHoldMs;
    if (candidate_target_fps < stable_target_fps) return kTargetDownshiftHoldMs;
    return 0;
}

// ── 主函数 ─────────────────────────────────────────────────

void fas_resolve_target_fps(
    FasBufferStateInfo* state,
    const FasConfig* config,
    const char* package,
    int current_fps_long,
    int current_fps_short,
    long long now_ms,
    bool suppress_downshift,
    int* out_target_fps,
    bool* out_changed)
{
    if (out_target_fps) *out_target_fps = 0;
    if (out_changed) *out_changed = false;
    if (!state || !config || !package || !package[0]) return;

    int targets[FAS_MAX_FAS_APPS];
    int target_count = get_targets(config, package, targets, FAS_MAX_FAS_APPS);
    target_count = dedup_sorted_targets(targets, target_count);

    if (target_count <= 0) return;

    int stable_target_fps = state->stable_target_fps;
    int candidate_target_fps = resolve_instant_target_fps(targets, target_count, current_fps_long);

    // long-window 更适合定档；短窗口只用来证明"当前档还没崩"，避免瞬时抖动换档。
    if (stable_target_fps > 0 && current_fps_short > 0) {
        int keep_floor = keep_current_floor_fps(stable_target_fps);
        if (candidate_target_fps <= 0 && current_fps_short >= keep_floor) {
            candidate_target_fps = stable_target_fps;
        } else if (!suppress_downshift &&
                   candidate_target_fps > 0 &&
                   candidate_target_fps < stable_target_fps &&
                   current_fps_short >= keep_floor) {
            candidate_target_fps = stable_target_fps;
        }
    }

    int resolved_target_fps = stable_target_fps;
    if (candidate_target_fps == stable_target_fps) {
        state->pending_target_fps = 0;
        state->pending_target_since_ms = 0;
    } else {
        if (state->pending_target_fps != candidate_target_fps) {
            state->pending_target_fps = candidate_target_fps;
            state->pending_target_since_ms = now_ms;
        }

        long long hold_ms = target_change_hold_ms(stable_target_fps, candidate_target_fps);
        if (hold_ms <= 0 ||
            (state->pending_target_since_ms > 0 && now_ms - state->pending_target_since_ms >= hold_ms)) {
            resolved_target_fps = candidate_target_fps;
            state->pending_target_fps = 0;
            state->pending_target_since_ms = 0;
        }
    }

    bool changed = resolved_target_fps != stable_target_fps;
    state->stable_target_fps = resolved_target_fps;

    if (out_target_fps) *out_target_fps = state->stable_target_fps;
    if (out_changed) *out_changed = changed;
}
