/* LAST.FM SCROBBLING
- Optional: needs LASTFM_API_KEY / LASTFM_API_SECRET (an API account from
  last.fm/api) and `moroder lastfm` once, which saves a session key to
  ~/.config/moroder/lastfm.json (0600)
- Now playing when a track starts; a scrobble once half of it (or 4 min) has
  played, for tracks of 30 s or more, as Last.fm asks
- Sent from a background thread; a failure is logged and dropped
*/
#pragma once

#include <string>

namespace services::scrobble {

bool enabled();     // key, secret and session present

void nowPlaying(const std::string& artist, const std::string& track, const std::string& album, int duration_s);
void scrobble(const std::string& artist, const std::string& track, const std::string& album, int duration_s, long long started_at);

int connect();      // `moroder lastfm`: interactive sign-in, returns an exit code

}
