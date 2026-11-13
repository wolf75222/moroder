/* MPRIS, macOS flavour
- Same interface as mpris.cpp, backed by the MediaPlayer framework instead of
  D-Bus: track info shows up in Control Center / the menu bar "Now Playing"
  widget, and the media keys, AirPods and Control Center buttons drive the player
- Commands are delivered on the main thread's run loop, which main.cpp pumps
  through services::pumpMainThread()
- Every state change is hopped onto the main queue, so the members below are
  only ever touched from one thread
*/

#import <AppKit/AppKit.h>
#import <CoreFoundation/CoreFoundation.h>
#import <MediaPlayer/MediaPlayer.h>

#include "../../include/services/mpris.hpp"

#include "spdlog/spdlog.h"

using namespace services;

namespace {

void onMain(void (^block)(void)) {
    dispatch_async(dispatch_get_main_queue(), block);
}

NSString* toNSString(const std::string& s) {
    return [NSString stringWithUTF8String:s.c_str()] ?: @"";
}

const sdbus::Variant* find(const Metadata& m, Field f) {
    auto it = m.find(detail::fieldToString(f));
    return it == m.end() ? nullptr : &it->second;
}

std::string stringField(const Metadata& m, Field f) {
    const sdbus::Variant* v = find(m, f);
    if (!v) return "";
    if (auto s = std::get_if<std::string>(&v->get())) return *s;
    return "";
}

int64_t intField(const Metadata& m, Field f) {
    const sdbus::Variant* v = find(m, f);
    if (!v) return 0;
    if (auto i = std::get_if<int64_t>(&v->get())) return *i;
    if (auto d = std::get_if<double>(&v->get())) return static_cast<int64_t>(*d);
    return 0;
}

// Artwork is fetched once per URL; the last one wins if tracks change quickly.
NSString* artwork_url = nil;
MPMediaItemArtwork* artwork = nil;

MPRemoteCommandHandlerStatus run(const std::function<void(void)>& fn) {
    if (!fn) return MPRemoteCommandHandlerStatusCommandFailed;
    fn();
    return MPRemoteCommandHandlerStatusSuccess;
}

}

void services::pumpMainThread(double seconds) {
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, seconds, true);
}

std::unique_ptr<Mpris> Mpris::make(std::string_view name) {
    return std::make_unique<Mpris>(name);
}

Mpris::Mpris(std::string_view player_name) : service_name(PREFIX + std::string(player_name)) {
    is_next_possible = false;
    is_previous_possible = false;

    onMain(^{
        MPRemoteCommandCenter* c = [MPRemoteCommandCenter sharedCommandCenter];

        [c.playCommand addTargetWithHandler:^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent*) {
            return run(play_fn);
        }];
        [c.pauseCommand addTargetWithHandler:^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent*) {
            return run(pause_fn);
        }];
        [c.togglePlayPauseCommand addTargetWithHandler:^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent*) {
            return run(play_pause_fn);
        }];
        [c.stopCommand addTargetWithHandler:^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent*) {
            return run(stop_fn);
        }];
        [c.nextTrackCommand addTargetWithHandler:^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent*) {
            return run(next_fn);
        }];
        [c.previousTrackCommand addTargetWithHandler:^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent*) {
            return run(previous_fn);
        }];

        // Scrubbing in Control Center: an absolute position, like MPRIS SetPosition.
        [c.changePlaybackPositionCommand addTargetWithHandler:^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent* e) {
            if (!setPosition_fn) return MPRemoteCommandHandlerStatusCommandFailed;
            auto* ev = (MPChangePlaybackPositionCommandEvent*)e;
            setPosition_fn(static_cast<int64_t>(ev.positionTime * 1000000.0));
            return MPRemoteCommandHandlerStatusSuccess;
        }];

        [c.changeRepeatModeCommand addTargetWithHandler:^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent* e) {
            if (!loop_status_changed_fn) return MPRemoteCommandHandlerStatusCommandFailed;
            auto* ev = (MPChangeRepeatModeCommandEvent*)e;
            LoopStatus status = LoopStatus::None;
            if (ev.repeatType == MPRepeatTypeOne) status = LoopStatus::Track;
            if (ev.repeatType == MPRepeatTypeAll) status = LoopStatus::Playlist;
            loop_status = status;
            loop_status_changed_fn(status);
            return MPRemoteCommandHandlerStatusSuccess;
        }];

        updatePlayerControls();
        spdlog::info("MPRIS: using macOS Now Playing");
    });
}

void Mpris::publishNowPlaying() {
    MPNowPlayingInfoCenter* center = [MPNowPlayingInfoCenter defaultCenter];

    const std::string title = stringField(metadata, Field::Title);

    if (title.empty()) {
        center.nowPlayingInfo = nil;
        center.playbackState = MPNowPlayingPlaybackStateStopped;
        return;
    }

    NSMutableDictionary* info = [NSMutableDictionary dictionary];
    info[MPMediaItemPropertyTitle] = toNSString(title);
    info[MPMediaItemPropertyArtist] = toNSString(stringField(metadata, Field::Artist));
    info[MPMediaItemPropertyAlbumTitle] = toNSString(stringField(metadata, Field::Album));
    info[MPMediaItemPropertyPlaybackDuration] = @(intField(metadata, Field::Length) / 1000000.0);
    info[MPNowPlayingInfoPropertyElapsedPlaybackTime] = @(position / 1000000.0);
    info[MPNowPlayingInfoPropertyPlaybackRate] = @(playback_status == PlaybackStatus::Playing ? 1.0 : 0.0);
    info[MPNowPlayingInfoPropertyMediaType] = @(MPNowPlayingInfoMediaTypeAudio);

    NSString* art = toNSString(stringField(metadata, Field::ArtUrl));
    if (artwork && [art isEqualToString:artwork_url]) {
        info[MPMediaItemPropertyArtwork] = artwork;
    }

    center.nowPlayingInfo = info;

    switch (playback_status) {
        case PlaybackStatus::Playing: center.playbackState = MPNowPlayingPlaybackStatePlaying; break;
        case PlaybackStatus::Paused:  center.playbackState = MPNowPlayingPlaybackStatePaused;  break;
        case PlaybackStatus::Stopped: center.playbackState = MPNowPlayingPlaybackStateStopped; break;
    }

    if (art.length == 0 || [art isEqualToString:artwork_url]) return;

    // New cover: fetch it, then publish again once it is there.
    artwork_url = art;
    artwork = nil;

    NSURL* url = [NSURL URLWithString:art];
    if (!url) return;

    [[[NSURLSession sharedSession] dataTaskWithURL:url completionHandler:^(NSData* data, NSURLResponse*, NSError* error) {
        if (error || !data) return;

        NSImage* image = [[NSImage alloc] initWithData:data];
        if (!image) return;

        onMain(^{
            if (![art isEqualToString:artwork_url]) return;   // track changed meanwhile
            artwork = [[MPMediaItemArtwork alloc] initWithBoundsSize:image.size
                                                      requestHandler:^NSImage*(CGSize) { return image; }];
            publishNowPlaying();
        });
    }] resume];
}

void Mpris::startLoopAsync() {}
void Mpris::startLoop() {}

void Mpris::setMetadata(const std::map<Field, sdbus::Variant>& value) {
    Metadata m;
    for (const auto& [field, v] : value) m[detail::fieldToString(field)] = v;

    onMain(^{
        metadata = m;
        publishNowPlaying();
    });
}

void Mpris::setPlaybackStatus(PlaybackStatus value) {
    onMain(^{
        playback_status = value;
        publishNowPlaying();
    });
}

void Mpris::setPosition(int64_t value) {
    onMain(^{
        position = value;
        publishNowPlaying();
    });
}

void Mpris::sendSeekedSignal(int64_t value) { setPosition(value); }

void Mpris::setLoopStatus(LoopStatus value) {
    onMain(^{
        loop_status = value;
        MPRemoteCommandCenter* c = [MPRemoteCommandCenter sharedCommandCenter];
        c.changeRepeatModeCommand.currentRepeatType =
            value == LoopStatus::Track    ? MPRepeatTypeOne :
            value == LoopStatus::Playlist ? MPRepeatTypeAll : MPRepeatTypeOff;
    });
}

void Mpris::setShuffle(bool value) { onMain(^{ shuffle = value; }); }
void Mpris::setHumanName(std::string_view value) { human_name = value; }

void Mpris::setIsNextPossible(bool possible) {
    onMain(^{ is_next_possible = possible; updatePlayerControls(); });
}

void Mpris::setIsPreviousPossible(bool possible) {
    onMain(^{ is_previous_possible = possible; updatePlayerControls(); });
}

void Mpris::updatePlayerControls() {
    onMain(^{
        MPRemoteCommandCenter* c = [MPRemoteCommandCenter sharedCommandCenter];
        c.nextTrackCommand.enabled = is_next_possible;
        c.previousTrackCommand.enabled = is_previous_possible;
    });
}

void Mpris::onLoopStatusChanged(std::function<void(LoopStatus)> fn) { loop_status_changed_fn = std::move(fn); }
void Mpris::onShuffleChanged(std::function<void(bool)> fn) { shuffle_changed_fn = std::move(fn); }

void Mpris::onPrevious(std::function<void(void)> fn) { previous_fn = std::move(fn); }
void Mpris::onToggle(std::function<void(void)> fn) { play_pause_fn = std::move(fn); }
void Mpris::onPause(std::function<void(void)> fn) { pause_fn = std::move(fn); }
void Mpris::onNext(std::function<void(void)> fn) { next_fn = std::move(fn); }
void Mpris::onStop(std::function<void(void)> fn) { stop_fn = std::move(fn); }
void Mpris::onPlay(std::function<void(void)> fn) { play_fn = std::move(fn); }
void Mpris::onQuit(std::function<void(void)> fn) { quit_fn = std::move(fn); }

void Mpris::onSetPosition(std::function<void(int64_t)> fn) { setPosition_fn = std::move(fn); }
void Mpris::onSeek(std::function<void(int64_t)> fn) { seek_fn = std::move(fn); }
