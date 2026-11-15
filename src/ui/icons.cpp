#include "../../include/ui/icons.hpp"
#include "../../include/config/config.hpp"
#include "../../include/utils/platform.hpp"

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
            {0xF04B2, "◷"},                                                             // sleep timer (Material Design range)
        };
        std::unordered_map<std::string, std::string> m;
        for (const auto& [cp, plain] : pairs) {
            std::string utf8;
            if (cp < 0x10000) {
                utf8 += static_cast<char>(0xE0 | (cp >> 12));
                utf8 += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                utf8 += static_cast<char>(0x80 | (cp & 0x3F));
            } else {
                utf8 += static_cast<char>(0xF0 | (cp >> 18));
                utf8 += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                utf8 += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                utf8 += static_cast<char>(0x80 | (cp & 0x3F));
            }
            m[utf8] = plain;
        }
        return m;
    }();
    return t;
}

/* Windows' own console window, not a terminal app: asked of the window itself
(see platform::legacyConsoleWindow). Environment variables weren't enough: a
moroder.exe Windows hands over to Windows Terminal (double-click, Win+R) has
no WT_SESSION.
*/
bool consoleWindow() {
    static const bool console = platform::isTerminal(1) && platform::legacyConsoleWindow();
    return console;
}

// A private-use glyph: U+E000..U+F8FF (three UTF-8 bytes EE/EF ..) or, for
// Nerd Fonts' Material Design icons, U+F0000..U+FFFFF (four bytes F3 B0-BF ..).
bool privateUse(const std::string& s) {
    if (s.size() == 4)
        return static_cast<unsigned char>(s[0]) == 0xF3 && static_cast<unsigned char>(s[1]) >= 0xB0;
    return s.size() == 3 && (static_cast<unsigned char>(s[0]) == 0xEE || static_cast<unsigned char>(s[0]) == 0xEF) &&
           !(static_cast<unsigned char>(s[0]) == 0xEF && static_cast<unsigned char>(s[1]) >= 0xA4);   // >= U+F900: not PUA
}

/* The Windows console's own fonts (Consolas, Lucida Console) and no font
fallback to count on: the symbols, partial blocks, heavy and rounded lines
and braille Moroder draws came out as boxes. Each becomes one of the
characters both fonts have (the old PC set: ─│┌ ═║╔ ▀▄█▌ ♪♥►○•≡↔∞...).
*/
const std::unordered_map<std::string, std::string>& consoleTable() {
    static const std::unordered_map<std::string, std::string> t = {
        // symbols and icons
        // Unliked ♡ / liked ♥ stay apart without colour; search shows its key.
        {"♡", "○"}, {"↻", "∞"}, {"↺", "←"}, {"⇄", "↔"}, {"⌕", "/"}, {"▤", "≡"}, {"◎", "◙"},
        {"◉", "•"}, {"●", "•"}, {"▶", "►"}, {"▸", "►"}, {"▾", "▼"}, {"▪", "■"}, {"□", "■"},
        {"✓", "√"}, {"✗", "x"}, {"✎", "*"}, {"⧉", "="}, {"↗", "→"}, {"↯", "♦"}, {"◷", "z"},
        {"⇧", "^"}, {"⌥", "+"}, {"⌫", "←"}, {"‖", "║"}, {"−", "-"}, {"\xEF\xBF\xBD", "?"},
        // rounded corners, heavy and half lines
        {"╭", "┌"}, {"╮", "┐"}, {"╰", "└"}, {"╯", "┘"},
        {"━", "═"}, {"┃", "║"}, {"┏", "╔"}, {"┓", "╗"}, {"┗", "╚"}, {"┛", "╝"},
        {"┣", "╠"}, {"┫", "╣"}, {"┳", "╦"}, {"┻", "╩"}, {"╋", "╬"}, {"╏", "║"},
        {"╴", "─"}, {"╶", "─"}, {"╵", "│"}, {"╷", "│"}, {"╸", "═"}, {"╺", "═"}, {"╹", "║"}, {"╻", "║"},
        {"⎺", "▀"}, {"⎻", "─"}, {"⎼", "─"}, {"⎽", "▄"}, {"⎢", "│"}, {"⎥", "│"},
        // partial blocks and quadrants
        {"▁", "─"}, {"▂", "▄"}, {"▃", "▄"}, {"▅", "▄"}, {"▆", "█"}, {"▇", "█"},
        {"▏", "▌"}, {"▎", "▌"}, {"▍", "▌"}, {"▋", "▌"}, {"▊", "█"}, {"▉", "█"},
        {"▖", "▄"}, {"▗", "▄"}, {"▘", "▀"}, {"▝", "▀"}, {"▚", "▀"}, {"▞", "▀"},
        {"▙", "█"}, {"▛", "█"}, {"▜", "█"}, {"▟", "█"},
        // spinner: braille frames to a turning line
        {"⠋", "|"}, {"⠙", "/"}, {"⠹", "-"}, {"⠸", "\\"}, {"⠼", "|"}, {"⠴", "/"}, {"⠦", "-"}, {"⠧", "\\"}, {"⠇", "|"}, {"⠏", "/"},
    };
    return t;
}

class ConsoleGlyphs : public ftxui::Node {
public:
    explicit ConsoleGlyphs(ftxui::Element child) : Node(ftxui::Elements{std::move(child)}) {}

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
        const auto& t = consoleTable();
        for (int y = box_.y_min; y <= box_.y_max; ++y) {
            for (int x = box_.x_min; x <= box_.x_max; ++x) {
                std::string& c = screen.CellAt(x, y).character;
                if (c.size() < 2) continue;
                auto it = t.find(c);
                if (it != t.end()) c = it->second;
            }
        }
    }
};

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
        // The console window set to a Nerd Font (the installer does it for
        // moroder.exe started on its own): its icons. Elsewhere plain:
        // Windows Terminal's default font has no Nerd Font glyphs.
        if (consoleWindow()) {
            const std::string font = platform::consoleFontName();
            if (font.find("Nerd Font") != std::string::npos || font.find(" NF") != std::string::npos) return false;
        }
        return true;
#else
        return false;
#endif
    }();
    return plain;
}

bool ui::consoleGlyphs() {
    static const bool on = [] {
        if (const char* env = std::getenv("MORODER_GLYPHS")) return std::string(env) == "console";
        // Nerd Font icons: the console has been given a Nerd Font, which has
        // every one of these characters too.
        return consoleWindow() && plainIcons();
    }();
    return on;
}

ftxui::Element ui::withIcons(ftxui::Element view) {
    if (plainIcons()) view = std::make_shared<PlainIcons>(std::move(view));
    if (consoleGlyphs()) view = std::make_shared<ConsoleGlyphs>(std::move(view));
    return view;
}
