#pragma once

#include <ftxui/screen/box.hpp>

#include "../../services/player.hpp"

#include <ftxui/component/component.hpp>
#include <string>

namespace ui {

struct PlaybackData {
    std::string title;
    std::string artist;
    std::string image_url;

    int progress = 0;
    int dimx = 0;

    // Seconds, from the position tick; duration 0 while a stream loads.
    uint64_t position = 0;
    uint64_t duration = 0;

    int volume = 100;
    int frame = 0;          // animation frame, for the loading indicator
    bool liked = false;
    bool can_like = false;
    std::string notice;     // transient message, empty when none

    // Screen areas of the clickable parts, filled at render time.
    ftxui::Box box_progress, box_prev, box_play, box_next, box_like, box_volume, box_loop, box_shuffle;

    bool is_playing = false;
    bool can_skip_forwards  = true;
    bool can_skip_backwards = true;

    services::Player::LoopMode loop_mode = services::Player::LoopMode::None;
};

void getPlaybackData(services::Player::PlayerState* state, PlaybackData* data, int dimx);
ftxui::Component PlaybackBar(PlaybackData* data);

}
