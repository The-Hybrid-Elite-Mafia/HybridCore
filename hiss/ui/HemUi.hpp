#pragma once
// HEM front-panel drawing kit (Windows, GDI+). Draws the metal plate, glass windows, screws, knobs, buttons and legends.
// Everything is drawn by code. There are no picture files and no web pages.
#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>
using std::min;     // the GDI+ headers use plain min / max
using std::max;
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include "HemControls.hpp"

namespace hem::ui {

bool startup();
void shutdown();

[[nodiscard]] std::unique_ptr<Gdiplus::Bitmap> makePlate(int w, int h, unsigned seed);   // brushed dark titanium
void drawPlateDressing(Gdiplus::Graphics& g, int w, int h);                               // rack ears, slots, screws, edge lines
[[nodiscard]] Gdiplus::RectF drawWindowFrame(Gdiplus::Graphics& g, Gdiplus::RectF outer, bool withScrews, float bezel = 15.0f);   // returns the glass area
void drawGlassSheen(Gdiplus::Graphics& g, Gdiplus::RectF glass);                          // reflection + edge shadow, drawn AFTER the contents
void drawKnob(Gdiplus::Graphics& g, const Knob& k, const std::wstring& readout);
void drawButton(Gdiplus::Graphics& g, const Button& b);

enum class Face { Legend, Mono, MonoBold, Title };
void text(Gdiplus::Graphics& g, const std::wstring& s, Gdiplus::RectF box, Face face, float px, Gdiplus::Color c,
          Gdiplus::StringAlignment h = Gdiplus::StringAlignmentNear, Gdiplus::StringAlignment v = Gdiplus::StringAlignmentNear);
void engraved(Gdiplus::Graphics& g, const std::wstring& s, float x, float y, float px, Gdiplus::StringAlignment h = Gdiplus::StringAlignmentNear, bool title = false);

[[nodiscard]] bool savePng(Gdiplus::Bitmap& bmp, const std::wstring& path);

inline Gdiplus::Color rgba(int r, int g, int b, int a = 255) { return Gdiplus::Color(static_cast<BYTE>(a), static_cast<BYTE>(r), static_cast<BYTE>(g), static_cast<BYTE>(b)); }

} // namespace hem::ui
