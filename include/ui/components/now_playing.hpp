/* NOW PLAYING
- Full-screen view of the current track: large cover, title, artist, album,
  progress, and the lyrics, scrolled in time with the music when synced
*/

#pragma once

#include "../../services/player.hpp"
#include <ftxui/dom/elements.hpp>
#include <memory>
#include <vector>

namespace ui {

// position: seconds into the track (interpolated between ticks by the caller).
// Lyrics lines drawn last frame and the time each starts at (click to seek).
extern std::vector<std::pair<std::shared_ptr<ftxui::Box>, double>> lyric_boxes;
extern ftxui::Box now_playing_cover_box;    // the cover: swipe it to skip

ftxui::Element renderNowPlaying(services::Player::PlayerState* state, double position, double duration, bool playing, int width, int height);

}
