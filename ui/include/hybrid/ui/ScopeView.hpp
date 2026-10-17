#pragma once
// =============================================================================
//  ScopeView — the ANALYZER page, drawn over the stem strips:
//    left : a 3D spectrum landscape (the newest audio in front, about 2.7 s going back;
//           low notes left, highs right; height and colour = level), projected in
//           perspective on the CPU and drawn as shaded, lit GPU quads, with a glowing
//           front ridge, floor grid and back wall. Drag it to turn it.
//    right: loudness (LUFS), peak, tone balance, stereo, and the plain-words checks.
//  Reads the analysis::Analyzer only (UI thread); never touches audio state.
// =============================================================================
#include "hybrid/ui/Geometry.hpp"
#include "hybrid/ui/MixerView.hpp"
#include "hybrid/ui/Renderer.hpp"

namespace hc::ui {

void drawScope(Renderer& r, Rect area, const ViewState& view);

// Where a drag in the 3D view goes: the left part of the scope area.
[[nodiscard]] Rect scopeViewArea(Rect area) noexcept;

} // namespace hc::ui
