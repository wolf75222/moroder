#pragma once

/* A line of text that ends in "…" when it doesn't fit
- text() is clipped at the edge of its box: in a narrow window a title just
  stopped mid-word ("Sound of Distant T", "10 trac"), which reads like a bug.
  This asks for the text's width but gives way to the row around it, and
  when it is given less it shows what fits and an ellipsis.
*/

#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/string.hpp>

#include <string>
#include <vector>

namespace ui {

class FitText : public ftxui::Node {
public:
    explicit FitText(std::string s) : text_(std::move(s)), glyphs_(ftxui::Utf8ToGlyphs(text_)) {}

    void ComputeRequirement() override {
        requirement_ = ftxui::Requirement{};
        requirement_.min_x = static_cast<int>(glyphs_.size());
        requirement_.min_y = 1;
        requirement_.flex_shrink_x = 1;
    }

    void Render(ftxui::Screen& screen) override {
        const int room = box_.x_max - box_.x_min + 1;
        if (room <= 0 || box_.y_min > box_.y_max) return;
        const int n = static_cast<int>(glyphs_.size());
        const bool cut = n > room;
        // Cut: what fits less one column, then the ellipsis. Never half of a
        // double-width character (its second cell is an empty glyph).
        int shown = cut ? room - 1 : n;
        while (cut && shown > 0 && shown < n && glyphs_[static_cast<size_t>(shown)].empty()) --shown;
        // Trailing spaces before the ellipsis look like a gap: drop them.
        while (cut && shown > 0 && glyphs_[static_cast<size_t>(shown - 1)] == " ") --shown;
        int x = box_.x_min;
        for (int i = 0; i < shown; ++i, ++x) screen.CellAt(x, box_.y_min).character = glyphs_[static_cast<size_t>(i)];
        if (cut) screen.CellAt(x, box_.y_min).character = "…";
    }

private:
    std::string text_;
    std::vector<std::string> glyphs_;
};

inline ftxui::Element fitText(std::string s) { return std::make_shared<FitText>(std::move(s)); }

}  // namespace ui
