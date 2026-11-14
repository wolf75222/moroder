#include "../../include/services/player.hpp"

#include "../../include/config/config.hpp"
#include "../../include/services/notify.hpp"
#include "../../include/services/scrobble.hpp"
#include "../../include/utils/json_str.hpp"
#include "../../include/utils/utils.hpp"
#include <fstream>

#include <spdlog/spdlog.h>
#include <unordered_map>
#include <unordered_set>
#include <csignal>
#include <unistd.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <spawn.h>
#include <regex>
#include <nlohmann/json.hpp>
#include <curl/curl.h>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <random>
#include <chrono>
#include <thread>
#include <algorithm>
#include <type_traits>
#include <sys/stat.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <mutex>

namespace {

/* LIBRARY_SOURCES
- The library page is one view fed by five independent requests
- Each request keeps its own error entry, and "library" is the aggregate the UI
  reads, so a source that succeeds can't erase a sibling's failure
*/
const char* LIBRARY_SOURCES[] = {
    "library_playlists",
    "library_albums",
    "library_songs",
    "library_artists",
    "library_podcasts",
};

/* loopStatusFor / loopModeFor
- MPRIS only knows None, Track and Playlist, so a queue loop goes out as
  Playlist
- The difference between the two is what happens to the radio queue, which
  MPRIS has no concept of, so nothing is lost in the mapping
*/
services::LoopStatus loopStatusFor(services::Player::LoopMode mode) {
    switch (mode) {
        case services::Player::LoopMode::Track: return services::LoopStatus::Track;
        case services::Player::LoopMode::Queue: return services::LoopStatus::Playlist;
        default:                                return services::LoopStatus::None;
    }
}

services::Player::LoopMode loopModeFor(services::LoopStatus status) {
    switch (status) {
        case services::LoopStatus::Track:    return services::Player::LoopMode::Track;
        case services::LoopStatus::Playlist: return services::Player::LoopMode::Queue;
        default:                             return services::Player::LoopMode::None;
    }
}

const char* loopModeName(services::Player::LoopMode mode) {
    switch (mode) {
        case services::Player::LoopMode::Track: return "track";
        case services::Player::LoopMode::Queue: return "queue";
        default:                                return "off";
    }
}

/* refreshLibraryError
- Recomputes the aggregate "library" entry from the per-source ones
- First failure wins, since the page can only show one error box
- The caller must already hold state_mutex
*/
void refreshLibraryError(services::Player::PlayerState& state) {
    for (const char* source : LIBRARY_SOURCES) {
        auto error = state.errors.find(source);

        if (error != state.errors.end()) {
            /* Copy before writing: operator[] can rehash, which would leave
            error->second dangling while it is still being read from
            */
            const services::Player::ErrorInfo info = error->second;

            spdlog::warn("PLAYER: library error held by {}, {}", source, info.message);

            state.errors["library"] = info;
            return;
        }
    }

    state.errors.erase("library");
}

}

namespace {

// Next to the logs, e.g. ~/.local/state/moroder/volume
std::filesystem::path volumeFile() {
    return utils::resolve_path(MORODER_LOG_PATH).parent_path() / "volume";
}

}

extern char** environ;

namespace {

std::filesystem::path cachePath(const std::string& name) {
    const char* home = std::getenv("HOME");
    if (!home) return {};
    return std::filesystem::path(home) / ".cache" / "moroder" / name;
}

nlohmann::json readJson(const std::string& name) {
    std::ifstream in(cachePath(name));
    if (!in.is_open()) return nlohmann::json();
    nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
    return j.is_discarded() ? nlohmann::json() : j;
}

void writeJson(const std::string& name, const nlohmann::json& j) {
    const auto file = cachePath(name);
    if (file.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    const auto tmp = file.string() + ".tmp";
    { std::ofstream out(tmp, std::ios::trunc); if (!out.is_open()) return; out << j.dump(); }
    std::filesystem::rename(tmp, file, ec);
}

const char* RECENT_SHELF = "Recently played";

}

namespace {

void prestartResolvers();

// Still usable for a while (stream URLs carry their expiry, unix time)?
bool streamFresh(const std::string& stream, long long margin_s = 120) {
    const size_t e = stream.find("expire=");
    if (e == std::string::npos) return true;
    const long long expire = std::atoll(stream.c_str() + e + 7);
    const long long now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    return expire <= 0 || expire >= now + margin_s;
}


/* YouTube refuses a stream URL (403) for ~2-3 s after issuing it. It expires
6 h after issue, which dates it: wait until issue + 3 s (seconds are rounded).
*/
std::chrono::milliseconds streamNotReadyFor(const std::string& stream) {
    const size_t e = stream.find("expire=");
    if (e == std::string::npos) return std::chrono::milliseconds(0);
    const long long expire = std::atoll(stream.c_str() + e + 7);
    const long long issued_ms = (expire - 6 * 3600) * 1000;
    const long long now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const long long wait = issued_ms + 3000 - now_ms;
    return std::chrono::milliseconds(std::clamp(wait, 0LL, 3000LL));
}

// 1-byte request: does the server accept this stream URL yet?
bool streamAccepted(const std::string& stream) {
    const size_t nl = stream.find('\n');
    const std::string url = stream.substr(0, nl);
    std::string ua = nl == std::string::npos ? "" : stream.substr(nl + 1);
    ua = ua.substr(0, ua.find('\n'));

    CURL* curl = curl_easy_init();
    if (!curl) return true;     // can't tell: let mpv try
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_RANGE, "0-0");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 3000L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +[](char*, size_t s, size_t n, void*) -> size_t { return s * n; });
    if (!ua.empty()) curl_easy_setopt(curl, CURLOPT_USERAGENT, ua.c_str());
    long code = 0;
    const CURLcode rc = curl_easy_perform(curl);
    if (rc == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(curl);
    return rc != CURLE_OK || code != 403;    // network trouble: let mpv try
}

/* Until the stream URL is accepted (at most ~8 s), probing from issue + 2 s
every 350 ms. stop() ends it early (the track was replaced).
*/
template <typename Stop>
void waitStreamReady(const std::string& stream, Stop stop) {
    const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    auto first = streamNotReadyFor(stream) - std::chrono::milliseconds(1000);
    if (first.count() > 0) std::this_thread::sleep_for(first);
    while (std::chrono::steady_clock::now() < give_up && !stop()) {
        if (streamAccepted(stream)) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(350));
    }
}

}


services::Player::Player(const std::string_view& app_name, const std::string_view& app_name_human, const uint64_t app_id) {
    this->app_name = app_name;

    mpris_service = Mpris::make(this->app_name);
    mpv_service = std::make_unique<MPV>();

    // Restore the volume from the last session.
    {
        std::ifstream in(volumeFile());
        int saved = -1;
        if (in >> saved && saved >= 0 && saved <= 100) {
            state.volume = saved;
            mpv_service->setVolume(saved);
        }
    }
    music_service = std::make_unique<Music>(this->app_name);

    // Python + yt-dlp loaded now, not when the first track is asked for.
    std::thread(prestartResolvers).detach();

    social_service = std::make_unique<Social>(app_id, Config::get().ENABLE_DISCORD_RICH_PRESENCE);

    if (!mpris_service) {
        spdlog::error("PLAYER: mpris service initialisation failed.");
        return;
    }

    /* Seed the page from the cache, so the first frame already shows the last
    home and playlists; the fresh copies replace them when they land.
    */
    if (music_service) {
        auto cached_home = music_service->getHome(true);
        auto cached_lists = music_service->getLibraryPlaylists(true);
        std::lock_guard lock(state_mutex);
        if (!cached_home.empty()) {
            state.home = std::move(cached_home);
            state.home_order = music_service->homeOrder();
            state.flags["home"] = Flags::Done;
        }
        if (!cached_lists.empty()) {
            state.library_playlists = std::move(cached_lists);
            state.flags["library_playlists"] = Flags::Done;
        }
        mergeHistoryShelf();
        spdlog::info("PLAYER: cache, {} home shelves, {} playlists", state.home.size(), state.library_playlists.size());
    }

    /* Session from the browser (COOKIE_BROWSER): fresh at launch, then every
    10 minutes, so it never goes stale while the browser stays signed in.
    */
    if (music_service && !Config::get().COOKIE_BROWSER.empty() && !Config::anonymous) {
        // First in the queue: the UI (and the cache) show at once, while the
        // network requests queued after it wait for the fresh session. A
        // Keychain prompt no longer holds the window back.
        {
            std::lock_guard lock(command_mutex);
            command_queue.push(RefreshSessionCommand{});
        }

        // Every 10 minutes, through the worker like everything else that
        // writes browser.json / cookies.txt, so writes never overlap.
        std::thread([this] {
            while (true) {
                std::this_thread::sleep_for(std::chrono::minutes(10));
                {
                    std::lock_guard lock(command_mutex);
                    command_queue.push(RefreshSessionCommand{});
                }
                command_cv.notify_one();
            }
        }).detach();
    }

    if (!music_service) {
        spdlog::error("PLAYER: music service initialisation failed.");
        return;
    }

    if (!social_service) {
        spdlog::error("PLAYER: social service initialisation failed.");
        return;
    }

    this->initialised = true;

    worker_thread = std::thread(&Player::worker_loop, this);

    {
        std::lock_guard lock(state_mutex);
        state.flags["audio"] = Flags::Done;
        state.flags["search"] = Flags::Done;
    }

    mpris_service->setHumanName(app_name_human);
    mpris_service->onQuit([&] {});

    mpris_service->onPause([&] {
        {
            std::lock_guard lock(state_mutex);
            state.flags["audio"] = Flags::Done;
            state.flags["paused"] = Flags::True;
        }

        mpv_service->pause();
        social_service->pause();

        mpris_service->setPosition(mpv_service->getStreamPosition());
        mpris_service->setPlaybackStatus(services::PlaybackStatus::Paused);
    });

    // Media keys / AirPods / Control Center play-pause. This used to flip the
    // flags and then send mpv the opposite of what they said (pause when it
    // meant play), so the first press did nothing and the state went inverted.
    mpris_service->onToggle([this] {
        togglePause();
    });

    mpris_service->onStop([&] {
        this->stopPositionTick();

        {
            std::lock_guard lock(state_mutex);
            state.flags["audio"] = Flags::Done;
        }

        mpv_service->stop();
        social_service->removeStatus();
        mpris_service->setPlaybackStatus(services::PlaybackStatus::Stopped);
        this->resetMprisData();
    });

    mpris_service->onPlay([&] {
        {
            std::lock_guard lock(state_mutex);
            state.flags["audio"] = Flags::Ongoing;
            state.flags["paused"] = Flags::False;
        }

        mpv_service->resume();
        social_service->resume();
        social_service->setPosition(mpv_service->getStreamPosition());

        mpris_service->setPlaybackStatus(services::PlaybackStatus::Playing);
    });

    mpris_service->onSeek([&] (int64_t p) {
        seekBy(p);
    });

    mpris_service->onSetPosition([&] (int64_t p) {
        mpv_service->setPosition(p);

        social_service->setPosition(p);
        mpris_service->setPosition(p);
    });

    /* The property has already been written by the time this runs, so the
    change is applied without notifying back: doing both emits two
    PropertiesChanged for one change, and playerctl reports the second as a
    spurious update.
    */
    mpris_service->onLoopStatusChanged([this] (services::LoopStatus status) {
        this->applyLoop(loopModeFor(status), false);
    });

    mpris_service->onShuffleChanged([&] (bool shuffle) { });

    mpris_service->onNext([this](){
        skipForward();
    });

    mpris_service->onPrevious([this](){
        skipBackward();
    });

    mpris_service->setIsNextPossible(false);
    mpris_service->setIsPreviousPossible(false);

    // Publishes LoopStatus and clears mpv's loop-file, so the three agree from
    // the start rather than only after the first change.
    this->applyLoop(this->loopMode(), true);

    mpris_service->startLoopAsync();

    mpv_service->setOnStreamStart([this] {
        advancing = false;
        // A new file: older tick readings no longer count, nothing is being
        // scrobbled until we know it is a song, a failed link may be refreshed again.
        ++file_generation;
        scrobble_done = true;
        refreshed_url.clear();

        // Last.fm: now playing; the scrobble comes from the position tick.
        if (services::scrobble::enabled()) {
            std::lock_guard lock(state_mutex);
            if (auto song = std::dynamic_pointer_cast<music::Song>(state.current)) {
                scrobble_track = { song->ref.artists.empty() ? "" : song->ref.artists[0].name, song->ref.title, song->album.title };
                scrobble_started_at = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                scrobble_played_s = 0;
                scrobble_done = false;
                services::scrobble::nowPlaying(scrobble_track[0], scrobble_track[1], scrobble_track[2], 0);
            }
        }

        // After a fade out, the new track fades in; a skip mid-fade ends it.
        if (fading_out.exchange(false)) {
            const uint64_t gen = ++fade_generation;
            const int fade_s = std::max(1, Config::get().FADE_SECONDS);
            std::thread([this, gen, fade_s] {
                const int steps = fade_s * 20;
                for (int i = 1; i <= steps && fade_generation == gen; i++) {
                    int vol;
                    { std::lock_guard sl(state_mutex); vol = state.volume; }
                    mpv_service->setVolume(vol * i / steps);
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                int vol;
                { std::lock_guard sl(state_mutex); vol = state.volume; }
                if (fade_generation == gen) mpv_service->setVolume(vol);
            }).detach();
        }

        spdlog::info("PLAYER: Stream start");
        uint64_t song_duration = mpv_service->getStreamDuration();
        {
            std::lock_guard lock(state_mutex);
            if (state.current) state.current->setDuration(song_duration);
            state.flags["audio"] = Flags::Ongoing;
            state.flags["paused"] = Flags::False;
        }

        mpris_service->setPlaybackStatus(services::PlaybackStatus::Playing);
        this->updateMprisControls();
        this->updateMprisData();
        this->updateSocialData();
        this->startPositionTick();
        this->preloadNext();

        // Resuming a saved session: back to where it was, paused; only for the
        // restored track (another one started first gets nothing of it).
        {
            std::string resumed;
            bool other_first = false;
            double at = -1;
            {
                std::lock_guard lock(state_mutex);
                auto s = std::dynamic_pointer_cast<music::Song>(state.current);
                at = resume_at;
                if (s && at >= 0 && s->ref.id == resume_id) resumed = s->ref.id;
                other_first = at >= 0 && resumed.empty();
            }
            if (!resumed.empty()) {
                mpv_service->setPosition(static_cast<uint64_t>(at * 1000000.0));
                this->pause();
                // Paused, so no tick would come: show the time now (it said "loading").
                if (on_position_tick) on_position_tick(static_cast<uint64_t>(at * 1000000.0), mpv_service->getStreamDuration());
            } else if (other_first) {
                mpv_service->resume();      // the restore had paused mpv
            }
            std::lock_guard lock(state_mutex);
            resume_at = -1;
            resume_id.clear();
        }

        {
            std::shared_ptr<music::Song> song;
            {
                std::lock_guard lock(state_mutex);
                song = std::dynamic_pointer_cast<music::Song>(state.current);
            }
            if (song) recordHistory(song->ref);

            // Lyrics for the Now Playing view, fetched off the player thread.
            if (song && Config::get().ENABLE_LYRICS) {
                const std::string id = song->ref.id, title = song->ref.title;
                const std::string artist = song->ref.artists.empty() ? "" : song->ref.artists[0].name;
                const double secs = static_cast<double>(song_duration) / 1000000.0;
                {
                    std::lock_guard lock(state_mutex);
                    state.lyrics.reset();
                    state.lyrics_id = id;
                    state.flags["lyrics"] = Flags::Ongoing;
                }
                std::thread([this, id, title, artist, secs] {
                    auto l = std::make_shared<services::Lyrics>(services::fetchLyrics(title, artist, secs));
                    std::lock_guard lock(state_mutex);
                    if (state.lyrics_id != id) return;      // track changed meanwhile
                    state.lyrics = l;
                    state.flags["lyrics"] = Flags::Done;
                }).detach();
            }
        }
        this->saveSession();

        if (on_stream_start) {
            on_stream_start();
        };
    });

    mpv_service->setOnStreamEnd([this] {
        spdlog::info("PLAYER: Stream end");

        {
            std::lock_guard lock(position_tick_mutex);
            position_tick_running = false;
        }
        position_tick_cv.notify_all();

        mpris_service->setPlaybackStatus(services::PlaybackStatus::Stopped);

        std::shared_ptr<music::IStreamable> radio_next;
        music::Radio state_radio;

        bool start_next_radio = false;
        bool to_radio_skip = false;
        bool should_skip = false;

        int loop_to = -1;

        {
            std::lock_guard lock(state_mutex);

            state.flags["audio"] = Flags::Done;

            const LoopMode loop = state.loop_mode;

            /* Track loop
            - mpv repeats the file itself, so reaching here at all means
              loop-file did not take; replay the same entry rather than
              letting the queue advance under it
            */
            if (loop == LoopMode::Track && !state.user_queue.empty()) {
                if (state.queue_position < 0) state.queue_position = 0;
                if (state.queue_position >= state.user_queue.size()) {
                    state.queue_position = state.user_queue.size() - 1;
                }

                loop_to = static_cast<int>(state.queue_position);
                state.current = state.user_queue[state.queue_position];

            } else {
                this->state.queue_position++;

                if (loop == LoopMode::Queue && !state.user_queue.empty()) {
                    if (state.queue_position >= state.user_queue.size()) {
                        state.queue_position = 0;
                        loop_to = 0;
                    }

                    state.current = state.user_queue[state.queue_position];
                    should_skip = (loop_to < 0);

                } else {
                    if(!this->state.radio_queue.empty() && state.queue_position >= state.user_queue.size()) {
                        to_radio_skip = true;

                        radio_next = state.radio_queue.front();
                        state.radio_queue.pop_front();
                        state.current = radio_next;
                    }

                    if(this->state.autoplay && this->state.radio_queue.empty()) {
                        start_next_radio = true;
                        state_radio = state.radio;
                    }

                    if (state.queue_position < state.user_queue.size()) {
                        state.current = state.user_queue[state.queue_position];
                        should_skip = true;
                    }
                }
            }
        }

        if(to_radio_skip) {
            {
                std::unique_lock lock(command_mutex);
                command_queue.push(QueueStreamableCommand{
                    .streamable=radio_next,
                    .fetch_album=false,
                    .start_radio=false,
                    .start_fresh=false,
                    .queue_in_radio=false,
                });
            }
            command_cv.notify_one();
        }

        // After the track that plays next: refilling the radio is a network
        // fetch and used to delay the next track's start.
        if(start_next_radio) {
            {
                std::unique_lock lock(command_mutex);
                command_queue.push(QueueNextRadioCommand{
                    .radio=state_radio,
                    .auto_play=true
                });
            }

            command_cv.notify_one();
        }

        if(loop_to >= 0) {
            spdlog::info("PLAYER: loop, restarting the queue at index {}", loop_to);
            mpv_service->skipTo(loop_to);
        }

        advancing = should_skip || to_radio_skip || loop_to >= 0;

        /* If should skip:
        - We don't actually call the skip method because mpv will autoplay by itself
        */
        if(should_skip || to_radio_skip || loop_to >= 0) {
            this->updateMprisControls();
            this->updateMprisData();
            this->updateSocialData();

        } else {
            this->resetMprisData();
        }

        if (on_stream_end) {
            on_stream_end();
        };
    });

    mpv_service->setOnStreamError([this] {
        spdlog::warn("PLAYER: Stream error");

        this->stopPositionTick();

        /* Drop the track that failed, not the last one (pop_back did), and keep
        the position: mpv has already moved on to the entry that slides into
        this index, so the queue and mpv's playlist stay in step.
        */
        std::lock_guard lock(state_mutex);
        const int pos = state.queue_position;
        if (pos < 0 || static_cast<size_t>(pos) >= state.user_queue.size()) return;

        /* A pre-resolved stream URL that failed (expired, refused): play the
        track through its YouTube URL instead (mpv's hook resolves it), once.
        */
        const std::string failed = mpv_service->entryUrl(static_cast<uint16_t>(pos));
        const std::string youtube = state.user_queue[pos]->getStreamUrl();

        // Refused because it is still fresh: the same URL again in 2 s, once.
        // A link old enough to be past YouTube's first seconds won't get better
        // (saved from another network, revoked): get a fresh one at once.
        static std::string retried;
        std::string& refreshed = refreshed_url;
        const bool is_stream = failed.find("googlevideo.com") != std::string::npos;
        if (is_stream) forgetStream(youtube);
        if (is_stream && failed != retried && streamNotReadyFor(failed).count() == 0 && refreshed != youtube) retried = failed;

        if (is_stream && failed == retried && refreshed != youtube && !youtube.empty()) {
            refreshed = youtube;
            spdlog::warn("PLAYER: stream URL refused again, getting a fresh one");
            mpv_service->park();    // mpv moved on to the next entry: hold it until the retry
            std::thread([this, pos, failed, youtube] {
                const std::string fresh = resolveStream(youtube, true);
                if (!fresh.empty()) waitStreamReady(fresh, [] { return false; });
                std::lock_guard lock(state_mutex);
                if (state.queue_position != pos || static_cast<size_t>(pos) >= state.user_queue.size()
                    || state.user_queue[pos]->getStreamUrl() != youtube) return;
                if (mpv_service->entryUrl(static_cast<uint16_t>(pos)) != failed) return;
                mpv_service->insertAt(fresh.empty() ? youtube : fresh, static_cast<uint16_t>(pos));   // else the hook
                mpv_service->removeAt(static_cast<uint16_t>(pos + 1));
                mpv_service->skipTo(static_cast<uint16_t>(pos));
            }).detach();
            return;
        }

        if (is_stream && failed != retried) {
            retried = failed;
            spdlog::warn("PLAYER: stream URL refused, retrying it shortly");
            mpv_service->park();    // mpv moved on to the next entry: hold it until the retry
            std::thread([this, pos, failed, youtube] {
                std::this_thread::sleep_for(std::max(std::chrono::milliseconds(800), streamNotReadyFor(failed)));
                std::lock_guard lock(state_mutex);
                if (state.queue_position != pos || static_cast<size_t>(pos) >= state.user_queue.size()
                    || state.user_queue[pos]->getStreamUrl() != youtube) return;
                if (mpv_service->entryUrl(static_cast<uint16_t>(pos)) != failed) return;
                mpv_service->skipTo(static_cast<uint16_t>(pos));
            }).detach();
            return;
        }

        if (failed.find("googlevideo.com") != std::string::npos && !youtube.empty() && failed != youtube) {
            spdlog::warn("PLAYER: stream URL refused, falling back to the YouTube URL");
            mpv_service->insertAt(youtube, static_cast<uint16_t>(pos));
            mpv_service->removeAt(static_cast<uint16_t>(pos + 1));
            mpv_service->skipTo(static_cast<uint16_t>(pos));
            return;
        }

        state.user_queue.erase(state.user_queue.begin() + pos);
        mpv_service->removeAt(static_cast<uint16_t>(pos));

        if (static_cast<size_t>(pos) < state.user_queue.size()) {
            state.current = state.user_queue[pos];
        } else {
            state.queue_position = static_cast<int>(state.user_queue.size()) - 1;
            state.flags["audio"] = Flags::Done;
        }

        state.notice = "Couldn't play that track, skipped it";
        state.notice_until = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    });
}

void services::Player::worker_loop() {
    while (true) {
        std::unique_lock lock(command_mutex);

        command_cv.wait(lock, [this] {
            return !command_queue.empty() || !running;
        });

        // On shutdown, pending work is dropped: draining it could mean a
        // thousand queued tracks (a big playlist) and minutes of network calls
        // while the music keeps playing after the UI is gone.
        if (!running) {
            break;
        }

        Command cmd = std::move(command_queue.front());
        command_queue.pop();

        lock.unlock();

        if (load_pending) {
            const bool postpone = std::visit([](auto&& c) -> bool {
                using T = std::decay_t<decltype(c)>;
                if constexpr (std::is_same_v<T, QueueStreamableCommand>) return !c.start_fresh && !c.queue_in_radio;
                else if constexpr (std::is_same_v<T, PreloadedCommand>) return c.index != SIZE_MAX;
                else if constexpr (std::is_same_v<T, PlayNextCommand> || std::is_same_v<T, MoveCommand> || std::is_same_v<T, ShuffleCommand>) return true;
                else return false;
            }, cmd);
            if (postpone) {
                postponed.push_back(std::move(cmd));
                continue;
            }
        }

        std::visit([this](auto&& c) {
            using T = std::decay_t<decltype(c)>;

            if constexpr (std::is_same_v<T, SearchCommand>) {
                spdlog::info("PLAYER: SearchCommand");

                {
                    std::lock_guard lock(state_mutex);
                    state.flags["search"] = Flags::Ongoing;
                    state.errors.erase("search");
                    state.search_results.clear();
                }

                std::vector<music::ApiResult> res = music_service->getSearch(c.query);
                const services::RequestError& err = music_service->lastError();

                {
                    std::lock_guard lock(state_mutex);

                    if (err.failed && !err.cancelled) {
                        state.flags["search"] = Flags::Error;
                        state.errors["search"] = { err.message, err.retryable };

                    } else if (!err.failed) {
                        state.flags["search"] = Flags::Done;
                        state.search_results = res;
                    }
                }
            }

            else if constexpr (std::is_same_v<T, QueueStreamableCommand>) {
                spdlog::info("PLAYER: QueueStreamableCommand");

                if (!c.streamable) {
                    spdlog::warn("PLAYER: QueueStreamableCommand, streamable is nullptr");
                    return;
                }

                bool should_queue = false;
                bool is_queue_empty = false;

                {
                    std::lock_guard lock(state_mutex);
                    // Something is loaded (playing, or paused) -> append behind it.
                    // Checking only "playing" made a track added while paused
                    // become the current one in the UI while mpv kept the old one.
                    should_queue = state.flags["audio"] == Flags::Ongoing || state.flags["paused"] == Flags::True
                                // Between two tracks (the next one still opening): still in use.
                                || advancing;
                    is_queue_empty = state.user_queue.empty() && state.radio_queue.empty();
                }

                std::shared_ptr<music::IStreamable> streamable;
                bool fetch_album_later = false;

                if (auto song_ptr = std::dynamic_pointer_cast<music::Song>(c.streamable)) {
                    music::Song song;

                    if (c.fetch_album) {
                        // Fetched after the track is loaded (below): the sound
                        // doesn't wait for an album lookup.
                        song.setRef(song_ptr->ref);
                        fetch_album_later = true;

                    } else if(!song_ptr->album.id.empty() && !song_ptr->album.title.empty()) {
                        spdlog::info("PLAYER: QueueStreamableCommand, album already provided");
                        song = *song_ptr;

                    } else {
                        spdlog::info("PLAYER: QueueStreamableCommand, skipping album data");
                        song.setRef(song_ptr->ref);
                    }

                    streamable = std::make_shared<music::Song>(song);

                } else if (auto episode_ptr = std::dynamic_pointer_cast<music::Episode>(c.streamable)) {
                    music::Episode episode = music_service->getEpisode(episode_ptr->ref);
                    streamable = std::make_shared<music::Episode>(episode);

                } else {
                    spdlog::warn("PLAYER: QueueStreamableCommand, unknown streamable type");
                    return;
                }

                // A late playlist rest for a queue that has been replaced since.
                if (c.epoch != 0 && c.epoch != queue_epoch) return;

                if(c.start_fresh) {
                    ++queue_epoch;
                    // A single track replaces the queue: no playlist rest to add.
                    if (!c.from_container) { std::lock_guard lock(state_mutex); queued_from_id.clear(); }

                    // A track still resolving for the old queue is dropped.
                    load_pending = false;
                    load_generation++;
                    postponed.clear();

                    mpv_service->clearQueue();
                    mpris_service->setPlaybackStatus(services::PlaybackStatus::Stopped);
                    this->resetMprisData();

                    {
                        std::lock_guard lock(state_mutex);
                        state.user_queue.clear();
                        state.radio_queue.clear();
                        state.queue_position = 0;
                        state.current = streamable;
                    }
                    should_queue = false;
                }

                if(c.start_radio) {
                    {
                        std::lock_guard lock(command_mutex);
                        command_queue.push(QueueStreamableRadioCommand{streamable});
                    }
                    command_cv.notify_one();
                }

                std::string defer_url;
                {
                    std::lock_guard lock(state_mutex);

                    if(c.queue_in_radio) {
                        state.radio_queue.push_back(streamable);

                    } else {
                        state.user_queue.push_back(streamable);
                    }

                    if(!should_queue && !c.queue_in_radio) {
                        state.queue_position = state.user_queue.size() - 1;
                        state.flags["audio"] = Flags::Ongoing;
                        state.current = state.user_queue.back();
                    }

                    if(!c.queue_in_radio) {
                        /* A resolved stream only for what plays soon (links expire
                        in hours): entries further down stay YouTube URLs until
                        preloadNext swaps them in.
                        */
                        const std::string yt = streamable->getStreamUrl();
                        const size_t index = state.user_queue.size() - 1;
                        const bool soon = state.queue_position < 0 || index <= static_cast<size_t>(state.queue_position) + 2;
                        const std::string load_url = soon ? streamUrlFor(yt) : yt;

                        /* The track that plays now: resolved off this thread (~2.5 s)
                        rather than through mpv's ytdl hook (~9 s). Only on a fresh
                        start: mpv's playlist is empty then, so the deferred load can
                        rebuild it in step with the queue.
                        */
                        if (!should_queue && c.start_fresh && load_url == yt && yt.find("youtube.com/watch") != std::string::npos) {
                            defer_url = yt;
                        } else {
                            // Under the same lock as the push: mpv's playlist keeps the queue's order.
                            mpv_service->load(load_url);
                        }
                    }
                }
                if (!defer_url.empty()) resolveCurrentAsync(defer_url);

                if (fetch_album_later) {
                    if (auto queued = std::dynamic_pointer_cast<music::Song>(streamable)) {
                        spdlog::info("PLAYER: QueueStreamableCommand, fetching album data");
                        const music::Song full = music_service->getSong(queued->ref);
                        std::lock_guard lock(state_mutex);
                        queued->album = full.album;
                    }
                }

                if(should_queue) {
                    this->updateMprisControls();
                }
            }

            else if constexpr (std::is_same_v<T, QueueStreamableContainerCommand>) {
                spdlog::info("PLAYER: QueueStreamableContainerCommand");

                if (!c.container) {
                    spdlog::warn("PLAYER: QueueStreamableContainerCommand, container is nullptr");
                    return;
                }

                std::shared_ptr<music::IStreamableContainer> container;

                if (auto album_ptr = std::dynamic_pointer_cast<music::Album>(c.container)) {
                    music::Album album = music_service->getAlbum(album_ptr->ref);
                    container = std::make_shared<music::Album>(album);

                } else if (auto playlist_ptr = std::dynamic_pointer_cast<music::Playlist>(c.container)) {
                    // Already loaded (the playlist view, or "play from here"): use
                    // those tracks as they are instead of fetching them again.
                    std::shared_ptr<music::Playlist> cached, first_page;
                    {
                        std::lock_guard lock(state_mutex);
                        auto it = playlist_cache.find(playlist_ptr->ref.id);
                        if (it != playlist_cache.end()) cached = it->second;
                        auto fp = playlist_first_page.find(playlist_ptr->ref.id);
                        if (fp != playlist_first_page.end()) first_page = fp->second;
                    }
                    if (!cached && first_page && playlist_ptr->getStreamables().empty()) {
                        cached = first_page;      // the rest follows
                        const size_t have = first_page->getStreamables().size();
                        std::lock_guard lock(command_mutex);
                        command_queue.push(PlaylistRestCommand{playlist_ptr->ref, have, c.start_fresh ? SIZE_MAX : have, queue_epoch.load()});
                    }
                    if (!playlist_ptr->getStreamables().empty()) {
                        container = playlist_ptr;
                    } else if (cached) {
                        container = cached;      // fetched when it was selected
                    } else {
                        // Starts on the first page; the rest is appended when it lands.
                        bool more = false;
                        music::Playlist playlist = music_service->getPlaylist(playlist_ptr->ref, true, &more);
                        const size_t have = playlist.getStreamables().size();
                        container = std::make_shared<music::Playlist>(playlist);
                        if (more) {
                            // Played: queued_from_* picks the rest up. Added to the queue:
                            // the rest is appended after this first page.
                            std::lock_guard lock(command_mutex);
                            command_queue.push(PlaylistRestCommand{playlist_ptr->ref, have, c.start_fresh ? SIZE_MAX : have, queue_epoch.load()});
                        }
                    }

                } else {
                    spdlog::warn("PLAYER: QueueStreamableContainerCommand, unknown streamable type");
                    return;
                }

                // A fresh start from a playlist: its missing tracks (still loading)
                // join the queue when they arrive. queued_from_have counts from
                // the playlist's start, so "play from track 40" lines up too.
                std::vector<std::shared_ptr<music::IStreamable>> items = container->getStreamables();
                if (c.start_fresh) {
                    std::lock_guard lock(state_mutex);
                    auto pl = std::dynamic_pointer_cast<music::Playlist>(container);
                    queued_from_id = pl ? pl->ref.id : "";
                    queued_from_have = pl ? items.size() + c.skip : 0;

                    // Its rest landed between Play and now: take it along.
                    if (pl) {
                        auto it = playlist_cache.find(pl->ref.id);
                        if (it != playlist_cache.end()) {
                            const auto all = it->second->getStreamables();
                            for (size_t i = queued_from_have; i < all.size(); i++) items.push_back(all[i]);
                            queued_from_have = std::max(queued_from_have, all.size());
                        }
                    }
                }

                /* Its tracks go before anything already waiting (home, library
                loads at startup): what the user asked to play comes first.
                */
                std::queue<Command> mine;
                for(auto streamable : items) {
                    mine.push(QueueStreamableCommand{
                        .streamable=streamable,
                        .fetch_album=false,
                        .start_radio=false,
                        .start_fresh=c.start_fresh,
                        .queue_in_radio=false,
                        .from_container=true
                    });
                    c.start_fresh = false;
                }
                if(c.start_radio) mine.push(QueueStreamableContainerRadioCommand{container});
                {
                    std::lock_guard lock(command_mutex);
                    while (!command_queue.empty()) { mine.push(std::move(command_queue.front())); command_queue.pop(); }
                    command_queue.swap(mine);
                }
                command_cv.notify_one();
            }

            else if constexpr (std::is_same_v<T, QueueStreamableRadioCommand>) {
                spdlog::info("PLAYER: QueueStreamableRadioCommand");

                music::Radio radio;

                if (auto song_ptr = std::dynamic_pointer_cast<music::Song>(c.streamable)) {
                    radio = music_service->getRadio(song_ptr->ref);

                } else if (auto episode_ptr = std::dynamic_pointer_cast<music::Episode>(c.streamable)) {
                    radio = music_service->getRadio(episode_ptr->ref);

                } else {
                    spdlog::warn("PLAYER: QueueStreamableRadioCommand, unknown streamable type");
                    return;
                }

                {
                    std::lock_guard lock(state_mutex);
                    this->state.radio = radio;
                    this->state.radio_queue.clear();
                }

                std::shared_ptr<music::IStreamableContainer> container = std::make_shared<music::Radio>(radio);

                for(auto streamable : container->getStreamables()) {
                    {
                        std::lock_guard lock(command_mutex);
                        command_queue.push(QueueStreamableCommand{
                            .streamable=streamable,
                            .fetch_album=false,
                            .start_radio=false,
                            .start_fresh=false,
                            .queue_in_radio=true
                        });
                    }
                }
                command_cv.notify_one();
            }

            else if constexpr (std::is_same_v<T, QueueStreamableContainerRadioCommand>) {
                spdlog::info("PLAYER: QueueStreamableContainerRadioCommand");
                music::Radio radio;

                if (auto playlist_ptr = std::dynamic_pointer_cast<music::Playlist>(c.container)) {
                    radio = music_service->getRadio(playlist_ptr->ref);

                } else if (auto album_ptr = std::dynamic_pointer_cast<music::Album>(c.container)){
                    radio = music_service->getRadio(album_ptr->ref);

                } else {
                    spdlog::warn("PLAYER: QueueStreamableRadioCommand, unsupported streamable type");
                    return;
                }

                {
                    std::lock_guard lock(state_mutex);
                    this->state.radio = radio;
                    this->state.radio_queue.clear();
                }

                std::shared_ptr<music::IStreamableContainer> container = std::make_shared<music::Radio>(radio);

                for(auto streamable : container->getStreamables()) {
                    {
                        std::lock_guard lock(command_mutex);
                        command_queue.push(QueueStreamableCommand{
                            .streamable=streamable,
                            .fetch_album=false,
                            .start_radio=false,
                            .start_fresh=false,
                            .queue_in_radio=true
                        });
                    }
                }
                command_cv.notify_one();

            } else if constexpr (std::is_same_v<T, QueueNextRadioCommand>) {
                spdlog::info("PLAYER: QueueNextRadioCommand");

                music::Radio state_radio;

                {
                    std::lock_guard lock(state_mutex);
                    state_radio = this->state.radio;
                }

                if(state_radio.continuation.empty()) {
                    spdlog::warn("PLAYER: QueueNextRadioCommand, no continuation");
                    return;
                }

                music::Radio new_radio = music_service->getRadioNext(state_radio);
                // A network blip must not end the radio: keep the old continuation.
                if (music_service->lastError().failed) {
                    spdlog::warn("PLAYER: radio continuation failed, kept for next time");
                    return;
                }

                {
                    std::lock_guard lock(state_mutex);
                    this->state.radio = new_radio;
                }

                std::shared_ptr<music::IStreamableContainer> container = std::make_shared<music::Radio>(new_radio);

                for(auto streamable : container->getStreamables()) {
                    {
                        std::lock_guard lock(command_mutex);
                        command_queue.push(QueueStreamableCommand{
                            .streamable=streamable,
                            .fetch_album=false,
                            .start_radio=false,
                            .start_fresh=false,
                            .queue_in_radio=!c.auto_play
                        });

                        if(c.auto_play) {
                            c.auto_play = false;
                        }
                    }
                }
                command_cv.notify_one();

            } else if constexpr (std::is_same_v<T, HomeCommand>) {
                spdlog::info("PLAYER: HomeCommand");

                {
                    std::lock_guard lock(state_mutex);
                    state.flags["home"] = Flags::Ongoing;
                    state.errors.erase("home");
                    // The current (maybe cached) page stays up until the new
                    // one lands, instead of blanking to a spinner.
                }

                /* Nothing shown yet (no cached page): the first shelves go up as
                soon as they arrive, the rest replaces them when it lands.
                */
                bool empty_page;
                {
                    std::lock_guard lock(state_mutex);
                    empty_page = state.home.empty() || (state.home.size() == 1 && state.home.count(RECENT_SHELF));
                }
                if (empty_page) {
                    auto first = music_service->getHome(false, true);
                    if (!music_service->lastError().failed && !first.empty()) {
                        std::lock_guard lock(state_mutex);
                        state.home = first;
                        state.home_order = music_service->homeOrder();
                        mergeHistoryShelf();
                        spdlog::info("PLAYER: home, first {} shelves shown", first.size());
                    }
                }

                std::unordered_map<std::string, std::vector<music::ApiResult>> user_home = music_service->getHome();
                const services::RequestError& err = music_service->lastError();

                {
                    std::lock_guard lock(state_mutex);

                    if (err.failed && !err.cancelled) {
                        state.flags["home"] = Flags::Error;
                        state.errors["home"] = { err.message, err.retryable };

                    } else if (!err.failed) {
                        state.flags["home"] = Flags::Done;
                        state.home = user_home;
                        state.home_order = music_service->homeOrder();
                        mergeHistoryShelf();
                    }
                }

            } else if constexpr (std::is_same_v<T, LibraryPlaylistsCommand>) {
                spdlog::info("PLAYER: LibraryPlaylistsCommand");

                {
                    std::lock_guard lock(state_mutex);
                    state.flags["library_playlists"] = Flags::Ongoing;
                    state.errors.erase("library_playlists");
                    refreshLibraryError(state);
                    // Keep the shown (maybe cached) list until the new one lands.
                }

                std::vector<music::Playlist> user_playlists = music_service->getLibraryPlaylists();
                const services::RequestError& err = music_service->lastError();

                {
                    std::lock_guard lock(state_mutex);

                    if (err.failed && !err.cancelled) {
                        state.flags["library_playlists"] = Flags::Error;
                        state.errors["library_playlists"] = { err.message, err.retryable };

                    } else if (!err.failed) {
                        state.flags["library_playlists"] = Flags::Done;
                        state.library_playlists = user_playlists;
                    }

                    refreshLibraryError(state);
                }

            } else if constexpr (std::is_same_v<T, PlayNextCommand>) {
                auto song_ptr = std::dynamic_pointer_cast<music::Song>(c.streamable);
                if (!song_ptr) return;
                auto song = std::make_shared<music::Song>();
                song->setRef(song_ptr->ref);
                std::string title = song->ref.title;

                bool queue_instead = false;
                {
                    std::lock_guard lock(state_mutex);
                    const int pos = state.queue_position;
                    if (pos < 0 || static_cast<size_t>(pos) >= state.user_queue.size()) {
                        queue_instead = true;      // nothing loaded: just play it
                    } else {
                        state.user_queue.insert(state.user_queue.begin() + pos + 1, song);
                        mpv_service->insertAt(streamUrlFor(song->getStreamUrl()), static_cast<uint16_t>(pos + 1));
                        state.notice = "Plays next: " + title;
                        state.notice_until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
                    }
                }

                if (queue_instead) {
                    std::lock_guard lock(command_mutex);
                    command_queue.push(QueueStreamableCommand{ .streamable = song, .fetch_album = false, .start_radio = true, .start_fresh = true });
                } else {
                    updateMprisControls();
                    preloadNext();
                }

            } else if constexpr (std::is_same_v<T, ShuffleCommand>) {
                std::vector<std::string> urls;
                size_t pos = 0, count = 0;

                {
                    // Queue and mpv change under one lock, so a track ending
                    // halfway can't advance one against the other.
                    std::lock_guard lock(state_mutex);
                    pos = static_cast<size_t>(std::max(0, state.queue_position));
                    count = state.user_queue.size();
                    if (count <= pos + 2) return;

                    static std::mt19937 rng{std::random_device{}()};
                    std::shuffle(state.user_queue.begin() + pos + 1, state.user_queue.end(), rng);
                    for (size_t i = pos + 1; i < count; i++) urls.push_back(state.user_queue[i]->getStreamUrl());

                    state.notice = "Queue shuffled (" + std::to_string(urls.size()) + " tracks)";
                    state.notice_until = std::chrono::steady_clock::now() + std::chrono::seconds(3);

                    // Rebuild mpv's tail in the new order; the current track plays on.
                    for (size_t i = count - 1; i > pos; i--) mpv_service->removeAt(static_cast<uint16_t>(i));
                    for (const auto& url : urls) mpv_service->load(url);
                }
                preloadNext();

            } else if constexpr (std::is_same_v<T, MoveCommand>) {
                {
                    std::lock_guard lock(state_mutex);
                    const size_t pos = static_cast<size_t>(std::max(0, state.queue_position));
                    const size_t n = state.user_queue.size();
                    if (c.from <= pos || c.to <= pos || c.from >= n || c.to >= n || c.from == c.to) return;

                    auto item = state.user_queue[c.from];
                    state.user_queue.erase(state.user_queue.begin() + c.from);
                    state.user_queue.insert(state.user_queue.begin() + c.to, item);

                    // playlist-move puts the entry before the target index.
                    mpv_service->move(static_cast<uint16_t>(c.from), static_cast<uint16_t>(c.to > c.from ? c.to + 1 : c.to));
                }
                preloadNext();

            } else if constexpr (std::is_same_v<T, PlaylistEditCommand>) {
                using K = PlaylistEditCommand::Kind;
                music_service->refreshCookies();    // writes want fresh cookies

                bool ok = false;
                switch (c.kind) {
                    case K::Create: ok = !music_service->createPlaylist(c.title, c.video_id).empty(); break;
                    case K::Add:    ok = music_service->addToPlaylist(c.playlist_id, c.video_id); break;
                    case K::Remove: ok = music_service->removeFromPlaylist(c.playlist_id, c.video_id, c.set_video_id); break;
                    case K::Rename: ok = music_service->renamePlaylist(c.playlist_id, c.title); break;
                    case K::Delete: ok = music_service->deletePlaylist(c.playlist_id); break;
                }
                const services::RequestError& err = music_service->lastError();

                std::optional<music::PlaylistRef> reopen;
                {
                    std::lock_guard lock(state_mutex);
                    state.notice_until = std::chrono::steady_clock::now() + std::chrono::seconds(4);
                    const char* what[] = { "Created \u201c", "Added to ", "Removed from the playlist", "Renamed to \u201c", "Deleted \u201c" };
                    if (ok) {
                        switch (c.kind) {
                            case K::Create: case K::Rename: case K::Delete: state.notice = what[static_cast<int>(c.kind)] + c.title + "\u201d"; break;
                            case K::Add:    state.notice = what[1] + c.title; break;
                            case K::Remove: state.notice = what[2]; break;
                        }
                    } else {
                        state.notice = "Couldn't change the playlist: " + err.message;
                    }

                    // The open page shows the change: fetch it again.
                    if (ok && c.kind != K::Create) {
                        playlist_cache.erase(c.playlist_id);
                        if (state.open_playlist && state.open_playlist->ref.id == c.playlist_id && c.kind != K::Delete) {
                            reopen = state.open_playlist->ref;
                            if (c.kind == K::Rename) reopen->title = c.title;
                        }
                    }
                }

                if (ok) {
                    std::lock_guard lock(command_mutex);
                    command_queue.push(LibraryPlaylistsCommand{});      // the sidebar shows the change
                }
                if (reopen) openPlaylist(*reopen);

            } else if constexpr (std::is_same_v<T, PlaylistRestCommand>) {
                // Already complete (asked twice: opened, then played): no refetch.
                std::shared_ptr<music::Playlist> done;
                {
                    std::lock_guard lock(state_mutex);
                    auto it = playlist_cache.find(c.ref.id);
                    if (it != playlist_cache.end() && it->second->getStreamables().size() >= c.have) done = it->second;   // the cache holds complete lists
                }
                music::Playlist full = done ? *done : music_service->getPlaylist(c.ref);
                if (!done && music_service->lastError().failed) {
                    std::lock_guard lock(state_mutex);
                    if (state.open_playlist && state.open_playlist->ref.id == c.ref.id) state.flags["open_playlist_more"] = Flags::Done;
                    state.notice = "Couldn't load the rest of " + c.ref.title + " (showing the first " + std::to_string(c.have) + ")";
                    state.notice_until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                    return;
                }
                const auto tracks = full.getStreamables();
                std::vector<std::shared_ptr<music::IStreamable>> rest;
                {
                    std::lock_guard lock(state_mutex);
                    playlist_cache[c.ref.id] = std::make_shared<music::Playlist>(full);
                    playlist_first_page.erase(c.ref.id);
                    if (state.open_playlist && state.open_playlist->ref.id == c.ref.id) {
                        // Same header as shown (a rename, an album's details).
                        auto shown = std::make_shared<music::Playlist>(full);
                        shown->setRef(state.open_playlist->ref);
                        state.open_playlist = shown;
                        state.flags["open_playlist_more"] = Flags::Done;
                    }
                    // Playing from it: the tracks that weren't there yet follow.
                    if (queued_from_id == c.ref.id && queued_from_have < tracks.size()) {
                        rest.assign(tracks.begin() + static_cast<long>(queued_from_have), tracks.end());
                        queued_from_have = tracks.size();
                    }
                    // Its first page was added to the queue: the rest goes after it.
                    else if (c.append_from != SIZE_MAX && c.epoch == queue_epoch && c.append_from < tracks.size()) {
                        rest.assign(tracks.begin() + static_cast<long>(c.append_from), tracks.end());
                    }
                }
                if (!rest.empty()) {
                    spdlog::info("PLAYER: {} more tracks from {} join the queue", rest.size(), c.ref.title);
                    std::lock_guard lock(command_mutex);
                    const uint64_t epoch = queue_epoch;
                    for (auto& s : rest)
                        command_queue.push(QueueStreamableCommand{ .streamable = s, .fetch_album = false, .start_radio = false, .start_fresh = false, .queue_in_radio = false, .from_container = true, .epoch = epoch });
                }

            } else if constexpr (std::is_same_v<T, ResolveCurrentCommand>) {
                if (!load_pending) return;
                std::string url;
                {
                    std::lock_guard lock(state_mutex);
                    if (state.current) url = state.current->getStreamUrl();
                }
                if (!url.empty()) resolveCurrentAsync(url);

            } else if constexpr (std::is_same_v<T, LoadCurrentCommand>) {
                if (c.generation != load_generation || !load_pending) return;   // replaced meanwhile
                load_pending = false;

                /* mpv's playlist mirrors the queue by index: give it every entry
                queued so far, then play the current one (a skip may have moved
                it while it resolved).
                */
                {
                    std::lock_guard lock(state_mutex);
                    const int pos = state.queue_position;
                    for (size_t i = 0; i < state.user_queue.size(); i++) {
                        const std::string yt = state.user_queue[i]->getStreamUrl();
                        const std::string url = (yt == c.youtube_url && !c.stream_url.empty()) ? c.stream_url : streamUrlFor(yt);
                        mpv_service->append(url);
                    }
                    if (pos >= 0 && static_cast<size_t>(pos) < state.user_queue.size()) mpv_service->skipTo(static_cast<uint16_t>(pos));
                }

                // What waited for mpv's playlist runs now, before anything newer.
                if (!postponed.empty()) {
                    std::lock_guard lock(command_mutex);
                    std::queue<Command> merged;
                    for (auto& p : postponed) merged.push(std::move(p));
                    while (!command_queue.empty()) { merged.push(std::move(command_queue.front())); command_queue.pop(); }
                    command_queue.swap(merged);
                    postponed.clear();
                }
                preloadNext();

            } else if constexpr (std::is_same_v<T, PreloadedCommand>) {
                // Not in mpv yet (autoplay radio): keep it for when it gets loaded.
                if (c.index == SIZE_MAX) {
                    std::lock_guard lock(preloaded_mutex);
                    if (preloaded.size() > 32) preloaded.clear();
                    preloaded[c.expected_url] = c.stream_url;
                    spdlog::info("PLAYER: next radio track preloaded");
                    return;
                }

                {
                    std::lock_guard lock(state_mutex);
                    // Still the same track at the same place, and not playing yet?
                    if (c.index >= state.user_queue.size() || static_cast<int>(c.index) <= state.queue_position) return;
                    if (state.user_queue[c.index]->getStreamUrl() != c.expected_url) return;

                    // Same lock as the check: the entry can't start in between.
                    mpv_service->insertAt(c.stream_url, static_cast<uint16_t>(c.index));
                    mpv_service->removeAt(static_cast<uint16_t>(c.index + 1));
                }
                spdlog::info("PLAYER: next track preloaded");

            } else if constexpr (std::is_same_v<T, OpenAlbumCommand>) {
                spdlog::info("PLAYER: OpenAlbumCommand, {}", c.ref.title);

                music::Album album = music_service->getAlbum(c.ref);
                const services::RequestError& err = music_service->lastError();

                // Shown through the playlist view: same header, same rows.
                music::PlaylistRef as_list;
                as_list.id = c.ref.id;
                as_list.title = album.ref.title.empty() ? c.ref.title : album.ref.title;
                const auto& artists = album.ref.artists.empty() ? c.ref.artists : album.ref.artists;
                as_list.author = artists.empty() ? "" : artists[0].name;
                if (!album.ref.year.empty()) as_list.author += (as_list.author.empty() ? "" : "  \u2022  ") + album.ref.year;
                as_list.thumbnail_large = album.ref.thumbnail_large.empty() ? c.ref.thumbnail_large : album.ref.thumbnail_large;
                as_list.thumbnail_small = album.ref.thumbnail_small.empty() ? c.ref.thumbnail_small : album.ref.thumbnail_small;

                auto list = std::make_shared<music::Playlist>();
                list->setRef(as_list);
                for (auto& s : album.getStreamables()) list->addStreamable(s);

                std::lock_guard lock(state_mutex);
                if (!state.open_playlist || state.open_playlist->ref.id != c.ref.id) return;

                if (err.failed && !err.cancelled) {
                    state.flags["open_playlist"] = Flags::Error;
                    state.errors["open_playlist"] = { err.message, err.retryable };
                } else {
                    state.open_playlist = list;
                    state.flags["open_playlist"] = Flags::Done;
                }

            } else if constexpr (std::is_same_v<T, OpenArtistCommand>) {
                spdlog::info("PLAYER: OpenArtistCommand, {}", c.ref.name);

                auto page = std::make_shared<services::Music::ArtistPage>(music_service->getArtist(c.ref));
                const services::RequestError& err = music_service->lastError();

                std::lock_guard lock(state_mutex);
                if (!state.open_artist || state.open_artist->ref.id != c.ref.id) return;

                if (err.failed && !err.cancelled) {
                    state.flags["open_artist"] = Flags::Error;
                    state.errors["open_artist"] = { err.message, err.retryable };
                } else {
                    state.open_artist = page;
                    state.flags["open_artist"] = Flags::Done;
                }

            } else if constexpr (std::is_same_v<T, RefreshSessionCommand>) {
                if (music_service->refreshCookies()) mpv_service->refreshSessionCookies();

            } else if constexpr (std::is_same_v<T, PrefetchPlaylistCommand>) {
                // Scrolled past it since: the worker doesn't fetch it (Play,
                // Open, Search would wait behind every playlist passed over).
                {
                    std::lock_guard lock(state_mutex);
                    if (c.ref.id != latest_prefetch) return;
                }
                // Its first track is resolved too, so Play starts at once.
                auto warm_first = [this](const music::Playlist& pl) {
                    const auto items = pl.getStreamables();
                    if (!items.empty()) warmNow(items.front()->getStreamUrl());
                };
                {
                    std::lock_guard lock(state_mutex);
                    auto it = playlist_cache.find(c.ref.id);
                    if (it != playlist_cache.end()) { auto pl = it->second; warm_first(*pl); return; }
                }
                {
                    std::lock_guard lock(state_mutex);
                    auto it = playlist_first_page.find(c.ref.id);
                    if (it != playlist_first_page.end()) { auto pl = it->second; warm_first(*pl); return; }
                }
                // First page only (one request): a long playlist doesn't hold
                // the worker, and opening it shows this page at once.
                bool more = false;
                music::Playlist playlist = music_service->getPlaylist(c.ref, true, &more);
                if (music_service->lastError().failed || playlist.getStreamables().empty()) return;
                warm_first(playlist);

                std::lock_guard lock(state_mutex);
                if (playlist_cache.size() > 40) playlist_cache.clear();
                if (playlist_first_page.size() > 40) playlist_first_page.clear();
                if (!more) playlist_cache[c.ref.id] = std::make_shared<music::Playlist>(playlist);
                else playlist_first_page[c.ref.id] = std::make_shared<music::Playlist>(playlist);
                spdlog::info("PLAYER: prefetched playlist {}", c.ref.title);

            } else if constexpr (std::is_same_v<T, OpenPlaylistCommand>) {
                spdlog::info("PLAYER: OpenPlaylistCommand, {}", c.ref.title);

                // First page (~100 tracks, one request) shown at once; the rest
                // follows in the background (PlaylistRestCommand).
                bool more = false;
                music::Playlist playlist = music_service->getPlaylist(c.ref, true, &more);
                const services::RequestError& err = music_service->lastError();
                const size_t have = playlist.getStreamables().size();
                more = more && !err.failed;

                {
                    std::lock_guard lock(state_mutex);
                    if (!err.failed && have > 0 && !more)
                        playlist_cache[c.ref.id] = std::make_shared<music::Playlist>(playlist);

                    // A newer open replaced this one while it loaded: drop it.
                    if (!state.open_playlist || state.open_playlist->ref.id != c.ref.id) return;

                    if (err.failed && !err.cancelled) {
                        state.flags["open_playlist"] = Flags::Error;
                        state.errors["open_playlist"] = { err.message, err.retryable };
                    } else {
                        state.open_playlist = std::make_shared<music::Playlist>(playlist);
                        state.flags["open_playlist"] = Flags::Done;
                        state.flags["open_playlist_more"] = more ? Flags::Ongoing : Flags::Done;
                    }
                }
                if (more) {
                    std::lock_guard lock(command_mutex);
                    command_queue.push(PlaylistRestCommand{c.ref, have});
                }

            } else if constexpr (std::is_same_v<T, RateCommand>) {
                spdlog::info("PLAYER: RateCommand, {}", c.like ? "like" : "remove like");

                // Writes need the freshest cookies; re-read them first if we can.
                music_service->refreshCookies();

                const bool ok = music_service->rateSong(c.video_id, c.like);
                const services::RequestError& err = music_service->lastError();

                std::lock_guard lock(state_mutex);
                state.notice_until = std::chrono::steady_clock::now() + std::chrono::seconds(6);

                if (ok) {
                    state.notice = c.like ? "Added to Liked Music" : "Removed from Liked Music";
                } else {
                    // Roll the optimistic heart back and say why.
                    state.liked[c.video_id] = !c.like;
                    const std::string why = err.message + " " + err.detail;
                    const bool auth = why.find("401") != std::string::npos
                                   || why.find("signed in") != std::string::npos;
                    state.notice = auth
                        ? "Like refused: session too old for writes, run: pbpaste | moroder setup"
                        : "Like failed: " + err.message;
                    spdlog::warn("PLAYER: rating failed, {}", err.message);
                }

            } else if constexpr (std::is_same_v<T, IsLoggedInCommand>) {
                spdlog::info("PLAYER: IsLoggedInCommand");

                {
                    /* is_logged_in starts false, so without a flag the UI cannot
                    tell "no account" from "not checked yet" and flashes the
                    signed-out notice on every start.
                    */
                    std::lock_guard lock(state_mutex);
                    state.flags["is_logged_in"] = Flags::Ongoing;
                }

                bool logged_in = music_service->isLoggedIn();

                {
                    std::lock_guard lock(state_mutex);
                    state.is_logged_in = logged_in;
                    state.flags["is_logged_in"] = Flags::Done;
                }

            } else if constexpr (std::is_same_v<T, LibraryAlbumsCommand>) {
                spdlog::info("PLAYER: LibraryAlbumsCommand");

                {
                    std::lock_guard lock(state_mutex);
                    state.flags["library_albums"] = Flags::Ongoing;
                    state.errors.erase("library_albums");
                    refreshLibraryError(state);
                    state.library_albums.clear();
                }

                std::vector<music::Album> user_albums = music_service->getLibraryAlbums();
                const services::RequestError& err = music_service->lastError();

                {
                    std::lock_guard lock(state_mutex);

                    if (err.failed && !err.cancelled) {
                        state.flags["library_albums"] = Flags::Error;
                        state.errors["library_albums"] = { err.message, err.retryable };

                    } else if (!err.failed) {
                        state.flags["library_albums"] = Flags::Done;
                        state.library_albums = user_albums;
                    }

                    refreshLibraryError(state);
                }

            } else if constexpr (std::is_same_v<T, LibrarySongsCommand>) {
                spdlog::info("PLAYER: LibrarySongsCommand");

                {
                    std::lock_guard lock(state_mutex);
                    state.flags["library_songs"] = Flags::Ongoing;
                    state.errors.erase("library_songs");
                    refreshLibraryError(state);
                    state.library_songs.clear();
                }

                std::vector<std::shared_ptr<music::IStreamable>> user_songs = music_service->getLibrarySongs();
                const services::RequestError& err = music_service->lastError();

                {
                    std::lock_guard lock(state_mutex);

                    if (err.failed && !err.cancelled) {
                        state.flags["library_songs"] = Flags::Error;
                        state.errors["library_songs"] = { err.message, err.retryable };

                    } else if (!err.failed) {
                        state.flags["library_songs"] = Flags::Done;
                        state.library_songs = user_songs;
                    }

                    refreshLibraryError(state);
                }

            } else if constexpr (std::is_same_v<T, LibraryArtistsCommand>) {
                spdlog::info("PLAYER: LibraryArtistsCommand");

                {
                    std::lock_guard lock(state_mutex);
                    state.flags["library_artists"] = Flags::Ongoing;
                    state.errors.erase("library_artists");
                    refreshLibraryError(state);
                    state.library_artists.clear();
                }

                std::vector<music::ArtistRef> user_artists = music_service->getLibraryArtists();
                const services::RequestError& err = music_service->lastError();

                {
                    std::lock_guard lock(state_mutex);

                    if (err.failed && !err.cancelled) {
                        state.flags["library_artists"] = Flags::Error;
                        state.errors["library_artists"] = { err.message, err.retryable };

                    } else if (!err.failed) {
                        state.flags["library_artists"] = Flags::Done;
                        state.library_artists = user_artists;
                    }

                    refreshLibraryError(state);
                }

            } else if constexpr (std::is_same_v<T, LibraryPodcastsCommand>) {
                spdlog::info("PLAYER: LibraryPodcastsCommand");

                {
                    std::lock_guard lock(state_mutex);
                    state.flags["library_podcasts"] = Flags::Ongoing;
                    state.errors.erase("library_podcasts");
                    refreshLibraryError(state);
                    state.library_podcasts.clear();
                }

                std::vector<music::PodcastRef> user_podcasts = music_service->getLibraryPodcasts();
                const services::RequestError& err = music_service->lastError();

                {
                    std::lock_guard lock(state_mutex);

                    if (err.failed && !err.cancelled) {
                        state.flags["library_podcasts"] = Flags::Error;
                        state.errors["library_podcasts"] = { err.message, err.retryable };

                    } else if (!err.failed) {
                        state.flags["library_podcasts"] = Flags::Done;
                        state.library_podcasts = user_podcasts;
                    }

                    refreshLibraryError(state);
                }
            }

        }, cmd);

        {
            std::lock_guard lock(callback_mutex);

            if (on_request_completed) {
                on_request_completed();
            }
        }
    }
}

services::Player::~Player() {
    // Silence first, whatever teardown still has to wait for.
    if (mpv_service) mpv_service->stop();

    this->stopPositionTick();
    this->detachUI();

    {
        std::lock_guard lock(command_mutex);
        running = false;
    }

    command_cv.notify_all();

    if (worker_thread.joinable()) {
        worker_thread.join();
    }
}

void services::Player::getHome() {
    {
        std::lock_guard lock(command_mutex);
        command_queue.push(HomeCommand{});
    }

    command_cv.notify_one();
}

void services::Player::isLoggedIn() {
    {
        std::lock_guard lock(command_mutex);
        command_queue.push(IsLoggedInCommand{});
    }

    command_cv.notify_one();
}

void services::Player::getLibraryPlaylists() {
    {
        std::lock_guard lock(command_mutex);
        command_queue.push(LibraryPlaylistsCommand{});
    }

    command_cv.notify_one();
}


void services::Player::search(const std::string& query) {
    {
        std::lock_guard lock(command_mutex);
        command_queue.push(SearchCommand{query});
    }

    command_cv.notify_one();
}

void services::Player::queue(std::shared_ptr<music::IStreamable> streamable, const bool fresh, const bool radio) {
    if (fresh) stopPreview(false);     // playing something else ends a preview
    const Config cfg = Config::get();

    {
        std::lock_guard lock(command_mutex);
        command_queue.push(QueueStreamableCommand{
            .streamable=streamable,
            .fetch_album=cfg.FETCH_ALBUMS,
            .start_radio=radio,
            .start_fresh=fresh,
            .queue_in_radio=false
        });
    }

    command_cv.notify_one();
}

void services::Player::queue(std::shared_ptr<music::IStreamableContainer> container, const bool fresh, const bool radio, size_t skip) {
    if (fresh) stopPreview(false);     // playing something else ends a preview
    const Config cfg = Config::get();

    {
        std::lock_guard lock(command_mutex);
        command_queue.push(QueueStreamableContainerCommand{
            .container=container,
            .fetch_albums=false,
            .start_fresh=fresh,
            .start_radio=radio,
            .queue_in_radio=false,
            .skip=skip
        });
    }

    command_cv.notify_one();
}

void services::Player::skipForward() {
    bool to_radio_skip = false;
    bool should_skip = true;

    int loop_to = -1;

    std::shared_ptr<music::IStreamable> radio_next;

    {
        std::lock_guard lock(state_mutex);

        /* Queue loop
        - Skipping past the end wraps rather than stopping, and the radio queue
          is passed over exactly as it is when the queue ends on its own
        */
        if (state.loop_mode == LoopMode::Queue && state.user_queue.size() > 1
            && state.queue_position + 1 >= state.user_queue.size()) {

            state.queue_position = 0;
            state.current = state.user_queue[0];
            loop_to = 0;

            spdlog::info("PLAYER: queue loop, wrapping to the start");

        } else if (state.queue_position + 1 >= state.user_queue.size() && state.radio_queue.empty()) {
            should_skip = false;

            spdlog::warn("PLAYER: tried to skip to next song, but queue is done");

        } else if (state.queue_position < state.user_queue.size() && state.radio_queue.empty()){
            state.queue_position++;
            state.current = state.user_queue[state.queue_position];

        } else if (state.queue_position + 1 >= state.user_queue.size() && !state.radio_queue.empty()) {
            state.queue_position++;
            radio_next = state.radio_queue.front();

            state.current = radio_next;
            state.user_queue.push_back(radio_next);
            state.radio_queue.pop_front();

            to_radio_skip = true;
            // Into mpv under the same lock as the push (see QueueStreamableCommand).
            if (!load_pending) mpv_service->load(streamUrlFor(radio_next->getStreamUrl()));

            spdlog::info("PLAYER: skipping to radio");

        } else if (state.queue_position < state.user_queue.size() && !state.radio_queue.empty()){
            state.queue_position++;
            state.current = state.user_queue[state.queue_position];

        } else if (state.queue_position < 0) {
            state.queue_position = 0;
            state.current = state.user_queue[state.queue_position];
        }
    }

    if(to_radio_skip) spdlog::info("PLAYER: loaded next radio song");

    // Still resolving the first track: mpv has nothing yet; resolve the new one.
    if (should_skip && load_pending) {
        {
            std::lock_guard lock(command_mutex);
            command_queue.push(ResolveCurrentCommand{});
        }
        command_cv.notify_one();
        updateMprisControls();
        return;
    }

    if(should_skip) {
        spdlog::info("PLAYER: skipping to next song");

        /* skipping
        - We keep these even if onSongStart handles skipping
        - because they don't wait for file load.
        */
        mpris_service->setPlaybackStatus(services::PlaybackStatus::Stopped);
        this->updateMprisControls();
        this->updateMprisData();
        this->updateSocialData();

        // playlist-next has nowhere to go at the end of the playlist, so a
        // wrap has to name the index it is going back to.
        if (loop_to >= 0) {
            mpv_service->skipTo(loop_to);
        } else {
            mpv_service->skipForward();
        }
    }
}

void services::Player::skipBackward() {
    /* Like any player: past the first 3 s, or on the first track, "previous"
    starts the track over. On the first track it used to mark playback as
    stopped while the music went on.
    */
    {
        bool restart = false;
        {
            std::lock_guard lock(state_mutex);
            if (!state.current) return;
            const bool wraps = state.loop_mode == LoopMode::Queue && state.user_queue.size() > 1;
            restart = !wraps && state.queue_position <= 0;
        }
        if (restart || mpv_service->getStreamPosition() > 3000000ULL) {
            mpv_service->setPosition(0);
            social_service->setPosition(0);
            return;
        }
    }

    bool should_skip = true;

    int loop_to = -1;

    {
        std::lock_guard lock(state_mutex);

        // The other end of the same wrap: going back from the first track
        // lands on the last one.
        if (state.loop_mode == LoopMode::Queue && state.user_queue.size() > 1
            && state.queue_position == 0) {

            state.queue_position = state.user_queue.size() - 1;
            state.current = state.user_queue.back();
            loop_to = static_cast<int>(state.queue_position);

            spdlog::info("PLAYER: queue loop, wrapping to the end");

        } else if (state.queue_position == 0 || state.user_queue.empty()) {
            state.flags["audio"] = Flags::Done;
            should_skip = false;

            spdlog::warn("PLAYER: tried to skip to previous song, but already at start of queue.");

        } else {
            state.queue_position--;
            state.current = state.user_queue[state.queue_position];
        }
    }

    // Still resolving the first track: mpv has nothing yet; resolve the new one.
    if (should_skip && load_pending) {
        {
            std::lock_guard lock(command_mutex);
            command_queue.push(ResolveCurrentCommand{});
        }
        command_cv.notify_one();
        updateMprisControls();
        return;
    }

    if(should_skip) {
        spdlog::info("PLAYER: skipping to previous song");

        if (loop_to >= 0) {
            mpv_service->skipTo(loop_to);
        } else {
            mpv_service->skipBackward();
        }

        mpris_service->setPlaybackStatus(services::PlaybackStatus::Stopped);
        this->updateMprisControls();
        this->updateMprisData();
        this->updateSocialData();
    }
}

void services::Player::updateMprisControls() {
    {
        std::lock_guard lock(state_mutex);

        /* A queue loop with something to wrap round always has a next and a
        previous, since running off either end comes back on the other
        */
        const bool wraps = state.loop_mode == LoopMode::Queue && state.user_queue.size() > 1;

        const bool can_skip_forwards  = wraps || (state.queue_position + 1 < state.user_queue.size()) || (!state.radio_queue.empty());
        const bool can_skip_backwards = wraps || (state.queue_position > 0 && state.user_queue.size() > 1);

        mpris_service->setIsNextPossible(can_skip_forwards);
        mpris_service->setIsPreviousPossible(can_skip_backwards);
        mpris_service->updatePlayerControls();

        state.flags["can_skip_forwards"] = can_skip_forwards ? Flags::True : Flags::False;
        state.flags["can_skip_backwards"] = can_skip_backwards ? Flags::True : Flags::False;
    }
}

void services::Player::updateMprisData() {
    std::shared_ptr<music::IStreamable> current;

    {
        std::lock_guard lock(state_mutex);
        current = state.current;
    }

    if (!current) {
        return;
    }

    std::string hash_id = std::to_string(std::hash<std::string>{}(current->getStreamUrl()));
    spdlog::info("PLAYER: hashed id, " + hash_id);

    if (auto song = std::dynamic_pointer_cast<music::Song>(current)) {
        mpris_service->setMetadata({
            { services::Field::TrackId, sdbus::Variant(services::OBJECT_PATH + "/track/" + hash_id) },
            { services::Field::Album,   sdbus::Variant(song->album.title) },
            { services::Field::Title,   sdbus::Variant(song->ref.title) },
            { services::Field::Artist,  sdbus::Variant(song->ref.artists.empty() ? "" : song->ref.artists[0].name) },
            { services::Field::Length,  sdbus::Variant(current->getDuration()) },
            { services::Field::ArtUrl,  sdbus::Variant(song->ref.thumbnail_large) }
        });
    }
    else if (auto episode = std::dynamic_pointer_cast<music::Episode>(current)) {
        mpris_service->setMetadata({
            { services::Field::TrackId, sdbus::Variant(services::OBJECT_PATH + "/track/" + hash_id) },
            { services::Field::Title,   sdbus::Variant(episode->ref.title) },
            { services::Field::Artist,  sdbus::Variant(episode->ref.podcast.name) },
            { services::Field::Length,  sdbus::Variant(current->getDuration()) },
            { services::Field::ArtUrl,  sdbus::Variant(episode->ref.thumbnail_large) }
        });
    }

    mpris_service->setPosition(0);
    mpris_service->sendSeekedSignal(0);
}

void services::Player::resetMprisData() {
    mpris_service->setMetadata({});

    mpris_service->setPosition(0);
    mpris_service->sendSeekedSignal(0);
}

void services::Player::updateSocialData() {
    std::shared_ptr<music::IStreamable> current;

    {
        std::lock_guard lock(state_mutex);
        current = state.current;
    }

    if (!current) {
        return;
    }

    const Config& cfg = Config::get();

    if (auto song = std::dynamic_pointer_cast<music::Song>(current)) {

        const std::string artist = song->ref.artists.empty() ? "" : song->ref.artists[0].name;

        // Once per track: this runs on stream start and on skips alike.
        if (cfg.ENABLE_NOTIFICATIONS && song->ref.id != last_notified_id) {
            last_notified_id = song->ref.id;
            // Album only when it adds something: singles are often named
            // after the track ("Get Lucky (feat. ...)" under "Get Lucky").
            const std::string t = utils::lower(song->ref.title), a = utils::lower(song->album.title);
            const std::string head = t.substr(0, std::min<size_t>(t.size(), 8));
            const bool redundant = a.empty() || a.rfind(head, 0) == 0 || t.find(a) != std::string::npos;
            notifyTrack(song->ref.title, redundant ? artist : artist + "  \u00b7  " + song->album.title,
                        song->ref.thumbnail_large.empty() ? song->ref.thumbnail_small : song->ref.thumbnail_large);
        }

        std::string status_label =
              cfg.RICH_PRESENCE_STATUS_LABEL == "artist" ? artist
            : cfg.RICH_PRESENCE_STATUS_LABEL == "album"  ? song->album.title
            :                                              song->ref.title;

        social_service->setStatus(
            status_label,
            song->ref.artists.empty() ? "" : song->ref.artists[0].name,
            song->album.title,
            song->ref.thumbnail_large,
            current->getDuration()
        );
    }
    else if (auto episode = std::dynamic_pointer_cast<music::Episode>(current)) {

        std::string status_label =
              cfg.RICH_PRESENCE_STATUS_LABEL == "artist" ? episode->ref.podcast.name
            : cfg.RICH_PRESENCE_STATUS_LABEL == "album"  ? episode->ref.podcast.name
            :                                              episode->ref.title;

        social_service->setStatus(
            status_label,
            episode->ref.podcast.name,
            episode->ref.podcast.name,
            episode->ref.thumbnail_large,
            current->getDuration()
        );
    }
}

void services::Player::removeFromUserQueue(uint16_t index) {
    bool removed_current = false, has_next = false;

    {
        std::lock_guard lock(state_mutex);
        if (index >= state.user_queue.size()) {
            spdlog::warn("PLAYER: tried to remove song at {} but index is invalid ({}).", index, state.user_queue.size());
            return;
        }
        state.user_queue.erase(state.user_queue.begin() + index);

        if (index < state.queue_position) {
            state.queue_position -= 1;
        } else if (index == state.queue_position) {
            removed_current = true;
            has_next = index < state.user_queue.size() || !state.radio_queue.empty();
            state.queue_position -= 1;
        }

        // All under the lock: mpv's playlist changes in step with the queue.
        if (!removed_current) {
            if (!load_pending) mpv_service->removeAt(index);
        } else if (!has_next) {
            // The last one, and it was playing: nothing plays now.
            if (!load_pending) mpv_service->removeAt(index);
            state.current = nullptr;
            state.flags["audio"] = Flags::Done;
            state.queue_position = static_cast<int>(state.user_queue.size()) - 1;
        }
    }

    if (removed_current && has_next) {
        // Moves mpv and the queue on together, then drops the old entry.
        this->skipForward();
        std::lock_guard lock(state_mutex);
        if (!load_pending) mpv_service->removeAt(index);
    }

    this->updateMprisControls();
}

void services::Player::removeFromRadioQueue(uint16_t index) {
    {
        std::lock_guard lock(state_mutex);

        if(index < state.radio_queue.size()) {
            state.radio_queue.erase(state.radio_queue.begin() + index);
        }
    }

    this->updateMprisControls();
}

void services::Player::startPositionTick() {
    stopPositionTick();

    {
        std::lock_guard lock(position_tick_mutex);
        position_tick_running = true;
    }

    std::lock_guard thread_lock(position_tick_thread_mutex);
    position_tick_thread = std::thread([this] {
        uint64_t last_position = 0;

        while (true) {
            std::unique_lock lock(position_tick_mutex);
            position_tick_cv.wait_for(lock, std::chrono::seconds(1), [this] {
                return !position_tick_running;
            });

            if (!position_tick_running) break;
            lock.unlock();

            const uint64_t file_gen = file_generation;
            uint64_t duration = mpv_service->getStreamDuration();
            uint64_t pos = mpv_service->getStreamPosition();

            /* A position that has gone backwards on its own is mpv restarting
            the file under loop-file. Nothing else in the player moves for
            that, so without this Discord's timestamps and the MPRIS position
            keep counting from the first play. A manual seek lands here too,
            which is the same resync.
            */
            if (last_position > pos + LOOP_REWIND_EPSILON) {
                spdlog::info("PLAYER: stream restarted, resyncing position");

                social_service->setPosition(pos);
                mpris_service->setPosition(pos);
                mpris_service->sendSeekedSignal(pos);
            }

            last_position = pos;

            // Last.fm: scrobble once half the track (or 4 min) has been played.
            bool paused_now;
            {
                std::lock_guard sl(state_mutex);
                auto it = state.flags.find("paused");
                paused_now = it != state.flags.end() && it->second == Flags::True;
            }
            if (!scrobble_done && !paused_now && duration >= 30000000ULL && services::scrobble::enabled()) {
                scrobble_played_s++;
                const uint64_t need = std::min<uint64_t>(duration / 2000000ULL, 240);
                if (static_cast<uint64_t>(scrobble_played_s) >= need) {
                    scrobble_done = true;
                    std::array<std::string, 3> t;
                    { std::lock_guard sl(state_mutex); t = scrobble_track; }
                    services::scrobble::scrobble(t[0], t[1], t[2], static_cast<int>(duration / 1000000ULL), scrobble_started_at);
                }
            }

            /* Fade out over the last FADE_SECONDS (the next track fades in, see
            the stream start). Not on a looping track or the queue's last one.
            */
            const int fade_s = Config::get().FADE_SECONDS;
            // Seeked back out of the fade: full volume again.
            if (fading_out && duration > 0 && pos + static_cast<uint64_t>(fade_s + 1) * 1000000ULL < duration) {
                fading_out = false;
                ++fade_generation;
                int vol;
                { std::lock_guard sl(state_mutex); vol = state.volume; }
                mpv_service->setVolume(vol);
            }
            bool paused_fade;
            {
                std::lock_guard sl(state_mutex);
                auto it = state.flags.find("paused");
                paused_fade = it != state.flags.end() && it->second == Flags::True;
            }
            // Not while paused, and not on a reading from the previous file.
            if (fade_s > 0 && !paused_fade && file_gen == file_generation && duration > 0
                && pos + static_cast<uint64_t>(fade_s) * 1000000ULL >= duration && !fading_out) {
                bool has_next;
                {
                    std::lock_guard sl(state_mutex);
                    has_next = state.loop_mode != LoopMode::Track
                            && (state.queue_position + 1 < static_cast<int>(state.user_queue.size()) || !state.radio_queue.empty());
                }
                if (has_next) {
                    fading_out = true;
                    const uint64_t gen = ++fade_generation;
                    const double left = static_cast<double>(duration - std::min(duration, pos)) / 1000000.0;
                    std::thread([this, gen, left] {
                        const int steps = std::max(1, static_cast<int>(left * 20));
                        for (int i = 1; i <= steps && fade_generation == gen; i++) {
                            int vol;
                            bool paused;
                            {
                                std::lock_guard sl(state_mutex);
                                vol = state.volume;
                                auto it = state.flags.find("paused");
                                paused = it != state.flags.end() && it->second == Flags::True;
                            }
                            // Follows playback: a pause holds the fade where it is.
                            if (paused) { i--; std::this_thread::sleep_for(std::chrono::milliseconds(100)); continue; }
                            mpv_service->setVolume(vol * (steps - i) / steps);
                            std::this_thread::sleep_for(std::chrono::milliseconds(50));
                        }
                    }).detach();
                }
            }

            if (on_position_tick) {
                on_position_tick(pos, duration);
            }
        }
    });
}

/* stopPositionTick
- Signal, then take the thread object out under its own mutex and join it
  with no lock held: joining while holding position_tick_mutex could deadlock
  (the thread needs it to see the stop), and the object was touched from the
  mpv and UI threads unguarded
*/
void services::Player::stopPositionTick() {
    {
        std::lock_guard lock(position_tick_mutex);
        position_tick_running = false;
    }
    position_tick_cv.notify_all();

    std::thread t;
    {
        std::lock_guard thread_lock(position_tick_thread_mutex);
        t = std::move(position_tick_thread);
    }

    if (t.joinable()) {
        if (t.get_id() != std::this_thread::get_id()) t.join();
        else t.detach();
    }
}

void services::Player::skipTo(uint16_t index) {
    bool should_skip = true;
    bool to_radio_skip = false;
    std::shared_ptr<music::IStreamable> radio_next;

    {
        std::lock_guard lock(state_mutex);

        const size_t user_size = state.user_queue.size();
        const size_t radio_size = state.radio_queue.size();
        const size_t total_size = user_size + radio_size;

        if (index >= total_size) {
            spdlog::warn("PLAYER: skipTo {}, but total queue size is {}.", index, total_size);
            should_skip = false;

        } else if (index < user_size) {
            state.queue_position = index;
            state.current = state.user_queue[index];

        } else {
            size_t radio_index = index - user_size;

            radio_next = state.radio_queue[radio_index];
            state.current = radio_next;
            state.queue_position = index;

            for (size_t i = 0; i <= radio_index; i++) {
                state.user_queue.push_back(state.radio_queue.front());
                state.radio_queue.pop_front();

                if (!load_pending) mpv_service->load(streamUrlFor(state.user_queue.back()->getStreamUrl()));
            }
        }
    }

    // Still resolving the first track: mpv has nothing yet; resolve this one.
    if (should_skip && load_pending) {
        {
            std::lock_guard lock(command_mutex);
            command_queue.push(ResolveCurrentCommand{});
        }
        command_cv.notify_one();
        updateMprisControls();
        return;
    }

    if (should_skip) {
        spdlog::info("PLAYER: skipping to index {}", index);

        mpv_service->skipTo(index);

        mpris_service->setPlaybackStatus(services::PlaybackStatus::Stopped);
        this->updateMprisControls();
        this->updateMprisData();
        this->updateSocialData();
    }
}

void services::Player::removeAt(uint16_t index) {
    size_t user_queue_size;

    {
        std::lock_guard lock(state_mutex);
        user_queue_size = state.user_queue.size();
    }

    if (index < user_queue_size) {
        this->removeFromUserQueue(index);
    } else {
        this->removeFromRadioQueue(index - user_queue_size);
    }

    {
        std::lock_guard lock(state_mutex);

        if(state.user_queue.size() == 0 && state.radio_queue.size() == 0) {
            this->resetMprisData();
            social_service->removeStatus();

            mpris_service->setPlaybackStatus(services::PlaybackStatus::Stopped);
            state.flags["audio"] = Flags::Done;
        }
    }
}

services::Player::LoopMode services::Player::loopMode() {
    std::lock_guard lock(state_mutex);
    return state.loop_mode;
}

void services::Player::setLoop(LoopMode mode) {
    this->applyLoop(mode, true);
}

/* cycleLoop
- off, then the queue, then the current track, in the order the icon in every
  other player cycles through
*/
services::Player::LoopMode services::Player::cycleLoop() {
    LoopMode next;

    {
        std::lock_guard lock(state_mutex);

        next = state.loop_mode == LoopMode::None  ? LoopMode::Queue
             : state.loop_mode == LoopMode::Queue ? LoopMode::Track
             :                                      LoopMode::None;
    }

    this->applyLoop(next, true);
    return next;
}

/* applyLoop
- notify_mpris is false when the change arrived over MPRIS, since the property
  was already written on the way in
- Track looping is handed to mpv rather than handled here: loop-file repeats
  the file without unloading it, so there is no end-of-file to catch, no gap
  between plays, and the queue position never moves
*/
void services::Player::applyLoop(LoopMode mode, bool notify_mpris) {
    {
        std::lock_guard lock(state_mutex);
        state.loop_mode = mode;
    }

    mpv_service->setLoopFile(mode == LoopMode::Track);

    if (notify_mpris) {
        mpris_service->setLoopStatus(loopStatusFor(mode));
    }

    // A queue loop changes what next and previous mean at the ends of the
    // queue, so the Can* properties and the playback bar arrows follow it.
    this->updateMprisControls();

    spdlog::info("PLAYER: loop mode is {}", loopModeName(mode));
}

void services::Player::detachUI() {
    std::lock_guard lock(callback_mutex);
    on_request_completed = nullptr;
}

void services::Player::pause() {
    {
        std::lock_guard lock(state_mutex);

        if (!state.current || state.flags["audio"] != Flags::Ongoing) {
            spdlog::warn("PLAYER: tried to pause, but nothing is playing");
            return;
        }

        state.flags["audio"]  = Flags::Done;
        state.flags["paused"] = Flags::True;
    }

    mpv_service->pause();
    social_service->pause();

    uint64_t position = mpv_service->getStreamPosition();

    social_service->setPosition(position);
    mpris_service->setPosition(position);
    mpris_service->setPlaybackStatus(services::PlaybackStatus::Paused);
}

void services::Player::resume() {
    {
        std::lock_guard lock(state_mutex);

        if (!state.current || state.flags["audio"] == Flags::Ongoing) {
            spdlog::warn("PLAYER: tried to resume, but nothing is paused");
            return;
        }

        state.flags["audio"]  = Flags::Ongoing;
        state.flags["paused"] = Flags::False;
    }

    mpv_service->resume();
    social_service->resume();

    uint64_t position = mpv_service->getStreamPosition();

    social_service->setPosition(position);
    mpris_service->setPosition(position);
    mpris_service->setPlaybackStatus(services::PlaybackStatus::Playing);
}

void services::Player::seekBy(int64_t delta) {
    {
        std::lock_guard lock(state_mutex);
        if (!state.current) return;
    }

    uint64_t new_position = mpv_service->getStreamPosition();
    const uint64_t duration = mpv_service->getStreamDuration();

    if (delta < 0) {
        // seekBackward takes the distance as a positive count.
        const uint64_t back = static_cast<uint64_t>(-delta);
        mpv_service->seekBackward(back);
        new_position = new_position > back ? new_position - back : 0;
    } else {
        mpv_service->seekForward(static_cast<uint64_t>(delta));
        new_position += static_cast<uint64_t>(delta);
        if (duration > 0 && new_position > duration) new_position = duration;
    }

    social_service->setPosition(new_position);
    mpris_service->setPosition(new_position);
}

void services::Player::seekTo(double seconds) {
    {
        std::lock_guard lock(state_mutex);
        if (!state.current) return;
    }
    const uint64_t position = static_cast<uint64_t>(std::max(0.0, seconds) * 1000000.0);
    mpv_service->setPosition(position);
    social_service->setPosition(position);
}

void services::Player::seekToFraction(double fraction) {
    {
        std::lock_guard lock(state_mutex);
        if (!state.current) return;
    }

    const uint64_t duration = mpv_service->getStreamDuration();
    if (duration == 0) return;

    const uint64_t position = static_cast<uint64_t>(std::clamp(fraction, 0.0, 1.0) * static_cast<double>(duration));
    mpv_service->setPosition(position);

    social_service->setPosition(position);
    mpris_service->setPosition(position);
    mpris_service->sendSeekedSignal(position);
}

void services::Player::changeVolume(int delta) {
    int volume;

    {
        std::lock_guard lock(state_mutex);
        state.volume = std::clamp(state.volume + delta, 0, 100);
        volume = state.volume;
    }

    mpv_service->setVolume(volume);

    /* Remembered for the next launch, once it settles: a drag on the slider
    or a held key would otherwise write the file at every step.
    */
    static std::atomic<int> to_save{-1};
    if (to_save.exchange(volume) < 0) {
        std::thread([] {
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            const int v = to_save.exchange(-1);
            if (v >= 0) std::ofstream(volumeFile(), std::ios::trunc) << v << "\n";
        }).detach();
    }
}

void services::Player::prefetchPlaylist(const music::PlaylistRef& ref) {
    if (ref.id.empty()) return;
    {
        std::lock_guard lock(state_mutex);
        latest_prefetch = ref.id;
    }
    {
        // Cached or not: the command also warms its first track.
        std::lock_guard lock(command_mutex);
        command_queue.push(PrefetchPlaylistCommand{ref});
    }
    command_cv.notify_one();
}

void services::Player::openPlaylist(const music::PlaylistRef& ref) {
    {
        std::lock_guard lock(state_mutex);

        // Already fetched (selected a moment ago): show it at once.
        if (auto it = playlist_cache.find(ref.id); it != playlist_cache.end()) {
            state.open_playlist = it->second;
            state.open_kind = "playlist";
            state.flags["open_playlist"] = Flags::Done;
            state.flags["open_playlist_more"] = Flags::Done;
            state.errors.erase("open_playlist");
            return;
        }

        // Its first page is here: show it, fetch the rest.
        if (auto it = playlist_first_page.find(ref.id); it != playlist_first_page.end()) {
            state.open_playlist = it->second;
            state.open_kind = "playlist";
            state.flags["open_playlist"] = Flags::Done;
            state.flags["open_playlist_more"] = Flags::Ongoing;
            state.errors.erase("open_playlist");
            const size_t have = it->second->getStreamables().size();
            std::lock_guard cl(command_mutex);
            command_queue.push(PlaylistRestCommand{ref, have});
            command_cv.notify_one();
            return;
        }

        auto placeholder = std::make_shared<music::Playlist>();
        placeholder->setRef(ref);
        state.open_playlist = placeholder;
        state.open_kind = "playlist";
        state.flags["open_playlist"] = Flags::Ongoing;
        state.flags["open_playlist_more"] = Flags::Done;
        state.errors.erase("open_playlist");
    }

    {
        std::lock_guard lock(command_mutex);
        command_queue.push(OpenPlaylistCommand{ref});
    }

    command_cv.notify_one();
}

void services::Player::recordHistory(const music::SongRef& song) {
    if (song.id.empty() || Config::anonymous) return;

    nlohmann::json list = readJson("history.json");
    if (!list.is_array()) list = nlohmann::json::array();

    nlohmann::json fresh = nlohmann::json::array({ song.to_json() });
    for (const auto& j : list) {
        if (j.value("videoId", std::string()) != song.id && fresh.size() < 50) fresh.push_back(j);
    }
    writeJson("history.json", fresh);

    std::lock_guard lock(state_mutex);
    mergeHistoryShelf(&fresh);
}

void services::Player::mergeHistoryShelf(const nlohmann::json* known) {
    if (Config::anonymous) return;

    // Callers holding state_mutex pass the list they have: no disk read under it.
    nlohmann::json list = known ? *known : readJson("history.json");
    if (!list.is_array() || list.empty()) return;

    std::vector<music::ApiResult> shelf;
    for (const auto& j : list) {
        if (shelf.size() == 20) break;
        shelf.push_back(music::ApiResult::from_json(j));
    }

    state.home[RECENT_SHELF] = std::move(shelf);
    auto& order = state.home_order;
    order.erase(std::remove(order.begin(), order.end(), RECENT_SHELF), order.end());
    order.insert(order.begin(), RECENT_SHELF);
}

void services::Player::saveSession() {
    if (Config::anonymous) return;

    nlohmann::json session;
    {
        std::lock_guard lock(state_mutex);
        auto current = std::dynamic_pointer_cast<music::Song>(state.current);
        if (!current) return;

        session["current"] = current->ref.to_json();
        session["upcoming"] = nlohmann::json::array();
        for (size_t i = static_cast<size_t>(std::max(0, state.queue_position)) + 1; i < state.user_queue.size() && session["upcoming"].size() < 200; i++) {
            if (auto s = std::dynamic_pointer_cast<music::Song>(state.user_queue[i])) session["upcoming"].push_back(s->ref.to_json());
        }
    }
    session["position"] = static_cast<double>(mpv_service->getStreamPosition()) / 1000000.0;
    writeJson("session.json", session);

    // Stream URLs still valid for a while: the next launch starts them at once.
    nlohmann::json streams = nlohmann::json::object();
    {
        std::lock_guard lock(preloaded_mutex);
        for (const auto& [k, v] : recent_streams) if (streamFresh(v, 900)) streams[k] = v;
        for (const auto& [k, v] : preloaded)      if (streamFresh(v, 900)) streams[k] = v;
    }
    writeJson("streams.json", streams);
}

bool services::Player::restoreSession() {
    if (Config::anonymous) return false;

    {
        nlohmann::json streams = readJson("streams.json");
        if (streams.is_object()) {
            std::lock_guard lock(preloaded_mutex);
            for (auto it = streams.begin(); it != streams.end(); ++it)
                if (it.value().is_string() && streamFresh(it.value().get<std::string>(), 300)) recent_streams[it.key()] = it.value().get<std::string>();
        }
    }

    nlohmann::json session = readJson("session.json");
    if (!session.is_object() || !session.contains("current")) return false;

    auto list = std::make_shared<music::Playlist>();
    music::PlaylistRef ref;
    ref.id = "__resume__";
    ref.title = "Resumed session";
    list->setRef(ref);

    auto add = [&](const nlohmann::json& j) {
        auto s = std::make_shared<music::Song>();
        s->setRef(music::SongRef::from_json(j));
        if (!s->ref.id.empty()) list->addStreamable(s);
    };
    add(session["current"]);
    if (session.contains("upcoming") && session["upcoming"].is_array())
        for (const auto& j : session["upcoming"]) add(j);
    if (list->getStreamables().empty()) return false;

    {
        std::lock_guard lock(state_mutex);
        resume_at = session.value("position", 0.0);
        auto first = std::dynamic_pointer_cast<music::Song>(list->getStreamables().front());
        resume_id = first ? first->ref.id : "";
    }
    mpv_service->pause();       // loads paused: no blip of sound before the seek
    queue(std::static_pointer_cast<music::IStreamableContainer>(list), true, true);
    spdlog::info("PLAYER: resuming {} track(s) at {:.0f}s", list->getStreamables().size(), resume_at);
    return true;
}


std::string services::Player::streamUrlFor(const std::string& url) {
    std::lock_guard lock(preloaded_mutex);
    auto it = preloaded.find(url);
    if (it != preloaded.end()) {
        std::string stream = it->second;
        preloaded.erase(it);
        if (streamFresh(stream)) return stream;
    }
    // Resolved earlier (this run or the last one, see saveSession): valid 6 h;
    // with 15 min left at least, so a long track doesn't outlive its link.
    auto r = recent_streams.find(url);
    if (r != recent_streams.end()) {
        if (streamFresh(r->second, 900)) return r->second;
        recent_streams.erase(r);
    }
    return url;
}

void services::Player::rememberStream(const std::string& url, const std::string& stream) {
    if (stream.empty()) return;
    std::lock_guard lock(preloaded_mutex);
    if (recent_streams.size() > 300) {
        for (auto it = recent_streams.begin(); it != recent_streams.end(); ) it = streamFresh(it->second, 600) ? std::next(it) : recent_streams.erase(it);
        if (recent_streams.size() > 300) recent_streams.clear();
    }
    recent_streams[url] = stream;
}

void services::Player::forgetStream(const std::string& url) {
    std::lock_guard lock(preloaded_mutex);
    recent_streams.erase(url);
    preloaded.erase(url);
}

void services::Player::preview(const music::SongRef& song) {
    if (song.id.empty()) return;

    std::lock_guard pl(preview_mutex);
    if (!preview_mpv) {
        preview_mpv = std::make_unique<MPV>();
        // The clip lasts 15 s from when audio actually starts (after yt-dlp).
        preview_mpv->setOnStreamStart([this] {
            const int gen = preview_gen.load();
            std::thread([this, gen] {
                std::this_thread::sleep_for(std::chrono::seconds(15));
                if (preview_gen.load() == gen) stopPreview(true);
            }).detach();
        });
    }

    ++preview_gen;

    bool was_playing = false;
    int volume = 100;
    {
        std::lock_guard lock(state_mutex);
        was_playing = state.flags["audio"] == Flags::Ongoing;
        volume = state.volume;
        state.preview_title = song.title;
        state.notice = "Preview: " + song.title + "  (15 s, Esc to stop)";
        state.notice_until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    }

    // Pause the main track once, even across chained previews.
    if (was_playing && !preview_paused_main) {
        preview_paused_main = true;
        pause();
    }

    spdlog::info("PLAYER: preview {} (main paused: {})", song.title, preview_paused_main);
    music::Song s;
    s.setRef(song);
    preview_mpv->setVolume(volume);
    preview_mpv->resume();
    preview_mpv->play(streamUrlFor(s.getStreamUrl()), "start=30%");
}

void services::Player::stopPreview(bool resume_main) {
    std::lock_guard pl(preview_mutex);
    ++preview_gen;
    if (preview_mpv) preview_mpv->stop();

    {
        std::lock_guard lock(state_mutex);
        if (state.preview_title.empty() && !preview_paused_main) return;
        state.preview_title.clear();
        state.notice.clear();
    }

    spdlog::info("PLAYER: preview over{}", preview_paused_main && resume_main ? ", resuming" : "");
    if (preview_paused_main) {
        preview_paused_main = false;
        if (resume_main) resume();
    }
}

bool services::Player::previewing() {
    std::lock_guard lock(state_mutex);
    return !state.preview_title.empty();
}

void services::Player::playNext(std::shared_ptr<music::IStreamable> streamable) {
    {
        std::lock_guard lock(command_mutex);
        command_queue.push(PlayNextCommand{std::move(streamable)});
    }
    command_cv.notify_one();
}

void services::Player::shuffleUpcoming() {
    {
        std::lock_guard lock(command_mutex);
        command_queue.push(ShuffleCommand{});
    }
    command_cv.notify_one();
}

void services::Player::moveInQueue(size_t from, size_t to) {
    {
        std::lock_guard lock(command_mutex);
        command_queue.push(MoveCommand{from, to});
    }
    command_cv.notify_one();
}

/* preloadNext
- yt-dlp resolving a track at load time is the "loading" pause between two
  tracks. As soon as one starts, resolve the next one in the background and
  swap its mpv entry for the direct stream URL, so it starts at once
- Checked again before swapping (PreloadedCommand), in case the queue moved
*/
void services::Player::createPlaylist(const std::string& title, const std::string& video_id) {
    {
        std::lock_guard lock(command_mutex);
        command_queue.push(PlaylistEditCommand{title, "", video_id, PlaylistEditCommand::Kind::Create});
    }
    command_cv.notify_one();
}

void services::Player::addToPlaylist(const std::string& playlist_id, const std::string& title, const std::string& video_id) {
    {
        std::lock_guard lock(command_mutex);
        command_queue.push(PlaylistEditCommand{title, playlist_id, video_id, PlaylistEditCommand::Kind::Add});
    }
    command_cv.notify_one();
}

void services::Player::removeFromPlaylist(const std::string& playlist_id, const std::string& video_id, const std::string& set_video_id) {
    {
        std::lock_guard lock(command_mutex);
        command_queue.push(PlaylistEditCommand{"", playlist_id, video_id, PlaylistEditCommand::Kind::Remove, set_video_id});
    }
    command_cv.notify_one();
}

void services::Player::renamePlaylist(const std::string& playlist_id, const std::string& title) {
    {
        std::lock_guard lock(command_mutex);
        command_queue.push(PlaylistEditCommand{title, playlist_id, "", PlaylistEditCommand::Kind::Rename});
    }
    command_cv.notify_one();
}

void services::Player::deletePlaylist(const std::string& playlist_id, const std::string& title) {
    {
        std::lock_guard lock(command_mutex);
        command_queue.push(PlaylistEditCommand{title, playlist_id, "", PlaylistEditCommand::Kind::Delete});
    }
    command_cv.notify_one();
}

void services::Player::resolveCurrentAsync(const std::string& youtube_url) {
    load_pending = true;
    const uint64_t generation = ++load_generation;
    std::thread([this, generation, youtube_url] {
        const std::string direct = resolveStream(youtube_url, true);

        /* A stream URL is refused (403) for its first 2-5 s: loading it at once
        failed, then fell back to the slow path. Hand it to mpv as soon as it
        is accepted.
        */
        if (!direct.empty()) waitStreamReady(direct, [this, generation] { return generation != load_generation; });
        {
            std::lock_guard lock(command_mutex);
            command_queue.push(LoadCurrentCommand{generation, youtube_url, direct});
        }
        command_cv.notify_one();
    }).detach();
}

void services::Player::preloadNext() {
    // The next two tracks: skipping twice in a row stays instant too.
    std::vector<std::pair<size_t, std::string>> wanted;
    {
        std::lock_guard lock(state_mutex);
        if (state.queue_position < 0) return;
        for (size_t index = static_cast<size_t>(state.queue_position) + 1; index < state.user_queue.size() && wanted.size() < 2; index++) {
            wanted.emplace_back(index, state.user_queue[index]->getStreamUrl());
        }
        if (wanted.size() < 2 && state.autoplay && !state.radio_queue.empty()) {
            wanted.emplace_back(SIZE_MAX, state.radio_queue.front()->getStreamUrl());   // the radio's next
        }
    }

    static const std::regex video(R"(watch\?v=[A-Za-z0-9_-]{11}(&|$))");
    for (auto& [index, url] : wanted) {
        if (!std::regex_search(url, video)) continue;

        std::string ready;
        {
            std::lock_guard lock(preloaded_mutex);
            if (preloading.count(url)) continue;
            auto it = preloaded.find(url);
            auto r = recent_streams.find(url);
            if (it != preloaded.end()) {
                if (index == SIZE_MAX) continue;        // stays there until loaded
                ready = it->second;                     // resolved by warmStream
                preloaded.erase(it);
            } else if (r != recent_streams.end() && streamFresh(r->second, 600)) {
                if (index == SIZE_MAX) continue;        // streamUrlFor finds it
                ready = r->second;                      // resolved earlier / last run
            } else {
                preloading.insert(url);
            }
        }

        std::thread([this, index, url, ready] {
            const std::string stream = ready.empty() ? resolveStream(url) : ready;
            if (ready.empty()) {
                std::lock_guard lock(preloaded_mutex);
                preloading.erase(url);
            }
            if (stream.empty()) return;

            {
                std::lock_guard lock(command_mutex);
                command_queue.push(PreloadedCommand{index, url, stream});
            }
            command_cv.notify_one();
        }).detach();
    }
}

namespace {

/* A yt-dlp kept running (Python + extractor loaded once): ~1 s faster per
track than spawning yt-dlp. Reads one URL per line, answers one JSON line.
Two of them, so a selection pre-resolve never delays the track that starts.
*/
const char* RESOLVER_SCRIPT = R"PY(
import sys, json
import yt_dlp
# The player response has YouTube's loudness figure (dB above its reference);
# yt-dlp drops it, so catch it on the way.
LOUD = {}
try:
    from yt_dlp.extractor.youtube import _video
    _orig = _video.YoutubeIE._extract_player_response
    def _wrapped(self, client, video_id, *a, **k):
        pr = _orig(self, client, video_id, *a, **k)
        try:
            l = ((pr or {}).get('playerConfig') or {}).get('audioConfig', {}).get('loudnessDb')
            if l is not None: LOUD[video_id] = l
        except Exception: pass
        return pr
    _video.YoutubeIE._extract_player_response = _wrapped
except Exception: pass
opts = {'quiet': True, 'no_warnings': True, 'noprogress': True, 'format': 'bestaudio/best', 'skip_download': True}
if len(sys.argv) > 1: opts['cookiefile'] = sys.argv[1]
y = yt_dlp.YoutubeDL(opts)
for line in sys.stdin:
    try:
        d = y.extract_info(line.strip(), download=False)
        out = {'url': d.get('url') or '', 'http_headers': d.get('http_headers') or {}}
        if d.get('id') in LOUD: out['loudness'] = LOUD[d['id']]
    except BaseException:
        out = {}
    sys.stdout.write(json.dumps(out) + '\n')
    sys.stdout.flush()
)PY";

struct Resolver {
    std::mutex mtx;
    pid_t pid = 0;
    int to = -1, from = -1;
    std::string cookies;
    long long cookies_mtime = 0;    // yt-dlp reads the cookie file once: restart when it changes
    std::string pending;     // bytes read past the last answer
};

Resolver resolvers[2];

// The Python yt-dlp runs on (its shebang), so `import yt_dlp` works.
std::string ytdlpPython() {
    static const std::string python = [] {
        const char* path = std::getenv("PATH");
        std::string p = path ? path : "";
        size_t start = 0;
        while (start <= p.size()) {
            const size_t end = std::min(p.find(':', start), p.size());
            const std::filesystem::path f = std::filesystem::path(p.substr(start, end - start)) / "yt-dlp";
            std::ifstream in(f);
            std::string line;
            if (in && std::getline(in, line)) {
                if (line.rfind("#!", 0) == 0 && line.find("python") != std::string::npos && line.find(' ') == std::string::npos) return line.substr(2);
                return std::string();
            }
            start = end + 1;
        }
        return std::string();
    }();
    return python;
}

void stopResolver(Resolver& r) {
    if (r.pid > 0) { kill(r.pid, SIGKILL); waitpid(r.pid, nullptr, 0); }
    if (r.to >= 0) close(r.to);
    if (r.from >= 0) close(r.from);
    r.pid = 0; r.to = r.from = -1; r.pending.clear();
}

bool startResolver(Resolver& r, const std::string& cookies) {
    const std::string python = ytdlpPython();
    if (python.empty()) return false;

    int in[2], out[2];
    if (pipe(in) != 0) return false;
    if (pipe(out) != 0) { close(in[0]); close(in[1]); return false; }
    fcntl(in[1], F_SETFD, FD_CLOEXEC);
    fcntl(out[0], F_SETFD, FD_CLOEXEC);
#ifdef F_SETNOSIGPIPE
    fcntl(in[1], F_SETNOSIGPIPE, 1);
#endif

    std::vector<std::string> args = { python, "-I", "-c", RESOLVER_SCRIPT };
    if (!cookies.empty()) args.push_back(cookies);
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, in[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, out[1], STDOUT_FILENO);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);

    // Only the fds set up above reach the child (other spawns' pipes would
    // keep their readers from ever seeing EOF).
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT);
#endif
    pid_t pid = 0;
    const bool ok = posix_spawn(&pid, python.c_str(), &actions, &attr, argv.data(), environ) == 0;
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&actions);
    close(in[0]);
    close(out[1]);
    if (!ok) { close(in[1]); close(out[0]); return false; }

    r.pid = pid; r.to = in[1]; r.from = out[0]; r.cookies = cookies; r.pending.clear();
    return true;
}

std::string resolverCookies() {
    const std::filesystem::path cookies = Config::get().YTM_COOKIES_PATH / "cookies.txt";
    std::error_code ec;
    return (!Config::anonymous && std::filesystem::exists(cookies, ec)) ? cookies.string() : "";
}

void prestartResolvers() {
    const std::string cookies = resolverCookies();
    for (Resolver& r : resolvers) {
        std::lock_guard lock(r.mtx);
        if (r.pid <= 0) startResolver(r, cookies);
    }
}

// One answer line from the resolver, "" on failure (the resolver is then restarted next time).
long long fileMtime(const std::string& path) {
    struct stat st{};
    return (!path.empty() && ::stat(path.c_str(), &st) == 0) ? static_cast<long long>(st.st_mtime) : 0;
}

std::string askResolver(Resolver& r, const std::string& url, const std::string& cookies) {
    const long long mtime = fileMtime(cookies);
    if (r.pid > 0 && (r.cookies != cookies || r.cookies_mtime != mtime)) stopResolver(r);
    if (r.pid <= 0) r.cookies_mtime = mtime;
    if (r.pid <= 0 && !startResolver(r, cookies)) return "";

    const std::string line = url + "\n";
    if (write(r.to, line.data(), line.size()) != static_cast<ssize_t>(line.size())) { stopResolver(r); return ""; }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(25);
    char buf[16384];
    while (true) {
        const size_t nl = r.pending.find('\n');
        if (nl != std::string::npos) {
            std::string answer = r.pending.substr(0, nl);
            r.pending.erase(0, nl + 1);
            return answer;
        }
        if (std::chrono::steady_clock::now() > deadline) { stopResolver(r); return ""; }
        fd_set set; FD_ZERO(&set); FD_SET(r.from, &set);
        timeval tv{0, 200000};
        if (select(r.from + 1, &set, nullptr, nullptr, &tv) > 0) {
            const ssize_t n = read(r.from, buf, sizeof(buf));
            if (n <= 0) { stopResolver(r); return ""; }
            r.pending.append(buf, static_cast<size_t>(n));
        }
    }
}

} // namespace

std::string services::Player::resolveStream(const std::string& url, bool priority) {
    static const std::regex video(R"(watch\?v=[A-Za-z0-9_-]{11}(&|$))");
    if (!std::regex_search(url, video)) return "";

    const Config& cfg = Config::get();

    // argv, no shell: nothing in the URL or paths is ever interpreted.
    // No --force-ipv4: the URL is tied to the address that asked for it, and
    // mpv connects the same way the system does (IPv6 when there is one).
    std::vector<std::string> args = { "yt-dlp", "--no-warnings", "-J", "-f", "bestaudio/best" };
    const std::string cookies = resolverCookies();
    const bool with_cookies = !cookies.empty();
    if (with_cookies) { args.push_back("--cookies"); args.push_back(cookies); }
    args.push_back(url);
    const auto t0 = std::chrono::steady_clock::now();
    auto took = [&] { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count(); };

    /* Resolver 0 is kept for the track the user is waiting for; preloads and
    selection warm-ups take turns on resolver 1, so they never delay it.
    Spawning yt-dlp is the fallback when a resolver can't answer at all.
    */
    std::string out;
    {
        Resolver& r = resolvers[priority ? 0 : 1];
        std::lock_guard lock(r.mtx);
        out = askResolver(r, url, cookies);
    }
    if (out == "{}") return "";      // answered: unavailable (a cold spawn would fail too)
    if (out.size() > 2) {
        spdlog::info("PLAYER: stream resolved in {} ms", took());
        const std::string r = parseResolved(out);
        rememberStream(url, r);
        return r;
    }
    out.clear();

    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);

    int fds[2];
    if (pipe(fds) != 0) return "";

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addclose(&actions, fds[0]);

    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT);
#endif
    pid_t pid = 0;
    const bool started = posix_spawnp(&pid, "yt-dlp", &actions, &attr, argv.data(), environ) == 0;
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&actions);
    close(fds[1]);

    if (started) {
        // A stuck yt-dlp is killed after 25 s.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(25);
        char buf[16384];
        while (true) {
            fd_set set; FD_ZERO(&set); FD_SET(fds[0], &set);
            timeval tv{0, 200000};
            if (select(fds[0] + 1, &set, nullptr, nullptr, &tv) > 0) {
                const ssize_t n = read(fds[0], buf, sizeof(buf));
                if (n <= 0) break;
                out.append(buf, static_cast<size_t>(n));
            }
            if (std::chrono::steady_clock::now() > deadline) { kill(pid, SIGKILL); break; }
        }
        int status = 0;
        waitpid(pid, &status, 0);
    }
    close(fds[0]);
    const std::string r = parseResolved(out);
    rememberStream(url, r);
    return r;
}

std::string services::Player::parseResolved(const std::string& out) {
    nlohmann::json j = nlohmann::json::parse(out, nullptr, false);
    if (!j.is_object()) return "";
    const std::string stream = utils::jstr(j, "url");
    if (stream.rfind("http", 0) != 0) return "";

    // The agent the URL was issued to goes with it (see MPV::load), and the
    // gain that levels the track: YouTube's loudness is in dB above its
    // reference; loud masters come down, quiet ones up a little.
    std::string ua;
    if (j.contains("http_headers") && j["http_headers"].is_object()) ua = utils::jstr(j["http_headers"], "User-Agent");
    if (ua.find('\n') != std::string::npos) ua.clear();
    std::string gain;
    if (Config::get().NORMALIZE_VOLUME && j.contains("loudness") && j["loudness"].is_number()) {
        const double g = std::clamp(4.0 - j["loudness"].get<double>(), -10.0, 3.0);
        // Not printf: a decimal-comma locale would write "-3,50".
        const long centi = std::lround(g * 100.0);
        const long a = std::labs(centi);
        gain = (centi < 0 ? "-" : "") + std::to_string(a / 100) + "." + (a % 100 < 10 ? "0" : "") + std::to_string(a % 100);
    }
    return stream + "\n" + ua + "\n" + gain;
}

void services::Player::warmNow(const std::string& url) {
    if (url.find("youtube.com/watch") == std::string::npos) return;
    {
        std::lock_guard lock(preloaded_mutex);
        // One at a time: the two resolvers stay free for the track that starts.
        if (preloaded.count(url) || preloading.count(url) || !preloading.empty()) return;
        auto r = recent_streams.find(url);
        if (r != recent_streams.end() && streamFresh(r->second, 600)) return;
        preloading.insert(url);
    }
    std::thread([this, url] {
        const std::string stream = resolveStream(url);
        std::lock_guard lock(preloaded_mutex);
        preloading.erase(url);
        if (stream.empty()) return;
        if (preloaded.size() > 32) preloaded.clear();
        preloaded[url] = stream;
    }).detach();
}

void services::Player::warmStream(const music::SongRef& song) {
    if (song.id.empty()) return;
    music::Song s;
    s.setRef(song);
    const std::string url = s.getStreamUrl();

    {
        std::lock_guard lock(preloaded_mutex);
        if (preloaded.count(url) || preloading.count(url)) return;
        auto r = recent_streams.find(url);
        if (r != recent_streams.end() && streamFresh(r->second, 600)) return;   // resolved earlier
        warm_latest = url;
    }

    /* Debounced: scrolling through a list asks for many songs; only the one
    the selection rests on (350 ms) is resolved, at most 2 yt-dlp at a time.
    */
    std::thread([this, url] {
        std::this_thread::sleep_for(std::chrono::milliseconds(350));
        {
            std::lock_guard lock(preloaded_mutex);
            if (warm_latest != url || preloaded.count(url) || preloading.count(url) || preloading.size() >= 2) return;
            preloading.insert(url);
        }
        const std::string stream = resolveStream(url);
        std::lock_guard lock(preloaded_mutex);
        preloading.erase(url);
        if (stream.empty()) return;
        if (preloaded.size() > 32) preloaded.clear();
        preloaded[url] = stream;
    }).detach();
}

void services::Player::openAlbum(const music::AlbumRef& ref) {
    {
        std::lock_guard lock(state_mutex);
        music::PlaylistRef as_list;
        as_list.id = ref.id;
        as_list.title = ref.title;
        as_list.author = ref.artists.empty() ? "" : ref.artists[0].name;
        as_list.thumbnail_large = ref.thumbnail_large;
        as_list.thumbnail_small = ref.thumbnail_small;

        auto placeholder = std::make_shared<music::Playlist>();
        placeholder->setRef(as_list);
        state.open_playlist = placeholder;
        state.flags["open_playlist_more"] = Flags::Done;
        state.open_kind = "album";
        state.flags["open_playlist"] = Flags::Ongoing;
        state.errors.erase("open_playlist");
    }

    {
        std::lock_guard lock(command_mutex);
        command_queue.push(OpenAlbumCommand{ref});
    }
    command_cv.notify_one();
}

void services::Player::openArtist(const music::ArtistRef& ref) {
    {
        std::lock_guard lock(state_mutex);
        auto placeholder = std::make_shared<services::Music::ArtistPage>();
        placeholder->ref = ref;
        state.open_artist = placeholder;
        state.flags["open_artist"] = Flags::Ongoing;
        state.errors.erase("open_artist");
    }

    {
        std::lock_guard lock(command_mutex);
        command_queue.push(OpenArtistCommand{ref});
    }
    command_cv.notify_one();
}

void services::Player::playOpenPlaylistFrom(size_t index) {
    std::shared_ptr<music::Playlist> from;

    {
        std::lock_guard lock(state_mutex);
        if (!state.open_playlist) return;

        auto tracks = state.open_playlist->getStreamables();
        if (index >= tracks.size()) return;

        from = std::make_shared<music::Playlist>();
        from->setRef(state.open_playlist->ref);
        for (size_t i = index; i < tracks.size(); i++) from->addStreamable(tracks[i]);
    }

    queue(std::static_pointer_cast<music::IStreamableContainer>(from), true, true, index);
}

void services::Player::toggleLikeCurrent() {
    std::string id;
    bool like;

    {
        std::lock_guard lock(state_mutex);

        auto song = std::dynamic_pointer_cast<music::Song>(state.current);
        if (!song || song->ref.id.empty()) return;

        if (!state.is_logged_in) {
            state.notice = "Sign in to like tracks: moroder setup";
            state.notice_until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            return;
        }

        id = song->ref.id;
        like = !state.liked[id];
        state.liked[id] = like;     // optimistic, rolled back if the request fails
    }

    {
        std::lock_guard lock(command_mutex);
        command_queue.push(RateCommand{id, like});
    }

    command_cv.notify_one();
}

void services::Player::togglePause() {
    bool is_playing;

    {
        std::lock_guard lock(state_mutex);
        is_playing = state.flags["audio"] == Flags::Ongoing;
    }

    if (is_playing) {
        pause();
    } else {
        resume();
    }
}

bool services::Player::isInitialized() {
    return this->initialised;
}

void services::Player::getLibraryAlbums() {
    {
        std::lock_guard lock(command_mutex);
        command_queue.push(LibraryAlbumsCommand{});
    }

    command_cv.notify_one();
}

void services::Player::getLibrarySongs() {
    {
        std::lock_guard lock(command_mutex);
        command_queue.push(LibrarySongsCommand{});
    }

    command_cv.notify_one();
}

void services::Player::getLibraryArtists() {
    {
        std::lock_guard lock(command_mutex);
        command_queue.push(LibraryArtistsCommand{});
    }

    command_cv.notify_one();
}

void services::Player::getLibraryPodcasts() {
    {
        std::lock_guard lock(command_mutex);
        command_queue.push(LibraryPodcastsCommand{});
    }

    command_cv.notify_one();
}

void services::Player::retryLibrary() {
    {
        std::lock_guard lock(state_mutex);

        for (const char* source : LIBRARY_SOURCES) {
            state.errors.erase(source);
            state.flags[source] = Flags::Ongoing;
        }

        state.errors.erase("library");
    }

    this->getLibraryPlaylists();
    this->getLibraryAlbums();
    this->getLibraryArtists();
    this->getLibraryPodcasts();
    this->getLibrarySongs();
}
