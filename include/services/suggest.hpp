/* SEARCH SUGGESTIONS
- What YouTube Music proposes for a partly typed search, as in its own bar
- One small anonymous request (no account data is sent); blocking: call it
  off the UI thread
*/
#pragma once

#include <string>
#include <vector>

namespace services {

std::vector<std::string> searchSuggestions(const std::string& input);

}
