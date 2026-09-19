"""Synthetic evidence checks for the P13 physical-loopback analyzer."""

import copy
import contextlib
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
SPEC = importlib.util.spec_from_file_location("p13_pcm", Path(__file__).with_name("analyze-p13-pcm.py"))
analyzer = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(analyzer)


def fixture(directory, capture=None, reference=None, lag=731, inverted=False):
    size = analyzer.FRAME_CAPACITY
    rng = np.random.default_rng(70919)
    if reference is None:
        reference = rng.normal(0, 0.08, size)
    if capture is None:
        capture = rng.normal(0, 0.0002, size)
        capture[lag:] += reference[:-lag] * (-0.6 if inverted else 0.6)
    output = np.column_stack((rng.normal(0, 0.05, size), reference)).astype("<f4")
    capture_bytes = np.asarray(capture, dtype="<f4").tobytes()
    output_bytes = output.tobytes()
    (directory / "capture.f32").write_bytes(capture_bytes)
    (directory / "output.f32").write_bytes(output_bytes)
    records = []
    offset = 0
    lengths = [64, 128, 127, 2048, 256, 31, 512]
    while offset < size:
        index = len(records)
        frames = lengths[index % len(lengths)]
        saved = min(frames, size - offset)
        records.append({"index": index, "sequence": str(index + 200),
                        "timestamp_ns": str(10**9 + offset * 10**9 // 192000),
                        "frame_offset": offset, "saved_frames": saved, "callback_frames": frames,
                        "truncated": saved != frames, "changes": 0, "status_flags": "0",
                        "original_result": 0, "configuration_valid": True, "owner": "2a334400",
                        "generation": "0", "thread": 11, "requested_sample_rate": 44100,
                        "host_api_type": 3, "driver_frames": 64, "input_device": 4, "output_device": 5,
                        "input_channels": 1, "output_channels": 2, "capture_byte_offset": offset * 4,
                        "capture_bytes": saved * 4, "output_byte_offset": offset * 8,
                        "output_bytes": saved * 8, "capture_status": 3, "output_status": 3,
                        "capture_nonfinite": 0, "output_nonfinite": 0})
        offset += saved
    manifest = {"schema": 1, "enabled": True, "complete": True,
                "claimed": len(records), "published": len(records), "frames_reserved": size,
                "format": "float32_little_endian_interleaved_per_record",
                "capture_file": "capture.f32", "output_file": "output.f32",
                "capture_sha256": hashlib.sha256(capture_bytes).hexdigest(),
                "output_sha256": hashlib.sha256(output_bytes).hexdigest(),
                "busy_drops": "0", "abandoned": "0", "actual_asio_sample_rate": 192000,
                "actual_rate_query_result": 0, "actual_rate_query_source": analyzer.RATE_SOURCE,
                "rate_scope": analyzer.RATE_SCOPE, "acceptance": "not_evaluated", "records": records}
    path = directory / "p13-pcm.json"
    path.write_text(json.dumps(manifest), encoding="utf-8")
    return path, manifest


class PcmAnalysisTests(unittest.TestCase):
    def setUp(self):
        self.workspace = tempfile.TemporaryDirectory(prefix="p13-pcm-")
        self.addCleanup(self.workspace.cleanup)
        self.directory = Path(self.workspace.name)
        self.path, self.manifest = fixture(self.directory)

    def rewrite(self, manifest):
        self.path.write_text(json.dumps(manifest), encoding="utf-8")

    def bind_callback_rate(self):
        for record in self.manifest["records"]:
            record.update(generation="1", rate_revision="2", actual_callback_rate=192000,
                          actual_rate_validated=True)
        self.rewrite(self.manifest)

    def test_variable_records_known_positive_lag(self):
        evidence = analyzer.load_evidence(self.path)
        result = analyzer.estimate(evidence, maximum_lag=2048)
        self.assertEqual(result["status"], "conditional_physical_loopback_estimate")
        self.assertEqual(result["candidate_lag_samples"], 731)
        self.assertGreater(result["absolute_correlation"], 0.99)
        self.assertEqual(result["callback_frames"], [31, 64, 127, 128, 256, 512, 2048])
        self.assertAlmostEqual(result["candidate_delay_ms_conditional"], 731 / 192.0)
        self.assertAlmostEqual(result["candidate_delay_seconds_conditional"], 731 / 192000.0)
        self.assertNotIn("candidate_delay_ms", result)
        self.assertFalse(result["sample_rate_bound_to_generation"])
        self.assertFalse(result["p13_acceptance"])
        self.assertEqual(result["requested_sample_rate"], 44100)
        self.assertEqual(result["measurement_scope"], analyzer.MEASUREMENT_SCOPE)
        self.assertTrue(all(part["lag_samples"] == 731 for part in result["temporal_segments"]))
        explicit = analyzer.estimate(evidence, maximum_lag=2048, sample_rate=96000)
        self.assertEqual(explicit["rate_scope"], "explicit_override_not_generation_bound")
        self.assertAlmostEqual(explicit["candidate_delay_ms_conditional"], 731 / 96.0)
        self.assertEqual(explicit["observed_asio_sample_rate"], 192000)

    def test_generation_bound_callback_rate(self):
        self.bind_callback_rate()
        evidence = analyzer.load_evidence(self.path)
        result = analyzer.estimate(evidence, maximum_lag=2048)
        self.assertEqual(result["status"], "physical_loopback_estimate")
        self.assertEqual(result["rate_scope"], analyzer.CALLBACK_RATE_SCOPE)
        self.assertTrue(result["sample_rate_bound_to_generation"])
        self.assertEqual(result["generation"], 1)
        self.assertEqual(result["rate_revision"], 2)
        self.assertEqual(result["validated_callback_sample_rate"], 192000)
        self.assertAlmostEqual(result["candidate_delay_ms"], 731 / 192.0)
        self.assertAlmostEqual(result["candidate_delay_seconds"], 731 / 192000.0)
        self.assertNotIn("candidate_delay_ms_conditional", result)
        self.assertFalse(result["p13_acceptance"])
        self.assertEqual(result["measurement_scope"], analyzer.MEASUREMENT_SCOPE)

    def test_bound_recording_uses_callback_rate_despite_later_query(self):
        self.bind_callback_rate()
        for post_rate, query_result in ((96000, 0), (None, -998)):
            with self.subTest(post_rate=post_rate, query_result=query_result):
                self.manifest.update(actual_asio_sample_rate=post_rate, actual_rate_query_result=query_result)
                self.rewrite(self.manifest)
                result = analyzer.estimate(analyzer.load_evidence(self.path), maximum_lag=2048)
                self.assertEqual(result["status"], "physical_loopback_estimate")
                self.assertEqual(result["sample_rate"], 192000)
                self.assertEqual(result["observed_asio_sample_rate"], post_rate)
                self.assertEqual(result["actual_rate_query_result"], query_result)
                self.assertAlmostEqual(result["candidate_delay_ms"], 731 / 192.0)

    def test_explicit_override_cannot_claim_generation_binding(self):
        self.bind_callback_rate()
        result = analyzer.estimate(analyzer.load_evidence(self.path), maximum_lag=2048, sample_rate=96000)
        self.assertEqual(result["status"], "conditional_physical_loopback_estimate")
        self.assertEqual(result["rate_scope"], "explicit_override_not_generation_bound")
        self.assertFalse(result["sample_rate_bound_to_generation"])
        self.assertEqual(result["validated_callback_sample_rate"], 192000)
        self.assertAlmostEqual(result["candidate_delay_ms_conditional"], 731 / 96.0)
        self.assertNotIn("candidate_delay_ms", result)

    def test_unvalidated_callback_rates_remain_conditional(self):
        for generation, revision, callback_rate in (("0", "0", 0), ("1", "3", 96000)):
            with self.subTest(generation=generation):
                for record in self.manifest["records"]:
                    record.update(generation=generation, rate_revision=revision, actual_callback_rate=callback_rate,
                                  actual_rate_validated=False)
                self.rewrite(self.manifest)
                result = analyzer.estimate(analyzer.load_evidence(self.path), maximum_lag=2048)
                self.assertEqual(result["status"], "conditional_physical_loopback_estimate")
                self.assertFalse(result["sample_rate_bound_to_generation"])
                self.assertIsNone(result["validated_callback_sample_rate"])
                self.assertEqual(result["sample_rate"], 192000)
                self.assertAlmostEqual(result["candidate_delay_ms_conditional"], 731 / 192.0)

    def test_inconsistent_or_incomplete_callback_rate_evidence_rejected(self):
        self.bind_callback_rate()
        mutations = [
            lambda m: m["records"][2].update(rate_revision="3"),
            lambda m: m["records"][2].update(actual_callback_rate=96000),
            lambda m: m["records"][2].update(actual_rate_validated=False),
            lambda m: m["records"][2].pop("actual_callback_rate"),
            lambda m: [r.pop("rate_revision") for r in m["records"]],
            lambda m: [r.update(generation="0") for r in m["records"]],
            lambda m: [r.update(rate_revision="0") for r in m["records"]],
            lambda m: m["records"][2].update(actual_rate_validated=1),
            lambda m: m["records"][2].update(rate_revision=2),
            lambda m: m["records"][2].update(actual_callback_rate=None),
            lambda m: m["records"][2].update(actual_callback_rate=True),
            lambda m: m["records"][2].update(actual_callback_rate=0),
            lambda m: m["records"][2].update(actual_callback_rate=10**400),
            lambda m: [r.pop(k) for r in m["records"][:1]
                       for k in ("rate_revision", "actual_callback_rate", "actual_rate_validated")],
        ]
        for index, mutate in enumerate(mutations):
            with self.subTest(index=index):
                manifest = copy.deepcopy(self.manifest)
                mutate(manifest)
                self.rewrite(manifest)
                with self.assertRaises(analyzer.EvidenceError):
                    analyzer.load_evidence(self.path)

    def test_cli_success_for_bound_and_legacy_evidence(self):
        for bind, status in ((False, "conditional_physical_loopback_estimate"),
                             (True, "physical_loopback_estimate")):
            with self.subTest(bind=bind):
                if bind:
                    self.bind_callback_rate()
                output = io.StringIO()
                with contextlib.redirect_stdout(output):
                    code = analyzer.main([str(self.path), "--max-lag", "2048"])
                self.assertEqual(code, 0)
                result = json.loads(output.getvalue())
                self.assertEqual(result["status"], status)
                self.assertFalse(result["p13_acceptance"])

    def test_inverted_polarity(self):
        path, _ = fixture(self.directory, lag=137, inverted=True)
        result = analyzer.estimate(analyzer.load_evidence(path), maximum_lag=2048)
        self.assertEqual(result["status"], "conditional_physical_loopback_estimate")
        self.assertEqual(result["candidate_lag_samples"], 137)
        self.assertEqual(result["polarity"], "inverted")

    def test_driver_mapping_identity(self):
        manifest = copy.deepcopy(self.manifest)
        for record in manifest["records"]:
            record.update(driver_channels_validated=True, driver_input_selectors=[1, -1],
                          driver_output_selectors=[0, 1])
        self.rewrite(manifest)
        evidence = analyzer.load_evidence(self.path)
        self.assertEqual(evidence["driver_capture_channel"], 1)
        self.assertEqual(evidence["driver_right_output_channel"], 1)
        for change in ({"driver_input_selectors": [0, -1]}, {"driver_output_selectors": [2, 3]},
                       {"driver_channels_validated": False}, {"driver_input_selectors": [1, 0]}):
            invalid = copy.deepcopy(manifest)
            invalid["records"][2].update(change)
            self.rewrite(invalid)
            with self.assertRaises(analyzer.EvidenceError):
                analyzer.load_evidence(self.path)

    def test_corrupt_metadata_is_rejected(self):
        mutations = [
            lambda m: m.update(complete=False),
            lambda m: m.update(busy_drops="1"),
            lambda m: m.update(abandoned="1"),
            lambda m: m.update(published=m["published"] - 1),
            lambda m: m.update(actual_rate_query_result=-998),
            lambda m: m.update(actual_asio_sample_rate=None),
            lambda m: m.update(actual_asio_sample_rate=10**400),
            lambda m: m.update(rate_scope="generation_bound"),
            lambda m: m.update(actual_rate_query_source="Pa_GetStreamInfo"),
            lambda m: m.update(capture_sha256="0" * 64),
            lambda m: m.update(capture_file="../capture.f32"),
            lambda m: m.update(frames_reserved=m["frames_reserved"] - 1),
            lambda m: m["records"][2].update(sequence="888"),
            lambda m: m["records"][2].update(owner="fff"),
            lambda m: m["records"][2].update(generation="1"),
            lambda m: m["records"][2].update(input_channels=2),
            lambda m: m["records"][2].update(changes=1),
            lambda m: m["records"][2].update(capture_byte_offset=0),
            lambda m: m["records"][2].update(output_bytes=4),
            lambda m: m["records"][2].update(output_status=4),
            lambda m: m["records"][2].update(capture_nonfinite=1),
            lambda m: m["records"][2].update(status_flags="1"),
            lambda m: m["records"][2].update(original_result=1),
            lambda m: m["records"][2].update(configuration_valid=False),
            lambda m: m["records"][2].update(timestamp_ns="1"),
            lambda m: m["records"][2].update(host_api_type=2),
            lambda m: m["records"][2].update(truncated=True),
            lambda m: m["records"][2].update(index=True),
        ]
        for index, mutate in enumerate(mutations):
            with self.subTest(index=index):
                manifest = copy.deepcopy(self.manifest)
                mutate(manifest)
                self.rewrite(manifest)
                with self.assertRaises(analyzer.EvidenceError):
                    analyzer.load_evidence(self.path)

    def test_pcm_size_and_undeclared_nan_rejected_even_with_updated_hash(self):
        output_path = self.directory / "output.f32"
        original = output_path.read_bytes()
        for content in (original[:-4], original + b"\x00\x00\x00\x00",
                        np.array([np.nan], dtype="<f4").tobytes() + original[4:]):
            output_path.write_bytes(content)
            manifest = copy.deepcopy(self.manifest)
            manifest["output_sha256"] = hashlib.sha256(content).hexdigest()
            self.rewrite(manifest)
            with self.assertRaises(analyzer.EvidenceError):
                analyzer.load_evidence(self.path)

    def test_duplicate_json_key_rejected(self):
        content = self.path.read_text(encoding="utf-8")
        self.path.write_text('{"schema": 2,' + content[1:], encoding="utf-8")
        with self.assertRaisesRegex(analyzer.EvidenceError, "duplicate JSON key"):
            analyzer.load_evidence(self.path)

    def test_silence_periodic_noise_and_wrong_direction_inconclusive(self):
        evidence = analyzer.load_evidence(self.path)
        size = evidence["frames"]
        rng = np.random.default_rng(732)
        sine = np.sin(np.arange(size) * 2 * np.pi / 128) * 0.1
        cases = [(np.zeros(size), np.zeros(size), "silent_or_constant_signal"),
                 (np.ones(size) * 0.1, np.ones(size) * 0.2, "silent_or_constant_signal"),
                 (sine, np.roll(sine, 43), "ambiguous_correlation_peaks"),
                 (evidence["reference"], rng.normal(0, 0.1, size), "insufficient_correlation"),
                 (evidence["reference"], evidence["reference"].copy(), "insufficient_correlation"),
                 (evidence["reference"], np.roll(evidence["reference"], -731), "insufficient_correlation")]
        for reference, capture, reason in cases:
            with self.subTest(reason=reason):
                current = dict(evidence, reference=reference, capture=capture)
                result = analyzer.estimate(current, maximum_lag=2048)
                self.assertEqual(result["status"], "inconclusive")
                self.assertIn(reason, result["reasons"])

    def test_changing_delay_rejected(self):
        evidence = analyzer.load_evidence(self.path)
        reference = evidence["reference"]
        capture = np.roll(reference, 731)
        middle = len(reference) // 3
        capture[middle:middle * 2] = np.roll(reference, 1111)[middle:middle * 2]
        result = analyzer.estimate(dict(evidence, capture=capture), maximum_lag=2048)
        self.assertEqual(result["status"], "inconclusive")
        self.assertIn("delay_not_consistent_across_recording", result["reasons"])

    def test_fft_orientation_and_overlap_normalization_against_direct_math(self):
        rng = np.random.default_rng(98)
        reference = rng.normal(0.2, 0.1, 113)
        capture = rng.normal(-0.3, 0.1, 113)
        lags, actual = analyzer.correlations(reference, capture, 15)
        for lag, value in zip(lags, actual):
            if lag >= 0:
                x, y = reference[:len(reference) - lag], capture[lag:]
            else:
                x, y = reference[-lag:], capture[:len(capture) + lag]
            self.assertAlmostEqual(value, np.corrcoef(x, y)[0, 1], places=11)


if __name__ == "__main__":
    unittest.main(verbosity=2)
