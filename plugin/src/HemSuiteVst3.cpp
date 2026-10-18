// =============================================================================
//  HEM Suite — every HEM effect (hc::fx) as its own VST3 plug-in, all in one bundle
//  ("HEM Suite.vst3"). One generic Processor / Controller pair serves every effect: the
//  factory registers each effect's classes with a pointer to its fx::EffectInfo as the
//  creation context.
//
//  Per effect: one stereo in, one stereo out; its parameters (ids 0..n-1, same ranges and
//  text as hc::fx), a Bypass parameter (latency-matched, no click: 10 ms crossfade) and,
//  for the dynamics ones, a read-only Gain Reduction meter for the host's generic editor.
//  Editor: every effect has its own obsidian faceplate (hc::ui::FxPanelWindow, OpenGL, inside
//  the host's window). Live meters reach it through read-only output parameters.
//  Steinberg VST3 SDK: MIT licence (verified from the SDK's LICENSE.txt, v3.8.1).
// =============================================================================
#include "hybrid/fx/Dsp.hpp"
#include "hybrid/fx/Effects.hpp"
#include "hybrid/ui/FxPanel.hpp"
#include "hybrid/ui/FxPanelWindow.hpp"

#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/vsttypes.h"
#include "public.sdk/source/common/pluginview.h"
#include "public.sdk/source/main/pluginfactory.h"
#include "public.sdk/source/vst/vstaudioeffect.h"
#include "public.sdk/source/vst/vsteditcontroller.h"

#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>

#ifndef HC_VERSION_STR
#define HC_VERSION_STR "0.0.0"
#endif

using namespace Steinberg;

namespace hc::hemfx {

constexpr Vst::ParamID kBypassId = 1000;
constexpr Vst::ParamID kMeterId  = 1001;
constexpr Vst::ParamID kMeterBase = 1100;   // + i: the effect's own meters (read-only, for the faceplate)

float meterToNorm(const fx::MeterInfo& m, float v) { return m.max > m.min ? std::clamp((v - m.min) / (m.max - m.min), 0.0f, 1.0f) : 0.0f; }
float normToMeter(const fx::MeterInfo& m, double n) { return m.min + static_cast<float>(n) * (m.max - m.min); }

// The bundle's Resources folder (fonts/, faceplates/): <bundle>/Contents/x86_64-win/<dll> -> <bundle>/Contents/Resources.
std::filesystem::path bundleResources() {
    HMODULE m = nullptr;
    ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(&bundleResources), &m);
    wchar_t buf[MAX_PATH * 4];
    const DWORD n = ::GetModuleFileNameW(m, buf, static_cast<DWORD>(std::size(buf)));
    return std::filesystem::path(std::wstring(buf, n)).parent_path().parent_path() / "Resources";
}
constexpr char         kStateMagic[] = "HEMFX1\n";

FUID toFuid(const std::array<std::uint32_t, 4>& u) { return FUID(u[0], u[1], u[2], u[3]); }
std::u16string toU16(const std::string& s) { return std::u16string(s.begin(), s.end()); }   // ASCII names
bool hasMeter(const fx::EffectInfo& info) {
    const std::string c = info.subCategory, n = info.name;
    return c.find("Dynamics") != std::string::npos || c.find("Mastering") != std::string::npos ||
           c.find("Restoration") != std::string::npos || n == "HEM CLIP";
}

// ---- state: "HEMFX1\n" then id=value lines (plain values), bypass=0/1 -----------------
std::string stateText(const fx::Effect& e, bool bypass) {
    std::string out = kStateMagic;
    const auto ps = e.params();
    char b[64];
    for (std::size_t i = 0; i < ps.size(); ++i) {
        std::snprintf(b, sizeof b, "%.9g", static_cast<double>(e.get(static_cast<int>(i))));
        out += std::string(ps[i].id) + "=" + b + "\n";
    }
    out += std::string("bypass=") + (bypass ? "1" : "0") + "\n";
    return out;
}
bool writeText(IBStream* s, const std::string& t) {
    if (!s) return false;
    int32 w = 0;
    return s->write(const_cast<char*>(t.data()), static_cast<int32>(t.size()), &w) == kResultOk && w == static_cast<int32>(t.size());
}
bool readState(IBStream* s, fx::Effect& e, bool& bypass) {
    if (!s) return false;
    std::string text;
    char buf[2048];
    for (;;) {
        int32 got = 0;
        if (s->read(buf, sizeof buf, &got) != kResultOk || got <= 0) break;
        text.append(buf, static_cast<std::size_t>(got));
        if (text.size() > (1u << 16)) return false;   // our state is < 1 KB; refuse junk
    }
    if (text.compare(0, sizeof(kStateMagic) - 1, kStateMagic) != 0) return false;
    std::istringstream in(text.substr(sizeof(kStateMagic) - 1));
    std::string line;
    const auto ps = e.params();
    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq);
        const float v = std::strtof(line.c_str() + eq + 1, nullptr);
        if (k == "bypass") { bypass = v >= 0.5f; continue; }
        for (std::size_t i = 0; i < ps.size(); ++i)
            if (k == ps[i].id) e.set(static_cast<int>(i), v);   // unknown ids (older / newer versions) are skipped
    }
    return true;
}

// ---- processor -----------------------------------------------------------------------
class Processor final : public Vst::AudioEffect {
public:
    explicit Processor(const fx::EffectInfo* info) : info_(info), fx_(info->create()) { setControllerClass(toFuid(info->controllerUid)); }

    static FUnknown* createInstance(void* ctx) {
        return static_cast<Vst::IAudioProcessor*>(new Processor(static_cast<const fx::EffectInfo*>(ctx)));
    }

    tresult PLUGIN_API initialize(FUnknown* context) SMTG_OVERRIDE {
        const tresult r = AudioEffect::initialize(context);
        if (r != kResultOk) return r;
        addAudioInput(STR16("Stereo In"), Vst::SpeakerArr::kStereo);
        addAudioOutput(STR16("Stereo Out"), Vst::SpeakerArr::kStereo);
        return kResultOk;
    }

    tresult PLUGIN_API setBusArrangements(Vst::SpeakerArrangement* inputs, int32 numIns, Vst::SpeakerArrangement* outputs,
                                          int32 numOuts) SMTG_OVERRIDE {
        if (numIns != 1 || numOuts != 1 || inputs[0] != Vst::SpeakerArr::kStereo || outputs[0] != Vst::SpeakerArr::kStereo) return kResultFalse;
        return AudioEffect::setBusArrangements(inputs, numIns, outputs, numOuts);
    }

    tresult PLUGIN_API canProcessSampleSize(int32 size) SMTG_OVERRIDE { return size == Vst::kSample32 ? kResultTrue : kResultFalse; }

    tresult PLUGIN_API setupProcessing(Vst::ProcessSetup& setup) SMTG_OVERRIDE {
        sampleRate_ = setup.sampleRate;
        maxBlock_   = static_cast<std::uint32_t>(std::clamp<int32>(setup.maxSamplesPerBlock, 32, static_cast<int32>(fx::Effect::kMaxBlock)));
        fx_->prepare(sampleRate_, maxBlock_);
        for (auto& d : bypassDelay_) d.prepare(fx_->latency());
        for (auto& b : dry_) b.assign(maxBlock_, 0.0f);
        fadeStep_ = static_cast<float>(1.0 / (0.010 * sampleRate_));
        return AudioEffect::setupProcessing(setup);
    }

    tresult PLUGIN_API setActive(TBool state) SMTG_OVERRIDE {
        if (state) {
            fx_->reset();
            for (auto& d : bypassDelay_) d.clear();
            mix_ = bypass_ ? 0.0f : 1.0f;
        }
        return AudioEffect::setActive(state);
    }

    uint32 PLUGIN_API getLatencySamples() SMTG_OVERRIDE { return fx_->latency(); }
    uint32 PLUGIN_API getTailSamples() SMTG_OVERRIDE {
        if (std::string(info_->subCategory).find("Reverb") != std::string::npos) return static_cast<uint32>(15.0 * sampleRate_);
        if (std::string(info_->subCategory).find("Delay") != std::string::npos) return static_cast<uint32>(8.0 * sampleRate_);
        return fx_->latency();
    }

    tresult PLUGIN_API process(Vst::ProcessData& data) SMTG_OVERRIDE {
        const auto ps = fx_->params();
        if (data.inputParameterChanges) {
            const int32 n = data.inputParameterChanges->getParameterCount();
            for (int32 i = 0; i < n; ++i) {
                Vst::IParamValueQueue* q = data.inputParameterChanges->getParameterData(i);
                if (!q) continue;
                const int32 points = q->getPointCount();
                Vst::ParamValue v = 0;
                int32 off = 0;
                if (points <= 0 || q->getPoint(points - 1, off, v) != kResultTrue) continue;
                const Vst::ParamID id = q->getParameterId();
                if (id == kBypassId) bypass_ = v >= 0.5;
                else if (id < ps.size()) fx_->set(static_cast<int>(id), fx::toPlain(ps[id], static_cast<float>(v)));
            }
        }
        if (data.processContext && (data.processContext->state & Vst::ProcessContext::kTempoValid))
            fx_->setTempo(data.processContext->tempo);   // tempo-synced effects (HEM ECHO)
        if (data.numSamples <= 0 || data.numInputs < 1 || data.numOutputs < 1 || maxBlock_ == 0) return kResultOk;
        Vst::AudioBusBuffers& in = data.inputs[0];
        Vst::AudioBusBuffers& out = data.outputs[0];
        if (!out.channelBuffers32 || out.numChannels < 2 || !in.channelBuffers32 || in.numChannels < 2) return kResultOk;
        float* oL = out.channelBuffers32[0];
        float* oR = out.channelBuffers32[1];
        const float* iL = in.channelBuffers32[0];
        const float* iR = in.channelBuffers32[1];
        const auto total = static_cast<std::uint32_t>(data.numSamples);
        if (oL != iL) std::memcpy(oL, iL, total * sizeof(float));
        if (oR != iR) std::memcpy(oR, iR, total * sizeof(float));

        const float target = bypass_ ? 0.0f : 1.0f;
        for (std::uint32_t done = 0; done < total;) {
            const std::uint32_t n = std::min(total - done, maxBlock_);
            float* L = oL + done;
            float* R = oR + done;
            // The dry, delayed by the effect's latency, for bypass (always run so it stays in time).
            for (std::uint32_t k = 0; k < n; ++k) {
                dry_[0][k] = bypassDelay_[0].process(L[k], fx_->latency());
                dry_[1][k] = bypassDelay_[1].process(R[k], fx_->latency());
            }
            fx_->process(L, R, n);
            if (mix_ != 1.0f || target != 1.0f) {
                for (std::uint32_t k = 0; k < n; ++k) {
                    mix_ = target > mix_ ? std::min(target, mix_ + fadeStep_) : std::max(target, mix_ - fadeStep_);
                    L[k] = mix_ * L[k] + (1.0f - mix_) * dry_[0][k];
                    R[k] = mix_ * R[k] + (1.0f - mix_) * dry_[1][k];
                }
            }
            done += n;
        }
        out.silenceFlags = 0;
        // Gain-reduction meter (0 dB .. -30 dB → 1 .. 0) for the host's generic editor.
        if (hasMeter(*info_) && data.outputParameterChanges) {
            int32 idx = 0;
            if (Vst::IParamValueQueue* q = data.outputParameterChanges->addParameterData(kMeterId, idx)) {
                int32 pi = 0;
                q->addPoint(0, std::clamp(1.0 + fx_->reductionDb() / 30.0, 0.0, 1.0), pi);
            }
        }
        if (data.outputParameterChanges) {   // the faceplate's meters
            const auto ms = fx_->meters();
            for (std::size_t m = 0; m < ms.size(); ++m) {
                int32 idx = 0;
                if (Vst::IParamValueQueue* q = data.outputParameterChanges->addParameterData(kMeterBase + static_cast<Vst::ParamID>(m), idx)) {
                    int32 pi = 0;
                    q->addPoint(0, meterToNorm(ms[m], fx_->meter(static_cast<int>(m))), pi);
                }
            }
        }
        return kResultOk;
    }

    tresult PLUGIN_API setState(IBStream* state) SMTG_OVERRIDE {
        bool b = bypass_;
        if (!readState(state, *fx_, b)) return kResultFalse;
        bypass_ = b;
        return kResultOk;
    }
    tresult PLUGIN_API getState(IBStream* state) SMTG_OVERRIDE { return writeText(state, stateText(*fx_, bypass_)) ? kResultOk : kResultFalse; }

private:
    const fx::EffectInfo*       info_;
    std::unique_ptr<fx::Effect> fx_;
    fx::DelayLine               bypassDelay_[2];
    std::vector<float>          dry_[2];
    double                      sampleRate_ = 48000.0;
    std::uint32_t               maxBlock_ = 0;
    bool                        bypass_ = false;
    float                       mix_ = 1.0f, fadeStep_ = 0.002f;
};

// ---- controller ----------------------------------------------------------------------
class FxParameter final : public Vst::Parameter {
public:
    FxParameter(const fx::ParamInfo& p, Vst::ParamID id)
        : Vst::Parameter(toU16(p.name).c_str(), id, unitFor(p), fx::toNormalized(p, p.def),
                         (p.taper == fx::Taper::Choice || p.taper == fx::Taper::Toggle) ? static_cast<int32>(p.max - p.min) : 0,
                         Vst::ParameterInfo::kCanAutomate | (p.taper == fx::Taper::Choice ? Vst::ParameterInfo::kIsList : 0)),
          p_(p) {}

    void toString(Vst::ParamValue norm, Vst::String128 out) const SMTG_OVERRIDE {
        const std::string s = fx::formatValue(p_, fx::toPlain(p_, static_cast<float>(norm)));
        const std::size_t n = std::min<std::size_t>(s.size(), 127);
        for (std::size_t i = 0; i < n; ++i) out[i] = static_cast<Vst::TChar>(static_cast<unsigned char>(s[i]));
        out[n] = 0;
    }
    bool fromString(const Vst::TChar* text, Vst::ParamValue& norm) const SMTG_OVERRIDE {
        std::string s;
        for (const Vst::TChar* t = text; t && *t && s.size() < 64; ++t) s.push_back(static_cast<char>(*t & 0x7F));
        float plain = 0;
        if (!fx::parseValue(p_, s, plain)) return false;
        norm = fx::toNormalized(p_, plain);
        return true;
    }
    Vst::ParamValue toPlain(Vst::ParamValue norm) const SMTG_OVERRIDE { return fx::toPlain(p_, static_cast<float>(norm)); }
    Vst::ParamValue toNormalized(Vst::ParamValue plain) const SMTG_OVERRIDE { return fx::toNormalized(p_, static_cast<float>(plain)); }

private:
    static const Vst::TChar* unitFor(const fx::ParamInfo& p) {
        const std::string u = p.unit ? p.unit : "";
        if (u == "dB" || u == "dBr") return STR16("dB");
        if (u == "Hz") return STR16("Hz");
        if (u == "ms") return STR16("ms");
        if (u == "s") return STR16("s");
        if (u == "%") return STR16("%");
        return STR16("");
    }
    const fx::ParamInfo& p_;
};

class Controller;

// The faceplate edits parameters the VST3 way: begin / perform / end edit through the controller.
class ControllerHost final : public ui::PanelHost {
public:
    ControllerHost(Vst::EditController& c, fx::Effect& e, bool gainMeter) : c_(c), e_(e), gainMeter_(gainMeter) {}
    float normalized(int i) const override { return static_cast<float>(c_.getParamNormalized(static_cast<Vst::ParamID>(i))); }
    void beginEdit(int i) override { c_.beginEdit(static_cast<Vst::ParamID>(i)); }
    void setNormalized(int i, float v) override {
        c_.setParamNormalized(static_cast<Vst::ParamID>(i), v);
        c_.performEdit(static_cast<Vst::ParamID>(i), v);
    }
    void endEdit(int i) override { c_.endEdit(static_cast<Vst::ParamID>(i)); }
    std::string text(int i) const override {
        const auto ps = e_.params();
        if (i < 0 || i >= static_cast<int>(ps.size())) return {};
        const auto& p = ps[static_cast<std::size_t>(i)];
        return fx::formatValue(p, fx::toPlain(p, normalized(i)));
    }
    float meter(int m) const override {
        const auto ms = e_.meters();
        if (m < static_cast<int>(ms.size())) return normToMeter(ms[static_cast<std::size_t>(m)], c_.getParamNormalized(kMeterBase + static_cast<Vst::ParamID>(m)));
        return gainMeter_ ? static_cast<float>((c_.getParamNormalized(kMeterId) - 1.0) * 30.0) : 0.0f;
    }
    // Whether a control is in use depends on the others (ECHO: TIME vs TEMPO SYNC): ask the controller's
    // own copy of the effect (it never processes audio) with the current values.
    bool active(int i) const override {
        const auto ps = e_.params();
        for (std::size_t j = 0; j < ps.size(); ++j) e_.set(static_cast<int>(j), fx::toPlain(ps[j], normalized(static_cast<int>(j))));
        return e_.paramActive(i);
    }
    bool bypassed() const override { return c_.getParamNormalized(kBypassId) >= 0.5; }
    void setBypassed(bool on) override {
        c_.beginEdit(kBypassId);
        c_.setParamNormalized(kBypassId, on ? 1.0 : 0.0);
        c_.performEdit(kBypassId, on ? 1.0 : 0.0);
        c_.endEdit(kBypassId);
    }

private:
    Vst::EditController& c_;
    fx::Effect&          e_;
    bool                 gainMeter_;
};

// The editor: an OpenGL child window inside the host's window.
class FxView final : public CPluginView {
public:
    FxView(const ui::PanelSpec& spec, ui::PanelHost& host) : spec_(spec), host_(host) {
        scale_ = ui::FxPanelWindow::defaultScale(spec_);
        int w = 0, h = 0;
        ui::FxPanelWindow::sizeFor(spec_, scale_, w, h);
        ViewRect r(0, 0, w, h);
        setRect(r);
    }
    tresult PLUGIN_API isPlatformTypeSupported(FIDString type) SMTG_OVERRIDE {
        return std::strcmp(type, kPlatformTypeHWND) == 0 ? kResultTrue : kResultFalse;
    }
    void attachedToParent() SMTG_OVERRIDE {
        std::string err;
        if (!window_.openChild(systemWindow, spec_, host_, bundleResources(), scale_, &err))
            ::OutputDebugStringA(("HEM Suite: faceplate failed: " + err + "\n").c_str());
    }
    void removedFromParent() SMTG_OVERRIDE { window_.close(); }

private:
    const ui::PanelSpec& spec_;
    ui::PanelHost&       host_;
    ui::FxPanelWindow    window_;
    float                scale_ = 0.62f;
};

class Controller final : public Vst::EditController {
public:
    explicit Controller(const fx::EffectInfo* info)
        : info_(info), fx_(info->create()), spec_(ui::panelSpecFor(*info, *fx_)), host_(*this, *fx_, hasMeter(*info)) {}
    static FUnknown* createInstance(void* ctx) {
        return static_cast<Vst::IEditController*>(new Controller(static_cast<const fx::EffectInfo*>(ctx)));
    }

    tresult PLUGIN_API initialize(FUnknown* context) SMTG_OVERRIDE {
        const tresult r = EditController::initialize(context);
        if (r != kResultOk) return r;
        const auto ps = fx_->params();
        for (std::size_t i = 0; i < ps.size(); ++i) parameters.addParameter(new FxParameter(ps[i], static_cast<Vst::ParamID>(i)));
        parameters.addParameter(STR16("Bypass"), nullptr, 1, 0, Vst::ParameterInfo::kCanAutomate | Vst::ParameterInfo::kIsBypass, kBypassId);
        if (hasMeter(*info_))
            parameters.addParameter(STR16("Gain Reduction"), STR16("dB"), 0, 1, Vst::ParameterInfo::kIsReadOnly, kMeterId);
        const auto ms = fx_->meters();
        for (std::size_t m = 0; m < ms.size(); ++m)
            parameters.addParameter(toU16(std::string("Meter ") + ms[m].name).c_str(), nullptr, 0, meterToNorm(ms[m], 0.0f),
                                    Vst::ParameterInfo::kIsReadOnly | Vst::ParameterInfo::kIsHidden, kMeterBase + static_cast<Vst::ParamID>(m));
        return kResultOk;
    }

    tresult PLUGIN_API setComponentState(IBStream* state) SMTG_OVERRIDE {
        bool bypass = false;
        if (!readState(state, *fx_, bypass)) return kResultFalse;
        const auto ps = fx_->params();
        for (std::size_t i = 0; i < ps.size(); ++i)
            setParamNormalized(static_cast<Vst::ParamID>(i), fx::toNormalized(ps[i], fx_->get(static_cast<int>(i))));
        setParamNormalized(kBypassId, bypass ? 1.0 : 0.0);
        return kResultOk;
    }

    IPlugView* PLUGIN_API createView(FIDString name) SMTG_OVERRIDE {
        if (name && std::strcmp(name, Vst::ViewType::kEditor) == 0) return new FxView(spec_, host_);
        return nullptr;
    }

private:
    const fx::EffectInfo*       info_;
    std::unique_ptr<fx::Effect> fx_;   // parameter descriptions and text (never processes audio)
    ui::PanelSpec               spec_;
    ControllerHost              host_;
};

} // namespace hc::hemfx

// ---- factory: two classes per effect, created with the effect's info as context ------------
SMTG_EXPORT_SYMBOL IPluginFactory* PLUGIN_API GetPluginFactory() {
    if (!gPluginFactory) {
        static PFactoryInfo factoryInfo("Hybrid Elite Mafia", "https://github.com/The-Hybrid-Elite-Mafia", "", PFactoryInfo::kUnicode);
        gPluginFactory = new CPluginFactory(factoryInfo);
        const auto list = hc::fx::effects();
        static std::vector<PClassInfo2> infos;   // must outlive the factory registration
        infos.reserve(list.size() * 2);
        for (const hc::fx::EffectInfo& e : list) {
            TUID pid, cid;
            hc::hemfx::toFuid(e.processorUid).toTUID(pid);
            hc::hemfx::toFuid(e.controllerUid).toTUID(cid);
            const std::string ctrlName = std::string(e.name) + " Controller";
            infos.emplace_back(pid, PClassInfo::kManyInstances, kVstAudioEffectClass, e.name, Vst::kDistributable, e.subCategory,
                               nullptr, HC_VERSION_STR, kVstVersionString);
            gPluginFactory->registerClass(&infos.back(), hc::hemfx::Processor::createInstance, const_cast<hc::fx::EffectInfo*>(&e));
            infos.emplace_back(cid, PClassInfo::kManyInstances, kVstComponentControllerClass, ctrlName.c_str(), 0, "", nullptr,
                               HC_VERSION_STR, kVstVersionString);
            gPluginFactory->registerClass(&infos.back(), hc::hemfx::Controller::createInstance, const_cast<hc::fx::EffectInfo*>(&e));
        }
    } else {
        gPluginFactory->addRef();
    }
    return gPluginFactory;
}
