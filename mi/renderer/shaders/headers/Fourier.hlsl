#ifndef FOURIER_HLSL
#define FOURIER_HLSL

#define MAX_FOURIER_ORDER 8

struct FourierFloat {
    // [l, r] -> [0, PI]
    float l, r;
    uint fourier_order;
    float fourier_a[MAX_FOURIER_ORDER];
    float fourier_b[MAX_FOURIER_ORDER]; // b[0] = 0
};

struct FourierFloat3 {
    // [l, r] -> [0, PI]
    float l, r;
    uint fourier_order;
    float3 fourier_a[MAX_FOURIER_ORDER];
    float3 fourier_b[MAX_FOURIER_ORDER]; // b[0] = 0
};

// Get Fourier series value at position x
float ComputeFourierValue(FourierFloat distr, float x) {
    float result = distr.fourier_a[0] * 0.5f;

    float l = distr.l;
    float r = distr.r;
    float u = PI * (x - l) / max(r - l, 1e-6);
    for(uint i = 1; i <= distr.fourier_order; i++) {
        float n_u = i * u;
        result += distr.fourier_a[i] * cos(n_u) + distr.fourier_b[i] * sin(n_u);
    }
    return result;
}

float3 ComputeFourierValue(FourierFloat3 distr, float x) {
    float3 result = distr.fourier_a[0] * 0.5f;

    float l = distr.l;
    float r = distr.r;
    float u = PI * (x - l) / max(r - l, 1e-6);
    for(uint i = 1; i <= distr.fourier_order; i++) {
        float n_u = i * u;
        result += distr.fourier_a[i] * cos(n_u) + distr.fourier_b[i] * sin(n_u);
    }
    return result;
}

// Get Fourier gradient at position x
float ComputeFourierGradient(FourierFloat distr, float x) {
    float result = 0.f;

    float l = distr.l;
    float r = distr.r;
    float half_period = r - l;
    float omega = PI / half_period;
    for(uint i = 1; i <= distr.fourier_order; i++) {
        float n_omega = i * omega;
        float n_omega_u = n_omega * (x - l);
        result += (-distr.fourier_a[i] * n_omega * sin(n_omega_u));
        result += distr.fourier_b[i] * n_omega * cos(n_omega_u);
    }
    return result;
}

float3 ComputeFourierGradient(FourierFloat3 distr, float x) {
    float3 result = float3(0.f, 0.f, 0.f);

    float l = distr.l;
    float r = distr.r;
    float half_period = r - l;
    float omega = PI / half_period;
    for(uint i = 1; i <= distr.fourier_order; i++) {
        float n_omega = i * omega;
        float n_omega_u = n_omega * (x - l);
        result += (-distr.fourier_a[i] * n_omega * sin(n_omega_u));
        result += distr.fourier_b[i] * n_omega * cos(n_omega_u);
    }
    return result;
}

// Get Fourier integral at [l, x]
float ComputeFourierIntegral(FourierFloat distr, float x) {
    float result = 0.f;

    float l = distr.l;
    float r = distr.r;
    float omega = PI / (r - l);

    result = (distr.fourier_a[0]) * 0.5f * (x - l);
    for(int i = 1; i <= distr.fourier_order; i++) {
        float n_omega = i * omega;
        float n_omega_inv = 1.f / n_omega;
        float u = n_omega * (x - l);
        result += distr.fourier_a[i] * n_omega_inv * sin(u);
        result += distr.fourier_b[i] * n_omega_inv * (1.f - cos(u));
    }
    return result;
}

float3 ComputeFourierIntegral(FourierFloat3 distr, float x) {
    float3 result = float3(0.f, 0.f, 0.f);

    float l = distr.l;
    float r = distr.r;
    float omega = PI / (r - l);

    result = (distr.fourier_a[0]) * 0.5f * (x - l);
    for(int i = 1; i <= distr.fourier_order; i++) {
        float n_omega = i * omega;
        float n_omega_inv = 1.f / n_omega;
        float u = n_omega * (x - l);
        result += distr.fourier_a[i] * n_omega_inv * sin(u);
        result += distr.fourier_b[i] * n_omega_inv * (1.f - cos(u));
    }
    return result;
}

#endif