/*
 * Created: 2025/5/4
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef R_INTERNAL_COMMON_H
#define R_INTERNAL_COMMON_H

#include <rdg/rdg_builder.h>
#include <rdg/rdg_cmd.h>
#include <rdg/rdg_shader.h>

#include "renderer/mi_renderer.h"
#include "renderer/mi_renderer_view.h"

#ifndef NDEBUG
#define DEBUG_UBER_BARRIER queue.MemoryBarrier(RHIPipelineStageFlagBits::kAll, RHIPipelineStageFlagBits::kAll, RHIGPUAccessFlagBits::kAll, RHIGPUAccessFlagBits::kAll);
#else
#define DEBUG_UBER_BARRIER
#endif

#endif //R_INTERNAL_COMMON_H
