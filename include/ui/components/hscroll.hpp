/* Sideways scrolling of a row (carousel, grid) by columns, for the mouse.
- Keys move the selection and the row follows it, as FTXUI's xframe does
- A trackpad swipe / Shift+wheel slides the row itself, a few columns per
  event, like a web page, leaving the selection where it is
- The next key goes back to following the selection
*/
#pragma once

#include <algorithm>

#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/box.hpp>

namespace ui {

struct HScroll {
    int offset = -1;            // columns scrolled; -1: follow the selection
    ftxui::Box frame, inner;    // where the row was drawn, and its full width
};

// Where its child is laid out, unclipped (reflect() keeps only the visible part).
class LaidOutBox : public ftxui::Node {
public:
    LaidOutBox(ftxui::Element child, ftxui::Box* out) : ftxui::Node({std::move(child)}), out_(out) {}
    void ComputeRequirement() override {
        ftxui::Node::ComputeRequirement();
        requirement_ = children_[0]->requirement();
    }
    void SetBox(ftxui::Box box) override {
        ftxui::Node::SetBox(box);
        *out_ = box;
        children_[0]->SetBox(box);
    }
private:
    ftxui::Box* out_;
};

inline ftxui::Element hscrolled(ftxui::Element row, HScroll* h) {
    using namespace ftxui;
    if (h->offset >= 0) {
        // xframe centres the focus: a focus point half a screen in shows `offset` first.
        const int visible = h->frame.x_max - h->frame.x_min;
        row = std::move(row) | focusPosition(h->offset + visible / 2, 0);
    }
    return std::make_shared<LaidOutBox>(std::move(row), &h->inner) | xframe | reflect(h->frame);
}

// Slide by delta columns (negative: back). False when the row fits or is at that end.
inline bool hscrollBy(HScroll* h, int delta) {
    const int visible = h->frame.x_max - h->frame.x_min;
    const int width = h->inner.x_max - h->inner.x_min;
    const int max_offset = std::max(0, width - visible - 1);
    if (max_offset == 0) return false;
    const int from = h->offset >= 0 ? h->offset : std::clamp(h->frame.x_min - h->inner.x_min, 0, max_offset);
    const int to = std::clamp(from + delta, 0, max_offset);
    h->offset = to;
    return to != from;
}

}
