#include "../../include/ui/icons.hpp"
#include "../../include/config/config.hpp"

#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>

#include <cstdlib>
#include <string>
#include <unordered_map>

namespace {

// Nerd Font glyph (its UTF-8) -> a one-column symbol common fonts have.
const std::unordered_map<std::string, std::string>& table() {
    static const std::unordered_map<std::string, std::string> t = [] {
        const std::pair<char32_t, const char*> pairs[] = {
            {0xF001, "♪"}, {0xF000, "♪"}, {0xF185, "☼"}, {0xEE8A, "♫"}, {0xF0E7, "↯"},   // categories
            {0xF048, "«"}, {0xF04B, "►"}, {0xF04C, "‖"}, {0xF051, "»"},                 // transport
            {0xF0B6, "↻"}, {0xF074, "⇄"},                                               // loop, shuffle
            {0xF026, "♪"}, {0xF027, "♪"}, {0xF028, "♪"},                                 // volume
            {0xF004, "♥"}, {0xF08A, "♡"},                                               // like
            {0xF053, "‹"}, {0xF054, "›"}, {0xF077, "▲"}, {0xF078, "▼"},                 // arrows
            {0xF067, "+"}, {0xF0FE, "+"}, {0xF068, "−"}, {0xF00D, "×"}, {0xF1F8, "×"},   // edit
            {0xF040, "✎"}, {0xF0C5, "⧉"}, {0xF0C1, "∞"}, {0xF0C7, "↓"}, {0xF12D, "⌫"}, {0xF08E, "↗"},
            {0xF07C, "▸"}, {0xF03A, "≡"}, {0xF1BE, "◉"}, {0xF007, "●"}, {0xF1DA, "↺"},
            {0xF002, "⌕"}, {0xF4E2, "⌂"}, {0xF14E, "◎"}, {0xF02E, "▤"}, {0xF16A, "►"},
            {0xF05D, "✓"}, {0xEA87, "✗"},
        };
        std::unordered_map<std::string, std::string> m;
        for (const auto& [cp, plain] : pairs) {
            std::string utf8;
            if (cp < 0x10000) {
                utf8 += static_cast<char>(0xE0 | (cp >> 12));
                utf8 += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                utf8 += static_cast<char>(0x80 | (cp & 0x3F));
            }
            m[utf8] = plain;
        }
        return m;
    }();
    return t;
}

// A private-use glyph (U+E000..U+F8FF, three UTF-8 bytes EE/EF ..).
bool privateUse(const std::string& s) {
    return s.size() == 3 && (static_cast<unsigned char>(s[0]) == 0xEE || static_cast<unsigned char>(s[0]) == 0xEF) &&
           !(static_cast<unsigned char>(s[0]) == 0xEF && static_cast<unsigned char>(s[1]) >= 0xA4);   // >= U+F900: not PUA
}

class PlainIcons : public ftxui::Node {
public:
    explicit PlainIcons(ftxui::Element child) : Node(ftxui::Elements{std::move(child)}) {}

    void ComputeRequirement() override {
        children_[0]->ComputeRequirement();
        requirement_ = children_[0]->requirement();
    }

    void SetBox(ftxui::Box box) override {
        Node::SetBox(box);
        children_[0]->SetBox(box);
    }

    void Render(ftxui::Screen& screen) override {
        children_[0]->Render(screen);
        const auto& t = table();
        for (int y = box_.y_min; y <= box_.y_max; ++y) {
            for (int x = box_.x_min; x <= box_.x_max; ++x) {
                std::string& c = screen.CellAt(x, y).character;
                if (!privateUse(c)) continue;
                auto it = t.find(c);
                c = it == t.end() ? "•" : it->second;
            }
        }
    }
};

}

bool ui::plainIcons() {
    static const bool plain = [] {
        if (const char* env = std::getenv("MORODER_ICONS")) return std::string(env) == "plain";
        std::string mode = "auto";
        try { mode = Config::get().ICONS; } catch (...) {}
        if (mode == "plain") return true;
        if (mode == "nerd") return false;
#ifdef _WIN32
        return true;      // Windows Terminal's default font has no Nerd Font glyphs
#else
        return false;
#endif
    }();
    return plain;
}

ftxui::Element ui::withIcons(ftxui::Element view) {
    if (!plainIcons()) return view;
    return std::make_shared<PlainIcons>(std::move(view));
}
