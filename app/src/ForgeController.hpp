#pragma once
// =============================================================================
//  ForgeController — the message-thread brain of BEAT FORGE in the app:
//  GENERATE (every unlocked track), DICE (one track, same chords), UNDO (one step),
//  CLEAR, the song form, and saving / restoring the song with its composer seeds.
//  Songs reach the audio thread only through ForgeEngine::setSong() (lock-free).
// =============================================================================
#include "hybrid/forge/Composer.hpp"
#include "hybrid/forge/ForgeEngine.hpp"
#include "hybrid/params/ParameterTree.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <string>

namespace hc::app {

class ForgeController {
public:
    // `engine` and `tree` must outlive the controller; `tree` holds the forge.* parameters.
    ForgeController(params::ParameterTree& tree, forge::ForgeEngine& engine);

    void generate();              // new chords + every unlocked track; turns Beat Forge on
    void reroll(int track);       // a new part for one track over the same chords
    void undo();
    [[nodiscard]] bool canUndo() const noexcept { return undo_.has_value(); }
    void clear();                 // empty pattern (Beat Forge stays on, silent)

    [[nodiscard]] const forge::Song& song() const noexcept { return song_; }
    [[nodiscard]] const std::string& chords() const noexcept { return chords_; }
    [[nodiscard]] std::string form() const { return forge::Composer::describe(song_.arrangement); }

    // Sessions / settings.
    [[nodiscard]] std::string songText() const { return forge::songToText(song_); }
    [[nodiscard]] std::string seedsText() const;
    void restore(const std::string& songText, const std::string& seedsText);

    // Bumped on every change (UI redraws, autosave).
    [[nodiscard]] std::uint64_t version() const noexcept { return version_; }

private:
    struct Snapshot { forge::Song song; std::uint32_t harmony; std::array<std::uint32_t, forge::kTracks> seeds; std::string chords; };
    [[nodiscard]] forge::ComposeSettings settings() const;
    void compose(const forge::ComposeSettings& s, bool applySounds, bool applyTempo);
    void publish();
    void remember();
    std::uint32_t newSeed() { return static_cast<std::uint32_t>(rng_()) | 1u; }

    params::ParameterTree& tree_;
    forge::ForgeEngine&    engine_;
    forge::Song            song_;
    std::uint32_t          harmony_ = 1;
    std::array<std::uint32_t, forge::kTracks> seeds_{};
    std::string            chords_;
    std::optional<Snapshot> undo_;
    std::mt19937_64        rng_;
    std::uint64_t          version_ = 0;
};

} // namespace hc::app
