"""
Universal GPU Rate-Limiter Dashboard & Benchmark GUI
SPDX-License-Identifier: GPL-2.0-only
"""
import os
import subprocess
import sys
import threading
import tkinter as tk
from tkinter import ttk

# Shared helpers live next to this script (tools/gu_system_info.py, tools/dashboard_i18n.py).
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dashboard_i18n import (  # noqa: E402
    LANGUAGE_LABELS,
    SUPPORTED_LANGUAGES,
    load_saved_language,
    save_saved_language,
    t,
    translate_s3_status,
    ui_font,
)
from gu_system_info import (  # noqa: E402
    apply_efficiency_profile,
    apply_saved_power_limits,
    benchmark_gemm,
    classify_unlock_status,
    configure_wake_reapply_task,
    export_latest_boot_logs,
    find_nvidia_smi,
    get_clock_cap_info,
    get_firmware_mode,
    get_gpu_utilization,
    get_last_wake_time,
    get_power_limit_info,
    get_rebar_info,
    get_s3_armed_status,
    load_benchmark_baseline,
    load_user_config,
    measure_fmla_ratio,
    reset_sm_clock_cap,
    resolve_card_profile,
    save_benchmark_baseline,
    set_power_limit,
    set_sm_clock_cap,
    snap_clock_mhz,
)


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
                    "torch_ok": True,
                })
            if gpus:
                return gpus
    except Exception:
        pass

    # Fallback to nvidia-smi if torch not imported
    try:
        smi_exe = find_nvidia_smi() or "nvidia-smi"
        kwargs = {"text": True}
        if os.name == "nt":
            kwargs["creationflags"] = getattr(subprocess, "CREATE_NO_WINDOW", 0)
        out = subprocess.check_output(
            [smi_exe, "--query-gpu=index,name,memory.total", "--format=csv,noheader"],
            **kwargs,
        )
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
                    "torch_ok": False,
                })
        if gpus:
            return gpus
    except Exception:
        pass

    return [{
        "index": 0,
        "name": None,
        "arch": "Unknown",
        "sm": 0,
        "vram": "N/A",
        "props": None,
        "torch_ok": False,
    }]


class GpuUnlockApp(tk.Tk):
    def __init__(self):
        super().__init__()
        self.lang = load_saved_language()
        self.title(self._t("window_title"))
        self.geometry("1020x960")
        self.minsize(940, 880)
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
        self.s3_status = get_s3_armed_status()
        self.bar1, self.fb, self.rebar_active = get_rebar_info(self.gpu_info["index"])
        self.pl_info = get_power_limit_info(self.gpu_info["index"])
        self.clk_info = get_clock_cap_info(self.gpu_info["index"], self._gpu_display_name())
        self.last_metrics = None
        self.last_benchmark_args = None
        self.benchmark_running = False
        self._slider_syncing = False
        self._clk_slider_syncing = False
        self.lang_buttons = {}

        self.build_ui()
        self.apply_language_texts(initial=True)
        self.after(15000, self._poll_wake_event)

    def _t(self, key, **kwargs):
        return t(self.lang, key, **kwargs)

    def _font(self, size, weight=None):
        return ui_font(self.lang, size, weight=weight)

    def _gpu_display_name(self):
        return self.gpu_info.get("name") or self._t("gpu_not_detected")

    def get_repo_dir(self):
        return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    def get_repo_path(self, rel_path):
        repo_dir = self.get_repo_dir()
        p = os.path.join(repo_dir, rel_path)
        if os.path.exists(p):
            return p
        p_tools = os.path.join(os.path.dirname(os.path.abspath(__file__)), os.path.basename(rel_path))
        if os.path.exists(p_tools):
            return p_tools
        p_root = os.path.join(repo_dir, os.path.basename(rel_path))
        if os.path.exists(p_root):
            return p_root
        return p

    def _s3_card_colors(self):
        code = self.s3_status.get("code")
        if code == "ARMED":
            return self.c_green
        if code in ("S0IX_WARN",):
            return self.c_yellow
        if code == "UNVERIFIED":
            return "#40C4FF" if self.fw_is_uefi else self.c_red
        return self.c_red

    def _s3_card_headline(self):
        headline, _ = translate_s3_status(self.lang, self.s3_status, self.fw_is_uefi)
        return headline

    def _rebar_texts(self, include_bios_hint=True):
        if self.rebar_active:
            txt = self._t("rebar_enabled")
            color = self.c_green
        elif self.rebar_active is False:
            txt = self._t("rebar_disabled")
            color = self.c_red
        else:
            txt = self._t("rebar_undetected")
            color = self.c_text_dim

        if self.bar1:
            if include_bios_hint and self.rebar_active is False:
                detail = self._t("rebar_bar1_bios", bar1=self.bar1)
            else:
                detail = self._t("rebar_bar1_short", bar1=self.bar1)
        else:
            detail = self._t("rebar_unavailable")
        return txt, color, detail

    def build_ui(self):
        header = tk.Frame(self, bg=self.c_bg, padx=20, pady=12)
        header.pack(fill="x")

        title_frame = tk.Frame(header, bg=self.c_bg)
        title_frame.pack(side="left", fill="both")

        self.lbl_header_title = tk.Label(
            title_frame,
            text=self._t("header_title"),
            font=self._font(16, "bold"),
            fg=self.c_text_bright,
            bg=self.c_bg,
        )
        self.lbl_header_title.pack(anchor="w")

        self.lbl_header_subtitle = tk.Label(
            title_frame,
            text=self._t("header_subtitle"),
            font=self._font(10),
            fg=self.c_text_dim,
            bg=self.c_bg,
        )
        self.lbl_header_subtitle.pack(anchor="w")

        # Compact Top-Right Language Switcher (EN / 简体中文 / DE)
        lang_frame = tk.Frame(header, bg=self.c_bg)
        lang_frame.pack(side="right", padx=(10, 0), anchor="n")
        for code in SUPPORTED_LANGUAGES:
            lbl = LANGUAGE_LABELS[code]
            btn = tk.Button(
                lang_frame,
                text=lbl,
                font=ui_font(code, 8, "bold"),
                relief="flat",
                padx=8,
                pady=3,
                cursor="hand2",
                command=lambda c=code: self.set_language(c),
            )
            btn.pack(side="left", padx=2)
            self.lang_buttons[code] = btn

        if len(self.gpus) > 1:
            sel_frame = tk.Frame(header, bg=self.c_bg)
            sel_frame.pack(side="right", padx=10)
            self.lbl_active_device = tk.Label(
                sel_frame,
                text=self._t("active_device"),
                font=self._font(9, "bold"),
                fg=self.c_text_dim,
                bg=self.c_bg,
            )
            self.lbl_active_device.pack(anchor="e")
            self.gpu_combo = ttk.Combobox(
                sel_frame,
                values=[f"GPU {g['index']}: {g['name'] or self._t('gpu_not_detected')}" for g in self.gpus],
                state="readonly",
                width=28,
            )
            self.gpu_combo.current(0)
            self.gpu_combo.bind("<<ComboboxSelected>>", self.on_gpu_selected)
            self.gpu_combo.pack(anchor="e")

        container = tk.Frame(self, bg=self.c_bg, padx=20)
        container.pack(fill="both", expand=True)

        # Status Cards Frame
        cards_frame = tk.Frame(container, bg=self.c_bg)
        cards_frame.pack(fill="x", pady=4)
        cards_frame.columnconfigure((0, 1, 2), weight=1, uniform="card")

        # Card 1: GPU Identity
        self.card_gpu = self.create_card(cards_frame, 0, self._t("card_hw_title"), self._gpu_display_name())
        self.lbl_card_title_gpu = self._last_card_title
        self.lbl_arch = tk.Label(
            self.card_gpu,
            text=self._t("hw_arch_vram", arch=self.gpu_info["arch"], sm=self.gpu_info["sm"], vram=self.gpu_info["vram"]),
            font=self._font(9), fg=self.c_text_dim, bg=self.c_card,
        )
        self.lbl_arch.pack(anchor="w", padx=12, pady=(0, 10))

        # Card 2: Firmware & S3 Persistence Live Traffic Light
        self.card_fw = self.create_card(
            cards_frame, 1, self._t("card_s3_title"), self._s3_card_headline(), val_color=self._s3_card_colors()
        )
        self.lbl_card_title_s3 = self._last_card_title
        self.lbl_card_val_s3 = self._last_card_val
        self.lbl_fw_detail = tk.Label(
            self.card_fw, text=self.firmware_detail_text(),
            font=self._font(8), fg=self.c_text_dim, bg=self.c_card, wraplength=270, justify="left",
        )
        self.lbl_fw_detail.pack(anchor="w", padx=12, pady=(0, 10))

        # Card 3: Resizable BAR
        rebar_txt, rebar_color, bar_sub = self._rebar_texts(include_bios_hint=True)
        self.card_rebar = self.create_card(cards_frame, 2, self._t("card_rebar_title"), rebar_txt, val_color=rebar_color)
        self.lbl_card_title_rebar = self._last_card_title
        self.lbl_card_val_rebar = self._last_card_val
        self.lbl_rebar_detail = tk.Label(
            self.card_rebar, text=bar_sub,
            font=self._font(9), fg=self.c_text_dim, bg=self.c_card,
        )
        self.lbl_rebar_detail.pack(anchor="w", padx=12, pady=(0, 10))

        # Benchmark Display Box
        bench_box = tk.Frame(container, bg=self.c_card, highlightbackground=self.c_card_border, highlightthickness=1)
        bench_box.pack(fill="x", pady=8)

        bench_header = tk.Frame(bench_box, bg=self.c_card, padx=16, pady=8)
        bench_header.pack(fill="x")

        self.lbl_bench_title = tk.Label(
            bench_header, text=self._t("bench_title"), font=self._font(11, "bold"),
            fg=self.c_text_bright, bg=self.c_card,
        )
        self.lbl_bench_title.pack(side="left")

        self.lbl_status_badge = tk.Label(
            bench_header, text=self._t("badge_ready"), font=self._font(10, "bold"),
            fg=self.c_text_bright, bg="#333333", padx=10, pady=4,
        )
        self.lbl_status_badge.pack(side="right")

        tflops_center = tk.Frame(bench_box, bg=self.c_card, pady=4)
        tflops_center.pack(fill="x")

        self.lbl_tflops_big = tk.Label(
            tflops_center, text="--.- TFLOPS", font=self._font(34, "bold"),
            fg=self.c_text_bright, bg=self.c_card,
        )
        self.lbl_tflops_big.pack()

        self.lbl_tflops_subtitle = tk.Label(
            tflops_center, text=self._t("bench_tflops_default_sub"),
            font=self._font(9), fg=self.c_text_dim, bg=self.c_card,
        )
        self.lbl_tflops_subtitle.pack()

        self.lbl_baseline_delta = tk.Label(
            tflops_center, text=self.baseline_summary_text(),
            font=self._font(9, "bold"), fg="#82B1FF", bg=self.c_card,
        )
        self.lbl_baseline_delta.pack(pady=(2, 0))

        metrics_frame = tk.Frame(bench_box, bg=self.c_card, padx=20, pady=8)
        metrics_frame.pack(fill="x")
        metrics_frame.columnconfigure((0, 1, 2, 3), weight=1)

        self.lbl_m_fp16, self.lbl_m_title_fp16 = self.create_metric_col(metrics_frame, 0, self._t("metric_fp16"), "--")
        self.lbl_m_bf16, self.lbl_m_title_bf16 = self.create_metric_col(metrics_frame, 1, self._t("metric_bf16"), "--")
        self.lbl_m_tf32, self.lbl_m_title_tf32 = self.create_metric_col(metrics_frame, 2, self._t("metric_tf32"), "--")
        self.lbl_m_fp32, self.lbl_m_title_fp32 = self.create_metric_col(metrics_frame, 3, self._t("metric_fp32"), "--")

        # Power Limit Control Card (Stepless Slider + Eco / Stock / Max Markers + Wake Re-apply)
        pl_box = tk.Frame(container, bg=self.c_card, highlightbackground=self.c_card_border, highlightthickness=1, padx=14, pady=10)
        pl_box.pack(fill="x", pady=6)

        pl_top = tk.Frame(pl_box, bg=self.c_card)
        pl_top.pack(fill="x")

        self.lbl_pl_title = tk.Label(
            pl_top, text=self._t("pl_title"), font=self._font(10, "bold"),
            fg=self.c_text_bright, bg=self.c_card,
        )
        self.lbl_pl_title.pack(side="left")

        self.lbl_pl_live = tk.Label(
            pl_top, text=self._t("pl_querying"), font=self._font(9),
            fg=self.c_text_dim, bg=self.c_card,
        )
        self.lbl_pl_live.pack(side="right")

        pl_slider_row = tk.Frame(pl_box, bg=self.c_card, pady=4)
        pl_slider_row.pack(fill="x")

        self.pl_var = tk.IntVar(value=0)
        self.pl_scale = tk.Scale(
            pl_slider_row,
            from_=100,
            to=500,
            resolution=1,
            orient="horizontal",
            variable=self.pl_var,
            showvalue=False,
            bg=self.c_card,
            fg=self.c_text_bright,
            troughcolor="#2E2E2E",
            activebackground="#2979FF",
            highlightthickness=0,
            bd=0,
            command=self.on_pl_slider_move,
        )
        self.pl_scale.pack(side="left", fill="x", expand=True, padx=(0, 12))

        self.lbl_pl_preview = tk.Label(
            pl_slider_row, text="-- W (--%)", font=self._font(10, "bold"),
            fg="#FFD600", bg=self.c_card, width=16, anchor="e",
        )
        self.lbl_pl_preview.pack(side="left", padx=(0, 8))

        self.btn_pl_apply = tk.Button(
            pl_slider_row, text=self._t("btn_apply_pl"), font=self._font(9, "bold"),
            bg="#1565C0", fg="#FFFFFF", activebackground="#1E88E5", activeforeground="#FFFFFF",
            relief="flat", padx=12, pady=4, cursor="hand2", command=self.apply_slider_power_limit,
        )
        self.btn_pl_apply.pack(side="right")

        pl_markers_row = tk.Frame(pl_box, bg=self.c_card)
        pl_markers_row.pack(fill="x", pady=(2, 0))

        self.btn_pl_eco = tk.Button(
            pl_markers_row, text=self._t("btn_eco"), font=self._font(8, "bold"),
            bg="#263238", fg="#80CBC4", activebackground="#37474F", relief="flat", padx=8, pady=3,
            cursor="hand2", command=lambda: self.snap_power_marker("eco"),
        )
        self.btn_pl_eco.pack(side="left", padx=(0, 4))

        self.btn_pl_stock = tk.Button(
            pl_markers_row, text=self._t("btn_stock"), font=self._font(8, "bold"),
            bg="#263238", fg="#FFFFFF", activebackground="#37474F", relief="flat", padx=8, pady=3,
            cursor="hand2", command=lambda: self.snap_power_marker("stock"),
        )
        self.btn_pl_stock.pack(side="left", padx=4)

        self.btn_pl_max = tk.Button(
            pl_markers_row, text=self._t("btn_max"), font=self._font(8, "bold"),
            bg="#263238", fg="#FFAB91", activebackground="#37474F", relief="flat", padx=8, pady=3,
            cursor="hand2", command=lambda: self.snap_power_marker("max"),
        )
        self.btn_pl_max.pack(side="left", padx=4)

        self.wake_reapply_var = tk.BooleanVar(
            value=bool(
                load_user_config().get("power_limit", {}).get("reapply_on_wake", False)
                or load_user_config().get("clock_lock", {}).get("reapply_on_wake", False)
            )
        )
        self.chk_wake_reapply = tk.Checkbutton(
            pl_markers_row,
            text=self._t("chk_wake_reapply"),
            variable=self.wake_reapply_var,
            command=self.on_wake_reapply_toggled,
            bg=self.c_card,
            fg=self.c_text_bright,
            selectcolor="#121212",
            activebackground=self.c_card,
            activeforeground=self.c_text_bright,
            font=self._font(8),
        )
        self.chk_wake_reapply.pack(side="left", padx=12)

        self.btn_prof_ai = tk.Button(
            pl_markers_row, text=self._t("btn_ai_eff"), font=self._font(8, "bold"),
            bg="#00695C", fg="#FFFFFF", activebackground="#00897B", relief="flat", padx=8, pady=3,
            cursor="hand2", command=lambda: self.apply_quick_profile("ai"),
        )
        self.btn_prof_ai.pack(side="right", padx=(4, 0))

        self.btn_prof_gaming = tk.Button(
            pl_markers_row, text=self._t("btn_gaming_stock"), font=self._font(8, "bold"),
            bg="#2E2E2E", fg="#FFFFFF", activebackground="#424242", relief="flat", padx=8, pady=3,
            cursor="hand2", command=lambda: self.apply_quick_profile("gaming"),
        )
        self.btn_prof_gaming.pack(side="right", padx=4)

        # SM Clock Cap Card (-lgc): beats -pl power throttling in render speed & Wh
        clk_box = tk.Frame(container, bg=self.c_card, highlightbackground=self.c_card_border, highlightthickness=1, padx=14, pady=9)
        clk_box.pack(fill="x", pady=(2, 6))

        clk_top = tk.Frame(clk_box, bg=self.c_card)
        clk_top.pack(fill="x")

        self.lbl_clk_title = tk.Label(
            clk_top, text=self._t("clk_title"), font=self._font(10, "bold"),
            fg=self.c_text_bright, bg=self.c_card,
        )
        self.lbl_clk_title.pack(side="left")

        self.lbl_clk_live = tk.Label(
            clk_top, text=self._t("clk_querying"), font=self._font(9),
            fg=self.c_text_dim, bg=self.c_card,
        )
        self.lbl_clk_live.pack(side="right")

        clk_slider_row = tk.Frame(clk_box, bg=self.c_card, pady=3)
        clk_slider_row.pack(fill="x")

        self.clk_var = tk.IntVar(value=2295)
        self.clk_scale = tk.Scale(
            clk_slider_row,
            from_=210,
            to=3105,
            resolution=15,
            orient="horizontal",
            variable=self.clk_var,
            showvalue=False,
            bg=self.c_card,
            fg=self.c_text_bright,
            troughcolor="#2E2E2E",
            activebackground="#00897B",
            highlightthickness=0,
            bd=0,
            command=self.on_clk_slider_move,
        )
        self.clk_scale.pack(side="left", fill="x", expand=True, padx=(0, 12))

        self.lbl_clk_preview = tk.Label(
            clk_slider_row, text="---- MHz", font=self._font(10, "bold"),
            fg="#80CBC4", bg=self.c_card, width=22, anchor="e",
        )
        self.lbl_clk_preview.pack(side="left", padx=(0, 8))

        self.btn_clk_reset = tk.Button(
            clk_slider_row, text=self._t("btn_clk_reset"), font=self._font(9),
            bg="#37474F", fg="#FFFFFF", activebackground="#455A64", activeforeground="#FFFFFF",
            relief="flat", padx=10, pady=4, cursor="hand2", command=self.reset_clock_cap_clicked,
        )
        self.btn_clk_reset.pack(side="right", padx=(4, 0))

        self.btn_clk_apply = tk.Button(
            clk_slider_row, text=self._t("btn_clk_apply"), font=self._font(9, "bold"),
            bg="#00695C", fg="#FFFFFF", activebackground="#00897B", activeforeground="#FFFFFF",
            relief="flat", padx=12, pady=4, cursor="hand2", command=self.apply_slider_clock_cap,
        )
        self.btn_clk_apply.pack(side="right")

        clk_markers_row = tk.Frame(clk_box, bg=self.c_card)
        clk_markers_row.pack(fill="x", pady=(2, 0))

        self.btn_clk_eff = tk.Button(
            clk_markers_row, text=self._t("btn_clk_max_eff"), font=self._font(8, "bold"),
            bg="#263238", fg="#80CBC4", activebackground="#37474F", relief="flat", padx=8, pady=3,
            cursor="hand2", command=lambda: self.snap_clock_marker("efficiency"),
        )
        self.btn_clk_eff.pack(side="left", padx=(0, 4))

        self.btn_clk_sweet = tk.Button(
            clk_markers_row, text=self._t("btn_clk_sweetspot"), font=self._font(8, "bold"),
            bg="#263238", fg="#FFD54F", activebackground="#37474F", relief="flat", padx=8, pady=3,
            cursor="hand2", command=lambda: self.snap_clock_marker("sweetspot"),
        )
        self.btn_clk_sweet.pack(side="left", padx=4)

        self.lbl_clk_tip = tk.Label(
            clk_markers_row,
            text=self._t("clk_tip"),
            font=self._font(8),
            fg=self.c_text_dim,
            bg=self.c_card,
        )
        self.lbl_clk_tip.pack(side="right")

        # Progress / Status Log Line
        self.lbl_log = tk.Label(
            container, text=self._t("log_initial"),
            font=self._font(9, "italic"), fg=self.c_text_dim, bg=self.c_bg,
        )
        self.lbl_log.pack(anchor="w", pady=(2, 4))

        # Action Buttons Frame (Row 1: Primary Controls)
        btn_frame1 = tk.Frame(container, bg=self.c_bg)
        btn_frame1.pack(fill="x", pady=4)

        self.btn_run = tk.Button(
            btn_frame1, text=self._t("btn_run_bench"), font=self._font(11, "bold"),
            bg="#00C853", fg="#FFFFFF", activebackground="#00E676", activeforeground="#FFFFFF",
            relief="flat", padx=16, pady=8, cursor="hand2", command=self.start_benchmark_thread,
        )
        self.btn_run.pack(side="left", padx=(0, 6))

        self.btn_save_baseline = tk.Button(
            btn_frame1, text=self._t("btn_save_baseline"), font=self._font(9, "bold"),
            bg="#2A2A2A", fg=self.c_text_bright, activebackground="#3A3A3A", activeforeground="#FFFFFF",
            relief="flat", padx=10, pady=8, cursor="hand2", command=self.save_current_baseline,
            state="disabled",
        )
        self.btn_save_baseline.pack(side="left", padx=4)

        self.btn_refresh = tk.Button(
            btn_frame1, text=self._t("btn_refresh"), font=self._font(10),
            bg="#2A2A2A", fg=self.c_text_bright, activebackground="#3A3A3A", activeforeground="#FFFFFF",
            relief="flat", padx=10, pady=8, cursor="hand2", command=self.refresh_all,
        )
        self.btn_refresh.pack(side="left", padx=4)

        self.guard_active = self.check_idle_guard()
        guard_txt = self._t("btn_guard_active") if self.guard_active else self._t("btn_guard_default")
        guard_col = "#00C853" if self.guard_active else "#2A2A2A"
        self.btn_guard = tk.Button(
            btn_frame1, text=guard_txt, font=self._font(9),
            bg=guard_col, fg=self.c_text_bright, activebackground="#3A3A3A", activeforeground="#FFFFFF",
            relief="flat", padx=10, pady=8, cursor="hand2", command=self.toggle_idle_guard,
        )
        self.btn_guard.pack(side="left", padx=4)

        self.btn_devmgmt = tk.Button(
            btn_frame1, text=self._t("btn_devmgmt"), font=self._font(9),
            bg="#2A2A2A", fg=self.c_text_bright, activebackground="#3A3A3A", activeforeground="#FFFFFF",
            relief="flat", padx=10, pady=8, cursor="hand2", command=lambda: subprocess.Popen(["devmgmt.msc"], shell=True),
        )
        self.btn_devmgmt.pack(side="right")

        self.btn_msinfo = tk.Button(
            btn_frame1, text=self._t("btn_msinfo"), font=self._font(9),
            bg="#2A2A2A", fg=self.c_text_bright, activebackground="#3A3A3A", activeforeground="#FFFFFF",
            relief="flat", padx=10, pady=8, cursor="hand2", command=lambda: subprocess.Popen(["msinfo32.exe"]),
        )
        self.btn_msinfo.pack(side="right", padx=4)

        # Quick Tools & Integrations Frame (Row 2)
        self.tools_box = tk.LabelFrame(
            container, text=self._t("utils_title"),
            font=self._font(9, "bold"), fg=self.c_text_dim, bg=self.c_bg, padx=10, pady=6,
        )
        self.tools_box.pack(fill="x", pady=(6, 6))

        self.btn_turing_gsp = tk.Button(
            self.tools_box, text=self._t("btn_turing_gsp"), font=self._font(9),
            bg="#242424", fg=self.c_text_bright, activebackground="#333333", relief="flat", padx=6, pady=5,
            command=lambda: self.launch_script("tools/Toggle-Turing-GSP.bat"),
        )
        self.btn_turing_gsp.pack(side="left", padx=3)

        self.btn_sleep_shortcut = tk.Button(
            self.tools_box, text=self._t("btn_sleep_shortcut"), font=self._font(9),
            bg="#242424", fg=self.c_text_bright, activebackground="#333333", relief="flat", padx=6, pady=5,
            command=lambda: self.launch_script("tools/Toggle-Sleep-Button.bat"),
        )
        self.btn_sleep_shortcut.pack(side="left", padx=3)

        self.btn_fast_resume = tk.Button(
            self.tools_box, text=self._t("btn_fast_resume"), font=self._font(9),
            bg="#242424", fg=self.c_text_bright, activebackground="#333333", relief="flat", padx=6, pady=5,
            command=lambda: self.launch_script("tools/Setup-FastResume.bat"),
        )
        self.btn_fast_resume.pack(side="left", padx=3)

        self.btn_adv_cc = tk.Button(
            self.tools_box, text=self._t("btn_adv_cc"), font=self._font(9),
            bg="#242424", fg=self.c_text_bright, activebackground="#333333", relief="flat", padx=6, pady=5,
            command=lambda: self.launch_script("tools/Universal-Control-Center.cmd"),
        )
        self.btn_adv_cc.pack(side="left", padx=3)

        self.btn_export_log = tk.Button(
            self.tools_box, text=self._t("btn_export_log"), font=self._font(9),
            bg="#2E7D32", fg="#FFFFFF", activebackground="#388E3C", relief="flat", padx=6, pady=5,
            command=self.export_boot_log_clicked,
        )
        self.btn_export_log.pack(side="left", padx=3)

        self.btn_usb_setup = tk.Button(
            self.tools_box, text=self._t("btn_usb_setup"), font=self._font(9, "bold"),
            bg="#1565C0", fg="#FFFFFF", activebackground="#1E88E5", relief="flat", padx=8, pady=5,
            command=lambda: self.launch_script("Setup.bat"),
        )
        self.btn_usb_setup.pack(side="right", padx=3)

    def _update_lang_buttons(self):
        for code, btn in self.lang_buttons.items():
            active = code == self.lang
            btn.config(
                bg="#1976D2" if active else "#242424",
                fg="#FFFFFF" if active else self.c_text_dim,
                activebackground="#1E88E5" if active else "#333333",
                activeforeground="#FFFFFF",
            )

    def set_language(self, lang_code):
        self.lang = save_saved_language(lang_code)
        self.apply_language_texts(initial=False)
        if not self.benchmark_running and not getattr(self, "_pl_busy", False):
            self.lbl_log.config(
                text=self._t("log_lang_switched", label=LANGUAGE_LABELS[self.lang]),
                fg=self.c_text_dim,
            )

    def apply_language_texts(self, initial=False):
        self._update_lang_buttons()
        self.title(self._t("window_title"))
        self.lbl_header_title.config(text=self._t("header_title"), font=self._font(16, "bold"))
        self.lbl_header_subtitle.config(text=self._t("header_subtitle"), font=self._font(10))

        if hasattr(self, "lbl_active_device"):
            self.lbl_active_device.config(text=self._t("active_device"), font=self._font(9, "bold"))

        # Cards 1-3
        self.lbl_card_title_gpu.config(text=self._t("card_hw_title"), font=self._font(8, "bold"))
        self.lbl_card_val_gpu.config(text=self._gpu_display_name(), font=self._font(10, "bold"))
        self.lbl_arch.config(
            text=self._t("hw_arch_vram", arch=self.gpu_info["arch"], sm=self.gpu_info["sm"], vram=self.gpu_info["vram"]),
            font=self._font(9),
        )

        self.lbl_card_title_s3.config(text=self._t("card_s3_title"), font=self._font(8, "bold"))
        self.lbl_card_val_s3.config(text=self._s3_card_headline(), fg=self._s3_card_colors(), font=self._font(10, "bold"))
        self.lbl_fw_detail.config(text=self.firmware_detail_text(), font=self._font(8))

        rebar_txt, rebar_color, bar_sub = self._rebar_texts(include_bios_hint=True)
        self.lbl_card_title_rebar.config(text=self._t("card_rebar_title"), font=self._font(8, "bold"))
        self.lbl_card_val_rebar.config(text=rebar_txt, fg=rebar_color, font=self._font(10, "bold"))
        self.lbl_rebar_detail.config(text=bar_sub, font=self._font(9))

        # Benchmark box & metrics
        self.lbl_bench_title.config(text=self._t("bench_title"), font=self._font(11, "bold"))
        self.lbl_m_title_fp16.config(text=self._t("metric_fp16"), font=self._font(9))
        self.lbl_m_title_bf16.config(text=self._t("metric_bf16"), font=self._font(9))
        self.lbl_m_title_tf32.config(text=self._t("metric_tf32"), font=self._font(9))
        self.lbl_m_title_fp32.config(text=self._t("metric_fp32"), font=self._font(9))
        self.lbl_m_fp16.config(font=self._font(12, "bold"))
        self.lbl_m_bf16.config(font=self._font(12, "bold"))
        self.lbl_m_tf32.config(font=self._font(12, "bold"))
        self.lbl_m_fp32.config(font=self._font(12, "bold"))
        self.lbl_tflops_big.config(font=self._font(34, "bold"))
        self.lbl_tflops_subtitle.config(font=self._font(9))
        self.lbl_baseline_delta.config(font=self._font(9, "bold"))

        if self.last_benchmark_args is not None:
            self.display_benchmark_results(*self.last_benchmark_args)
        else:
            badge_key = "badge_running" if self.benchmark_running else "badge_ready"
            self.lbl_status_badge.config(text=self._t(badge_key), font=self._font(10, "bold"))
            self.lbl_tflops_subtitle.config(text=self._t("bench_tflops_default_sub"))
            self.lbl_baseline_delta.config(text=self.baseline_summary_text())

        # PL & Clock Cards
        self.lbl_pl_title.config(text=self._t("pl_title"), font=self._font(10, "bold"))
        self.lbl_pl_live.config(font=self._font(9))
        self.lbl_pl_preview.config(font=self._font(10, "bold"))
        self.btn_pl_apply.config(text=self._t("btn_apply_pl"), font=self._font(9, "bold"))
        self.btn_pl_eco.config(font=self._font(8, "bold"))
        self.btn_pl_stock.config(font=self._font(8, "bold"))
        self.btn_pl_max.config(font=self._font(8, "bold"))
        self.chk_wake_reapply.config(text=self._t("chk_wake_reapply"), font=self._font(8))
        self.btn_prof_ai.config(text=self._t("btn_ai_eff"), font=self._font(8, "bold"))
        self.btn_prof_gaming.config(text=self._t("btn_gaming_stock"), font=self._font(8, "bold"))

        self.lbl_clk_title.config(text=self._t("clk_title"), font=self._font(10, "bold"))
        self.lbl_clk_live.config(font=self._font(9))
        self.lbl_clk_preview.config(font=self._font(10, "bold"))
        self.btn_clk_reset.config(text=self._t("btn_clk_reset"), font=self._font(9))
        self.btn_clk_apply.config(text=self._t("btn_clk_apply"), font=self._font(9, "bold"))
        self.btn_clk_eff.config(font=self._font(8, "bold"))
        self.btn_clk_sweet.config(font=self._font(8, "bold"))
        self.lbl_clk_tip.config(text=self._t("clk_tip"), font=self._font(8))

        self.sync_power_limit_ui()
        self.sync_clock_cap_ui()

        # Primary Buttons & Utilities
        run_key = "btn_running_bench" if self.benchmark_running else "btn_run_bench"
        self.btn_run.config(text=self._t(run_key), font=self._font(11, "bold"))
        self.btn_save_baseline.config(text=self._t("btn_save_baseline"), font=self._font(9, "bold"))
        self.btn_refresh.config(text=self._t("btn_refresh"), font=self._font(10))
        self.btn_guard.config(
            text=self._t("btn_guard_active") if self.guard_active else self._t("btn_guard_default"),
            font=self._font(9),
        )
        self.btn_devmgmt.config(text=self._t("btn_devmgmt"), font=self._font(9))
        self.btn_msinfo.config(text=self._t("btn_msinfo"), font=self._font(9))

        self.tools_box.config(text=self._t("utils_title"), font=self._font(9, "bold"))
        self.btn_turing_gsp.config(text=self._t("btn_turing_gsp"), font=self._font(9))
        self.btn_sleep_shortcut.config(text=self._t("btn_sleep_shortcut"), font=self._font(9))
        self.btn_fast_resume.config(text=self._t("btn_fast_resume"), font=self._font(9))
        self.btn_adv_cc.config(text=self._t("btn_adv_cc"), font=self._font(9))
        export_key = "btn_exporting_log" if getattr(self, "_export_busy", False) else "btn_export_log"
        self.btn_export_log.config(text=self._t(export_key), font=self._font(9))
        self.btn_usb_setup.config(text=self._t("btn_usb_setup"), font=self._font(9, "bold"))

        self.lbl_log.config(font=self._font(9, "italic"))
        if initial:
            self.lbl_log.config(text=self._t("log_initial"))

    def _set_tuning_controls_state(self, state):
        pl_adj = bool(self.pl_info and self.pl_info.get("adjustable"))
        clk_adj = bool(self.clk_info and self.clk_info.get("adjustable"))
        pl_widgets = (self.pl_scale, self.btn_pl_apply, self.btn_pl_eco, self.btn_pl_stock, self.btn_pl_max)
        clk_widgets = (self.clk_scale, self.btn_clk_apply, self.btn_clk_reset, self.btn_clk_eff, self.btn_clk_sweet)
        for w in pl_widgets:
            w.config(state=state if pl_adj else "disabled")
        for w in clk_widgets:
            w.config(state=state if clk_adj else "disabled")
        any_adj = pl_adj or clk_adj
        for w in (self.chk_wake_reapply, self.btn_prof_ai, self.btn_prof_gaming):
            w.config(state=state if any_adj else "disabled")

    def sync_power_limit_ui(self):
        info = self.pl_info
        adjustable = bool(info and info.get("adjustable"))
        if not adjustable:
            err = info.get("error") if info else self._t("pl_unavailable")
            self.lbl_pl_live.config(text=self._t("pl_locked_err", err=err or "VBIOS"))
            self.lbl_pl_preview.config(text=self._t("pl_preview_locked"), fg=self.c_text_dim)
            self.btn_pl_eco.config(text=self._t("btn_eco"))
            self.btn_pl_stock.config(text=self._t("btn_stock"))
            self.btn_pl_max.config(text=self._t("btn_max"))
            self._set_tuning_controls_state("normal")
            return

        min_w = int(round(info["min_w"]))
        max_w = int(round(info["max_w"]))
        cur_w = int(round(info["current_w"] if info["current_w"] is not None else info["stock_w"]))
        draw_txt = f"{info['draw_w']:.1f} W" if info.get("draw_w") is not None else self._t("na_value")
        self.lbl_pl_live.config(
            text=self._t(
                "pl_readout",
                cur_w=cur_w,
                draw_txt=draw_txt,
                min_w=min_w,
                max_w=max_w,
                stock_w=info["stock_w"],
            )
        )
        self.btn_pl_eco.config(text=self._t("btn_eco_w", w=info["eco_w"]))
        self.btn_pl_stock.config(text=self._t("btn_stock_w", w=info["stock_w"]))
        self.btn_pl_max.config(text=self._t("btn_max_w", w=max_w))

        self._slider_syncing = True
        self.pl_scale.config(from_=min_w, to=max_w)
        self.pl_var.set(cur_w)
        self._slider_syncing = False
        self.update_pl_preview(cur_w)
        self._set_tuning_controls_state("normal")

    def sync_clock_cap_ui(self):
        cinfo = self.clk_info
        adjustable = bool(cinfo and cinfo.get("adjustable"))
        if not adjustable:
            err = cinfo.get("error") if cinfo else self._t("pl_unavailable")
            self.lbl_clk_live.config(text=self._t("clk_unavailable", err=err or self._t("clk_unsupported")))
            self.lbl_clk_preview.config(text=self._t("na_value"), fg=self.c_text_dim)
            self.btn_clk_eff.config(text=self._t("btn_clk_max_eff"))
            self.btn_clk_sweet.config(text=self._t("btn_clk_sweetspot"))
            self._set_tuning_controls_state("normal")
            return

        min_c = int(cinfo["min_sm_mhz"])
        max_c = int(cinfo["max_sm_mhz"])
        cur_sm = cinfo.get("current_sm_mhz")
        cur_txt = f"{cur_sm} MHz" if cur_sm is not None else self._t("na_value")
        sw_cap_txt = self._t("clk_sw_power_cap") if cinfo.get("sw_power_cap") else ""
        if cinfo.get("lock_enabled") and cinfo.get("locked_max_mhz"):
            cap_txt = self._t(
                "clk_cap_locked",
                min_mhz=cinfo["locked_min_mhz"],
                max_mhz=cinfo["locked_max_mhz"],
            )
            slider_val = int(cinfo["locked_max_mhz"])
        else:
            cap_txt = self._t("clk_cap_unlocked")
            slider_val = int(cinfo["sweetspot_mhz"] or max_c)

        self.lbl_clk_live.config(
            text=self._t(
                "clk_live_readout",
                cur_txt=cur_txt,
                cap_txt=cap_txt,
                min_c=min_c,
                max_c=max_c,
                sw_cap_txt=sw_cap_txt,
            )
        )
        self.btn_clk_eff.config(text=self._t("btn_clk_max_eff_val", mhz=cinfo["efficiency_mhz"]))
        self.btn_clk_sweet.config(text=self._t("btn_clk_sweetspot_val", mhz=cinfo["sweetspot_mhz"]))

        self._clk_slider_syncing = True
        self.clk_scale.config(from_=min_c, to=max_c)
        self.clk_var.set(slider_val)
        self._clk_slider_syncing = False
        self.update_clk_preview(slider_val)
        self._set_tuning_controls_state("normal")

    def update_clk_preview(self, mhz):
        cinfo = self.clk_info or {}
        snapped = snap_clock_mhz(
            mhz,
            cinfo.get("supported_clocks") or None,
            cinfo.get("min_sm_mhz") or 210,
            cinfo.get("max_sm_mhz") or 3500,
        )
        eff = cinfo.get("efficiency_mhz")
        sweet = cinfo.get("sweetspot_mhz")
        if eff and snapped == eff:
            preview = self._t("clk_preview_max_eff", mhz=snapped)
        elif sweet and snapped == sweet:
            preview = self._t("clk_preview_sweetspot", mhz=snapped)
        else:
            preview = self._t("clk_preview_custom", mhz=snapped)
        self.lbl_clk_preview.config(text=preview, fg="#80CBC4")

    def on_clk_slider_move(self, val):
        if self._clk_slider_syncing:
            return
        try:
            self.update_clk_preview(int(float(val)))
        except Exception:
            pass

    def snap_clock_marker(self, marker):
        if not self.clk_info or not self.clk_info.get("adjustable"):
            return
        target = self.clk_info["efficiency_mhz"] if marker == "efficiency" else self.clk_info["sweetspot_mhz"]
        self.clk_var.set(target)
        self.update_clk_preview(target)
        self.apply_clock_cap_value(target)

    def apply_slider_clock_cap(self):
        self.apply_clock_cap_value(self.clk_var.get())

    def apply_clock_cap_value(self, target_mhz):
        if getattr(self, "_pl_busy", False):
            return
        cinfo = self.clk_info or {}
        snapped = snap_clock_mhz(
            target_mhz,
            cinfo.get("supported_clocks") or None,
            cinfo.get("min_sm_mhz") or 210,
            cinfo.get("max_sm_mhz") or 3500,
        )
        self._pl_busy = True
        self._set_tuning_controls_state("disabled")
        self.lbl_log.config(text=self._t("clk_applying", mhz=snapped), fg=self.c_yellow)
        idx = self.selected_gpu_idx
        repo_dir = self.get_repo_dir()

        def worker():
            ok = False
            readback = self.clk_info
            msg = ""
            wake_msg = None
            try:
                ok, readback, msg = set_sm_clock_cap(idx, snapped, allow_uac=True)
                if ok and self.wake_reapply_var.get():
                    _, wake_msg = configure_wake_reapply_task(
                        True, repo_dir, idx,
                        clock_min_mhz=readback.get("locked_min_mhz"),
                        clock_max_mhz=readback.get("locked_max_mhz"),
                        clock_enabled=True,
                    )
            except Exception as e:
                msg = str(e)
            self.after(0, lambda: self._finish_apply_clock_cap(ok, readback, msg, wake_msg))

        threading.Thread(target=worker, daemon=True).start()

    def reset_clock_cap_clicked(self):
        if getattr(self, "_pl_busy", False):
            return
        self._pl_busy = True
        self._set_tuning_controls_state("disabled")
        self.lbl_log.config(text=self._t("clk_resetting"), fg=self.c_yellow)
        idx = self.selected_gpu_idx

        def worker():
            ok = False
            readback = self.clk_info
            msg = ""
            try:
                ok, readback, msg = reset_sm_clock_cap(idx, allow_uac=True)
            except Exception as e:
                msg = str(e)
            self.after(0, lambda: self._finish_apply_clock_cap(ok, readback, msg, None))

        threading.Thread(target=worker, daemon=True).start()

    def _finish_apply_clock_cap(self, ok, readback, msg, wake_msg=None):
        self._pl_busy = False
        self.clk_info = readback
        self.sync_clock_cap_ui()
        if ok:
            full_msg = f"{msg} | {wake_msg}" if wake_msg else msg
            self.lbl_log.config(text=full_msg, fg=self.c_green)
        else:
            self.lbl_log.config(text=msg, fg=self.c_red)

    def update_pl_preview(self, watts):
        stock_w = (self.pl_info or {}).get("stock_w") or 1
        pct = (float(watts) / float(stock_w)) * 100.0
        self.lbl_pl_preview.config(text=self._t("pl_target", w=int(watts), pct=f"{pct:.0f}"), fg="#FFD600")

    def on_pl_slider_move(self, val):
        if self._slider_syncing:
            return
        try:
            self.update_pl_preview(int(float(val)))
        except Exception:
            pass

    def snap_power_marker(self, marker):
        if not self.pl_info or not self.pl_info.get("adjustable"):
            return
        if marker == "eco":
            target = self.pl_info["eco_w"]
        elif marker == "stock":
            target = self.pl_info["stock_w"]
        else:
            target = int(round(self.pl_info["max_w"]))
        self.pl_var.set(target)
        self.update_pl_preview(target)
        self.apply_power_limit_value(target)

    def apply_slider_power_limit(self):
        self.apply_power_limit_value(self.pl_var.get())

    def apply_power_limit_value(self, target_w, enable_wake_after=None):
        if getattr(self, "_pl_busy", False):
            return
        self._pl_busy = True
        self._set_tuning_controls_state("disabled")
        self.lbl_log.config(text=self._t("pl_applying", w=int(target_w)), fg=self.c_yellow)
        idx = self.selected_gpu_idx
        repo_dir = self.get_repo_dir()

        def worker():
            ok = False
            readback = self.pl_info
            msg = ""
            wake_msg = None
            try:
                ok, readback, msg = set_power_limit(idx, target_w, allow_uac=True)
                if ok:
                    want_wake = enable_wake_after if enable_wake_after is not None else self.wake_reapply_var.get()
                    if want_wake:
                        _, wake_msg = configure_wake_reapply_task(True, repo_dir, idx, target_w)
                    elif enable_wake_after is False:
                        _, wake_msg = configure_wake_reapply_task(False, repo_dir, idx, target_w)
            except Exception as e:
                msg = str(e)
            self.after(0, lambda: self._finish_apply_power_limit(ok, readback, msg, wake_msg))

        threading.Thread(target=worker, daemon=True).start()

    def _finish_apply_power_limit(self, ok, readback, msg, wake_msg):
        self._pl_busy = False
        self.pl_info = readback
        self.sync_power_limit_ui()
        if ok:
            full_msg = f"{msg} | {wake_msg}" if wake_msg else msg
            self.lbl_log.config(text=full_msg, fg=self.c_green)
        else:
            self.lbl_log.config(text=msg, fg=self.c_red)

    def on_wake_reapply_toggled(self):
        if getattr(self, "_pl_busy", False):
            return
        self._pl_busy = True
        self._set_tuning_controls_state("disabled")
        enabled = bool(self.wake_reapply_var.get())
        target = self.pl_var.get() if (self.pl_info and self.pl_info.get("adjustable")) else None
        cinfo = self.clk_info or {}
        clk_enabled = bool(cinfo.get("lock_enabled"))
        clk_min = cinfo.get("locked_min_mhz")
        clk_max = cinfo.get("locked_max_mhz")
        idx = self.selected_gpu_idx
        repo_dir = self.get_repo_dir()

        def worker():
            ok = False
            msg = ""
            try:
                ok, msg = configure_wake_reapply_task(
                    enabled, repo_dir, idx, target,
                    clock_min_mhz=clk_min, clock_max_mhz=clk_max, clock_enabled=clk_enabled,
                )
            except Exception as e:
                msg = str(e)
            self.after(0, lambda: self._finish_wake_toggle(ok, msg))

        threading.Thread(target=worker, daemon=True).start()

    def _finish_wake_toggle(self, ok, msg):
        self._pl_busy = False
        self.sync_power_limit_ui()
        self.sync_clock_cap_ui()
        self.lbl_log.config(text=msg, fg=self.c_green if ok else self.c_yellow)

    def apply_quick_profile(self, mode):
        if getattr(self, "_pl_busy", False):
            return
        pl_adj = bool(self.pl_info and self.pl_info.get("adjustable"))
        clk_adj = bool(self.clk_info and self.clk_info.get("adjustable"))
        if not pl_adj and not clk_adj:
            return

        if mode == "ai":
            target_w = int(round(self.pl_info["max_w"])) if pl_adj else None
            target_clk = self.clk_info["sweetspot_mhz"] if clk_adj else None
            unlock_clks = False
            wake_after = True
        else:
            target_w = int(round(self.pl_info["stock_w"])) if pl_adj else None
            target_clk = None
            unlock_clks = True
            wake_after = False

        if target_w is not None:
            self.pl_var.set(target_w)
            self.update_pl_preview(target_w)
        if target_clk is not None:
            self.clk_var.set(target_clk)
            self.update_clk_preview(target_clk)

        self._pl_busy = True
        self._set_tuning_controls_state("disabled")
        label = self._t("btn_ai_eff") if mode == "ai" else self._t("btn_gaming_stock")
        self.lbl_log.config(text=self._t("pl_preset_applying", preset=label), fg=self.c_yellow)
        idx = self.selected_gpu_idx
        repo_dir = self.get_repo_dir()

        def worker():
            ok = False
            new_pl = self.pl_info
            new_clk = self.clk_info
            msg = ""
            wake_msg = None
            try:
                ok, new_pl, new_clk, msg = apply_efficiency_profile(
                    idx, target_watts=target_w, clock_max_mhz=target_clk,
                    unlock_clocks=unlock_clks, allow_uac=True,
                )
                if ok:
                    _, wake_msg = configure_wake_reapply_task(
                        wake_after, repo_dir, idx, watts=target_w,
                        clock_min_mhz=new_clk.get("locked_min_mhz"),
                        clock_max_mhz=new_clk.get("locked_max_mhz"),
                        clock_enabled=bool(new_clk.get("lock_enabled")),
                    )
            except Exception as e:
                msg = str(e)
            self.after(0, lambda: self._finish_apply_profile(ok, new_pl, new_clk, wake_after, msg, wake_msg))

        threading.Thread(target=worker, daemon=True).start()

    def _finish_apply_profile(self, ok, new_pl, new_clk, wake_after, msg, wake_msg):
        self._pl_busy = False
        self.pl_info = new_pl
        self.clk_info = new_clk
        if ok:
            self.wake_reapply_var.set(wake_after)
        self.sync_power_limit_ui()
        self.sync_clock_cap_ui()
        if ok:
            full_msg = f"{msg} | {wake_msg}" if wake_msg else msg
            self.lbl_log.config(text=full_msg, fg=self.c_green)
        else:
            self.lbl_log.config(text=msg, fg=self.c_red)

    def _poll_wake_event(self):
        try:
            latest_wake = get_last_wake_time()
            if latest_wake and latest_wake != self.s3_wake:
                self.s3_wake = latest_wake
                if self.wake_reapply_var.get():
                    threading.Thread(target=apply_saved_power_limits, daemon=True).start()
                self.refresh_all(from_wake=True)
        except Exception:
            pass
        self.after(15000, self._poll_wake_event)

    def baseline_summary_text(self):
        base = load_benchmark_baseline(self.selected_gpu_idx)
        if not base or not base.get("fp16"):
            return self._t("baseline_none")
        if self.last_metrics and self.last_metrics.get("fp16", 0) > 0:
            delta_pct = ((self.last_metrics["fp16"] - base["fp16"]) / base["fp16"]) * 100.0
            sign = "+" if delta_pct >= 0 else ""
            return self._t(
                "baseline_compared",
                base16=base["fp16"],
                ts=base["saved_at"],
                sign=sign,
                delta=delta_pct,
            )
        return self._t("baseline_saved_only", base16=base["fp16"], ts=base["saved_at"])

    def save_current_baseline(self):
        if not self.last_metrics:
            return
        save_benchmark_baseline(self.selected_gpu_idx, self._gpu_display_name(), self.last_metrics)
        self.lbl_baseline_delta.config(text=self.baseline_summary_text(), fg="#00E676")
        self.lbl_log.config(text=self._t("log_baseline_saved"), fg=self.c_green)

    def on_gpu_selected(self, event=None):
        self.selected_gpu_idx = self.gpu_combo.current()
        self.gpu_info = self.gpus[self.selected_gpu_idx]
        self.lbl_card_val_gpu.config(text=self._gpu_display_name())
        self.lbl_arch.config(
            text=self._t("hw_arch_vram", arch=self.gpu_info["arch"], sm=self.gpu_info["sm"], vram=self.gpu_info["vram"])
        )
        self.bar1, self.fb, self.rebar_active = get_rebar_info(self.gpu_info["index"])
        rebar_txt, rebar_color, bar_sub = self._rebar_texts(include_bios_hint=True)
        self.lbl_card_val_rebar.config(text=rebar_txt, fg=rebar_color)
        self.lbl_rebar_detail.config(text=bar_sub)
        self.pl_info = get_power_limit_info(self.gpu_info["index"])
        self.clk_info = get_clock_cap_info(self.gpu_info["index"], self._gpu_display_name())
        self.sync_power_limit_ui()
        self.sync_clock_cap_ui()
        self.lbl_baseline_delta.config(text=self.baseline_summary_text())

    def firmware_detail_text(self):
        csm = self._t("fw_csm_off") if self.fw_is_uefi else self._t("fw_csm_on")
        wake = f" | {self._t('wake_label', wake=self.s3_wake)}" if self.s3_wake else ""
        _, s3_detail = translate_s3_status(self.lang, self.s3_status, self.fw_is_uefi)
        if not s3_detail:
            s3_detail = self.s3_status.get("detail", "")
        return f"{csm}{wake}\n{s3_detail}"

    def launch_script(self, rel_path):
        """Opens a .bat/.cmd/.ps1 from the repository in its own console window."""
        script = self.get_repo_path(rel_path)
        if not os.path.exists(script):
            self.lbl_log.config(text=self._t("log_not_found", rel_path=rel_path), fg=self.c_red)
            return
        subprocess.Popen(f'start "" cmd /c "{script}"', shell=True)

    def export_boot_log_clicked(self):
        if getattr(self, "_export_busy", False):
            return
        self._export_busy = True
        self.btn_export_log.config(state="disabled", text=self._t("btn_exporting_log"))
        self.lbl_log.config(text=self._t("log_export_searching"), fg=self.c_yellow)
        repo_dir = self.get_repo_dir()

        def worker():
            ok, latest_path, msg = False, None, ""
            try:
                ok, latest_path, msg = export_latest_boot_logs(repo_dir=repo_dir, max_files=10, allow_uac=True)
            except Exception as e:
                msg = str(e)
            self.after(0, lambda: self._finish_export_boot_log(ok, latest_path, msg))

        threading.Thread(target=worker, daemon=True).start()

    def _finish_export_boot_log(self, ok, latest_path, msg):
        self._export_busy = False
        self.btn_export_log.config(state="normal", text=self._t("btn_export_log"))
        if ok and latest_path:
            self.s3_status = get_s3_armed_status()
            self.lbl_card_val_s3.config(text=self._s3_card_headline(), fg=self._s3_card_colors())
            self.lbl_fw_detail.config(text=self.firmware_detail_text())
            self.lbl_log.config(text=msg, fg=self.c_green)
            try:
                if os.name == "nt":
                    subprocess.Popen(["explorer.exe", f"/select,{os.path.normpath(latest_path)}"])
            except Exception:
                pass
        else:
            self.lbl_log.config(text=msg, fg=self.c_yellow)

    def create_card(self, parent, col, title, val_text, val_color=None):
        if val_color is None:
            val_color = self.c_text_bright
        card = tk.Frame(parent, bg=self.c_card, highlightbackground=self.c_card_border, highlightthickness=1)
        card.grid(row=0, column=col, padx=4, sticky="nsew")

        lbl_title = tk.Label(card, text=title, font=self._font(8, "bold"), fg=self.c_text_dim, bg=self.c_card)
        lbl_title.pack(anchor="w", padx=12, pady=(10, 2))
        lbl_val = tk.Label(card, text=val_text, font=self._font(10, "bold"), fg=val_color, bg=self.c_card)
        lbl_val.pack(anchor="w", padx=12, pady=(0, 4))
        self._last_card_title = lbl_title
        self._last_card_val = lbl_val
        if col == 0:
            self.lbl_card_val_gpu = lbl_val
        return card

    def create_metric_col(self, parent, col, title, initial_val):
        frame = tk.Frame(parent, bg=self.c_card)
        frame.grid(row=0, column=col, sticky="nsew")
        lbl_title = tk.Label(frame, text=title, font=self._font(9), fg=self.c_text_dim, bg=self.c_card)
        lbl_title.pack()
        lbl_val = tk.Label(frame, text=initial_val, font=self._font(12, "bold"), fg=self.c_text_bright, bg=self.c_card)
        lbl_val.pack()
        return lbl_val, lbl_title

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
            self.lbl_log.config(text=self._t("log_guard_applying"), fg=self.c_yellow)
            subprocess.Popen(cmd, shell=True)
            self.after(2500, self.refresh_all)
        except Exception as e:
            self.lbl_log.config(text=self._t("log_guard_failed", err=e), fg=self.c_red)

    def refresh_all(self, from_wake=False):
        self.gpus = get_gpu_list()
        if self.selected_gpu_idx >= len(self.gpus):
            self.selected_gpu_idx = 0
        self.gpu_info = self.gpus[self.selected_gpu_idx]
        self.lbl_card_val_gpu.config(text=self._gpu_display_name())
        self.lbl_arch.config(
            text=self._t("hw_arch_vram", arch=self.gpu_info["arch"], sm=self.gpu_info["sm"], vram=self.gpu_info["vram"])
        )

        self.fw_mode, self.fw_is_uefi = get_firmware_mode()
        self.s3_wake = get_last_wake_time()
        self.s3_status = get_s3_armed_status()
        self.lbl_card_val_s3.config(text=self._s3_card_headline(), fg=self._s3_card_colors())
        self.lbl_fw_detail.config(text=self.firmware_detail_text())

        self.bar1, self.fb, self.rebar_active = get_rebar_info(self.gpu_info["index"])
        rebar_txt, rebar_color, bar_sub = self._rebar_texts(include_bios_hint=True)
        self.lbl_card_val_rebar.config(text=rebar_txt, fg=rebar_color)
        self.lbl_rebar_detail.config(text=bar_sub)

        self.pl_info = get_power_limit_info(self.gpu_info["index"])
        self.clk_info = get_clock_cap_info(self.gpu_info["index"], self._gpu_display_name())
        self.sync_power_limit_ui()
        self.sync_clock_cap_ui()
        self.lbl_baseline_delta.config(text=self.baseline_summary_text())

        self.guard_active = self.check_idle_guard()
        if hasattr(self, "btn_guard"):
            self.btn_guard.config(
                text=self._t("btn_guard_active") if self.guard_active else self._t("btn_guard_default"),
                bg="#00C853" if self.guard_active else "#2A2A2A",
            )
        log_key = "log_wake_auto_refresh" if from_wake else "log_refreshed"
        self.lbl_log.config(text=self._t(log_key), fg=self.c_green)

    def start_benchmark_thread(self):
        if self.benchmark_running:
            return
        self.benchmark_running = True
        self.btn_run.config(state="disabled", text=self._t("btn_running_bench"), bg="#555555")
        self.lbl_status_badge.config(text=self._t("badge_running"))
        self.lbl_log.config(text=self._t("log_bench_init"), fg=self.c_yellow)
        threading.Thread(target=self.run_benchmark_worker, daemon=True).start()

    def run_benchmark_worker(self):
        try:
            import torch
            if not torch.cuda.is_available():
                self.after(0, lambda: self.finish_benchmark_error(self._t("log_cuda_unavail")))
                return

            dev_idx = self.selected_gpu_idx
            device = torch.device(f"cuda:{dev_idx}")
            gpu_name = torch.cuda.get_device_name(dev_idx)
            props = torch.cuda.get_device_properties(dev_idx)
            size = 4096

            # Query external load before running PyTorch CUDA GEMM kernels.
            util_info = get_gpu_utilization(dev_idx)
            profile = resolve_card_profile(gpu_name, props.major, props.minor, props.multi_processor_count)

            matmul = getattr(getattr(torch.backends, "cuda", None), "matmul", None)
            has_fmla_flag = matmul is not None and hasattr(matmul, "allow_fp16_reduced_precision_reduction")
            has_tf32 = props.major >= 8 and matmul is not None and hasattr(matmul, "allow_tf32")
            orig_fmla = getattr(matmul, "allow_fp16_reduced_precision_reduction", True) if matmul else True
            orig_tf32 = getattr(matmul, "allow_tf32", True) if matmul else True

            try:
                if has_fmla_flag:
                    matmul.allow_fp16_reduced_precision_reduction = True
                self.after(0, lambda: self.lbl_log.config(text=self._t("bench_step_fp16", idx=dev_idx)))
                ms_fp16, tflops_fp16 = benchmark_gemm(torch, device, torch.float16, size=size, warmup=8, runs=25)

                if has_fmla_flag:
                    self.after(0, lambda: self.lbl_log.config(text=self._t("bench_step_fmla", idx=dev_idx)))
                fmla_info = measure_fmla_ratio(torch, device, size=size, tflops_fmla16=tflops_fp16)

                has_bf16 = props.major >= 8 and hasattr(torch.cuda, "is_bf16_supported") and torch.cuda.is_bf16_supported()
                if has_bf16:
                    self.after(0, lambda: self.lbl_log.config(text=self._t("bench_step_bf16", idx=dev_idx)))
                    ms_bf16, tflops_bf16 = benchmark_gemm(torch, device, torch.bfloat16, size=size, warmup=4, runs=15)
                else:
                    ms_bf16, tflops_bf16 = 0.0, 0.0

                if has_tf32:
                    matmul.allow_tf32 = True
                    self.after(0, lambda: self.lbl_log.config(text=self._t("bench_step_tf32", idx=dev_idx)))
                    ms_tf32, tflops_tf32 = benchmark_gemm(torch, device, torch.float32, size=size, warmup=4, runs=15)
                else:
                    ms_tf32, tflops_tf32 = 0.0, 0.0

                if has_tf32:
                    matmul.allow_tf32 = False
                self.after(0, lambda: self.lbl_log.config(text=self._t("bench_step_fp32", idx=dev_idx)))
                ms_fp32, tflops_fp32 = benchmark_gemm(torch, device, torch.float32, size=size, warmup=4, runs=15)
            finally:
                if has_fmla_flag:
                    matmul.allow_fp16_reduced_precision_reduction = orig_fmla
                if has_tf32:
                    matmul.allow_tf32 = orig_tf32
                torch.cuda.empty_cache()

            verdict = classify_unlock_status(tflops_fp16, profile, util_info, fmla_info, props.major, props.minor)
            ratio = tflops_fp16 / tflops_fp32 if tflops_fp32 > 0 else 1.0

            self.after(0, lambda: self.display_benchmark_results(
                tflops_fp16, ms_fp16, tflops_bf16, ms_bf16, tflops_tf32, ms_tf32, tflops_fp32, ms_fp32,
                verdict, ratio, profile,
            ))

        except Exception as e:
            self.after(0, lambda err=str(e): self.finish_benchmark_error(err))

    def display_benchmark_results(self, fp16, ms16, bf16, msbf, tf32, mstf, fp32, ms32, verdict, ratio, profile):
        self.last_metrics = {"fp16": fp16, "bf16": bf16, "tf32": tf32, "fp32": fp32}
        self.last_benchmark_args = (fp16, ms16, bf16, msbf, tf32, mstf, fp32, ms32, verdict, ratio, profile)
        self.btn_save_baseline.config(state="normal")

        code = verdict["code"]
        fmla_ratio = verdict.get("fmla_ratio")
        gpu_util = verdict.get("gpu_util")

        self.lbl_tflops_big.config(text=f"{fp16:.2f} TFLOPS")
        fmla_sub = self._t("bench_fmla_sub", fmla=fmla_ratio) if fmla_ratio is not None else ""
        load_sub = self._t("bench_pre_load", util=gpu_util) if gpu_util is not None else ""
        self.lbl_tflops_subtitle.config(
            text=self._t("bench_tflops_subtitle", fmla_sub=fmla_sub, load_sub=load_sub)
        )
        self.lbl_m_fp16.config(text=self._t("metric_tflops_ms", val=fp16, ms=ms16))
        self.lbl_m_bf16.config(text=self._t("metric_tflops", val=bf16) if bf16 > 0 else self._t("na_value"))
        self.lbl_m_tf32.config(text=self._t("metric_tflops", val=tf32) if tf32 > 0 else self._t("na_value"))
        self.lbl_m_fp32.config(text=self._t("metric_tflops_ms", val=fp32, ms=ms32))
        self.lbl_baseline_delta.config(text=self.baseline_summary_text(), fg="#82B1FF")

        if code == "UNLOCKED":
            self.lbl_tflops_big.config(fg=self.c_green)
            self.lbl_status_badge.config(text=self._t("badge_status_unlocked"), bg="#00C853", fg="#FFFFFF")
            log_text = self._t("bench_verdict_unlocked", ratio=ratio)
            if verdict.get("under_load") and gpu_util is not None:
                log_text += self._t("bench_verdict_unlocked_ext", util=gpu_util)
            self.lbl_log.config(text=log_text, fg=self.c_green)
        elif code == "UNLOCKED_UNDER_LOAD":
            self.lbl_tflops_big.config(fg=self.c_green)
            self.lbl_status_badge.config(text=self._t("badge_status_unlocked_load"), bg="#00C853", fg="#FFFFFF")
            log_text = self._t("bench_verdict_under_load", util=gpu_util, fmla=fmla_ratio)
            self.lbl_log.config(text=log_text, fg=self.c_yellow)
        elif code == "EXTERNAL_LOAD":
            self.lbl_tflops_big.config(fg=self.c_yellow)
            self.lbl_status_badge.config(
                text=self._t("badge_status_external_load", util=gpu_util), bg="#FFAB00", fg="#121212"
            )
            log_text = self._t("bench_verdict_external_load", util=gpu_util)
            self.lbl_log.config(text=log_text, fg=self.c_yellow)
        elif code == "UNLOCKED_LOW_TFLOPS":
            self.lbl_tflops_big.config(fg=self.c_yellow)
            self.lbl_status_badge.config(text=self._t("badge_status_low_tflops"), bg="#FFAB00", fg="#121212")
            log_text = self._t(
                "bench_verdict_low_tflops",
                fmla=fmla_ratio,
                fp16=fp16,
                thresh=profile["threshold"],
            )
            self.lbl_log.config(text=log_text, fg=self.c_yellow)
        else:
            self.lbl_tflops_big.config(fg=self.c_red)
            self.lbl_status_badge.config(text=self._t("badge_status_rate_limited"), bg="#D50000", fg="#FFFFFF")
            log_text = self._t("bench_verdict_rate_limited", fp16=fp16, thresh=profile["threshold"])
            self.lbl_log.config(text=log_text, fg=self.c_red)

        self.btn_run.config(state="normal", text=self._t("btn_run_bench"), bg="#00C853")
        self.benchmark_running = False

    def finish_benchmark_error(self, err_msg):
        self.lbl_status_badge.config(text=self._t("badge_status_error"), bg="#D50000")
        self.lbl_log.config(text=self._t("bench_error_msg", err=err_msg), fg=self.c_red)
        self.btn_run.config(state="normal", text=self._t("btn_run_bench"), bg="#00C853")
        self.benchmark_running = False


if __name__ == "__main__":
    app = GpuUnlockApp()
    app.mainloop()
