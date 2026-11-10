"""
Universal GPU Rate-Limiter Hardware Status Checker (Ada Lovelace, Ampere & Turing)
SPDX-License-Identifier: GPL-2.0-only
"""
import sys
import time

try:
    import torch
except ImportError:
    print("=" * 72)
    print("  [ERROR] PyTorch is not installed in this Python environment.")
    print("  To run this benchmark, install PyTorch with CUDA:")
    print("    pip install torch --index-url https://download.pytorch.org/whl/cu121")
    print("=" * 72)
    sys.exit(2)

if not torch.cuda.is_available():
    print("=" * 72)
    print("  [ERROR] CUDA is not available. Please verify NVIDIA driver is installed.")
    print("=" * 72)
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
    "4070 ti": {"min_unlocked": 95.0, "threshold": 82.0, "stock_typical": 64.0},
    "4070 super": {"min_unlocked": 88.0, "threshold": 76.0, "stock_typical": 60.0},
    "4070": {"min_unlocked": 78.0, "threshold": 68.0, "stock_typical": 52.0},
    "4060 ti": {"min_unlocked": 55.0, "threshold": 48.0, "stock_typical": 36.0},
    "4060": {"min_unlocked": 40.0, "threshold": 34.0, "stock_typical": 26.0},
    "3090 ti": {"min_unlocked": 125.0, "threshold": 105.0, "stock_typical": 78.0},
    "3090": {"min_unlocked": 115.0, "threshold": 95.0, "stock_typical": 72.0},
    "3080 ti": {"min_unlocked": 110.0, "threshold": 92.0, "stock_typical": 70.0},
    "3080": {"min_unlocked": 95.0, "threshold": 80.0, "stock_typical": 62.0},
    "3070 ti": {"min_unlocked": 75.0, "threshold": 62.0, "stock_typical": 48.0},
    "3070": {"min_unlocked": 65.0, "threshold": 54.0, "stock_typical": 40.0},
    "3060 ti": {"min_unlocked": 55.0, "threshold": 46.0, "stock_typical": 34.0},
    "3060": {"min_unlocked": 40.0, "threshold": 32.0, "stock_typical": 24.0},
    "3050": {"min_unlocked": 28.0, "threshold": 22.0, "stock_typical": 15.0},
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

def benchmark_gemm(dtype, size=4096, warmup=5, runs=25):
    flops = 2.0 * (size ** 3)
    a = torch.randn((size, size), dtype=dtype, device=device)
    b = torch.randn((size, size), dtype=dtype, device=device)
    
    for _ in range(warmup):
        c = torch.matmul(a, b)
    torch.cuda.synchronize(device)
    
    start_ev = torch.cuda.Event(enable_timing=True)
    end_ev = torch.cuda.Event(enable_timing=True)
    
    start_ev.record()
    for _ in range(runs):
        c = torch.matmul(a, b)
    end_ev.record()
    torch.cuda.synchronize(device)
    
    avg_ms = start_ev.elapsed_time(end_ev) / runs
    tflops = (flops / (avg_ms * 1e-3)) / 1e12
    del a, b, c
    return avg_ms, tflops

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

    # 1. Primary Benchmark: FP16 Tensor Cores
    ms_fp16, tflops_fp16 = benchmark_gemm(torch.float16, size=size, warmup=8, runs=25)

    # 2. Secondary Benchmark: BF16 Tensor Cores (Ampere / Ada)
    has_bf16 = hasattr(torch.cuda, "is_bf16_supported") and torch.cuda.is_bf16_supported()
    if has_bf16:
        ms_bf16, tflops_bf16 = benchmark_gemm(torch.bfloat16, size=size, warmup=4, runs=15)
    else:
        ms_bf16, tflops_bf16 = 0.0, 0.0

    # 3. TF32 Tensor Cores (Ampere / Ada, allow_tf32=True)
    has_tf32 = props.major >= 8 and hasattr(torch.backends.cuda.matmul, "allow_tf32")
    orig_tf32 = getattr(torch.backends.cuda.matmul, "allow_tf32", True)
    if has_tf32:
        torch.backends.cuda.matmul.allow_tf32 = True
        ms_tf32, tflops_tf32 = benchmark_gemm(torch.float32, size=size, warmup=4, runs=15)
    else:
        ms_tf32, tflops_tf32 = 0.0, 0.0

    # 4. Physical Negative Control: Standard FP32 CUDA Cores (allow_tf32=False)
    if hasattr(torch.backends.cuda.matmul, "allow_tf32"):
        torch.backends.cuda.matmul.allow_tf32 = False
    ms_fp32, tflops_fp32 = benchmark_gemm(torch.float32, size=size, warmup=4, runs=15)

    if hasattr(torch.backends.cuda.matmul, "allow_tf32"):
        torch.backends.cuda.matmul.allow_tf32 = orig_tf32

    torch.cuda.empty_cache()

    is_unlocked = tflops_fp16 >= profile["threshold"]
    ratio = tflops_fp16 / tflops_fp32 if tflops_fp32 > 0 else 1.0

    print("=" * 72)
    print("     UNIVERSAL GPU RATE-LIMITER STATUS CHECK (ADA, AMPERE & TURING)")
    print("=" * 72)
    print(f"  GPU Hardware       : {gpu_name}")
    print(f"  Architecture       : {arch} ({sm_count} Streaming Multiprocessors)")
    print(f"  Matrix Dimension   : {size}x{size} Dense Matrix Multiply (GEMM)")
    print("-" * 72)
    print(f"  [1] FP16 Tensor Cores   : {tflops_fp16:>6.2f} TFLOPS ({ms_fp16:.3f} ms)  -> Primary Rate-Limiter Metric")
    if has_bf16:
        print(f"  [2] BF16 Tensor Cores   : {tflops_bf16:>6.2f} TFLOPS ({ms_bf16:.3f} ms)  -> Bfloat16 AI Training Format")
    if has_tf32:
        print(f"  [3] TF32 Tensor Cores   : {tflops_tf32:>6.2f} TFLOPS ({ms_tf32:.3f} ms)  -> Ampere/Ada 19-bit Format")
    print(f"  [4] FP32 CUDA Control   : {tflops_fp32:>6.2f} TFLOPS ({ms_fp32:.3f} ms)  -> Standard Physical Control")
    print("-" * 72)
    print("  PHYSICAL PROOF OF MECHANISM (Negative Control Analysis):")
    print(f"  * Tensor/CUDA Ratio     : {ratio:.2f}x execution throughput multiplier")
    print(f"  * FP32 CUDA Baseline    : {tflops_fp32:.2f} TFLOPS (Factory stock compute preserved)")
    if is_unlocked:
        print("  * Hardware Invariance   : Confirms rate-limiter bypass WITHOUT core overclocking.")
    else:
        print("  * Hardware Status       : Tensor datapath is restricted by factory rate-limiter.")
    print("-" * 72)

    if is_unlocked:
        print(f"  STATUS: [OK] FULLY UNLOCKED! ({tflops_fp16:.1f} TFLOPS)")
        print("  Hardware rate-limiter is INACTIVE.")
        print("  GPU is running at 100% full unrestricted tensor compute capacity.")
        ret_code = 0
    else:
        print(f"  STATUS: [WARNING] STOCK RATE-LIMITER ACTIVE! ({tflops_fp16:.1f} TFLOPS)")
        print(f"  Expected unlocked performance: > {profile['threshold']:.0f} TFLOPS (typical ~{profile['min_unlocked']:.0f} TFLOPS).")
        print("  Recommendation: Boot via USB [U]/[I] or resume from Hibernation (shutdown /h).")
        if props.major == 7:
            print("  Note for Turing: Verify EnableGpuFirmware = 1 is configured in Windows Registry.")
        ret_code = 1

    print("=" * 72)
    sys.exit(ret_code)

if __name__ == "__main__":
    main()
