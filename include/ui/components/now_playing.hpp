/* NOW PLAYING
- Full-screen view of the current track: large cover, title, artist, album,
  progress, and the lyrics, scrolled in time with the music when synced
*/

#pragma once

#include "../../services/player.hpp"
#include <ftxui/dom/elements.hpp>

namespace ui {

// position: seconds into the track (interpolated between ticks by the caller).
ftxui::Element renderNowPlaying(services::Player::PlayerState* state, double position, double duration, bool playing, int width, int height);

}
