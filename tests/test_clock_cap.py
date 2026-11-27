# SPDX-License-Identifier: GPL-2.0-only
"""Unit tests for SM clock-cap parsing, Ada/Ampere/Turing presets, and wake reapply."""
import os
import re
import string
import sys
import tempfile
import unittest
from unittest import mock

TOOLS_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "tools"))
if TOOLS_DIR not in sys.path:
    sys.path.insert(0, TOOLS_DIR)

import dashboard_i18n as di18n  # noqa: E402
import gu_system_info as gsi  # noqa: E402


SAMPLE_SUPPORTED_4090 = "\n".join(
    ["    Supported Clocks", "        Memory : 10501 MHz"]
    + [f"            Graphics : {mhz} MHz" for mhz in range(3105, 209, -15)]
    + ["        Memory : 810 MHz", "            Graphics : 210 MHz"]
)


class ClockCapTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self._cfg_patch = mock.patch.object(gsi, "get_config_dir", return_value=self._tmp.name)
        self._cfg_patch.start()

    def tearDown(self):
        self._cfg_patch.stop()
        self._tmp.cleanup()

    def test_parse_and_snap_discrete_clocks(self):
        clocks = gsi._parse_supported_graphics_clocks(SAMPLE_SUPPORTED_4090)
        self.assertEqual(clocks[0], 210)
        self.assertEqual(clocks[-1], 3105)
        self.assertEqual(len(clocks), len(set(clocks)))
        self.assertEqual(gsi.snap_clock_mhz(2300, clocks, 210, 3105), 2295)
        self.assertEqual(gsi.snap_clock_mhz(2450, clocks, 210, 3105), 2445)
        self.assertEqual(gsi.snap_clock_mhz(100, clocks, 210, 3105), 210)
        self.assertEqual(gsi.snap_clock_mhz(4000, clocks, 210, 3105), 3105)

    def test_architecture_presets(self):
        ada_clocks = list(range(210, 3120, 15))
        ada = gsi.resolve_clock_cap_presets("NVIDIA GeForce RTX 4090", 210, 3105, ada_clocks)
        self.assertTrue(ada["is_ada"])
        self.assertEqual(ada["efficiency_mhz"], 2295)
        self.assertEqual(ada["sweetspot_mhz"], 2445)

        ada_ws = gsi.resolve_clock_cap_presets("NVIDIA RTX 6000 Ada Generation", 210, 2955, list(range(210, 2970, 15)))
        self.assertTrue(ada_ws["is_ada"])
        self.assertEqual(ada_ws["efficiency_mhz"], 2295)

        amp_clocks = list(range(210, 2115, 15))
        amp = gsi.resolve_clock_cap_presets("NVIDIA GeForce RTX 3090", 210, 2100, amp_clocks)
        self.assertFalse(amp["is_ada"])
        self.assertLess(amp["efficiency_mhz"], amp["sweetspot_mhz"])
        self.assertIn(amp["efficiency_mhz"], amp_clocks)
        self.assertIn(amp["sweetspot_mhz"], amp_clocks)
        self.assertLess(amp["sweetspot_mhz"], 1900)

    def test_get_clock_cap_info_and_config_state(self):
        gsi._save_clock_lock_state(0, True, min_mhz=210, max_mhz=2295, reapply_on_wake=True)

        def fake_hidden(cmd):
            joined = " ".join(str(x) for x in cmd)
            if "clocks.current.sm" in joined:
                return "NVIDIA GeForce RTX 4090, 2295, 3105, Active\n"
            if "SUPPORTED_CLOCKS" in joined:
                return SAMPLE_SUPPORTED_4090
            raise RuntimeError(f"Unexpected cmd: {cmd}")

        with mock.patch.object(gsi, "find_nvidia_smi", return_value="nvidia-smi"), \
             mock.patch.object(gsi, "_run_hidden", side_effect=fake_hidden):
            info = gsi.get_clock_cap_info(0)

        self.assertTrue(info["adjustable"])
        self.assertEqual(info["current_sm_mhz"], 2295)
        self.assertEqual(info["min_sm_mhz"], 210)
        self.assertEqual(info["max_sm_mhz"], 3105)
        self.assertTrue(info["sw_power_cap"])
        self.assertTrue(info["lock_enabled"])
        self.assertEqual(info["locked_max_mhz"], 2295)

    def test_set_reset_and_wake_reapply(self):
        recorded = []
        current_pl = [450.0]

        def fake_hidden(cmd):
            joined = " ".join(cmd)
            if "power.draw" in joined:
                return f"40.0, {current_pl[0]}, 450.0, 150.0, 450.0\n"
            if "clocks.current.sm" in joined:
                return "NVIDIA GeForce RTX 4090, 345, 3105, Not Active\n"
            if "SUPPORTED_CLOCKS" in joined:
                return SAMPLE_SUPPORTED_4090
            raise RuntimeError(f"Unexpected query: {cmd}")

        def fake_batch(cmds, allow_uac=True, action_label="GPU tuning"):
            recorded.append((cmds, allow_uac))
            for args in cmds:
                if "-pl" in args:
                    current_pl[0] = float(args[args.index("-pl") + 1])
            return True, ""

        with mock.patch.object(gsi, "find_nvidia_smi", return_value="nvidia-smi"), \
             mock.patch.object(gsi, "_run_hidden", side_effect=fake_hidden), \
             mock.patch.object(gsi, "_run_nvidia_smi_batch", side_effect=fake_batch):
            ok, clk_info, msg = gsi.set_sm_clock_cap(0, 2300, allow_uac=True)
            self.assertTrue(ok, msg)
            self.assertEqual(recorded[-1][0], [["-i", "0", "-lgc", "210,2295"]])
            self.assertTrue(clk_info["lock_enabled"])
            self.assertEqual(clk_info["locked_max_mhz"], 2295)

            cfg = gsi.load_user_config()
            cfg.setdefault("power_limit", {}).update({"reapply_on_wake": True, "target_watts": 450, "gpu_index": 0})
            cfg.setdefault("clock_lock", {}).update({"reapply_on_wake": True})
            gsi.save_user_config(cfg)

            recorded.clear()
            ok_wake, msg_wake = gsi.apply_saved_power_limits()
            self.assertTrue(ok_wake, msg_wake)
            self.assertEqual(recorded[0][0], [["-i", "0", "-pl", "450"]])
            self.assertFalse(recorded[0][1])  # Wake task must not prompt UAC
            self.assertEqual(recorded[1][0], [["-i", "0", "-lgc", "210,2295"]])
            self.assertFalse(recorded[1][1])

            # Gaming / Stock profile clears -lgc in a single batch call
            recorded.clear()
            ok_prof, _, clk_after, _ = gsi.apply_efficiency_profile(0, target_watts=450, unlock_clocks=True)
            self.assertTrue(ok_prof)
            self.assertEqual(recorded[0][0], [["-i", "0", "-pl", "450"], ["-i", "0", "-rgc"]])
            self.assertFalse(clk_after["lock_enabled"])

    def test_laptop_card_profiles_and_plm_decode(self):
        p_desktop = gsi.resolve_card_profile("NVIDIA GeForce RTX 4090", 8, 9, 128)
        p_laptop_4090 = gsi.resolve_card_profile("NVIDIA GeForce RTX 4090 Laptop GPU", 8, 9, 76)
        p_laptop_4080 = gsi.resolve_card_profile("NVIDIA GeForce RTX 4080 Laptop GPU", 8, 9, 58)
        p_laptop_4070 = gsi.resolve_card_profile("NVIDIA GeForce RTX 4070 Laptop GPU", 8, 9, 36)
        p_laptop_4060 = gsi.resolve_card_profile("NVIDIA GeForce RTX 4060 Laptop GPU", 8, 9, 24)
        self.assertGreater(p_desktop["threshold"], p_laptop_4090["threshold"])
        self.assertEqual(p_laptop_4090["threshold"], 120.0)
        self.assertEqual(p_laptop_4080["threshold"], 82.0)
        self.assertEqual(p_laptop_4070["threshold"], 46.0)
        self.assertEqual(p_laptop_4060["threshold"], 32.0)

        self.assertIn("OPEN", gsi.decode_plm_mask(0xFFFFFFFF))
        self.assertIn("STOCK-LOCK", gsi.decode_plm_mask(0xFFFFFF8F))
        self.assertIn("RELOCK-L0", gsi.decode_plm_mask(0xFFFFFFEF))
        self.assertIn("DEEP-LOCK", gsi.decode_plm_mask(0xFFFFFFCF))

    def test_dashboard_i18n_key_parity_and_formatting(self):
        self.assertEqual(di18n.SUPPORTED_LANGUAGES, ("en", "zh", "de"))
        en_keys = set(di18n.STRINGS["en"])
        fmt = string.Formatter()

        for lang in di18n.SUPPORTED_LANGUAGES:
            self.assertEqual(set(di18n.STRINGS[lang]), en_keys, f"Key mismatch for {lang}")

        for key in en_keys:
            base_fields = {fname for _, fname, _, _ in fmt.parse(di18n.STRINGS["en"][key]) if fname is not None}
            for lang in di18n.SUPPORTED_LANGUAGES:
                val = di18n.STRINGS[lang][key]
                self.assertTrue(isinstance(val, str) and len(val) > 0, f"Empty string {lang}.{key}")
                lang_fields = {fname for _, fname, _, _ in fmt.parse(val) if fname is not None}
                self.assertEqual(lang_fields, base_fields, f"Placeholder mismatch in {lang}.{key}")
                self.assertIsNone(re.search(r"\b(we|us|our|wir|uns)\b", val, re.IGNORECASE), val)
                self.assertNotIn("我们", val)

    def test_dashboard_i18n_persistence_and_s3_translation(self):
        self.assertEqual(di18n.normalize_language("zh-CN"), "zh")
        self.assertEqual(di18n.normalize_language("de_DE"), "de")
        self.assertEqual(di18n.normalize_language("fr"), "en")

        saved_zh = di18n.save_saved_language("zh-CN")
        self.assertEqual(saved_zh, "zh")
        self.assertEqual(di18n.load_saved_language(), "zh")
        self.assertEqual(gsi.load_user_config()["ui"]["language"], "zh")

        saved_de = di18n.save_saved_language("de-DE")
        self.assertEqual(saved_de, "de")
        self.assertEqual(di18n.load_saved_language(), "de")

        for lang in di18n.SUPPORTED_LANGUAGES:
            h_arm, d_arm = di18n.translate_s3_status(
                lang, {"code": "ARMED", "log_name": "RUN0001.TXT", "shadow_facs": "0xDC637400"}, True
            )
            self.assertTrue(h_arm)
            self.assertIn("RUN0001.TXT", d_arm)

            h_s0ix, d_s0ix = di18n.translate_s3_status(lang, {"code": "S0IX_WARN"}, True)
            self.assertIn("S0ix", h_s0ix)
            self.assertTrue(d_s0ix)

            h_unv, _ = di18n.translate_s3_status(lang, {"code": "UNVERIFIED"}, True)
            self.assertTrue(h_unv)

            h_csm, _ = di18n.translate_s3_status(lang, {"code": "ARMED"}, False)
            self.assertIn("CSM", h_csm)

    def test_sync_boot_lang_cfg_writes_only_existing_boot_volumes(self):
        with tempfile.TemporaryDirectory() as tmp:
            vol_boot = os.path.join(tmp, "vol_boot")
            vol_empty = os.path.join(tmp, "vol_empty")
            boot_dir = os.path.join(vol_boot, "EFI", "BOOT")
            os.makedirs(boot_dir, exist_ok=True)
            os.makedirs(vol_empty, exist_ok=True)
            with open(os.path.join(boot_dir, "BOOTX64.EFI"), "wb") as f:
                f.write(b"MZ")

            mock_roots = [(vol_boot, "USB", False), (vol_empty, "OTHER", True)]
            with mock.patch.object(gsi, "_list_log_search_roots", return_value=mock_roots):
                updated = gsi.sync_boot_lang_cfg("de-DE")
                cfg_file = os.path.join(boot_dir, "lang.cfg")
                self.assertEqual(updated, [cfg_file])
                with open(cfg_file, "r", encoding="ascii") as f:
                    self.assertEqual(f.read(), "de\n")
                self.assertFalse(os.path.exists(os.path.join(vol_empty, "EFI", "BOOT", "lang.cfg")))

                saved = di18n.save_saved_language("zh-CN")
                self.assertEqual(saved, "zh")
                with open(cfg_file, "r", encoding="ascii") as f:
                    self.assertEqual(f.read(), "zh\n")

    def test_multigpu_benchmark_stream_and_gtx16_classification(self):
        events = []

        class _DevCtx:
            def __init__(self, dev):
                self.dev = dev

            def __enter__(self):
                events.append(("enter", str(self.dev)))
                return self

            def __exit__(self, *args):
                events.append(("exit", str(self.dev)))
                return False

        class _FakeEvent:
            def __init__(self, enable_timing=True):
                self.stream = None

            def record(self, stream=None):
                events.append(("record", stream))
                self.stream = stream

            def elapsed_time(self, other):
                return 20.0

        class _FakeCuda:
            def device(self, dev):
                return _DevCtx(dev)

            def current_stream(self, dev):
                return f"stream:{dev}"

            def synchronize(self, dev=None):
                events.append(("sync", str(dev)))

            def Event(self, enable_timing=True):
                return _FakeEvent(enable_timing=enable_timing)

        class _FakeTorch:
            backends = type("B", (), {"cudnn": type("C", (), {"benchmark": False})()})()
            cuda = _FakeCuda()

            def randn(self, shape, dtype=None, device=None):
                return ("t", str(device))

            def matmul(self, a, b):
                return ("m", a[1])

        avg_ms, tflops = gsi.benchmark_gemm(_FakeTorch(), "cuda:1", "fp16", size=4096, warmup=1, runs=5)
        self.assertAlmostEqual(avg_ms, 4.0)
        self.assertGreater(tflops, 30.0)
        self.assertIn(("enter", "cuda:1"), events)
        self.assertIn(("exit", "cuda:1"), events)
        self.assertEqual(events.count(("record", "stream:cuda:1")), 2)
        self.assertEqual(events.count(("sync", "cuda:1")), 2)

        prof = {"min_unlocked": 18.0, "threshold": 14.0, "stock_typical": 10.0}
        v_1660 = gsi.classify_unlock_status(0.59, prof, {}, None, 7, 5, gpu_name="NVIDIA GeForce GTX 1660 SUPER")
        self.assertEqual(v_1660["code"], "NO_TENSOR_CORES")
        self.assertFalse(v_1660["is_unlocked"])

        v_3060ti = gsi.classify_unlock_status(61.2, prof, {}, {"supported": True, "ratio": 1.01}, 8, 6, gpu_name="NVIDIA GeForce RTX 3060 Ti")
        self.assertEqual(v_3060ti["code"], "UNLOCKED")
        self.assertTrue(v_3060ti["is_unlocked"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
