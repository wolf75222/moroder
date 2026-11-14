/* COVER CACHE
- Covers are kept on disk (~/.cache/moroder/covers), so a page seen before
  shows its artwork at once instead of downloading it again
- cover(url) returns the local file when it is there, otherwise the URL
  (image_view downloads it as before) and fetches a copy in the background
*/

#pragma once

#include <functional>
#include <string>

namespace ui {

std::string cover(const std::string& url);

// Called (from a download thread) when a cover becomes available on disk.
void onCoverReady(std::function<void()> callback);

}
