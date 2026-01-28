import argparse
from dataclasses import dataclass
from typing import Tuple

import torch
import torch.nn as nn
import torch.nn.functional as F


@dataclass
class ModelConfig:
    in_channels: int = 5
    out_channels: int = 4
    base_channels: int = 32
    max_bottleneck_channels: int = 64
    num_downsamples: int = 2


class DepthwiseSeparableConv(nn.Module):
    def __init__(self, in_ch: int, out_ch: int, kernel_size: int = 3, padding: int = 1):
        super().__init__()
        self.depthwise = nn.Conv2d(in_ch, in_ch, kernel_size=kernel_size, padding=padding, groups=in_ch, bias=False)
        self.pointwise = nn.Conv2d(in_ch, out_ch, kernel_size=1, bias=False)
        self.act = nn.ReLU(inplace=True)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        x = self.depthwise(x)
        x = self.pointwise(x)
        return self.act(x)


class UNetDenoiser(nn.Module):
    def __init__(self, cfg: ModelConfig):
        super().__init__()
        ch1 = cfg.base_channels
        ch2 = min(cfg.max_bottleneck_channels, ch1 * 2)
        ch3 = min(cfg.max_bottleneck_channels, ch2 * 2)

        # Encoder
        self.enc1 = DepthwiseSeparableConv(cfg.in_channels, ch1)
        self.down1 = nn.Conv2d(ch1, ch2, kernel_size=3, stride=2, padding=1, bias=False)
        self.enc2 = DepthwiseSeparableConv(ch2, ch2)
        self.down2 = nn.Conv2d(ch2, ch3, kernel_size=3, stride=2, padding=1, bias=False)

        # Bottleneck
        self.bottleneck = DepthwiseSeparableConv(ch3, ch3)

        # Decoder
        self.up2 = nn.Conv2d(ch3, ch2, kernel_size=1, bias=False)
        self.dec2 = DepthwiseSeparableConv(ch2 + ch2, ch2)
        self.up1 = nn.Conv2d(ch2, ch1, kernel_size=1, bias=False)
        self.dec1 = DepthwiseSeparableConv(ch1 + ch1, ch1)

        self.out_conv = nn.Conv2d(ch1, cfg.out_channels, kernel_size=1)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        # Encoder
        e1 = self.enc1(x)
        d1 = self.down1(e1)
        e2 = self.enc2(d1)
        d2 = self.down2(e2)

        # Bottleneck
        b = self.bottleneck(d2)

        # Decoder
        u2 = F.interpolate(b, scale_factor=2, mode="nearest")
        u2 = self.up2(u2)
        u2 = torch.cat([u2, e2], dim=1)
        u2 = self.dec2(u2)

        u1 = F.interpolate(u2, scale_factor=2, mode="nearest")
        u1 = self.up1(u1)
        u1 = torch.cat([u1, e1], dim=1)
        u1 = self.dec1(u1)

        out = self.out_conv(u1)
        return torch.sigmoid(out)


def _pad_to_multiple(x: torch.Tensor, multiple: int = 4) -> Tuple[torch.Tensor, Tuple[int, int, int, int]]:
    h, w = x.shape[-2], x.shape[-1]
    pad_h = (multiple - (h % multiple)) % multiple
    pad_w = (multiple - (w % multiple)) % multiple
    pad_top = pad_h // 2
    pad_bottom = pad_h - pad_top
    pad_left = pad_w // 2
    pad_right = pad_w - pad_left
    if pad_h or pad_w:
        x = F.pad(x, (pad_left, pad_right, pad_top, pad_bottom), mode="replicate")
    return x, (pad_left, pad_right, pad_top, pad_bottom)


def _unpad(x: torch.Tensor, pads: Tuple[int, int, int, int]) -> torch.Tensor:
    pad_left, pad_right, pad_top, pad_bottom = pads
    if pad_left or pad_right or pad_top or pad_bottom:
        return x[..., pad_top:x.shape[-2] - pad_bottom, pad_left:x.shape[-1] - pad_right]
    return x


def run_demo(height: int, width: int, device: str) -> None:
    cfg = ModelConfig()
    model = UNetDenoiser(cfg).to(device)
    model.eval()

    x = torch.rand(1, cfg.in_channels, height, width, device=device)
    x, pads = _pad_to_multiple(x, multiple=4)
    with torch.no_grad():
        y = model(x)
        y = _unpad(y, pads)

    print(f"Input shape: {tuple(x.shape)}")
    print(f"Output shape: {tuple(y.shape)}")
    print(f"Output range: min={y.min().item():.4f} max={y.max().item():.4f}")


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Simple U-Net denoiser demo")
    parser.add_argument("--height", type=int, default=1080)
    parser.add_argument("--width", type=int, default=1920)
    parser.add_argument("--device", type=str, default="cpu")
    return parser.parse_args()


if __name__ == "__main__":
    args = _parse_args()
    run_demo(args.height, args.width, args.device)
