// Copy pasted from AMD Capsaicin
/**********************************************************************
Copyright (c) 2024 Advanced Micro Devices, Inc. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
********************************************************************/

#ifndef RANDOM_HLSL
#define RANDOM_HLSL

class Random
{
    uint rngState;

    /**
     * Generate a random uint.
     * @return The new random number (range  [0, 2^32)).
     */
    uint randInt()
    {
        // Using PCG hash function
        uint state = rngState;
        rngState = rngState * 747796405u + 2891336453u;
        uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
        word = (word >> 22u) ^ word;
        return word;
    }

    /**
     * Generate a random uint between 0 and a requested maximum value.
     * @return The new random number (range  [0, max)).
     */
    uint randInt(uint max)
    {
        uint ret = randInt() % max; // Has some bias depending on value of max
        return ret;
    }

    /**
     * Generate a random number.
     * @return The new random number (range [0->1.0)).
     */
    float rand()
    {
        // Note: Use the upper 24 bits to avoid a bias due to floating point rounding error.
        float ret = (float)(randInt() >> 8) * 0x1.0p-24f;
        return ret;
    }

    /**
     * Generate 2 random numbers.
     * @return The new numbers (range [0->1.0)).
     */
    float2 rand2()
    {
        return float2(rand(), rand());
    }

    /**
     * Generate 3 random numbers.
     * @return The new numbers (range [0->1.0)).
     */
    float3 rand3()
    {
        return float3(rand(), rand(), rand());
    }
};

/**
 * Make a new random number generator.
 * @param seed The seed value to use when initliasing the random generator.
 * @returns The new random number generator.
 */
Random MakeRandom(uint seed)
{
    Random ret =
    {
        seed * 747796405U + 2891336453u
    };
    return ret;
}

/**
 * Make a new random number generator.
 * @param seed Seed value to initialise random with (e.g. 1D pixel index).
 * @param frame Temporal seed value to initialise random with (e.g. frame number).
 * @return The created random number generator.
 */
Random MakeRandom(uint seed, uint frame)
{
    const uint inc = (frame << 1) | 1U;
    Random ret =
    {
        (seed + inc) * 747796405U + inc
    };
    return ret;
}

// Copy-pasted from UE5. A simple and fast way to get an interleaved gradient noise.

// high frequency dither pattern appearing almost random without banding steps
// note: from "NEXT GENERATION POST PROCESSING IN CALL OF DUTY: ADVANCED WARFARE"
//      http://advances.realtimerendering.com/s2014/index.html
// Epic extended by FrameId
// ~7 ALU operations (2 frac, 3 mad, 2 *)
// @return 0..1
float InterleavedGradientNoise(float2 uv, float FrameId)
{
    // magic values are found by experimentation
    uv += FrameId * (float2(47, 17) * 0.695f);

    const float3 magic = float3(0.06711056f, 0.00583715f, 52.9829189f);
    return frac(magic.z * frac(dot(uv, magic.xy)));
}

#endif