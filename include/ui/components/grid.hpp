#pragma once

#include <ftxui/component/component.hpp>
#include <string>
#include <vector>

#include "../../../include/services/player.hpp"
#include "../structs/entries.hpp"
#include "hscroll.hpp"

namespace ui {

enum class GridStyle {
    List,
    Tile
};

constexpr int GRID_TILE_COVER_WIDTH = 20;
constexpr int GRID_TILE_COVER_HEIGHT = 10;
constexpr int GRID_TILE_WIDTH = 23;
constexpr int GRID_TILE_HEIGHT = 14;

struct GridData {
    std::string category_name;

    std::vector<music::ApiResult> results;
    std::vector<std::string> entries_spoof;
    std::vector<ImageEntry> entries;

    bool is_loading = true;

    HScroll hscroll;     // sideways, for the mouse
    bool more = false;   // its title opens a "More" page (Explore)
};

void getGridData(services::Player::PlayerState* state, std::string category, GridData* data);
/* getLibraryGridData
- filter is the active chip id, empty means everything except songs
- query narrows the rows by title and by artist / author, empty means no narrowing
*/
void getLibraryGridData(services::Player::PlayerState* state, const std::string& filter, const std::string& query, GridData* data);

/* rows > 0: that many rows, filled column by column, wider than the screen if
need be (a shelf). rows < 0: -rows columns, filled row by row, as many rows
as it takes (a page that scrolls down, like the library). */
ftxui::Component Grid(GridData* data, int* selected, bool* focused, int rows, std::function<bool(const ftxui::Event&, const music::ApiResult&)> on_press, GridStyle style = GridStyle::List);
}
