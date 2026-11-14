/* MPRIS
- Provides an interface to sdbus-c++ so that the user can control the player
  remotely from other applications.

- This implementation is based on code from: https://github.com/chrg127/mpris-server and has been modified for this application's use case.
- Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
*/

#pragma once

#include <string>

#ifndef MORODER_NO_MPRIS
#include <sdbus-c++/sdbus-c++.h>
#else
// No D-Bus (macOS and others): small stand-ins so the same interface compiles.
// On macOS the backend is mpris_macos.mm (Now Playing + media keys), elsewhere
// mpris_stub.cpp. Variant keeps the value so the backend can read metadata.
#include <cstdint>
#include <type_traits>
#include <variant>
namespace sdbus {
class Variant {
public:
    using Value = std::variant<std::monostate, std::string, int64_t, double, bool>;

    Variant() = default;

    template <typename T>
    Variant(const T& v) {
        if constexpr (std::is_same_v<T, bool>) value = v;
        else if constexpr (std::is_integral_v<T>) value = static_cast<int64_t>(v);
        else if constexpr (std::is_floating_point_v<T>) value = static_cast<double>(v);
        else if constexpr (std::is_convertible_v<const T&, std::string>) value = std::string(v);
    }

    const Value& get() const { return value; }

private:
    Value value;
};
struct ObjectPath : std::string { using std::string::string; };
struct IConnection {};
struct IObject {};
}
#endif
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace services {

using namespace std::literals::string_literals;

using StringList = std::vector<std::string>;
using Metadata   = std::map<std::string, sdbus::Variant>;

static const auto PROPERTIES = "org.freedesktop.DBus.Properties"s;
static const auto MEDIAPLAYER2PLAYER = "org.mpris.MediaPlayer2.Player"s;
static const auto MEDIAPLAYER2 = "org.mpris.MediaPlayer2"s;
static const auto OBJECT_PATH = "/org/mpris/MediaPlayer2"s;
static const auto PREFIX = "org.mpris.MediaPlayer2."s;

enum class PlaybackStatus {
    Playing,
    Paused,
    Stopped
};

enum class LoopStatus {
    None,
    Track,
    Playlist
};

enum class Field {
    TrackId     , Length     , ArtUrl      , Album          ,
    AlbumArtist , Artist     , AsText      , AudioBPM       ,
    AutoRating  , Comment    , Composer    , ContentCreated ,
    DiscNumber  , FirstUsed  , Genre       , LastUsed       ,
    Lyricist    , Title      , TrackNumber , Url            ,
    UseCount    , UserRating
};

static const char *playback_status_strings[] = {
    "Playing",
    "Paused",
    "Stopped"
};

static const char *loop_status_strings[] = {
    "None",
    "Track",
    "Playlist"
};

static const char *metadata_strings[] = { 
    "mpris:trackid"     , "mpris:length"        , "mpris:artUrl"       , "xesam:album"           ,
    "xesam:albumArtist" , "xesam:artist"        , "xesam:asText"       , "xesam:audioBPM"        ,
    "xesam:autoRating"  , "xesam:comment"       , "xesam:composer"    , "xesam:contentCreated" ,
    "xesam:discNumber" , "xesam:firstUsed"    , "xesam:genre"       , "xesam:lastUsed"       ,
    "xesam:lyricist"   , "xesam:title"        , "xesam:trackNumber" , "xesam:url"            ,
    "xesam:useCount"   , "xesam:userRating"
};

namespace detail {

template <typename T, typename R, typename... Args> std::function<R(Args...)> member_fn(T *obj, R (T::*fn)(Args...)) {
    return [=](Args&&... args) -> R { return (obj->*fn)(args...); };
}

template <typename T, typename R, typename... Args> std::function<R(Args...)> member_fn(T *obj, R (T::*fn)(Args...) const) {
    return [=](Args&&... args) -> R { return (obj->*fn)(args...); };
}

inline std::string playbackStatusToString(PlaybackStatus status) {
    return playback_status_strings[static_cast<int>(status)];
}

inline std::string loopStatusToString(LoopStatus status) {
    return loop_status_strings[static_cast<int>(status)];
}

inline std::string fieldToString(Field entry) { 
    return metadata_strings[static_cast<int>(entry)];
}

}

class Mpris {
private:
    std::unique_ptr<sdbus::IConnection> connection;
    std::unique_ptr<sdbus::IObject> object;

    std::function<void(void)> quit_fn;
    std::function<void(void)> next_fn;
    std::function<void(void)> previous_fn;
    std::function<void(void)> pause_fn;
    std::function<void(void)> play_pause_fn;
    std::function<void(void)> stop_fn;
    std::function<void(void)> play_fn;
    
    std::function<void(LoopStatus)> loop_status_changed_fn;
    std::function<void(int64_t)> setPosition_fn;
    std::function<void(int64_t)> seek_fn;

    std::function<void(bool)> shuffle_changed_fn;

    std::string service_name;
    std::string human_name;

    PlaybackStatus playback_status = PlaybackStatus::Stopped;
    LoopStatus loop_status = LoopStatus::None;
    Metadata metadata = {};

    int64_t position = 0;

    bool shuffle = false;
    bool is_next_possible;
    bool is_previous_possible;

    void changeProperty(const std::string &interface, const std::string &name, sdbus::Variant value);
    void changePropertyControlled(std::vector<std::string> args);

    bool canGoPrevious() const;
    bool canControl() const;
    bool canGoNext() const;
    bool canPause() const;
    bool canPlay() const;
    bool canSeek() const;

    void setPositionMethod(sdbus::ObjectPath id, int64_t pos);

#ifdef __APPLE__
    // Pushes the current state to MPNowPlayingInfoCenter. Main thread only.
    void publishNowPlaying();
#endif
    void setLoopStatusExternal(const std::string &value);
    void setShuffleExternal(bool value);

public:
    static std::unique_ptr<Mpris> make(std::string_view name);

    explicit Mpris(std::string_view player_name);
    
    void startLoopAsync();
    void startLoop();

    void setMetadata(const std::map<Field, sdbus::Variant> &value);
    void setPlaybackStatus(PlaybackStatus value);

    void onLoopStatusChanged(std::function<void(LoopStatus)> fn);
    void onShuffleChanged(std::function<void(bool)> fn);

    void onPrevious(std::function<void(void)> fn);
    void onToggle(std::function<void(void)> fn);
    void onPause(std::function<void(void)> fn);
    void onNext(std::function<void(void)> fn);
    void onStop(std::function<void(void)> fn);
    void onPlay(std::function<void(void)> fn);
    void onQuit(std::function<void(void)> fn);

    void onSetPosition(std::function<void(int64_t)> fn);
    void onSeek(std::function<void(int64_t)> fn);

    void setHumanName(std::string_view value);
    void setLoopStatus(LoopStatus value);
    void setPosition(int64_t value);
    void setShuffle(bool value);

    void sendSeekedSignal(int64_t position);

    void setIsNextPossible(bool possible);
    void setIsPreviousPossible(bool possible);
    void updatePlayerControls();
};

#ifdef __APPLE__
/* pumpMainThread
- macOS delivers media keys / Control Center commands to the main thread's run
  loop, which the TUI otherwise never yields; call this from the UI loop
- Waits at most `seconds` for events, so it doubles as the loop's idle sleep
*/
void pumpMainThread(double seconds);

/* runMainLoop
- Parks the main thread in its run loop, where macOS delivers media keys and
  Control Center commands; never returns. The UI runs on another thread.
*/
[[noreturn]] void runMainLoop();
#endif

}
