// Test VST3, loaded only inside Guitar Pro. Its native Qt IPlugView sends
// performEdit; the processor independently measures input/output energy.
#include "pluginterfaces/base/funknownimpl.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>
#include <atomic>
#include <cmath>
#include <cstring>

namespace Steinberg { DEF_CLASS_IID(IPlugView) }
using namespace Steinberg;
using namespace Steinberg::Vst;
namespace {
const FUID cid(0x10203040, 0x50607080, 0x11223344, 0x55667788);
class Gain final : public U::Implements<U::Directly<IComponent, IAudioProcessor, IEditController>> {
public:
    IPtr<IComponentHandler> handler;
    double controlValue = 1.0;
    std::atomic<double> gain{1.0}, inputEnergy{0}, outputEnergy{0};
    std::atomic<unsigned long long> edits{0}, blocks{0};
    tresult PLUGIN_API initialize(FUnknown *) override { return kResultOk; }
    tresult PLUGIN_API terminate() override { handler = nullptr; return kResultOk; }
    tresult PLUGIN_API getControllerClassId(TUID) override { return kNoInterface; }
    tresult PLUGIN_API setIoMode(IoMode) override { return kResultOk; }
    int32 PLUGIN_API getBusCount(MediaType type, BusDirection) override { return type == kAudio ? 1 : 0; }
    tresult PLUGIN_API getBusInfo(MediaType type, BusDirection direction, int32 index, BusInfo &info) override {
        if (type != kAudio || index != 0) return kInvalidArgument;
        info = {}; info.mediaType = type; info.direction = direction; info.channelCount = 2;
        info.busType = kMain; info.flags = BusInfo::kDefaultActive;
        return kResultOk;
    }
    tresult PLUGIN_API getRoutingInfo(RoutingInfo &, RoutingInfo &) override { return kNotImplemented; }
    tresult PLUGIN_API activateBus(MediaType, BusDirection, int32, TBool) override { return kResultOk; }
    tresult PLUGIN_API setActive(TBool) override { return kResultOk; }
    tresult PLUGIN_API setState(IBStream *stream) override {
        double value = 1; int32 bytes = 0;
        if (!stream || stream->read(&value, sizeof(value), &bytes) != kResultOk || bytes != sizeof(value)) return kResultFalse;
        if (!std::isfinite(value) || value < 0 || value > 1) return kInvalidArgument;
        controlValue = value; gain.store(value); return kResultOk;
    }
    tresult PLUGIN_API getState(IBStream *stream) override {
        auto value = gain.load(); return stream ? stream->write(&value, sizeof(value)) : kInvalidArgument;
    }
    tresult PLUGIN_API setBusArrangements(SpeakerArrangement *inputs, int32 ins, SpeakerArrangement *outputs, int32 outs) override {
        return ins == 1 && outs == 1 && inputs[0] == SpeakerArr::kStereo && outputs[0] == SpeakerArr::kStereo ? kResultOk : kResultFalse;
    }
    tresult PLUGIN_API getBusArrangement(BusDirection, int32 index, SpeakerArrangement &value) override {
        value = SpeakerArr::kStereo; return index == 0 ? kResultOk : kInvalidArgument;
    }
    tresult PLUGIN_API canProcessSampleSize(int32 size) override { return size == kSample32 ? kResultOk : kResultFalse; }
    uint32 PLUGIN_API getLatencySamples() override { return 0; }
    tresult PLUGIN_API setupProcessing(ProcessSetup &setup) override { return canProcessSampleSize(setup.symbolicSampleSize); }
    tresult PLUGIN_API setProcessing(TBool) override { return kResultOk; }
    tresult PLUGIN_API process(ProcessData &data) override {
        double value = gain.load();
        if (data.inputParameterChanges) for (int32 i = 0; i < data.inputParameterChanges->getParameterCount(); ++i) {
            auto *queue = data.inputParameterChanges->getParameterData(i);
            int32 offset = 0;
            if (queue && queue->getParameterId() == 1 && queue->getPoint(queue->getPointCount() - 1, offset, value) == kResultOk) {
                gain.store(value); edits.fetch_add(1); inputEnergy.store(0); outputEnergy.store(0);
            }
        }
        if (data.numInputs != 1 || data.numOutputs != 1) return kInvalidArgument;
        double input = 0, output = 0;
        for (int c = 0; c < data.outputs[0].numChannels; ++c) for (int32 i = 0; i < data.numSamples; ++i) {
            const float sample = data.inputs[0].channelBuffers32[c][i];
            const float processed = sample * static_cast<float>(value);
            data.outputs[0].channelBuffers32[c][i] = processed;
            input += double(sample) * sample; output += double(processed) * processed;
        }
        inputEnergy.store(inputEnergy.load() + input); outputEnergy.store(outputEnergy.load() + output);
        blocks.fetch_add(1);
        return kResultOk;
    }
    uint32 PLUGIN_API getTailSamples() override { return 0; }
    tresult PLUGIN_API setComponentState(IBStream *stream) override { return setState(stream); }
    int32 PLUGIN_API getParameterCount() override { return 1; }
    tresult PLUGIN_API getParameterInfo(int32 index, ParameterInfo &info) override {
        if (index != 0) return kInvalidArgument;
        info = {}; info.id = 1; info.defaultNormalizedValue = 1; info.flags = ParameterInfo::kCanAutomate;
        const char *name = "Gain"; for (int i = 0; name[i]; ++i) info.title[i] = name[i];
        return kResultOk;
    }
    tresult PLUGIN_API getParamStringByValue(ParamID, ParamValue, String128) override { return kNotImplemented; }
    tresult PLUGIN_API getParamValueByString(ParamID, TChar *, ParamValue &) override { return kNotImplemented; }
    ParamValue PLUGIN_API normalizedParamToPlain(ParamID, ParamValue value) override { return value; }
    ParamValue PLUGIN_API plainParamToNormalized(ParamID, ParamValue value) override { return value; }
    ParamValue PLUGIN_API getParamNormalized(ParamID) override { return controlValue; }
    tresult PLUGIN_API setParamNormalized(ParamID id, ParamValue value) override {
        if (id != 1) return kInvalidArgument;
        controlValue = value; return kResultOk;
    }
    tresult PLUGIN_API setComponentHandler(IComponentHandler *value) override { handler = value; return kResultOk; }
    IPlugView *PLUGIN_API createView(FIDString) override;
};

class View final : public U::Implements<U::Directly<IPlugView>> {
    Gain &owner;
    QPointer<QWidget> widget;
    IPtr<IPlugFrame> frame;
    ViewRect size{0, 0, 400, 200};
public:
    explicit View(Gain &effect) : owner(effect) {}
    ~View() { removed(); }
    tresult PLUGIN_API isPlatformTypeSupported(FIDString type) override { return std::strcmp(type, kPlatformTypeHWND) == 0 ? kResultOk : kResultFalse; }
    tresult PLUGIN_API attached(void *parent, FIDString type) override {
        auto *host = QWidget::find(reinterpret_cast<WId>(parent));
        if (!host || isPlatformTypeSupported(type) != kResultOk) return kResultFalse;
        widget = new QWidget(host);
        widget->setAttribute(Qt::WA_NativeWindow);
        auto *layout = new QVBoxLayout(widget);
        auto *gain = new QDoubleSpinBox(widget);
        gain->setObjectName("gpvst3TestGain"); gain->setRange(0, 1); gain->setSingleStep(0.25); gain->setValue(owner.controlValue);
        layout->addWidget(gain);
        auto *status = new QLabel(widget); status->setObjectName("gpvst3TestProcessor"); status->setWordWrap(true); layout->addWidget(status);
        QObject::connect(gain, QOverload<double>::of(&QDoubleSpinBox::valueChanged), widget, [this](double value) {
            owner.controlValue = value;
            if (owner.handler) { owner.handler->beginEdit(1); owner.handler->performEdit(1, value); owner.handler->endEdit(1); }
        });
        auto *resize = new QPushButton("Resize / move test editor", widget); resize->setObjectName("gpvst3TestResize"); layout->addWidget(resize);
        QObject::connect(resize, &QPushButton::clicked, widget, [this] {
            size = ViewRect(0, 0, 480, 240);
            if (frame) frame->resizeView(this, &size);
            widget->window()->move(widget->window()->pos() + QPoint(50, 30));
        });
        auto *timer = new QTimer(widget);
        QObject::connect(timer, &QTimer::timeout, widget, [this, status] {
            status->setText(QString::fromUtf8(QJsonDocument(QJsonObject{
                {"instance", QString::number(reinterpret_cast<quintptr>(&owner), 16)}, {"gain", owner.gain.load()},
                {"edits", qint64(owner.edits.load())}, {"blocks", qint64(owner.blocks.load())},
                {"input_energy", owner.inputEnergy.load()}, {"output_energy", owner.outputEnergy.load()}}).toJson(QJsonDocument::Compact)));
        });
        timer->start(100); widget->resize(size.getWidth(), size.getHeight()); widget->show(); return kResultOk;
    }
    tresult PLUGIN_API removed() override { delete widget.data(); widget = nullptr; return kResultOk; }
    tresult PLUGIN_API onWheel(float) override { return kResultFalse; }
    tresult PLUGIN_API onKeyDown(char16, int16, int16) override { return kResultFalse; }
    tresult PLUGIN_API onKeyUp(char16, int16, int16) override { return kResultFalse; }
    tresult PLUGIN_API getSize(ViewRect *value) override { if (!value) return kInvalidArgument; *value = size; return kResultOk; }
    tresult PLUGIN_API onSize(ViewRect *value) override { if (!value) return kInvalidArgument; size = *value; if (widget) widget->resize(size.getWidth(), size.getHeight()); return kResultOk; }
    tresult PLUGIN_API onFocus(TBool) override { return kResultOk; }
    tresult PLUGIN_API setFrame(IPlugFrame *value) override { frame = value; return kResultOk; }
    tresult PLUGIN_API canResize() override { return kResultFalse; }
    tresult PLUGIN_API checkSizeConstraint(ViewRect *) override { return kResultOk; }
};
IPlugView *Gain::createView(FIDString) { return new View(*this); }

class Factory final : public U::Implements<U::Directly<IPluginFactory2>> {
public:
    tresult PLUGIN_API getFactoryInfo(PFactoryInfo *info) override { if (!info) return kInvalidArgument; *info = {}; std::strcpy(info->vendor, "GuitarProVST3 Test"); return kResultOk; }
    int32 PLUGIN_API countClasses() override { return 1; }
    tresult PLUGIN_API getClassInfo(int32 index, PClassInfo *info) override {
        if (index || !info) return kInvalidArgument;
        *info = {}; cid.toTUID(info->cid); std::strcpy(info->category, "Audio Module Class"); std::strcpy(info->name, "P7 Gain Fixture"); return kResultOk;
    }
    tresult PLUGIN_API getClassInfo2(int32 index, PClassInfo2 *info) override {
        if (index || !info) return kInvalidArgument;
        *info = {}; cid.toTUID(info->cid); std::strcpy(info->category, "Audio Module Class"); std::strcpy(info->name, "P7 Gain Fixture"); std::strcpy(info->subCategories, "Fx"); return kResultOk;
    }
    tresult PLUGIN_API createInstance(FIDString classId, FIDString iid, void **object) override {
        if (!FUnknownPrivate::iidEqual(classId, cid)) return kNoInterface;
        auto effect = Steinberg::owned(new Gain);
        return effect->queryInterface(iid, object);
    }
};
}
extern "C" __declspec(dllexport) bool InitDll() { return true; }
extern "C" __declspec(dllexport) bool ExitDll() { return true; }
extern "C" __declspec(dllexport) IPluginFactory *GetPluginFactory() { return new Factory; }
