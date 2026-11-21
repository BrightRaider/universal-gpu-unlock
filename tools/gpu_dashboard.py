"""
Universal GPU Rate-Limiter Dashboard & Benchmark GUI
SPDX-License-Identifier: GPL-2.0-only
"""
import sys
import os
import re
import time
import subprocess
import ctypes
import threading
import tkinter as tk
from tkinter import ttk, messagebox

# --- Hardware & System Inspection Helpers ---

# Shared helpers live next to this script (tools/gu_system_info.py).
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gu_system_info import CARD_PROFILES, get_firmware_mode, get_last_wake_time, get_rebar_info  # noqa: E402

def get_gpu_list():
    gpus = []
    try:
        import torch
        if torch.cuda.is_available():
            count = torch.cuda.device_count()
            for i in range(count):
                name = torch.cuda.get_device_name(i)
                props = torch.cuda.get_device_properties(i)
                vram_gb = props.total_memory / (1024**3)
                arch = f"sm_{props.major}{props.minor}"
                sm = props.multi_processor_count
                gpus.append({
                    "index": i,
                    "name": name,
                    "arch": arch,
                    "sm": sm,
                    "vram": f"{vram_gb:.1f} GB",
                    "props": props,
                    "torch_ok": True
                })
            if gpus:
                return gpus
    except Exception:
        pass

    # Fallback to nvidia-smi if torch not imported
    try:
        out = subprocess.check_output(["nvidia-smi", "--query-gpu=index,name,memory.total", "--format=csv,noheader"], text=True)
        for line in out.strip().splitlines():
            parts = [p.strip() for p in line.split(",")]
            if len(parts) >= 3:
                gpus.append({
                    "index": int(parts[0]),
                    "name": parts[1],
                    "arch": "NVIDIA",
                    "sm": 0,
                    "vram": parts[2],
                    "props": None,
                    "torch_ok": False
                })
        if gpus:
            return gpus
    except Exception:
        pass

    return [{
        "index": 0,
        "name": "NVIDIA GPU Not Detected",
        "arch": "Unknown",
        "sm": 0,
        "vram": "N/A",
        "props": None,
        "torch_ok": False
    }]

class GpuUnlockApp(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("Universal GPU Unlock - Status & Diagnostics")
        self.geometry("860x730")
        self.minsize(800, 680)
        self.configure(bg="#121212")

        # Color Palette
        self.c_bg = "#121212"
        self.c_card = "#1E1E1E"
        self.c_card_border = "#2D2D2D"
        self.c_text_bright = "#FFFFFF"
        self.c_text_dim = "#A0A0A0"
        self.c_green = "#00E676"
        self.c_yellow = "#FFD600"
        self.c_red = "#FF5252"
        self.c_blue = "#2979FF"

        self.gpus = get_gpu_list()
        self.selected_gpu_idx = 0
        self.gpu_info = self.gpus[0]
        self.fw_mode, self.fw_is_uefi = get_firmware_mode()
        self.s3_wake = get_last_wake_time()
        self.bar1, self.fb, self.rebar_active = get_rebar_info(self.gpu_info["index"])

        self.benchmark_running = False
        self.build_ui()

    def get_repo_path(self, rel_path):
        repo_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        p = os.path.join(repo_dir, rel_path)
        if os.path.exists(p):
            return p
        p_tools = os.path.join(os.path.dirname(os.path.abspath(__file__)), os.path.basename(rel_path))
        if os.path.exists(p_tools):
            return p_tools
        return p

    def build_ui(self):
        # Header Frame
        header = tk.Frame(self, bg=self.c_bg, padx=20, pady=14)
        header.pack(fill="x")

        title_frame = tk.Frame(header, bg=self.c_bg)
        title_frame.pack(side="left", fill="both")

        title_lbl = tk.Label(
            title_frame,
            text="UNIVERSAL GPU UNLOCK DASHBOARD",
            font=("Segoe UI", 16, "bold"),
            fg=self.c_text_bright,
            bg=self.c_bg
        )
        title_lbl.pack(anchor="w")

        sub_lbl = tk.Label(
            title_frame,
            text="Hardware Rate-Limiter Bypass, Multi-GPU Topology & S3 Sleep Persistence",
            font=("Segoe UI", 10),
            fg=self.c_text_dim,
            bg=self.c_bg
        )
        sub_lbl.pack(anchor="w")

        # Multi-GPU Selector in Header if > 1 GPU
        if len(self.gpus) > 1:
            sel_frame = tk.Frame(header, bg=self.c_bg)
            sel_frame.pack(side="right", padx=10)
            tk.Label(sel_frame, text="Active Device:", font=("Segoe UI", 9, "bold"),
                     fg=self.c_text_dim, bg=self.c_bg).pack(anchor="e")
            self.gpu_combo = ttk.Combobox(
                sel_frame, values=[f"GPU {g['index']}: {g['name']}" for g in self.gpus],
                state="readonly", width=28
            )
            self.gpu_combo.current(0)
            self.gpu_combo.bind("<<ComboboxSelected>>", self.on_gpu_selected)
            self.gpu_combo.pack(anchor="e")

        # Main Content Scrollable / Container
        container = tk.Frame(self, bg=self.c_bg, padx=20)
        container.pack(fill="both", expand=True)

        # Status Cards Frame (Grid)
        cards_frame = tk.Frame(container, bg=self.c_bg)
        cards_frame.pack(fill="x", pady=6)
        cards_frame.columnconfigure((0, 1, 2), weight=1, uniform="card")

        # Card 1: GPU Identity
        self.card_gpu = self.create_card(cards_frame, 0, "GPU HARDWARE", self.gpu_info["name"])
        self.lbl_arch = tk.Label(self.card_gpu, text=f"Arch: {self.gpu_info['arch']} ({self.gpu_info['sm']} SMs) | VRAM: {self.gpu_info['vram']}",
                                 font=("Segoe UI", 9), fg=self.c_text_dim, bg=self.c_card)
        self.lbl_arch.pack(anchor="w", padx=12, pady=(0, 10))

        # Card 2: Firmware & S3 Persistence
        fw_color = self.c_green if self.fw_is_uefi else (self.c_red if self.fw_is_uefi is False else self.c_yellow)
        fw_status = "Pure UEFI (OK)" if self.fw_is_uefi else "Legacy CSM (Warning!)"
        self.card_fw = self.create_card(cards_frame, 1, "BIOS FIRMWARE & S3", fw_status, val_color=fw_color)
        self.lbl_fw_detail = tk.Label(self.card_fw, text=self.firmware_detail_text(),
                                      font=("Segoe UI", 9), fg=self.c_text_dim, bg=self.c_card)
        self.lbl_fw_detail.pack(anchor="w", padx=12, pady=(0, 10))

        # Card 3: Resizable BAR
        rebar_color = self.c_green if self.rebar_active else (self.c_red if self.rebar_active is False else self.c_text_dim)
        rebar_txt = "ENABLED (Full VRAM)" if self.rebar_active else ("DISABLED (256 MB)!" if self.rebar_active is False else "Undetected")
        self.card_rebar = self.create_card(cards_frame, 2, "RESIZABLE BAR (ReBAR)", rebar_txt, val_color=rebar_color)
        bar_sub = f"BAR1: {self.bar1} MiB (Enable in BIOS!)" if (self.rebar_active is False and self.bar1) else (f"BAR1: {self.bar1} MiB" if self.bar1 else "Query unavailable")
        self.lbl_rebar_detail = tk.Label(self.card_rebar, text=bar_sub,
                                         font=("Segoe UI", 9), fg=self.c_text_dim, bg=self.c_card)
        self.lbl_rebar_detail.pack(anchor="w", padx=12, pady=(0, 10))

        # Main Benchmark Display Box
        bench_box = tk.Frame(container, bg=self.c_card, highlightbackground=self.c_card_border, highlightthickness=1)
        bench_box.pack(fill="x", pady=10)

        bench_header = tk.Frame(bench_box, bg=self.c_card, padx=16, pady=10)
        bench_header.pack(fill="x")

        self.lbl_bench_title = tk.Label(bench_header, text="COMPUTE THROUGHPUT STATUS", font=("Segoe UI", 11, "bold"),
                                        fg=self.c_text_bright, bg=self.c_card)
        self.lbl_bench_title.pack(side="left")

        self.lbl_status_badge = tk.Label(
            bench_header, text="READY TO BENCHMARK", font=("Segoe UI", 10, "bold"),
            fg=self.c_text_bright, bg="#333333", padx=10, pady=4
        )
        self.lbl_status_badge.pack(side="right")

        # TFLOPS Display Center
        tflops_center = tk.Frame(bench_box, bg=self.c_card, pady=8)
        tflops_center.pack(fill="x")

        self.lbl_tflops_big = tk.Label(
            tflops_center, text="--.- TFLOPS", font=("Segoe UI", 36, "bold"),
            fg=self.c_text_bright, bg=self.c_card
        )
        self.lbl_tflops_big.pack()

        self.lbl_tflops_subtitle = tk.Label(
            tflops_center, text="FP16 Tensor Core Peak Performance (GEMM 4096x4096)",
            font=("Segoe UI", 10), fg=self.c_text_dim, bg=self.c_card
        )
        self.lbl_tflops_subtitle.pack()

        # Detailed Metrics Table
        metrics_frame = tk.Frame(bench_box, bg=self.c_card, padx=20, pady=10)
        metrics_frame.pack(fill="x")
        metrics_frame.columnconfigure((0, 1, 2, 3), weight=1)

        self.lbl_m_fp16 = self.create_metric_col(metrics_frame, 0, "FP16 Tensor", "--")
        self.lbl_m_bf16 = self.create_metric_col(metrics_frame, 1, "BF16 Tensor", "--")
        self.lbl_m_tf32 = self.create_metric_col(metrics_frame, 2, "TF32 Tensor", "--")
        self.lbl_m_fp32 = self.create_metric_col(metrics_frame, 3, "FP32 CUDA", "--")

        # Progress / Status Log Line
        self.lbl_log = tk.Label(
            container, text="Press 'Run TFLOPS Benchmark' to measure live tensor throughput.",
            font=("Segoe UI", 9, "italic"), fg=self.c_text_dim, bg=self.c_bg
        )
        self.lbl_log.pack(anchor="w", pady=(2, 6))

        # Action Buttons Frame (Row 1: Primary Controls)
        btn_frame1 = tk.Frame(container, bg=self.c_bg)
        btn_frame1.pack(fill="x", pady=4)

        self.btn_run = tk.Button(
            btn_frame1, text="RUN TFLOPS BENCHMARK", font=("Segoe UI", 11, "bold"),
            bg="#00C853", fg="#FFFFFF", activebackground="#00E676", activeforeground="#FFFFFF",
            relief="flat", padx=18, pady=8, cursor="hand2", command=self.start_benchmark_thread
        )
        self.btn_run.pack(side="left", padx=(0, 8))

        btn_refresh = tk.Button(
            btn_frame1, text="Refresh Info", font=("Segoe UI", 10),
            bg="#2A2A2A", fg=self.c_text_bright, activebackground="#3A3A3A", activeforeground="#FFFFFF",
            relief="flat", padx=12, pady=8, cursor="hand2", command=self.refresh_all
        )
        btn_refresh.pack(side="left", padx=4)

        self.guard_active = self.check_idle_guard()
        guard_txt = "Idle Guard: ACTIVE" if self.guard_active else "Idle Guard: DEFAULT"
        guard_col = "#00C853" if self.guard_active else "#2A2A2A"
        self.btn_guard = tk.Button(
            btn_frame1, text=guard_txt, font=("Segoe UI", 10),
            bg=guard_col, fg=self.c_text_bright, activebackground="#3A3A3A", activeforeground="#FFFFFF",
            relief="flat", padx=12, pady=8, cursor="hand2", command=self.toggle_idle_guard
        )
        self.btn_guard.pack(side="left", padx=4)

        btn_devmgr = tk.Button(
            btn_frame1, text="Device Manager", font=("Segoe UI", 10),
            bg="#2A2A2A", fg=self.c_text_bright, activebackground="#3A3A3A", activeforeground="#FFFFFF",
            relief="flat", padx=12, pady=8, cursor="hand2", command=lambda: subprocess.Popen(["devmgmt.msc"], shell=True)
        )
        btn_devmgr.pack(side="right")

        btn_msinfo = tk.Button(
            btn_frame1, text="msinfo32", font=("Segoe UI", 10),
            bg="#2A2A2A", fg=self.c_text_bright, activebackground="#3A3A3A", activeforeground="#FFFFFF",
            relief="flat", padx=12, pady=8, cursor="hand2", command=lambda: subprocess.Popen(["msinfo32.exe"])
        )
        btn_msinfo.pack(side="right", padx=4)

        # Quick Tools & Integrations Frame (Row 2: 1-Click Utilities)
        tools_box = tk.LabelFrame(
            container, text=" 60-Second Quick Utilities & Integrations ",
            font=("Segoe UI", 9, "bold"), fg=self.c_text_dim, bg=self.c_bg, padx=10, pady=8
        )
        tools_box.pack(fill="x", pady=(10, 4))

        btn_gsp = tk.Button(
            tools_box, text="Turing GSP Mode", font=("Segoe UI", 9),
            bg="#242424", fg=self.c_text_bright, activebackground="#333333", relief="flat", padx=10, pady=6,
            command=lambda: self.launch_script("tools/Toggle-Turing-GSP.bat")
        )
        btn_gsp.pack(side="left", padx=4)

        btn_sleep = tk.Button(
            tools_box, text="Sleep Shortcut", font=("Segoe UI", 9),
            bg="#242424", fg=self.c_text_bright, activebackground="#333333", relief="flat", padx=10, pady=6,
            command=lambda: self.launch_script("tools/Toggle-Sleep-Button.bat")
        )
        btn_sleep.pack(side="left", padx=4)

        btn_fastresume = tk.Button(
            tools_box, text="Fast-Resume Setup", font=("Segoe UI", 9),
            bg="#242424", fg=self.c_text_bright, activebackground="#333333", relief="flat", padx=10, pady=6,
            command=lambda: self.launch_script("tools/Setup-FastResume.bat")
        )
        btn_fastresume.pack(side="left", padx=4)

        btn_cc = tk.Button(
            tools_box, text="Universal Control Center", font=("Segoe UI", 9),
            bg="#242424", fg=self.c_text_bright, activebackground="#333333", relief="flat", padx=10, pady=6,
            command=lambda: self.launch_script("tools/Universal-Control-Center.cmd")
        )
        btn_cc.pack(side="left", padx=4)

        btn_usb = tk.Button(
            tools_box, text="Update USB Stick", font=("Segoe UI", 9, "bold"),
            bg="#1565C0", fg="#FFFFFF", activebackground="#1E88E5", relief="flat", padx=12, pady=6,
            command=lambda: self.launch_script("Update-USB.bat")
        )
        btn_usb.pack(side="right", padx=4)

    def on_gpu_selected(self, event=None):
        self.selected_gpu_idx = self.gpu_combo.current()
        self.gpu_info = self.gpus[self.selected_gpu_idx]
        self.lbl_card_val_gpu.config(text=self.gpu_info["name"])
        self.lbl_arch.config(text=f"Arch: {self.gpu_info['arch']} ({self.gpu_info['sm']} SMs) | VRAM: {self.gpu_info['vram']}")
        self.bar1, self.fb, self.rebar_active = get_rebar_info(self.gpu_info["index"])
        bar_sub = f"BAR1: {self.bar1} MiB" if self.bar1 else "Query unavailable"
        self.lbl_rebar_detail.config(text=bar_sub)

    def firmware_detail_text(self):
        csm = "CSM: Disabled" if self.fw_is_uefi else "CSM: Active!"
        return f"{csm} | Last wake: {self.s3_wake}" if self.s3_wake else csm

    def launch_script(self, rel_path):
        """Opens a .bat/.cmd from the repository in its own console window."""
        script = self.get_repo_path(rel_path)
        if not os.path.exists(script):
            self.lbl_log.config(text=f"Not found: {rel_path}", fg=self.c_red)
            return
        subprocess.Popen(f'start "" cmd /c "{script}"', shell=True)

    def create_card(self, parent, col, title, val_text, val_color=None):
        if val_color is None:
            val_color = self.c_text_bright
        card = tk.Frame(parent, bg=self.c_card, highlightbackground=self.c_card_border, highlightthickness=1)
        card.grid(row=0, column=col, padx=4, sticky="nsew")

        tk.Label(card, text=title, font=("Segoe UI", 8, "bold"), fg=self.c_text_dim, bg=self.c_card).pack(anchor="w", padx=12, pady=(10, 2))
        lbl_val = tk.Label(card, text=val_text, font=("Segoe UI", 11, "bold"), fg=val_color, bg=self.c_card)
        lbl_val.pack(anchor="w", padx=12, pady=(0, 4))
        if col == 0:
            self.lbl_card_val_gpu = lbl_val
        return card

    def create_metric_col(self, parent, col, title, initial_val):
        frame = tk.Frame(parent, bg=self.c_card)
        frame.grid(row=0, column=col, sticky="nsew")
        tk.Label(frame, text=title, font=("Segoe UI", 9), fg=self.c_text_dim, bg=self.c_card).pack()
        lbl_val = tk.Label(frame, text=initial_val, font=("Segoe UI", 12, "bold"), fg=self.c_text_bright, bg=self.c_card)
        lbl_val.pack()
        return lbl_val

    def check_idle_guard(self):
        try:
            import winreg
            base_key = r"SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}"
            with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, base_key) as root:
                for i in range(16):
                    try:
                        sub = winreg.EnumKey(root, i)
                        with winreg.OpenKey(root, sub) as k:
                            try:
                                desc, _ = winreg.QueryValueEx(k, "DriverDesc")
                                if "NVIDIA" in str(desc).upper():
                                    val, _ = winreg.QueryValueEx(k, "RMDisableRtd3")
                                    if val == 1:
                                        return True
                            except OSError:
                                pass
                    except OSError:
                        break
        except Exception:
            pass
        return False

    def toggle_idle_guard(self):
        fast_resume = self.get_repo_path("tools/Setup-FastResume.bat")
        target_arg = "--revert" if self.guard_active else "--yes"
        cmd = f'powershell -NoProfile -Command "Start-Process cmd.exe -ArgumentList \'/c \"\"{fast_resume}\"\" {target_arg} --no-pause\' -Verb RunAs -Wait"'
        try:
            self.lbl_log.config(text="Applying Idle Relock Guard configuration...", fg=self.c_yellow)
            subprocess.Popen(cmd, shell=True)
            self.after(2500, self.refresh_all)
        except Exception as e:
            self.lbl_log.config(text=f"Failed to toggle guard: {e}", fg=self.c_red)

    def refresh_all(self):
        self.gpus = get_gpu_list()
        if self.selected_gpu_idx >= len(self.gpus):
            self.selected_gpu_idx = 0
        self.gpu_info = self.gpus[self.selected_gpu_idx]
        self.lbl_card_val_gpu.config(text=self.gpu_info["name"])
        self.lbl_arch.config(text=f"Arch: {self.gpu_info['arch']} ({self.gpu_info['sm']} SMs) | VRAM: {self.gpu_info['vram']}")

        self.fw_mode, self.fw_is_uefi = get_firmware_mode()
        self.s3_wake = get_last_wake_time()
        self.lbl_fw_detail.config(text=self.firmware_detail_text())

        self.bar1, self.fb, self.rebar_active = get_rebar_info(self.gpu_info["index"])
        bar_sub = f"BAR1: {self.bar1} MiB" if self.bar1 else "Query unavailable"
        self.lbl_rebar_detail.config(text=bar_sub)

        self.guard_active = self.check_idle_guard()
        if hasattr(self, "btn_guard"):
            self.btn_guard.config(
                text="Idle Guard: ACTIVE" if self.guard_active else "Idle Guard: DEFAULT",
                bg="#00C853" if self.guard_active else "#2A2A2A"
            )
        self.lbl_log.config(text="Hardware configuration refreshed.", fg=self.c_green)

    def start_benchmark_thread(self):
        if self.benchmark_running:
            return
        self.benchmark_running = True
        self.btn_run.config(state="disabled", text="Running Benchmark...", bg="#555555")
        self.lbl_log.config(text="Initializing CUDA tensors and warming up kernels...", fg=self.c_yellow)
        threading.Thread(target=self.run_benchmark_worker, daemon=True).start()

    def run_benchmark_worker(self):
        try:
            import torch
            if not torch.cuda.is_available():
                self.after(0, lambda: self.finish_benchmark_error("CUDA is not available on this system."))
                return

            dev_idx = self.selected_gpu_idx
            device = torch.device(f"cuda:{dev_idx}")
            gpu_name = torch.cuda.get_device_name(dev_idx)
            props = torch.cuda.get_device_properties(dev_idx)
            size = 4096

            if hasattr(torch.backends, "cudnn") and hasattr(torch.backends.cudnn, "benchmark"):
                torch.backends.cudnn.benchmark = True

            # Lookup Profile
            profile = None
            gpu_lower = gpu_name.lower()
            for key, prof in CARD_PROFILES.items():
                if key in gpu_lower:
                    profile = prof
                    break
            if profile is None:
                threshold = props.multi_processor_count * 1.35 if (props.major == 8 and props.minor == 9) else props.multi_processor_count * 1.15
                profile = {"min_unlocked": threshold * 1.15, "threshold": threshold, "stock_typical": threshold * 0.75}

            def bench(dtype, runs=20, warmup=5):
                a = torch.randn((size, size), dtype=dtype, device=device)
                b = torch.randn((size, size), dtype=dtype, device=device)
                for _ in range(warmup):
                    torch.matmul(a, b)
                torch.cuda.synchronize(device)
                start_ev = torch.cuda.Event(enable_timing=True)
                end_ev = torch.cuda.Event(enable_timing=True)
                start_ev.record()
                for _ in range(runs):
                    torch.matmul(a, b)
                end_ev.record()
                torch.cuda.synchronize(device)
                ms = start_ev.elapsed_time(end_ev) / runs
                tflops = (2.0 * (size**3) / (ms * 1e-3)) / 1e12
                del a, b
                return ms, tflops

            self.after(0, lambda: self.lbl_log.config(text=f"Measuring FP16 Tensor Cores on GPU {dev_idx}..."))
            ms_fp16, tflops_fp16 = bench(torch.float16, runs=25, warmup=8)

            has_bf16 = hasattr(torch.cuda, "is_bf16_supported") and torch.cuda.is_bf16_supported()
            if has_bf16:
                self.after(0, lambda: self.lbl_log.config(text=f"Measuring BF16 Tensor Cores on GPU {dev_idx}..."))
                ms_bf16, tflops_bf16 = bench(torch.bfloat16, runs=15, warmup=4)
            else:
                ms_bf16, tflops_bf16 = 0.0, 0.0

            has_tf32 = props.major >= 8 and hasattr(torch.backends.cuda.matmul, "allow_tf32")
            if has_tf32:
                torch.backends.cuda.matmul.allow_tf32 = True
                self.after(0, lambda: self.lbl_log.config(text=f"Measuring TF32 Tensor Cores on GPU {dev_idx}..."))
                ms_tf32, tflops_tf32 = bench(torch.float32, runs=15, warmup=4)
            else:
                ms_tf32, tflops_tf32 = 0.0, 0.0

            if hasattr(torch.backends.cuda.matmul, "allow_tf32"):
                torch.backends.cuda.matmul.allow_tf32 = False
            self.after(0, lambda: self.lbl_log.config(text=f"Measuring FP32 CUDA Control on GPU {dev_idx}..."))
            ms_fp32, tflops_fp32 = bench(torch.float32, runs=15, warmup=4)

            torch.cuda.empty_cache()

            is_unlocked = tflops_fp16 >= profile["threshold"]
            ratio = tflops_fp16 / tflops_fp32 if tflops_fp32 > 0 else 1.0

            self.after(0, lambda: self.display_benchmark_results(
                tflops_fp16, ms_fp16, tflops_bf16, ms_bf16, tflops_tf32, ms_tf32, tflops_fp32, ms_fp32,
                is_unlocked, ratio, profile
            ))

        except Exception as e:
            self.after(0, lambda: self.finish_benchmark_error(str(e)))

    def display_benchmark_results(self, fp16, ms16, bf16, msbf, tf32, mstf, fp32, ms32, is_unlocked, ratio, profile):
        self.lbl_tflops_big.config(text=f"{fp16:.2f} TFLOPS")
        self.lbl_m_fp16.config(text=f"{fp16:.1f} TFLOPS ({ms16:.2f}ms)")
        self.lbl_m_bf16.config(text=f"{bf16:.1f} TFLOPS" if bf16 > 0 else "N/A")
        self.lbl_m_tf32.config(text=f"{tf32:.1f} TFLOPS" if tf32 > 0 else "N/A")
        self.lbl_m_fp32.config(text=f"{fp32:.1f} TFLOPS ({ms32:.2f}ms)")

        if is_unlocked:
            self.lbl_tflops_big.config(fg=self.c_green)
            self.lbl_status_badge.config(text="STATUS: FULLY UNLOCKED!", bg="#00C853", fg="#FFFFFF")
            log_text = f"Rate-limiter BYPASS CONFIRMED! ({ratio:.2f}x CUDA baseline). cuBLAS autotune active."
            self.lbl_log.config(text=log_text, fg=self.c_green)
        else:
            self.lbl_tflops_big.config(fg=self.c_red)
            self.lbl_status_badge.config(text="STATUS: STOCK RATE-LIMITED", bg="#D50000", fg="#FFFFFF")
            log_text = f"Rate-limiter ACTIVE ({fp16:.1f} TFLOPS). Expected unlocked performance: > {profile['threshold']:.0f} TFLOPS."
            self.lbl_log.config(text=log_text, fg=self.c_red)

        self.btn_run.config(state="normal", text="RUN TFLOPS BENCHMARK", bg="#00C853")
        self.benchmark_running = False

    def finish_benchmark_error(self, err_msg):
        self.lbl_status_badge.config(text="BENCHMARK ERROR", bg="#D50000")
        self.lbl_log.config(text=f"Error: {err_msg}", fg=self.c_red)
        self.btn_run.config(state="normal", text="RUN TFLOPS BENCHMARK", bg="#00C853")
        self.benchmark_running = False

if __name__ == "__main__":
    app = GpuUnlockApp()
    app.mainloop()
