/*
 * Created: 2025/9/24
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_MI_NOISE_H
#define MI_MI_NOISE_H

#include <core/base.h>
#include <vector>
#include <glm/glm.hpp>

MI_NAMESPACE_BEGIN

class NoiseHelpers {
public:
    static float HaltonValue(int index, int base) ;
    static std::vector<glm::vec2> GenerateHaltonSequence2D(int count, int base1 = 2, int base2 = 3) ;

    static float PerlinNoise1D(float x, int repeat = -1) ;
    static float PerlinNoise2D(float x, float y, int repeat = -1) ;

    static std::vector<float> BlueNoiseTexture2D(int width, int height, int seed = 12345) ;

    static std::vector<float> SobolSequence1D(int count) ;

};

MI_NAMESPACE_END

#endif //MI_MI_NOISE_H