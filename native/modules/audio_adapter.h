#pragma once

#include <cstddef>

namespace gpvst3::audio {

// P0 only records the shape expected by the later adapter. GP buffers are not
// touched until a real processing callback is verified in P2.
struct BlockView {
    float **channels = nullptr;
    std::size_t channelCount = 0;
    std::size_t frameCount = 0;
    double sampleRate = 0.0;
};

}
