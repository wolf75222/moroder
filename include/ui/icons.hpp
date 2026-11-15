/* ICONS
- Moroder's icons are Nerd Font glyphs (Unicode private use area). A font
  without them shows "�" (Windows Terminal's default, Cascadia, for one)
- ICONS = "auto" (plain on Windows, Nerd Font elsewhere), "nerd" or "plain":
  plain swaps every such glyph for a symbol any font has (♪ ► ♥ ‹ ›...),
  once per frame, at the root of the view, so no call site has to know
*/
#pragma once

#include <ftxui/dom/elements.hpp>

namespace ui {

bool plainIcons();

/* The Windows console window (conhost), not a terminal app: its fonts lack
most symbols and partial blocks and it has no font fallback, so they are
swapped for characters its fonts have, and covers are drawn with half blocks.
MORODER_GLYPHS=console / full decides instead.
*/
bool consoleGlyphs();

// The view with its Nerd Font glyphs swapped for plain ones when plainIcons(),
// and, in the console window, the characters its fonts lack (consoleGlyphs()).
ftxui::Element withIcons(ftxui::Element view);

}
