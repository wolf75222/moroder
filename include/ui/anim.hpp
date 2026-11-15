#pragma once

/* ANIMATION CLOCK
- Short, quiet motion (scrolling, a sliding underline, a fade): what is
  drawn asks for another frame with more() while it is still moving, and the
  app's clock thread then redraws at about 30 fps. Nothing moving: no frames
  beyond the player's 1 s tick (no CPU spent on an idle screen)
- Time-based, not frame-based: a slow or busy terminal shows fewer frames of
  the same motion, never a slower one
*/

#include <atomic>
#include <chrono>
#include <cmath>

namespace ui::anim {

using Clock = std::chrono::steady_clock;

// Set while drawing when something is still in motion (read by the clock thread).
inline std::atomic<bool> wanted{false};
inline void more() { wanted = true; }

inline double msSince(Clock::time_point t) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

// 0..1 over `ms` from t (1 once done; asks for frames until then).
inline double progress(Clock::time_point t, double ms) {
    if (t == Clock::time_point{}) return 1.0;
    const double p = msSince(t) / ms;
    if (p >= 1.0) return 1.0;
    more();
    return p < 0 ? 0 : p;
}

// Ease-out (fast start, gentle stop).
inline double easeOut(double p) { return 1.0 - std::pow(1.0 - p, 3.0); }

/* A value that glides to its target: each frame closes `rate` of the gap
(about 4 frames for most of it), snapping once under `snap`. */
inline double approach(double& shown, double target, double rate = 0.45, double snap = 0.35) {
    const double gap = target - shown;
    if (std::abs(gap) <= snap) { shown = target; return shown; }
    shown += gap * rate;
    more();
    return shown;
}

}  // namespace ui::anim
