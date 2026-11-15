/* SEARCH SUGGESTIONS
- What YouTube Music proposes for a partly typed search, as in its own bar
- One small anonymous request (no account data is sent); blocking: call it
  off the UI thread
*/
#pragma once
#include <functional>

#include <string>
#include <vector>

namespace services {

// cancelled: polled while the request runs; true drops it (the next letter typed).
std::vector<std::string> searchSuggestions(const std::string& input, std::function<bool()> cancelled = {});

}
