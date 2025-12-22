#ifndef GAUSSIAN_POINT_CLOUD_H
#define GAUSSIAN_POINT_CLOUD_H
#include "Transform.hlsl"
#include "../shared/SharedGaussianRadianceField.hlsl"

// TanFoV: 2 * tan(fov / 2)
// @return The first two rows of Jacobian Matrix.
float3x3 EWAJacobian2 (float3 Mean, float2 TanFoV, float4x4 View) {
    float3 P = mul(View, float4(Mean, 1.0)).xyz;
    const float limx = 0.65f * TanFoV.x;
    const float limy = 0.65f * TanFoV.y;
    const float txtz = P.x / P.z;
    const float tytz = P.y / P.z;
    // Clamp to (fov expanded) frustum
    P.x = clamp(txtz, -limx, limx) * P.z;
    P.y = clamp(tytz, -limy, limy) * P.z;

    float3x3 J = float3x3(
        (2 / TanFoV.x) / P.z, 0.0f, - (2 / TanFoV.x) * P.x / (P.z * P.z),
        0.0f, (2 / TanFoV.y) / P.z, - (2 / TanFoV.y) * P.y / (P.z * P.z),
        0.0f, 0.0f, 0.0f);
    return J;
}

struct SymmetricMatrix3D {
    float3 Diagonal;
    float3 OffDiagonal;
};

float3x3 ExpandSymmetricMatrix3D (SymmetricMatrix3D C) {
    float3x3 MC = float3x3(
        C.Diagonal.x, C.OffDiagonal.x, C.OffDiagonal.y,
        C.OffDiagonal.x, C.Diagonal.y, C.OffDiagonal.z,
        C.OffDiagonal.y, C.OffDiagonal.z, C.Diagonal.z
    );
    return MC;
}

float3 ProjectCovarianceMatrixToNDC(float3x3 J, SymmetricMatrix3D Covariance3D, float4x4 View)
{
    float3x3 W = float3x3(
        View[0][0], View[0][1], View[0][2],
        View[1][0], View[1][1], View[1][2],
        View[2][0], View[2][1], View[2][2]);

    float3x3 Mk = mul(J, W);
    
    float3x3 C = ExpandSymmetricMatrix3D(Covariance3D);
    float3x3 C_2D = mul(mul(Mk, C), transpose(Mk));

    return float3(C_2D[0][0], C_2D[0][1], C_2D[1][1]);
}

SymmetricMatrix3D ComputeCovarianceMatrix (float3 Scale, float4 Rotation) {
    float3x3 M = BuildRotationScaleMatrix(Rotation, Scale);
    // Covariance matrixs
    float3x3 Covariance = mul(M, transpose(M));

    float3 Diagonal = float3(
        Covariance[0][0],
        Covariance[1][1],
        Covariance[2][2]
    );
    float3 OffDiagonal = float3(
        Covariance[0][1],
        Covariance[0][2],
        Covariance[1][2]
    );
    SymmetricMatrix3D Ret = (SymmetricMatrix3D)0;
    Ret.Diagonal = Diagonal;
    Ret.OffDiagonal = OffDiagonal;
    return Ret;
}

SymmetricMatrix3D ComputeCovarianceMatrix (float3 Scale, float4 Rotation, float3x3 InstanceRotationScale) {
    float3x3 M = mul(InstanceRotationScale, BuildRotationScaleMatrix(Rotation, Scale));
    // Covariance matrixs
    float3x3 Covariance = mul(M, transpose(M));

    float3 Diagonal = float3(
        Covariance[0][0],
        Covariance[1][1],
        Covariance[2][2]
    );
    float3 OffDiagonal = float3(
        Covariance[0][1],
        Covariance[0][2],
        Covariance[1][2]
    );
    SymmetricMatrix3D Ret = (SymmetricMatrix3D)0;
    Ret.Diagonal = Diagonal;
    Ret.OffDiagonal = OffDiagonal;
    return Ret;
}


float3 ProjectCovarianceMatrixToRaySpace(float3x3 J, SymmetricMatrix3D Covariance3D, float3x3 RaySpace)
{
    float3x3 W = float3x3(
        RaySpace[0][0], RaySpace[0][1], RaySpace[0][2],
        RaySpace[1][0], RaySpace[1][1], RaySpace[1][2],
        RaySpace[2][0], RaySpace[2][1], RaySpace[2][2]);

    float3x3 Mk = mul(J, W);
    
    float3x3 C = ExpandSymmetricMatrix3D(Covariance3D);
    float3x3 C_2D = mul(mul(Mk, C), transpose(Mk));

    return float3(C_2D[0][0], C_2D[0][1], C_2D[1][1]);
}

// Evaluate the gaussian rasterization-response along the ray
float EvaluateGaussianResponseRast (float3 Origin, float3x3 RaySpace, Gaussian3D G, out float RayRastResponseT) {
    float3x3 J = float3x3(
        1, 0, 0,
        0, 1, 0,
        0, 0, 0
    );
    // Instance local covariance 3D
    SymmetricMatrix3D Cov3D = ComputeCovarianceMatrix(G.Scales, G.Rotation);
    float3 Cov2D   = ProjectCovarianceMatrixToRaySpace(J, Cov3D, RaySpace);
    // Cov2D = float3(0.001, 0, 0.001);
    float3 RaySpacePosition = mul(RaySpace, G.Position - Origin);
    RayRastResponseT = RaySpacePosition.z;
    float  Det    = Cov2D.x * Cov2D.z - Cov2D.y * Cov2D.y;
    float2x2 InvCov2D = float2x2(
        Cov2D.z / Det, -Cov2D.y / Det,
        -Cov2D.y / Det, Cov2D.x / Det
    );
    float Dist = dot(RaySpacePosition.xy, mul(InvCov2D, RaySpacePosition.xy));
    // Unnormalized 2D gaussian distribution, similar to the rasterizer.
    return exp(-0.5 * Dist) /*/ sqrt(Det)*/ * G.Opacity;
}

float Evaluate2DUnnormalizedGaussian (float2 P) {
    // float NormalizationFactor = 1 / (2 * PI);
    return exp(-dot(P, P) / 2);
}

float Evaluate2DGaussian (float2 P) {
    float NormalizationFactor = 1 / (2 * PI);
    return NormalizationFactor * exp(-dot(P, P) / 2);
}

#endif // GAUSSIAN_POINT_CLOUD_H