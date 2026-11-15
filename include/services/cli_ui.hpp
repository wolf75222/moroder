/* TERMINAL OUTPUT FOR THE COMMANDS (update, doctor, uninstall, help)
- Same look as install.sh: a red ▶ title, then a list of tasks; the ones that
  run side by side each show a spinner and resolve to ✓ / ! / ✗ with a detail
  as soon as they finish
- Live redraw only on a terminal (and without NO_COLOR); otherwise each task
  prints one plain line when it finishes
*/
#pragma once

#include <algorithm>
#include <csignal>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include "../utils/platform.hpp"
#include <vector>

namespace cli {

inline bool fancy() {
    static const bool on = [] {
        const char* term = std::getenv("TERM");
#ifdef _WIN32
        (void)term;   // Windows Terminal / conhost with VT: no TERM there
        return platform::isTerminal(1) && !std::getenv("NO_COLOR");
#else
        return platform::isTerminal(1) && !std::getenv("NO_COLOR") && term && std::string(term) != "dumb";
#endif
    }();
    return on;
}

inline std::string sgr(const char* code) { return fancy() ? std::string("\033[") + code + "m" : ""; }
inline std::string red()    { return sgr("31"); }
inline std::string green()  { return sgr("32"); }
inline std::string yellow() { return sgr("33"); }
inline std::string dim()    { return sgr("2"); }
inline std::string bold()   { return sgr("1"); }
inline std::string reset()  { return sgr("0"); }

inline int columns() { return platform::terminalColumns(); }

// At most n columns, never cutting a UTF-8 character in half.
inline std::string clip(std::string s, size_t n) {
    size_t cols = 0, i = 0;
    while (i < s.size()) {
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80 && ++cols > n) { s.resize(i); break; }
        i++;
    }
    return s;
}

/* hideCursor
- Hidden while a spinner or the picker draws; shown again on the way out,
  whatever the way out is: normally, Ctrl-C, or the program ending
*/
inline void showCursorNow(int sig) {
    platform::writeTerminal("\033[?25h");
    if (sig) { std::signal(sig, SIG_DFL); std::raise(sig); }
}

inline void hideCursor() {
    static std::once_flag once;
    std::call_once(once, [] {
        std::signal(SIGINT, showCursorNow);
        std::signal(SIGTERM, showCursorNow);
        std::atexit([] { if (fancy()) showCursorNow(0); });
    });
    platform::writeTerminal("\033[?25l");
}

inline void title(const std::string& what, const std::string& detail = "") {
    std::cout << "\n  " << bold() << red() << "▶ " << what << reset();
    if (!detail.empty()) std::cout << "  " << dim() << detail << reset();
    std::cout << "\n\n";
}

// A plain aligned row: "    label     value".
inline void row(const std::string& label, const std::string& value, const std::string& note = "") {
    std::string pad = label;
    while (pad.size() < 12) pad += ' ';
    std::cout << "    " << dim() << pad << reset() << value;
    if (!note.empty()) std::cout << "  " << dim() << note << reset();
    std::cout << "\n";
}

// [Y/n] (or [y/N] when the default is no). An empty line takes the default;
// no answer at all (closed or empty input) is always no.
inline bool ask(const std::string& question, bool yes_by_default = true) {
    std::cout << "\n  " << bold() << "?" << reset() << " " << question
              << dim() << (yes_by_default ? " [Y/n] " : " [y/N] ") << reset() << std::flush;
    std::string answer;
    if (!std::getline(std::cin, answer)) { std::cout << "\n"; return false; }
    if (!platform::isTerminal(0)) std::cout << answer << "\n";
    if (answer.empty()) return yes_by_default;
    const char c = answer[0];
    return c == 'y' || c == 'Y' || c == 'o' || c == 'O';
}

/* select
- A list picked with the arrow keys (or j/k, or its number), Enter to
  choose, Esc / q to cancel (-1). Collapses to one line once chosen
- Not a terminal: the default, without asking
*/
struct Option { std::string label, detail; };

inline int select(const std::string& question, const std::vector<Option>& options, int initial = 0) {
    if (options.empty()) return -1;
    initial = std::clamp(initial, 0, static_cast<int>(options.size()) - 1);
    if (!fancy() || !platform::isTerminal(0)) return initial;

    platform::RawInput raw;
    if (!raw.ok()) return initial;

    size_t width = 0;
    for (const auto& o : options) width = std::max(width, o.label.size());

    int at = initial;
    const int n = static_cast<int>(options.size());
    // Each line within the width: a wrapped one would throw the redraw off.
    const size_t cols = static_cast<size_t>(std::max(20, columns() - 1));
    auto draw = [&](bool first) {
        std::string out;
        if (!first) out += "\033[" + std::to_string(n + 1) + "A";
        out += "\r\033[2K  " + bold() + "?" + reset() + "  " + clip(question, cols > 16 ? cols - 16 : cols) + "  " + dim() + "↑↓ Enter" + reset() + "\r\n";
        for (int i = 0; i < n; i++) {
            std::string label = options[i].label;
            label.resize(width + 2, ' ');
            const size_t room = cols > 5 + label.size() ? cols - 5 - label.size() : 0;
            out += "\r\033[2K  " + (i == at ? red() + "❯" + reset() + "  " + bold() + clip(label, cols - 5) + reset()
                                            : std::string("   ") + clip(label, cols - 5))
                 + dim() + clip(options[i].detail, room) + reset() + "\r\n";
        }
        platform::writeTerminal(out);
    };

    std::cout << std::flush;
    hideCursor();
    draw(true);
    int chosen = -2;
    std::string pending;
    while (chosen == -2) {
        char buf[32];
        const int got = raw.read(buf, sizeof buf, 1000);
        if (got < 0) { chosen = -1; break; }
        pending.append(buf, static_cast<size_t>(got));
        while (!pending.empty() && chosen == -2) {
            if (pending.rfind("\033[A", 0) == 0 || pending.rfind("\033OA", 0) == 0) { at = (at + n - 1) % n; pending.erase(0, 3); }
            else if (pending.rfind("\033[B", 0) == 0 || pending.rfind("\033OB", 0) == 0) { at = (at + 1) % n; pending.erase(0, 3); }
            else if (pending[0] == '\033' && pending.size() >= 3 && pending[1] == '[') pending.erase(0, 3);   // other keys
            else if (pending[0] == '\033' && pending.size() == 1) {
                // Esc alone, or the start of an arrow key whose rest is late (SSH)?
                char more[16];
                const int m = raw.read(more, sizeof more, 60);
                if (m > 0) pending.append(more, static_cast<size_t>(m));
                else chosen = -1;
            }
            else {
                const char c = pending[0];
                pending.erase(0, 1);
                if (c == '\r' || c == '\n') chosen = at;
                else if (c == 'k') at = (at + n - 1) % n;
                else if (c == 'j') at = (at + 1) % n;
                else if (c == 'q' || c == 3) chosen = -1;                 // q, Ctrl-C
                else if (c >= '1' && c <= '9' && c - '1' < n) at = c - '1';
            }
        }
        if (chosen == -2) draw(false);
    }

    // Collapse to the answer: clear the list, one line in its place.
    std::string out = "\033[" + std::to_string(n + 1) + "A\033[J\r";
    out += "  " + (chosen >= 0 ? green() + "✓" : dim() + "–") + reset() + "  " + clip(question, cols > 30 ? cols - 30 : cols) + "  "
         + (chosen >= 0 ? bold() + options[chosen].label + reset() : dim() + "cancelled" + reset()) + "\r\n";
    out += "\033[?25h";
    platform::writeTerminal(out);
    return chosen;
}


enum class State { Wait, Run, Ok, Warn, Fail, Skip };

class Live {
public:
    size_t add(const std::string& label) {
        std::lock_guard<std::mutex> lock(mutex_);
        tasks_.push_back({label, "", State::Wait, false});
        return tasks_.size() - 1;
    }

    void set(size_t i, State state, const std::string& detail = "") {
        std::lock_guard<std::mutex> lock(mutex_);
        tasks_[i].state = state;
        tasks_[i].detail = detail;
        if (!fancy() && done(state) && !tasks_[i].printed) {
            std::cout << line(tasks_[i]) << "\n" << std::flush;
            tasks_[i].printed = true;
        }
    }

    void start() {
        if (!fancy() || thread_.joinable()) return;
        std::cout << std::flush;
        hideCursor();
        stop_ = false;
        thread_ = std::thread([this] {
            while (!stop_) {
                { std::lock_guard<std::mutex> lock(mutex_); draw(); }
                std::this_thread::sleep_for(std::chrono::milliseconds(80));
                frame_++;
            }
        });
    }

    // Final picture; the list can be extended and started again afterwards.
    void finish() {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
        std::lock_guard<std::mutex> lock(mutex_);
        if (fancy()) { draw(); std::cout << "\033[?25h" << std::flush; }
        else for (auto& t : tasks_) if (!t.printed) { std::cout << line(t) << "\n"; t.printed = true; }
        // What is on screen is final: later draws start below it.
        for (auto& t : tasks_) t.printed = true;
        drawn_ = 0;
        first_ = tasks_.size();
    }

    ~Live() { if (thread_.joinable()) finish(); }

    int count(State s) {
        std::lock_guard<std::mutex> lock(mutex_);
        int n = 0;
        for (const auto& t : tasks_) n += t.state == s;
        return n;
    }

private:
    struct Task { std::string label, detail; State state; bool printed; };

    static bool done(State s) { return s != State::Wait && s != State::Run; }

    std::string line(const Task& t) const {
        static const char* frames[] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
        std::string icon;
        switch (t.state) {
            case State::Run:  icon = yellow() + frames[frame_ % 10] + reset(); break;
            case State::Ok:   icon = green() + "✓" + reset(); break;
            case State::Warn: icon = yellow() + "!" + reset(); break;
            case State::Fail: icon = red() + "✗" + reset(); break;
            case State::Skip: icon = dim() + "–" + reset(); break;
            default:          icon = dim() + "·" + reset(); break;
        }
        std::string label = t.label;
        while (label.size() < 14) label += ' ';
        const std::string detail = clip(t.detail, static_cast<size_t>(std::max(10, columns() - 22)));
        const std::string tint = t.state == State::Warn ? yellow() : t.state == State::Fail ? red() : dim();
        return (fancy() ? "\r\033[2K" : "") + std::string("  ") + icon + "  " + label + " " + tint + detail + reset();
    }

    void draw() {
        if (drawn_) std::cout << "\033[" << drawn_ << "A\033[J";
        drawn_ = 0;
        for (size_t i = first_; i < tasks_.size(); i++) { std::cout << line(tasks_[i]) << "\n"; drawn_++; }
        std::cout << std::flush;
    }

    std::vector<Task> tasks_;
    std::mutex mutex_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<unsigned> frame_{0};
    int drawn_ = 0;
    size_t first_ = 0;
};

}
