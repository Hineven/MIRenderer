/*
 * Created: 2025/4/19
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "renderer/mi_renderer_view.h"

MI_NAMESPACE_BEGIN

RendererView::RendererView(RDGPool *pool, uint32_t width, uint32_t height, World *world) {
    pool_ = pool;
    film_width_ = width;
}

RendererView::~RendererView() {

}



MI_NAMESPACE_END