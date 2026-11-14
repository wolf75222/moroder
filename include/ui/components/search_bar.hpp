#pragma once

#include <ftxui/component/component.hpp>
#include <string>
#include <vector>

namespace ui {

struct SearchBarData {
    std::string value;

    // Past searches, newest first; Up / Down walk through them.
    std::vector<std::string> history;
    int history_index = -1;
    int cursor = 0;
    std::function<void(const std::string&)> onSearch;
};

ftxui::Component SearchBar(SearchBarData* data);

}
