// =============================================================================
//  The Hybrid Core — VST3 build of the 13-stem matrix (optional, HC_BUILD_VST3).
//
//  Buses: 13 stereo inputs (Stem 1 = main input, Stems 2..13 = aux/side-chain inputs)
//         4 stereo outputs (Main + Aux 1..3)
//  Parameters: the same 125 mixer parameters as the standalone app (same ids, ranges,
//  tapers and text formatting). No custom editor: hosts show their generic controls.
//  Audio thread: parameter changes go straight into the matrix through MixerBindings
//  (allocation-free); the matrix processes any sample rate (it only applies gains).
//  Steinberg VST3 SDK: MIT licence (verified from the SDK's LICENSE.txt, v3.8.1).
// =============================================================================
#include "hybrid/dsp/StemMatrix.hpp"
#include "hybrid/params/MixerParams.hpp"
#include "hybrid/params/ParameterTree.hpp"

#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/vsttypes.h"
#include "public.sdk/source/main/pluginfactory.h"
#include "public.sdk/source/vst/vstaudioeffect.h"
#include "public.sdk/source/vst/vsteditcontroller.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#ifndef HC_VERSION_STR
#define HC_VERSION_STR "0.0.0"
#endif

using namespace Steinberg;

namespace hc::vst3 {

static const FUID kProcessorUID(0xE4133E8E, 0x65404158, 0x9D8117A1, 0x6928E0A2);
static const FUID kControllerUID(0xA5852258, 0xFAEF4846, 0xA61EED2D, 0x265BCB67);

constexpr std::uint32_t kStems = dsp::StemMatrix::kStems;
constexpr std::uint32_t kBuses = dsp::StemMatrix::kBuses;
constexpr char          kStateMagic[] = "HCV3\n";

std::u16string toU16(const std::string& s) { return std::u16string(s.begin(), s.end()); }   // ASCII ids/names

// ---- state helpers ---------------------------------------------------------------
bool writeState(IBStream* stream, const params::ParameterTree& tree) {
    if (!stream) return false;
    const std::string text = std::string(kStateMagic) + tree.serialize();
    int32 written = 0;
    return stream->write(const_cast<char*>(text.data()), static_cast<int32>(text.size()), &written) == kResultOk &&
           written == static_cast<int32>(text.size());
}

bool readState(IBStream* stream, params::ParameterTree& tree) {
    if (!stream) return false;
    std::string text;
    char buf[4096];
    for (;;) {
        int32 got = 0;
        if (stream->read(buf, sizeof(buf), &got) != kResultOk || got <= 0) break;
        text.append(buf, static_cast<std::size_t>(got));
        if (text.size() > (1u << 20)) return false;   // our state is ~3 KB; refuse junk
    }
    if (text.compare(0, sizeof(kStateMagic) - 1, kStateMagic) != 0) return false;
    tree.deserialize(std::string_view(text).substr(sizeof(kStateMagic) - 1));
    return true;
}

// ---- processor -------------------------------------------------------------------
class Processor final : public Vst::AudioEffect {
public:
    Processor() {
        setControllerClass(kControllerUID);
        params::addMixerParameters(tree_);
        bindings_ = std::make_unique<params::MixerBindings>(tree_);
    }

    static FUnknown* createInstance(void*) { return static_cast<Vst::IAudioProcessor*>(new Processor); }

    tresult PLUGIN_API initialize(FUnknown* context) SMTG_OVERRIDE {
        const tresult r = AudioEffect::initialize(context);
        if (r != kResultOk) return r;
        static const Vst::TChar* inNames[kStems] = {
            STR16("Stem 1"), STR16("Stem 2"), STR16("Stem 3"), STR16("Stem 4"), STR16("Stem 5"), STR16("Stem 6"), STR16("Stem 7"),
            STR16("Stem 8"), STR16("Stem 9"), STR16("Stem 10"), STR16("Stem 11"), STR16("Stem 12"), STR16("Stem 13")};
        static const Vst::TChar* outNames[kBuses] = {STR16("Main"), STR16("Aux 1"), STR16("Aux 2"), STR16("Aux 3")};
        addAudioInput(inNames[0], Vst::SpeakerArr::kStereo);
        for (std::uint32_t s = 1; s < kStems; ++s) addAudioInput(inNames[s], Vst::SpeakerArr::kStereo, Vst::BusTypes::kAux, 0);
        addAudioOutput(outNames[0], Vst::SpeakerArr::kStereo);
        for (std::uint32_t b = 1; b < kBuses; ++b) addAudioOutput(outNames[b], Vst::SpeakerArr::kStereo, Vst::BusTypes::kAux, 0);
        bindings_->applyAll(tree_, matrix_);
        return kResultOk;
    }

    tresult PLUGIN_API setBusArrangements(Vst::SpeakerArrangement* inputs, int32 numIns,
                                          Vst::SpeakerArrangement* outputs, int32 numOuts) SMTG_OVERRIDE {
        // Stereo everywhere; anything else is refused so the host keeps stereo.
        if (numIns != static_cast<int32>(kStems) || numOuts != static_cast<int32>(kBuses)) return kResultFalse;
        for (int32 i = 0; i < numIns; ++i) if (inputs[i] != Vst::SpeakerArr::kStereo) return kResultFalse;
        for (int32 i = 0; i < numOuts; ++i) if (outputs[i] != Vst::SpeakerArr::kStereo) return kResultFalse;
        return AudioEffect::setBusArrangements(inputs, numIns, outputs, numOuts);
    }

    tresult PLUGIN_API canProcessSampleSize(int32 symbolicSampleSize) SMTG_OVERRIDE {
        return symbolicSampleSize == Vst::kSample32 ? kResultTrue : kResultFalse;
    }

    tresult PLUGIN_API setupProcessing(Vst::ProcessSetup& setup) SMTG_OVERRIDE {
        const auto maxFrames = static_cast<std::uint32_t>(std::clamp<int32>(setup.maxSamplesPerBlock, 32, 8192));
        matrix_.prepare(maxFrames);
        bindings_->applyAll(tree_, matrix_);
        return AudioEffect::setupProcessing(setup);
    }

    tresult PLUGIN_API process(Vst::ProcessData& data) SMTG_OVERRIDE {
        // 1) Parameter changes (last point per queue; the matrix ramps every change per block).
        if (data.inputParameterChanges) {
            const int32 n = data.inputParameterChanges->getParameterCount();
            for (int32 i = 0; i < n; ++i) {
                Vst::IParamValueQueue* q = data.inputParameterChanges->getParameterData(i);
                if (!q) continue;
                const Vst::ParamID id = q->getParameterId();
                const int32 points = q->getPointCount();
                Vst::ParamValue value = 0;
                int32 offset = 0;
                if (points > 0 && id < tree_.size() && q->getPoint(points - 1, offset, value) == kResultTrue) {
                    params::Parameter& p = tree_.at(id);
                    p.setNormalized(static_cast<float>(value));
                    bindings_->apply(p, matrix_);
                }
            }
        }
        if (data.numSamples <= 0 || matrix_.maxFrames() == 0) return kResultOk;   // parameter flush

        // 2) Audio, in chunks no larger than the prepared block.
        const auto total = static_cast<std::uint32_t>(data.numSamples);
        for (std::uint32_t done = 0; done < total;) {
            const std::uint32_t frames = std::min(total - done, matrix_.maxFrames());
            std::array<const float*, kStems> L{}, R{};
            for (std::uint32_t s = 0; s < kStems && s < static_cast<std::uint32_t>(data.numInputs); ++s) {
                const Vst::AudioBusBuffers& in = data.inputs[s];
                if (in.numChannels >= 2 && in.channelBuffers32 && in.channelBuffers32[0] && in.channelBuffers32[1]) {
                    L[s] = in.channelBuffers32[0] + done;
                    R[s] = in.channelBuffers32[1] + done;
                }
            }
            matrix_.process(L.data(), R.data(), frames);
            for (std::uint32_t b = 0; b < kBuses && b < static_cast<std::uint32_t>(data.numOutputs); ++b) {
                Vst::AudioBusBuffers& out = data.outputs[b];
                if (!out.channelBuffers32) continue;
                for (int32 c = 0; c < out.numChannels && c < 2; ++c)
                    if (out.channelBuffers32[c])
                        std::memcpy(out.channelBuffers32[c] + done, matrix_.busChannel(b, static_cast<std::uint32_t>(c)),
                                    frames * sizeof(float));
                out.silenceFlags = 0;
            }
            done += frames;
        }
        return kResultOk;
    }

    tresult PLUGIN_API setState(IBStream* state) SMTG_OVERRIDE {
        if (!readState(state, tree_)) return kResultFalse;
        bindings_->applyAll(tree_, matrix_);
        return kResultOk;
    }

    tresult PLUGIN_API getState(IBStream* state) SMTG_OVERRIDE { return writeState(state, tree_) ? kResultOk : kResultFalse; }

private:
    params::ParameterTree                  tree_;
    std::unique_ptr<params::MixerBindings> bindings_;
    dsp::StemMatrix                        matrix_;
};

// ---- controller ------------------------------------------------------------------
// A VST3 parameter that formats/parses exactly like the standalone app.
class HcParameter final : public Vst::Parameter {
public:
    explicit HcParameter(const params::Parameter& p)
        : Vst::Parameter(toU16(p.spec().name).c_str(), p.index(), unitFor(p), p.toNormalized(p.spec().def),
                         p.spec().kind == params::Kind::Toggle ? 1 : 0, Vst::ParameterInfo::kCanAutomate),
          param_(p) {}

    void toString(Vst::ParamValue norm, Vst::String128 out) const SMTG_OVERRIDE {
        const std::string s = param_.formatValue(param_.toPlain(static_cast<float>(norm)));
        const std::size_t n = std::min<std::size_t>(s.size(), 127);
        for (std::size_t i = 0; i < n; ++i) out[i] = static_cast<Vst::TChar>(static_cast<unsigned char>(s[i]));
        out[n] = 0;
    }

    bool fromString(const Vst::TChar* text, Vst::ParamValue& norm) const SMTG_OVERRIDE {
        std::string s;
        for (const Vst::TChar* t = text; t && *t && s.size() < 64; ++t) s.push_back(static_cast<char>(*t & 0x7F));
        float plain = 0.0f;
        const auto& sp = param_.spec();
        if (sp.kind == params::Kind::Toggle) {
            plain = (s == "ON" || s == "on" || s == "1") ? 1.0f : 0.0f;
        } else if (sp.kind == params::Kind::Pan) {
            if (s.empty()) return false;
            if (s[0] == 'C' || s[0] == 'c') plain = 0.0f;
            else if (s[0] == 'L' || s[0] == 'l') plain = -std::strtof(s.c_str() + 1, nullptr) / 100.0f;
            else if (s[0] == 'R' || s[0] == 'r') plain = std::strtof(s.c_str() + 1, nullptr) / 100.0f;
            else plain = std::strtof(s.c_str(), nullptr);
        } else if (s.find("inf") != std::string::npos) {
            plain = sp.min;
        } else {
            char* end = nullptr;
            plain = std::strtof(s.c_str(), &end);
            if (end == s.c_str()) return false;
        }
        norm = param_.toNormalized(plain);
        return true;
    }

    Vst::ParamValue toPlain(Vst::ParamValue norm) const SMTG_OVERRIDE { return param_.toPlain(static_cast<float>(norm)); }
    Vst::ParamValue toNormalized(Vst::ParamValue plain) const SMTG_OVERRIDE { return param_.toNormalized(static_cast<float>(plain)); }

private:
    static const Vst::TChar* unitFor(const params::Parameter& p) {
        return p.spec().kind == params::Kind::Decibel ? STR16("dB") : STR16("");
    }
    const params::Parameter& param_;
};

class Controller final : public Vst::EditController {
public:
    static FUnknown* createInstance(void*) { return static_cast<Vst::IEditController*>(new Controller); }

    tresult PLUGIN_API initialize(FUnknown* context) SMTG_OVERRIDE {
        const tresult r = EditController::initialize(context);
        if (r != kResultOk) return r;
        params::addMixerParameters(tree_);
        for (std::size_t i = 0; i < tree_.size(); ++i) parameters.addParameter(new HcParameter(tree_.at(i)));
        return kResultOk;
    }

    tresult PLUGIN_API setComponentState(IBStream* state) SMTG_OVERRIDE {
        if (!readState(state, tree_)) return kResultFalse;
        for (std::size_t i = 0; i < tree_.size(); ++i)
            setParamNormalized(static_cast<Vst::ParamID>(i), tree_.at(i).normalized());
        return kResultOk;
    }

private:
    params::ParameterTree tree_;
};

} // namespace hc::vst3

BEGIN_FACTORY("Hybrid Elite Mafia", "https://github.com/The-Hybrid-Elite-Mafia", "", PFactoryInfo::kUnicode)
    DEF_CLASS2(INLINE_UID_FROM_FUID(hc::vst3::kProcessorUID), PClassInfo::kManyInstances, kVstAudioEffectClass,
               "The Hybrid Core", Vst::kDistributable, Vst::PlugType::kFxTools, HC_VERSION_STR, kVstVersionString,
               hc::vst3::Processor::createInstance)
    DEF_CLASS2(INLINE_UID_FROM_FUID(hc::vst3::kControllerUID), PClassInfo::kManyInstances, kVstComponentControllerClass,
               "The Hybrid Core Controller", 0, "", HC_VERSION_STR, kVstVersionString,
               hc::vst3::Controller::createInstance)
END_FACTORY
