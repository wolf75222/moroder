#pragma once

#include "../../../include/services/player.hpp"
#include "../structs/entries.hpp"
#include "hscroll.hpp"

#include <ftxui/component/component.hpp>
#include <string>
#include <vector>

namespace ui {

struct CarouselData {
    std::string category_name;

    std::vector<music::ApiResult> results;
    std::vector<std::string> entries_spoof;
    std::vector<ImageEntry> entries;

    bool is_loading = true;

    HScroll hscroll;     // sideways, for the mouse
    bool more = false;   // its title opens a "More" page (Explore)
    std::vector<ftxui::Box> item_boxes;   // each item, as drawn last frame (hover)

};

void getCarouselData(services::Player::PlayerState* state, std::string category, CarouselData* data);
// Same, from any set of shelves (an artist page's, for one).
void getCarouselDataFrom(const std::unordered_map<std::string, std::vector<music::ApiResult>>& shelves, std::string category, CarouselData* data);
ftxui::Component Carousel(CarouselData* data, int* selected, bool* focused, std::function<bool(const ftxui::Event&, const music::ApiResult&)> on_press);

}
