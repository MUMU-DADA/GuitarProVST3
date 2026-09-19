"""Synthetic source/return evidence checks; these do not exercise real hardware."""

import contextlib
import copy
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest

import numpy as np

sys.dont_write_bytecode = True
SPEC = importlib.util.spec_from_file_location("p13_monitor", Path(__file__).with_name("analyze-p13-monitor.py"))
analyzer = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(analyzer)
FIXTURE_SPEC = importlib.util.spec_from_file_location("p13_pcm_fixture", Path(__file__).with_name("test_analyze_p13_pcm.py"))
fixtures = importlib.util.module_from_spec(FIXTURE_SPEC)
FIXTURE_SPEC.loader.exec_module(fixtures)


def write_json(path, value):
    path.write_text(json.dumps(value), encoding="utf-8")


def fixture(directory, mode, lag):
    data = directory / "data"
    data.mkdir(parents=True)
    path, manifest = fixtures.fixture(data)
    rng = np.random.default_rng(531)
    source = rng.normal(0, 0.08, analyzer.pcm.FRAME_CAPACITY)
    returned = rng.normal(0, 0.0002, len(source))
    returned[lag:] += source[:-lag] * 0.6
    capture = np.column_stack((source, returned)).astype("<f4")
    content = capture.tobytes()
    (data / "capture.f32").write_bytes(content)
    manifest.update(capture_sha256=hashlib.sha256(content).hexdigest(), monitor_latency_measurement=True,
                    capture_stage="raw_device_capture_before_measurement_mapping")
    for record in manifest["records"]:
        record.update(input_channels=2, capture_byte_offset=record["frame_offset"] * 8,
                      capture_bytes=record["saved_frames"] * 8, generation="2", rate_revision="2",
                      actual_callback_rate=192000, actual_rate_validated=True,
                      monitor_reference_is_input1=True, driver_channels_validated=True,
                      driver_input_selectors=[0, 1], driver_output_selectors=[0, 1])
    write_json(path, manifest)
    collection = {"status": "collected_unvalidated", "cleanup_errors": [],
                  "loopback_input2": {"restore_confirmed": True},
                  "monitor_latency": {"configured": True, "mode": mode, "configured_ns": "999999999",
                                      "native_gain": 500, "rse_score": "empty_score; latency_only"},
                  "input_overlay": None}
    if mode == "overlay":
        collection["input_overlay"] = {"runtime": {
            "state": "active", "mode": "low_latency_overlay", "input_native_effect_bypass": True,
            "error_blocks": "0", "processed_blocks": "1024", "gain": 0.5,
            "sample_rate": 192000, "driver_buffer_frames": 64,
            "plugin_latency_samples": "0", "clipped_blocks": "0"}}
    write_json(directory / "collection.json", collection)
    write_json(directory / "shutdown.json", {"exit_code": 0, "forced": False, "exited": True})
    return manifest, collection


class MonitorAnalysisTests(unittest.TestCase):
    def setUp(self):
        workspace = tempfile.TemporaryDirectory(prefix="p13-monitor-")
        self.addCleanup(workspace.cleanup)
        self.directory = Path(workspace.name)
        self.native = self.directory / "native"
        self.overlay = self.directory / "overlay"
        self.manifest, self.collection = fixture(self.native, "native", 15064)
        self.overlay_manifest, self.overlay_collection = fixture(self.overlay, "overlay", 610)

    def update_capture(self, capture, directory=None, manifest=None):
        directory = directory or self.native
        manifest = copy.deepcopy(manifest or self.manifest)
        content = np.asarray(capture, dtype="<f4").tobytes()
        (directory / "data/capture.f32").write_bytes(content)
        manifest["capture_sha256"] = hashlib.sha256(content).hexdigest()
        write_json(directory / "data/p13-pcm.json", manifest)

    def capture(self):
        return np.frombuffer((self.native / "data/capture.f32").read_bytes(), dtype="<f4").reshape(-1, 2).copy()

    def test_known_delays_use_raw_input1_and_report_limited_scope(self):
        result = analyzer.compare(self.native, self.overlay)
        self.assertEqual(result["status"], "measured_monitor_delay_comparison")
        self.assertFalse(result["p13_acceptance"])
        self.assertAlmostEqual(result["reduction_ms"], (15064 - 610) / 192.0)
        for mode, lag in (("native", 15064), ("overlay", 610)):
            measured = result[mode]
            self.assertEqual(measured["candidate_lag_samples"], lag)
            self.assertTrue(measured["sample_rate_bound_to_generation"])
            self.assertEqual([part["lag_samples"] for part in measured["temporal_segments"]], [lag] * 3)
            self.assertGreater(measured["absolute_correlation"], 0.99)
            self.assertEqual(measured["source_driver_channel"], 0)
            self.assertEqual(measured["return_driver_channel"], 1)
            self.assertEqual(measured["rse_score"], "empty_score; latency_only")
            self.assertFalse(measured["p13_acceptance"])

    def test_missing_score_description_does_not_claim_rse_coexistence(self):
        self.collection["monitor_latency"].pop("rse_score")
        write_json(self.native / "collection.json", self.collection)
        self.assertEqual(analyzer.analyze(self.native, "native")["rse_score"],
                         "unspecified; RSE coexistence not established")

    def test_mode_configuration_and_restoration_required(self):
        mutations = [
            lambda c: c.update(status="failed"),
            lambda c: c["monitor_latency"].update(configured=False),
            lambda c: c["monitor_latency"].update(mode="overlay"),
            lambda c: c["monitor_latency"].update(configured_ns="0"),
            lambda c: c["monitor_latency"].update(configured_ns=1),
            lambda c: c["monitor_latency"].update(configured_ns="1000000001"),
            lambda c: c["loopback_input2"].update(restore_confirmed=False),
            lambda c: c.update(cleanup_errors=["input restore failed"]),
            lambda c: c.pop("cleanup_errors"),
            lambda c: c.update(input_overlay=self.overlay_collection["input_overlay"]),
            lambda c: c["monitor_latency"].update(native_gain=0),
        ]
        for index, mutate in enumerate(mutations):
            with self.subTest(index=index):
                collection = copy.deepcopy(self.collection)
                mutate(collection)
                write_json(self.native / "collection.json", collection)
                with self.assertRaises(analyzer.pcm.EvidenceError):
                    analyzer.analyze(self.native, "native")

    def test_normal_exit_required(self):
        for change in ({"exit_code": 1}, {"exit_code": False}, {"forced": True}, {"exited": False}):
            with self.subTest(change=change):
                write_json(self.native / "shutdown.json", dict(exit_code=0, forced=False, exited=True) | change)
                with self.assertRaisesRegex(analyzer.pcm.EvidenceError, "host did not exit normally"):
                    analyzer.analyze(self.native, "native")

    def test_every_record_requires_validated_source_and_return_mapping(self):
        mutations = [
            lambda m: m.update(monitor_latency_measurement=False),
            lambda m: m.update(capture_stage="after_measurement_mapping"),
            lambda m: m["records"][2].update(monitor_reference_is_input1=False),
            lambda m: m["records"][2].update(driver_input_selectors=[1, 0]),
            lambda m: m["records"][2].update(driver_output_selectors=[0, 2]),
            lambda m: m["records"][2].update(driver_channels_validated=False),
            lambda m: [r.pop("driver_channels_validated") for r in m["records"]],
        ]
        for index, mutate in enumerate(mutations):
            with self.subTest(index=index):
                manifest = copy.deepcopy(self.manifest)
                mutate(manifest)
                write_json(self.native / "data/p13-pcm.json", manifest)
                with self.assertRaises(analyzer.pcm.EvidenceError):
                    analyzer.analyze(self.native, "native")

    def test_integrity_gaps_and_generation_bound_rate_required(self):
        mutations = [
            lambda m: m.update(capture_sha256="0" * 64),
            lambda m: m.update(complete=False),
            lambda m: m.update(busy_drops="1"),
            lambda m: m["records"][2].update(sequence="1"),
            lambda m: m["records"][2].update(generation="3"),
            lambda m: m["records"][2].update(rate_revision="3"),
            lambda m: m["records"][2].update(status_flags="1"),
            lambda m: [r.update(actual_rate_validated=False) for r in m["records"]],
        ]
        for index, mutate in enumerate(mutations):
            with self.subTest(index=index):
                manifest = copy.deepcopy(self.manifest)
                mutate(manifest)
                write_json(self.native / "data/p13-pcm.json", manifest)
                with self.assertRaises(analyzer.pcm.EvidenceError):
                    analyzer.analyze(self.native, "native")

    def test_collection_and_shutdown_reject_duplicate_keys_and_nonfinite_json(self):
        for filename in ("collection.json", "shutdown.json"):
            path = self.native / filename
            original = path.read_text(encoding="utf-8")
            for content in ('{"duplicate": 1, "duplicate": 2,' + original[1:],
                            '{"unrelated_value": NaN,' + original[1:]):
                with self.subTest(filename=filename, content=content[:35]):
                    path.write_text(content, encoding="utf-8")
                    with self.assertRaises(analyzer.pcm.EvidenceError):
                        analyzer.analyze(self.native, "native")
            path.write_text(original, encoding="utf-8")

    def test_overlay_must_have_processed_with_expected_timing_and_no_errors(self):
        for change in ({"state": "preparing"}, {"mode": "off"}, {"input_native_effect_bypass": False},
                       {"error_blocks": "1"}, {"error_blocks": False}, {"processed_blocks": "0"},
                       {"sample_rate": 96000}, {"driver_buffer_frames": 128}, {"gain": 0},
                       {"plugin_latency_samples": "-1"}):
            with self.subTest(change=change):
                collection = copy.deepcopy(self.overlay_collection)
                collection["input_overlay"]["runtime"].update(change)
                write_json(self.overlay / "collection.json", collection)
                with self.assertRaises(analyzer.pcm.EvidenceError):
                    analyzer.analyze(self.overlay, "overlay")

    def test_comparison_requires_same_device_and_buffer_configuration(self):
        for field, changed in (("driver_frames", 128), ("input_device", 8), ("output_device", 8),
                               ("requested_sample_rate", 48000), ("actual_callback_rate", 96000)):
            with self.subTest(field=field):
                manifest = copy.deepcopy(self.manifest)
                for record in manifest["records"]:
                    record[field] = changed
                write_json(self.native / "data/p13-pcm.json", manifest)
                with self.assertRaisesRegex(analyzer.pcm.EvidenceError, "configuration or timing changed"):
                    analyzer.compare(self.native, self.overlay)

    def test_changed_delay_in_middle_third_is_inconclusive(self):
        capture = self.capture()
        begin, end = len(capture) // 3, len(capture) * 2 // 3
        capture[begin:end, 1] = np.roll(capture[:, 0], 16000)[begin:end] * 0.6
        self.update_capture(capture)
        result = analyzer.compare(self.native, self.overlay)
        self.assertEqual(result["status"], "inconclusive")
        self.assertNotIn("reduction_ms", result)
        self.assertIn("delay_not_consistent_across_recording", result["native"]["reasons"])

    def test_silent_middle_third_rejects_otherwise_strong_global_correlation(self):
        capture = self.capture()
        begin, end = len(capture) // 3, len(capture) * 2 // 3
        capture[begin:end, 1] = 0
        self.update_capture(capture)
        result = analyzer.analyze(self.native, "native")
        self.assertGreater(result["absolute_correlation"], 0.65)
        self.assertEqual(result["status"], "inconclusive")
        self.assertIn("delay_not_consistent_across_recording", result["reasons"])

    def test_input_return_swapped_is_inconclusive(self):
        self.update_capture(self.capture()[:, ::-1])
        result = analyzer.analyze(self.native, "native")
        self.assertEqual(result["status"], "inconclusive")
        self.assertIn("insufficient_correlation", result["reasons"])

    def test_signal_clipping_rejected_but_block_counter_preserves_latency_scope(self):
        self.overlay_collection["input_overlay"]["runtime"]["clipped_blocks"] = "122"
        write_json(self.overlay / "collection.json", self.overlay_collection)
        result = analyzer.compare(self.native, self.overlay)
        self.assertEqual(result["status"], "measured_monitor_delay_comparison")
        self.assertEqual(result["overlay"]["clipped_blocks"], 122)
        self.assertFalse(result["p13_acceptance"])
        capture = self.capture()
        capture[:len(capture) // 20, 0] = 1
        self.update_capture(capture)
        result = analyzer.analyze(self.native, "native")
        self.assertEqual(result["status"], "inconclusive")
        self.assertIn("substantial_signal_clipping", result["reasons"])

    def test_cli_success_inconclusive_and_invalid_exit_codes(self):
        output_path = self.directory / "comparison.json"
        for expected_code, expected_status in ((0, "measured_monitor_delay_comparison"),
                                                (1, "inconclusive"), (2, "invalid_evidence")):
            if expected_code == 1:
                self.update_capture(np.zeros_like(self.capture()))
            elif expected_code == 2:
                (self.native / "shutdown.json").write_text("not JSON", encoding="utf-8")
            with self.subTest(expected_code=expected_code), contextlib.redirect_stdout(io.StringIO()) as output:
                code = analyzer.main([str(self.native), str(self.overlay), "--output", str(output_path)])
            self.assertEqual(code, expected_code)
            result = json.loads(output.getvalue())
            self.assertEqual(result, json.loads(output_path.read_text(encoding="utf-8")))
            self.assertEqual(result["status"], expected_status)
            self.assertFalse(result["p13_acceptance"])

    def test_cli_malformed_metadata_is_invalid_evidence_without_traceback(self):
        for filename, value in (("collection.json", []),
                                ("collection.json", dict(self.collection, monitor_latency=None)),
                                ("data/p13-pcm.json", dict(self.manifest, records=[None]))):
            path = self.native / filename
            original = path.read_bytes()
            write_json(path, value)
            with self.subTest(filename=filename), contextlib.redirect_stdout(io.StringIO()) as output:
                code = analyzer.main([str(self.native), str(self.overlay)])
            self.assertEqual(code, 2)
            self.assertEqual(json.loads(output.getvalue())["status"], "invalid_evidence")
            path.write_bytes(original)


if __name__ == "__main__":
    unittest.main(verbosity=2)
