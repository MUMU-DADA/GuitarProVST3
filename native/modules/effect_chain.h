#pragma once

namespace gpvst3::effects {

class Chain {
public:
    bool bypassed() const noexcept { return bypassed_; }
    void setBypassed(bool value) noexcept { bypassed_ = value; }

private:
    bool bypassed_ = true;
};

}
