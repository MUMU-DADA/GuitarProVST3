#include "gp_hook.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef max
#undef min

namespace gpvst3::hook {
namespace {

constexpr char kMasterProcess[] =
    "?process@Master@rse@gp@@QEAAXAEAVAudioBuffer@audio@am@@AEBV?$vector@VTick@audio@am@@V?$allocator@VTick@audio@am@@@std@@@std@@AEBV?$vector@PEAVMusician@rse@gp@@V?$allocator@PEAVMusician@rse@gp@@@std@@@8@AEBV?$shared_ptr@VBackingTrack@rse@gp@@@8@@Z";
constexpr char kEffectsChainProcessDsp[] =
    "?processDSP@EffectsChain@rse@gp@@QEAAXAEAVIAudioBuffer@audio@am@@AEAV?$array@VAudioBuffer@audio@am@@$02@std@@AEBV?$vector@VTick@audio@am@@V?$allocator@VTick@audio@am@@@std@@@8@@Z";
constexpr char kRawData[] =
    "?rawData@AudioBuffer@audio@am@@UEBAAEBV?$array@PEAM$01@std@@XZ";
constexpr char kFrameCount[] =
    "?frameCount@AudioBuffer@audio@am@@UEBA_JXZ";
constexpr char kChannelCount[] =
    "?channelCount@AudioBuffer@audio@am@@UEBAIXZ";

EntryPointObservation observe(HMODULE module, const char *symbol) noexcept {
    EntryPointObservation result;
    result.moduleLoaded = module != nullptr;
    result.exportFound = module && GetProcAddress(module, symbol) != nullptr;
    return result;
}

}

State prepare(const host::Verification &verification) noexcept {
    State result;
    result.hostSupported = verification.supported;
    if (!verification.supported) {
        result.reason = "host_unsupported";
        return result;
    }

    const auto gprse = GetModuleHandleW(L"GPRSE.dll");
    const auto amaudio = GetModuleHandleW(L"AMAudio.dll");
    result.masterProcess = observe(gprse, kMasterProcess);
    result.effectsChainProcessDsp = observe(gprse, kEffectsChainProcessDsp);
    result.audioBufferAccessorsFound =
        observe(amaudio, kRawData).exportFound && observe(amaudio, kFrameCount).exportFound &&
        observe(amaudio, kChannelCount).exportFound;
    if (!gprse)
        result.reason = "gprse_not_loaded";
    else if (!result.masterProcess.exportFound && !result.effectsChainProcessDsp.exportFound)
        result.reason = "entry_points_not_found";
    else
        result.reason = "p2_observation_only_callsite_unverified";
    return result;
}

} // namespace gpvst3::hook
