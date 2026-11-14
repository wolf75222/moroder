#pragma once

#include <ftxui/dom/elements.hpp>
#include <vector>

namespace ui {

/* Viewport
- A vertical scroll area that moves only as much as it has to: the selection
  stays where it is on screen unless it would leave the view (FTXUI's frame
  re-centres the selection on every move, so a click on a row near the edge
  scrolled the page under the pointer, and a double-click played the row that
  slid under it)
- Scrolls by lines for the wheel / trackpad, independently of the selection
- Children are a window of the page (indices[i] is the page index of
  children[i]); the scroll position is kept as page index + line, so it stays
  put when the window moves
*/
struct ViewportState {
    int anchor = 0;             // page index of the child at the top
    int line = 0;               // lines of it scrolled past
    int pending = 0;            // wheel lines to apply on the next layout
    bool follow = true;         // keep the selection in view (false after a wheel scroll)

    // Filled by the last layout.
    int height = 0;
    int first_visible = -1;     // page indices of children fully in view
    int last_visible = -1;
    bool at_top = true, at_bottom = true;
};

ftxui::Element viewport(ftxui::Elements children, std::vector<int> indices, int active, ViewportState* state);

}
