/*
 * Created: 2024/9/4
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MIRENDERER_RUNNABLE_H
#define MIRENDERER_RUNNABLE_H
#include "common.h"
#include "base.h"
#include "conalloc.h"
#include "refcounted.h"
#include "util/lockfree.h"

MI_NAMESPACE_BEGIN

class ThreadRunnable : public NonCopyable, public NonMovable {
public:
    virtual void Run () = 0;
    virtual ~ThreadRunnable () = default;
};

MI_NAMESPACE_END
#endif //MIRENDERER_RUNNABLE_H
