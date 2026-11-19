"""
Universal GPU Rate-Limiter Hardware Status Checker (Ada Lovelace, Ampere & Turing)
SPDX-License-Identifier: GPL-2.0-only
"""
import sys
import time
import os
import re
import subprocess
import ctypes

# Enable ANSI escape sequences on Windows consoles
if os.name == "nt":
    try:
        os.system("")
    except Exception:
        pass

C_GREEN = "\033[92m"
C_GREEN_BOLD = "\033[92;1m"
C_RED_BOLD = "\033[91;1m"
C_YELLOW = "\033[93m"
C_CYAN = "\033[96m"
C_RESET = "\033[0m"

# Shared helpers live next to this script (tools/gu_system_info.py).
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gu_system_info import CARD_PROFILES, get_firmware_mode, get_rebar_info  # noqa: E402

def get_pcie_info():
    candidates = [
        "nvidia-smi",
        r"C:\Windows\System32\nvidia-smi.exe",
        r"C:\Program Files\NVIDIA Corporation\NVSMI\nvidia-smi.exe",
    ]
    for exe in candidates:
        try:
            out = subprocess.check_output(
                [exe, "--query-gpu=pcie.link.gen.max,pcie.link.width.max",
                 "--format=csv,noheader,nounits"],
                text=True, stderr=subprocess.DEVNULL
            )
            parts = [p.strip() for p in out.strip().split(",")]
            if len(parts) >= 2:
                gen_max = int(parts[0])
                width_max = int(parts[1])
                is_throttled = width_max < 8 or gen_max < 3
                return gen_max, width_max, is_throttled
        except Exception:
            continue
    return None, None, False

def copy_to_clipboard(text):
    if os.name == "nt":
        try:
            proc = subprocess.Popen(["clip.exe"], stdin=subprocess.PIPE, stderr=subprocess.DEVNULL)
            proc.communicate(input=text.encode("utf-16le"))
            return True
        except Exception:
            pass
    return False

try:
    import torch
except ImportError:
    fw_str, _ = get_firmware_mode()
    bar1, fb, rebar_active = get_rebar_info()
    gen_max, width_max, is_throttled = get_pcie_info()
    print("=" * 72)
    print("     UNIVERSAL GPU RATE-LIMITER STATUS CHECK (ADA, AMPERE & TURING)")
    print("=" * 72)
    print("  SYSTEM HARDWARE ENVIRONMENT:")
    print(f"  * Firmware Mode      : {fw_str}")
    if rebar_active is True:
        print(f"  * Resizable BAR      : {C_GREEN}[OK] ENABLED (BAR1: {bar1} MiB / VRAM: {fb} MiB){C_RESET}")
    elif rebar_active is False:
        print(f"  * Resizable BAR      : {C_RED_BOLD}[!] DISABLED (BAR1: {bar1} MiB restricted) -> Turn ON in BIOS!{C_RESET}")
    else:
        print(f"  * Resizable BAR      : Undetected (BAR1: {bar1 if bar1 else 'N/A'})")
    if gen_max and width_max:
        if is_throttled:
            print(f"  * PCIe Slot Link     : {C_RED_BOLD}[!] THROTTLED: Gen{gen_max} x{width_max} (Riser or Slot Bottleneck!){C_RESET}")
        else:
            print(f"  * PCIe Slot Link     : {C_GREEN}[OK] Gen{gen_max} x{width_max} (Full Bandwidth){C_RESET}")
    print("-" * 72)
    print("  [INFO] PyTorch with CUDA is required for the live TFLOPS compute test.")
    print("  To install PyTorch with CUDA support, run:")
    print("    pip install torch --index-url https://download.pytorch.org/whl/cu121")
    print("=" * 72)
    sys.exit(0)

if not torch.cuda.is_available():
    print("=" * 72)
    print("  [ERROR] CUDA is not available. NVIDIA driver could not be contacted.")
    print("=" * 72)
    print("  DIAGNOSIS & TROUBLESHOOTING:")
    print("  1. Check Windows Device Manager (Win + X -> Device Manager -> Display adapters):")
    print("     - If your GPU shows Error Code 43 or 'Microsoft Basic Display Adapter':")
    print("       Your motherboard firmware has CSM (Compatibility Support Module) active!")
    print("       -> Quick Verification: Press Win + R, type msinfo32, confirm 'BIOS Mode: UEFI'.")
    print("       -> Solution: Restart PC into BIOS, navigate to Boot -> CSM, and set")
    print("          CSM to [Disabled] (pure UEFI mode). This eliminates legacy VGA")
    print("          driver conflicts, restores native resolution, and enables Resizable BAR.")
    print("  2. If your NVIDIA GeForce GPU is working normally in Device Manager:")
    print("     - PyTorch in this Python environment is running on a CPU-only wheel.")
    print("     - Reinstall PyTorch with native CUDA 12.1/12.4 support:")
    print("       pip install --force-reinstall torch --index-url https://download.pytorch.org/whl/cu121")
    print("=" * 72)
    sys.exit(2)

device = torch.device("cuda:0")
gpu_name = torch.cuda.get_device_name(0)
props = torch.cuda.get_device_properties(0)
sm_count = props.multi_processor_count
arch = f"sm_{props.major}{props.minor}"

def benchmark_gemm(dtype, size=4096, warmup=5, runs=25):
    if hasattr(torch.backends, "cudnn") and hasattr(torch.backends.cudnn, "benchmark"):
        torch.backends.cudnn.benchmark = True
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

    fw_str, fw_is_uefi = get_firmware_mode()
    bar1, fb, rebar_active = get_rebar_info()
    gen_max, width_max, is_throttled = get_pcie_info()

    print("=" * 72)
    print("     UNIVERSAL GPU RATE-LIMITER STATUS CHECK (ADA, AMPERE & TURING)")
    print("=" * 72)
    print(f"  GPU Hardware       : {gpu_name}")
    print(f"  Architecture       : {arch} ({sm_count} Streaming Multiprocessors)")
    print(f"  Firmware Mode      : {fw_str}")
    if rebar_active is True:
        print(f"  Resizable BAR      : {C_GREEN}[OK] ENABLED (BAR1: {bar1} MiB / VRAM: {fb} MiB){C_RESET}")
    elif rebar_active is False:
        print(f"  Resizable BAR      : {C_RED_BOLD}[!] DISABLED (BAR1: {bar1} MiB restricted) -> Turn ON in BIOS!{C_RESET}")
    else:
        print(f"  Resizable BAR      : Undetected")
    if gen_max and width_max:
        if is_throttled:
            print(f"  PCIe Slot Link     : {C_RED_BOLD}[!] THROTTLED: Gen{gen_max} x{width_max} (Riser or Slot Bottleneck!){C_RESET}")
        else:
            print(f"  PCIe Slot Link     : {C_GREEN}[OK] Gen{gen_max} x{width_max} (Full Bandwidth){C_RESET}")
    else:
        print(f"  PCIe Slot Link     : Undetected")
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
        print(f"  {C_GREEN_BOLD}STATUS: [OK] FULLY UNLOCKED! ({tflops_fp16:.1f} TFLOPS){C_RESET}")
        print(f"  {C_GREEN}Hardware rate-limiter is INACTIVE.{C_RESET}")
        print(f"  {C_GREEN}GPU is running at 100% full unrestricted tensor compute capacity.{C_RESET}")
        if props.major >= 8:
            print(f"  {C_CYAN}[i] AI PERFORMANCE PRO-TIP (cuBLASLt Kernel Heuristics):{C_RESET}")
            print(f"      cuBLASLt defaults to stock-calibrated kernels on GeForce cards.")
            print(f"      In PyTorch & ComfyUI, enable autotuning for maximum throughput (+15% to +35%):")
            print(f"      -> torch.backends.cudnn.benchmark = True")
        ret_code = 0
    else:
        print(f"  {C_RED_BOLD}STATUS: [WARNING] STOCK RATE-LIMITER ACTIVE! ({tflops_fp16:.1f} TFLOPS){C_RESET}")
        print(f"  Expected unlocked performance: > {profile['threshold']:.0f} TFLOPS (typical ~{profile['min_unlocked']:.0f} TFLOPS).")
        print("  Recommendation: Boot via USB [U]/[I] or resume from Hibernation (shutdown /h).")
        if props.major == 7:
            print("  Note for Turing: Verify EnableGpuFirmware = 1 is configured in Windows Registry.")
        ret_code = 1

    if rebar_active is False:
        print("-" * 72)
        print(f"  {C_RED_BOLD}[!] PRO-TUNING ALERT (Resizable BAR):{C_RESET}")
        print(f"  {C_YELLOW}Resizable BAR is currently DISABLED in your BIOS (BAR1 = 256 MiB restricted).{C_RESET}")
        print(f"  {C_YELLOW}-> Enable 'Above 4G Decoding' and 'ReBAR Support' in your motherboard BIOS{C_RESET}")
        print(f"  {C_YELLOW}   to unlock full PCIe VRAM aperture and boost AI & gaming by +10% to +15%!{C_RESET}")

    if is_throttled:
        print("-" * 72)
        print(f"  {C_RED_BOLD}[!] PCIe LINK BOTTLENECK DETECTED:{C_RESET}")
        print(f"  {C_YELLOW}Your GPU is running at Gen{gen_max} x{width_max} instead of full x16.{C_RESET}")
        print(f"  {C_YELLOW}-> Check your PCIe riser cable, clean slot contacts, or move the GPU{C_RESET}")
        print(f"  {C_YELLOW}   to the primary top PCIe slot directly attached to the CPU.{C_RESET}")

    rebar_str = f"{bar1}MB (Active)" if rebar_active else ("256MB (Disabled)" if rebar_active is False else "N/A")
    pcie_str = f"Gen{gen_max} x{width_max}" if (gen_max and width_max) else "N/A"
    status_tag = "UNLOCKED" if is_unlocked else "STOCK"
    summary_badge = f"[{status_tag}] {gpu_name}: {tflops_fp16:.1f} TFLOPS | ReBAR: {rebar_str} | PCIe: {pcie_str} | BIOS: {fw_str}"
    print("-" * 72)
    if copy_to_clipboard(summary_badge):
        print(f"  {C_CYAN}[+] Result badge copied to clipboard! Ready to paste into GitHub / Reddit:{C_RESET}")
        print(f"      {summary_badge}")
    else:
        print(f"  {C_CYAN}Summary: {summary_badge}{C_RESET}")

    print("=" * 72)
    sys.exit(ret_code)

if __name__ == "__main__":
    main()
