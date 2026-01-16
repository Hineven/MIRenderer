import argparse
import numpy as np
import matplotlib.pyplot as plt

PI4 = 4.0 * np.pi

def _clamp_g(g: float, eps: float = 1e-6) -> float:
    """将 g 轻微钳制到 (-1, 1) 内，避免数值溢出。"""
    return float(np.clip(g, -1.0 + eps, 1.0 - eps))

def hg_phase(mu: np.ndarray, g: float) -> np.ndarray:
    """
    Henyey–Greenstein 相函数:
      P(mu; g) = (1 - g^2) / (4π * (1 + g^2 - 2 g mu)^(3/2))
    其中 mu = cos(theta)
    """
    g = _clamp_g(g)
    # 防止由于浮点误差导致的负数开方
    denom = np.maximum(1.0 + g * g - 2.0 * g * mu, 1e-15)
    return (1.0 - g * g) / (PI4 * np.power(denom, 1.5))

def plot_hg_vs_angle(gs, num=2000, yscale="linear", ax=None):
    """绘制 P(θ) 随 θ 变化的曲线，θ ∈ [0°, 180°]。"""
    if ax is None:
        ax = plt.gca()
    theta = np.linspace(0.0, np.pi, num=num)
    mu = np.cos(theta)
    for g in gs:
        y = hg_phase(mu, g)
        ax.plot(np.degrees(theta), y, label=f"g={g:.2f}")
    ax.set_xlabel("θ (度)")
    ax.set_ylabel("P(θ)")
    ax.set_yscale(yscale)
    ax.set_title("Henyey–Greenstein: P(θ)")
    ax.grid(True, alpha=0.3)
    ax.legend()

def plot_hg_vs_mu(gs, num=2000, yscale="linear", ax=None):
    """绘制 P(μ) 随 μ 变化的曲线，μ ∈ [-1, 1]。"""
    if ax is None:
        ax = plt.gca()
    mu = np.linspace(-1.0, 1.0, num=num)
    for g in gs:
        y = hg_phase(mu, g)
        ax.plot(mu, y, label=f"g={g:.2f}")
    ax.set_xlabel("μ = cos(θ)")
    ax.set_ylabel("P(μ)")
    ax.set_yscale(yscale)
    ax.set_title("Henyey–Greenstein: P(μ)")
    ax.grid(True, alpha=0.3)
    ax.legend()

def plot_hg_polar(gs, num=2000, ax=None):
    """
    极坐标绘制相函数形状：r(θ) ∝ P(θ)，为了对比形状，每个 g 分别按自身最大值归一化到 [0,1]。
    """
    if ax is None:
        ax = plt.subplot(111, projection="polar")
    theta = np.linspace(0.0, np.pi, num=num)
    mu = np.cos(theta)
    for g in gs:
        y = hg_phase(mu, g)
        y_norm = y / np.max(y)
        ax.plot(theta, y_norm, label=f"g={g:.2f}")
        # 镜像绘制到 [π, 2π]，便于在极坐标上闭合观察
        ax.plot(2.0 * np.pi - theta, y_norm, color=ax.lines[-1].get_color())
    ax.set_title("Henyey–Greenstein: 极坐标形状（各自归一化）")
    ax.set_rticks([0.25, 0.5, 0.75, 1.0])
    ax.grid(True, alpha=0.3)
    ax.legend(loc="upper right", bbox_to_anchor=(1.2, 1.1))

def check_normalization(gs, num=2_000_000):
    """
    数值积分验证归一性：∫_S P dΩ = 2π ∫_{-1}^{1} P(μ) dμ ≈ 1
    """
    mu = np.linspace(-1.0, 1.0, num=num, dtype=np.float64)
    dmu = mu[1] - mu[0]
    for g in gs:
        y = hg_phase(mu, g)
        integ = 2.0 * np.pi * np.trapz(y, dx=dmu)
        print(f"g={g:.4f}  2π∫ P(μ)dμ ≈ {integ:.8f}")

def parse_args():
    p = argparse.ArgumentParser(description="可视化 Henyey–Greenstein 相函数")
    p.add_argument("--g", type=float, nargs="+",
                   default=[-0.8, -0.5, 0.0, 0.5, 0.8],
                   help="相函数各向异性参数列表 g∈(-1,1)")
    p.add_argument("--num", type=int, default=2000, help="采样分辨率")
    p.add_argument("--plot", type=str, default="both",
                   choices=["angle", "mu", "polar", "both"],
                   help="选择绘图类型")
    p.add_argument("--yscale", type=str, default="linear",
                   choices=["linear", "log"], help="y 轴尺度（angle/mu 图有效）")
    p.add_argument("--save", type=str, default=None, help="保存图片路径")
    p.add_argument("--no-show", action="store_true", help="不显示窗口，仅保存")
    p.add_argument("--check", action="store_true", help="打印归一性数值检查")
    return p.parse_args()

def main():
    args = parse_args()
    gs = [_clamp_g(g) for g in args.g]

    if args.check:
        check_normalization(gs, num=min(max(args.num * 500, 200000), 2_000_000))

    if args.plot == "both":
        fig, axs = plt.subplots(1, 2, figsize=(12, 4), dpi=120)
        plot_hg_vs_angle(gs, num=args.num, yscale=args.yscale, ax=axs[0])
        plot_hg_vs_mu(gs, num=args.num, yscale=args.yscale, ax=axs[1])
    elif args.plot == "angle":
        plt.figure(figsize=(6, 4), dpi=120)
        plot_hg_vs_angle(gs, num=args.num, yscale=args.yscale, ax=plt.gca())
    elif args.plot == "mu":
        plt.figure(figsize=(6, 4), dpi=120)
        plot_hg_vs_mu(gs, num=args.num, yscale=args.yscale, ax=plt.gca())
    elif args.plot == "polar":
        plt.figure(figsize=(6, 6), dpi=120)
        ax = plt.subplot(111, projection="polar")
        plot_hg_polar(gs, num=args.num, ax=ax)

    plt.tight_layout()

    if args.save:
        plt.savefig(args.save, bbox_inches="tight")
        print(f"已保存到: {args.save}")

    if not args.no_show:
        plt.show()

if __name__ == "__main__":
    main()
