/* NOTIFY
- Desktop notification when the track changes, so you know what is playing
  without switching back to the terminal
- macOS: osascript (Notification Center); Linux: notify-send when installed
- Fire-and-forget: runs off the caller's thread and never blocks playback
*/

#pragma once

#include <string>

namespace services {

// cover: image URL (or path) shown in the notification where supported.
void notifyTrack(const std::string& title, const std::string& subtitle, const std::string& cover = "");

}
