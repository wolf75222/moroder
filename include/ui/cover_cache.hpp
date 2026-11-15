/* COVER CACHE
- Covers are kept on disk (~/.cache/moroder/covers), so a page seen before
  shows its artwork at once instead of downloading it again
- cover(url) returns the local file when it is there, otherwise the URL
  (image_view downloads it as before) and fetches a copy in the background
*/

#pragma once

#include <ftxui/dom/elements.hpp>
#include <functional>
#include <string>

namespace ui {

std::string cover(const std::string& url);

/* A cover drawn w x h cells: while it downloads, a dark tile with a soft
light passing over it; once there, it fades in (about 250 ms) instead of
popping in. */
ftxui::Element coverArt(const std::string& url, int w, int h);

// The same loading shimmer, w x h cells (skeleton pages).
ftxui::Element shimmer(int w, int h);

// A cover-sized placeholder for items without artwork (a new playlist):
// a quiet frame with a note, instead of an empty hole.
ftxui::Element noCover(int width, int height);

// Called (from a download thread) when a cover becomes available on disk.
void onCoverReady(std::function<void()> callback);

}
