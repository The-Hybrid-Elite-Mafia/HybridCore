// =============================================================================
//  HEM WAVE — VST3 instrument build of the hc::synth wavetable synth.
//
//  One MIDI input (16 channels), one stereo output. Parameters: the synth's own list
//  (same ids, ranges and text), plus PRESET (a list: picking one loads that factory sound
//  and reports every changed value back to the host), and hidden PITCH BEND / MOD WHEEL
//  parameters that the host feeds from MIDI through IMidiMapping.
//  Editor: the HEM obsidian faceplate (hc::ui::FxPanelWindow) with OSC / FILTER / ENV / LFO / VOICE sections.
//  Audio thread: notes go into the synth's fixed event queue with their sample offsets;
//  parameter changes apply at the start of the block; no allocation, locks or waiting.
//  Steinberg VST3 SDK: MIT licence (verified from the SDK's LICENSE.txt, v3.8.1).
// =============================================================================
#include "hybrid/synth/Wave.hpp"
#include "hybrid/ui/FxPanel.hpp"
#include "hybrid/ui/FxPanelWindow.hpp"

#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstmidicontrollers.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/vsttypes.h"
#include "public.sdk/source/common/pluginview.h"
#include "public.sdk/source/main/pluginfactory.h"
#include "public.sdk/source/vst/vstaudioeffect.h"
#include "public.sdk/source/vst/vsteditcontroller.h"
#include "public.sdk/source/vst/vstparameters.h"

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

namespace hc::wave3 {

static const FUID kProcessorUID(0x6E8716DB, 0xAB3A48C1, 0x8E9F3FB2, 0x10425F18);
static const FUID kControllerUID(0x4033D500, 0x4BD749F6, 0x92C62785, 0x49388A3C);

constexpr Vst::ParamID kPresetId = 100, kBendId = 101, kWheelId = 102;
constexpr char         kStateMagic[] = "HEMW1\n";
constexpr std::uint32_t kMaxChunk = 4096;

std::u16string toU16(const std::string& s) { return std::u16string(s.begin(), s.end()); }   // ASCII names

// ---- state: "HEMW1\n" then "preset=<n>" and "<id>=<value>" lines ----------------------
bool writeState(IBStream* stream, const synth::WaveSynth& s, int preset) {
    if (!stream) return false;
    std::string text = kStateMagic;
    text += "preset=" + std::to_string(preset) + "\n";
    const auto ps = synth::WaveSynth::params();
    char b[64];
    for (std::size_t i = 0; i < ps.size(); ++i) {
        std::snprintf(b, sizeof b, "%.6g", static_cast<double>(s.get(static_cast<int>(i))));
        text += std::string(ps[i].id) + "=" + b + "\n";
    }
    int32 written = 0;
    return stream->write(text.data(), static_cast<int32>(text.size()), &written) == kResultOk && written == static_cast<int32>(text.size());
}

// Reads the state into `s` (values not in the text keep their defaults); returns false on junk.
bool readState(IBStream* stream, synth::WaveSynth& s, int& preset) {
    if (!stream) return false;
    std::string text;
    char buf[4096];
    for (;;) {
        int32 got = 0;
        if (stream->read(buf, sizeof(buf), &got) != kResultOk || got <= 0) break;
        text.append(buf, static_cast<std::size_t>(got));
        if (text.size() > (1u << 16)) return false;   // our state is ~600 bytes
    }
    if (text.compare(0, sizeof(kStateMagic) - 1, kStateMagic) != 0) return false;
    s.setDefaults();
    preset = 0;
    std::istringstream in(text.substr(sizeof(kStateMagic) - 1));
    std::string line;
    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        const float v = std::strtof(line.c_str() + eq + 1, nullptr);
        if (key == "preset") { preset = std::clamp(static_cast<int>(v), 0, synth::WaveSynth::presetCount() - 1); continue; }
        const int i = s.paramIndex(key);
        if (i >= 0) s.set(i, v);
    }
    return true;
}

// ---- processor -------------------------------------------------------------------
class Processor final : public Vst::AudioEffect {
public:
    Processor() { setControllerClass(kControllerUID); }

    static FUnknown* createInstance(void*) { return static_cast<Vst::IAudioProcessor*>(new Processor); }

    tresult PLUGIN_API initialize(FUnknown* context) SMTG_OVERRIDE {
        const tresult r = AudioEffect::initialize(context);
        if (r != kResultOk) return r;
        addEventInput(STR16("MIDI In"), 16);
        addAudioOutput(STR16("Stereo Out"), Vst::SpeakerArr::kStereo);
        return kResultOk;
    }

    tresult PLUGIN_API setBusArrangements(Vst::SpeakerArrangement* inputs, int32 numIns,
                                          Vst::SpeakerArrangement* outputs, int32 numOuts) SMTG_OVERRIDE {
        if (numIns != 0 || numOuts != 1 || outputs[0] != Vst::SpeakerArr::kStereo) return kResultFalse;
        return AudioEffect::setBusArrangements(inputs, numIns, outputs, numOuts);
    }

    tresult PLUGIN_API canProcessSampleSize(int32 symbolicSampleSize) SMTG_OVERRIDE {
        return symbolicSampleSize == Vst::kSample32 ? kResultTrue : kResultFalse;
    }

    tresult PLUGIN_API setupProcessing(Vst::ProcessSetup& setup) SMTG_OVERRIDE {
        maxBlock_ = static_cast<std::uint32_t>(std::clamp<int32>(setup.maxSamplesPerBlock, 32, static_cast<int32>(kMaxChunk)));
        synth_.prepare(setup.sampleRate, maxBlock_);
        return AudioEffect::setupProcessing(setup);
    }

    tresult PLUGIN_API setActive(TBool state) SMTG_OVERRIDE {
        if (!state) synth_.reset();
        return AudioEffect::setActive(state);
    }

    tresult PLUGIN_API process(Vst::ProcessData& data) SMTG_OVERRIDE {
        // 1) Parameters (last point of each queue, applied at the start of the block).
        if (data.inputParameterChanges) {
            const int32 n = data.inputParameterChanges->getParameterCount();
            for (int32 i = 0; i < n; ++i) {
                Vst::IParamValueQueue* q = data.inputParameterChanges->getParameterData(i);
                if (!q) continue;
                const int32 points = q->getPointCount();
                Vst::ParamValue value = 0;
                int32 offset = 0;
                if (points <= 0 || q->getPoint(points - 1, offset, value) != kResultTrue) continue;
                const Vst::ParamID id = q->getParameterId();
                const auto norm = static_cast<float>(value);
                if (id < static_cast<Vst::ParamID>(synth::kParamCount)) {
                    synth_.set(static_cast<int>(id), synth::toPlain(synth::WaveSynth::params()[id], norm));
                } else if (id == kBendId) {
                    synth_.pitchBend(norm * 2.0f - 1.0f);
                } else if (id == kWheelId) {
                    synth_.modWheel(norm);
                } else if (id == kPresetId) {
                    const int count = synth::WaveSynth::presetCount();
                    const int p = std::clamp(static_cast<int>(norm * static_cast<float>(count - 1) + 0.5f), 0, count - 1);
                    if (p != preset_) {
                        preset_ = p;
                        synth_.applyPreset(p);
                        reportAll(data);
                    }
                }
            }
        }
        // 2) Notes, with their sample offsets (the synth carries offsets past a chunk over).
        if (data.inputEvents) {
            const int32 n = data.inputEvents->getEventCount();
            for (int32 i = 0; i < n; ++i) {
                Vst::Event e{};
                if (data.inputEvents->getEvent(i, e) != kResultOk) continue;
                const auto at = static_cast<std::uint32_t>(std::max<int32>(0, e.sampleOffset));
                if (e.type == Vst::Event::kNoteOnEvent) {
                    if (e.noteOn.velocity > 0.0f) synth_.noteOn(e.noteOn.pitch, e.noteOn.velocity, at);
                    else synth_.noteOff(e.noteOn.pitch, at);
                } else if (e.type == Vst::Event::kNoteOffEvent) {
                    synth_.noteOff(e.noteOff.pitch, at);
                }
            }
        }
        if (data.numSamples <= 0 || data.numOutputs < 1 || maxBlock_ == 0) return kResultOk;   // parameter flush
        Vst::AudioBusBuffers& out = data.outputs[0];
        if (out.numChannels < 2 || !out.channelBuffers32 || !out.channelBuffers32[0] || !out.channelBuffers32[1]) return kResultOk;
        // 3) Audio, in chunks no larger than the prepared block.
        const auto total = static_cast<std::uint32_t>(data.numSamples);
        float* L = out.channelBuffers32[0];
        float* R = out.channelBuffers32[1];
        for (std::uint32_t done = 0; done < total;) {
            const std::uint32_t frames = std::min(total - done, maxBlock_);
            synth_.render(L + done, R + done, frames);
            done += frames;
        }
        bool silent = synth_.activeVoices() == 0;
        for (std::uint32_t i = 0; silent && i < total; ++i) silent = L[i] == 0.0f && R[i] == 0.0f;
        out.silenceFlags = silent ? 3 : 0;
        return kResultOk;
    }

    tresult PLUGIN_API setState(IBStream* state) SMTG_OVERRIDE {
        int p = 0;
        if (!readState(state, synth_, p)) return kResultFalse;
        preset_ = p;
        return kResultOk;
    }

    tresult PLUGIN_API getState(IBStream* state) SMTG_OVERRIDE { return writeState(state, synth_, preset_) ? kResultOk : kResultFalse; }

private:
    // After a preset change: every parameter's new value goes back to the host (and its controls).
    void reportAll(Vst::ProcessData& data) {
        if (!data.outputParameterChanges) return;
        const auto ps = synth::WaveSynth::params();
        for (std::size_t i = 0; i < ps.size(); ++i) {
            int32 index = 0;
            if (Vst::IParamValueQueue* q = data.outputParameterChanges->addParameterData(static_cast<Vst::ParamID>(i), index)) {
                int32 point = 0;
                q->addPoint(0, synth::toNormalized(ps[i], synth_.get(static_cast<int>(i))), point);
            }
        }
    }

    synth::WaveSynth synth_;
    std::uint32_t    maxBlock_ = 0;
    int              preset_ = 0;
};

// ---- controller ------------------------------------------------------------------
// A VST3 parameter that formats / parses exactly like the synth.
class WaveParameter final : public Vst::Parameter {
public:
    WaveParameter(const synth::ParamInfo& p, Vst::ParamID tag)
        : Vst::Parameter(toU16(p.name).c_str(), tag, unitFor(p), synth::toNormalized(p, p.def),
                         (p.taper == synth::Taper::Choice || p.taper == synth::Taper::Toggle) ? static_cast<int32>(p.max) : 0,
                         Vst::ParameterInfo::kCanAutomate | ((p.taper == synth::Taper::Choice) ? Vst::ParameterInfo::kIsList : 0)),
          info_(p) {}

    void toString(Vst::ParamValue norm, Vst::String128 out) const SMTG_OVERRIDE {
        const std::string s = synth::formatValue(info_, synth::toPlain(info_, static_cast<float>(norm)));
        const std::size_t n = std::min<std::size_t>(s.size(), 127);
        for (std::size_t i = 0; i < n; ++i) out[i] = static_cast<Vst::TChar>(static_cast<unsigned char>(s[i]));
        out[n] = 0;
    }
    bool fromString(const Vst::TChar* text, Vst::ParamValue& norm) const SMTG_OVERRIDE {
        std::string s;
        for (const Vst::TChar* t = text; t && *t && s.size() < 64; ++t) s.push_back(static_cast<char>(*t & 0x7F));
        float plain = 0.0f;
        if (!synth::parseValue(info_, s, plain)) return false;
        norm = synth::toNormalized(info_, plain);
        return true;
    }
    Vst::ParamValue toPlain(Vst::ParamValue norm) const SMTG_OVERRIDE { return synth::toPlain(info_, static_cast<float>(norm)); }
    Vst::ParamValue toNormalized(Vst::ParamValue plain) const SMTG_OVERRIDE { return synth::toNormalized(info_, static_cast<float>(plain)); }

private:
    static const Vst::TChar* unitFor(const synth::ParamInfo& p) {
        const std::string u = p.unit ? p.unit : "";
        if (u == "dB") return STR16("dB");
        if (u == "Hz") return STR16("Hz");
        if (u == "ms") return STR16("ms");
        if (u == "%") return STR16("%");
        if (u == "st") return STR16("st");
        if (u == "ct") return STR16("ct");
        return STR16("");
    }
    const synth::ParamInfo& info_;
};

// ---- faceplate --------------------------------------------------------------------------
std::filesystem::path bundleResources() {
    HMODULE m = nullptr;
    ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(&bundleResources), &m);
    wchar_t buf[MAX_PATH * 4];
    const DWORD n = ::GetModuleFileNameW(m, buf, static_cast<DWORD>(std::size(buf)));
    return std::filesystem::path(std::wstring(buf, n)).parent_path().parent_path() / "Resources";
}

// Panel parameter i: synth parameters 0..kParamCount-1, then the preset list.
ui::PanelSpec waveSpec() {
    ui::PanelSpec s;
    s.name = "HEM WAVE";
    s.blurb = "Morphing wavetable synth: 8 tables, unison, sub, filter, two envelopes, LFO";
    for (const synth::ParamInfo& p : synth::WaveSynth::params()) {
        ui::PanelParam pp;
        pp.name = p.name;
        if (p.taper == synth::Taper::Toggle) pp.kind = ui::PanelParam::Kind::Toggle;
        else if (p.taper == synth::Taper::Choice) {
            pp.kind = ui::PanelParam::Kind::Choice;
            for (int c = 0; c <= static_cast<int>(p.max); ++c) pp.choices.emplace_back(p.choices ? p.choices[c] : std::to_string(c));
        }
        pp.defNormalized = synth::toNormalized(p, p.def);
        pp.bipolar = pp.kind == ui::PanelParam::Kind::Knob && p.min < 0.0f && p.max > 0.0f && p.taper == synth::Taper::Linear;
        s.params.push_back(std::move(pp));
    }
    ui::PanelParam preset;
    preset.name = "Preset";
    preset.kind = ui::PanelParam::Kind::Choice;
    for (int i = 0; i < synth::WaveSynth::presetCount(); ++i) preset.choices.emplace_back(synth::WaveSynth::presetName(i));
    s.params.push_back(std::move(preset));
    using P = synth::Param;
    s.groups = {{"OSCILLATOR", P::Table, P::FilterType - P::Table},
                {"FILTER", P::FilterType, P::AmpAttack - P::FilterType},
                {"AMP ENVELOPE", P::AmpAttack, P::ModAttack - P::AmpAttack},
                {"MOD ENVELOPE", P::ModAttack, P::LfoShape - P::ModAttack},
                {"LFO", P::LfoShape, P::VelAmp - P::LfoShape},
                {"VOICE", P::VelAmp, P::kParamCount - P::VelAmp + 1}};   // + the preset list
    return s;
}

class WaveHost final : public ui::PanelHost {
public:
    explicit WaveHost(Vst::EditController& c) : c_(c) {}
    static Vst::ParamID idOf(int i) { return i < synth::kParamCount ? static_cast<Vst::ParamID>(i) : kPresetId; }
    float normalized(int i) const override { return static_cast<float>(c_.getParamNormalized(idOf(i))); }
    void beginEdit(int i) override { c_.beginEdit(idOf(i)); }
    void setNormalized(int i, float v) override {
        c_.setParamNormalized(idOf(i), v);
        c_.performEdit(idOf(i), v);
    }
    void endEdit(int i) override { c_.endEdit(idOf(i)); }
    std::string text(int i) const override {
        if (i >= synth::kParamCount) {
            const int count = synth::WaveSynth::presetCount();
            const int p = std::clamp(static_cast<int>(normalized(i) * static_cast<float>(count - 1) + 0.5f), 0, count - 1);
            return synth::WaveSynth::presetName(p);
        }
        const auto& p = synth::WaveSynth::params()[static_cast<std::size_t>(i)];
        return synth::formatValue(p, synth::toPlain(p, normalized(i)));
    }
    bool hasBypass() const override { return false; }

private:
    Vst::EditController& c_;
};

class WaveView final : public CPluginView {
public:
    WaveView(const ui::PanelSpec& spec, ui::PanelHost& host) : spec_(spec), host_(host) {
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
            ::OutputDebugStringA(("HEM WAVE: faceplate failed: " + err + "\n").c_str());
    }
    void removedFromParent() SMTG_OVERRIDE { window_.close(); }

private:
    const ui::PanelSpec& spec_;
    ui::PanelHost&       host_;
    ui::FxPanelWindow    window_;
    float                scale_ = 0.62f;
};

class Controller final : public Vst::EditController, public Vst::IMidiMapping {
public:
    Controller() : spec_(waveSpec()), host_(*this) {}
    static FUnknown* createInstance(void*) { return static_cast<Vst::IEditController*>(new Controller); }

    IPlugView* PLUGIN_API createView(FIDString name) SMTG_OVERRIDE {
        if (name && std::strcmp(name, Vst::ViewType::kEditor) == 0) return new WaveView(spec_, host_);
        return nullptr;
    }

    tresult PLUGIN_API initialize(FUnknown* context) SMTG_OVERRIDE {
        const tresult r = EditController::initialize(context);
        if (r != kResultOk) return r;
        const auto ps = synth::WaveSynth::params();
        for (std::size_t i = 0; i < ps.size(); ++i) parameters.addParameter(new WaveParameter(ps[i], static_cast<Vst::ParamID>(i)));
        auto* presets = new Vst::StringListParameter(STR16("Preset"), kPresetId, nullptr, Vst::ParameterInfo::kIsList);
        for (int p = 0; p < synth::WaveSynth::presetCount(); ++p) presets->appendString(toU16(synth::WaveSynth::presetName(p)).c_str());
        parameters.addParameter(presets);
        parameters.addParameter(STR16("Pitch Bend"), nullptr, 0, 0.5, Vst::ParameterInfo::kCanAutomate | Vst::ParameterInfo::kIsHidden, kBendId);
        parameters.addParameter(STR16("Mod Wheel"), nullptr, 0, 0.0, Vst::ParameterInfo::kCanAutomate | Vst::ParameterInfo::kIsHidden, kWheelId);
        return kResultOk;
    }

    tresult PLUGIN_API setComponentState(IBStream* state) SMTG_OVERRIDE {
        synth::WaveSynth s;   // only used as a value store (never prepared)
        int preset = 0;
        if (!readState(state, s, preset)) return kResultFalse;
        const auto ps = synth::WaveSynth::params();
        for (std::size_t i = 0; i < ps.size(); ++i)
            setParamNormalized(static_cast<Vst::ParamID>(i), synth::toNormalized(ps[i], s.get(static_cast<int>(i))));
        const int count = synth::WaveSynth::presetCount();
        setParamNormalized(kPresetId, count > 1 ? static_cast<double>(preset) / (count - 1) : 0.0);
        return kResultOk;
    }

    // MIDI pitch bend and mod wheel (any channel) drive the hidden parameters.
    tresult PLUGIN_API getMidiControllerAssignment(int32 busIndex, int16 /*channel*/, Vst::CtrlNumber number, Vst::ParamID& id) SMTG_OVERRIDE {
        if (busIndex != 0) return kResultFalse;
        if (number == Vst::kPitchBend) { id = kBendId; return kResultTrue; }
        if (number == Vst::kCtrlModWheel) { id = kWheelId; return kResultTrue; }
        return kResultFalse;
    }

    OBJ_METHODS(Controller, EditController)
private:
    ui::PanelSpec spec_;
    WaveHost      host_;
public:
    DEFINE_INTERFACES
        DEF_INTERFACE(IMidiMapping)
    END_DEFINE_INTERFACES(EditController)
    REFCOUNT_METHODS(EditController)
};

} // namespace hc::wave3

BEGIN_FACTORY("Hybrid Elite Mafia", "https://github.com/The-Hybrid-Elite-Mafia", "", PFactoryInfo::kUnicode)
    DEF_CLASS2(INLINE_UID_FROM_FUID(hc::wave3::kProcessorUID), PClassInfo::kManyInstances, kVstAudioEffectClass,
               "HEM WAVE", Vst::kDistributable, Vst::PlugType::kInstrumentSynth, HC_VERSION_STR, kVstVersionString,
               hc::wave3::Processor::createInstance)
    DEF_CLASS2(INLINE_UID_FROM_FUID(hc::wave3::kControllerUID), PClassInfo::kManyInstances, kVstComponentControllerClass,
               "HEM WAVE Controller", 0, "", HC_VERSION_STR, kVstVersionString,
               hc::wave3::Controller::createInstance)
END_FACTORY
