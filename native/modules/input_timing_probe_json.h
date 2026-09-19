#pragma once

#include "input_timing_probe.h"
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>

namespace gpvst3::input::timingprobe {

inline QJsonObject histogramJson(const HistogramSnapshot &samples) {
    const auto percentile = [&](unsigned percent) {
        const auto upper = samples.percentileUpper(percent);
        return upper ? QJsonValue(qint64(upper)) : QJsonValue();
    };
    QJsonArray buckets;
    for (const auto count : samples.buckets) buckets.append(qint64(count));
    return {{"count", QString::number(samples.count)}, {"bucket_width_ns", qint64(kBucketNanoseconds)},
        {"overflow_at_ns", qint64(kHistogramBuckets * kBucketNanoseconds)},
        {"overflow", QString::number(samples.overflow)}, {"buckets", buckets},
        {"p50_exclusive_upper_ns", percentile(50)}, {"p95_exclusive_upper_ns", percentile(95)},
        {"maximum_ns", QString::number(samples.maximum)}};
}

inline QJsonObject snapshotJson(const Snapshot &value, const char *scope, const char *budgetScope) {
    return {{"schema", 1}, {"enabled", value.enabled}, {"stopped", value.stopped},
        {"acceptance", "not_evaluated"}, {"scope", scope}, {"coherent_snapshot", value.coherent},
        {"window_start_ns", QString::number(value.windowStart)}, {"window_end_ns", QString::number(value.windowEnd)},
        {"maximum_window_ns", QString::number(kWindowNanoseconds)}, {"maximum_callbacks", QString::number(kCallbackLimit)},
        {"time_bound_reached", value.timeBoundReached}, {"callback_bound_reached", value.callbackBoundReached},
        {"first_started_ns", QString::number(value.firstStarted)}, {"last_ended_ns", QString::number(value.lastEnded)},
        {"admitted_callbacks", QString::number(value.admitted)}, {"completed_callbacks", QString::number(value.completed)},
        {"overlapping_callbacks_skipped", QString::number(value.overlappingCallbacks)},
        {"status_flag_callbacks", QString::number(value.statusCallbacks)}, {"status_flags_or", QString::number(value.statusFlags)},
        {"callback_result_errors", QString::number(value.resultErrors)}, {"invalid_clock_callbacks", QString::number(value.invalidClocks)},
        {"budget_scope", budgetScope}, {"budget_validated_callbacks", QString::number(value.validatedBudgets)},
        {"budget_unvalidated_callbacks", QString::number(value.unvalidatedBudgets)},
        {"callback_budget_exceedances", QString::number(value.callbackBudgetExceeded)},
        {"input_process_budget_exceedances", QString::number(value.inputBudgetExceeded)},
        {"input_process_calls", QString::number(value.inputCalls)}, {"input_process_failures", QString::number(value.inputFailures)},
        {"validated_identity_changes", QString::number(value.identityChanges)},
        {"minimum_validated_rate", value.minimumRate}, {"maximum_validated_rate", value.maximumRate},
        {"minimum_validated_frames", qint64(value.minimumFrames)}, {"maximum_validated_frames", qint64(value.maximumFrames)},
        {"callback", histogramJson(value.callback)}, {"input_process", histogramJson(value.inputProcess)}};
}

} // namespace gpvst3::input::timingprobe
