#ifndef FAS_BRIDGE_H
#define FAS_BRIDGE_H

/*
 * fas/bridge.h -- FAS bridge header
 *
 * Included at the top of activity_diaodu.cpp.
 * Provides FAS types + engine extern declarations to all .inc files.
 */

#include "fas/fas_types.h"
#include "fas/fas_engine.h"

extern FasEngine g_fas_engine;
extern bool g_fas_engine_inited;

#endif
