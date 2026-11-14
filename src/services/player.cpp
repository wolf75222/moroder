#include "../../include/services/player.hpp"

#include "../../include/config/config.hpp"
#include "../../include/services/notify.hpp"
#include "../../include/utils/utils.hpp"
#include <fstream>

#include <spdlog/spdlog.h>
#include <unordered_map>
#include <nlohmann/json.hpp>
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

        std::thread([this] {
            while (true) {
                std::this_thread::sleep_for(std::chrono::minutes(10));
                if (music_service->refreshCookies()) mpv_service->refreshSessionCookies();
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

    mpris_service->onToggle([&] {
        bool is_playing;

        {
            std::lock_guard lock(state_mutex);

            if(state.flags["audio"] == Flags::Ongoing) {
                state.flags["audio"] = Flags::Done;
                state.flags["paused"] = Flags::True;
                is_playing = false;
            } else {
                state.flags["audio"] = Flags::Ongoing;
                state.flags["paused"] = Flags::False;
                is_playing = true;
            }
        }

        if(is_playing) {
            mpv_service->pause();
            social_service->pause();
        } else {
            mpv_service->resume();
            social_service->resume();
        }

        mpris_service->setPlaybackStatus(is_playing ? services::PlaybackStatus::Playing : services::PlaybackStatus::Paused);
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
        spdlog::info("PLAYER: Stream start");
        uint64_t song_duration = mpv_service->getStreamDuration();
        {
            std::lock_guard lock(state_mutex);
            state.current->setDuration(song_duration);
            state.flags["audio"] = Flags::Ongoing;
            state.flags["paused"] = Flags::False;
        }

        mpris_service->setPlaybackStatus(services::PlaybackStatus::Playing);
        this->updateMprisControls();
        this->updateMprisData();
        this->updateSocialData();
        this->startPositionTick();
        this->preloadNext();

        // Resuming a saved session: back to where it was, paused.
        if (resume_at >= 0) {
            mpv_service->setPosition(static_cast<uint64_t>(resume_at * 1000000.0));
            resume_at = -1;
            this->pause();
        }

        {
            std::shared_ptr<music::Song> song;
            {
                std::lock_guard lock(state_mutex);
                song = std::dynamic_pointer_cast<music::Song>(state.current);
            }
            if (song) recordHistory(song->ref);
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

        if(loop_to >= 0) {
            spdlog::info("PLAYER: loop, restarting the queue at index {}", loop_to);
            mpv_service->skipTo(loop_to);
        }

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
        int current_pos;

        {
            std::lock_guard lock(state_mutex);
            state.user_queue.pop_back();
            current_pos = state.queue_position;
            state.queue_position = state.user_queue.size() - 1;
        }

        if(current_pos < 0) {
            current_pos = 0;
        }

        mpv_service->removeAt(current_pos);
        this->skipForward();

        // We do this because mpv automatically skips on stream error
        mpv_service->skipTo(current_pos);
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
                    should_queue = state.flags["audio"] == Flags::Ongoing;
                    is_queue_empty = state.user_queue.empty() && state.radio_queue.empty();
                }

                std::shared_ptr<music::IStreamable> streamable;

                if (auto song_ptr = std::dynamic_pointer_cast<music::Song>(c.streamable)) {
                    music::Song song;

                    if (c.fetch_album) {
                        spdlog::info("PLAYER: QueueStreamableCommand, fetching album data");
                        song = music_service->getSong(song_ptr->ref);

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

                if(c.start_fresh) {
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
                }

                if(c.start_radio) {
                    {
                        std::lock_guard lock(command_mutex);
                        command_queue.push(QueueStreamableRadioCommand{streamable});
                    }
                    command_cv.notify_one();
                }

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
                }

                if(!c.queue_in_radio) {
                    mpv_service->load(streamable->getStreamUrl());
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
                    if (!playlist_ptr->getStreamables().empty()) {
                        container = playlist_ptr;
                    } else {
                        music::Playlist playlist = music_service->getPlaylist(playlist_ptr->ref);
                        container = std::make_shared<music::Playlist>(playlist);
                    }

                } else {
                    spdlog::warn("PLAYER: QueueStreamableContainerCommand, unknown streamable type");
                    return;
                }

                for(auto streamable : container->getStreamables()) {
                    {
                        std::lock_guard lock(command_mutex);
                        command_queue.push(QueueStreamableCommand{
                            .streamable=streamable,
                            .fetch_album=false,
                            .start_radio=false,
                            .start_fresh=c.start_fresh,
                            .queue_in_radio=false
                        });
                    }

                    if(c.start_fresh) {
                        c.start_fresh = false;
                    }
                }

                if(c.start_radio) {
                    {
                        std::lock_guard lock(command_mutex);
                        command_queue.push(QueueStreamableContainerRadioCommand{container});
                    }
                    command_cv.notify_one();
                }
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

            } else if constexpr (std::is_same_v<T, ShuffleCommand>) {
                std::vector<std::string> urls;
                size_t pos = 0, count = 0;

                {
                    std::lock_guard lock(state_mutex);
                    pos = static_cast<size_t>(std::max(0, state.queue_position));
                    count = state.user_queue.size();
                    if (count <= pos + 2) return;

                    static std::mt19937 rng{std::random_device{}()};
                    std::shuffle(state.user_queue.begin() + pos + 1, state.user_queue.end(), rng);
                    for (size_t i = pos + 1; i < count; i++) urls.push_back(state.user_queue[i]->getStreamUrl());

                    state.notice = "Queue shuffled (" + std::to_string(urls.size()) + " tracks)";
                    state.notice_until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
                }

                // Rebuild mpv's tail in the new order; the current track plays on.
                for (size_t i = count - 1; i > pos; i--) mpv_service->removeAt(static_cast<uint16_t>(i));
                for (const auto& url : urls) mpv_service->load(url);
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
                }

                // playlist-move puts the entry before the target index.
                mpv_service->move(static_cast<uint16_t>(c.from), static_cast<uint16_t>(c.to > c.from ? c.to + 1 : c.to));
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
                }

                mpv_service->insertAt(c.stream_url, static_cast<uint16_t>(c.index));
                mpv_service->removeAt(static_cast<uint16_t>(c.index + 1));
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

            } else if constexpr (std::is_same_v<T, OpenPlaylistCommand>) {
                spdlog::info("PLAYER: OpenPlaylistCommand, {}", c.ref.title);

                music::Playlist playlist = music_service->getPlaylist(c.ref);
                const services::RequestError& err = music_service->lastError();

                std::lock_guard lock(state_mutex);

                // A newer open replaced this one while it loaded: drop it.
                if (!state.open_playlist || state.open_playlist->ref.id != c.ref.id) return;

                if (err.failed && !err.cancelled) {
                    state.flags["open_playlist"] = Flags::Error;
                    state.errors["open_playlist"] = { err.message, err.retryable };
                } else {
                    state.open_playlist = std::make_shared<music::Playlist>(playlist);
                    state.flags["open_playlist"] = Flags::Done;
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

void services::Player::queue(std::shared_ptr<music::IStreamableContainer> container, const bool fresh, const bool radio) {
    const Config cfg = Config::get();

    {
        std::lock_guard lock(command_mutex);
        command_queue.push(QueueStreamableContainerCommand{
            .container=container,
            .fetch_albums=false,
            .start_fresh=fresh,
            .start_radio=radio,
            .queue_in_radio=false
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

            spdlog::info("PLAYER: skipping to radio");

        } else if (state.queue_position < state.user_queue.size() && !state.radio_queue.empty()){
            state.queue_position++;
            state.current = state.user_queue[state.queue_position];

        } else if (state.queue_position < 0) {
            state.queue_position = 0;
            state.current = state.user_queue[state.queue_position];
        }
    }

    if(to_radio_skip) {
        spdlog::info("PLAYER: loading next radio song");
        mpv_service->load(streamUrlFor(radio_next->getStreamUrl()));
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
    bool should_skip = false;

    {
        std::lock_guard lock(state_mutex);
        if (index < state.user_queue.size()) {
            state.user_queue.erase(state.user_queue.begin() + index);

        } else {
            spdlog::warn("PLAYER: tried to remove song at {} but index is invalid ({}).", index, state.user_queue.size());
            return;
        }

        if(index < state.queue_position) {
            state.queue_position -= 1;

        } else if(index == state.queue_position) {
            state.queue_position -= 1;
            should_skip = true;
        }
    }

    if(should_skip) {
        this->skipForward();
    }

    mpv_service->removeAt(index);
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
    {
        std::lock_guard lock(position_tick_mutex);
        position_tick_running = false;
    }

    position_tick_cv.notify_all();

    if (position_tick_thread.joinable()) {
        position_tick_thread.join();
    }

    {
        std::lock_guard lock(position_tick_mutex);
        position_tick_running = true;
    }

    position_tick_thread = std::thread([this] {
        uint64_t last_position = 0;

        while (true) {
            std::unique_lock lock(position_tick_mutex);
            position_tick_cv.wait_for(lock, std::chrono::seconds(1), [this] {
                return !position_tick_running;
            });

            if (!position_tick_running) break;
            lock.unlock();

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

            if (on_position_tick) {
                on_position_tick(pos, duration);
            }
        }
    });
}

void services::Player::stopPositionTick() {
    {
        std::lock_guard lock(position_tick_mutex);
        if (!position_tick_running) {
            if (position_tick_thread.joinable())
                position_tick_thread.join();
            return;
        }
        position_tick_running = false;
    }

    position_tick_cv.notify_all();

    if (position_tick_thread.joinable()) {
        if (position_tick_thread.get_id() != std::this_thread::get_id()) {
            position_tick_thread.join();
        } else {
            position_tick_thread.detach();
        }
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

                mpv_service->load(streamUrlFor(state.user_queue.back()->getStreamUrl()));
            }
        }
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

    // Remembered for the next launch.
    std::ofstream(volumeFile(), std::ios::trunc) << volume << "\n";
}

void services::Player::openPlaylist(const music::PlaylistRef& ref) {
    {
        std::lock_guard lock(state_mutex);
        auto placeholder = std::make_shared<music::Playlist>();
        placeholder->setRef(ref);
        state.open_playlist = placeholder;
        state.open_kind = "playlist";
        state.flags["open_playlist"] = Flags::Ongoing;
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
    mergeHistoryShelf();
}

void services::Player::mergeHistoryShelf() {
    if (Config::anonymous) return;

    nlohmann::json list = readJson("history.json");
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
}

bool services::Player::restoreSession() {
    if (Config::anonymous) return false;

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

    resume_at = session.value("position", 0.0);
    mpv_service->pause();       // loads paused: no blip of sound before the seek
    queue(std::static_pointer_cast<music::IStreamableContainer>(list), true, true);
    spdlog::info("PLAYER: resuming {} track(s) at {:.0f}s", list->getStreamables().size(), resume_at);
    return true;
}

std::string services::Player::streamUrlFor(const std::string& url) {
    std::lock_guard lock(preloaded_mutex);
    auto it = preloaded.find(url);
    if (it == preloaded.end()) return url;
    std::string stream = it->second;
    preloaded.erase(it);
    return stream;
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
void services::Player::preloadNext() {
    size_t index;
    std::string url;

    {
        std::lock_guard lock(state_mutex);
        if (state.queue_position < 0) return;
        index = static_cast<size_t>(state.queue_position) + 1;
        if (index < state.user_queue.size()) {
            url = state.user_queue[index]->getStreamUrl();
        } else if (state.autoplay && !state.radio_queue.empty()) {
            url = state.radio_queue.front()->getStreamUrl();     // the radio's next
            index = SIZE_MAX;
        } else {
            return;
        }
    }

    if (url.rfind("http", 0) != 0 || url.find("youtube.com") == std::string::npos) return;

    std::thread([this, index, url] {
        const Config& cfg = Config::get();
        std::string cmd = "yt-dlp --no-warnings -f 'bestaudio/best' -g ";
        const std::filesystem::path cookies = cfg.YTM_COOKIES_PATH / "cookies.txt";
        std::error_code ec;
        if (!Config::anonymous && std::filesystem::exists(cookies, ec)) cmd += "--cookies '" + cookies.string() + "' ";
        cmd += "'" + url + "' 2>/dev/null";

        std::string out;
        if (FILE* pipe = popen(cmd.c_str(), "r")) {
            char buf[4096];
            while (fgets(buf, sizeof(buf), pipe)) out += buf;
            pclose(pipe);
        }

        const std::string stream = out.substr(0, out.find('\n'));
        if (stream.rfind("http", 0) != 0) return;

        {
            std::lock_guard lock(command_mutex);
            command_queue.push(PreloadedCommand{index, url, stream});
        }
        command_cv.notify_one();
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

    queue(std::static_pointer_cast<music::IStreamableContainer>(from), true, true);
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
