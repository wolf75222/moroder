/* BROWSER COOKIES
- Reads the YouTube session straight from a Chromium-family browser's own
  cookie store (Arc, Chrome, Brave, Edge, Chromium, Vivaldi), as yt-dlp's
  --cookies-from-browser does, so the session never goes stale: the browser
  keeps rotating it and Moroder re-reads it
- macOS only: the store's key lives in the login Keychain ("<Browser> Safe
  Storage"); macOS asks once for access ("Always Allow" keeps it quiet)
*/

#pragma once

#include <string>

namespace services {

// True for the browsers readCookieStore() knows (on this platform).
bool isCookieStoreBrowser(const std::string& browser);

// Cookie header for music.youtube.com, or empty with `error` set.
std::string readYouTubeCookies(const std::string& browser, std::string& error);

}
