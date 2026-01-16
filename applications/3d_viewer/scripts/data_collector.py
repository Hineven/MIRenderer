from pymi.zmq_client import ViewerClient
import matplotlib.pyplot as plt
import numpy as np

client = ViewerClient()
client.ping()

result = client.render_and_export_current_frame(types=["radiance"], timeout_sec=60)
exports = result.get("exports", [])

print(f"Received {len(exports)} exports")

for exp in exports:
    data = exp["data"]
    name = exp.get("name", "")
    fmt = exp.get("format", "")

    # If float16/float32, visualize first three channels
    arr = data
    if arr.dtype.kind == "f":
        # Normalize to [0,1] for display
        arr_vis = np.clip(arr[..., :3], 0, None)
        arr_vis = arr_vis / (arr_vis.max() + 1e-6)
    else:
        arr_vis = arr[..., :3].astype(np.float32) / 255.0

    # Convert to f32 for visualization
    arr_vis = arr_vis.astype(np.float32)

    plt.figure()
    plt.title(f"{name} ({fmt})")
    plt.imshow(arr_vis)

plt.show()
