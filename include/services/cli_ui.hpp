/* TERMINAL OUTPUT FOR THE COMMANDS (update, doctor, uninstall, help)
- Same look as install.sh: a red ▶ title, then a list of tasks; the ones that
  run side by side each show a spinner and resolve to ✓ / ! / ✗ with a detail
  as soon as they finish
- Live redraw only on a terminal (and without NO_COLOR); otherwise each task
  prints one plain line when it finishes
*/
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <sys/ioctl.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace cli {

inline bool fancy() {
    static const bool on = [] {
        const char* term = std::getenv("TERM");
        return isatty(STDOUT_FILENO) && !std::getenv("NO_COLOR") && term && std::string(term) != "dumb";
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

inline int columns() {
    winsize w{};
    return ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 20 ? w.ws_col : 80;
}

// At most n columns, never cutting a UTF-8 character in half.
inline std::string clip(std::string s, size_t n) {
    size_t cols = 0, i = 0;
    while (i < s.size()) {
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80 && ++cols > n) { s.resize(i); break; }
        i++;
    }
    return s;
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
    if (!isatty(STDIN_FILENO)) std::cout << answer << "\n";
    if (answer.empty()) return yes_by_default;
    const char c = answer[0];
    return c == 'y' || c == 'Y' || c == 'o' || c == 'O';
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
        std::cout << "\033[?25l" << std::flush;
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
