#pragma once

#include <algorithm>

#include <deque>

#include "carousel.hpp"
#include "chips.hpp"
#include "grid.hpp"

namespace ui {

// Set while the UI replays a mouse press only to move the focus (right-click):
// buttons that act on a click (Play, Retry) must ignore that press.
extern bool suppress_click;

// Mouse events skip a component whose `shown` is false (not drawn this frame,
// so its click areas are stale): see render_page in main.cpp.
ftxui::Component MouseGate(ftxui::Component child, std::shared_ptr<bool> shown);

struct ContentEntry {
    std::string category;
    std::shared_ptr<bool> shown = std::make_shared<bool>(true);

    ui::CarouselData carousel_data;
    ui::GridData grid_data;
    ui::ChipsData chips_data;

    int selected = 0;
    bool focused = false;

    ftxui::Component component;
    music::ApiResult result;
};

struct QueueEntry {
    std::string id;
    std::string top;
    std::string bottom;
    std::string url;

    bool is_divider = false;
    std::string divider_label;
};

void buildHome(services::Player::PlayerState* state, std::deque<ContentEntry>& items, ftxui::Component main_container, std::function<bool(const ftxui::Event&, const music::ApiResult&)> on_press, std::function<void()> on_retry);
void buildSearch(services::Player::PlayerState* state, std::deque<ContentEntry>& items, ftxui::Component main_container, std::function<bool(const ftxui::Event&, const music::ApiResult&)> on_press, std::function<void()> on_retry,
                 std::function<bool(const ftxui::Event&, const ui::ChipEntry&)> on_filter);
void buildQueue(services::Player::PlayerState* state, std::deque<ContentEntry>& items, ftxui::Component main_container, std::function<bool(const ftxui::Event&, const int)> on_queue_press);
// Animation frame, advanced by the UI's animation clock while something loads.
extern int anim_frame;

// Queue view drag & drop: each row's area by queue index, and the drag state.
extern std::vector<std::pair<int, std::shared_ptr<ftxui::Box>>> queue_row_boxes;
extern int queue_drag_from, queue_drag_over;
extern int queue_focus_index;     // queue view: row to select after the next rebuild (a move)
extern bool search_has_focus;
// Where covers / thumbnails were drawn this frame (double-click on one plays
// the track in full screen). coverHit() records one.
extern std::deque<ftxui::Box> cover_hit_boxes;
// Where the pointer is (for hover), -1 when not known.
extern int mouse_x, mouse_y;
inline bool hovered(const ftxui::Box& b) { return mouse_x >= 0 && b.Contain(mouse_x, mouse_y); }
// Section titles with a "More" page, drawn this frame: click to open it.
extern std::deque<std::pair<ftxui::Box, std::string>> more_boxes;
// Explore > Charts: its country button (click: the list), drawn this frame.
extern ftxui::Box charts_country_box;
// The countries YouTube Music has charts for: code, name ("ZZ": Global).
const std::vector<std::pair<std::string, std::string>>& chartCountries();
// A section title; with " ›" and clickable when it has a "More" page.
ftxui::Element sectionTitle(const std::string& title, bool more);

// The sidebar's width: a sixth of the window, 22 to 30 columns (a fixed 30
// left a narrow window's page cramped and a wide one's sidebar half empty).
inline int sidebarWidth(int dimx) { return std::clamp(dimx / 6, 22, 30); }
// The keys reach a section's "More" link too: up from the shelf's top row
// lands on its title (drawn lit), enter opens it. open_more is set by main.
extern std::string more_focus;
extern std::function<void(const std::string&)> open_more;
ftxui::Element coverHit(ftxui::Element cover);     // pages must not take the focus from the search bar

/* buildPlaylist
- The open playlist (state->open_playlist): a header with Play / Add to queue,
  then its tracks; on_track gets the key event and the track's index
*/
void buildPlaylist(services::Player::PlayerState* state, std::deque<ContentEntry>& items, ftxui::Component main_container,
                   std::function<bool(const ftxui::Event&, int)> on_track, std::function<void()> on_play_all,
                   std::function<void()> on_queue_all, std::function<void()> on_retry);

// Artist page (state->open_artist): header, then one carousel per section.
// Explore: YouTube Music's page of new albums & singles, trending, videos...
void buildExplore(services::Player::PlayerState* state, std::deque<ContentEntry>& items, ftxui::Component main_container,
                  std::function<bool(const ftxui::Event&, const music::ApiResult&)> on_press, std::function<void()> on_retry,
                  std::function<bool(const ftxui::Event&, const ui::ChipEntry&)> on_tab);
void buildArtist(services::Player::PlayerState* state, std::deque<ContentEntry>& items, ftxui::Component main_container,
                 std::function<bool(const ftxui::Event&, const music::ApiResult&)> on_press, std::function<void()> on_retry);

void buildLibrary(services::Player::PlayerState* state, std::deque<ContentEntry>& main_content_items, ftxui::Component main_content, int rows, const std::string& query, std::function<bool(const ftxui::Event&, const ui::ChipEntry&)> on_chip_press, std::function<bool(const ftxui::Event&, const music::ApiResult&)> on_item_press, std::function<void()> on_retry);

}
