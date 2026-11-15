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
extern ftxui::Box now_playing_title_box;    // right-click: the track's menu
extern ftxui::Box now_playing_artist_box;   // click: the artist's page
extern ftxui::Box now_playing_album_box;    // click: the album

/* What plays next, shown beside the lyrics (no need for the queue view):
filled by the caller each frame, index = the queue index (skipTo plays it). */
struct UpNextRow {
    int index = 0;
    bool autoplay = false;      // from the autoplay radio, after the queue
    std::string title, artist, thumb;
};
extern std::vector<UpNextRow> now_playing_upnext;
// Its rows drawn last frame, with their queue index (click to play).
extern std::vector<std::pair<ftxui::Box, int>> now_playing_upnext_boxes;

ftxui::Element renderNowPlaying(services::Player::PlayerState* state, double position, double duration, bool playing, int width, int height);

}
