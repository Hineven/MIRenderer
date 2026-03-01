import os
import sys

from pymi.zmq_client import ViewerClient
import matplotlib.pyplot as plt
import numpy as np
import math
from typing import Dict, List, Tuple, Optional


# Min-max distances to world origin for data generation
MIN_DISTANCE = 0.1
MAX_DISTANCE = 10.0

# Camera direction jittering range (radians)
DIRECTION_JITTER = 1.5

# Number of view items batched for each training data capture
BATCH_SIZE = 128

# Camera vertical FOV (degrees)
CAMERA_FOVY_DEG = 60.0


def _normalize(v: np.ndarray) -> np.ndarray:
    norm = np.linalg.norm(v)
    if norm < 1e-8:
        return v
    return v / norm


def _random_unit_vector(rng: np.random.Generator) -> np.ndarray:
    v = rng.normal(size=(3,))
    return _normalize(v)


def _sample_camera_pose(rng: np.random.Generator) -> Tuple[np.ndarray, np.ndarray]:
    # Sample position on a spherical shell and look towards origin with jitter.
    direction = _random_unit_vector(rng)
    distance = rng.uniform(MIN_DISTANCE, MAX_DISTANCE)
    pos = direction * distance

    view_dir = _normalize(-pos)
    if DIRECTION_JITTER > 0:
        jitter = _random_unit_vector(rng) * DIRECTION_JITTER
        view_dir = _normalize(view_dir + jitter)

    return pos, view_dir


def set_camera_pose(client: ViewerClient, pos: np.ndarray, view_dir: np.ndarray) -> None:
    client.set_camera_pos(pos.tolist())
    client.set_camera_dir(view_dir.tolist())
    client.set_camera_fovy(CAMERA_FOVY_DEG)


def capture_training_data_pair(client: ViewerClient) -> Dict[str, List[Dict]]:
    # Capture a pair of training data (Noisy RGB & Noisy Depth + Regular RGB)
    # Enable stochastic rendering for i-th frame
    client.set_cvar("r.grf.stochastic_rendering", True)
    # Collect stochastic rendering results from i-th frame
    results_stochastic = client.render_and_export_current_frame(types=["overlay", "grf_depth"], timeout_sec=60)
    # Disable stochastic rendering for (i+1)-th frame
    client.set_cvar("r.grf.stochastic_rendering", False)
    # Collect regular rendering results from (i+1)-th frame
    results_regular = client.render_and_export_current_frame(types=["overlay"], timeout_sec=60)

    # Pack results
    return {
        "stochastic": results_stochastic.get("exports", []),
        "regular": results_regular.get("exports", [])
    }


def collect_batch(client: ViewerClient, batch_size: int = BATCH_SIZE, seed: Optional[int] = None) -> List[Dict]:
    rng = np.random.default_rng(seed)
    batch: List[Dict] = []

    for _ in range(batch_size):
        pos, view_dir = _sample_camera_pose(rng)
        set_camera_pose(client, pos, view_dir)

        pair = capture_training_data_pair(client)
        batch.append({
            "camera_pos": pos,
            "camera_dir": view_dir,
            "data": pair,
        })

    return batch

