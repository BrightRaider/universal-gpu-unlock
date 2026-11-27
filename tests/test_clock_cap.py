# SPDX-License-Identifier: GPL-2.0-only
"""Unit tests for SM clock-cap parsing, Ada/Ampere/Turing presets, and wake reapply."""
import os
import sys
import tempfile
import unittest
from unittest import mock

TOOLS_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "tools"))
if TOOLS_DIR not in sys.path:
    sys.path.insert(0, TOOLS_DIR)

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


if __name__ == "__main__":
    unittest.main(verbosity=2)
