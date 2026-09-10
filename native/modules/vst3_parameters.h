#pragma once

#include <atomic>
#include <memory>
#include "pluginterfaces/base/funknownimpl.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"

namespace gpvst3::vst3 {

// Each parameter owns its mailbox, so editing two controls between audio
// callbacks cannot overwrite one another. Storage is allocated at activation.
class ParameterQueue final : public Steinberg::U::Implements<
    Steinberg::U::Directly<Steinberg::Vst::IParamValueQueue>> {
public:
    Steinberg::Vst::ParamID id = 0;
    std::atomic<double> pendingValue{0};
    std::atomic<bool> pending{false};
    double value = 0;
    Steinberg::Vst::ParamID PLUGIN_API getParameterId() override { return id; }
    Steinberg::int32 PLUGIN_API getPointCount() override { return 1; }
    Steinberg::tresult PLUGIN_API getPoint(Steinberg::int32 index, Steinberg::int32 &offset,
                                          Steinberg::Vst::ParamValue &out) override {
        if (index != 0) return Steinberg::kInvalidArgument;
        offset = 0; out = value; return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API addPoint(Steinberg::int32, Steinberg::Vst::ParamValue,
                                          Steinberg::int32 &) override {
        return Steinberg::kNotImplemented;
    }
};

class ParameterChanges final : public Steinberg::U::Implements<
    Steinberg::U::Directly<Steinberg::Vst::IParameterChanges>> {
public:
    bool prepare(Steinberg::Vst::IEditController *controller) {
        size_ = controller ? controller->getParameterCount() : 0;
        if (size_ < 0 || size_ > 16384) return false;
        queues_ = std::make_unique<ParameterQueue[]>(size_);
        active_ = std::make_unique<ParameterQueue *[]>(size_);
        count_ = 0;
        for (int i = 0; i < size_; ++i) {
            Steinberg::Vst::ParameterInfo info{};
            if (controller->getParameterInfo(i, info) != Steinberg::kResultOk) return false;
            queues_[i].id = info.id;
        }
        return true;
    }
    bool publish(Steinberg::Vst::ParamID id, double value) noexcept {
        if (value < 0 || value > 1) return false;
        for (int i = 0; i < size_; ++i) if (queues_[i].id == id) {
            queues_[i].pendingValue.store(value, std::memory_order_relaxed);
            queues_[i].pending.store(true, std::memory_order_release);
            return true;
        }
        return false;
    }
    void drain() noexcept {
        count_ = 0;
        for (int i = 0; i < size_; ++i)
            if (queues_[i].pending.exchange(false, std::memory_order_acq_rel)) {
                queues_[i].value = queues_[i].pendingValue.load(std::memory_order_relaxed);
                active_[count_++] = &queues_[i];
            }
    }
    void clear() noexcept { count_ = 0; }
    Steinberg::int32 PLUGIN_API getParameterCount() override { return count_; }
    Steinberg::Vst::IParamValueQueue *PLUGIN_API getParameterData(Steinberg::int32 index) override {
        return index >= 0 && index < count_ ? active_[index] : nullptr;
    }
    Steinberg::Vst::IParamValueQueue *PLUGIN_API addParameterData(
        const Steinberg::Vst::ParamID &, Steinberg::int32 &index) override {
        index = -1; return nullptr;
    }
private:
    int size_ = 0, count_ = 0;
    std::unique_ptr<ParameterQueue[]> queues_;
    std::unique_ptr<ParameterQueue *[]> active_;
};

}
