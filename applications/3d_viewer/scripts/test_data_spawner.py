import os
import sys
# Allow running this script directly by adding project root to sys.path.
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(SCRIPT_DIR)
if PROJECT_ROOT not in sys.path:
    sys.path.insert(0, PROJECT_ROOT)

from typing import Dict, List, Optional

import argparse
import json
import numpy as np
import imageio.v3 as iio

from pymi.zmq_client import ViewerClient
from scripts.data_collector import collect_batch


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Spawn test data with the renderer")
    parser.add_argument("--output-dir", type=str, default=os.path.join(SCRIPT_DIR, "..", "data"))
    parser.add_argument("--num-views", type=int, default=100)
    parser.add_argument("--batch-size", type=int, default=8)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--model-path", type=str, default="")
    parser.add_argument("--dry-run", action="store_true", help="Generate random data without the renderer")
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


def _save_rgba_png(path: str, rgba: np.ndarray) -> None:
    rgba = np.clip(rgba, 0.0, 1.0)
    rgba_u8 = (rgba * 255.0 + 0.5).astype(np.uint8)
    iio.imwrite(path, rgba_u8)


def _save_depth_exr(path: str, depth: np.ndarray) -> None:
    depth = depth.astype(np.float32)
    if depth.ndim == 3 and depth.shape[-1] > 1:
        depth = depth[..., :1]
    try:
        iio.imwrite(path, depth, plugin="opencv")
    except Exception as exc:
        raise RuntimeError(
            "Failed to write EXR depth. Install opencv-python for EXR support."
        ) from exc


def _write_view(out_dir: str, index: int, noisy_rgba: np.ndarray, noisy_depth: np.ndarray,
                clean_rgba: np.ndarray, meta: Dict) -> None:
    view_dir = os.path.join(out_dir, f"view_{index:06d}")
    os.makedirs(view_dir, exist_ok=True)

    _save_rgba_png(os.path.join(view_dir, "noisy_rgba.png"), noisy_rgba)
    _save_depth_exr(os.path.join(view_dir, "noisy_depth.exr"), noisy_depth)
    _save_rgba_png(os.path.join(view_dir, "clean_rgba.png"), clean_rgba)

    with open(os.path.join(view_dir, "meta.json"), "w", encoding="utf-8") as f:
        json.dump(meta, f, indent=2)


def _make_synthetic_batch(batch_size: int, height: int = 256, width: int = 256) -> List[Dict]:
    batch: List[Dict] = []
    for _ in range(batch_size):
        noisy_rgba = np.random.rand(height, width, 4).astype(np.float32)
        noisy_depth = np.random.rand(height, width, 1).astype(np.float32)
        clean_rgba = np.random.rand(height, width, 4).astype(np.float32)
        batch.append({
            "camera_pos": np.zeros(3),
            "camera_dir": np.array([0.0, 0.0, -1.0]),
            "data": {
                "stochastic": [
                    {"name": "overlay", "data": noisy_rgba},
                    {"name": "grf_depth", "data": noisy_depth},
                ],
                "regular": [
                    {"name": "overlay", "data": clean_rgba},
                ],
            },
        })
    return batch


def main() -> None:
    args = _parse_args()
    out_dir = os.path.abspath(args.output_dir)
    os.makedirs(out_dir, exist_ok=True)

    client: Optional[ViewerClient] = None
    if not args.dry_run:
        client = ViewerClient()
        client.ping()
        client.set_suspended(True)
        if args.model_path:
            client.load_ply_abs_path(args.model_path)

    remaining = args.num_views
    current_index = 0

    while remaining > 0:
        current_batch = min(args.batch_size, remaining)
        if args.dry_run:
            batch = _make_synthetic_batch(current_batch)
        else:
            batch = collect_batch(client, batch_size=current_batch, seed=args.seed + current_index)

        for item in batch:
            data = item["data"]
            stoch = data["stochastic"]
            regular = data["regular"]

            noisy_rgba = _to_float01(_find_export(stoch, "overlay")["data"])
            noisy_depth = _to_float01(_find_export(stoch, "grf_depth")["data"])
            clean_rgba = _to_float01(_find_export(regular, "overlay")["data"])

            if noisy_depth.ndim == 2:
                noisy_depth = noisy_depth[..., None]

            meta = {
                "camera_pos": item["camera_pos"].tolist(),
                "camera_dir": item["camera_dir"].tolist(),
            }
            _write_view(out_dir, current_index, noisy_rgba, noisy_depth, clean_rgba, meta)
            current_index += 1
            remaining -= 1

        del batch

    if client:
        client.close()


if __name__ == "__main__":
    main()
