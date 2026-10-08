#ifndef FAS_BRIDGE_THREAD_H
#define FAS_BRIDGE_THREAD_H

/*
 * fas/bridge_thread.h —— 纯线程版（-DMURONG_THREAD_ONLY）专用桥接头。
 *
 * 与 fas/bridge.h 的唯一区别：这里【不编译】FAS 的 4 个实现单元。
 *
 * 说明：daemon 里有一句具体的对象定义
 *     FasEngine g_fas_engine;
 *   （activity_diaodu.cpp:372），它需要 FasEngine 的【完整定义】，
 *   因此这里仍要包含 fas_engine.h —— 该头文件只声明类型与 API，不产生任何实现。
 *
 * 纯线程版【不参与编译】的单元：
 *     fas/fas_engine.cpp、fas/fas_config.cpp、fas/fas_sampling.cpp、fas/fas_target.cpp
 * 由 fas/fas_thread_stub.cpp 提供同名符号的空实现：
 *     生命周期函数为空操作，fas_engine_tick() 返回全零 FasOutput（= 无 FAS 信号）。
 * 另外 activity_fas_sampling.inc / activity_dyn_config.inc 也换成 *_thread.inc 桩。
 *
 * 函数签名与 fas_engine.h 完全一致 —— 主循环与其它 .inc 文件一行都不用改。
 */

#include "fas/fas_types.h"
#include "fas/fas_engine.h"

extern FasEngine g_fas_engine;
extern bool g_fas_engine_inited;

#endif /* FAS_BRIDGE_THREAD_H */
