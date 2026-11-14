#pragma once

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
    bool liked = false;
    bool can_like = false;
    std::string notice;     // transient message, empty when none

    bool is_playing = false;
    bool can_skip_forwards  = true;
    bool can_skip_backwards = true;

    services::Player::LoopMode loop_mode = services::Player::LoopMode::None;
};

void getPlaybackData(services::Player::PlayerState* state, PlaybackData* data, int dimx);
ftxui::Component PlaybackBar(PlaybackData* data);

}
