/*
 * Created: 2024/6/27
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_RHI_COMMON_H
#define MI_RHI_COMMON_H

#include "core/common.h"

#ifndef NDEBUG
// Set this macro to true to bypass the RHI thread and execute commands directly on the render thread
#define BYPASS_RHI_THREAD false
#else
#define BYPASS_RHI_THREAD false
#endif

#define CHECK_RHI_THREAD() {assert(IsRHIThread()  || BYPASS_RHI_THREAD);}
#define CHECK_NOT_RHI_THREAD() {assert(!IsRHIThread() || BYPASS_RHI_THREAD);}

MI_NAMESPACE_BEGIN

// nothing

MI_NAMESPACE_END

#endif //MI_RHI_COMMON_H
