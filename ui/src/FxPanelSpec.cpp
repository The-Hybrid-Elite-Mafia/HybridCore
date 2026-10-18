#include "hybrid/fx/Effects.hpp"
#include "hybrid/ui/FxPanel.hpp"

#include <algorithm>

namespace hc::ui {

PanelSpec panelSpecFor(const fx::EffectInfo& info, const fx::Effect& e) {
    PanelSpec s;
    s.name = info.name;
    s.blurb = info.blurb;
    for (const fx::ParamInfo& p : e.params()) {
        PanelParam pp;
        pp.name = p.name;
        if (p.taper == fx::Taper::Toggle) pp.kind = PanelParam::Kind::Toggle;
        else if (p.taper == fx::Taper::Choice) {
            pp.kind = PanelParam::Kind::Choice;
            for (int c = 0; c <= static_cast<int>(p.max); ++c) pp.choices.emplace_back(p.choices[c]);
        }
        pp.defNormalized = fx::toNormalized(p, p.def);
        pp.bipolar = pp.kind == PanelParam::Kind::Knob && p.min < 0.0f && p.max > 0.0f && p.taper == fx::Taper::Linear;
        s.params.push_back(std::move(pp));
    }
    for (const fx::MeterInfo& m : e.meters()) s.meters.push_back({m.name, m.unit, m.min, m.max, m.centred});
    // Dynamics / restoration effects without their own meters show the gain reduction.
    const std::string cat = info.subCategory;
    if (s.meters.empty() && (cat.find("Dynamics") != std::string::npos || cat.find("Mastering") != std::string::npos ||
                             cat.find("Restoration") != std::string::npos || s.name == "HEM CLIP"))
        s.meters.push_back({"GAIN RED.", "dB", -24.0f, 0.0f, false});
    return s;
}

} // namespace hc::ui

namespace hc::ui {

float EffectPanelHost::normalized(int i) const {
    const auto ps = fx_.params();
    if (i < 0 || i >= static_cast<int>(ps.size())) return 0.0f;
    return fx::toNormalized(ps[static_cast<std::size_t>(i)], fx_.get(i));
}
void EffectPanelHost::setNormalized(int i, float v) {
    const auto ps = fx_.params();
    if (i < 0 || i >= static_cast<int>(ps.size())) return;
    fx_.set(i, fx::toPlain(ps[static_cast<std::size_t>(i)], std::clamp(v, 0.0f, 1.0f)));
}
std::string EffectPanelHost::text(int i) const {
    const auto ps = fx_.params();
    if (i < 0 || i >= static_cast<int>(ps.size())) return {};
    return fx::formatValue(ps[static_cast<std::size_t>(i)], fx_.get(i));
}
float EffectPanelHost::meter(int m) const {
    return m < static_cast<int>(fx_.meters().size()) ? fx_.meter(m) : fx_.reductionDb();
}

} // namespace hc::ui

namespace hc::ui {
bool EffectPanelHost::active(int param) const { return fx_.paramActive(param); }
} // namespace hc::ui
