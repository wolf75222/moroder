#pragma once

/* A pop-up that takes on what is under it
- Each cell under the pop-up keeps the colour that was there, darkened, and
  loses its character: over a cover you see the cover, dimmed; over an empty
  page you see the terminal's own background (see-through if the terminal is).
- A solid panel looked like a grey box on a transparent terminal; nothing at
  all let the page's text show through. This sits between the two.
*/

#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/string.hpp>

namespace ui {

class SeeThrough : public ftxui::Node {
public:
    explicit SeeThrough(ftxui::Element child) : Node(ftxui::Elements{std::move(child)}) {}

    void ComputeRequirement() override {
        children_[0]->ComputeRequirement();
        requirement_ = children_[0]->requirement();
    }

    void SetBox(ftxui::Box box) override {
        Node::SetBox(box);
        children_[0]->SetBox(box);
    }

    void Render(ftxui::Screen& screen) override {
        using ftxui::Color;
        const Color none = Color::Default, black = Color::RGB(0, 0, 0);
        // A double-width character cut by an edge: its other half goes too,
        // or the rest of the row shifts by a column.
        for (int y = box_.y_min; y <= box_.y_max; ++y) {
            if (box_.x_min > 0 && screen.CellAt(box_.x_min, y).character.empty()
                && ftxui::string_width(screen.CellAt(box_.x_min - 1, y).character) == 2) screen.CellAt(box_.x_min - 1, y).character = " ";
            if (box_.x_max + 1 < screen.dimx() && screen.CellAt(box_.x_max + 1, y).character.empty()) screen.CellAt(box_.x_max + 1, y).character = " ";
        }
        for (int y = box_.y_min; y <= box_.y_max; ++y) {
            for (int x = box_.x_min; x <= box_.x_max; ++x) {
                ftxui::Cell& c = screen.CellAt(x, y);
                Color under = c.background_color;
                // A cover is drawn with block glyphs: their ink is half the picture.
                if (block(c.character) && !(c.foreground_color == none))
                    under = under == none ? c.foreground_color : Color::Interpolate(0.5f, c.foreground_color, under);
                c = ftxui::Cell();
                c.character = " ";
                if (!(under == none)) c.background_color = Color::Interpolate(0.72f, under, black);
            }
        }
        children_[0]->Render(screen);
    }

private:
    // U+2580..U+259F (block elements) and the box / legacy shapes covers use.
    static bool block(const std::string& s) {
        if (s.size() != 3) return false;
        const auto a = static_cast<unsigned char>(s[0]), b = static_cast<unsigned char>(s[1]);
        return a == 0xE2 && (b == 0x96 || b == 0x94 || b == 0x95 || b == 0x8E || b == 0x8F);
    }
};

inline ftxui::Element seeThrough(ftxui::Element e) { return std::make_shared<SeeThrough>(std::move(e)); }

}  // namespace ui
