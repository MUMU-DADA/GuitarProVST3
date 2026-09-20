// Real installed VST3, synthetic guitar-like input, production processor/router.
// This measures DSP budget; it does not claim physical ASIO or listening acceptance.
#include "p8_runtime_test.cpp"
#include <QtCore/QTemporaryDir>
#include <QtWidgets/QApplication>
#include <xmmintrin.h>

namespace gpvst3::hook::asioprobe {
bool install(void *) noexcept { return false; }
CallbackIdentity currentCallback() noexcept { return {}; }
StreamIdentity currentStream() noexcept { return {}; }
bool currentRate(double &, int &) noexcept { return false; }
bool requestNativeReset() noexcept { return false; }
bool beginOutputSrc(const void *, SrcInputObserver) noexcept { return false; }
SrcObservation finishOutputSrc() noexcept { return {}; }
QJsonObject snapshot() { return {}; }
}

namespace {
using namespace gpvst3;
using namespace gpvst3::hook;

// Test-only transparent delegate: keep RuntimeEffect and audio_adapter intact,
// and bracket exactly one third-party process call with the same QPC clock.
class TimedProcessor final : public Steinberg::U::Implements<
    Steinberg::U::Directly<Steinberg::Vst::IAudioProcessor>> {
public:
    explicit TimedProcessor(Steinberg::Vst::IAudioProcessor *target) : target_(target) {}
    Steinberg::tresult PLUGIN_API setBusArrangements(Steinberg::Vst::SpeakerArrangement *inputs,
        Steinberg::int32 numInputs, Steinberg::Vst::SpeakerArrangement *outputs,
        Steinberg::int32 numOutputs) override {
        return target_->setBusArrangements(inputs, numInputs, outputs, numOutputs);
    }
    Steinberg::tresult PLUGIN_API getBusArrangement(Steinberg::Vst::BusDirection direction,
        Steinberg::int32 index, Steinberg::Vst::SpeakerArrangement &arrangement) override {
        return target_->getBusArrangement(direction, index, arrangement);
    }
    Steinberg::tresult PLUGIN_API canProcessSampleSize(Steinberg::int32 size) override {
        return target_->canProcessSampleSize(size);
    }
    Steinberg::uint32 PLUGIN_API getLatencySamples() override { return target_->getLatencySamples(); }
    Steinberg::tresult PLUGIN_API setupProcessing(Steinberg::Vst::ProcessSetup &setup) override {
        return target_->setupProcessing(setup);
    }
    Steinberg::tresult PLUGIN_API setProcessing(Steinberg::TBool state) override {
        return target_->setProcessing(state);
    }
    Steinberg::tresult PLUGIN_API process(Steinberg::Vst::ProcessData &data) override {
        lastFrames = data.numSamples;
        ++calls;
        const auto started = steadyNanoseconds();
        const auto result = target_->process(data);
        lastNanoseconds = steadyNanoseconds() - started;
        return result;
    }
    Steinberg::uint32 PLUGIN_API getTailSamples() override { return target_->getTailSamples(); }
    std::uint64_t calls = 0, lastNanoseconds = 0;
    Steinberg::int32 lastFrames = 0;
private:
    Steinberg::IPtr<Steinberg::Vst::IAudioProcessor> target_;
};

// Restore and release the proxy before RuntimeEffect can unload its module,
// including when an assertion throws during the measured loop.
class ProcessorTimingAttachment final {
public:
    explicit ProcessorTimingAttachment(RuntimeEffect &effect)
        : effect_(effect), original_(effect.processor),
          proxy_(Steinberg::owned(new TimedProcessor(original_.get()))) {
        effect_.processor = Steinberg::FUnknownPtr<Steinberg::Vst::IAudioProcessor>(proxy_.get());
        require(effect_.processor.get() == proxy_.get(), "attach transparent processor timing proxy");
    }
    ~ProcessorTimingAttachment() { detach(); }
    TimedProcessor &proxy() noexcept { return *proxy_; }
    void detach() noexcept {
        if (!proxy_) return;
        effect_.processor = original_;
        proxy_ = nullptr;
        original_ = nullptr;
    }
private:
    RuntimeEffect &effect_;
    Steinberg::FUnknownPtr<Steinberg::Vst::IAudioProcessor> original_;
    Steinberg::IPtr<TimedProcessor> proxy_;
};

void hashSample(std::uint64_t &hash, float sample) noexcept {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &sample, sizeof(bits));
    for (unsigned byte = 0; byte < sizeof(bits); ++byte) {
        hash ^= (bits >> (byte * 8)) & 0xffU;
        hash *= 1099511628211ULL;
    }
}

QJsonObject distribution(std::vector<std::uint64_t> values) {
    if (values.empty()) return {{"blocks", 0}};
    std::sort(values.begin(), values.end());
    const auto percentile = [&](double p) { return qint64(values[std::size_t(p * (values.size() - 1))]); };
    double sum = 0;
    for (const auto value : values) sum += double(value);
    return {{"blocks", qint64(values.size())}, {"p50_ns", percentile(0.50)},
        {"p95_ns", percentile(0.95)}, {"p99_ns", percentile(0.99)},
        {"minimum_ns", qint64(values.front())}, {"maximum_ns", qint64(values.back())},
        {"mean_ns", sum / values.size()}};
}

void verifyParameterMailboxes(Steinberg::Vst::IEditController *controller) {
    require(controller && controller->getParameterCount() > 0, "real plugin exposes parameters");
    vst3::ParameterChanges changes;
    require(changes.prepare(controller), "prepare parameter mailboxes");
    std::vector<Steinberg::Vst::ParamID> ids;
    for (int i = 0; i < controller->getParameterCount(); ++i) {
        Steinberg::Vst::ParameterInfo info{};
        require(succeeded(controller->getParameterInfo(i, info)), "enumerate real parameter IDs");
        ids.push_back(info.id);
        require(changes.publish(info.id, 0.375), "publish distinct parameter mailbox");
    }
    changes.drain();
    require(changes.getParameterCount() == static_cast<int>(ids.size()), "all edited parameters delivered");
    for (int i = 0; i < changes.getParameterCount(); ++i) {
        auto *queue = changes.getParameterData(i);
        Steinberg::int32 offset = -1;
        double value = -1;
        require(queue && queue->getParameterId() == ids[i] &&
                succeeded(queue->getPoint(0, offset, value)) && offset == 0 && value == 0.375,
                "independent values and sample offsets retained");
    }
    changes.clear();
    changes.drain();
    require(changes.getParameterCount() == 0, "unchanged parameter block stays empty");

    std::atomic<bool> finished{false};
    std::thread producer([&] {
        for (int i = 0; i < 20000; ++i) changes.publish(ids.back(), double(i % 100) / 100.0);
        changes.publish(ids.back(), 1.0);
        finished.store(true, std::memory_order_release);
    });
    double last = -1;
    bool valid = true;
    do {
        changes.drain();
        for (int i = 0; i < changes.getParameterCount(); ++i) {
            auto *queue = changes.getParameterData(i);
            Steinberg::int32 offset = -1;
            valid &= queue && queue->getParameterId() == ids.back() &&
                succeeded(queue->getPoint(0, offset, last)) && offset == 0 && last >= 0 && last <= 1;
        }
        changes.clear();
    } while (!finished.load(std::memory_order_acquire));
    producer.join();
    changes.drain();
    if (auto *queue = changes.getParameterData(0)) {
        Steinberg::int32 offset = -1;
        valid &= succeeded(queue->getPoint(0, offset, last));
    }
    require(valid && last == 1.0, "concurrent mailbox publication retains final UI edit");
}

QJsonObject measure(const char *plugin, int rate, int frames, int maxBlock,
                    double seconds, bool flushDenormals, bool verifyQueues) {
    RuntimeEffect effect;
    if (!effect.initialize(rate, maxBlock, fs::u8path(plugin)))
        throw std::runtime_error(effect.error);
    if (verifyQueues) verifyParameterMailboxes(effect.controller.get());
    ProcessorTimingAttachment timing(effect);
    input::Router router;
    require(router.prepare(2, maxBlock), "prepare input router");
    router.setProcessor({&effect, &RuntimeEffect::processCallback});
    router.setRoute(input::Route::Overlay);
    router.setEnabled(true);
    router.setStreamRunning(true);
    const auto count = static_cast<std::size_t>(std::ceil(seconds * rate / frames));
    const auto warmup = static_cast<std::size_t>(rate / frames);
    std::vector<float> capture((count + warmup) * frames), output(frames * 2);
    // Three non-harmonically identical plucked tones, each bounded below full
    // scale. Input samples are prepared before measurement, never in the callback.
    constexpr double pi = 3.14159265358979323846;
    for (std::size_t i = 0; i < capture.size(); ++i) {
        const double t = double(i) / rate;
        const double envelope = 0.20 * std::exp(-3.0 * std::fmod(t, 0.5));
        capture[i] = static_cast<float>(envelope * (std::sin(2 * pi * 110 * t) +
            0.4 * std::sin(2 * pi * 165 * t) + 0.2 * std::sin(2 * pi * 220 * t)));
    }
    std::vector<std::uint64_t> times, processTimes, residualTimes;
    std::array<std::vector<std::uint64_t>, 2> parityTimes;
    times.reserve(count);
    processTimes.reserve(count);
    residualTimes.reserve(count);
    for (auto &values : parityTimes) values.reserve((count + 1) / 2);
    struct ConsecutiveBlock {
        std::uint64_t sequence = 0, total = 0, process = 0, residual = 0, calls = 0;
        Steinberg::int32 frames = 0;
    };
    std::array<ConsecutiveBlock, 256> consecutive{};
    std::size_t consecutiveCount = 0;
    const double budget = 1.0e9 * frames / rate;
    std::uint64_t exceeded = 0, processExceeded = 0, nonFinite = 0, clipped = 0;
    std::uint64_t inputHash = 14695981039346656037ULL, outputHash = inputHash;
    for (std::size_t i = warmup * frames; i < capture.size(); ++i) hashSample(inputHash, capture[i]);
    double energy = 0, peak = 0;
    struct FloatingPointRestore {
        unsigned previous = _mm_getcsr();
        ~FloatingPointRestore() { _mm_setcsr(previous); }
    } floatingPoint;
    _mm_setcsr(flushDenormals ? floatingPoint.previous | 0x8040U : floatingPoint.previous & ~0x8040U);
    for (std::size_t i = 0; i < count + warmup; ++i) {
        std::fill(output.begin(), output.end(), 0.0F);
        const auto callsBefore = timing.proxy().calls;
        const auto start = steadyNanoseconds();
        const auto result = router.processInterleaved({capture.data() + i * frames, output.data(),
            std::size_t(frames), 1, 2, double(rate), std::size_t(frames)});
        const auto elapsed = steadyNanoseconds() - start;
        const auto processElapsed = timing.proxy().lastNanoseconds;
        const auto calls = timing.proxy().calls - callsBefore;
        require(result.completed && result.processed && !result.error, "real input processor completed");
        require(calls == 1 && timing.proxy().lastFrames == frames && processElapsed <= elapsed,
                "one processor invocation with unchanged frame count and nested timing per block");
        if (i >= warmup) {
            const auto sequence = i - warmup;
            times.push_back(elapsed);
            processTimes.push_back(processElapsed);
            residualTimes.push_back(elapsed - processElapsed);
            parityTimes[sequence & 1].push_back(processElapsed);
            if (consecutiveCount < consecutive.size())
                consecutive[consecutiveCount++] = {sequence, elapsed, processElapsed,
                    elapsed - processElapsed, calls, timing.proxy().lastFrames};
            exceeded += elapsed > budget;
            processExceeded += processElapsed > budget;
            for (const auto sample : output) {
                hashSample(outputHash, sample);
                nonFinite += !std::isfinite(sample);
                clipped += std::abs(sample) > 1.0F;
                peak = (std::max)(peak, double(std::abs(sample)));
                energy += double(sample) * sample;
            }
        }
    }
    require(nonFinite == 0 && energy > 1.0e-10, "plugin emits finite non-silent audio");
    QJsonArray consecutiveJson;
    for (std::size_t i = 0; i < consecutiveCount; ++i) {
        const auto &block = consecutive[i];
        consecutiveJson.append(QJsonObject{{"sequence", qint64(block.sequence)},
            {"total_ns", qint64(block.total)}, {"processor_ns", qint64(block.process)},
            {"router_adapter_residual_ns", qint64(block.residual)},
            {"processor_calls", qint64(block.calls)}, {"processor_frames", block.frames}});
    }
    std::sort(times.begin(), times.end());
    const auto percentile = [&](double p) { return qint64(times[std::size_t(p * (times.size() - 1))]); };
    QJsonObject result{{"plugin_name", QString::fromStdString(effect.name)}, {"rate", rate},
        {"frames", frames}, {"setup_max_frames", maxBlock}, {"flush_denormals", flushDenormals},
        {"blocks", qint64(count)}, {"audio_seconds", double(count * frames) / rate},
        {"warmup_audio_seconds", double(warmup * frames) / rate},
        {"parameter_count", effect.controller ? effect.controller->getParameterCount() : 0},
        {"budget_ns", budget}, {"p50_ns", percentile(0.50)}, {"p95_ns", percentile(0.95)},
        {"p99_ns", percentile(0.99)}, {"max_ns", qint64(times.back())},
        {"over_budget_blocks", qint64(exceeded)}, {"non_finite_samples", qint64(nonFinite)},
        {"over_full_scale_samples", qint64(clipped)}, {"output_peak", peak},
        {"plugin_latency_samples", qint64(effect.processor->getLatencySamples())},
        {"output_rms", std::sqrt(energy / (count * frames * 2))},
        {"sample_hash_algorithm", "fnv1a64_float32_bits_little_endian_measured_blocks_only"},
        {"input_sample_hash", QString::number(inputHash, 16).rightJustified(16, '0')},
        {"output_sample_hash", QString::number(outputHash, 16).rightJustified(16, '0')},
        {"processor_timing_scope", "transparent_test_delegate_around_IAudioProcessor_process_QPC_only"},
        {"timing_includes_probe_overhead", true},
        {"processor_over_budget_blocks", qint64(processExceeded)},
        {"processor", distribution(processTimes)},
        {"router_adapter_residual", distribution(residualTimes)},
        {"processor_even_sequences", distribution(parityTimes[0])},
        {"processor_odd_sequences", distribution(parityTimes[1])},
        {"consecutive_blocks", consecutiveJson}};
    timing.detach();
    effect.shutdown();
    return result;
}
}

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QTemporaryDir data;
    qputenv("GPVST3_DATA_DIR", data.path().toUtf8());
    qputenv("GPVST3_DISABLE_PROJECT_RESTORE", "1");
    try {
        require(argc == 6 && data.isValid(), "plugin, output, sample rate, frames and seconds required");
        const int rate = std::stoi(argv[3]), frames = std::stoi(argv[4]);
        const double seconds = std::stod(argv[5]);
        require(rate >= 44100 && rate <= 192000 && frames > 0 && frames <= 2048 &&
                std::isfinite(seconds) && seconds > 0 && seconds <= 120, "valid measurement format");
        QJsonArray cases;
        for (const auto maxBlock : {2048, frames}) {
            for (const auto flush : {false, true}) {
                const auto result = measure(argv[1], rate, frames, maxBlock, seconds, flush, cases.isEmpty());
                cases.append(result);
                auto summary = result;
                summary.remove("consecutive_blocks");
                std::cout << QJsonDocument(summary).toJson(QJsonDocument::Compact).constData() << std::endl;
                QCoreApplication::processEvents();
            }
        }
        QFile output(QString::fromLocal8Bit(argv[2]));
        require(output.open(QIODevice::WriteOnly), "open timing evidence");
        output.write(QJsonDocument(QJsonObject{{"evidence_scope", "synthetic_input_real_vst3_no_audio_device"},
            {"timing_scope", "production_input_router_and_vst3_only_unpaced"},
            {"parameter_mailbox_verified", true}, {"physical_xrun_acceptance", false}, {"cases", cases}}).toJson());
        std::cout << "PASS: real VST3 processes finite non-silent input; budget observations saved.\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
