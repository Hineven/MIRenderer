# scripts

## unet_denoiser.py

Simple PyTorch U-Net denoiser for 5-channel input (RGBA + Depth) to 4-channel output (RGBA).

- Input shape: `N x 5 x H x W`
- Output shape: `N x 4 x H x W`
- Output range: `[0, 1]` via `sigmoid`
- Downsamples: 2 (input size should be divisible by 4; otherwise padded)
- Bottleneck max channels: 64

### Quick run

```powershell
python .\unet_denoiser.py --height 1080 --width 1920 --device cpu
```

### Dependencies

Install PyTorch for your CUDA/CPU setup (see https://pytorch.org/get-started/locally/).

## train.py

Train the U-Net denoiser using live data from the renderer (no disk cache).

### Dry-run (no renderer needed)

```powershell
python .\\train.py --dry-run --steps 1 --batch-size 2 --height 32 --width 32 --device cpu
```

### Live training (renderer running)

```powershell
python .\\train.py --steps 1000 --batch-size 4 --device cuda
```

## test_data_spawner.py

Generate 100 test views into `data/` using the renderer.

```powershell
python .\\test_data_spawner.py --num-views 100 --batch-size 8 --model-path F:\\path\\to\\point_cloud.ply
```

Dry-run (no renderer):

```powershell
python .\\test_data_spawner.py --num-views 10 --batch-size 5 --dry-run
```

## test.py

Evaluate a trained model on the generated test data and report MSE / PSNR.

```powershell
python .\\test.py --data-dir ..\\data --model-path .\\unet_denoiser.pt --save-vis ..\\data_vis
```
