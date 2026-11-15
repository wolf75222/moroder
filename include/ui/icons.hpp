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

// The view with its Nerd Font glyphs swapped for plain ones when plainIcons().
ftxui::Element withIcons(ftxui::Element view);

}
