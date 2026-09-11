// Exercise production runtime code with a real test VST3 and deterministic
// buffers. Only host discovery and editor window placement are substituted.
#include "../modules/gp_hook.cpp"
#include <iostream>
#include <stdexcept>

namespace {
std::vector<gpvst3::gp_audio::Binding> testBindings;
int testRate = 44100;
int readRate(const void *) { return testRate; }
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
}
namespace gpvst3::gp_audio {
std::size_t refresh() noexcept { return testBindings.size(); }
std::vector<Binding> snapshot() { return testBindings; }
bool currentTrack(Binding &binding) noexcept { if (testBindings.empty()) return false; binding = testBindings[0]; return true; }
const char *bindingSource() noexcept { return "fixture"; }
}
namespace gpvst3::ui {
void resizeNativeEditor(void *, int, int) {}
double nativeEditorScale(void *) { return 1; }
}
extern "C" __declspec(dllexport) int gpvst3_run_runtime_tests(const char *fixture) {
    using namespace gpvst3;
    using namespace gpvst3::hook;
    try {
        require(fixture && *fixture && qApp, "test VST3 path and real Qt host required");
        std::vector<Vst3SelectionEntry> entries;
        for (const auto &id : {"41302010605080701122334455667788", "42302010605080701122334455667788", "43302010605080701122334455667788"})
            entries.push_back({fixture, id});
        QJsonArray effects;
        for (const auto &entry : entries) effects.append(QJsonObject{{"module", fixture}, {"class_id", QString::fromStdString(entry.classId)}, {"enabled", true}});
        QJsonObject saved;
        state::setScopeEffects(saved, state::ScopeKind::Global, effects);
        state::setScopeEffects(saved, state::ScopeKind::Track, effects, "score", "track", 0);
        require(state::writeChain(saved), "write initial state");
        gp_audio::Binding binding;
        binding.chain = reinterpret_cast<void *>(1); binding.trackId = "track"; binding.trackKey = "track";
        binding.scoreKey = "score"; binding.documentId = "document"; binding.trackIndex = 0; binding.selectedTrack = true;
        testBindings.push_back(binding);
        g_initial.hostSupported = true;
        // No patch is installed by this fixture; calls enter the production
        // control API after substituting discovery and the rate accessor.
        g_runtime.master.installed = true; g_runtime.dsp.installed = true;
        g_runtime.sampleRate = &readRate; g_runtime.audioCore = reinterpret_cast<void *>(1);
        refreshTrackContext();
        std::string error;
        require(setTrackVst3Selection("track", entries, &error), "prepare track processors");
        require(setGlobalVst3Selection(entries, &error), "prepare global processors");
        auto &track = g_runtime.trackRuntimes[0];
        auto *trackProcessor = track.trackSlots[track.chain.snapshot().activeSlot].effects[0].get();
        auto *globalProcessor = g_runtime.selectionSlots[g_runtime.chain.snapshot().activeSlot].effects[0].get();
        require(trackProcessor != globalProcessor, "scope processors are independent");
        float left[256], right[256]; float *channels[]{left, right};
        auto block = [&](int rate) { return audio::BlockView{nullptr, nullptr, nullptr, channels, 2, 256, double(rate), 256}; };
        auto reset = [&] { std::fill_n(left, 256, 0.4F); std::fill_n(right, 256, 0.4F); };
        for (const int rate : {44100, 48000, 96000, 44100}) {
            testRate = rate;
            if (track.configuredRate.load() != rate) {
                reset(); bool processed = true;
                require(track.processBlock(block(rate), &processed) && !processed && left[0] == 0.4F,
                    "changed rate bypasses until reconfigured");
                require(!track.chain.faulted(), "rate transition must not latch an audio fault");
            }
            refreshTrackContext();
            require(track.configuredRate.load() == rate && globalProcessor->configuredRate.load() == rate,
                "both scopes adopt the new callback sample rate");
            require(track.trackSlots[track.chain.snapshot().activeSlot].effects[0].get() == trackProcessor &&
                g_runtime.selectionSlots[g_runtime.chain.snapshot().activeSlot].effects[0].get() == globalProcessor,
                "rate reconfiguration preserves processor identity");
            reset(); bool processed = false;
            require(track.processBlock(block(rate), &processed) && processed && std::abs(left[0] - 0.5125F) < 0.000001F,
                "track actual sample after reconfiguration");
            reset(); require(g_runtime.chain.process(block(rate)).completed && std::abs(left[0] - 0.5125F) < 0.000001F,
                "global actual sample after reconfiguration");
        }
        auto missing = entries; missing[1].module = "C:/missing/P8 Missing.vst3";
        require(!setTrackVst3Selection("track", missing, &error) && error == "runtime_vst3_not_found", "missing explicit selection rejected");
        reset(); require(track.processBlock(block(testRate)) && std::abs(left[0] - 0.5125F) < 0.000001F,
            "failed selection preserves the previous live track chain");
        // Saved state rejection disables only the failing entry and restores
        // the two remaining real processors in their original order.
        auto corrupted = entries; corrupted[1].componentState = {1, 2, 3};
        const auto restored = restoreSavedEntries(corrupted, state::ScopeKind::Global, {}, {}, -1,
            [](const auto &candidate, std::string *failure) { return setGlobalVst3Selection(candidate, failure); });
        require(restored.size() == 2 && restored[0].classId == entries[0].classId && restored[1].classId == entries[2].classId,
            "restore isolates an invalid component state");
        require(state::loadChain(saved), "read rejected state");
        require(!state::scopeEffects(saved, state::ScopeKind::Global)[1].toObject().value("enabled").toBool(),
            "failed auto restore is not persisted as active");
        require(setTrackVst3Selection("track", entries, &error), "restore explicit track selection");
        auto &slot = track.trackSlots[track.chain.snapshot().activeSlot];
        slot.effects[1]->forceError = true;
        reset(); require(!track.processBlock(block(testRate)), "failed process is detected");
        refreshTrackContext();
        reset(); require(track.processBlock(block(testRate)) && std::abs(left[0] - 0.775F) < 0.000001F,
            "remaining track processors continue after failure isolation");
        reset(); require(g_runtime.chain.process(block(testRate)).completed && std::abs(left[0] - 0.775F) < 0.000001F,
            "a track failure does not change the independent global chain");
        require(consumeSelectionStateChanges(), "failure publishes UI reload notification");
        g_runtime.master.installed = false; g_runtime.dsp.installed = false;
        shutdown();
        std::cout << "PASS: P8 real VST3 buffers at 44100/48000/96000 Hz, scope/instance/state preservation, missing/invalid state rejection and per-entry process failure isolation.\n";
        return 0;
    } catch (const std::exception &error) {
        g_runtime.master.installed = false; g_runtime.dsp.installed = false;
        shutdown(); std::cerr << "FAIL: " << error.what() << '\n'; return 1;
    }
}
