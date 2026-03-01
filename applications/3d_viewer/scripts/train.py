import os
import sys

from typing import Dict, List, Optional, Tuple

import argparse
import numpy as np
import torch
import torch.nn.functional as F

from pymi.zmq_client import ViewerClient
from scripts.data_collector import collect_batch
from scripts.unet_denoiser import ModelConfig, UNetDenoiser, _pad_to_multiple, _unpad


gaussian_model_path = "F:/CLionProjects/3DGS_GI/data/caterpillar/point_cloud/iteration_50000/point_cloud.ply"

# Total number of views to generate
NUM_VIEWS = 10000


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Train U-Net denoiser with on-the-fly renderer data")
    parser.add_argument("--device", type=str, default="cuda" if torch.cuda.is_available() else "cpu")
    parser.add_argument("--batch-size", type=int, default=8)
    parser.add_argument("--steps", type=int, default=1000)
    parser.add_argument("--lr", type=float, default=1e-3)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--log-interval", type=int, default=10)
    parser.add_argument("--save-path", type=str, default="unet_denoiser.pt")
    parser.add_argument("--height", type=int, default=1080)
    parser.add_argument("--width", type=int, default=1920)
    parser.add_argument("--dry-run", action="store_true", help="Use random data instead of the renderer")
    return parser.parse_args()


def _find_export(exports: List[Dict], name: str) -> Dict:
    for item in exports:
        if item.get("name") == name:
            return item
    raise KeyError(f"Missing export '{name}', available: {[e.get('name') for e in exports]}")


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


def _prepare_batch(batch: List[Dict], device: str) -> Tuple[torch.Tensor, torch.Tensor]:
    inputs = []
    targets = []

    for item in batch:
        data = item["data"]
        stoch = data["stochastic"]
        regular = data["regular"]

        noisy_rgba = _to_float01(_find_export(stoch, "overlay")["data"])
        noisy_depth = _to_float01(_find_export(stoch, "grf_depth")["data"])
        clean_rgba = _to_float01(_find_export(regular, "overlay")["data"])

        if noisy_depth.ndim == 2:
            noisy_depth = noisy_depth[..., None]
        if noisy_depth.shape[-1] > 1:
            noisy_depth = noisy_depth[..., :1]

        inp = np.concatenate([noisy_rgba, noisy_depth], axis=-1)  # H x W x 5
        inputs.append(inp)
        targets.append(clean_rgba)  # H x W x 4

    x = torch.from_numpy(np.stack(inputs, axis=0)).permute(0, 3, 1, 2).to(device)
    y = torch.from_numpy(np.stack(targets, axis=0)).permute(0, 3, 1, 2).to(device)

    x = x.clamp(0.0, 1.0)
    y = y.clamp(0.0, 1.0)
    return x, y


def _loss_fn(pred: torch.Tensor, target: torch.Tensor) -> torch.Tensor:
    pred_rgb = pred[:, :3]
    pred_a = pred[:, 3:4]
    tgt_rgb = target[:, :3]
    tgt_a = target[:, 3:4]

    # Use target alpha as mask to avoid background dominating.
    mask = (tgt_a > 0.01).float()
    denom = mask.sum() * 3.0 + 1e-6
    loss_rgb = (torch.abs(pred_rgb - tgt_rgb) * mask).sum() / denom
    loss_a = F.l1_loss(pred_a, tgt_a)
    return loss_rgb + 0.1 * loss_a


def _make_synthetic_batch(batch_size: int, height: int, width: int, device: str) -> Tuple[torch.Tensor, torch.Tensor]:
    x = torch.rand(batch_size, 5, height, width, device=device)
    y = torch.rand(batch_size, 4, height, width, device=device)
    return x, y


def train() -> None:
    args = _parse_args()
    torch.manual_seed(args.seed)
    np.random.seed(args.seed)

    model = UNetDenoiser(ModelConfig()).to(args.device)
    optimizer = torch.optim.Adam(model.parameters(), lr=args.lr)

    client: Optional[ViewerClient] = None
    if not args.dry_run:
        client = ViewerClient()
        client.ping()
        client.set_suspended(True)
        client.load_ply_abs_path(gaussian_model_path)

    model.train()
    for step in range(1, args.steps + 1):
        if args.dry_run:
            x, y = _make_synthetic_batch(args.batch_size, args.height, args.width, args.device)
        else:
            batch = collect_batch(client, batch_size=args.batch_size, seed=args.seed + step)
            x, y = _prepare_batch(batch, args.device)

        x, pads = _pad_to_multiple(x, multiple=4)
        pred = model(x)
        pred = _unpad(pred, pads)

        loss = _loss_fn(pred, y)
        optimizer.zero_grad(set_to_none=True)
        loss.backward()
        optimizer.step()

        if step % args.log_interval == 0:
            print(f"step {step:06d} | loss {loss.item():.6f}")

        # Free memory promptly after each batch.
        del x, y, pred, loss
        if not args.dry_run:
            del batch
        if args.device.startswith("cuda"):
            torch.cuda.empty_cache()

    torch.save({
        "model_state": model.state_dict(),
        "optimizer_state": optimizer.state_dict(),
        "config": ModelConfig(),
        "steps": args.steps,
    }, args.save_path)

    if client:
        client.close()


if __name__ == "__main__":
    train()
