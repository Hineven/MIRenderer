#!/usr/bin/env python3
"""
Fit piecewise-linear constants for:
    LightGrid_EstimateLightGridPerceptualContribution_ClusterNormalVarianceToLerpFactor(var)

Model assumptions:
- var is computed from intensity-normalized first/second moments of normals:
    m1 = E[n]
    m2 = E[n*n]     (component-wise)
    var_metric = ||m2 - m1*m1||^2
- Target lerp factor t is solved per synthetic cluster by least-squares over random query directions:
    lerp(approx_cosine, 1/pi, t) ~= true_cluster_average_cosine

This script outputs:
1) A 2-point linear fit (through origin, capped at 1)
2) A 3-point piecewise-linear fit (0,0)->(knee_var,knee_t)->(v1,1)
"""

from __future__ import annotations

import argparse
import math
import random
from dataclasses import dataclass
from typing import Iterable, List, Sequence, Tuple

Vec3 = Tuple[float, float, float]


def v_add(a: Vec3, b: Vec3) -> Vec3:
    return (a[0] + b[0], a[1] + b[1], a[2] + b[2])


def v_sub(a: Vec3, b: Vec3) -> Vec3:
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def v_mul(a: Vec3, s: float) -> Vec3:
    return (a[0] * s, a[1] * s, a[2] * s)


def v_dot(a: Vec3, b: Vec3) -> float:
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def v_len(a: Vec3) -> float:
    return math.sqrt(max(v_dot(a, a), 0.0))


def v_norm(a: Vec3) -> Vec3:
    l = v_len(a)
    if l <= 1e-20:
        return (0.0, 0.0, 0.0)
    inv = 1.0 / l
    return (a[0] * inv, a[1] * inv, a[2] * inv)


def v_square(a: Vec3) -> Vec3:
    return (a[0] * a[0], a[1] * a[1], a[2] * a[2])


def v_hadamard(a: Vec3, b: Vec3) -> Vec3:
    return (a[0] * b[0], a[1] * b[1], a[2] * b[2])


def saturate(x: float) -> float:
    return max(0.0, min(1.0, x))


def sample_uniform_sphere(rng: random.Random) -> Vec3:
    z = 1.0 - 2.0 * rng.random()
    phi = 2.0 * math.pi * rng.random()
    r = math.sqrt(max(0.0, 1.0 - z * z))
    return (r * math.cos(phi), r * math.sin(phi), z)


def sample_cosine_power_hemisphere_z(rng: random.Random, k: float) -> Vec3:
    # pdf proportional to cos(theta)^k over +Z hemisphere.
    z = rng.random() ** (1.0 / (k + 1.0))
    phi = 2.0 * math.pi * rng.random()
    r = math.sqrt(max(0.0, 1.0 - z * z))
    return (r * math.cos(phi), r * math.sin(phi), z)


def sample_cluster_normal(rng: random.Random) -> Vec3:
    # Mixture families to cover concentrated, broad, and bimodal cases.
    family = rng.random()

    if family < 0.55:
        # Single lobe around +Z
        k = math.exp(rng.uniform(math.log(1e-3), math.log(256.0)))
        return sample_cosine_power_hemisphere_z(rng, k)

    if family < 0.9:
        # Two opposite lobes: choose +Z/-Z lobe.
        k = math.exp(rng.uniform(math.log(1e-3), math.log(64.0)))
        p_plus = rng.uniform(0.1, 0.9)
        n = sample_cosine_power_hemisphere_z(rng, k)
        if rng.random() <= p_plus:
            return n
        return (n[0], n[1], -n[2])

    # Uniform sphere tail cases.
    return sample_uniform_sphere(rng)


@dataclass
class ClusterStats:
    var_metric: float
    t_opt: float


def compute_cluster_var_metric(normals: Sequence[Vec3], weights: Sequence[float]) -> float:
    w_sum = sum(weights)
    if w_sum <= 1e-20:
        return 0.0

    inv_w_sum = 1.0 / w_sum
    m1 = (0.0, 0.0, 0.0)
    m2 = (0.0, 0.0, 0.0)

    for n, w in zip(normals, weights):
        wn = w * inv_w_sum
        m1 = v_add(m1, v_mul(n, wn))
        m2 = v_add(m2, v_mul(v_square(n), wn))

    diff = v_sub(m2, v_hadamard(m1, m1))
    return v_dot(diff, diff)


def solve_optimal_t(normals: Sequence[Vec3], weights: Sequence[float], query_dirs: Sequence[Vec3]) -> float:
    w_sum = sum(weights)
    if w_sum <= 1e-20:
        return 0.0

    inv_w_sum = 1.0 / w_sum
    m1 = (0.0, 0.0, 0.0)
    for n, w in zip(normals, weights):
        m1 = v_add(m1, v_mul(n, w * inv_w_sum))

    mean_normal = v_norm(m1)
    dst = 1.0 / math.pi

    num = 0.0
    den = 0.0

    for d in query_dirs:
        true_avg = 0.0
        for n, w in zip(normals, weights):
            true_avg += (w * inv_w_sum) * saturate(v_dot(n, d))

        approx = saturate(v_dot(mean_normal, d))
        ba = dst - approx
        ya = true_avg - approx
        num += ba * ya
        den += ba * ba

    if den <= 1e-20:
        return 0.0
    return saturate(num / den)


def generate_dataset(
    rng: random.Random,
    cluster_count: int,
    normals_per_cluster: int,
    query_dir_count: int,
) -> List[ClusterStats]:
    query_dirs = [sample_uniform_sphere(rng) for _ in range(query_dir_count)]
    out: List[ClusterStats] = []

    for _ in range(cluster_count):
        normals: List[Vec3] = []
        weights: List[float] = []

        for _ in range(normals_per_cluster):
            normals.append(sample_cluster_normal(rng))
            # Mild weight variation; roughly mimics intensity spread.
            weights.append(math.exp(rng.uniform(-1.0, 1.0)))

        var_metric = compute_cluster_var_metric(normals, weights)
        t_opt = solve_optimal_t(normals, weights, query_dirs)
        out.append(ClusterStats(var_metric=var_metric, t_opt=t_opt))

    return out


def mse(pred: Iterable[float], gt: Iterable[float]) -> float:
    p = list(pred)
    g = list(gt)
    if not p:
        return 0.0
    s = 0.0
    for a, b in zip(p, g):
        d = a - b
        s += d * d
    return s / len(p)


def fit_two_point(data: Sequence[ClusterStats], v1: float) -> float:
    if v1 <= 1e-20:
        return float("inf")
    pred = [saturate(d.var_metric / v1) for d in data]
    gt = [d.t_opt for d in data]
    return mse(pred, gt)


def fit_two_point_search_v1(data: Sequence[ClusterStats], v1_min: float, v1_max: float, steps: int = 600) -> Tuple[float, float]:
    best_v1 = v1_min
    best_loss = float("inf")
    for i in range(steps):
        t = i / max(steps - 1, 1)
        v1 = v1_min * (v1_max / max(v1_min, 1e-20)) ** t
        l = fit_two_point(data, v1)
        if l < best_loss:
            best_loss = l
            best_v1 = v1
    return best_v1, best_loss


def piecewise3(x: float, vk: float, tk: float, v1: float) -> float:
    if x <= 0.0:
        return 0.0
    if x >= v1:
        return 1.0
    if x <= vk:
        return tk * x / max(vk, 1e-20)
    return tk + (1.0 - tk) * (x - vk) / max(v1 - vk, 1e-20)


def fit_three_point(data: Sequence[ClusterStats], v1: float, vk: float, tk: float) -> float:
    pred = [saturate(piecewise3(d.var_metric, vk, tk, v1)) for d in data]
    gt = [d.t_opt for d in data]
    return mse(pred, gt)


def fit_three_point_grid(data: Sequence[ClusterStats], v1: float) -> Tuple[float, float, float]:
    best_vk = v1 * 0.4
    best_tk = 0.4
    best_loss = float("inf")

    for i in range(8, 93):
        vk = v1 * (i / 100.0)
        if vk <= 1e-20 or vk >= v1:
            continue
        for j in range(5, 96):
            tk = j / 100.0
            l = fit_three_point(data, v1, vk, tk)
            if l < best_loss:
                best_loss = l
                best_vk = vk
                best_tk = tk

    return best_vk, best_tk, best_loss


def estimate_uniform_sphere_var_mc(rng: random.Random, n: int = 200000) -> float:
    # For unit sphere, theoretical m2=(1/3,1/3,1/3), m1=0 => var_metric=1/3.
    acc = (0.0, 0.0, 0.0)
    for _ in range(n):
        d = sample_uniform_sphere(rng)
        acc = v_add(acc, v_square(d))
    m2 = v_mul(acc, 1.0 / n)
    return v_dot(m2, m2)


def percentile(values: Sequence[float], p: float) -> float:
    if not values:
        return 0.0
    s = sorted(values)
    idx = min(max(int(round((len(s) - 1) * p)), 0), len(s) - 1)
    return s[idx]


def main() -> None:
    ap = argparse.ArgumentParser(description="Fit lerp factor constants for cluster normal variance mapping.")
    ap.add_argument("--seed", type=int, default=12345)
    ap.add_argument("--clusters", type=int, default=500)
    ap.add_argument("--normals-per-cluster", type=int, default=96)
    ap.add_argument("--query-dirs", type=int, default=256)
    ap.add_argument("--fit-v1", choices=["fixed-uniform", "search"], default="fixed-uniform")
    ap.add_argument("--v1-max-scale", type=float, default=2.0, help="Only used in search mode, relative to theoretical uniform var (1/3).")
    args = ap.parse_args()

    rng = random.Random(args.seed)
    data = generate_dataset(
        rng=rng,
        cluster_count=args.clusters,
        normals_per_cluster=args.normals_per_cluster,
        query_dir_count=args.query_dirs,
    )

    v_uniform_theory = 1.0 / 3.0
    v_uniform_mc = estimate_uniform_sphere_var_mc(rng)

    if args.fit_v1 == "fixed-uniform":
        v1 = v_uniform_theory
        loss2 = fit_two_point(data, v1)
    else:
        # Start search from low percentile to avoid v1 collapsing to near-zero.
        vars_only = [d.var_metric for d in data]
        v1_min = max(percentile(vars_only, 0.50), 1e-4)
        v1_max = max(v_uniform_theory * args.v1_max_scale, v1_min * 1.01)
        v1, loss2 = fit_two_point_search_v1(data, v1_min, v1_max)

    vk, tk, loss3 = fit_three_point_grid(data, v1)

    print("=== LightGrid Lerp Fit Report ===")
    print(f"seed: {args.seed}")
    print(f"clusters: {args.clusters}, normals/cluster: {args.normals_per_cluster}, query_dirs: {args.query_dirs}")
    print()
    print(f"theoretical uniform-sphere var: {v_uniform_theory:.8f}")
    print(f"MC-estimated uniform-sphere var: {v_uniform_mc:.8f}")
    print()
    print("2-point linear fit (0,0) -> (v1,1):")
    print(f"v1 = {v1:.8f}, mse = {loss2:.8f}")
    print()
    print("3-point piecewise-linear fit:")
    print(f"knee_var = {vk:.8f}, knee_t = {tk:.8f}, v1 = {v1:.8f}, mse = {loss3:.8f}")
    print()
    print("Suggested HLSL snippets:")
    print("--- two-point ---")
    print("float ClusterNormalVarianceToLerpFactor(float v) {")
    print(f"    const float v1 = {v1:.8f}f;")
    print("    return saturate(v / v1);")
    print("}")
    print("--- three-point ---")
    print("float ClusterNormalVarianceToLerpFactor(float v) {")
    print(f"    const float kVar = {vk:.8f}f;")
    print(f"    const float kT   = {tk:.8f}f;")
    print(f"    const float v1   = {v1:.8f}f;")
    print("    if (v <= 0.f) return 0.f;")
    print("    if (v >= v1) return 1.f;")
    print("    if (v <= kVar) return kT * v / kVar;")
    print("    return kT + (1.f - kT) * (v - kVar) / (v1 - kVar);")
    print("}")


if __name__ == "__main__":
    main()
