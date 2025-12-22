/*
 * Created: 2024/9/15
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "rdg/rdg_base.h"
MI_NAMESPACE_BEGIN

const char *ToCString(RDGPassType type) {
    static const char * names[] = {
        "Graphics",
        "Compute",
        "Generic",
    };
    return names[(int)type];
}


MI_NAMESPACE_END