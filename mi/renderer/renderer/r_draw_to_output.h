/*
 * Created: 2025/5/4
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef R_DRAW_TO_OUTPUT_H
#define R_DRAW_TO_OUTPUT_H

#include "r_internal_common.h"

MI_NAMESPACE_BEGIN

BEGIN_SHADER_PARAMETERS(DrawToOutputPass)
    SHADER_RENDER_TARGET(PixelFormatType::kR8G8B8A8_UNORM, Output)
END_SHADER_PARAMETERS()

MI_NAMESPACE_END

#endif //R_DRAW_TO_OUTPUT_H
