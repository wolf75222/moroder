/* LYRICS
- Synced lyrics from LRCLIB (lrclib.net, a free public lyrics database):
  timestamped lines when available, plain text otherwise
- Sends the track's title, artist and duration to lrclib.net; off with
  ENABLE_LYRICS = false
*/

#pragma once

#include <string>
#include <utility>
#include <vector>

namespace services {

struct Lyrics {
    std::vector<std::pair<double, std::string>> lines;   // seconds, text (synced)
    std::vector<std::string> plain;                      // when not synced
    bool found = false;
    double shift = 0;   // suggested delay (s): a music video's intro before the album's timings
};

// video: a music video (its intro / outro make it longer than the song).
Lyrics fetchLyrics(const std::string& title, const std::string& artist, double duration_seconds, bool video = false);

}
