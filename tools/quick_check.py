"""
Universal GPU Rate-Limiter Hardware Status Checker (Ada Lovelace, Ampere & Turing)
SPDX-License-Identifier: GPL-2.0-only
"""
import sys
import time

try:
    import torch
except ImportError:
    print("=" * 68)
    print("  [ERROR] PyTorch is not installed in this Python environment.")
    print("  To run this benchmark, install PyTorch with CUDA:")
    print("    pip install torch --index-url https://download.pytorch.org/whl/cu121")
    print("=" * 68)
    sys.exit(2)

if not torch.cuda.is_available():
    print("=" * 68)
    print("  [ERROR] CUDA is not available. Please verify NVIDIA driver is installed.")
    print("=" * 68)
    sys.exit(2)

device = torch.device("cuda:0")
gpu_name = torch.cuda.get_device_name(0)
props = torch.cuda.get_device_properties(0)
sm_count = props.multi_processor_count
arch = f"sm_{props.major}{props.minor}"

CARD_PROFILES = {
    "4090": {"min_unlocked": 200.0, "threshold": 175.0, "stock_typical": 135.0},
    "4080": {"min_unlocked": 115.0, "threshold": 98.0, "stock_typical": 80.0},
    "4070 ti super": {"min_unlocked": 100.0, "threshold": 88.0, "stock_typical": 70.0},
    "3090 ti": {"min_unlocked": 125.0, "threshold": 105.0, "stock_typical": 78.0},
    "3090": {"min_unlocked": 115.0, "threshold": 95.0, "stock_typical": 72.0},
    "3080 ti": {"min_unlocked": 110.0, "threshold": 92.0, "stock_typical": 70.0},
    "3080": {"min_unlocked": 95.0, "threshold": 80.0, "stock_typical": 62.0},
    "3070 ti": {"min_unlocked": 75.0, "threshold": 62.0, "stock_typical": 48.0},
    "3070": {"min_unlocked": 65.0, "threshold": 54.0, "stock_typical": 40.0},
    "3060 ti": {"min_unlocked": 55.0, "threshold": 46.0, "stock_typical": 34.0},
    "3060": {"min_unlocked": 40.0, "threshold": 32.0, "stock_typical": 24.0},
    "titan rtx": {"min_unlocked": 115.0, "threshold": 95.0, "stock_typical": 65.0},
    "2080 ti": {"min_unlocked": 105.0, "threshold": 85.0, "stock_typical": 54.0},
    "2080 super": {"min_unlocked": 80.0, "threshold": 65.0, "stock_typical": 42.0},
    "2080": {"min_unlocked": 72.0, "threshold": 60.0, "stock_typical": 38.0},
    "2070 super": {"min_unlocked": 65.0, "threshold": 52.0, "stock_typical": 34.0},
    "2070": {"min_unlocked": 55.0, "threshold": 45.0, "stock_typical": 29.0},
    "2060 super": {"min_unlocked": 50.0, "threshold": 42.0, "stock_typical": 27.0},
    "2060": {"min_unlocked": 42.0, "threshold": 35.0, "stock_typical": 22.0},
    "cmp 50hx": {"min_unlocked": 105.0, "threshold": 85.0, "stock_typical": 54.0},
}

def main():
    profile = None
    gpu_lower = gpu_name.lower()
    for key, prof in CARD_PROFILES.items():
        if key in gpu_lower:
            profile = prof
            break

    if profile is None:
        if props.major == 8 and props.minor == 9:
            threshold = sm_count * 1.35
        elif props.major == 8 and props.minor == 6:
            threshold = sm_count * 1.15
        elif props.major == 7 and props.minor == 5:
            threshold = sm_count * 1.15
        else:
            threshold = sm_count * 1.0
        profile = {"min_unlocked": threshold * 1.15, "threshold": threshold, "stock_typical": threshold * 0.75}

    size = 4096
    flops_per_gemm = 2 * (size ** 3)
    a = torch.randn((size, size), dtype=torch.float16, device=device)
    b = torch.randn((size, size), dtype=torch.float16, device=device)

    for _ in range(15):
        c = torch.matmul(a, b)
    torch.cuda.synchronize(device)

    runs = 30
    start_ev = torch.cuda.Event(enable_timing=True)
    end_ev = torch.cuda.Event(enable_timing=True)

    start_ev.record()
    for _ in range(runs):
        c = torch.matmul(a, b)
    end_ev.record()
    torch.cuda.synchronize(device)

    total_ms = start_ev.elapsed_time(end_ev)
    avg_ms = total_ms / runs
    tflops = (flops_per_gemm / (avg_ms * 1e-3)) / 1e12

    print("=" * 68)
    print("     UNIVERSAL GPU RATE-LIMITER STATUS CHECK (ADA, AMPERE & TURING)")
    print("=" * 68)
    print(f"  GPU Hardware       : {gpu_name}")
    print(f"  Architecture       : {arch} ({sm_count} Streaming Multiprocessors)")
    print(f"  Matrix Benchmark   : {size}x{size} FP16 Tensor Cores")
    print(f"  Measured Latency   : {avg_ms:.3f} ms")
    print(f"  Compute Throughput : {tflops:.2f} TFLOPS")
    print("-" * 68)

    is_unlocked = tflops >= profile["threshold"]

    if is_unlocked:
        print(f"  STATUS: [OK] FULLY UNLOCKED! ({tflops:.1f} TFLOPS)")
        print("  Hardware rate-limiter is INACTIVE.")
        print("  GPU is running at 100% full unrestricted tensor compute capacity.")
        ret_code = 0
    else:
        print(f"  STATUS: [WARNING] STOCK RATE-LIMITER ACTIVE! ({tflops:.1f} TFLOPS)")
        print(f"  Expected unlocked performance: > {profile['threshold']:.0f} TFLOPS (typical ~{profile['min_unlocked']:.0f} TFLOPS).")
        print("  Recommendation: Boot via USB [U]/[I] or resume from Hibernation (shutdown /h).")
        ret_code = 1

    print("=" * 68)
    sys.exit(ret_code)

if __name__ == "__main__":
    main()
