"""Validate source/return PCM and compare native versus overlay monitor delay."""
from __future__ import annotations

import argparse
import importlib.util
import json
import math
from pathlib import Path
import sys

sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location("p13_pcm", Path(__file__).with_name("analyze-p13-pcm.py"))
pcm = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pcm)


def metadata(path: Path) -> dict:
    with path.open("rb") as stream:
        content = stream.read(16 * 1024 * 1024 + 1)
    pcm.require(len(content) <= 16 * 1024 * 1024, "measurement metadata exceeds bounded schema size")
    result = json.loads(content, object_pairs_hook=pcm.unique_object, parse_constant=pcm.reject_constant)
    pcm.require(isinstance(result, dict), "measurement metadata must be a JSON object")
    return result


def analyze(run: Path, expected_mode: str) -> dict:
    pcm.require(expected_mode in ("native", "overlay"), "unsupported measurement mode")
    run = run.resolve(strict=True)
    collection = metadata(run / "collection.json")
    shutdown = metadata(run / "shutdown.json")
    manifest_path = run / "data/p13-pcm.json"
    manifest = metadata(manifest_path)
    measurement = collection.get("monitor_latency", {})
    pcm.require(isinstance(measurement, dict), "monitor latency metadata must be an object")
    pcm.require(collection.get("status") == "collected_unvalidated", "collection did not complete")
    pcm.require(measurement.get("configured") is True and measurement.get("mode") == expected_mode,
                "measurement mode was not configured")
    pcm.require(collection.get("cleanup_errors") == [] and collection["loopback_input2"]["restore_confirmed"] is True,
                "measurement configuration was not restored")
    pcm.require(type(shutdown.get("exit_code")) is int and shutdown["exit_code"] == 0 and
                shutdown.get("forced") is False and shutdown.get("exited") is True, "host did not exit normally")
    pcm.require(manifest.get("monitor_latency_measurement") is True and
                manifest.get("capture_stage") == "raw_device_capture_before_measurement_mapping",
                "raw synchronous source/return recording is required")
    configured_ns = pcm.decimal(measurement["configured_ns"], "configured_ns")
    pcm.require(configured_ns > 0, "measurement configuration timestamp is missing")
    pcm.require(isinstance(manifest.get("records"), list), "PCM records must be an array")
    for record in manifest["records"]:
        pcm.require(isinstance(record, dict), "PCM record must be an object")
        pcm.require(record.get("monitor_reference_is_input1") is True and
                    record.get("driver_channels_validated") is True and
                    record["driver_input_selectors"] == [0, 1] and record["driver_output_selectors"] == [0, 1],
                    "unverified measurement routing")
        pcm.require(pcm.decimal(record["timestamp_ns"], "timestamp_ns") >= configured_ns,
                    "recording preceded routing validation")
    source = pcm.load_evidence(manifest_path, 0)
    returned = pcm.load_evidence(manifest_path, 1)
    pcm.require(source["sample_rate_bound_to_generation"] and returned["sample_rate_bound_to_generation"],
                "sample rate must be bound to every callback")
    returned["reference"] = source["capture"]
    result = pcm.estimate(returned, maximum_lag=32768)
    result["measurement_scope"] = "synchronous_raw_input1_to_monitor_DAC_cable_ADC_raw_input2; test_input1_duplicated_to_stereo"
    result["mode"] = expected_mode
    result["source_driver_channel"] = 0
    result["return_driver_channel"] = 1
    result["device_configuration"] = {key: manifest["records"][0][key]
                                      for key in ("input_device", "output_device", "driver_frames",
                                                  "requested_sample_rate")}
    result["native_software_gain"] = measurement["native_gain"]
    pcm.require(type(result["native_software_gain"]) in (int, float) and
                0 < result["native_software_gain"] <= 1000, "invalid native software gain")
    result["rse_score"] = measurement.get("rse_score", "unspecified; RSE coexistence not established")
    result["collection"] = str(run / "collection.json")
    result["status"] = "monitor_return_estimate" if result["status"] == "physical_loopback_estimate" else "inconclusive"
    if expected_mode == "overlay":
        runtime = collection["input_overlay"]["runtime"]
        pcm.require(runtime["state"] == "active" and runtime["mode"] == "low_latency_overlay" and
                    runtime["input_native_effect_bypass"] is True and
                    pcm.decimal(runtime["error_blocks"], "error_blocks") == 0 and
                    pcm.decimal(runtime["processed_blocks"], "processed_blocks") > 0,
                    "overlay end-of-run state is not active and error-free")
        pcm.require(pcm.rate(runtime["sample_rate"], "runtime sample_rate") == source["sample_rate"] and
                    pcm.integer(runtime["driver_buffer_frames"], "driver_buffer_frames", minimum=1) ==
                    result["device_configuration"]["driver_frames"], "overlay runtime timing differs from PCM evidence")
        result["monitor_gain"] = runtime["gain"]
        pcm.require(type(result["monitor_gain"]) in (int, float) and result["monitor_gain"] > 0 and
                    math.isfinite(result["monitor_gain"]), "invalid monitor gain")
        result["plugin_latency_samples"] = pcm.decimal(runtime["plugin_latency_samples"], "plugin_latency_samples")
        result["clipped_blocks"] = pcm.decimal(runtime["clipped_blocks"], "clipped_blocks")
    else:
        pcm.require(collection.get("input_overlay") is None, "native measurement also configured the input overlay")
    return result


def compare(native_run: Path, overlay_run: Path) -> dict:
    native, overlay = analyze(native_run, "native"), analyze(overlay_run, "overlay")
    pcm.require(native["sample_rate"] == overlay["sample_rate"] and
                native["callback_frames"] == overlay["callback_frames"] and
                native["device_configuration"] == overlay["device_configuration"],
                "device configuration or timing changed between runs")
    result = {"schema": 1, "status": "inconclusive", "p13_acceptance": False, "native": native, "overlay": overlay}
    if all(item["status"] == "monitor_return_estimate" for item in (native, overlay)):
        result["reduction_ms"] = native["candidate_delay_ms"] - overlay["candidate_delay_ms"]
        result["status"] = "measured_monitor_delay_comparison"
    return result


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("native", type=Path)
    parser.add_argument("overlay", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args(argv)
    try:
        result = compare(args.native, args.overlay)
        code = 0 if result["status"] == "measured_monitor_delay_comparison" else 1
    except (pcm.EvidenceError, OSError, ValueError, KeyError, TypeError, OverflowError) as error:
        result = {"schema": 1, "status": "invalid_evidence", "error": str(error), "p13_acceptance": False}
        code = 2
    encoded = json.dumps(result, ensure_ascii=True, indent=2, allow_nan=False) + "\n"
    if args.output:
        args.output.write_text(encoded, encoding="utf-8")
    print(encoded, end="")
    return code


if __name__ == "__main__":
    sys.exit(main())
