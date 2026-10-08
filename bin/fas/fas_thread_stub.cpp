/*
 * fas_thread_stub.cpp —— 纯线程版（-DMURONG_THREAD_ONLY）的 FAS 引擎桩实现。
 *
 * 只在纯线程版构建里参与编译，替代 fas/fas_engine.cpp、fas_config.cpp、
 * fas_sampling.cpp、fas_target.cpp 四个实现单元。
 *
 * 语义：生命周期函数为空操作；tick 返回全零的 FasOutput（等价于"没有 FAS 信号"），
 *       因此 FAS 的动作档位恒为最低档，动态调频也已被桩关闭 —— 调度只剩线程侧。
 *
 * 产物自检：纯线程版二进制里不应出现 fas_engine / fas_config / fas_sampling /
 *           fas_target / fas_predictor 等符号（除本文件导出的这几个空桩）。
 */

#include "fas/bridge_thread.h"

#include <string.h>

static FasOutput make_empty_output() {
    FasOutput out;
    memset(&out, 0, sizeof(out));
    return out;
}

extern "C" {

bool fas_engine_init(FasEngine* e, const char* config_path) {
    (void)e;
    (void)config_path;
    return false;   /* 纯线程版：引擎永不初始化 */
}

void fas_engine_destroy(FasEngine* e) {
    (void)e;
}

void fas_engine_push_frame(FasEngine* e, long long interval_ns) {
    (void)e;
    (void)interval_ns;
}

void fas_engine_on_app_change(FasEngine* e, const char* package, int pid) {
    (void)e;
    (void)package;
    (void)pid;
}

void fas_engine_set_ladder(FasEngine* e, const FasLadderConfig* ladder) {
    (void)e;
    (void)ladder;
}

FasOutput fas_engine_tick(FasEngine* e) {
    (void)e;
    return make_empty_output();
}

} /* extern "C" */
