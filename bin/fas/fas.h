#ifndef FAS_H
#define FAS_H

/*
 * fas.h — FAS 引擎统一头文件
 *
 * 一个 include 即可使用完整 FAS API。
 *
 * 用法:
 *   FasEngine engine;
 *   fas_engine_init(&engine, "/path/to/fas.json");
 *   fas_engine_on_app_change(&engine, "com.example.game", 1234);
 *   // 每帧: fas_engine_push_frame(&engine, interval_ns);
 *   // 每个采样周期: FasOutput out = fas_engine_tick(&engine);
 *   // 落频: 按 out.action_level 查表写 scaling_min/max（见 activity_dynamic_tuning.inc）
 *   fas_engine_destroy(&engine);
 */

#include "fas_types.h"
#include "fas_engine.h"

#endif // FAS_H
