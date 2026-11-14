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

    // Suggestions for what is typed (history matches first), picked with
    // Up / Down, Tab fills one in, Enter searches it.
    std::vector<std::string> suggestions;
    std::string suggestions_for;            // the text they were made for
    int suggestion_index = -1;
    std::vector<ftxui::Box> suggestion_boxes;
    ftxui::Box box;                         // the bar, to place the list under it
    std::function<void(const std::string&)> onTyped;    // the text changed
};

ftxui::Component SearchBar(SearchBarData* data);

}
