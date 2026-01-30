/*
 * Created: 2024/6/27
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_RHI_COMMON_H
#define MI_RHI_COMMON_H

#include "core/common.h"

#ifndef MI_BYPASS_RHI_THREAD
// Set this macro to true to bypass the RHI thread and execute commands directly on the render thread
#define MI_BYPASS_RHI_THREAD false
#endif

#define CHECK_RHI_THREAD() {assert(IsRHIThread()  || MI_BYPASS_RHI_THREAD);}
#define CHECK_NOT_RHI_THREAD() {assert(!IsRHIThread() || MI_BYPASS_RHI_THREAD);}

MI_NAMESPACE_BEGIN

// nothing

MI_NAMESPACE_END

#endif //MI_RHI_COMMON_H
