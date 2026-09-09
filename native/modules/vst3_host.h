#pragma once

namespace gpvst3::vst3 {

struct State {
    const char *status = "pending_p1";
    bool ready = false;
};

inline State prepare() noexcept { return {}; }

}
