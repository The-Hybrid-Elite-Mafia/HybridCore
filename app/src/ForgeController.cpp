#include "ForgeController.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace hc::app {

using namespace hc::forge;

ForgeController::ForgeController(params::ParameterTree& tree, ForgeEngine& engine)
    : tree_(tree), engine_(engine),
      rng_(static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) ^ 0x48454D464F524745ull) {
    for (auto& s : seeds_) s = newSeed();
    harmony_ = newSeed();
}

ComposeSettings ForgeController::settings() const {
    ComposeSettings s;
    auto val = [this](const char* id) { const params::Parameter* p = tree_.find(id); return p ? p->plain() : 0.0f; };
    s.genre     = static_cast<int>(val(id::genre));
    s.key       = static_cast<int>(val(id::key));
    s.scale     = static_cast<int>(val(id::scale));
    s.bars      = kBarChoices[std::clamp(static_cast<int>(val(id::bars)), 0, 3)];
    s.density   = val(id::density) * 0.01f;
    s.variation = val(id::variation) * 0.01f;
    s.humanize  = val(id::humanize) * 0.01f;
    s.harmonySeed = harmony_;
    s.trackSeed   = seeds_;
    for (int t = 0; t < kTracks; ++t) {
        const params::Parameter* l = tree_.find(id::lock(t));
        s.locked[static_cast<std::size_t>(t)] = l && l->isOn() && song_.pattern.tracks[static_cast<std::size_t>(t)].count > 0;
    }
    return s;
}

void ForgeController::compose(const ComposeSettings& s, bool applySounds, bool applyTempo) {
    ComposeResult r;
    Composer::compose(s, song_.pattern, r);
    song_.pattern = r.pattern;
    song_.arrangement = Composer::arrange(s.genre, song_.pattern, s.harmonySeed, s.variation);
    chords_ = r.chordNames;
    if (applySounds)
        for (int t = bass; t < kTracks; ++t)
            if (!s.locked[static_cast<std::size_t>(t)])
                if (params::Parameter* p = tree_.find(id::type(t))) p->setPlain(static_cast<float>(r.soundType[static_cast<std::size_t>(t)]));
    if (applyTempo) {
        if (params::Parameter* t = tree_.find(id::setTempo); t && t->isOn()) {
            if (params::Parameter* b = tree_.find(id::bpm)) b->setPlain(static_cast<float>(r.bpm));
            if (params::Parameter* w = tree_.find(id::swing)) w->setPlain(static_cast<float>(Composer::genreSwing(s.genre)));
        }
    }
    publish();
}

void ForgeController::publish() {
    engine_.setSong(song_);
    ++version_;
}

void ForgeController::remember() { undo_ = Snapshot{song_, harmony_, seeds_, chords_}; }

void ForgeController::generate() {
    remember();
    harmony_ = newSeed();
    for (int t = 0; t < kTracks; ++t) {
        const params::Parameter* l = tree_.find(id::lock(t));
        if (!(l && l->isOn())) seeds_[static_cast<std::size_t>(t)] = newSeed();
    }
    compose(settings(), true, true);
    if (params::Parameter* on = tree_.find(id::on)) on->setPlain(1.0f);
}

void ForgeController::reroll(int track) {
    if (track < 0 || track >= kTracks) return;
    remember();
    seeds_[static_cast<std::size_t>(track)] = newSeed();
    ComposeSettings s = settings();
    for (int t = 0; t < kTracks; ++t)   // keep every other track exactly as it is
        s.locked[static_cast<std::size_t>(t)] = t != track && song_.pattern.tracks[static_cast<std::size_t>(t)].count > 0;
    s.locked[static_cast<std::size_t>(track)] = false;
    compose(s, false, false);
}

void ForgeController::undo() {
    if (!undo_) return;
    song_    = undo_->song;
    harmony_ = undo_->harmony;
    seeds_   = undo_->seeds;
    chords_  = undo_->chords;
    undo_.reset();
    publish();
}

void ForgeController::clear() {
    remember();
    song_ = Song{};
    chords_.clear();
    publish();
}

std::string ForgeController::seedsText() const {
    std::string out = std::to_string(harmony_);
    for (auto s : seeds_) out += "," + std::to_string(s);
    out += ";" + chords_;
    return out;
}

void ForgeController::restore(const std::string& text, const std::string& seeds) {
    Song s;
    if (!songFromText(text, s)) s = Song{};
    song_ = s;
    if (!seeds.empty()) {
        const auto semi = seeds.find(';');
        std::stringstream ss(seeds.substr(0, semi));
        std::string item;
        int i = -1;
        while (std::getline(ss, item, ',')) {
            const auto v = static_cast<std::uint32_t>(std::strtoul(item.c_str(), nullptr, 10));
            if (i < 0) harmony_ = v ? v : harmony_;
            else if (i < kTracks && v) seeds_[static_cast<std::size_t>(i)] = v;
            ++i;
        }
        chords_ = semi == std::string::npos ? std::string() : seeds.substr(semi + 1);
    }
    undo_.reset();
    publish();
}

} // namespace hc::app
