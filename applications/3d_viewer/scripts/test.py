import os
import sys
from typing import Dict, List, Tuple

import argparse
import numpy as np
import torch
import imageio.v3 as iio
import matplotlib.pyplot as plt

from scripts.unet_denoiser import ModelConfig, UNetDenoiser, _pad_to_multiple, _unpad


# Allow running this script directly by adding project root to sys.path.
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(SCRIPT_DIR)
if PROJECT_ROOT not in sys.path:
    sys.path.insert(0, PROJECT_ROOT)


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Evaluate trained U-Net on test data")
    parser.add_argument("--data-dir", type=str, default=os.path.join(SCRIPT_DIR, "..", "data"))
    parser.add_argument("--model-path", type=str, default="unet_denoiser.pt")
    parser.add_argument("--device", type=str, default="cuda" if torch.cuda.is_available() else "cpu")
    parser.add_argument("--max-views", type=int, default=50)
    parser.add_argument("--show", action="store_true", help="Show visualizations interactively")
    parser.add_argument("--save-vis", type=str, default="", help="Directory to save visualization PNGs")
    return parser.parse_args()


def _to_float01(array: np.ndarray) -> np.ndarray:
    if array.dtype == np.uint8:
        return array.astype(np.float32) / 255.0
    if array.dtype == np.uint16:
        return array.astype(np.float32) / 65535.0
    array = array.astype(np.float32)
    max_val = float(array.max()) if array.size else 1.0
    if max_val > 1.5:
        if max_val <= 255.0:
            return array / 255.0
        if max_val <= 65535.0:
            return array / 65535.0
    return array


def _load_rgba(path: str) -> np.ndarray:
    rgba = iio.imread(path)
    return _to_float01(rgba)


def _load_depth(path: str) -> np.ndarray:
    depth = iio.imread(path, plugin="opencv")
    depth = _to_float01(depth)
    if depth.ndim == 2:
        depth = depth[..., None]
    if depth.shape[-1] > 1:
        depth = depth[..., :1]
    return depth


def _prepare_input(noisy_rgba: np.ndarray, noisy_depth: np.ndarray, device: str) -> torch.Tensor:
    inp = np.concatenate([noisy_rgba, noisy_depth], axis=-1)
    x = torch.from_numpy(inp).permute(2, 0, 1).unsqueeze(0).to(device)
    return x.clamp(0.0, 1.0)


def _mse_psnr(pred: torch.Tensor, target: torch.Tensor) -> Tuple[float, float]:
    mse = torch.mean((pred - target) ** 2).item()
    if mse <= 0:
        psnr = float("inf")
    else:
        psnr = 10.0 * np.log10(1.0 / mse)
    return mse, psnr


def _save_vis(path: str, noisy_rgb: np.ndarray, clean_rgb: np.ndarray, pred_rgb: np.ndarray, depth: np.ndarray) -> None:
    fig, axs = plt.subplots(1, 4, figsize=(16, 4))
    axs[0].imshow(noisy_rgb)
    axs[0].set_title("noisy")
    axs[1].imshow(clean_rgb)
    axs[1].set_title("clean")
    axs[2].imshow(pred_rgb)
    axs[2].set_title("pred")
    axs[3].imshow(depth.squeeze(), cmap="magma")
    axs[3].set_title("depth")
    for ax in axs:
        ax.axis("off")
    fig.tight_layout()
    fig.savefig(path, dpi=150)
    plt.close(fig)


def main() -> None:
    args = _parse_args()
    data_dir = os.path.abspath(args.data_dir)

    ckpt = torch.load(args.model_path, map_location=args.device)
    cfg = ckpt.get("config", ModelConfig())
    model = UNetDenoiser(cfg).to(args.device)
    model.load_state_dict(ckpt["model_state"])
    model.eval()

    if args.save_vis:
        os.makedirs(args.save_vis, exist_ok=True)

    view_dirs = [
        os.path.join(data_dir, d)
        for d in sorted(os.listdir(data_dir))
        if d.startswith("view_") and os.path.isdir(os.path.join(data_dir, d))
    ]
    view_dirs = view_dirs[: args.max_views]

    mse_list: List[float] = []
    psnr_list: List[float] = []

    for idx, view_dir in enumerate(view_dirs):
        noisy_rgba = _load_rgba(os.path.join(view_dir, "noisy_rgba.png"))
        noisy_depth = _load_depth(os.path.join(view_dir, "noisy_depth.exr"))
        clean_rgba = _load_rgba(os.path.join(view_dir, "clean_rgba.png"))

        x = _prepare_input(noisy_rgba, noisy_depth, args.device)
        x, pads = _pad_to_multiple(x, multiple=4)

        with torch.no_grad():
            pred = model(x)
            pred = _unpad(pred, pads)

        pred = pred.squeeze(0).permute(1, 2, 0).clamp(0.0, 1.0).cpu().numpy()

        pred_rgb = pred[..., :3]
        clean_rgb = clean_rgba[..., :3]
        noisy_rgb = noisy_rgba[..., :3]

        mse, psnr = _mse_psnr(torch.from_numpy(pred_rgb), torch.from_numpy(clean_rgb))
        mse_list.append(mse)
        psnr_list.append(psnr)

        if args.save_vis:
            out_path = os.path.join(args.save_vis, f"vis_{idx:04d}.png")
            _save_vis(out_path, noisy_rgb, clean_rgb, pred_rgb, noisy_depth)

        if args.show:
            _save_vis("_temp_vis.png", noisy_rgb, clean_rgb, pred_rgb, noisy_depth)
            img = iio.imread("_temp_vis.png")
            plt.imshow(img)
            plt.axis("off")
            plt.show()

        del x, pred

    mean_mse = float(np.mean(mse_list)) if mse_list else 0.0
    mean_psnr = float(np.mean(psnr_list)) if psnr_list else 0.0
    print(f"MSE: {mean_mse:.6f}")
    print(f"PSNR: {mean_psnr:.2f} dB")


if __name__ == "__main__":
    main()
