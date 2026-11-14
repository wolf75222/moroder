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
#include <utility>
#include <vector>

namespace services {

// True for the browsers readCookieStore() knows (on this platform).
bool isCookieStoreBrowser(const std::string& browser);

/* Cookie header for music.youtube.com, or empty with `error` set.
- browser is "arc", or "arc:Profile 1" for a profile other than the one
  signed in first (Default, else the first profile that is)
*/
std::string readYouTubeCookies(const std::string& browser, std::string& error);

/* signedInProfiles
- The browser's profiles that hold a YouTube sign-in, {directory, name shown
  in the browser}: only checks the login cookie is there, without decrypting
  anything, so no Keychain prompt
*/
std::vector<std::pair<std::string, std::string>> signedInProfiles(const std::string& browser);

}
