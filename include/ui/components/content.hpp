#pragma once

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
void buildSearch(services::Player::PlayerState* state, std::deque<ContentEntry>& items, ftxui::Component main_container, std::function<bool(const ftxui::Event&, const music::ApiResult&)> on_press, std::function<void()> on_retry);
void buildQueue(services::Player::PlayerState* state, std::deque<ContentEntry>& items, ftxui::Component main_container, std::function<bool(const ftxui::Event&, const int)> on_queue_press);
// Animation frame, advanced by the UI's animation clock while something loads.
extern int anim_frame;

/* buildPlaylist
- The open playlist (state->open_playlist): a header with Play / Add to queue,
  then its tracks; on_track gets the key event and the track's index
*/
void buildPlaylist(services::Player::PlayerState* state, std::deque<ContentEntry>& items, ftxui::Component main_container,
                   std::function<bool(const ftxui::Event&, int)> on_track, std::function<void()> on_play_all,
                   std::function<void()> on_queue_all, std::function<void()> on_retry);

// Artist page (state->open_artist): header, then one carousel per section.
void buildArtist(services::Player::PlayerState* state, std::deque<ContentEntry>& items, ftxui::Component main_container,
                 std::function<bool(const ftxui::Event&, const music::ApiResult&)> on_press, std::function<void()> on_retry);

void buildLibrary(services::Player::PlayerState* state, std::deque<ContentEntry>& main_content_items, ftxui::Component main_content, int rows, const std::string& query, std::function<bool(const ftxui::Event&, const ui::ChipEntry&)> on_chip_press, std::function<bool(const ftxui::Event&, const music::ApiResult&)> on_item_press, std::function<void()> on_retry);

}
