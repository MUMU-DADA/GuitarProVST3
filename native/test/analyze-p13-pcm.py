"""Validate P13 PCM evidence and estimate output-to-capture physical loopback lag."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import sys

import numpy as np


FRAME_CAPACITY = 262144
RECORD_CAPACITY = 8192
MAX_CALLBACK_FRAMES = 2048
RATE_SCOPE = "control_thread_observation_after_recording_not_generation_bound"
RATE_SOURCE = "ASIOGetSampleRate_thunk_0x6A540_ASIOError"
CALLBACK_RATE_SCOPE = "validated_per_callback_generation_and_rate_revision"
MEASUREMENT_SCOPE = "device_output_to_device_capture_roundtrip_not_input_monitor_latency"


class EvidenceError(ValueError):
    pass


def require(condition: bool, reason: str) -> None:
    if not condition:
        raise EvidenceError(reason)


def integer(value: object, name: str, *, minimum: int = 0, maximum: int = 2**64 - 1) -> int:
    require(type(value) is int, f"{name}: expected JSON integer")
    require(minimum <= value <= maximum, f"{name}: integer out of range")
    return value


def decimal(value: object, name: str) -> int:
    require(isinstance(value, str) and len(value) <= 20 and re.fullmatch(r"0|[1-9][0-9]*", value) is not None,
            f"{name}: expected unsigned decimal string")
    result = int(value)
    require(result <= 2**64 - 1, f"{name}: uint64 overflow")
    return result


def rate(value: object, name: str) -> float:
    require(type(value) in (int, float) and 8000 <= value <= 768000 and math.isfinite(value),
            f"{name}: invalid audio sample rate")
    return float(value)


def unique_object(items: list[tuple[str, object]]) -> dict:
    result = {}
    for key, value in items:
        require(key not in result, f"duplicate JSON key: {key}")
        result[key] = value
    return result


def reject_constant(value: str) -> None:
    raise EvidenceError(f"invalid JSON numeric constant: {value}")


def read_pcm(directory: Path, name: object, digest: object) -> bytes:
    require(isinstance(name, str) and name not in ("", ".", "..") and
            not any(character in name for character in ("/", "\\", ":")),
            "PCM path must be a local filename")
    require(isinstance(digest, str) and re.fullmatch(r"[0-9a-fA-F]{64}", digest) is not None,
            f"{name}: invalid SHA-256")
    path = (directory / name).resolve(strict=True)
    require(path.parent == directory, f"{name}: PCM path escapes manifest directory")
    with path.open("rb") as stream:
        content = stream.read(FRAME_CAPACITY * 2 * 4 + 1)
    require(len(content) <= FRAME_CAPACITY * 2 * 4, f"{name}: PCM exceeds frame capacity")
    require(hashlib.sha256(content).hexdigest() == digest.lower(), f"{name}: SHA-256 mismatch")
    return content


def load_evidence(path: Path, capture_channel: int = 0) -> dict:
    """Load one immutable PCM export; reject any gap or invalid sample evidence."""
    path = path.resolve(strict=True)
    with path.open("rb") as stream:
        content = stream.read(16 * 1024 * 1024 + 1)
    require(len(content) <= 16 * 1024 * 1024, "manifest exceeds bounded schema size")
    manifest = json.loads(content, object_pairs_hook=unique_object, parse_constant=reject_constant)
    require(isinstance(manifest, dict), "manifest must be a JSON object")
    require(manifest.get("schema") == 1 and type(manifest.get("schema")) is int, "unsupported schema")
    require(manifest.get("enabled") is True and manifest.get("complete") is True,
            "PCM recorder is disabled or incomplete")
    require(not manifest.get("error"), "PCM export reports an error")
    require(manifest.get("format") == "float32_little_endian_interleaved_per_record",
            "unsupported PCM encoding")
    require(manifest.get("rate_scope") == RATE_SCOPE, "unknown rate observation scope")
    require(manifest.get("actual_rate_query_source") == RATE_SOURCE, "unknown ASIO sample-rate query ABI")
    query_result = integer(manifest.get("actual_rate_query_result"), "actual_rate_query_result",
                           minimum=-(2**31), maximum=2**31 - 1)
    # This query occurs after recording. A later rate change or failed query
    # cannot invalidate a complete, generation-bound callback-rate history.
    observed_rate = (rate(manifest.get("actual_asio_sample_rate"), "actual_asio_sample_rate")
                     if query_result == 0 else None)
    require(decimal(manifest.get("busy_drops"), "busy_drops") == 0, "concurrent callback records were dropped")
    require(decimal(manifest.get("abandoned"), "abandoned") == 0, "PCM record was abandoned")
    claimed = integer(manifest.get("claimed"), "claimed", minimum=1, maximum=RECORD_CAPACITY)
    require(integer(manifest.get("published"), "published") == claimed, "unpublished PCM records")
    total = integer(manifest.get("frames_reserved"), "frames_reserved", minimum=1, maximum=FRAME_CAPACITY)
    require(total == FRAME_CAPACITY or claimed == RECORD_CAPACITY,
            "complete manifest did not reach either recorder capacity")
    records = manifest.get("records")
    require(isinstance(records, list) and len(records) == claimed, "record count mismatch")
    capture_bytes = read_pcm(path.parent, manifest.get("capture_file"), manifest.get("capture_sha256"))
    output_bytes = read_pcm(path.parent, manifest.get("output_file"), manifest.get("output_sha256"))
    require(manifest["capture_file"] != manifest["output_file"], "capture and output must use distinct files")
    capture = np.empty(total, dtype=np.float64)
    reference = np.empty(total, dtype=np.float64)
    capture_offset = output_offset = frame_offset = 0
    previous_sequence = previous_timestamp = None
    identity = None
    callback_lengths = set()
    driver_mapping = None
    callback_rate_identity = None
    callback_rate_fields = ("rate_revision", "actual_callback_rate", "actual_rate_validated")
    has_callback_rate = False
    for index, record in enumerate(records):
        require(isinstance(record, dict), f"record {index}: expected object")
        if index == 0:
            has_callback_rate = any(key in record for key in callback_rate_fields)
        require(integer(record.get("index"), "index") == index, "record index discontinuity")
        require(record.get("configuration_valid") is True, f"record {index}: unvalidated host configuration")
        require(integer(record.get("changes"), "changes") == 0, f"record {index}: stream identity changed")
        require(decimal(record.get("status_flags"), "status_flags") == 0, f"record {index}: callback/xrun status flags")
        require(integer(record.get("original_result"), "original_result", minimum=-(2**31)) == 0,
                f"record {index}: original callback did not continue")
        sequence = decimal(record.get("sequence"), "sequence")
        require(sequence != 0 and (previous_sequence is None or sequence == previous_sequence + 1),
                f"record {index}: callback sequence gap")
        timestamp = decimal(record.get("timestamp_ns"), "timestamp_ns")
        require(timestamp != 0 and (previous_timestamp is None or timestamp > previous_timestamp),
                f"record {index}: non-increasing callback time")
        owner = record.get("owner")
        require(isinstance(owner, str) and re.fullmatch(r"[0-9a-fA-F]{1,16}", owner) is not None and int(owner, 16) != 0,
                f"record {index}: missing/invalid owner")
        generation = decimal(record.get("generation"), "generation")
        present_rate_fields = [key in record for key in callback_rate_fields]
        require(all(present_rate_fields) if has_callback_rate else not any(present_rate_fields),
                f"record {index}: partial or inconsistent callback-rate schema")
        if has_callback_rate:
            revision = decimal(record["rate_revision"], "rate_revision")
            validated = record["actual_rate_validated"]
            require(type(validated) is bool, f"record {index}: invalid callback-rate validation flag")
            callback_rate = record["actual_callback_rate"]
            if validated:
                require(generation > 0 and revision > 0, f"record {index}: callback rate has no generation/revision")
                callback_rate = rate(callback_rate, "actual_callback_rate")
            else:
                require(type(callback_rate) in (int, float), f"record {index}: invalid unvalidated callback rate")
                if callback_rate != 0:
                    callback_rate = rate(callback_rate, "actual_callback_rate")
            current_rate_identity = (revision, callback_rate, validated)
            if callback_rate_identity is None:
                callback_rate_identity = current_rate_identity
            require(current_rate_identity == callback_rate_identity,
                    f"record {index}: callback rate/revision differs despite changes=0")
        thread = integer(record.get("thread"), "thread", minimum=1, maximum=2**32 - 1)
        requested_rate = rate(record.get("requested_sample_rate"), "requested_sample_rate")
        require(integer(record.get("host_api_type"), "host_api_type") == 3, "capture backend is not verified ASIO")
        driver_frames = integer(record.get("driver_frames"), "driver_frames", minimum=1, maximum=2**32 - 1)
        input_device = integer(record.get("input_device"), "input_device", maximum=2**31 - 1)
        output_device = integer(record.get("output_device"), "output_device", maximum=2**31 - 1)
        input_channels = integer(record.get("input_channels"), "input_channels", minimum=1, maximum=2)
        require(integer(record.get("output_channels"), "output_channels", minimum=1, maximum=2) == 2,
                "right-output analysis requires two output channels")
        require(0 <= capture_channel < input_channels, "selected capture channel is unavailable")
        if "driver_channels_validated" in record:
            require(record["driver_channels_validated"] is True, "driver channel mapping is unvalidated")
            inputs_map = record.get("driver_input_selectors")
            outputs_map = record.get("driver_output_selectors")
            require(isinstance(inputs_map, list) and len(inputs_map) == 2 and
                    isinstance(outputs_map, list) and len(outputs_map) == 2, "invalid driver mapping shape")
            for channel, selector in enumerate(inputs_map):
                integer(selector, "input selector", minimum=-1, maximum=65535)
                require(selector >= 0 if channel < input_channels else selector == -1, "invalid input selector")
            for selector in outputs_map:
                integer(selector, "output selector", maximum=65535)
            mapping = (tuple(inputs_map), tuple(outputs_map))
            if index == 0:
                driver_mapping = mapping
            require(driver_mapping == mapping, "driver channel mapping changed")
        else:
            require(driver_mapping is None, "driver channel mapping disappeared")
        current_identity = (int(owner, 16), generation, thread, requested_rate, driver_frames,
                            input_device, output_device, input_channels)
        if identity is None:
            identity = current_identity
        require(current_identity == identity, f"record {index}: identity differs despite changes=0")
        frames = integer(record.get("callback_frames"), "callback_frames", minimum=1, maximum=MAX_CALLBACK_FRAMES)
        saved = integer(record.get("saved_frames"), "saved_frames", minimum=1, maximum=frames)
        require(integer(record.get("frame_offset"), "frame_offset") == frame_offset, "frame-offset discontinuity")
        require(type(record.get("truncated")) is bool and record["truncated"] == (saved != frames),
                f"record {index}: inconsistent truncation flag")
        require(saved == frames or (index == claimed - 1 and frame_offset + saved == FRAME_CAPACITY),
                "truncated callback is not the final capacity prefix")
        require(frame_offset + saved <= total, "records exceed reserved frames")
        require(integer(record.get("capture_status"), "capture_status") == 3 and
                integer(record.get("output_status"), "output_status") == 3,
                f"record {index}: PCM side is not Captured")
        require(integer(record.get("capture_nonfinite"), "capture_nonfinite") == 0 and
                integer(record.get("output_nonfinite"), "output_nonfinite") == 0,
                f"record {index}: non-finite sample count")
        input_size, output_size = saved * input_channels * 4, saved * 2 * 4
        require(integer(record.get("capture_byte_offset"), "capture_byte_offset") == capture_offset and
                integer(record.get("capture_bytes"), "capture_bytes") == input_size,
                f"record {index}: capture byte layout mismatch")
        require(integer(record.get("output_byte_offset"), "output_byte_offset") == output_offset and
                integer(record.get("output_bytes"), "output_bytes") == output_size,
                f"record {index}: output byte layout mismatch")
        require(capture_offset + input_size <= len(capture_bytes) and output_offset + output_size <= len(output_bytes),
                "PCM file is shorter than records")
        inputs = np.frombuffer(capture_bytes, dtype="<f4", count=saved * input_channels,
                               offset=capture_offset).reshape(saved, input_channels)
        outputs = np.frombuffer(output_bytes, dtype="<f4", count=saved * 2,
                                offset=output_offset).reshape(saved, 2)
        require(bool(np.isfinite(inputs).all() and np.isfinite(outputs).all()), "PCM contains undeclared non-finite values")
        capture[frame_offset:frame_offset + saved] = inputs[:, capture_channel]
        reference[frame_offset:frame_offset + saved] = outputs[:, 1]
        capture_offset += input_size
        output_offset += output_size
        frame_offset += saved
        previous_sequence, previous_timestamp = sequence, timestamp
        callback_lengths.add(frames)
    require(frame_offset == total, "record frame sum differs from reserved frames")
    require(capture_offset == len(capture_bytes) and output_offset == len(output_bytes),
            "PCM file contains trailing bytes not described by records")
    rate_bound = callback_rate_identity is not None and callback_rate_identity[2]
    require(rate_bound or query_result == 0, "ASIO sample-rate query did not return ASE_OK (0)")
    actual_rate = callback_rate_identity[1] if rate_bound else observed_rate
    return {"capture": capture, "reference": reference, "sample_rate": actual_rate,
            "observed_asio_sample_rate": observed_rate, "actual_rate_query_result": query_result,
            "sample_rate_bound_to_generation": rate_bound,
            "rate_revision": callback_rate_identity[0] if has_callback_rate else None,
            "validated_callback_sample_rate": callback_rate_identity[1] if rate_bound else None,
            "records": claimed, "frames": total, "callback_frames": sorted(callback_lengths),
            "capture_channel": capture_channel, "input_channels": identity[-1],
            "driver_capture_channel": driver_mapping[0][capture_channel] if driver_mapping else None,
            "driver_right_output_channel": driver_mapping[1][1] if driver_mapping else None,
            "requested_sample_rate": identity[3], "generation": identity[1],
            "capture_sha256": manifest["capture_sha256"], "output_sha256": manifest["output_sha256"]}


def correlations(reference: np.ndarray, capture: np.ndarray, maximum_lag: int) -> tuple[np.ndarray, np.ndarray]:
    """Overlap-normalized Pearson correlation; positive lag means capture follows output."""
    size = len(reference)
    transform_size = 1 << (2 * size - 1).bit_length()
    cross = np.fft.irfft(np.fft.rfft(capture, transform_size) *
                        np.conj(np.fft.rfft(reference, transform_size)), transform_size)
    lags = np.arange(-maximum_lag, maximum_lag + 1, dtype=np.int64)
    x_start, y_start = np.maximum(-lags, 0), np.maximum(lags, 0)
    x_end, y_end = size - y_start, size - x_start
    count = size - np.abs(lags)
    def moments(values: np.ndarray, start: np.ndarray, end: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
        sums = np.concatenate(([0.0], np.cumsum(values)))
        squares = np.concatenate(([0.0], np.cumsum(values * values)))
        total = sums[end] - sums[start]
        variance = np.maximum(squares[end] - squares[start] - total * total / count, 0.0)
        return total, variance
    x_sum, x_variance = moments(reference, x_start, x_end)
    y_sum, y_variance = moments(capture, y_start, y_end)
    numerator = cross[lags % transform_size] - x_sum * y_sum / count
    denominator = np.sqrt(x_variance * y_variance)
    scores = np.zeros(len(lags), dtype=np.float64)
    np.divide(numerator, denominator, out=scores, where=denominator > np.finfo(float).tiny)
    return lags, np.clip(scores, -1.0, 1.0)


def signal_statistics(signal: np.ndarray) -> dict:
    centered = signal - signal.mean()
    return {"rms": float(np.sqrt(np.mean(signal * signal))),
            "ac_rms": float(np.sqrt(np.mean(centered * centered))),
            "peak": float(np.abs(signal).max()),
            "clipped_fraction": float(np.mean(np.abs(signal) >= 0.999999))}


def estimate(evidence: dict, *, maximum_lag: int = 16384, minimum_correlation: float = 0.65,
             minimum_margin: float = 0.05, sample_rate: float | None = None) -> dict:
    require(type(maximum_lag) is int and maximum_lag >= 1, "maximum lag must be positive")
    require(0.0 < minimum_correlation <= 1.0 and 0.0 < minimum_margin < 1.0, "invalid confidence thresholds")
    actual_rate = evidence["sample_rate"] if sample_rate is None else rate(sample_rate, "explicit sample rate")
    rate_bound = sample_rate is None and evidence["sample_rate_bound_to_generation"]
    rate_scope = CALLBACK_RATE_SCOPE if rate_bound else RATE_SCOPE
    result = {key: value for key, value in evidence.items() if key not in ("capture", "reference", "sample_rate")}
    result.update({"schema": 1, "status": "inconclusive", "measurement_scope": MEASUREMENT_SCOPE,
                   "p13_acceptance": False, "sample_rate": actual_rate,
                   "rate_scope": rate_scope if sample_rate is None else "explicit_override_not_generation_bound",
                   "sample_rate_bound_to_generation": rate_bound, "output_channel_zero_based": 1,
                   "minimum_correlation": minimum_correlation, "minimum_peak_margin": minimum_margin,
                   "reasons": []})
    reference, capture = evidence["reference"], evidence["capture"]
    result["reference_signal"] = signal_statistics(reference)
    result["capture_signal"] = signal_statistics(capture)
    if min(result["reference_signal"]["ac_rms"], result["capture_signal"]["ac_rms"]) < 1e-7:
        result["reasons"].append("silent_or_constant_signal")
        return result
    if max(result["reference_signal"]["clipped_fraction"], result["capture_signal"]["clipped_fraction"]) > 0.01:
        result["reasons"].append("substantial_signal_clipping")
    maximum_lag = min(maximum_lag, len(reference) // 8)
    if maximum_lag < 1 or len(reference) < 4096:
        result["reasons"].append("insufficient_continuous_samples")
        return result
    lags, signed = correlations(reference, capture, maximum_lag)
    scores = np.abs(signed)
    positive = np.flatnonzero(lags > 0)
    best = int(positive[np.argmax(scores[positive])])
    peak = float(scores[best])
    lag = int(lags[best])
    # Separate the main lobe at the nearest half-height crossings. A periodic
    # signal's next peak remains a competitor; a broad single lobe is reported
    # as a wide interval instead of manufacturing sample-level precision.
    left = right = best
    while left > 0 and scores[left - 1] >= peak * 0.5:
        left -= 1
    while right + 1 < len(scores) and scores[right + 1] >= peak * 0.5:
        right += 1
    competitors = scores.copy()
    competitors[left:right + 1] = -1.0
    alternative_index = int(np.argmax(competitors))
    resolved_lobe = bool(competitors[alternative_index] >= 0.0)
    alternative = float(competitors[alternative_index]) if resolved_lobe else peak
    margin = peak - alternative
    result.update({"maximum_lag_samples": maximum_lag, "candidate_lag_samples": lag,
                   "signed_correlation": float(signed[best]), "absolute_correlation": peak,
                   "competing_lag_samples": int(lags[alternative_index]) if resolved_lobe else None,
                   "competing_absolute_correlation": alternative, "peak_margin": margin,
                   "main_lobe_half_height_samples": [int(lags[left]), int(lags[right])],
                   "polarity": "inverted" if signed[best] < 0 else "same"})
    suffix = "" if rate_bound else "_conditional"
    result["candidate_delay_seconds" + suffix] = lag / actual_rate
    result["candidate_delay_ms" + suffix] = lag * 1000.0 / actual_rate
    if peak < minimum_correlation:
        result["reasons"].append("insufficient_correlation")
    if margin < minimum_margin:
        result["reasons"].append("ambiguous_correlation_peaks")
    if not resolved_lobe:
        result["reasons"].append("main_lobe_spans_entire_search_range")
    if lag == maximum_lag:
        result["reasons"].append("peak_at_search_boundary")
    if left <= maximum_lag:  # Index maximum_lag is lag zero.
        result["reasons"].append("peak_lobe_includes_nonpositive_delay")
    segment_results = []
    for begin, end in zip(np.linspace(0, len(reference), 4, dtype=int)[:-1],
                          np.linspace(0, len(reference), 4, dtype=int)[1:]):
        segment_lags, segment_scores = correlations(reference[begin:end], capture[begin:end], maximum_lag)
        segment_best = int(np.argmax(np.abs(segment_scores)))
        segment_results.append({"begin_frame": int(begin), "end_frame": int(end),
                                "lag_samples": int(segment_lags[segment_best]),
                                "absolute_correlation": float(abs(segment_scores[segment_best]))})
    result["temporal_segments"] = segment_results
    tolerance = max(2, min(16, (right - left) // 2))
    if any(abs(segment["lag_samples"] - lag) > tolerance or
           segment["absolute_correlation"] < minimum_correlation for segment in segment_results):
        result["reasons"].append("delay_not_consistent_across_recording")
    if not result["reasons"]:
        result["status"] = "physical_loopback_estimate" if rate_bound else "conditional_physical_loopback_estimate"
    return result


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--capture-channel", type=int, default=0, help="zero-based captured channel")
    parser.add_argument("--max-lag", type=int, default=16384, help="maximum positive/negative search lag in samples")
    parser.add_argument("--sample-rate", type=float, help="explicit conversion rate; remains unbound to stream generation")
    parser.add_argument("--minimum-correlation", type=float, default=0.65)
    parser.add_argument("--minimum-margin", type=float, default=0.05)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args(argv)
    try:
        evidence = load_evidence(args.manifest, args.capture_channel)
        result = estimate(evidence, maximum_lag=args.max_lag, sample_rate=args.sample_rate,
                          minimum_correlation=args.minimum_correlation, minimum_margin=args.minimum_margin)
        code = 0 if result["status"] in ("physical_loopback_estimate", "conditional_physical_loopback_estimate") else 1
    except (EvidenceError, OSError, ValueError, KeyError, TypeError) as error:
        result = {"schema": 1, "status": "invalid_evidence", "p13_acceptance": False,
                  "measurement_scope": MEASUREMENT_SCOPE, "error": str(error)}
        code = 2
    encoded = json.dumps(result, ensure_ascii=True, allow_nan=False, indent=2) + "\n"
    if args.output:
        args.output.write_text(encoded, encoding="utf-8")
    print(encoded, end="")
    return code


if __name__ == "__main__":
    sys.exit(main())
