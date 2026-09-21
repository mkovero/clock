#!/usr/bin/env python3
"""Focused regression tests for the dashboard collector."""

import importlib.util
import unittest
from datetime import datetime, timedelta, timezone
from importlib.machinery import SourceFileLoader
from pathlib import Path


SCRIPT = Path(__file__).with_name("clock-dashboard")
SPEC = importlib.util.spec_from_loader(
    "clock_dashboard", SourceFileLoader("clock_dashboard", str(SCRIPT))
)
assert SPEC and SPEC.loader
dashboard = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(dashboard)


class PublicHistoryTest(unittest.TestCase):
    def test_historical_samples_keep_diagnostic_telemetry(self):
        now = datetime(2026, 9, 19, tzinfo=timezone.utc)
        old = {
            "at": (now - timedelta(hours=1)).isoformat(),
            "status": "degraded",
            "rf_blocks": [{"id": 0, "state": "warning", "jam_indicator": 61,
                           "agc": 5967, "noise_per_ms": 63}],
            "spoof_state": "none-indicated",
            "best_cn0_dbhz": [48.0, 46.0],
            "pdop": 1.24,
            "tdop": 0.62,
            "constellations": {"GPS": 7},
            "ref_freq_live": -3e-9,
            "holdover_us_per_day": 259.2,
            "checks": [
                {"level": "ok", "text": "routine success"},
                {"level": "warn", "text": "RF block 0 changed"},
            ],
            "system_time": "presentation-only value",
        }
        latest = {"at": now.isoformat(), "status": "healthy",
                  "checks": [{"level": "ok", "text": "latest success"}]}

        published = dashboard.public_history([old, latest], now)

        self.assertEqual(published[0]["rf_blocks"], old["rf_blocks"])
        self.assertEqual(published[0]["best_cn0_dbhz"], [48.0, 46.0])
        self.assertEqual(published[0]["constellations"], {"GPS": 7})
        self.assertEqual(published[0]["ref_freq_live"], -3e-9)
        self.assertEqual(published[0]["checks"], [old["checks"][1]])
        self.assertNotIn("system_time", published[0])
        self.assertEqual(published[1], latest)


class HoldoverLabelTest(unittest.TestCase):
    """gpsstat renamed this line on 2026-09-21; both spellings must still parse.

    The old label called the static offset "holdover", which it is not -- chrony
    carries a static offset in its frequency estimate. History written before the
    rename is still on disk, so the collector has to read both.
    """

    CURRENT = "  static offset as time error per day       : 1.9 us/day"
    LEGACY = "  holdover drift if GNSS is lost            : 259.2 us/day"

    def _parse(self, line):
        text = "\n".join(["reference oscillator (AR-40A)", line])
        return dashboard.parse_gpsstat(text, 0, "2026-09-21T19:00:00Z")

    def test_current_label_parses(self):
        self.assertEqual(self._parse(self.CURRENT)["holdover_us_per_day"], 1.9)

    def test_legacy_label_still_parses(self):
        self.assertEqual(self._parse(self.LEGACY)["holdover_us_per_day"], 259.2)


if __name__ == "__main__":
    unittest.main()
