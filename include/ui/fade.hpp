#pragma once

/* FADE IN
- Draws its child with every colour it set blended from the page's dark
  (p = 0: hidden, 1: as drawn). Cells left at the terminal's own colours
  are untouched, so a see-through terminal stays see-through
- Used for covers arriving, Now Playing opening and a page changing
*/

#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>

namespace ui {

class FadeIn : public ftxui::Node {
public:
    FadeIn(ftxui::Element child, double p) : Node(ftxui::Elements{std::move(child)}), p_(static_cast<float>(p)) {}
    void ComputeRequirement() override { children_[0]->ComputeRequirement(); requirement_ = children_[0]->requirement(); }
    void SetBox(ftxui::Box box) override { Node::SetBox(box); children_[0]->SetBox(box); }
    void Render(ftxui::Screen& screen) override {
        children_[0]->Render(screen);
        using ftxui::Color;
        const Color base = Color::RGB(24, 24, 28), none = Color::Default;
        for (int y = box_.y_min; y <= box_.y_max; ++y)
            for (int x = box_.x_min; x <= box_.x_max; ++x) {
                auto& c = screen.CellAt(x, y);
                if (!(c.background_color == none)) c.background_color = Color::Interpolate(p_, base, c.background_color);
                if (!(c.foreground_color == none)) c.foreground_color = Color::Interpolate(p_, base, c.foreground_color);
            }
    }
private:
    float p_;
};

// The child faded in by p (0..1); itself once p reaches 1.
inline ftxui::Element fadeIn(ftxui::Element e, double p) {
    return p >= 1.0 ? e : std::make_shared<FadeIn>(std::move(e), p);
}

}  // namespace ui
