#pragma once

#include "host_lock.h"

namespace gpvst3::hook {

struct State {
    bool installed = false;
    const char *reason = "p0_disabled";
};

inline State prepare(const host::Verification &) noexcept {
    // P0 deliberately performs no private ABI patching.
    return {};
}

}
