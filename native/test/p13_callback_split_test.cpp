// Calls the production hook with borrowed buffers and synthetic host metadata.
// No hook is installed and no device/native application is opened.
#include "p8_runtime_test.cpp"
#include <QtWidgets/QApplication>

namespace {
gpvst3::hook::asioprobe::CallbackIdentity callbackIdentity{1, 1, 192000, true};
}
namespace gpvst3::hook::asioprobe {
bool install(void *) noexcept { return false; }
CallbackIdentity currentCallback() noexcept { return callbackIdentity; }
StreamIdentity currentStream() noexcept { return {}; }
bool currentRate(double &rate, int &result) noexcept { rate = 0; result = -1; return false; }
bool requestNativeReset() noexcept { return false; }
bool beginOutputSrc(const void *, SrcInputObserver) noexcept { return false; }
SrcObservation finishOutputSrc() noexcept { return {}; }
QJsonObject snapshot() { return {}; }
}

namespace {
using namespace gpvst3::hook;
std::size_t inputChannels = 0, outputChannels = 0, total = 0, cursor = 0, calls = 0, stopAt = 0;
const float *inputBase = nullptr;
float *outputBase = nullptr;
bool valid = true;
std::array<double, 3> initialTimes{{11.0, 12.0, 13.0}};
int nativeCallback(const void *input, void *output, unsigned long frames,
                   const void *timeInfo, unsigned long status, void *) {
    ++calls;
    valid &= frames > 0 && frames <= portaudio::kMaxFrames && cursor + frames <= total && status == 2;
    valid &= input == (inputBase ? inputBase + cursor * inputChannels : nullptr);
    valid &= output == outputBase + cursor * outputChannels;
    if (timeInfo) {
        const auto *time = static_cast<const double *>(timeInfo);
        const auto shift = callbackIdentity.rateValidated ? double(cursor) / callbackIdentity.actualRate : 0;
        for (std::size_t i = 0; i < initialTimes.size(); ++i)
            valid &= std::abs(time[i] - initialTimes[i] - (i == 1 ? 0 : shift)) < 1e-12;
        valid &= std::abs((time[2] - time[1]) - (initialTimes[2] - initialTimes[1]) - shift) < 1e-12;
    }
    auto *destination = static_cast<float *>(output);
    for (std::size_t f = 0; f < frames; ++f)
        for (std::size_t c = 0; c < outputChannels; ++c)
            destination[f * outputChannels + c] = input
                ? static_cast<const float *>(input)[f * inputChannels + c % inputChannels] * 0.5F : 0;
    cursor += frames;
    return stopAt == calls ? 1 : 0;
}
template <typename T> void put(void *p, std::size_t offset, T value) {
    std::memcpy(static_cast<std::uint8_t *>(p) + offset, &value, sizeof(value));
}
void runCase(unsigned long frames, std::size_t ins, std::size_t outs, double rate,
             bool validated = true, bool missingInput = false, bool stopEarly = false) {
    std::vector<std::uint8_t> module(0x270900), parent(0x180), stream(0x200);
    std::array<std::uint8_t, 40> owner{};
    put(owner.data(), 0, parent.data()); put(owner.data(), 8, stream.data());
    put(parent.data(), 0x178, owner.data());
    put(stream.data(), 0, std::uint32_t{0x18273645});
    put(stream.data(), 0x48, 44100.0);
    put(module.data(), portaudio::kInputParametersRva,
        portaudio::StreamParameters{0, std::int32_t(ins), 1, 0, 0, nullptr});
    put(module.data(), portaudio::kOutputParametersRva,
        portaudio::StreamParameters{0, std::int32_t(outs), 1, 0, 0, nullptr});
    std::vector<float> input(std::size_t(frames) * ins), output(std::size_t(frames) * outs + 16, -7.0F);
    for (std::size_t i = 0; i < input.size(); ++i) input[i] = float(i % 101) / 101.0F;
    callbackIdentity = {1, 1, validated ? rate : 0, validated};
    inputChannels = ins; outputChannels = outs; total = frames; cursor = calls = 0;
    stopAt = stopEarly ? 2 : 0;
    inputBase = missingInput ? nullptr : input.data(); outputBase = output.data(); valid = true;
    g_runtime.audioModule = reinterpret_cast<HMODULE>(module.data());
    g_runtime.stream.trampoline = reinterpret_cast<void *>(&nativeCallback);
    const auto result = streamCallbackHook(inputBase, output.data(), frames, initialTimes.data(), 2, owner.data());
    if (!valid) std::cerr << "case frames=" << frames << " rate=" << rate << " validated=" << validated
                         << " missing_input=" << missingInput << " in=" << ins << " out=" << outs << '\n';
    require(valid, "chunk callback pointers, native capacity, flags and timestamps stay correct");
    require(result == (stopEarly ? 1 : 0), "native terminal result propagates");
    require(calls == (stopEarly ? 2 : (frames + 2047) / 2048), "every requested frame is visited once");
    for (std::size_t f = 0; f < frames; ++f)
        for (std::size_t c = 0; c < outs; ++c) {
            const auto expected = f >= cursor || missingInput ? 0 : input[f * ins + c % ins] * 0.5F;
            require(output[f * outs + c] == expected, "entire output including large-buffer tail is initialized");
        }
    for (std::size_t i = std::size_t(frames) * outs; i < output.size(); ++i)
        require(output[i] == -7.0F, "no write beyond callback output range");
}
}

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    try {
        for (const auto rate : {44100., 48000., 88200., 96000., 176400., 192000.})
            for (const unsigned long frames : {32UL, 64UL, 128UL, 256UL, 512UL, 1024UL, 2048UL, 4096UL, 8192UL})
                for (const auto channels : {std::pair<std::size_t, std::size_t>{1, 2}, {2, 1}, {2, 2}})
                    runCase(frames, channels.first, channels.second, rate);
        runCase(8192, 2, 2, 192000, false);
        runCase(8192, 2, 2, 192000, true, true);
        runCase(8192, 2, 2, 192000, true, false, true);
        g_runtime.audioModule = nullptr;
        g_runtime.stream.trampoline = nullptr;
        std::cout << "PASS: production callback split, 6 rates x 9 buffers x 3 channel mappings, startup, absent capture and native completion.\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
